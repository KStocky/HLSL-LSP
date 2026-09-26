using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;

namespace HlslLsp.VisualStudio.Bootstrap;

internal enum AnalysisSummaryKind
{
    Success,
    Attention,
    Unavailable,
    Information,
}

internal sealed class AnalysisViewState
{
    private readonly Dictionary<string, bool> sections =
        new(StringComparer.Ordinal);

    internal string Filter { get; set; } = string.Empty;

    internal bool IsExpanded(string section, bool defaultValue)
        => sections.TryGetValue(section, out var expanded)
            ? expanded
            : defaultValue;

    internal void SetExpanded(string section, bool expanded)
        => sections[section] = expanded;

    internal bool Matches(params string[] values)
    {
        var filter = Filter?.Trim();
        if (string.IsNullOrEmpty(filter))
        {
            return true;
        }
        foreach (var value in values)
        {
            if (!string.IsNullOrEmpty(value) &&
                value.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return true;
            }
        }
        return false;
    }
}

internal static class AnalysisViewPresentation
{
    internal static void AddSummary(
        Panel target,
        AnalysisSummaryKind kind,
        string title,
        string detail = null)
    {
        var label = kind switch
        {
            AnalysisSummaryKind.Success => "Success",
            AnalysisSummaryKind.Attention => "Attention",
            AnalysisSummaryKind.Unavailable => "Unavailable",
            _ => "Summary",
        };
        var panel = new StackPanel
        {
            Margin = new Thickness(0, 0, 0, 10),
        };
        AutomationProperties.SetName(panel, $"{label}: {title}");
        panel.Children.Add(new TextBlock
        {
            Text = $"{label}: {title}",
            FontWeight = FontWeights.SemiBold,
            TextWrapping = TextWrapping.Wrap,
        });
        if (!string.IsNullOrWhiteSpace(detail))
        {
            panel.Children.Add(new TextBlock
            {
                Text = detail,
                Margin = new Thickness(0, 2, 0, 0),
                Opacity = 0.75,
                TextWrapping = TextWrapping.Wrap,
            });
        }
        target.Children.Add(panel);
    }

    internal static void AddSection(
        StackPanel target,
        AnalysisViewState state,
        string title,
        bool initiallyExpanded,
        Action addBody)
    {
        var insertionIndex = target.Children.Count;
        addBody();
        var body = new StackPanel();
        while (target.Children.Count > insertionIndex)
        {
            var child = target.Children[insertionIndex];
            target.Children.RemoveAt(insertionIndex);
            body.Children.Add(child);
        }
        var section = new Expander
        {
            Header = title,
            Content = body,
            IsExpanded = state.IsExpanded(title, initiallyExpanded),
            Margin = new Thickness(0, 8, 0, 0),
        };
        AutomationProperties.SetName(section, title);
        section.Expanded += (_, _) => state.SetExpanded(title, true);
        section.Collapsed += (_, _) => state.SetExpanded(title, false);
        target.Children.Insert(insertionIndex, section);
    }

    internal static TextBox AddFilter(
        Panel target,
        AnalysisViewState state,
        string accessibleName,
        Action refresh)
    {
        var filter = VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = state.Filter,
            MinWidth = 220,
            Margin = new Thickness(0, 2, 0, 8),
            ToolTip = "Type to filter this collection.",
        });
        AutomationProperties.SetName(filter, accessibleName);
        filter.TextChanged += (_, _) => state.Filter = filter.Text;
        filter.KeyDown += (_, eventArgs) =>
        {
            if (eventArgs.Key == Key.Enter)
            {
                refresh();
                eventArgs.Handled = true;
            }
        };
        var apply = VisualStudioTheme.ApplyButtonStyle(new Button
        {
            Content = "Apply filter",
            Margin = new Thickness(6, 2, 0, 8),
            Padding = new Thickness(8, 2, 8, 2),
        });
        AutomationProperties.SetName(apply, $"Apply {accessibleName}");
        apply.Click += (_, _) => refresh();
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        row.Children.Add(new TextBlock
        {
            Text = "Filter",
            Margin = new Thickness(0, 6, 6, 0),
            VerticalAlignment = VerticalAlignment.Top,
        });
        row.Children.Add(filter);
        row.Children.Add(apply);
        target.Children.Add(row);
        return filter;
    }

    internal static double CaptureScrollOffset(ScrollViewer scrollViewer)
        => scrollViewer?.VerticalOffset ?? 0;

    internal static void RestoreScrollOffset(
        ScrollViewer scrollViewer,
        double offset)
    {
        if (scrollViewer == null)
        {
            return;
        }
        scrollViewer.ScrollToVerticalOffset(offset);
    }
}
