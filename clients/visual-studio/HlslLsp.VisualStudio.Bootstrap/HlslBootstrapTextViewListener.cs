using System;
using System.ComponentModel.Composition;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio.Bootstrap;

[Export(typeof(IWpfTextViewCreationListener))]
[ContentType("text")]
[TextViewRole(PredefinedTextViewRoles.Document)]
internal sealed class HlslBootstrapTextViewListener : IWpfTextViewCreationListener
{
    private static readonly Guid CMakePackageGuid =
        new("f50a0ab8-b1fa-4901-ba52-1af791aba4b1");
    private static readonly Guid BootstrapPackageGuid =
        new(HlslBootstrapPackage.PackageGuidString);
    private static int activationStarted;

    [Import(typeof(SVsServiceProvider))]
    internal IServiceProvider ServiceProvider { get; set; }

    [Import]
    internal ITextDocumentFactoryService TextDocuments { get; set; }

    public void TextViewCreated(IWpfTextView textView)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!TextDocuments.TryGetTextDocument(
                textView.TextBuffer,
                out var document))
        {
            return;
        }
        HlslBootstrapPackage.RequestActivation(document.FilePath);
        if (Interlocked.Exchange(ref activationStarted, 1) != 0)
        {
            return;
        }

        var shell = ServiceProvider.GetService(typeof(SVsShell)) as IVsShell7;
        if (shell == null)
        {
            ActivityLog.LogError(
                nameof(HlslBootstrapTextViewListener),
                "Visual Studio's shell service is unavailable.");
            Interlocked.Exchange(ref activationStarted, 0);
            return;
        }

        if (IsInCMakeWorkspace(document.FilePath))
        {
            var cmakePackageGuid = CMakePackageGuid;
            var cmakeLoad = shell.LoadPackageAsync(ref cmakePackageGuid);
            _ = Task.Run(
                () => LoadBootstrapAfterCMakeAsync(shell, cmakeLoad));
            return;
        }

        _ = LoadBootstrapAndReportAsync(shell);
    }

    private static bool IsInCMakeWorkspace(string filePath)
    {
        var directory = Path.GetDirectoryName(filePath);
        while (!string.IsNullOrEmpty(directory))
        {
            if (File.Exists(Path.Combine(directory, "CMakeLists.txt")))
            {
                return true;
            }
            directory = Path.GetDirectoryName(directory);
        }
        return false;
    }

    private static async Task LoadBootstrapAfterCMakeAsync(
        IVsShell7 shell,
        IVsTask cmakeLoad)
    {
        try
        {
            while (!cmakeLoad.IsCompleted)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(250))
                    .ConfigureAwait(false);
            }
            if (cmakeLoad.IsFaulted || cmakeLoad.IsCanceled)
            {
                ActivityLog.LogError(
                    nameof(HlslBootstrapTextViewListener),
                    "Visual Studio's CMake package did not load successfully.");
                Interlocked.Exchange(ref activationStarted, 0);
                return;
            }

            await LoadBootstrapAndReportAsync(shell);
        }
        catch (Exception error)
        {
            ActivityLog.LogError(
                nameof(HlslBootstrapTextViewListener),
                error.ToString());
            Interlocked.Exchange(ref activationStarted, 0);
        }
    }

    private static async Task LoadBootstrapAndReportAsync(IVsShell7 shell)
    {
        try
        {
            await LoadBootstrapAsync(shell);
        }
        catch (Exception error)
        {
            ActivityLog.LogError(
                nameof(HlslBootstrapTextViewListener),
                error.ToString());
            Interlocked.Exchange(ref activationStarted, 0);
        }
    }

    private static async Task LoadBootstrapAsync(IVsShell7 shell)
    {
        await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
        var bootstrapPackageGuid = BootstrapPackageGuid;
        await shell.LoadPackageAsync(ref bootstrapPackageGuid);
    }
}
