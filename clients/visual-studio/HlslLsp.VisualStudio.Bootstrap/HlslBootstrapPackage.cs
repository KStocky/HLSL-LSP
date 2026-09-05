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
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("HLSL-LSP", "DXC-powered HLSL IntelliSense", "0.8.0")]
[ProvideSettingsManifest(PackageRelativeManifestFile = "HlslLsp.registration.json")]
[ProvideMenuResource("Menus.ctmenu", 1)]
[ProvideToolWindow(typeof(MemoryLayoutToolWindow))]
[ProvideToolWindow(typeof(CompilationInfoToolWindow))]
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
            page.DxcRuntimeDirectory);
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
        string dxcRuntimeDirectory)
    {
        FileExtensions = fileExtensions;
        LanguageVersion = languageVersion;
        DxcRuntimeDirectory = dxcRuntimeDirectory;
    }

    public string FileExtensions { get; }

    public string LanguageVersion { get; }

    public string DxcRuntimeDirectory { get; }
}

[TypeDescriptionProvider(typeof(HlslOptionsTypeDescriptionProvider))]
public sealed class HlslOptionsPage : DialogPage
{
    private string fileExtensions = ".hlsl;.hlsli;.usf";
    private string languageVersion = "2021";
    private string dxcRuntimeDirectory = "";

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
