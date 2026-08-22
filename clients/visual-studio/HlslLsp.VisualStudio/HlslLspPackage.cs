using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;
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
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("HLSL-LSP", "DXC-powered HLSL IntelliSense", "0.5.16")]
[ProvideAutoLoad(VSConstants.UICONTEXT.ShellInitialized_string, PackageAutoLoadFlags.BackgroundLoad)]
[ProvideOptionPage(typeof(HlslOptionsPage), "HLSL-LSP", "General", 0, 0, true)]
[Guid(PackageGuidString)]
public sealed class HlslLspPackage :
    AsyncPackage,
    IVsSolutionEvents,
    IVsSolutionEvents7
{
    public const string PackageGuidString = "d1d7cf67-e1e0-452d-9a2e-1f556f76c1d7";

    private static readonly object Gate = new();
    private static HlslLspPackage instance;

    private readonly HashSet<string> registeredExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, IContentType> replacedExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private IContentTypeRegistryService contentTypes;
    private IFileExtensionRegistryService fileExtensions;
    private ITextDocumentFactoryService textDocuments;
    private IComponentModel componentModel;
    private IVsRunningDocumentTable runningDocuments;
    private IVsEditorAdaptersFactoryService editorAdapters;
    private IContentType nativeShaderContentType;
    private IContentType nativeHeaderContentType;
    private IContentType remoteShaderContentType;
    private IContentType remoteHeaderContentType;
    private HlslLanguageClient languageClient;
    private HashSet<string> configuredExtensions =
        new(StringComparer.OrdinalIgnoreCase);
    private bool servicesReady;

    internal static void OptionsChanged()
    {
        HlslLspPackage package;
        lock (Gate)
        {
            package = instance;
        }

        if (package != null)
        {
            package.JoinableTaskFactory.RunAsync(package.ApplyOptionsAsync)
                .FileAndForget("HlslLsp/ApplyOptions");
        }
    }

    protected override async Task InitializeAsync(
        CancellationToken cancellationToken,
        IProgress<ServiceProgressData> progress)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);

        lock (Gate)
        {
            instance = this;
        }

        var solution = await GetServiceAsync(typeof(SVsSolution)) as IVsSolution;
        if (solution == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's solution service is unavailable.");
        }
        ErrorHandler.ThrowOnFailure(solution.AdviseSolutionEvents(this, out _));

        JoinableTaskFactory.RunAsync(
                () => ActivateAndReportAsync(DisposalToken))
            .FileAndForget("HlslLsp/DeferredActivation");
    }

    private async Task ActivateAndReportAsync(CancellationToken cancellationToken)
    {
        try
        {
            await ActivateLanguageClientAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            ActivityLog.LogError(nameof(HlslLspPackage), error.ToString());
            throw;
        }
    }

    private async Task ActivateLanguageClientAsync(CancellationToken cancellationToken)
    {
        await WaitForWorkspaceReadyAsync(cancellationToken);
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);

        var resolvedComponentModel = await GetServiceAsync(typeof(SComponentModel))
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
            await GetServiceAsync(typeof(SVsRunningDocumentTable))
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
        remoteShaderContentType = GetOrCreateRemoteContentType(
            "HLSL-LSP-Colored",
            "HLSL");
        textDocuments.TextDocumentCreated += OnTextDocumentCreated;
        servicesReady = true;
        await ApplyOpenDocumentMappingsAsync(cancellationToken);

        var broker = componentModel.GetService<ILanguageClientBroker>();
        languageClient = new HlslLanguageClient(GetOptions().LanguageVersion);
        await broker.LoadAsync(new HlslLanguageClientMetadata(), languageClient);
    }

    private async Task WaitForWorkspaceReadyAsync(
        CancellationToken cancellationToken)
    {
        var folderOpened = WaitForContextAsync(KnownUIContexts.FolderOpened);
        var solutionOpened = WaitForContextAsync(KnownUIContexts.SolutionExistsContext);
        var documentOpened = WaitForContextAsync(KnownUIContexts.DocumentWindowActive);
        var activated = await Task.WhenAny(folderOpened, solutionOpened, documentOpened);
        cancellationToken.ThrowIfCancellationRequested();

        if (activated == documentOpened)
        {
            var workspaceOpened = await Task.WhenAny(
                folderOpened,
                solutionOpened,
                Task.Delay(TimeSpan.FromSeconds(15), cancellationToken));
            cancellationToken.ThrowIfCancellationRequested();
            if (workspaceOpened != folderOpened && workspaceOpened != solutionOpened)
            {
                return;
            }
            activated = workspaceOpened;
        }

        if (activated == folderOpened)
        {
            await WaitForCMakeParseAsync(cancellationToken);
        }
        else
        {
            await KnownUIContexts.SolutionExistsAndFullyLoadedContext;
        }

        await Task.Delay(TimeSpan.FromSeconds(2), cancellationToken)
            .ConfigureAwait(false);
    }

    private static async Task WaitForContextAsync(UIContext context)
    {
        await context;
    }

    private async Task WaitForCMakeParseAsync(CancellationToken cancellationToken)
    {
        var service = await GetServiceAsync(typeof(SCMakeEventNotificationService));
        if (service == null)
        {
            return;
        }

        var completion =
            new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        Action signal = () => completion.TrySetResult(true);
        var parseComplete = AddEventHandler(service, "NotifyParseComplete", signal);
        var parseError = AddEventHandler(service, "NotifyParseError", signal);
        using (cancellationToken.Register(() => completion.TrySetCanceled()))
        {
            try
            {
                var ready = await Task.WhenAny(
                    completion.Task,
                    Task.Delay(TimeSpan.FromSeconds(30), cancellationToken));
                await ready;
            }
            finally
            {
                RemoveEventHandler(service, "NotifyParseComplete", parseComplete);
                RemoveEventHandler(service, "NotifyParseError", parseError);
            }
        }
    }

    private static Delegate AddEventHandler(
        object source,
        string eventName,
        Action callback)
    {
        var eventInfo = GetEvent(source, eventName);
        var invoke = eventInfo.EventHandlerType.GetMethod("Invoke");
        var parameters = invoke.GetParameters()
            .Select(parameter => Expression.Parameter(parameter.ParameterType))
            .ToArray();
        var handler = Expression.Lambda(
                eventInfo.EventHandlerType,
                Expression.Call(Expression.Constant(callback), nameof(Action.Invoke), null),
                parameters)
            .Compile();
        eventInfo.AddEventHandler(source, handler);
        return handler;
    }

    private static void RemoveEventHandler(
        object source,
        string eventName,
        Delegate handler)
    {
        GetEvent(source, eventName).RemoveEventHandler(source, handler);
    }

    private static EventInfo GetEvent(object source, string eventName)
    {
        var contract = source.GetType()
            .GetInterfaces()
            .FirstOrDefault(
                candidate =>
                    candidate.FullName ==
                    "Microsoft.VisualStudio.CMake.Project.ICMakeEventNotificationService");
        return contract?.GetEvent(eventName)
            ?? throw new InvalidOperationException(
                $"Visual Studio's CMake event service does not expose {eventName}.");
    }

    private async Task ApplyOpenDocumentMappingsAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
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
        JoinableTaskFactory.RunAsync(
                async () =>
                {
                    await JoinableTaskFactory.SwitchToMainThreadAsync();
                    ApplyConfiguredContentType(eventArgs.TextDocument.TextBuffer);
                })
            .FileAndForget("HlslLsp/ApplyDocumentMapping");
    }

    private async Task ApplyOptionsAsync()
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync();
        if (!servicesReady)
        {
            return;
        }

        var options = GetOptions();
        ApplyFileExtensions(options.FileExtensions);
        await ApplyOpenDocumentMappingsAsync(DisposalToken);
        if (languageClient != null)
        {
            await languageClient.UpdateLanguageVersionAsync(options.LanguageVersion);
        }
    }

    private HlslOptionsPage GetOptions()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        return (HlslOptionsPage)GetDialogPage(typeof(HlslOptionsPage));
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
                    nameof(HlslLspPackage),
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
        return (value ?? string.Empty)
            .Split(new[] { ';', ',', ' ' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(extension => extension.Trim())
            .Where(extension => extension.Length > 0)
            .Select(extension => extension[0] == '.' ? extension : "." + extension)
            .Distinct(StringComparer.OrdinalIgnoreCase);
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
        DemoteOpenDocuments();
        return VSConstants.S_OK;
    }

    public int OnAfterCloseSolution(object reserved) => VSConstants.S_OK;

    public void OnAfterOpenFolder(string folderPath)
    {
    }

    public void OnBeforeCloseFolder(string folderPath)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        DemoteOpenDocuments();
    }

    public void OnQueryCloseFolder(string folderPath, ref int cancel)
    {
    }

    public void OnAfterCloseFolder(string folderPath)
    {
    }

#pragma warning disable CS0618
    public void OnAfterLoadAllDeferredProjects()
    {
    }
#pragma warning restore CS0618

    [Guid("AE781D07-40B6-4C0A-8AB3-4B75FEFC43C7")]
    private sealed class SCMakeEventNotificationService
    {
    }
}

public sealed class HlslOptionsPage : DialogPage
{
    [Category("Files")]
    [System.ComponentModel.DisplayName("HLSL file extensions")]
    [Description(
        "Semicolon-separated file extensions to treat as HLSL. " +
        "The built-in .hlsl and .hlsli extensions are always supported.")]
    public string FileExtensions { get; set; } = ".hlsl;.hlsli;.usf";

    [Category("Language")]
    [System.ComponentModel.DisplayName("Default HLSL language version")]
    [Description(
        "The default DXC -HV value, such as 2016, 2017, 2018, 2021, or 202x. " +
        "A shadertoolsconfig.json languageVersion setting takes precedence.")]
    public string LanguageVersion { get; set; } = "2021";

    protected override void OnApply(PageApplyEventArgs e)
    {
        base.OnApply(e);
        if (e.ApplyBehavior == ApplyKind.Apply)
        {
            HlslLspPackage.OptionsChanged();
        }
    }
}
