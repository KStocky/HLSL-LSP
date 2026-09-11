using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.VisualStudio.Shell;

namespace HlslLsp.VisualStudio.Bootstrap;

[Guid("dd38da7e-5392-4ef4-8498-a4d77e9d7639")]
public sealed class MacroExpansionToolWindow : ToolWindowPane, IAnalysisFreshnessView
{
    private readonly StackPanel content = new();

    public MacroExpansionToolWindow()
        : base(null)
    {
        Caption = "HLSL Macro Expansion";
        var scrollViewer = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        Content = VisualStudioTheme.ApplyScrollViewerStyle(scrollViewer);
        VisualStudioTheme.ApplyToolWindowTheme(scrollViewer);
        SetStatus("Right-click an HLSL macro invocation, then choose HLSL > Expand Macro.");
    }

    internal void SetExpansion(MacroExpansionModel expansion)
    {
        content.Children.Clear();
        content.Margin = new Thickness(12);
        Caption = $"Macro Expansion: {expansion.Name}";
        EffectiveShaderContextDisplay.AddHeader(content, expansion.Name, expansion.Context);
        AddSection("Invocation", expansion.Invocation);
        AddSection("Expanded result", expansion.Expansion);
    }

    internal void SetStatus(string message)
    {
        content.Children.Clear();
        content.Margin = new Thickness(12);
        Caption = "HLSL Macro Expansion";
        content.Children.Add(new TextBlock
        {
            Text = message,
            TextWrapping = TextWrapping.Wrap,
        });
    }

    void IAnalysisFreshnessView.BeginRefresh(AnalysisFreshnessCause cause)
        => SetStatus("Resolving compiler expansion...");

    void IAnalysisFreshnessView.MarkStale(
        AnalysisFreshnessCause cause,
        bool refreshPending)
        => SetStatus(
            "The displayed macro expansion is stale (" +
            AnalysisFreshnessReducer.Text(
                new AnalysisFreshnessState(AnalysisFreshnessStatus.Stale, cause)) +
            "). Run Expand Macro again.");

    private void AddSection(string heading, string value)
    {
        content.Children.Add(new TextBlock
        {
            Text = heading,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 8, 0, 4),
        });
        content.Children.Add(VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = value ?? string.Empty,
            IsReadOnly = true,
            TextWrapping = TextWrapping.NoWrap,
            AcceptsReturn = true,
            FontFamily = new FontFamily("Consolas"),
            Padding = new Thickness(8),
        }));
    }
}
