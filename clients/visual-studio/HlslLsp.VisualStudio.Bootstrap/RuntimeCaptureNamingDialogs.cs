using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using Microsoft.VisualStudio.PlatformUI;

namespace HlslLsp.VisualStudio.Bootstrap;

internal sealed class RuntimeCaptureVariantDialog : DialogWindow
{
    public RuntimeCaptureVariantDialog(RuntimeCaptureEntryModel entry)
    {
        Title = "Name Captured Shader Variant";
        Width = 620;
        Height = 230;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var guidance = new TextBlock
        {
            Text = $"Name the variant for {entry.Invocation?.Source} / " +
                $"{entry.Invocation?.EntryPoint} ({entry.Invocation?.TargetProfile}). " +
                "Distinct permutations of this file require distinct variant names.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12),
        };
        var name = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Margin = new Thickness(12, 0, 12, 8),
            MaxLength = 128,
        });
        AutomationProperties.SetName(name, "Shader variant name");
        var ok = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Use Name",
            Width = 100,
            IsDefault = true,
            IsEnabled = false,
            Margin = new Thickness(0, 0, 8, 0),
        });
        name.TextChanged += (_, _) => ok.IsEnabled = !string.IsNullOrWhiteSpace(name.Text);
        ok.Click += (_, _) =>
        {
            VariantName = name.Text.Trim();
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
            Margin = new Thickness(12),
        };
        buttons.Children.Add(ok);
        buttons.Children.Add(cancel);
        var panel = new StackPanel();
        panel.Children.Add(guidance);
        panel.Children.Add(name);
        panel.Children.Add(buttons);
        var container = new ContentControl { Content = panel };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
        name.Focus();
    }

    public string VariantName { get; private set; }
}

internal sealed class RuntimeCapturePipelineDialog : DialogWindow
{
    private readonly TextBox name;
    private readonly Dictionary<string, ListBox> stages = new(StringComparer.Ordinal);
    private readonly IReadOnlyList<RuntimeCaptureEntryModel> entries;

    public RuntimeCapturePipelineDialog(IReadOnlyList<RuntimeCaptureEntryModel> selected)
    {
        entries = selected ?? throw new ArgumentNullException(nameof(selected));
        Title = "Group Captured Graphics Pipeline";
        Width = 720;
        Height = 760;
        MinHeight = 510;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var guidance = new TextBlock
        {
            Text = "Explicitly name a pipeline and select its vertex and pixel stages. " +
                "Geometry, hull, and domain stages are optional; hull and domain must " +
                "be paired. Choose stages only when the captured invocations belong " +
                "together. The final JSON must be reviewed and confirmed.",
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(12, 12, 12, 8),
        };
        name = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            MaxLength = 128,
            Margin = new Thickness(12, 0, 12, 8),
        });
        AutomationProperties.SetName(name, "Graphics pipeline name");
        var stagePanel = new StackPanel { Margin = new Thickness(12, 0, 12, 8) };
        AddStage(stagePanel, "vertex", "vs_", false);
        AddStage(stagePanel, "pixel", "ps_", false);
        AddStage(stagePanel, "geometry", "gs_", true);
        AddStage(stagePanel, "hull", "hs_", true);
        AddStage(stagePanel, "domain", "ds_", true);

        var ok = VisualStudioTheme.ApplyDialogButtonStyle(new Button
        {
            Content = "Add Pipeline",
            Width = 110,
            IsDefault = true,
            IsEnabled = false,
            Margin = new Thickness(0, 0, 8, 0),
        });
        void Update()
        {
            var hull = Selected("hull") != null;
            var domain = Selected("domain") != null;
            ok.IsEnabled = !string.IsNullOrWhiteSpace(name.Text) &&
                Selected("vertex") != null && Selected("pixel") != null &&
                hull == domain;
        }
        name.TextChanged += (_, _) => Update();
        foreach (var list in stages.Values)
        {
            list.SelectionChanged += (_, _) => Update();
        }
        ok.Click += (_, _) =>
        {
            var chosen = stages
                .Select(stage => (stage: stage.Key, entry: Selected(stage.Key)))
                .Where(stage => stage.entry != null)
                .ToDictionary(stage => stage.stage, stage => stage.entry.Id);
            var pipelineName = name.Text.Trim();
            var source = stages
                .Where(stage => Selected(stage.Key) != null)
                .All(stage =>
                    string.Equals(
                        Selected(stage.Key).Invocation?.Pipeline,
                        pipelineName, StringComparison.Ordinal) &&
                    string.Equals(
                        Selected(stage.Key).Invocation?.Stage,
                        stage.Key, StringComparison.Ordinal))
                    ? "host" : "user";
            Pipeline = new RuntimeCapturePipelineModel
            {
                Name = pipelineName,
                Source = source,
                Stages = chosen,
            };
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
        buttons.Children.Add(ok);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var scroll = VisualStudioTheme.ApplyScrollViewerStyle(new ScrollViewer
        {
            Content = stagePanel,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        });
        Grid.SetRow(guidance, 0);
        Grid.SetRow(name, 1);
        Grid.SetRow(scroll, 2);
        Grid.SetRow(buttons, 3);
        grid.Children.Add(guidance);
        grid.Children.Add(name);
        grid.Children.Add(scroll);
        grid.Children.Add(buttons);
        var container = new ContentControl { Content = grid };
        VisualStudioTheme.ApplyToolWindowTheme(container);
        Content = container;
        name.Focus();
    }

    public RuntimeCapturePipelineModel Pipeline { get; private set; }

    private void AddStage(StackPanel panel, string stage, string profile, bool optional)
    {
        panel.Children.Add(new TextBlock
        {
            Text = stage,
            Margin = new Thickness(0, 5, 0, 3),
        });
        var list = VisualStudioTheme.ApplyDialogListBoxStyle(new ListBox
        {
            Height = 75,
            SelectionMode = SelectionMode.Single,
        });
        AutomationProperties.SetName(list, $"Captured {stage} stage");
        if (optional)
        {
            list.Items.Add(new ListBoxItem { Content = "(None)", Tag = null });
            list.SelectedIndex = 0;
        }
        foreach (var entry in entries.Where(entry =>
                     entry.Invocation?.TargetProfile?.StartsWith(
                         profile, StringComparison.OrdinalIgnoreCase) == true))
        {
            list.Items.Add(new ListBoxItem
            {
                Content = $"{entry.Invocation.Source} / {entry.Invocation.EntryPoint}",
                Tag = entry,
            });
        }
        stages.Add(stage, list);
        panel.Children.Add(list);
    }

    private RuntimeCaptureEntryModel Selected(string stage)
        => (stages[stage].SelectedItem as ListBoxItem)?.Tag as RuntimeCaptureEntryModel;
}
