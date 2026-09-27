using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.VisualStudio.PlatformUI;

namespace HlslLsp.VisualStudio.Bootstrap;

internal sealed class RuntimeCaptureCredentialsDialog : DialogWindow
{
    public RuntimeCaptureCredentialsDialog(RuntimeCaptureStartModel session)
    {
        Title = "HLSL Runtime Compilation Capture";
        Width = 720;
        Height = 340;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var instructions = new TextBlock
        {
            Text = "Capture is active. Pass this local endpoint and private token to the " +
                "opt-in capture SDK in your shader compilation wrapper, then run your project. " +
                "Do not publish the token or compiler arguments. Closing this window does " +
                "not stop the session; use Review Capture or Stop Capture in the Tools menu.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12),
        };
        var endpoint = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = session.Endpoint,
            IsReadOnly = true,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(endpoint, "Local capture endpoint");
        var token = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = session.Token,
            IsReadOnly = true,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(token, "Private capture session token");
        var close = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Close",
            Width = 90,
            IsDefault = true,
            IsCancel = true,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(12),
        });
        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(instructions, 0);
        Grid.SetRow(endpoint, 1);
        Grid.SetRow(token, 2);
        Grid.SetRow(close, 3);
        grid.Children.Add(instructions);
        grid.Children.Add(endpoint);
        grid.Children.Add(token);
        grid.Children.Add(close);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
    }
}

internal sealed class RuntimeCaptureReviewDialog : DialogWindow
{
    private readonly RuntimeCaptureSelection selection;
    private readonly List<RuntimeCapturePipelineModel> pipelines = new();
    private readonly Dictionary<string, string> variantNames =
        new(StringComparer.Ordinal);

