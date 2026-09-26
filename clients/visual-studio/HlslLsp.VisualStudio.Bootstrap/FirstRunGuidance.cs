using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Microsoft.VisualStudio.Imaging;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class FirstRunGuidancePolicy
{
    internal const string GuidanceMarker = "visual-studio-first-run-guidance-shown";
    internal const string ConflictMarker = "visual-studio-hlsl-tools-conflict-warning-shown";
    internal const string HlslToolsAssembly = "ShaderTools.VisualStudio.LanguageServices.Hlsl";

    internal static bool HasHlslToolsConflict(IEnumerable<string> assemblyNames)
        => assemblyNames.Any(
            name => string.Equals(
                name,
                HlslToolsAssembly,
                StringComparison.OrdinalIgnoreCase));
}

internal sealed class FirstRunMarkerStore
{
    private readonly string directory;

    internal FirstRunMarkerStore(string directory)
    {
        this.directory = directory;
    }

    internal static FirstRunMarkerStore CreateDefault()
        => new(
            Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "HLSL-LSP"));

    internal bool TryClaim(string marker, out string errorMessage)
    {
        var path = Path.Combine(directory, marker);
        try
        {
            Directory.CreateDirectory(directory);
            using var stream = new FileStream(
                path,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.Read);
            using var writer = new StreamWriter(stream);
            writer.WriteLine(DateTimeOffset.UtcNow.ToString("O"));
            errorMessage = null;
            return true;
        }
        catch (IOException error)
        {
            errorMessage = File.Exists(path) ? null : error.Message;
            return false;
        }
        catch (UnauthorizedAccessException error)
        {
            errorMessage = error.Message;
            return false;
        }
    }
}

internal sealed class HlslInfoBarEvents : IVsInfoBarUIEvents
{
    private readonly Action<string> action;
    private uint cookie;

    internal HlslInfoBarEvents(Action<string> action)
    {
        this.action = action;
    }

    internal void Connect(IVsInfoBarUIElement element)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        element.Advise(this, out cookie);
    }

    public void OnActionItemClicked(
        IVsInfoBarUIElement infoBarUIElement,
        IVsInfoBarActionItem actionItem)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (actionItem.ActionContext is string actionName)
        {
            action(actionName);
        }
    }

    public void OnClosed(IVsInfoBarUIElement infoBarUIElement)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (cookie != 0)
        {
            infoBarUIElement.Unadvise(cookie);
            cookie = 0;
        }
    }
}

internal static class FirstRunInfoBar
{
    internal const string ConfigurationAction = "configuration";
    internal const string VariantAction = "variant";
    internal const string DiagnosticsAction = "diagnostics";
    internal const string ConflictDetailsAction = "conflict";

    internal static IVsInfoBarUIElement CreateGuidance(
        IVsInfoBarUIFactory factory,
        Action<string> action)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var model = new InfoBarModel(
            "HLSL-LSP is active. Right-click the editor for the HLSL menu.",
            new IVsInfoBarActionItem[]
            {
                new InfoBarHyperlink("Configuration guide", ConfigurationAction),
                new InfoBarHyperlink("Select variant", VariantAction),
                new InfoBarHyperlink("Setup diagnostics", DiagnosticsAction),
            },
            KnownMonikers.StatusInformation,
            true);
        var element = factory.CreateInfoBar(model);
        new HlslInfoBarEvents(action).Connect(element);
        return element;
    }

    internal static IVsInfoBarUIElement CreateConflictWarning(
        IVsInfoBarUIFactory factory,
        Action<string> action)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var model = new InfoBarModel(
            "HLSL Tools is also active and can claim the same editor features. " +
            "Disable or uninstall HLSL Tools, then restart Visual Studio.",
            new IVsInfoBarActionItem[]
            {
                new InfoBarHyperlink("HLSL Tools details", ConflictDetailsAction),
                new InfoBarHyperlink("Setup diagnostics", DiagnosticsAction),
            },
            KnownMonikers.StatusWarning,
            true);
        var element = factory.CreateInfoBar(model);
        new HlslInfoBarEvents(action).Connect(element);
        return element;
    }
}
