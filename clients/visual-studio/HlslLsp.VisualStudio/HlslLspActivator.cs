using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.ComponentModelHost;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;
using Microsoft.VisualStudio.Threading;
using Microsoft.VisualStudio.Utilities;
using HlslLsp.VisualStudio.Bootstrap;

namespace HlslLsp.VisualStudio;

public sealed class HlslLspActivator :
    IVsSolutionEvents,
    IVsSolutionEvents7,
    IVsSelectionEvents,
    IVsRunningDocTableEvents
{
    private readonly HlslBootstrapPackage host;
    private readonly JoinableTaskFactory joinableTaskFactory;
    private readonly CancellationToken disposalToken;
    private readonly HashSet<string> registeredExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, IContentType> replacedExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private IContentTypeRegistryService contentTypes;
    private IFileExtensionRegistryService fileExtensions;
    private ITextDocumentFactoryService textDocuments;
    private IComponentModel componentModel;
    private IVsRunningDocumentTable runningDocuments;
    private IVsMonitorSelection selectionMonitor;
    private IVsEditorAdaptersFactoryService editorAdapters;
    private IContentType nativeShaderContentType;
    private IContentType nativeHeaderContentType;
    private IContentType remoteShaderContentType;
    private IContentType remoteHeaderContentType;
    private HlslLanguageClient languageClient;
    private HlslNavigationBarManager navigationBars;
    private CancellationTokenSource navigationAttachCancellation;
    private CancellationTokenSource unsavedHlslBufferDebounceCancellation;
    private readonly ConditionalWeakTable<ITextBuffer, object> unsavedHlslBufferHookedBuffers =
        new();
    private static readonly object UnsavedHlslBufferHookedMarker = new();
    private HashSet<string> configuredExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private bool servicesReady;
    private string workspaceRuntimeDirectory = string.Empty;
    private string lastRuntimeDirectory = string.Empty;
    private string workspaceActiveVariant = string.Empty;

    private HlslLspActivator(
        HlslBootstrapPackage host,
        CancellationToken disposalToken)
    {
        this.host = host;
        joinableTaskFactory = host.JoinableTaskFactory;
        this.disposalToken = disposalToken;
    }

    public static async Task ActivateAsync(
        HlslBootstrapPackage host,
        CancellationToken cancellationToken)
    {
        var activator = new HlslLspActivator(host, cancellationToken);
        HlslBootstrapPackage.OptionsChanged += activator.OnOptionsChanged;
        await activator.InitializeAsync(cancellationToken);
    }

    private async Task InitializeAsync(CancellationToken cancellationToken)
    {
        await joinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var solution = await host.GetServiceAsync(typeof(SVsSolution)) as IVsSolution;
        if (solution == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's solution service is unavailable.");
        }
        ErrorHandler.ThrowOnFailure(solution.AdviseSolutionEvents(this, out _));
        var resolvedSelectionMonitor =
            await host.GetServiceAsync(typeof(SVsShellMonitorSelection))
                as IVsMonitorSelection;
        if (resolvedSelectionMonitor == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's selection monitor is unavailable.");
        }
        selectionMonitor = resolvedSelectionMonitor;
        ErrorHandler.ThrowOnFailure(
            selectionMonitor.AdviseSelectionEvents(this, out _));

        await ActivateLanguageClientAsync(cancellationToken);
    }

    private void OnOptionsChanged()
    {
        joinableTaskFactory.RunAsync(ApplyOptionsAsync)
            .FileAndForget("HlslLsp/ApplyOptions");
    }

    private async Task ActivateLanguageClientAsync(CancellationToken cancellationToken)
    {
        await joinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);

        var resolvedComponentModel = await host.GetServiceAsync(typeof(SComponentModel))
            as IComponentModel;
        if (resolvedComponentModel == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's component model is unavailable.");
        }
        componentModel = resolvedComponentModel;

        contentTypes = componentModel.GetService<IContentTypeRegistryService>();
        fileExtensions = componentModel.GetService<IFileExtensionRegistryService>();
        textDocuments = componentModel.GetService<ITextDocumentFactoryService>();
        editorAdapters = componentModel.GetService<IVsEditorAdaptersFactoryService>();
        runningDocuments =
            await host.GetServiceAsync(typeof(SVsRunningDocumentTable))
                as IVsRunningDocumentTable;
        if (runningDocuments == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's running document table is unavailable.");
        }
        ErrorHandler.ThrowOnFailure(
            runningDocuments.AdviseRunningDocTableEvents(this, out _));
        ApplyFileExtensions(GetOptions().FileExtensions);
        nativeShaderContentType = contentTypes.GetContentType("HLSL")
            ?? throw new InvalidOperationException(
                "Visual Studio's HLSL content type is unavailable.");
        nativeHeaderContentType = contentTypes.GetContentType("HLSLHeader")
            ?? throw new InvalidOperationException(
                "Visual Studio's HLSL header content type is unavailable.");
        remoteShaderContentType = GetOrCreateRemoteContentType(
            "HLSL-LSP-Colored",
            "HLSL");
        remoteHeaderContentType = GetOrCreateRemoteContentType(
            "HLSLHeader-LSP-Colored",
            "HLSLHeader");
        textDocuments.TextDocumentCreated += OnTextDocumentCreated;
        servicesReady = true;
        await ApplyOpenDocumentMappingsAsync(cancellationToken);

        var broker = componentModel.GetService<ILanguageClientBroker>();
        var initialOptions = GetOptions();
        lastRuntimeDirectory = EffectiveRuntimeDirectory(initialOptions);
        languageClient = new HlslLanguageClient(
            initialOptions.LanguageVersion,
            lastRuntimeDirectory,
            workspaceActiveVariant,
            initialOptions.InlayHints,
            OnServerRuntimeRestartRequestedAsync,
            OnActiveVariantChangedFromServerAsync,
            OnConfigurationChangedFromServerAsync);
        MemoryLayoutBridge.Register(languageClient.GetMemoryLayoutAsync);
        CompilationInfoBridge.Register(languageClient.GetCompilationInfoAsync);
        PreprocessorExplorerBridge.Register(languageClient.GetPreprocessorExplorerAsync);
        EntryPointDataFlowBridge.Register(languageClient.GetEntryPointDataFlowAsync);
        ComputeVisualizationBridge.Register(languageClient.GetComputeVisualizationAsync);
        CallHierarchyBridge.RegisterPrepare(languageClient.PrepareCallHierarchyAsync);
        CallHierarchyBridge.RegisterIncomingCalls(languageClient.GetIncomingCallsAsync);
        CallHierarchyBridge.RegisterOutgoingCalls(languageClient.GetOutgoingCallsAsync);
        VariantBridge.Register(
            languageClient.GetVariantsAsync,
            OnActiveVariantSelectedAsync);
        await broker.LoadAsync(new HlslLanguageClientMetadata(), languageClient);

        navigationBars = new HlslNavigationBarManager(
            editorAdapters,
            textDocuments,
            languageClient,
            joinableTaskFactory,
            host);
        ScheduleNavigationBarAttachment();
    }

    private async Task ApplyOpenDocumentMappingsAsync(CancellationToken cancellationToken)
    {
        await joinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        ForEachOpenBuffer(ApplyConfiguredContentType);
    }

    private void ForEachOpenBuffer(Action<ITextBuffer> action)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (runningDocuments == null || editorAdapters == null)
        {
            return;
        }

        ErrorHandler.ThrowOnFailure(
            runningDocuments.GetRunningDocumentsEnum(out var documents));
        var cookies = new uint[1];
        while (documents.Next(1, cookies, out var fetched) == VSConstants.S_OK &&
               fetched == 1)
        {
            ErrorHandler.ThrowOnFailure(
                runningDocuments.GetDocumentInfo(
                    cookies[0],
                    out _,
                    out _,
                    out _,
                    out _,
                    out _,
                    out _,
                    out var documentData));
            if (documentData == IntPtr.Zero)
            {
                continue;
            }

            try
            {
                if (Marshal.GetObjectForIUnknown(documentData) is IVsTextBuffer adapter)
                {
                    var buffer = editorAdapters.GetDocumentBuffer(adapter);
                    if (buffer != null)
                    {
                        action(buffer);
                    }
                }
            }
            finally
            {
                Marshal.Release(documentData);
            }
        }
    }

    private void OnTextDocumentCreated(object sender, TextDocumentEventArgs eventArgs)
    {
        joinableTaskFactory.RunAsync(
                async () =>
                {
                    await joinableTaskFactory.SwitchToMainThreadAsync();
                    ApplyConfiguredContentType(eventArgs.TextDocument.TextBuffer);
                    ScheduleNavigationBarAttachment();
                })
            .FileAndForget("HlslLsp/ApplyDocumentMapping");
    }

    private void ScheduleNavigationBarAttachment()
    {
        var replacement =
            CancellationTokenSource.CreateLinkedTokenSource(disposalToken);
        var previous = Interlocked.Exchange(
            ref navigationAttachCancellation,
            replacement);
        previous?.Cancel();
        previous?.Dispose();
        joinableTaskFactory.RunAsync(
                () => AttachActiveNavigationBarAsync(replacement.Token))
            .FileAndForget("HlslLsp/AttachNavigationBar");
    }

    private async Task AttachActiveNavigationBarAsync(
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < 20; ++attempt)
        {
            await joinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            ErrorHandler.ThrowOnFailure(
                selectionMonitor.GetCurrentElementValue(
                    (uint)VSConstants.VSSELELEMID.SEID_DocumentFrame,
                    out var frame));
            navigationBars?.AttachToDocumentFrame(frame);
            await Task.Delay(250, cancellationToken);
        }
    }

    private async Task ApplyOptionsAsync()
    {
        await joinableTaskFactory.SwitchToMainThreadAsync();
        if (!servicesReady)
        {
            return;
        }

        var options = GetOptions();
        ApplyFileExtensions(options.FileExtensions);
        await ApplyOpenDocumentMappingsAsync(disposalToken);
        if (languageClient == null)
        {
            return;
        }

        var effectiveRuntime = EffectiveRuntimeDirectory(options);
        if (!string.Equals(
                effectiveRuntime,
                lastRuntimeDirectory,
                StringComparison.OrdinalIgnoreCase))
        {
            // An explicit editor option supersedes any workspace-driven runtime.
            if (!string.IsNullOrWhiteSpace(options.DxcRuntimeDirectory))
            {
                workspaceRuntimeDirectory = string.Empty;
                effectiveRuntime = EffectiveRuntimeDirectory(options);
            }
            lastRuntimeDirectory = effectiveRuntime;
            await languageClient.RestartWithRuntimeAsync(
                options.LanguageVersion,
                effectiveRuntime);
        }
        else
        {
            await languageClient.UpdateLanguageVersionAsync(options.LanguageVersion);
        }
        await languageClient.UpdateInlayHintsAsync(options.InlayHints);
    }

    private string EffectiveRuntimeDirectory(HlslOptionsSnapshot options)
    {
        var option = options.DxcRuntimeDirectory?.Trim() ?? string.Empty;
        return option.Length > 0 ? option : workspaceRuntimeDirectory;
    }

    // The server requests a controlled restart when shadertoolsconfig.json selects
    // a different DXC runtime. An explicit editor option wins, and an already
    // applied selection is ignored, so this cannot form a restart loop.
    private Task OnServerRuntimeRestartRequestedAsync(string directory, string reason)
    {
        joinableTaskFactory.RunAsync(async () =>
            {
                await joinableTaskFactory.SwitchToMainThreadAsync();
                if (languageClient == null)
                {
                    return;
                }
                var options = GetOptions();
                if (!string.IsNullOrWhiteSpace(options.DxcRuntimeDirectory))
                {
                    return;
                }
                var requested = string.IsNullOrWhiteSpace(directory)
                    ? string.Empty
                    : directory.Trim();
                if (string.Equals(
                        workspaceRuntimeDirectory,
                        requested,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }
                workspaceRuntimeDirectory = requested;
                lastRuntimeDirectory = requested;
                await languageClient.RestartWithRuntimeAsync(
                    options.LanguageVersion,
                    requested);
                RefreshVariantDependentWindows(CancellationToken.None);
                navigationBars?.Refresh();
            })
            .FileAndForget("HlslLsp/RuntimeRestart");
        return Task.CompletedTask;
    }

    // Persists the workspace's active variant so it survives a controlled runtime
    // restart, then notifies the running server. A variant change reanalyzes open
    // documents rather than restarting.
    private async Task OnActiveVariantSelectedAsync(
        string variant,
        CancellationToken cancellationToken)
    {
        var client = languageClient;
        if (client == null)
        {
            return;
        }
        var value = variant ?? string.Empty;
        workspaceActiveVariant = value;
        // The refresh below must observe the server's new active variant, so
        // it is only scheduled after the notification is awaited. Awaiting
        // (rather than returning the task, as before) preserves the same
        // error-propagation behavior for a failed notification while adding
        // that ordering guarantee.
        await client.UpdateActiveVariantAsync(value);
        RefreshVariantDependentWindows(cancellationToken);
    }

    // Applies a variant the server itself already selected and applied (via
    // the hlsl-lsp.selectVariant command, e.g. from a code action's include
    // recovery), reported back through the hlsl/activeVariantChanged
    // notification. This keeps the client's own durable/cached active variant
    // and any variant-dependent windows equivalent to what the manual "Select
    // HLSL Shader Variant" picker above produces, without re-notifying the
    // server of a change it already made (which would be a redundant,
    // feedback-loop-prone round trip: the server is the source of truth
    // here, not the client).
    private Task OnActiveVariantChangedFromServerAsync(string variant)
    {
        workspaceActiveVariant = variant ?? string.Empty;
        RefreshVariantDependentWindows(CancellationToken.None);
        return Task.CompletedTask;
    }

    private Task OnConfigurationChangedFromServerAsync()
    {
        ActivityLog.LogInformation(
            nameof(HlslLspActivator),
            "Refreshing HLSL views after server configuration processing.");
        RefreshVariantDependentWindows(CancellationToken.None);
        joinableTaskFactory.RunAsync(async () =>
            {
                await joinableTaskFactory.SwitchToMainThreadAsync();
                navigationBars?.Refresh();
            })
            .FileAndForget("HlslLsp/RefreshNavigationBarAfterConfigurationChange");
        return Task.CompletedTask;
    }

    private void RefreshVariantDependentWindows(CancellationToken cancellationToken)
    {
        // A previously opened Shader Compilation window can only become stale
        // through this variant change (the server itself is not restarted),
        // so refresh it here rather than waiting for the next manual
        // invocation of the context command.
        joinableTaskFactory.RunAsync(
                () => host.RefreshCompilationInfoIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshCompilationInfo");
        // The Resource Bindings window reuses the same request and is
        // refreshed independently, mirroring Shader Compilation above.
        joinableTaskFactory.RunAsync(
                () => host.RefreshResourceBindingsIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshResourceBindings");
        // The Preprocessor Explorer window issues its own request and is
        // refreshed independently, mirroring the other two windows above.
        joinableTaskFactory.RunAsync(
                () => host.RefreshPreprocessorExplorerIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshPreprocessorExplorer");
        // The Entry-Point Data Flow window issues its own request and is
        // refreshed independently, mirroring the other windows above: a
        // variant change can change the configured entry point, so a
        // previously opened window can only become stale through this path.
        joinableTaskFactory.RunAsync(
                () => host.RefreshEntryPointDataFlowIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshEntryPointDataFlow");
        joinableTaskFactory.RunAsync(
                () => host.RefreshComputeVisualizationIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshComputeVisualization");
        // The Call Hierarchy window issues its own request (re-fetching
        // incoming/outgoing calls for its current item, not a fresh
        // prepareCallHierarchy) and is refreshed independently, mirroring
        // the other windows above: a variant change can change which
        // declaration a previously resolved item's opaque data still
        // identifies, so a previously opened window can only become stale
        // through this path.
        joinableTaskFactory.RunAsync(
                () => host.RefreshCallHierarchyIfOpenAsync(null, cancellationToken))
            .FileAndForget("HlslLsp/RefreshCallHierarchy");
    }

    // A saved HLSL document may change what the server would compile, so a
    // currently open Shader Compilation window is refreshed if it is showing
    // that same document. Unrelated saves are filtered out inside
    // RefreshCompilationInfoIfOpenAsync to avoid unnecessary requests.
    public int OnAfterSave(uint docCookie)
    {
        if (runningDocuments == null)
        {
            return VSConstants.S_OK;
        }
        if (ErrorHandler.Failed(
                runningDocuments.GetDocumentInfo(
                    docCookie,
                    out _,
                    out _,
                    out _,
                    out var moniker,
                    out _,
                    out _,
                    out var documentData)))
        {
            return VSConstants.S_OK;
        }
        try
        {
            if (string.IsNullOrEmpty(moniker))
            {
                return VSConstants.S_OK;
            }
            if (string.Equals(
                    Path.GetFileName(moniker),
                    "shadertoolsconfig.json",
                    StringComparison.OrdinalIgnoreCase))
            {
                ActivityLog.LogInformation(
                    nameof(HlslLspActivator),
                    "Notifying the HLSL language server about a saved configuration file.");
                var client = languageClient;
                if (client != null &&
                    Uri.TryCreate(moniker, UriKind.Absolute, out var configurationUri) &&
                    configurationUri.IsFile)
                {
                    joinableTaskFactory.RunAsync(
                            () => client.NotifyConfigurationFileChangedAsync(
                                configurationUri))
                        .FileAndForget("HlslLsp/NotifyConfigurationFileSave");
                }
                return VSConstants.S_OK;
            }
            joinableTaskFactory.RunAsync(
                    () => host.RefreshCompilationInfoIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshCompilationInfoOnSave");
            // The Resource Bindings window is refreshed independently on the
            // same save, matching the same non-file/unrelated-document
            // filtering performed inside RefreshResourceBindingsIfOpenAsync.
            joinableTaskFactory.RunAsync(
                    () => host.RefreshResourceBindingsIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshResourceBindingsOnSave");
            // The Preprocessor Explorer window is refreshed independently on
            // the same save, matching the same non-file/unrelated-document
            // filtering performed inside RefreshPreprocessorExplorerIfOpenAsync.
            joinableTaskFactory.RunAsync(
                    () => host.RefreshPreprocessorExplorerIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshPreprocessorExplorerOnSave");
            // The Entry-Point Data Flow window is refreshed independently on
            // the same save, matching the same non-file/unrelated-document
            // filtering performed inside RefreshEntryPointDataFlowIfOpenAsync.
            joinableTaskFactory.RunAsync(
                    () => host.RefreshEntryPointDataFlowIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshEntryPointDataFlowOnSave");
            joinableTaskFactory.RunAsync(
                    () => host.RefreshComputeVisualizationIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshComputeVisualizationOnSave");
            // The Call Hierarchy window is refreshed independently on the
            // same save, matching the same non-file/unrelated-document
            // filtering performed inside RefreshCallHierarchyIfOpenAsync
            // (which re-fetches incoming/outgoing calls for its current
            // item only).
            joinableTaskFactory.RunAsync(
                    () => host.RefreshCallHierarchyIfOpenAsync(moniker, disposalToken))
                .FileAndForget("HlslLsp/RefreshCallHierarchyOnSave");
            return VSConstants.S_OK;
        }
        finally
        {
            if (documentData != IntPtr.Zero)
            {
                Marshal.Release(documentData);
            }
        }
    }

    public int OnAfterFirstDocumentLock(
        uint docCookie,
        uint lockType,
        uint readLocksRemaining,
        uint editLocksRemaining) => VSConstants.S_OK;

    public int OnBeforeLastDocumentUnlock(
        uint docCookie,
        uint lockType,
        uint readLocksRemaining,
        uint editLocksRemaining) => VSConstants.S_OK;

    public int OnAfterAttributeChange(uint docCookie, uint grfAttribs) => VSConstants.S_OK;

    public int OnBeforeDocumentWindowShow(
        uint docCookie,
        int firstShow,
        IVsWindowFrame frame) => VSConstants.S_OK;

    public int OnAfterDocumentWindowHide(uint docCookie, IVsWindowFrame frame) =>
        VSConstants.S_OK;

    private HlslOptionsSnapshot GetOptions()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        return host.GetOptions();
    }

    private void ApplyFileExtensions(string value)
    {
        ThreadHelper.ThrowIfNotOnUIThread();

        configuredExtensions =
            new HashSet<string>(ParseExtensions(value), StringComparer.OrdinalIgnoreCase);
        foreach (var extension in registeredExtensions)
        {
            fileExtensions.RemoveFileExtension(extension);
            if (replacedExtensions.TryGetValue(extension, out var previous))
            {
                fileExtensions.AddFileExtension(extension, previous);
            }
        }
        registeredExtensions.Clear();
        replacedExtensions.Clear();

        var hlslContentType = contentTypes.GetContentType("HLSL")
            ?? throw new InvalidOperationException(
                "Visual Studio's HLSL content type is unavailable.");
        foreach (var extension in configuredExtensions)
        {
            if (extension.Equals(".hlsl", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".hlsli", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var existing = fileExtensions.GetContentTypeForExtension(extension);
            if (existing != null &&
                !ReferenceEquals(existing, contentTypes.UnknownContentType) &&
                !existing.IsOfType("text") &&
                !existing.IsOfType("plaintext"))
            {
                ActivityLog.LogWarning(
                    nameof(HlslLspActivator),
                    $"The {extension} extension is already associated with " +
                    $"{existing.DisplayName}; HLSL-LSP did not replace it.");
                continue;
            }

            if (existing != null &&
                !ReferenceEquals(existing, contentTypes.UnknownContentType))
            {
                replacedExtensions.Add(extension, existing);
                fileExtensions.RemoveFileExtension(extension);
            }
            fileExtensions.AddFileExtension(extension, hlslContentType);
            registeredExtensions.Add(extension);
        }
    }

    private static IEnumerable<string> ParseExtensions(string value)
    {
        return HlslBootstrapPackage.ParseExtensions(value);
    }

    private IContentType GetOrCreateRemoteContentType(string name, string nativeBaseType)
    {
        return contentTypes.GetContentType(name) ??
            contentTypes.AddContentType(
                name,
                new[]
                {
                    nativeBaseType,
                    CodeRemoteContentDefinition.CodeRemoteContentTypeName,
                });
    }

    private IContentType GetOrCreateRemoteHeaderContentType()
    {
        if (remoteHeaderContentType != null)
        {
            return remoteHeaderContentType;
        }

        nativeHeaderContentType = contentTypes.GetContentType("HLSLHeader")
            ?? throw new InvalidOperationException(
                "Visual Studio's HLSL header content type is unavailable.");
        remoteHeaderContentType = GetOrCreateRemoteContentType(
            "HLSLHeader-LSP-Colored",
            "HLSLHeader");
        return remoteHeaderContentType;
    }

    private void ApplyConfiguredContentType(ITextBuffer buffer)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (buffer.ContentType.IsOfType("HLSLHeader"))
        {
            if (!buffer.ContentType.IsOfType("HLSLHeader-LSP-Colored"))
            {
                buffer.ChangeContentType(GetOrCreateRemoteHeaderContentType(), this);
            }
            HookUnsavedHlslBufferRefreshTrigger(buffer);
            return;
        }
        if (buffer.ContentType.IsOfType("HLSL"))
        {
            if (!buffer.ContentType.IsOfType("HLSL-LSP-Colored"))
            {
                buffer.ChangeContentType(remoteShaderContentType, this);
            }
            HookUnsavedHlslBufferRefreshTrigger(buffer);
            return;
        }

        if (textDocuments.TryGetTextDocument(buffer, out var document))
        {
            if (configuredExtensions.Contains(
                    System.IO.Path.GetExtension(document.FilePath)))
            {
                buffer.ChangeContentType(remoteShaderContentType, this);
                HookUnsavedHlslBufferRefreshTrigger(buffer);
            }
            // shadertoolsconfig.json is intentionally NOT hooked here: the
            // server only ever reads it from disk (it is not part of
            // textDocument sync the way HLSL/header content is), so an
            // unsaved in-editor edit has no effect on the server's analysis
            // until the file is saved. Debounced refresh on every keystroke
            // here would just be wasted requests against unchanged server
            // state. OnAfterSave's IsHlslOrConfigRelevantPath-filtered
            // refresh already covers this file once its on-disk content
            // actually changes.
        }
    }

    // Debounces unsaved edits to any open HLSL/header document (root or
    // #include'd) into a single conservative refresh of both the
    // Entry-Point Data Flow and Call Hierarchy windows: either window's
    // result can depend on the active root document or any #include'd
    // header even before the edited file is saved, and a
    // save-only refresh (OnAfterSave below) would otherwise leave the
    // window stale while the user is still actively editing.
    // shadertoolsconfig.json is deliberately never hooked here (only on
    // save) since the server only ever reads it from disk. ConditionalWeak
    // Table avoids both double-subscribing the same buffer and leaking a
    // strong reference to buffers that are later closed.
    private void HookUnsavedHlslBufferRefreshTrigger(ITextBuffer buffer)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (unsavedHlslBufferHookedBuffers.TryGetValue(buffer, out _))
        {
            return;
        }
        unsavedHlslBufferHookedBuffers.Add(buffer, UnsavedHlslBufferHookedMarker);
        buffer.ChangedLowPriority += OnUnsavedHlslBufferRelevantBufferChanged;
    }

    private void OnUnsavedHlslBufferRelevantBufferChanged(
        object sender,
        TextContentChangedEventArgs eventArgs)
    {
        if (eventArgs.Changes.Count == 0)
        {
            return;
        }
        ScheduleUnsavedHlslBufferDebouncedRefresh();
    }

    // Mirrors ScheduleNavigationBarAttachment's cancel-and-replace pattern:
    // each further edit cancels the previous pending refresh and starts a
    // new delay, so a burst of keystrokes collapses into a single refresh
    // once editing pauses, rather than one request per keystroke.
    private void ScheduleUnsavedHlslBufferDebouncedRefresh()
    {
        var replacement =
            CancellationTokenSource.CreateLinkedTokenSource(disposalToken);
        var previous = Interlocked.Exchange(
            ref unsavedHlslBufferDebounceCancellation,
            replacement);
        previous?.Cancel();
        previous?.Dispose();
        joinableTaskFactory.RunAsync(
                () => DebounceUnsavedHlslBufferRefreshAsync(replacement.Token))
            .FileAndForget("HlslLsp/DebounceUnsavedHlslBufferRefresh");
    }

    private async Task DebounceUnsavedHlslBufferRefreshAsync(
        CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(TimeSpan.FromMilliseconds(750), cancellationToken);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        if (cancellationToken.IsCancellationRequested)
        {
            return;
        }
        await host.RefreshEntryPointDataFlowIfOpenAsync(null, cancellationToken);
        await host.RefreshComputeVisualizationIfOpenAsync(null, cancellationToken);
        // The Call Hierarchy window is refreshed independently on the same
        // debounced trigger, matching the same non-file/unrelated-document
        // filtering performed inside RefreshCallHierarchyIfOpenAsync (which
        // re-runs prepareCallHierarchy at the original root position rather
        // than trusting the possibly now-stale current item).
        await host.RefreshCallHierarchyIfOpenAsync(null, cancellationToken);
    }

    private void DemoteOpenDocuments()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        ForEachOpenBuffer(
            buffer =>
            {
                if (buffer.ContentType.IsOfType("HLSLHeader-LSP-Colored"))
                {
                    buffer.ChangeContentType(nativeHeaderContentType, this);
                }
                else if (buffer.ContentType.IsOfType("HLSL-LSP-Colored"))
                {
                    buffer.ChangeContentType(nativeShaderContentType, this);
                }
            });
    }

    private sealed class HlslLanguageClientMetadata : ILanguageClientMetadata
    {
        public IEnumerable<string> ContentTypes { get; } =
            new[]
            {
                "HLSL-LSP-Colored",
                "HLSLHeader-LSP-Colored",
            };

        public string ClientName => null;
    }

    public int OnAfterOpenProject(IVsHierarchy hierarchy, int added) =>
        VSConstants.S_OK;

    public int OnQueryCloseProject(
        IVsHierarchy hierarchy,
        int removing,
        ref int cancel) =>
        VSConstants.S_OK;

    public int OnBeforeCloseProject(IVsHierarchy hierarchy, int removed) =>
        VSConstants.S_OK;

    public int OnAfterLoadProject(
        IVsHierarchy stubHierarchy,
        IVsHierarchy realHierarchy) =>
        VSConstants.S_OK;

    public int OnQueryUnloadProject(
        IVsHierarchy realHierarchy,
        ref int cancel) =>
        VSConstants.S_OK;

    public int OnBeforeUnloadProject(
        IVsHierarchy realHierarchy,
        IVsHierarchy stubHierarchy) =>
        VSConstants.S_OK;

    public int OnAfterOpenSolution(object reserved, int newSolution) =>
        VSConstants.S_OK;

    public int OnQueryCloseSolution(object reserved, ref int cancel) =>
        VSConstants.S_OK;

    public int OnBeforeCloseSolution(object reserved)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        navigationBars?.RemoveAll();
        DemoteOpenDocuments();
        return VSConstants.S_OK;
    }

    public int OnAfterCloseSolution(object reserved) => VSConstants.S_OK;

    public void OnAfterOpenFolder(string folderPath)
    {
        ScheduleNavigationBarAttachment();
    }

    public void OnBeforeCloseFolder(string folderPath)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        navigationBars?.RemoveAll();
        DemoteOpenDocuments();
    }

    public void OnQueryCloseFolder(string folderPath, ref int cancel)
    {
    }

    public void OnAfterCloseFolder(string folderPath)
    {
    }

    public int OnCmdUIContextChanged(uint commandUiCookie, int active) =>
        VSConstants.S_OK;

    public int OnElementValueChanged(
        uint elementId,
        object oldValue,
        object newValue)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (elementId == (uint)VSConstants.VSSELELEMID.SEID_DocumentFrame)
        {
            ScheduleNavigationBarAttachment();
        }
        return VSConstants.S_OK;
    }

    public int OnSelectionChanged(
        IVsHierarchy oldHierarchy,
        uint oldItemId,
        IVsMultiItemSelect oldMultiItemSelect,
        ISelectionContainer oldSelectionContainer,
        IVsHierarchy newHierarchy,
        uint newItemId,
        IVsMultiItemSelect newMultiItemSelect,
        ISelectionContainer newSelectionContainer) =>
        VSConstants.S_OK;

#pragma warning disable CS0618
    public void OnAfterLoadAllDeferredProjects()
    {
    }
#pragma warning restore CS0618

}
