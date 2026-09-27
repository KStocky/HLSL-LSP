using System;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.VisualStudio.PlatformUI;

namespace HlslLsp.VisualStudio.Bootstrap;

// The package owns the two RPC calls and the eventual edit. These dialogs only
// gather explicit choices and confirm the server-generated preview.
internal sealed class ConfigurationSelectionDialog : DialogWindow
{
    private readonly ConfigurationSelectionListModel selections;

    public ConfigurationSelectionDialog(ConfigurationAuthoringModel discovery)
    {
        selections = new ConfigurationSelectionListModel(discovery);
        Title = "Select HLSL Configuration Entries";
        Width = 760;
        Height = 500;
        MinWidth = 480;
        MinHeight = 300;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var instructions = new TextBlock
        {
            Text = "Select the entries to add. Click or press Space to toggle entries; " +
                "ambiguous profiles require an explicit choice. Unresolved entries cannot be selected.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12, 12, 12, 4),
        };
        var list = VisualStudioTheme.ApplyDialogListBoxStyle(new ListBox
        {
            Margin = new Thickness(12, 4, 12, 8),
            SelectionMode = SelectionMode.Multiple,
        });
        AutomationProperties.SetName(list, "Discovered shader entry points and target profiles");
        AutomationProperties.SetHelpText(list, instructions.Text);
        foreach (var candidate in selections.Candidates)
        {
            var item = new ListBoxItem
            {
                Content = $"{candidate.Selection.RelativePath} — {candidate.Selection.EntryPoint} " +
                    $"({candidate.Selection.TargetProfile}) — {candidate.State}: {candidate.Explanation}",
                Tag = candidate,
                IsSelected = candidate.IsSelected,
            };
            AutomationProperties.SetName(item, item.Content.ToString());
            list.Items.Add(item);
        }
        list.SelectionChanged += (_, args) =>
        {
            foreach (ListBoxItem item in args.RemovedItems)
            {
                selections.SetSelected((ConfigurationCandidateModel)item.Tag, false);
            }
            foreach (ListBoxItem item in args.AddedItems)
            {
                selections.SetSelected((ConfigurationCandidateModel)item.Tag, true);
            }
        };

        var files = discovery?.Discovery?.Files;
        var warnings = string.Join(
            Environment.NewLine,
            (files ?? Array.Empty<ConfigurationDiscoveryFileModel>())
                .Where(file => file != null && (!string.IsNullOrEmpty(file.AnalysisError) || file.Truncated))
                .Select(file => $"{file.RelativePath}: {file.AnalysisError ?? "entry point limit reached"}"));
        if (discovery?.Discovery?.Truncated == true)
        {
            warnings = $"Discovery limited: {discovery.Discovery.TruncationReason}" +
                (warnings.Length == 0 ? "" : Environment.NewLine + warnings);
        }
        var warningText = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = warnings,
            IsReadOnly = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Height = warnings.Length == 0 ? 0 : 72,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(warningText, "Discovery warnings");

        var next = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Preview",
            Width = 92,
            IsDefault = true,
            Margin = new Thickness(0, 0, 8, 0),
        });
        next.Click += (_, _) => DialogResult = true;
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
        buttons.Children.Add(next);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(instructions, 0);
        Grid.SetRow(list, 1);
        Grid.SetRow(warningText, 2);
        Grid.SetRow(buttons, 3);
        grid.Children.Add(instructions);
        grid.Children.Add(list);
        grid.Children.Add(warningText);
        grid.Children.Add(buttons);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
    }

    public System.Collections.Generic.IReadOnlyList<ConfigurationSelectionModel> SelectedSelections =>
        selections.SelectedSelections;
}

internal sealed class ConfigurationPreviewDialog : DialogWindow
{
    public ConfigurationPreviewDialog(ConfigurationAuthoringModel result)
    {
        Title = "Preview HLSL Configuration";
        Width = 820;
        Height = 650;
        MinWidth = 500;
        MinHeight = 350;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var canConfirm = ConfigurationPreviewValidation.CanConfirm(result);
        var heading = new TextBlock
        {
            Text = result?.Preview?.Valid != true
                ? "The preview has validation errors and cannot be applied."
                : canConfirm
                    ? "Review the server-generated JSON before applying the configuration."
                    : "The preview makes no changes.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12, 12, 12, 8),
        };
        var content = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = result?.Preview?.Content ?? string.Empty,
            IsReadOnly = true,
            AcceptsReturn = true,
            AcceptsTab = false,
            TextWrapping = TextWrapping.NoWrap,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(content, "Configuration JSON preview, read only");
        var errors = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = ConfigurationPreviewValidation.FormatErrors(result),
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(errors, "Configuration validation errors, read only");
        var apply = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Confirm Preview",
            Width = 130,
            IsEnabled = canConfirm,
            IsDefault = canConfirm,
            Margin = new Thickness(0, 0, 8, 0),
        });
        apply.Click += (_, _) =>
        {
            if (ConfigurationPreviewValidation.CanConfirm(result))
            {
                DialogResult = true;
            }
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
        buttons.Children.Add(apply);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(3, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(heading, 0);
        Grid.SetRow(content, 1);
        Grid.SetRow(errors, 2);
        Grid.SetRow(buttons, 3);
        grid.Children.Add(heading);
        grid.Children.Add(content);
        grid.Children.Add(errors);
        grid.Children.Add(buttons);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
    }
}
