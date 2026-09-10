using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Markup;
using Microsoft.VisualStudio.PlatformUI;
using Microsoft.VisualStudio.Shell;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class VisualStudioTheme
{
    internal static void ApplyToolWindowTheme(Control control)
    {
        control.SetResourceReference(
            Control.ForegroundProperty,
            EnvironmentColors.ToolWindowTextBrushKey);
        control.SetResourceReference(
            Control.BackgroundProperty,
            EnvironmentColors.ToolWindowBackgroundBrushKey);
        control.SetResourceReference(
            Control.FontFamilyProperty,
            VsFonts.EnvironmentFontFamilyKey);
        control.SetResourceReference(
            Control.FontSizeProperty,
            VsFonts.EnvironmentFontSizeKey);

        var hyperlinkStyle = new Style(typeof(Hyperlink));
        hyperlinkStyle.Setters.Add(
            new Setter(
                TextElement.ForegroundProperty,
                new DynamicResourceExtension(EnvironmentColors.PanelHyperlinkBrushKey)));
        var mouseOver = new Trigger
        {
            Property = Hyperlink.IsMouseOverProperty,
            Value = true,
        };
        mouseOver.Setters.Add(
            new Setter(
                TextElement.ForegroundProperty,
                new DynamicResourceExtension(EnvironmentColors.PanelHyperlinkHoverBrushKey)));
        hyperlinkStyle.Triggers.Add(mouseOver);
        control.Resources[typeof(Hyperlink)] = hyperlinkStyle;
    }

    internal static T ApplyScrollViewerStyle<T>(T scrollViewer)
        where T : ScrollViewer
    {
        scrollViewer.SetResourceReference(
            FrameworkElement.StyleProperty,
            VsResourceKeys.ScrollViewerStyleKey);
        return scrollViewer;
    }

    internal static T ApplyButtonStyle<T>(T button)
        where T : Button
    {
        button.SetResourceReference(FrameworkElement.StyleProperty, VsResourceKeys.ButtonStyleKey);
        return button;
    }

    internal static T ApplyTextBoxStyle<T>(T textBox)
        where T : TextBox
    {
        textBox.SetResourceReference(FrameworkElement.StyleProperty, VsResourceKeys.TextBoxStyleKey);
        return textBox;
    }

    internal static T ApplyDialogButtonStyle<T>(T button)
        where T : Button
    {
        button.SetResourceReference(
            FrameworkElement.StyleProperty,
            VsResourceKeys.ThemedDialogButtonStyleKey);
        return button;
    }

    internal static T ApplyDialogListBoxStyle<T>(T listBox)
        where T : ListBox
    {
        listBox.SetResourceReference(
            FrameworkElement.StyleProperty,
            VsResourceKeys.ThemedDialogListBoxStyleKey);
        return listBox;
    }
}
