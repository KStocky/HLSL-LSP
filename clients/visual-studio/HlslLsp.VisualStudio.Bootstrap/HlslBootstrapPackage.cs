using System;
using System.Collections.Generic;
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

namespace HlslLsp.VisualStudio.Bootstrap;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("HLSL-LSP", "DXC-powered HLSL IntelliSense", "0.5.37")]
[ProvideOptionPage(typeof(HlslOptionsPage), "HLSL-LSP", "General", 0, 0, true)]
[Guid(PackageGuidString)]
public sealed class HlslBootstrapPackage : AsyncPackage
{
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
        return new HlslOptionsSnapshot(page.FileExtensions, page.LanguageVersion);
    }

    protected override Task InitializeAsync(
        CancellationToken cancellationToken,
        IProgress<ServiceProgressData> progress)
    {
        lock (Gate)
        {
            instance = this;
        }
        return TryActivateLanguageClientAsync(cancellationToken);
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
    public HlslOptionsSnapshot(string fileExtensions, string languageVersion)
    {
        FileExtensions = fileExtensions;
        LanguageVersion = languageVersion;
    }

    public string FileExtensions { get; }

    public string LanguageVersion { get; }
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
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }
}
