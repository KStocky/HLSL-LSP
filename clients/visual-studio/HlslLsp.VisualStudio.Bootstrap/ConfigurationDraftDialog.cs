using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.VisualStudio.PlatformUI;

namespace HlslLsp.VisualStudio.Bootstrap;

internal sealed class ConfigurationDraftDialog : DialogWindow
{
    private readonly TextBox content;

    public ConfigurationDraftDialog(ConfigurationAuthoringModel result)
    {
        Title = "Edit HLSL Configuration Draft";
        Width = 880;
        Height = 700;
        MinWidth = 550;
        MinHeight = 400;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var instructions = new TextBlock
        {
            Text = "Edit the proposed JSON to configure include paths, virtual mappings, " +
                "defines, arguments, runtime, variants, and pipelines. Validate the draft " +
                "before reviewing and applying it. Existing comments or formatting may " +
                "change when generating the initial preview.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12, 12, 12, 8),
        };
        content = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = result?.Preview?.Content ?? string.Empty,
            AcceptsReturn = true,
            AcceptsTab = true,
            TextWrapping = TextWrapping.NoWrap,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(content, "Editable shadertoolsconfig.json draft");
        var errors = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = ConfigurationPreviewValidation.FormatErrors(result),
            IsReadOnly = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Height = result?.Preview?.Valid == false ? 92 : 0,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(errors, "Configuration validation errors");

        var validate = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Validate Draft",
            Width = 120,
            IsDefault = true,
            Margin = new Thickness(0, 0, 8, 0),
        });
        validate.Click += (_, _) =>
        {
            DraftContent = content.Text;
            DialogResult = true;
        };
        var cancel = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Cancel",
            Width = 84,
            IsCancel = true,
        });
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(12, 0, 12, 12),
        };
        buttons.Children.Add(validate);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(instructions, 0);
        Grid.SetRow(content, 1);
        Grid.SetRow(errors, 2);
        Grid.SetRow(buttons, 3);
        grid.Children.Add(instructions);
        grid.Children.Add(content);
        grid.Children.Add(errors);
        grid.Children.Add(buttons);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
        content.Focus();
    }

    public string DraftContent { get; private set; }
}
