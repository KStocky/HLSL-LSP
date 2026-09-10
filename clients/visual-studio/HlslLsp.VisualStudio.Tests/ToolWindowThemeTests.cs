using System;
using System.IO;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class ToolWindowThemeTests
{
    [Fact]
    public void EveryCustomToolWindowControl_AppliesTheSharedVisualStudioTheme()
    {
        var bootstrapDirectory = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap"));
        var toolWindowFiles = Directory.GetFiles(
            bootstrapDirectory,
            "*ToolWindow.cs",
            SearchOption.TopDirectoryOnly);

        foreach (var file in toolWindowFiles)
        {
            var source = File.ReadAllText(file);
            if (!source.Contains(": UserControl"))
            {
                continue;
            }
            Assert.Contains("VisualStudioTheme.ApplyToolWindowTheme(this);", source);
            Assert.Contains("VisualStudioTheme.ApplyScrollViewerStyle(", source);
        }
    }

    [Fact]
    public void SharedTheme_UsesDynamicVisualStudioToolWindowResources()
    {
        var themePath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\VisualStudioTheme.cs"));
        var source = File.ReadAllText(themePath);

        Assert.Contains("SetResourceReference(", source);
        Assert.Contains("EnvironmentColors.ToolWindowTextBrushKey", source);
        Assert.Contains("EnvironmentColors.ToolWindowBackgroundBrushKey", source);
        Assert.Contains("EnvironmentColors.PanelHyperlinkBrushKey", source);
        Assert.Contains("VsResourceKeys.ScrollViewerStyleKey", source);
        Assert.DoesNotContain("Brushes.Black", source);
        Assert.DoesNotContain("Brushes.White", source);
    }

    [Fact]
    public void VariantPicker_UsesVisualStudioDialogControlStyles()
    {
        var dialogPath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\VariantSelectionDialog.cs"));
        var source = File.ReadAllText(dialogPath);

        Assert.Contains("VisualStudioTheme.ApplyDialogListBoxStyle(", source);
        Assert.Equal(2, Count(source, "VisualStudioTheme.ApplyDialogButtonStyle("));
    }

    [Fact]
    public void ComputeVisualization_DefaultTextDoesNotSetANullForeground()
    {
        var controlPath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\ComputeVisualizationToolWindow.cs"));
        var source = File.ReadAllText(controlPath);

        Assert.DoesNotContain("Foreground = foreground,", source);
        Assert.Contains("if (foreground != null)", source);
    }

    private static int Count(string text, string value)
    {
        var count = 0;
        var index = 0;
        while ((index = text.IndexOf(value, index, StringComparison.Ordinal)) >= 0)
        {
            ++count;
            index += value.Length;
        }
        return count;
    }

}