    public RuntimeCaptureReviewDialog(RuntimeCaptureSnapshotModel snapshot)
    {
        selection = new RuntimeCaptureSelection(snapshot);
        Title = "Review Captured HLSL Compilations";
        Width = 880;
        Height = 600;
        MinWidth = 550;
        MinHeight = 400;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var guidance = new TextBlock
        {
            Text = $"Select entries to propose (accepted {snapshot.Accepted}, " +
                $"rejected {snapshot.Rejected}, overflow {snapshot.Overflow}). " +
                "Nothing is written until you review and confirm the JSON preview. " +
                "Inspect transient defines and arguments before selecting.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12, 12, 12, 6),
        };
        var list = VisualStudioTheme.ApplyDialogListBoxStyle(new ListBox
        {
            SelectionMode = SelectionMode.Multiple,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(list, "Captured shader compilation entries");
        var filter = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(filter, "Filter captured entries by source, entry point, or defines");
        var details = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(details, "Captured compiler settings and warnings");
        var next = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Preview Selected",
            Width = 130,
            IsEnabled = false,
            IsDefault = true,
            Margin = new Thickness(0, 0, 8, 0),
        });
        var addPipeline = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Group Pipeline",
            Width = 125,
            IsEnabled = false,
            Margin = new Thickness(0, 0, 8, 0),
        });
        var nameVariant = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Name Variant",
            Width = 115,
            IsEnabled = false,
            Margin = new Thickness(0, 0, 8, 0),
        });
        var rebuilding = false;
        list.SelectionChanged += (_, args) =>
        {
            if (rebuilding)
            {
                return;
            }
            foreach (ListBoxItem item in args.RemovedItems)
            {
                selection.SetSelected((RuntimeCaptureEntryModel)item.Tag, false);
            }
            foreach (ListBoxItem item in args.AddedItems)
            {
                selection.SetSelected((RuntimeCaptureEntryModel)item.Tag, true);
            }
            pipelines.RemoveAll(pipeline =>
                pipeline.Stages.Values.Any(id =>
                    !selection.Selected.Any(entry => entry.Id == id)));
            foreach (var id in variantNames.Keys
                         .Where(id => !selection.Selected.Any(entry => entry.Id == id))
                         .ToArray())
            {
                variantNames.Remove(id);
            }
            next.IsEnabled = selection.Selected.Count > 0;
            nameVariant.IsEnabled = selection.Selected.Count == 1;
            addPipeline.IsEnabled = selection.Selected.Any(entry =>
                    entry.Invocation?.TargetProfile?.StartsWith(
                        "vs_", StringComparison.OrdinalIgnoreCase) == true) &&
                selection.Selected.Any(entry =>
                    entry.Invocation?.TargetProfile?.StartsWith(
                        "ps_", StringComparison.OrdinalIgnoreCase) == true);
            var current = list.SelectedItem as ListBoxItem;
            if (current?.Tag is RuntimeCaptureEntryModel selected)
            {
                var invocation = selected.Invocation;
                details.Text =
                    $"Source: {invocation?.Source}\nPipeline: {invocation?.Pipeline}\n" +
                    $"Stage: {invocation?.Stage}\nLanguage: {invocation?.LanguageVersion}\n" +
                    $"Defines: {string.Join(", ", (invocation?.Defines ?? Array.Empty<IReadOnlyList<string>>()).Select(pair => string.Join("=", pair)))}\n" +
                    $"Arguments: {string.Join(" ", invocation?.Arguments ?? Array.Empty<string>())}\n" +
                    $"Include directories: {string.Join(", ", invocation?.IncludeDirectories ?? Array.Empty<string>())}\n" +
                    $"Warnings: {string.Join("; ", selected.Review?.Warnings ?? Array.Empty<string>())}";
            }
        };
        void Render()
        {
            rebuilding = true;
            list.Items.Clear();
            foreach (var entry in selection.Entries.Where(item => item != null))
            {
                var invocation = entry.Invocation;
                var defines = string.Join(
                    ", ",
                    invocation?.Defines?.Select(pair => string.Join("=", pair)) ??
                        Array.Empty<string>());
                var searchable =
                    $"{invocation?.Source} {invocation?.EntryPoint} {invocation?.TargetProfile} " +
                    $"{invocation?.Pipeline} {invocation?.Stage} {defines}";
                if (searchable.IndexOf(
                        filter.Text, StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }
                var label = $"{invocation?.Source} — {invocation?.EntryPoint} " +
                    $"({invocation?.TargetProfile}), {entry.Count} occurrence(s)" +
                    (selection.CanSelect(entry) ? "" : " — manual file mapping required");
                var item = new ListBoxItem
                {
                    Content = label,
                    Tag = entry,
                    IsEnabled = selection.CanSelect(entry),
                    IsSelected = selection.Selected.Contains(entry),
                };
                AutomationProperties.SetName(item, label);
                list.Items.Add(item);
            }
            rebuilding = false;
            next.IsEnabled = selection.Selected.Count > 0;
            nameVariant.IsEnabled = selection.Selected.Count == 1;
            addPipeline.IsEnabled = selection.Selected.Any(entry =>
                    entry.Invocation?.TargetProfile?.StartsWith(
                        "vs_", StringComparison.OrdinalIgnoreCase) == true) &&
                selection.Selected.Any(entry =>
                    entry.Invocation?.TargetProfile?.StartsWith(
                        "ps_", StringComparison.OrdinalIgnoreCase) == true);
        }
        filter.TextChanged += (_, _) => Render();
        Render();
        nameVariant.Click += (_, _) =>
        {
            var entry = selection.Selected.Single();
            var dialog = new RuntimeCaptureVariantDialog(entry);
            if (dialog.ShowModal() == true)
            {
                variantNames[entry.Id] = dialog.VariantName;
                details.Text = $"Variant \"{dialog.VariantName}\" named explicitly. " +
                    "The server will validate its settings in the final preview.";
            }
        };
        addPipeline.Click += (_, _) =>
        {
            var dialog = new RuntimeCapturePipelineDialog(selection.Selected);
            if (dialog.ShowModal() != true)
            {
                return;
            }
            pipelines.RemoveAll(pipeline => string.Equals(
                pipeline.Name, dialog.Pipeline.Name, StringComparison.Ordinal));
            pipelines.Add(dialog.Pipeline);
            details.Text = $"Pipeline \"{dialog.Pipeline.Name}\" grouped explicitly. " +
                "The server will validate every selected stage in the final preview.";
        };
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
        buttons.Children.Add(nameVariant);
        buttons.Children.Add(addPipeline);
        buttons.Children.Add(next);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(3, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(guidance, 0);
        Grid.SetRow(filter, 1);
        Grid.SetRow(list, 2);
        Grid.SetRow(details, 3);
        Grid.SetRow(buttons, 4);
        grid.Children.Add(guidance);
        grid.Children.Add(filter);
        grid.Children.Add(list);
        grid.Children.Add(details);
        grid.Children.Add(buttons);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
    }

    public IReadOnlyList<RuntimeCaptureEntryModel> SelectedEntries => selection.Selected;

    public IReadOnlyList<RuntimeCapturePipelineModel> Pipelines => pipelines;

    public IReadOnlyDictionary<string, string> VariantNames => variantNames;
}
