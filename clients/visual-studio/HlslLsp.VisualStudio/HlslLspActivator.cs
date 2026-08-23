using System;
using System.Collections.Generic;
using System.Linq;
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
    IVsSelectionEvents
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
    private HashSet<string> configuredExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private bool servicesReady;

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
        languageClient = new HlslLanguageClient(GetOptions().LanguageVersion);
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
        if (languageClient != null)
        {
            await languageClient.UpdateLanguageVersionAsync(options.LanguageVersion);
        }
    }

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
            return;
        }
        if (buffer.ContentType.IsOfType("HLSL"))
        {
            if (!buffer.ContentType.IsOfType("HLSL-LSP-Colored"))
            {
                buffer.ChangeContentType(remoteShaderContentType, this);
            }
            return;
        }

        if (textDocuments.TryGetTextDocument(buffer, out var document) &&
            configuredExtensions.Contains(
                System.IO.Path.GetExtension(document.FilePath)))
        {
            buffer.ChangeContentType(remoteShaderContentType, this);
        }
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
