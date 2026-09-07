using System;
using System.Collections.Generic;
using System.ComponentModel.Design;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.ComponentModelHost;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("HLSL-LSP", "DXC-powered HLSL IntelliSense", "0.12.0")]
[ProvideSettingsManifest(PackageRelativeManifestFile = "HlslLsp.registration.json")]
[ProvideMenuResource("Menus.ctmenu", 1)]
[ProvideToolWindow(typeof(MemoryLayoutToolWindow))]
[ProvideToolWindow(typeof(CompilationInfoToolWindow))]
[ProvideToolWindow(typeof(ResourceBindingsToolWindow))]
[ProvideToolWindow(typeof(PreprocessorExplorerToolWindow))]
[ProvideToolWindow(typeof(EntryPointDataFlowToolWindow))]
[ProvideToolWindow(typeof(ComputeVisualizationToolWindow))]
[ProvideToolWindow(typeof(CallHierarchyExplorerToolWindow))]
[ProvideOptionPage(
    typeof(HlslOptionsPage),
    "HLSL-LSP",
    "General",
    0,
    0,
    true,
    IsInUnifiedSettings = true,
    UnifiedSettingsCategoryMoniker = "hlslLsp.general",
    ShouldShowUnifiedSettingsPlaceholder = false)]
[Guid(PackageGuidString)]
public sealed class HlslBootstrapPackage : AsyncPackage
{
    private long memoryLayoutRequestGeneration;
    private long compilationInfoRequestGeneration;
    private int explicitCompilationInfoRequests;
    private long resourceBindingsRequestGeneration;
    private int explicitResourceBindingsRequests;
    private long preprocessorExplorerRequestGeneration;
    private int explicitPreprocessorExplorerRequests;
    private long entryPointDataFlowRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate entryPointDataFlowRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation entryPointDataFlowBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long computeVisualizationRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate computeVisualizationRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation computeVisualizationBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long callHierarchyRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate callHierarchyRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation callHierarchyBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    // Resolved lazily, on the UI thread, the first time a call-hierarchy
    // root position needs to be anchored/re-resolved against a live text
    // buffer (see EnsureCallHierarchyEditorServicesAsync). Never touched by
    // any LSP-dependent code path -- these are plain VS editor services,
    // kept isolated from HlslLanguageClient/StreamJsonRpc exactly like the
    // rest of this bootstrap package.
    private IComponentModel callHierarchyComponentModel;
    private IVsEditorAdaptersFactoryService callHierarchyEditorAdapters;
    private IVsRunningDocumentTable callHierarchyRunningDocuments;
    public const string PackageGuidString = "5ac7fbe7-1b9f-45eb-bca6-ffb9ae1ab67f";

    private static readonly object Gate = new();
    private static readonly HashSet<string> PendingDocuments =
        new(StringComparer.OrdinalIgnoreCase);
    private static HlslBootstrapPackage instance;
    private bool activationStarted;

    public static event Action OptionsChanged;

    internal static void RequestActivation(string filePath)
    {
        HlslBootstrapPackage package;
        lock (Gate)
        {
            PendingDocuments.Add(filePath);
            package = instance;
        }

        package?.JoinableTaskFactory.RunAsync(
                () => package.TryActivateLanguageClientAsync(package.DisposalToken))
            .FileAndForget("HlslLsp/TryActivate");
    }

    public HlslOptionsSnapshot GetOptions()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var page = (HlslOptionsPage)GetDialogPage(typeof(HlslOptionsPage));
        return new HlslOptionsSnapshot(
            page.FileExtensions,
            page.LanguageVersion,
            page.DxcRuntimeDirectory,
            new InlayHintOptionsSnapshot(
                page.InlayHintTypes,
                page.InlayHintParameters,
                page.InlayHintMatrixOrientation,
                page.InlayHintRegisters,
                page.InlayHintPackedOffsets,
                page.InlayHintArrayStrides,
                page.InlayHintActiveVariant));
    }

    protected override async Task InitializeAsync(
        CancellationToken cancellationToken,
        IProgress<ServiceProgressData> progress)
    {
        lock (Gate)
        {
            instance = this;
        }
        MemoryLayoutBridge.RegisterPresenter(
            (uri, line, character) =>
                JoinableTaskFactory.RunAsync(
                        () => ShowMemoryLayoutAsync(uri, line, character, DisposalToken))
                    .FileAndForget("HlslLsp/ShowMemoryLayout"));
        ComputeVisualizationBridge.RegisterPresenter(
            (uri, options) =>
                JoinableTaskFactory.RunAsync(
                        () => ShowComputeVisualizationExplicitAsync(
                            uri,
                            options,
                            DisposalToken))
                    .FileAndForget("HlslLsp/ShowComputeVisualization"));
        await RegisterCommandsAsync(cancellationToken);
        await TryActivateLanguageClientAsync(cancellationToken);
    }

    private async Task RegisterCommandsAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var commands = await GetServiceAsync(typeof(IMenuCommandService))
            as OleMenuCommandService;
        if (commands == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's command service is unavailable.");
        }
        var commandSet = new Guid("cedfa85a-cd51-4825-af1f-0e05bd475426");
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowMemoryLayoutAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowMemoryLayout"),
                new CommandID(commandSet, 0x0100)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => SelectVariantAsync(DisposalToken))
                    .FileAndForget("HlslLsp/SelectVariant"),
                new CommandID(commandSet, 0x0101)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowCompilationInfoAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowCompilationInfo"),
                new CommandID(commandSet, 0x0102)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowResourceBindingsAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowResourceBindings"),
                new CommandID(commandSet, 0x0103)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowPreprocessorExplorerAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowPreprocessorExplorer"),
                new CommandID(commandSet, 0x0104)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowEntryPointDataFlowAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowEntryPointDataFlow"),
                new CommandID(commandSet, 0x0105)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowCallHierarchyAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowCallHierarchy"),
                new CommandID(commandSet, 0x0106)));
        commands.AddCommand(
            new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowComputeVisualizationAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowComputeVisualization"),
                new CommandID(commandSet, 0x0107)));
    }

    private async Task ShowMemoryLayoutAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var textManager = await GetServiceAsync(typeof(SVsTextManager)) as IVsTextManager;
        if (textManager == null ||
            ErrorHandler.Failed(textManager.GetActiveView(1, null, out var view)) ||
            view == null ||
            ErrorHandler.Failed(view.GetCaretPos(out var line, out var character)) ||
            ErrorHandler.Failed(view.GetBuffer(out var lines)) ||
            lines is not IVsUserData userData)
        {
            return;
        }
        var monikerKey = VSConstants.VsTextBufferUserDataGuid.VsBufferMoniker_guid;
        if (ErrorHandler.Failed(userData.GetData(ref monikerKey, out var value)) ||
            value is not string moniker)
        {
            return;
        }
        var uri = new Uri(Path.GetFullPath(moniker));

        await ShowMemoryLayoutAsync(uri, line, character, cancellationToken);
    }

    private async Task ShowMemoryLayoutAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var generation = Interlocked.Increment(ref memoryLayoutRequestGeneration);
        var layout = await MemoryLayoutBridge.RequestAsync(
            uri,
            line,
            character,
            cancellationToken);
        if (generation != Interlocked.Read(ref memoryLayoutRequestGeneration))
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var window = await ShowToolWindowAsync(
            typeof(MemoryLayoutToolWindow),
            0,
            true,
            cancellationToken) as MemoryLayoutToolWindow;
        if (generation == Interlocked.Read(ref memoryLayoutRequestGeneration))
        {
            window?.SetLayout(layout);
        }
    }

    private async Task ShowCompilationInfoAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then run Tools > HLSL Shader Compilation.",
                cancellationToken);
            return;
        }
        Interlocked.Increment(ref explicitCompilationInfoRequests);
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                await ShowCompilationInfoAsync(
                    uri,
                    requestCancellation.Token,
                    null,
                    cancellationToken);
            }
        }
        finally
        {
            Interlocked.Decrement(ref explicitCompilationInfoRequests);
        }
    }

    // The generation guard mirrors ShowMemoryLayoutAsync: a stale response
    // (e.g. from a superseded variant change or an earlier command
    // invocation) can never overwrite a newer one. A failed or cancelled
    // request never regresses the window to the "open a document"
    // placeholder or leaves it stuck: it keeps the last successful content
    // when one exists, and otherwise shows an explicit error.
    private async Task ShowCompilationInfoAsync(
        Uri uri,
        CancellationToken cancellationToken,
        CompilationInfoToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref compilationInfoRequestGeneration);
        CompilationInfoModel info = null;
        string failureMessage = null;
        try
        {
            info = await CompilationInfoBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The shader compilation request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve shader compilation information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref compilationInfoRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(CompilationInfoToolWindow),
                0,
                true,
                ambientCancellationToken) as CompilationInfoToolWindow;
        }
        if (generation != Interlocked.Read(ref compilationInfoRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (info == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide shader compilation information.",
                existingWindow != null);
            return;
        }
        window?.SetInfo(uri, info);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshCompilationInfoIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        // A background save/variant refresh must never supersede an explicit
        // Tools command that the user is waiting for.
        if (Volatile.Read(ref explicitCompilationInfoRequests) != 0)
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(CompilationInfoToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not CompilationInfoToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null &&
            (!Uri.TryCreate(savedFilePath, UriKind.Absolute, out var savedUri) ||
             !savedUri.IsFile ||
             !window.DocumentUri.Equals(savedUri)))
        {
            return;
        }
        // Re-check after the asynchronous UI/tool-window lookup. An explicit
        // command may have started while this background refresh was yielding.
        if (Volatile.Read(ref explicitCompilationInfoRequests) != 0)
        {
            return;
        }
        await ShowCompilationInfoAsync(
            window.DocumentUri,
            cancellationToken,
            window,
            cancellationToken);
    }

    private async Task ShowResourceBindingsAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then run Tools > HLSL Resource Bindings.",
                cancellationToken);
            return;
        }
        Interlocked.Increment(ref explicitResourceBindingsRequests);
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                await ShowResourceBindingsAsync(
                    uri,
                    requestCancellation.Token,
                    null,
                    cancellationToken);
            }
        }
        finally
        {
            Interlocked.Decrement(ref explicitResourceBindingsRequests);
        }
    }

    // Mirrors ShowCompilationInfoAsync: the generation guard ensures a stale
    // response (e.g. from a superseded variant change or an earlier command
    // invocation) can never overwrite a newer one. A failed or cancelled
    // request never regresses the window to the "open a document"
    // placeholder or leaves it stuck: it keeps the last successful content
    // when one exists, and otherwise shows an explicit error. Reuses the
    // same CompilationInfoBridge request as the Shader Compilation window;
    // no new RPC surface is introduced for Resource Bindings.
    private async Task ShowResourceBindingsAsync(
        Uri uri,
        CancellationToken cancellationToken,
        ResourceBindingsToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref resourceBindingsRequestGeneration);
        CompilationInfoModel info = null;
        string failureMessage = null;
        try
        {
            info = await CompilationInfoBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The resource bindings request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve resource binding information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref resourceBindingsRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(ResourceBindingsToolWindow),
                0,
                true,
                ambientCancellationToken) as ResourceBindingsToolWindow;
        }
        if (generation != Interlocked.Read(ref resourceBindingsRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (info == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide resource binding information.",
                existingWindow != null);
            return;
        }
        window?.SetInfo(uri, info);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshResourceBindingsIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        // A background save/variant refresh must never supersede an explicit
        // Tools command that the user is waiting for.
        if (Volatile.Read(ref explicitResourceBindingsRequests) != 0)
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(ResourceBindingsToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not ResourceBindingsToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null &&
            (!Uri.TryCreate(savedFilePath, UriKind.Absolute, out var savedUri) ||
             !savedUri.IsFile ||
             !window.DocumentUri.Equals(savedUri)))
        {
            return;
        }
        // Re-check after the asynchronous UI/tool-window lookup. An explicit
        // command may have started while this background refresh was yielding.
        if (Volatile.Read(ref explicitResourceBindingsRequests) != 0)
        {
            return;
        }
        await ShowResourceBindingsAsync(
            window.DocumentUri,
            cancellationToken,
            window,
            cancellationToken);
    }

    private async Task ShowPreprocessorExplorerAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then run Tools > HLSL Preprocessor Explorer.",
                cancellationToken);
            return;
        }
        Interlocked.Increment(ref explicitPreprocessorExplorerRequests);
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                await ShowPreprocessorExplorerAsync(
                    uri,
                    requestCancellation.Token,
                    null,
                    cancellationToken);
            }
        }
        finally
        {
            Interlocked.Decrement(ref explicitPreprocessorExplorerRequests);
        }
    }

    // Mirrors ShowCompilationInfoAsync/ShowResourceBindingsAsync: the
    // generation guard ensures a stale response (e.g. from a superseded
    // variant change or an earlier command invocation) can never overwrite
    // a newer one. A failed or cancelled request never regresses the window
    // to the "open a document" placeholder or leaves it stuck: it keeps the
    // last successful content when one exists, and otherwise shows an
    // explicit error. Issues its own hlsl/preprocessorExplorer request
    // through PreprocessorExplorerBridge -- a distinct protocol request from
    // CompilationInfoBridge, unlike Resource Bindings which reuses it.
    private async Task ShowPreprocessorExplorerAsync(
        Uri uri,
        CancellationToken cancellationToken,
        PreprocessorExplorerToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref preprocessorExplorerRequestGeneration);
        PreprocessorExplorerModel report = null;
        string failureMessage = null;
        try
        {
            report = await PreprocessorExplorerBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The preprocessor explorer request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve preprocessor explorer information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref preprocessorExplorerRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(PreprocessorExplorerToolWindow),
                0,
                true,
                ambientCancellationToken) as PreprocessorExplorerToolWindow;
        }
        if (generation != Interlocked.Read(ref preprocessorExplorerRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (report == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide preprocessor explorer information.",
                existingWindow != null);
            return;
        }
        window?.SetReport(uri, report);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshPreprocessorExplorerIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        // A background save/variant refresh must never supersede an explicit
        // Tools command that the user is waiting for.
        if (Volatile.Read(ref explicitPreprocessorExplorerRequests) != 0)
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(PreprocessorExplorerToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not PreprocessorExplorerToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null &&
            (!Uri.TryCreate(savedFilePath, UriKind.Absolute, out var savedUri) ||
             !savedUri.IsFile ||
             !window.DocumentUri.Equals(savedUri)))
        {
            return;
        }
        // Re-check after the asynchronous UI/tool-window lookup. An explicit
        // command may have started while this background refresh was yielding.
        if (Volatile.Read(ref explicitPreprocessorExplorerRequests) != 0)
        {
            return;
        }
        await ShowPreprocessorExplorerAsync(
            window.DocumentUri,
            cancellationToken,
            window,
            cancellationToken);
    }

    private async Task ShowComputeVisualizationAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then run Tools > HLSL Compute Visualization.",
                cancellationToken);
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var existingWindow = await FindToolWindowAsync(
            typeof(ComputeVisualizationToolWindow),
            0,
            false,
            cancellationToken) as ComputeVisualizationToolWindow;
        var options = existingWindow?.Options ?? new ComputeVisualizationOptions
        {
            DispatchDimensions = new ComputeDimensionsModel { X = 1, Y = 1, Z = 1 },
        };
        await ShowComputeVisualizationExplicitAsync(uri, options, cancellationToken);
    }

    private async Task ShowComputeVisualizationExplicitAsync(
        Uri uri,
        ComputeVisualizationOptions options,
        CancellationToken cancellationToken)
    {
        computeVisualizationRefreshGate.EnterExplicitRequest();
        try
        {
            using var requestCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
            await ShowComputeVisualizationAsync(
                uri,
                options,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (computeVisualizationRefreshGate.ExitExplicitRequest())
            {
                await RefreshComputeVisualizationIfOpenAsync(null, cancellationToken);
            }
        }
    }

    private async Task ShowComputeVisualizationAsync(
        Uri uri,
        ComputeVisualizationOptions options,
        CancellationToken cancellationToken,
        ComputeVisualizationToolWindow existingWindow,
        CancellationToken ambientCancellationToken)
    {
        var generation = Interlocked.Increment(ref computeVisualizationRequestGeneration);
        var priorWindow = existingWindow;
        if (priorWindow == null)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
            priorWindow = await FindToolWindowAsync(
                typeof(ComputeVisualizationToolWindow),
                0,
                false,
                ambientCancellationToken) as ComputeVisualizationToolWindow;
        }
        var preserveContent =
            ComputeVisualizationRefreshLogic.ShouldPreserveContentOnFailure(
                priorWindow?.DocumentUri,
                uri);
        ComputeVisualizationModel report = null;
        string failureMessage = null;
        try
        {
            report = await ComputeVisualizationBridge.RequestAsync(
                uri,
                options,
                cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The compute visualization request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage = "Could not retrieve compute visualization: " + error.Message;
        }
        if (generation != Interlocked.Read(ref computeVisualizationRequestGeneration))
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (ComputeVisualizationRefreshLogic.ShouldRevealToolWindow(existingWindow != null))
        {
            window = await ShowToolWindowAsync(
                typeof(ComputeVisualizationToolWindow),
                0,
                true,
                ambientCancellationToken) as ComputeVisualizationToolWindow;
        }
        if (generation != Interlocked.Read(ref computeVisualizationRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, options, failureMessage, preserveContent);
        }
        else if (report == null)
        {
            window?.SetError(
                uri,
                options,
                "The HLSL language server is not ready to provide compute visualization.",
                preserveContent);
        }
        else
        {
            window?.SetReport(uri, options, report);
        }
    }

    public async Task RefreshComputeVisualizationIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        if (!computeVisualizationRefreshGate.TryBeginBackgroundRefresh())
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(ComputeVisualizationToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not ComputeVisualizationToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null &&
            !ComputeVisualizationRefreshLogic.IsHlslOrConfigRelevantPath(
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (!computeVisualizationRefreshGate.TryBeginBackgroundRefresh())
        {
            return;
        }
        var options = ComputeVisualizationRefreshLogic.OptionsForBackgroundRefresh(
            window.Options);
        var refreshCancellation =
            computeVisualizationBackgroundRefreshCancellation.BeginNext(cancellationToken);
        await ShowComputeVisualizationAsync(
            window.DocumentUri,
            options,
            refreshCancellation.Token,
            window,
            cancellationToken);
    }

    private async Task ShowEntryPointDataFlowAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then run Tools > HLSL Entry-Point Data Flow.",
                cancellationToken);
            return;
        }
        entryPointDataFlowRefreshGate.EnterExplicitRequest();
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                await ShowEntryPointDataFlowAsync(
                    uri,
                    requestCancellation.Token,
                    null,
                    cancellationToken);
            }
        }
        finally
        {
            if (entryPointDataFlowRefreshGate.ExitExplicitRequest())
            {
                // A save/variant/edit refresh arrived while the explicit
                // request above was in flight and was deferred rather than
                // silently dropped (see RefreshEntryPointDataFlowIfOpenAsync
                // below). Replay it once, now that the explicit request this
                // deferral protected has finished, so the window can never
                // be left stale just because a refresh trigger happened to
                // overlap with an explicit command.
                await RefreshEntryPointDataFlowIfOpenAsync(null, cancellationToken);
            }
        }
    }

    // Mirrors ShowCompilationInfoAsync/ShowPreprocessorExplorerAsync: the
    // generation guard ensures a stale response (e.g. from a superseded
    // variant change or an earlier command invocation) can never overwrite
    // a newer one. A failed or cancelled request never regresses the window
    // to the "open a document" placeholder or leaves it stuck: it keeps the
    // last successful content when one exists, and otherwise shows an
    // explicit error. Issues its own hlsl/entryPointDataFlow request through
    // EntryPointDataFlowBridge -- a distinct protocol request, not a
    // different presentation of an existing one.
    private async Task ShowEntryPointDataFlowAsync(
        Uri uri,
        CancellationToken cancellationToken,
        EntryPointDataFlowToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref entryPointDataFlowRequestGeneration);
        // Captured before the request starts, independent of whether the
        // caller already held a window reference: the explicit Tools command
        // below always passes existingWindow: null, even when the window is
        // already open and already showing good content for this exact
        // document, so using existingWindow's null-ness alone to decide
        // whether to preserve that content on failure (as a prior version of
        // this method did) incorrectly erased it on every failed manual
        // retry. This performs a non-creating lookup only -- it must never
        // itself create or show the window, which stays governed solely by
        // ShowToolWindowAsync below, exactly as before.
        var priorWindow = existingWindow;
        if (priorWindow == null)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
            priorWindow = await FindToolWindowAsync(
                typeof(EntryPointDataFlowToolWindow),
                0,
                false,
                ambientCancellationToken) as EntryPointDataFlowToolWindow;
        }
        var hadMatchingDocument = EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
            priorWindow?.DocumentUri,
            uri);
        EntryPointDataFlowModel report = null;
        string failureMessage = null;
        try
        {
            report = await EntryPointDataFlowBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The entry-point data flow request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve entry-point data flow information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref entryPointDataFlowRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        // priorWindow above is used only for the preserve-on-failure
        // decision, never substituted in here: when existingWindow is null
        // (the explicit Tools-command path), ShowToolWindowAsync must
        // always run so an existing-but-hidden pane is revealed. Using
        // priorWindow as a stand-in for window would let ShowToolWindowAsync
        // be skipped whenever FindToolWindowAsync above happened to find an
        // already-constructed (but not necessarily visible/foregrounded)
        // pane instance, silently leaving a hidden window hidden. A
        // background refresh (which always passes its own existingWindow)
        // is unaffected and still never shows/forces focus.
        var window = existingWindow;
        if (EntryPointDataFlowRefreshLogic.ShouldRevealToolWindow(existingWindow != null))
        {
            window = await ShowToolWindowAsync(
                typeof(EntryPointDataFlowToolWindow),
                0,
                true,
                ambientCancellationToken) as EntryPointDataFlowToolWindow;
        }
        if (generation != Interlocked.Read(ref entryPointDataFlowRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, hadMatchingDocument);
            return;
        }
        if (report == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide entry-point data flow information.",
                hadMatchingDocument);
            return;
        }
        window?.SetReport(uri, report);
    }

    // Invoked after an active-variant selection, a debounced unsaved edit to
    // any open HLSL/header buffer or shadertoolsconfig.json, or a document
    // save. Only refreshes an already-open window. A save is treated as
    // relevant conservatively, by file type (a configured HLSL/header
    // extension, or shadertoolsconfig.json by name) rather than requiring an
    // exact match against the window's own root document: an #include'd
    // file's declarations/global accesses are reported against their own
    // uri, and a shadertoolsconfig.json change can change the active
    // variant's entry point/defines/include paths entirely, so restricting
    // this to the root document alone would leave the window stale after
    // exactly the changes that matter most. savedFilePath is null for a
    // variant change or a debounced edit refresh, which are always treated
    // as relevant once the window is open.
    public async Task RefreshEntryPointDataFlowIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        // A background save/variant/edit refresh must never supersede an
        // explicit Tools command the user is waiting for, but it must also
        // never be silently discarded: record it so ShowEntryPointDataFlowAsync's
        // explicit-command overload can replay a single bounded refresh once
        // that command completes, ensuring the window can never be left
        // stale immediately after an explicit request finishes.
        if (!entryPointDataFlowRefreshGate.TryBeginBackgroundRefresh())
        {
            // A background save/variant/edit refresh must never supersede
            // an explicit Tools command the user is waiting for; the gate
            // above records this refresh as pending rather than dropping it
            // (see ShowEntryPointDataFlowAsync's explicit-command overload,
            // which replays it once the explicit request finishes).
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(EntryPointDataFlowToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not EntryPointDataFlowToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null && !IsHlslOrConfigRelevantPath(savedFilePath))
        {
            return;
        }
        // Re-check after the asynchronous UI/tool-window lookup. An explicit
        // command may have started while this background refresh was
        // yielding; defer to it exactly as above rather than dropping this
        // refresh outright.
        if (!entryPointDataFlowRefreshGate.TryBeginBackgroundRefresh())
        {
            return;
        }
        // Bounds this refresh to a fixed timeout and coalesces it with any
        // earlier still-in-flight background refresh for this same window
        // (a burst of triggers -- e.g. a save arriving while a debounced
        // unsaved-edit refresh is still outstanding -- must not let
        // superseded requests accumulate; see CoalescingBackgroundRefreshCancellation).
        // The ambient cancellationToken is preserved separately below so a
        // timeout/coalescing cancellation still allows presentation logic
        // to run against the real package-lifetime token.
        var refreshCancellation =
            entryPointDataFlowBackgroundRefreshCancellation.BeginNext(cancellationToken);
        await ShowEntryPointDataFlowAsync(
            window.DocumentUri,
            refreshCancellation.Token,
            window,
            cancellationToken);
    }

    // Conservative relevance test for a saved file path used by
    // RefreshEntryPointDataFlowIfOpenAsync above: a configured HLSL/header
    // extension, or a file literally named shadertoolsconfig.json regardless
    // of its folder, both of which can change the reported data flow for the
    // currently shown document even though neither is that document itself.
    // Delegates to EntryPointDataFlowRefreshLogic (a pure, unit-tested
    // helper) for the actual decision; this wrapper only supplies the
    // UI-thread-affine configured-extensions lookup.
    private bool IsHlslOrConfigRelevantPath(string path)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        return EntryPointDataFlowRefreshLogic.IsHlslOrConfigRelevantPath(
            path,
            ParseExtensions(GetOptions().FileExtensions));
    }

    // Lazily resolves the plain VS editor services needed to anchor/re-
    // resolve a call-hierarchy root position against a live text buffer
    // (see TryGetOpenCallHierarchyBuffer/CreateRootTrackingPoint below).
    // Resolved at most once per package instance; a failure to resolve any
    // of them is never fatal -- callers simply fall back to a less precise
    // root position source (see CallHierarchyRootPositionResolver). These
    // are plain VS editor services already bundled by Microsoft.VisualStudio.SDK
    // (no new package reference), kept entirely isolated from
    // HlslLanguageClient/StreamJsonRpc, exactly like the rest of this
    // bootstrap package.
    private async Task EnsureCallHierarchyEditorServicesAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        callHierarchyComponentModel ??=
            await GetServiceAsync(typeof(SComponentModel)) as IComponentModel;
        callHierarchyEditorAdapters ??=
            callHierarchyComponentModel?.GetService<IVsEditorAdaptersFactoryService>();
        callHierarchyRunningDocuments ??=
            await GetServiceAsync(typeof(SVsRunningDocumentTable)) as IVsRunningDocumentTable;
    }

    // Looks up the ITextBuffer currently backing an open document by its
    // file path, independent of which view (if any) is active -- the
    // call-hierarchy root document need not still be the focused editor
    // when a background refresh runs. Returns null (never throws) when the
    // document is not open, or the editor services above could not be
    // resolved; both are treated identically to "no live buffer to anchor
    // against" by callers.
    private ITextBuffer TryGetOpenCallHierarchyBuffer(Uri documentUri)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (callHierarchyRunningDocuments == null || callHierarchyEditorAdapters == null)
        {
            return null;
        }
        string filePath;
        try
        {
            filePath = documentUri.LocalPath;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
        if (ErrorHandler.Failed(
                callHierarchyRunningDocuments.FindAndLockDocument(
                    (uint)_VSRDTFLAGS.RDT_NoLock,
                    filePath,
                    out _,
                    out _,
                    out var docData,
                    out _)) ||
            docData == IntPtr.Zero)
        {
            return null;
        }
        try
        {
            return Marshal.GetObjectForIUnknown(docData) is IVsTextBuffer adapter
                ? callHierarchyEditorAdapters.GetDocumentBuffer(adapter)
                : null;
        }
        finally
        {
            Marshal.Release(docData);
        }
    }

    // Creates a live anchor for (line, character) in buffer, tracking
    // edits made anywhere before it so a later background refresh can
    // re-resolve the *same* logical position rather than a stale raw
    // line/character (see CallHierarchyRootPositionResolver/
    // CallHierarchyExplorerToolWindow.RootTrackingPoint).
    // PointTrackingMode.Positive mirrors how the editor's own caret tracks
    // a position of interest: text inserted exactly at the anchored offset
    // shifts the anchor forward with it, rather than leaving it stranded
    // before newly typed content. Returns null (never throws) when
    // line/character do not correspond to a valid position in buffer's
    // current snapshot.
    private static ITrackingPoint CreateRootTrackingPoint(ITextBuffer buffer, int line, int character)
    {
        var snapshot = buffer.CurrentSnapshot;
        if (line < 0 || line >= snapshot.LineCount)
        {
            return null;
        }
        var snapshotLine = snapshot.GetLineFromLineNumber(line);
        if (character < 0 || character > snapshotLine.LengthIncludingLineBreak)
        {
            return null;
        }
        var position = snapshotLine.Start.Position + character;
        if (position > snapshot.Length)
        {
            return null;
        }
        return snapshot.CreateTrackingPoint(position, PointTrackingMode.Positive);
    }

    // Translates trackingPoint to a (line, character) tuple against its
    // buffer's CURRENT snapshot -- the whole point of anchoring with an
    // ITrackingPoint is that this reflects any inserts/deletes made
    // anywhere before it since it was created. Returns null (never throws)
    // when trackingPoint is null.
    private static (int Line, int Character)? ResolveTrackedPosition(ITrackingPoint trackingPoint)
    {
        if (trackingPoint == null)
        {
            return null;
        }
        var snapshot = trackingPoint.TextBuffer.CurrentSnapshot;
        var point = trackingPoint.GetPoint(snapshot);
        var containingLine = point.GetContainingLine();
        return (containingLine.LineNumber, point.Position - containingLine.Start.Position);
    }

    // The custom Call Hierarchy surface (Tools > HLSL Call Hierarchy):
    // Visual Studio 17.14's generic ILanguageClient infrastructure does not
    // route the editor's built-in View Call Hierarchy command to any
    // language client, regardless of the callHierarchyProvider capability
    // it advertises -- there is no bespoke call-hierarchy hookup in that
    // SDK the way there is for hover/signature-help/go-to-definition. This
    // command, its tool window, and the three requests below
    // (textDocument/prepareCallHierarchy, then callHierarchy/incomingCalls
    // and callHierarchy/outgoingCalls -- see docs/call-hierarchy.md) are
    // the custom replacement.
    private async Task ShowCallHierarchyAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var textManager = await GetServiceAsync(typeof(SVsTextManager)) as IVsTextManager;
        if (textManager == null ||
            ErrorHandler.Failed(textManager.GetActiveView(1, null, out var view)) ||
            view == null ||
            ErrorHandler.Failed(view.GetCaretPos(out var line, out var character)) ||
            ErrorHandler.Failed(view.GetBuffer(out var lines)) ||
            lines is not IVsUserData userData)
        {
            await ShowInformationAsync(
                "Open an HLSL document, place the caret on a function, then run " +
                "Tools > HLSL Call Hierarchy.",
                cancellationToken);
            return;
        }
        var monikerKey = VSConstants.VsTextBufferUserDataGuid.VsBufferMoniker_guid;
        if (ErrorHandler.Failed(userData.GetData(ref monikerKey, out var value)) ||
            value is not string moniker)
        {
            await ShowInformationAsync(
                "Open an HLSL document, place the caret on a function, then run " +
                "Tools > HLSL Call Hierarchy.",
                cancellationToken);
            return;
        }
        var uri = new Uri(Path.GetFullPath(moniker));

        // Anchors the explicit request's caret position in the live
        // buffer so later background refreshes can re-resolve the same
        // logical position even after unsaved edits made anywhere before
        // it (see CreateRootTrackingPoint/CallHierarchyRootPositionResolver).
        // A null tracking point (editor services unavailable, or the
        // position somehow out of range) is always safe: refreshes simply
        // fall back to the root item's own SelectionRange, or the raw
        // caret position captured here.
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var buffer = lines is IVsTextBuffer bufferAdapter
            ? callHierarchyEditorAdapters?.GetDocumentBuffer(bufferAdapter)
            : null;
        var rootTrackingPoint = buffer != null
            ? CreateRootTrackingPoint(buffer, line, character)
            : null;

        callHierarchyRefreshGate.EnterExplicitRequest();
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                await EstablishCallHierarchyRootAsync(
                    uri,
                    line,
                    character,
                    rootTrackingPoint,
                    requestCancellation.Token,
                    cancellationToken);
            }
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest())
            {
                // Mirrors ShowEntryPointDataFlowAsync: a refresh trigger that
                // arrived while this explicit request was in flight was
                // deferred, not dropped -- replay it once now.
                await RefreshCallHierarchyIfOpenAsync(null, cancellationToken);
            }
        }
    }

    // Resolves the callable at (uri, line, character) via
    // textDocument/prepareCallHierarchy, then fetches its incoming and
    // outgoing calls, and always shows/reveals the tool window (an explicit
    // Tools-command invocation must never leave an existing-but-hidden pane
    // hidden -- there is no "existing window" preservation concern here the
    // way ShowEntryPointDataFlowAsync has, since establishing a *new* root
    // always intentionally replaces whatever was shown before).
    private async Task EstablishCallHierarchyRootAsync(
        Uri uri,
        int line,
        int character,
        ITrackingPoint rootTrackingPoint,
        CancellationToken cancellationToken,
        CancellationToken ambientCancellationToken)
    {
        var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);
        IReadOnlyList<CallHierarchyItemModel> prepared = null;
        string failureMessage = null;
        try
        {
            prepared = await CallHierarchyBridge.PrepareAsync(uri, line, character, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The call hierarchy request was cancelled.";
        }
        catch (CallHierarchyContentModifiedException)
        {
            failureMessage = CallHierarchyExplorerDisplay.StaleItemMessage();
        }
        catch (Exception error)
        {
            failureMessage = CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }

        // The server guarantees at most one element (see
        // docs/call-hierarchy.md), so "the first item, or not-callable" is
        // never actually ambiguous.
        var item = failureMessage == null && prepared != null && prepared.Count > 0
            ? prepared[0]
            : null;
        CallHierarchyFrame frame = null;
        if (failureMessage == null && item != null)
        {
            (frame, failureMessage) = await FetchCallHierarchyFrameAsync(
                item,
                cancellationToken,
                ambientCancellationToken);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = await ShowToolWindowAsync(
            typeof(CallHierarchyExplorerToolWindow),
            0,
            true,
            ambientCancellationToken) as CallHierarchyExplorerToolWindow;
        WireCallHierarchyWindow(window, ambientCancellationToken);
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }
        if (failureMessage == null && item == null)
        {
            // An authoritative "no callable symbol here" result, not a
            // transient failure -- always overwrites, mirroring
            // EntryPointDataFlowToolWindow's found:false handling.
            window?.SetNotCallable();
            return;
        }
        if (failureMessage != null)
        {
            if (window?.CurrentItem != null)
            {
                window.SetBannerOnCurrent(failureMessage);
            }
            else
            {
                window?.SetGlobalError(failureMessage);
            }
            return;
        }
        window?.SetRoot(uri, line, character, frame, rootTrackingPoint);
    }

    // Invoked when the user clicks "Explore calls" on a caller/callee row:
    // re-centers the tool window on that item by fetching its own
    // incoming/outgoing calls. The item's opaque `data` (identity envelope)
    // is already known from the previous response, so no new
    // prepareCallHierarchy call is needed -- see docs/call-hierarchy.md.
    // Treated as an explicit, gated, timeout-bounded operation exactly like
    // the Tools command itself, since a concurrent background refresh must
    // not silently drop or be dropped by it. A failed drill-in never
    // mutates the tool window's persisted state (unlike a failed
    // root/refresh): the currently displayed frame is left exactly as-is
    // and the failure is surfaced through a one-off message box, so a bad
    // drill-in attempt can never corrupt or blank out an otherwise good
    // view.
    internal async Task PerformCallHierarchyDrillInAsync(
        CallHierarchyItemModel item,
        CallHierarchySection section,
        CancellationToken cancellationToken)
    {
        if (item == null)
        {
            return;
        }
        // Captured before the network round-trip below so a concurrent
        // Back click (or another drill-in/refresh) that changes the stack
        // while this is in flight can be detected and rejected once the
        // frame is ready to push (see the NavigationRevision check before
        // PushFrame). Uses a non-creating lookup, mirroring
        // RefreshCallHierarchyIfOpenAsync -- a drill-in only ever
        // originates from a row click within an already-open window, so
        // windowBeforeFetch is null only in the defensive/unreachable case
        // of the window having been closed between the click and here; ??
        // 0 matches a freshly (re-)created window's own starting
        // revision, so that edge case still pushes correctly rather than
        // spuriously rejecting the very first drill-in.
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var windowBeforeFetch = await FindToolWindowAsync(
                typeof(CallHierarchyExplorerToolWindow),
                0,
                false,
                cancellationToken)
            as CallHierarchyExplorerToolWindow;
        var revisionAtStart = windowBeforeFetch?.NavigationRevision ?? 0;
        callHierarchyRefreshGate.EnterExplicitRequest();
        try
        {
            using (var requestCancellation =
                   CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                requestCancellation.CancelAfter(TimeSpan.FromSeconds(30));
                var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);
                var (frame, failureMessage) = await FetchCallHierarchyFrameAsync(
                    item,
                    requestCancellation.Token,
                    cancellationToken);
                if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
                {
                    return;
                }
                await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
                if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
                {
                    return;
                }
                if (failureMessage != null)
                {
                    VsShellUtilities.ShowMessageBox(
                        this,
                        failureMessage,
                        "HLSL Call Hierarchy",
                        OLEMSGICON.OLEMSGICON_WARNING,
                        OLEMSGBUTTON.OLEMSGBUTTON_OK,
                        OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
                    return;
                }
                var window = await ShowToolWindowAsync(
                    typeof(CallHierarchyExplorerToolWindow),
                    0,
                    true,
                    cancellationToken) as CallHierarchyExplorerToolWindow;
                WireCallHierarchyWindow(window, cancellationToken);
                if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
                {
                    return;
                }
                if (window != null && window.NavigationRevision != revisionAtStart)
                {
                    // The user navigated (most importantly, pressed Back)
                    // while this drill-in's network round-trip was in
                    // flight: pushing this frame now would land on top of
                    // a stack the user has already moved away from,
                    // silently overriding their navigation. Discard it
                    // rather than applying a decision made against an
                    // outdated stack.
                    return;
                }
                window?.PushFrame(item, section, frame);
            }
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest())
            {
                await RefreshCallHierarchyIfOpenAsync(null, cancellationToken);
            }
        }
    }

    // Invoked after an active-variant selection, a document save, or a
    // debounced unsaved edit to any open HLSL/header buffer or
    // shadertoolsconfig.json (see RefreshVariantDependentWindows/
    // OnAfterSave/DebounceUnsavedHlslBufferRefreshAsync in
    // HlslLspActivator). Every one of those triggers is exactly the kind of
    // change that bumps the root item's own generation (see
    // docs/call-hierarchy.md), so simply round-tripping the previously
    // resolved CallHierarchyItem's opaque `data` back into incomingCalls/
    // outgoingCalls (as an earlier version of this method did) would be
    // guaranteed to fail with ContentModified every single time this
    // refresh actually runs -- it could never succeed once triggered. This
    // re-runs textDocument/prepareCallHierarchy at the originally captured
    // root position to obtain a genuinely fresh item, confirms it is still
    // the *same* declaration as the currently displayed root (see
    // CallHierarchyItemIdentity.IsSameCallable -- a tracking point can land
    // on a different callable after the original was deleted/replaced, or
    // after the document was closed and reopened with different content),
    // then, if the user had drilled deeper than the root, attempts to
    // relocate each drilled item within its parent's freshly fetched list
    // by stable cross-generation identity (see CallHierarchyItemIdentity).
    // The rebuild is all-or-nothing: on full success the entire stack is
    // replaced in place (preserving depth); if the root itself is no
    // longer callable, an authoritative not-callable result is shown; if
    // the root's identity cannot be confirmed, or on any other failure,
    // the last successful content is preserved with a banner; if only the
    // drilled path can't be relocated, the view resets to the fresh root
    // with an explanatory banner rather than showing a partially-rebuilt or
    // possibly-wrong path. Only refreshes an already-open window with an
    // established root; never creates or shows one. Uses the same broad,
    // conservative relevance test as RefreshEntryPointDataFlowIfOpenAsync (a
    // configured HLSL/header extension, or shadertoolsconfig.json by name)
    // since an #include'd dependency or a variant/config change can change
    // the current item's own callers/callees or generation without the
    // currently displayed item's own document changing.
    public async Task RefreshCallHierarchyIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken)
    {
        if (!callHierarchyRefreshGate.TryBeginBackgroundRefresh())
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(CallHierarchyExplorerToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not CallHierarchyExplorerToolWindow window ||
            window.RootDocumentUri == null)
        {
            return;
        }
        if (savedFilePath != null && !IsHlslOrConfigRelevantPath(savedFilePath))
        {
            return;
        }
        // Re-check after the asynchronous UI/tool-window lookup, matching
        // RefreshEntryPointDataFlowIfOpenAsync's own double-check.
        if (!callHierarchyRefreshGate.TryBeginBackgroundRefresh())
        {
            return;
        }

        // Captured before any request is (re-)issued: the raw root
        // position/document/drill-in path last established, and the
        // navigation revision at that exact moment (see
        // CallHierarchyExplorerState.Revision). If the user navigates
        // (most importantly, presses Back) before this refresh finishes,
        // the revision will have moved on by the time this method is ready
        // to apply its result, and that result must then be discarded
        // rather than silently overwriting/undoing the user's navigation
        // (checked just before committing, below).
        var rootUri = window.RootDocumentUri;
        var rootLine = window.RootLine;
        var rootCharacter = window.RootCharacter;
        var path = window.CapturePathSteps();
        var revisionAtStart = window.NavigationRevision;

        // Resolve the position to re-run prepareCallHierarchy at: prefer a
        // live tracking-point translation, which alone correctly follows
        // unsaved inserts/deletes made anywhere before the root since it
        // was established; fall back to the last resolved root item's own
        // compiler-supplied SelectionRange when the tracking point's
        // buffer is no longer the live buffer for this document (closed
        // and reopened, or externally reloaded -- a new buffer identity
        // the old tracking point cannot follow); finally fall back to the
        // raw position captured at root establishment (see
        // CallHierarchyRootPositionResolver).
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var liveBuffer = TryGetOpenCallHierarchyBuffer(rootUri);
        var trackedPosition =
            liveBuffer != null &&
            window.RootTrackingPoint != null &&
            ReferenceEquals(liveBuffer, window.RootTrackingPoint.TextBuffer)
                ? ResolveTrackedPosition(window.RootTrackingPoint)
                : null;
        (int Line, int Character)? fallbackSelectionStart = null;
        var rootItemSelectionStart = window.RootItem?.SelectionRange?.Start;
        if (rootItemSelectionStart != null)
        {
            fallbackSelectionStart = ((int)rootItemSelectionStart.Line, (int)rootItemSelectionStart.Character);
        }
        var (resolvedLine, resolvedCharacter) = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedPosition,
            fallbackSelectionStart,
            rootLine,
            rootCharacter);

        // Bounds this refresh to a fixed timeout and coalesces it with any
        // earlier still-in-flight background refresh for this window (see
        // CoalescingBackgroundRefreshCancellation) so a burst of triggers
        // cannot accumulate unboundedly in-flight requests.
        var refreshCancellation =
            callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
        var token = refreshCancellation.Token;
        var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);

        IReadOnlyList<CallHierarchyItemModel> prepared = null;
        string failureMessage = null;
        try
        {
            prepared = await CallHierarchyBridge.PrepareAsync(rootUri, resolvedLine, resolvedCharacter, token);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The call hierarchy request was cancelled.";
        }
        catch (CallHierarchyContentModifiedException)
        {
            failureMessage = CallHierarchyExplorerDisplay.StaleItemMessage();
        }
        catch (Exception error)
        {
            failureMessage = CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            // Never treat a guaranteed-stale, superseded response as
            // successful: a newer request (explicit or background) has
            // already taken over, so this one must simply stop without
            // touching the window at all.
            return;
        }

        // The server guarantees at most one element (see
        // docs/call-hierarchy.md).
        var rootItem = failureMessage == null && prepared != null && prepared.Count > 0
            ? prepared[0]
            : null;

        // Before trusting this freshly re-prepared item as a continuation
        // of the currently displayed root, confirm it is actually the
        // *same* declaration (see CallHierarchyItemIdentity.IsSameCallable)
        // -- a tracking point (or its fallbacks) can land on an unrelated
        // callable after the original was deleted, replaced by a
        // differently-named-or-typed declaration, or the document was
        // closed and reopened with different content at that position.
        // Reusing failureMessage here deliberately routes this into the
        // exact same "preserve last successful content, show a banner"
        // handling as any other transient failure below -- this is not a
        // transient error, but the safe behavior (never silently switch
        // roots) is identical.
        if (failureMessage == null &&
            rootItem != null &&
            !CallHierarchyItemIdentity.IsSameCallable(window.RootItem, rootItem))
        {
            failureMessage = CallHierarchyExplorerDisplay.RootIdentityChangedMessage();
        }

        CallHierarchyFrame rootFrame = null;
        if (failureMessage == null && rootItem != null)
        {
            (rootFrame, failureMessage) = await FetchCallHierarchyFrameAsync(rootItem, token, cancellationToken);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }

        // A fresh anchor for the root actually resolved above, used to
        // update the window's tracked position going forward (a stale
        // caret-derived resolvedLine/resolvedCharacter should not linger
        // as the anchor once a compiler-verified SelectionRange for the
        // same declaration is available). liveBuffer above is still the
        // correct buffer to anchor against: rootUri itself never changes
        // across a refresh, only the position within it might.
        var anchorLine = rootItem?.SelectionRange?.Start != null
            ? (int)rootItem.SelectionRange.Start.Line
            : resolvedLine;
        var anchorCharacter = rootItem?.SelectionRange?.Start != null
            ? (int)rootItem.SelectionRange.Start.Character
            : resolvedCharacter;
        var freshTrackingPoint = liveBuffer != null
            ? CreateRootTrackingPoint(liveBuffer, anchorLine, anchorCharacter)
            : null;

        // Attempt to rebuild the user's drill-in path (if any) underneath
        // the freshly fetched root, one step at a time. All-or-nothing: any
        // step that cannot be relocated (removed/renamed declaration) or
        // whose own frame cannot be fetched abandons the rebuild in favor
        // of resetting to the fresh root with an explanation.
        List<CallHierarchyFrame> rebuiltFrames = null;
        List<CallHierarchyPathStep> rebuiltSteps = null;
        string rebuildFailureMessage = null;
        if (failureMessage == null && rootFrame != null && path.Count > 0)
        {
            rebuiltFrames = new List<CallHierarchyFrame> { rootFrame };
            rebuiltSteps = new List<CallHierarchyPathStep>();
            var parentFrame = rootFrame;
            foreach (var step in path)
            {
                var matched = CallHierarchyItemIdentity.FindMatch(parentFrame, step);
                if (matched == null)
                {
                    rebuiltFrames = null;
                    rebuildFailureMessage = CallHierarchyExplorerDisplay.PathNotRelocatedMessage();
                    break;
                }
                var (stepFrame, stepFailureMessage) =
                    await FetchCallHierarchyFrameAsync(matched, token, cancellationToken);
                if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
                {
                    return;
                }
                if (stepFrame == null)
                {
                    rebuiltFrames = null;
                    rebuildFailureMessage = stepFailureMessage
                        ?? CallHierarchyExplorerDisplay.PathNotRelocatedMessage();
                    break;
                }
                rebuiltFrames.Add(stepFrame);
                rebuiltSteps.Add(step);
                parentFrame = stepFrame;
            }
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }
        if (window.NavigationRevision != revisionAtStart)
        {
            // The user navigated (most importantly, pressed Back) while
            // this refresh was in flight: applying its result now --
            // whatever it is -- would silently overwrite or undo that
            // navigation. Discard it outright rather than reconciling a
            // now-outdated result with a since-changed stack; the next
            // refresh trigger (or a manual Tools command) will re-capture
            // a correct revision and path from the user's actual current
            // position.
            return;
        }
        if (failureMessage == null && rootItem == null)
        {
            // The previously callable root no longer resolves to anything
            // callable (e.g. the function was deleted or renamed) -- an
            // authoritative result, not a transient failure, exactly like
            // EstablishCallHierarchyRootAsync's own not-callable handling.
            window.SetNotCallable();
            return;
        }
        if (failureMessage != null)
        {
            // Preserve the last successful content on a transient
            // failure/stale response -- only overlay a banner, matching the
            // "preserve last successful content on transient errors"
            // contract established for entry-point data flow.
            window.SetBannerOnCurrent(failureMessage);
            return;
        }
        if (rebuiltFrames != null)
        {
            // The full drilled path was successfully relocated underneath
            // the fresh root: replace the entire stack in one step,
            // preserving the user's drill-in depth and clearing any earlier
            // banner, since this is a fully fresh, successful result. The
            // root anchor is refreshed too, so a *future* refresh tracks
            // forward from here rather than the now-outdated position.
            window.UpdateRootAnchor(rootUri, anchorLine, anchorCharacter, freshTrackingPoint);
            window.ReplaceAllFrames(rebuiltFrames, rebuiltSteps);
            return;
        }
        // Either there was no drill-in path to rebuild (a root-only refresh
        // fully succeeds with the fresh root alone) or the path could not be
        // relocated: reset to the fresh root either way, but only attach an
        // explanatory banner when a path actually failed to relocate.
        window.SetRoot(rootUri, anchorLine, anchorCharacter, rootFrame, freshTrackingPoint);
        if (rebuildFailureMessage != null)
        {
            window.SetBannerOnCurrent(rebuildFailureMessage);
        }
    }

    private static async Task<(CallHierarchyFrame Frame, string FailureMessage)> FetchCallHierarchyFrameAsync(
        CallHierarchyItemModel item,
        CancellationToken cancellationToken,
        CancellationToken ambientCancellationToken)
    {
        try
        {
            var incomingCalls =
                await CallHierarchyBridge.RequestIncomingCallsAsync(item, cancellationToken);
            var outgoingCalls =
                await CallHierarchyBridge.RequestOutgoingCallsAsync(item, cancellationToken);
            return (new CallHierarchyFrame(item, incomingCalls, outgoingCalls), null);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            return (null, "The call hierarchy request was cancelled.");
        }
        catch (CallHierarchyContentModifiedException)
        {
            return (null, CallHierarchyExplorerDisplay.StaleItemMessage());
        }
        catch (Exception error)
        {
            return (null, CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message));
        }
    }

    // Wires the tool window's Back/"Explore calls" interactions to this
    // package exactly once per window instance (EnsureWired itself is
    // idempotent, since ShowToolWindowAsync/FindToolWindowAsync return the
    // same singleton pane across every invocation).
    private void WireCallHierarchyWindow(
        CallHierarchyExplorerToolWindow window,
        CancellationToken cancellationToken)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        window?.EnsureWired(
            (item, section) => JoinableTaskFactory.RunAsync(
                    () => PerformCallHierarchyDrillInAsync(item, section, cancellationToken))
                .FileAndForget("HlslLsp/CallHierarchyDrillIn"));
    }

    private async Task SelectVariantAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        VariantListModel variants = null;
        try
        {
            variants = await VariantBridge.ListAsync(uri, cancellationToken);
        }
        catch (Exception)
        {
            // A missing or failed server connection is reported below.
        }
        if (variants?.Variants == null || variants.Variants.Count == 0)
        {
            await ShowInformationAsync(
                VariantBridge.IsAvailable
                    ? "No shader variants are declared under hlsl.variants in shadertoolsconfig.json."
                    : "Open an HLSL document so the language server can load shader variants.",
                cancellationToken);
            return;
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var dialog = new VariantSelectionDialog(variants);
        if (dialog.ShowModal() == true)
        {
            await VariantBridge.SetActiveAsync(dialog.SelectedVariant, cancellationToken);
        }
    }

    private async Task<Uri> GetActiveDocumentUriAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var textManager = await GetServiceAsync(typeof(SVsTextManager)) as IVsTextManager;
        if (textManager == null ||
            ErrorHandler.Failed(textManager.GetActiveView(1, null, out var view)) ||
            view == null ||
            ErrorHandler.Failed(view.GetBuffer(out var lines)) ||
            lines is not IVsUserData userData)
        {
            return null;
        }
        var monikerKey = VSConstants.VsTextBufferUserDataGuid.VsBufferMoniker_guid;
        if (ErrorHandler.Failed(userData.GetData(ref monikerKey, out var value)) ||
            value is not string moniker)
        {
            return null;
        }
        return new Uri(Path.GetFullPath(moniker));
    }

    private async Task ShowInformationAsync(string message, CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        VsShellUtilities.ShowMessageBox(
            this,
            message,
            "HLSL-LSP",
            OLEMSGICON.OLEMSGICON_INFO,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    private async Task TryActivateLanguageClientAsync(
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var configuredExtensions = new HashSet<string>(
            ParseExtensions(GetOptions().FileExtensions),
            StringComparer.OrdinalIgnoreCase);
        lock (Gate)
        {
            if (activationStarted ||
                !PendingDocuments.Any(
                    path => configuredExtensions.Contains(Path.GetExtension(path))))
            {
                return;
            }
            activationStarted = true;
        }

        try
        {
            await LoadLanguageClientAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            lock (Gate)
            {
                activationStarted = false;
            }
        }
        catch (Exception error)
        {
            ActivityLog.LogError(nameof(HlslBootstrapPackage), error.ToString());
            lock (Gate)
            {
                activationStarted = false;
            }
        }
    }

    private async Task LoadLanguageClientAsync(
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var directory = Path.GetDirectoryName(GetType().Assembly.Location);
        var assembly = Assembly.LoadFrom(
            Path.Combine(directory, "Client", "HlslLsp.VisualStudio.dll"));
        var activator = assembly.GetType(
            "HlslLsp.VisualStudio.HlslLspActivator",
            throwOnError: true);
        var activate = activator.GetMethod(
            "ActivateAsync",
            BindingFlags.Public | BindingFlags.Static);
        if (activate == null)
        {
            throw new InvalidOperationException(
                "The HLSL language client activation entry point is unavailable.");
        }

        var task = activate.Invoke(
            null,
            new object[] { this, cancellationToken }) as Task;
        if (task == null)
        {
            throw new InvalidOperationException(
                "The HLSL language client did not return an activation task.");
        }
        await task;
    }

    public static IEnumerable<string> ParseExtensions(string value)
    {
        return (value ?? string.Empty)
            .Split(new[] { ';', ',', ' ' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(extension => extension.Trim())
            .Where(extension => extension.Length > 0)
            .Select(extension => extension[0] == '.' ? extension : "." + extension)
            .Distinct(StringComparer.OrdinalIgnoreCase);
    }

    internal static void NotifyOptionsChanged()
    {
        OptionsChanged?.Invoke();
        HlslBootstrapPackage package;
        lock (Gate)
        {
            package = instance;
        }
        package?.JoinableTaskFactory.RunAsync(
                () => package.TryActivateLanguageClientAsync(package.DisposalToken))
            .FileAndForget("HlslLsp/ApplyBootstrapOptions");
    }
}

public sealed class HlslOptionsSnapshot
{
    public HlslOptionsSnapshot(
        string fileExtensions,
        string languageVersion,
        string dxcRuntimeDirectory,
        InlayHintOptionsSnapshot inlayHints)
    {
        FileExtensions = fileExtensions;
        LanguageVersion = languageVersion;
        DxcRuntimeDirectory = dxcRuntimeDirectory;
        InlayHints = inlayHints;
    }

    public string FileExtensions { get; }

    public string LanguageVersion { get; }

    public string DxcRuntimeDirectory { get; }

    public InlayHintOptionsSnapshot InlayHints { get; }
}

public sealed class InlayHintOptionsSnapshot
{
    public InlayHintOptionsSnapshot(
        bool types,
        bool parameters,
        bool matrixOrientation,
        bool registers,
        bool packedOffsets,
        bool arrayStrides,
        bool activeVariant)
    {
        Types = types;
        Parameters = parameters;
        MatrixOrientation = matrixOrientation;
        Registers = registers;
        PackedOffsets = packedOffsets;
        ArrayStrides = arrayStrides;
        ActiveVariant = activeVariant;
    }

    public bool Types { get; }
    public bool Parameters { get; }
    public bool MatrixOrientation { get; }
    public bool Registers { get; }
    public bool PackedOffsets { get; }
    public bool ArrayStrides { get; }
    public bool ActiveVariant { get; }
}

[TypeDescriptionProvider(typeof(HlslOptionsTypeDescriptionProvider))]
public sealed class HlslOptionsPage : DialogPage
{
    private string fileExtensions = ".hlsl;.hlsli;.usf";
    private string languageVersion = "2021";
    private string dxcRuntimeDirectory = "";
    private bool inlayHintTypes = true;
    private bool inlayHintParameters = true;
    private bool inlayHintMatrixOrientation;
    private bool inlayHintRegisters;
    private bool inlayHintPackedOffsets;
    private bool inlayHintArrayStrides;
    private bool inlayHintActiveVariant = true;

    [Category("Files")]
    [System.ComponentModel.DisplayName("HLSL file extensions")]
    [Description(
        "Semicolon-separated file extensions to treat as HLSL. " +
        "The built-in .hlsl and .hlsli extensions are always supported.")]
    public string FileExtensions
    {
        get => fileExtensions;
        set
        {
            if (string.Equals(fileExtensions, value, StringComparison.Ordinal))
            {
                return;
            }
            fileExtensions = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("Language")]
    [System.ComponentModel.DisplayName("Default HLSL language version")]
    [Description(
        "The default DXC -HV value, such as 2016, 2017, 2018, 2021, or 202x. " +
        "A shadertoolsconfig.json languageVersion setting takes precedence.")]
    public string LanguageVersion
    {
        get => languageVersion;
        set
        {
            if (string.Equals(languageVersion, value, StringComparison.Ordinal))
            {
                return;
            }
            languageVersion = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("DXC")]
    [System.ComponentModel.DisplayName("DXC runtime directory")]
    [Description(
        "Directory containing a compatible DXC runtime to load instead of the " +
        "bundled one (dxcompiler.dll and dxil.dll). An explicit value overrides " +
        "shadertoolsconfig.json. Leave empty to use the bundled runtime. " +
        "Changing this restarts the language server.")]
    public string DxcRuntimeDirectory
    {
        get => dxcRuntimeDirectory;
        set
        {
            if (string.Equals(dxcRuntimeDirectory, value, StringComparison.Ordinal))
            {
                return;
            }
            dxcRuntimeDirectory = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Inferred types")]
    [Description("Show compiler-inferred type hints.")]
    public bool InlayHintTypes
    {
        get => inlayHintTypes;
        set => SetOption(ref inlayHintTypes, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Parameter names")]
    [Description("Show unambiguous overload parameter-name hints at call sites.")]
    public bool InlayHintParameters
    {
        get => inlayHintParameters;
        set => SetOption(ref inlayHintParameters, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Matrix orientation")]
    [Description("Show compiler-derived row-major or column-major matrix orientation hints.")]
    public bool InlayHintMatrixOrientation
    {
        get => inlayHintMatrixOrientation;
        set => SetOption(ref inlayHintMatrixOrientation, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Resource registers")]
    [Description("Show reflected resource register and space hints.")]
    public bool InlayHintRegisters
    {
        get => inlayHintRegisters;
        set => SetOption(ref inlayHintRegisters, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Packed offsets")]
    [Description("Show compiler-derived constant-buffer packed-offset hints.")]
    public bool InlayHintPackedOffsets
    {
        get => inlayHintPackedOffsets;
        set => SetOption(ref inlayHintPackedOffsets, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Array strides")]
    [Description("Show compiler-derived array-stride hints.")]
    public bool InlayHintArrayStrides
    {
        get => inlayHintArrayStrides;
        set => SetOption(ref inlayHintArrayStrides, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Active variant")]
    [Description("Show the selected compilation variant where it affects the current shader.")]
    public bool InlayHintActiveVariant
    {
        get => inlayHintActiveVariant;
        set => SetOption(ref inlayHintActiveVariant, value);
    }

    private static void SetOption(ref bool field, bool value)
    {
        if (field == value)
        {
            return;
        }
        field = value;
        HlslBootstrapPackage.NotifyOptionsChanged();
    }

    internal sealed class HlslOptionsTypeDescriptionProvider : TypeDescriptionProvider
    {
        private const string AttributeTypeName =
            "Microsoft.VisualStudio.Shell.UnifiedSettingsMonikerAttribute, " +
            "Microsoft.VisualStudio.Shell.15.0";

        public HlslOptionsTypeDescriptionProvider()
            : base(TypeDescriptor.GetProvider(typeof(DialogPage)))
        {
        }

        public override ICustomTypeDescriptor GetTypeDescriptor(
            Type objectType,
            object instance)
        {
            return new HlslOptionsTypeDescriptor(
                base.GetTypeDescriptor(objectType, instance));
        }

        private sealed class HlslOptionsTypeDescriptor : CustomTypeDescriptor
        {
            public HlslOptionsTypeDescriptor(ICustomTypeDescriptor parent)
                : base(parent)
            {
            }

            public override PropertyDescriptorCollection GetProperties()
            {
                return AddUnifiedSettingsMonikers(base.GetProperties());
            }

            public override PropertyDescriptorCollection GetProperties(
                Attribute[] attributes)
            {
                return AddUnifiedSettingsMonikers(base.GetProperties(attributes));
            }

            private static PropertyDescriptorCollection AddUnifiedSettingsMonikers(
                PropertyDescriptorCollection properties)
            {
                var attributeType = Type.GetType(AttributeTypeName, throwOnError: true);
                var result = properties.Cast<PropertyDescriptor>()
                    .Select(property =>
                    {
                        var moniker = GetMoniker(property.Name);
                        if (moniker == null)
                        {
                            return property;
                        }

                        var attribute = (Attribute)Activator.CreateInstance(
                            attributeType,
                            moniker);
                        return TypeDescriptor.CreateProperty(
                            typeof(HlslOptionsPage),
                            property,
                            attribute);
                    })
                    .ToArray();
                return new PropertyDescriptorCollection(result, readOnly: true);
            }

            private static string GetMoniker(string propertyName)
            {
                switch (propertyName)
                {
                    case nameof(HlslOptionsPage.FileExtensions):
                        return "hlslLsp.general.fileExtensions";
                    case nameof(HlslOptionsPage.LanguageVersion):
                        return "hlslLsp.general.languageVersion";
                    case nameof(HlslOptionsPage.DxcRuntimeDirectory):
                        return "hlslLsp.general.dxcRuntimeDirectory";
                    case nameof(HlslOptionsPage.InlayHintTypes):
                        return "hlslLsp.general.inlayHintTypes";
                    case nameof(HlslOptionsPage.InlayHintParameters):
                        return "hlslLsp.general.inlayHintParameters";
                    case nameof(HlslOptionsPage.InlayHintMatrixOrientation):
                        return "hlslLsp.general.inlayHintMatrixOrientation";
                    case nameof(HlslOptionsPage.InlayHintRegisters):
                        return "hlslLsp.general.inlayHintRegisters";
                    case nameof(HlslOptionsPage.InlayHintPackedOffsets):
                        return "hlslLsp.general.inlayHintPackedOffsets";
                    case nameof(HlslOptionsPage.InlayHintArrayStrides):
                        return "hlslLsp.general.inlayHintArrayStrides";
                    case nameof(HlslOptionsPage.InlayHintActiveVariant):
                        return "hlslLsp.general.inlayHintActiveVariant";
                    default:
                        return null;
                }
            }
        }
    }

    protected override void OnApply(PageApplyEventArgs e)
    {
        base.OnApply(e);
        if (e.ApplyBehavior == ApplyKind.Apply)
        {
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }
}
