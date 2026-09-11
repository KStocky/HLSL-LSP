using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

// A dedicated **HLSL Entry-Point Data Flow** tool window: traces every
// function transitively reachable from the active document's configured
// entry point, plus the globals/resources those functions read or write and
// the functions/declarations unused for the active variant. Requests its
// own hlsl/entryPointDataFlow RPC through EntryPointDataFlowBridge, mirroring
// PreprocessorExplorerToolWindow's/CompilationInfoToolWindow's structure
// against a distinct protocol request. See docs/call-hierarchy.md.
[Guid("4f5bd608-c0c5-4fe7-9a1e-b1946e2fc279")]
public sealed class EntryPointDataFlowToolWindow : ToolWindowPane
{
    private readonly EntryPointDataFlowControl control = new();

    public EntryPointDataFlowToolWindow()
        : base(null)
    {
        Caption = "HLSL Entry-Point Data Flow";
        Content = control;
    }

    // Tracks the document the window currently shows so external refresh
    // triggers (active-variant selection, document save) know which document
    // to re-request without needing the caret or active-view context.
    internal Uri DocumentUri { get; private set; }

    internal void SetReport(Uri uri, EntryPointDataFlowModel report)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        DocumentUri = uri;
        control.SetReport(report);
    }

    // A failed or cancelled request must never regress this window to the
    // "open a document" placeholder or leave it stuck showing a previous
    // request's transient state: SetError keeps whatever the control
    // already renders when it has real content, and only replaces the
    // placeholder with an explicit error when there is no content yet.
    internal void SetError(Uri uri, string message, bool preserveSameDocumentContent)
    {
        var preserveContent =
            preserveSameDocumentContent &&
            DocumentUri != null &&
            DocumentUri.Equals(uri);
        DocumentUri = uri;
        control.SetError(message, preserveContent);
    }
}

internal sealed class EntryPointDataFlowControl : UserControl
{
    private readonly StackPanel content = new();
    private bool hasContent;

    internal EntryPointDataFlowControl()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        VisualStudioTheme.ApplyToolWindowTheme(this);
        Content = VisualStudioTheme.ApplyScrollViewerStyle(new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        });
        SetReport(null);
    }

    internal void SetReport(EntryPointDataFlowModel report)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        hasContent = report != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (report == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Right-click an HLSL entry point, then choose " +
                       "HLSL > Entry-Point Data Flow.",
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        AddHeader(report);
        if (!report.Found)
        {
            content.Children.Add(new TextBlock
            {
                Text = EntryPointDataFlowDisplay.NotFoundMessage(report.Explanation),
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        if (report.FunctionsVisitedTruncated)
        {
            content.Children.Add(new TextBlock
            {
                Text = EntryPointDataFlowDisplay.FunctionsVisitedSummary(
                    report.FunctionsVisited,
                    report.FunctionsVisitedTruncated),
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 8),
            });
        }

        AddSection("Reachable functions", () => AddReachableFunctions(report.ReachableFunctions));
        AddSection(
            "Global & resource accesses",
            () => AddGlobalAccesses(report.GlobalAccesses, report.GlobalAccessesTruncated));
        AddSection(
            "Unreachable functions",
            () => AddUnreachableFunctions(
                report.UnreachableFunctions,
                report.FunctionsVisitedTruncated,
                report.DefinitionsTruncated));
        AddSection(
            "Unused declarations",
            () => AddUnusedDeclarations(
                report.UnusedDeclarations,
                report.UnusedDeclarationsTruncated));
    }

    internal void SetError(string message, bool preserveContent)
    {
        if (hasContent && preserveContent)
        {
            // Keep the last successful content on screen rather than
            // replacing it with an error for a transient refresh failure.
            return;
        }
        hasContent = false;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        content.Children.Add(new TextBlock
        {
            Text = message,
            Foreground = Brushes.OrangeRed,
            TextWrapping = TextWrapping.Wrap,
        });
    }

    private void AddHeader(EntryPointDataFlowModel report)
    {
        EffectiveShaderContextDisplay.AddHeader(
            content,
            "Entry-Point Data Flow",
            report.Context);
        var entryPoint = report.EntryPoint;
        var subtitle = entryPoint != null
            ? $"Entry point: {entryPoint.Name}" +
              (string.IsNullOrEmpty(entryPoint.Detail) ? string.Empty : $" ({entryPoint.Detail})")
            : "No entry point resolved.";
        content.Children.Add(new TextBlock
        {
            Text = subtitle,
            Margin = new Thickness(0, 0, 0, 12),
            Opacity = 0.75,
            TextWrapping = TextWrapping.Wrap,
        });
    }

    private void AddSection(string title, Action addBody)
    {
        content.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 14, 0, 6),
        });
        addBody();
    }

    // --- Reachable functions ------------------------------------------

    private void AddReachableFunctions(IReadOnlyList<ReachableFunctionModel> reachable)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (reachable == null || reachable.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        var grid = new Grid();
        foreach (var width in new[] { 260d, 100d, 100d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableHeaderRow(grid, row++, new[] { "Function", "Depth", "Recursion" });
        foreach (var node in reachable)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            AddNavigableCell(grid, row, 0, FunctionLabel(node.Function), node.Function);
            AddPlainCell(grid, row, 1, EntryPointDataFlowDisplay.DepthLabel(node.Depth));
            var recursionCell = new TextBlock
            {
                Text = EntryPointDataFlowDisplay.RecursionLabel(node.Recursive),
                Foreground = node.Recursive ? Brushes.Goldenrod : Brushes.Gray,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(recursionCell, row);
            Grid.SetColumn(recursionCell, 2);
            grid.Children.Add(recursionCell);
            ++row;
        }
        content.Children.Add(grid);
    }

    // --- Global & resource accesses -------------------------------------

    private void AddGlobalAccesses(
        IReadOnlyList<GlobalAccessModel> accesses,
        bool globalAccessesTruncated)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var warning = EntryPointDataFlowDisplay.GlobalAccessesTruncationWarning(
            globalAccessesTruncated);
        if (warning != null)
        {
            content.Children.Add(new TextBlock
            {
                Text = warning,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 6),
            });
        }
        if (accesses == null || accesses.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        var grid = new Grid();
        foreach (var width in new[] { 220d, 100d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableHeaderRow(grid, row++, new[] { "Global / resource", "Access" });
        foreach (var access in accesses)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var displayName = string.IsNullOrEmpty(access.QualifiedName)
                ? access.Name
                : access.QualifiedName;
            AddNavigableCell(
                grid,
                row,
                0,
                displayName,
                access.Uri,
                EntryPointDataFlowNavigation.SelectNavigationRange(
                    access.Range,
                    access.SelectionRange));
            var accessCell = new TextBlock
            {
                Text = EntryPointDataFlowDisplay.AccessLabel(access.Access),
                Foreground = AccessBrush(access.Access),
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(accessCell, row);
            Grid.SetColumn(accessCell, 1);
            grid.Children.Add(accessCell);
            ++row;
        }
        content.Children.Add(grid);
    }

    private static Brush AccessBrush(string access)
        => access switch
        {
            "read" => Brushes.DodgerBlue,
            "write" => Brushes.OrangeRed,
            "readWrite" => Brushes.Goldenrod,
            _ => Brushes.Gray,
        };

    // --- Unreachable functions -------------------------------------------

    private void AddUnreachableFunctions(
        IReadOnlyList<CallHierarchyItemModel> unreachable,
        bool functionsVisitedTruncated,
        bool definitionsTruncated)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var warning = EntryPointDataFlowDisplay.DefinitionsTruncatedWarning(definitionsTruncated);
        if (warning != null)
        {
            content.Children.Add(new TextBlock
            {
                Text = warning,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 6),
            });
        }
        if (unreachable == null || unreachable.Count == 0)
        {
            content.Children.Add(new TextBlock
            {
                Text = EntryPointDataFlowDisplay.UnreachableFunctionsPlaceholder(
                    functionsVisitedTruncated,
                    definitionsTruncated),
                Opacity = 0.75,
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }
        foreach (var function in unreachable)
        {
            var line = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            };
            var link = new Hyperlink(new Run(FunctionLabel(function)))
            {
                ToolTip = "Go to declaration",
            };
            var navRange = EntryPointDataFlowNavigation.SelectNavigationRange(
                function.Range,
                function.SelectionRange);
            link.Click += (_, _) => NavigateTo(function.Uri, navRange);
            line.Inlines.Add(link);
            content.Children.Add(line);
        }
    }

    // --- Unused declarations ---------------------------------------------

    private void AddUnusedDeclarations(
        IReadOnlyList<UnusedDeclarationModel> unused,
        bool unusedDeclarationsTruncated)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var warning = EntryPointDataFlowDisplay.UnusedDeclarationsTruncationWarning(
            unusedDeclarationsTruncated);
        if (warning != null)
        {
            content.Children.Add(new TextBlock
            {
                Text = warning,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 6),
            });
        }
        if (unused == null || unused.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        foreach (var declaration in unused)
        {
            var line = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            };
            var link = new Hyperlink(new Run(declaration.Name ?? string.Empty))
            {
                ToolTip = "Go to declaration",
            };
            var uri = declaration.Uri;
            var range = EntryPointDataFlowNavigation.SelectNavigationRange(
                declaration.Range,
                declaration.SelectionRange);
            link.Click += (_, _) => NavigateTo(uri, range);
            line.Inlines.Add(link);
            content.Children.Add(line);
        }
    }

    private static string FunctionLabel(CallHierarchyItemModel item)
        => item == null
            ? string.Empty
            : string.IsNullOrEmpty(item.Detail)
                ? item.Name ?? string.Empty
                : item.Detail;

    // --- Shared table helpers -------------------------------------------

    private static void AddTableHeaderRow(Grid grid, int row, string[] values)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (var column = 0; column < values.Length; ++column)
        {
            var text = new TextBlock
            {
                Text = values[column],
                FontWeight = FontWeights.SemiBold,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(text, row);
            Grid.SetColumn(text, column);
            grid.Children.Add(text);
        }
    }

    private static void AddPlainCell(Grid grid, int row, int column, string text)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var block = new TextBlock
        {
            Text = text ?? string.Empty,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(5, 4, 5, 4),
        };
        Grid.SetRow(block, row);
        Grid.SetColumn(block, column);
        grid.Children.Add(block);
    }

    // Renders a clickable Hyperlink cell for a CallHierarchyItem, navigating
    // through its own compiler-reported uri/range -- never inferred from
    // the displayed name.
    private void AddNavigableCell(
        Grid grid,
        int row,
        int column,
        string label,
        CallHierarchyItemModel item)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        AddNavigableCell(
            grid,
            row,
            column,
            label,
            item?.Uri,
            EntryPointDataFlowNavigation.SelectNavigationRange(item?.Range, item?.SelectionRange));
    }

    private void AddNavigableCell(
        Grid grid,
        int row,
        int column,
        string label,
        string uri,
        CompilationSourceRangeModel range)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var block = new TextBlock
        {
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(5, 4, 5, 4),
        };
        var link = new Hyperlink(new Run(label ?? string.Empty)) { ToolTip = "Go to declaration" };
        link.Click += (_, _) => NavigateTo(uri, range);
        block.Inlines.Add(link);
        Grid.SetRow(block, row);
        Grid.SetColumn(block, column);
        grid.Children.Add(block);
    }

    // --- Navigation --------------------------------------------------

    // Opens the authoritative file/range on an explicit user click (never
    // from background save/variant refresh, which never calls this method).
    // Validates the untrusted uri/range before touching any file or VS SDK
    // API, and surfaces an explicit error dialog on any failure rather than
    // failing silently or guessing a fallback location. Mirrors
    // ResourceBindingsToolWindow.NavigateToResourceLocation and
    // PreprocessorExplorerToolWindow.NavigateToRange, generalized to the
    // CompilationSourceRangeModel/uri shape every navigable entry in this
    // response (CallHierarchyItem, unused declaration, global access) uses.
    private void NavigateTo(string uriValue, CompilationSourceRangeModel range)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!TryValidateLocation(
                uriValue,
                range,
                out var filePath,
                out var startLine,
                out var startCharacter,
                out var endLine,
                out var endCharacter,
                out var validationError))
        {
            ShowNavigationError(validationError);
            return;
        }
        try
        {
            VsShellUtilities.OpenDocument(
                ServiceProvider.GlobalProvider,
                filePath,
                VSConstants.LOGVIEWID.TextView_guid,
                out _,
                out _,
                out var frame,
                out var view);
            frame?.Show();
            view?.SetSelection(startLine, startCharacter, endLine, endCharacter);
            view?.EnsureSpanVisible(new TextSpan
            {
                iStartLine = startLine,
                iStartIndex = startCharacter,
                iEndLine = endLine,
                iEndIndex = endCharacter,
            });
        }
        catch (Exception ex)
        {
            ShowNavigationError(
                $"Unable to navigate to the entry-point data flow location: {ex.Message}");
        }
    }

    private static void ShowNavigationError(string message)
    {
        VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Entry-Point Data Flow",
            OLEMSGICON.OLEMSGICON_WARNING,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    // Validates an untrusted uri/range before any file-system or VS SDK API
    // call touches it: the uri must be an absolute local-file URI whose
    // target exists, and the range must be a well-formed, non-inverted set
    // of non-negative integer positions that fit in the Int32 range the VS
    // text-view APIs require.
    private static bool TryValidateLocation(
        string uriValue,
        CompilationSourceRangeModel range,
        out string filePath,
        out int startLine,
        out int startCharacter,
        out int endLine,
        out int endCharacter,
        out string error)
    {
        filePath = null;
        startLine = startCharacter = endLine = endCharacter = 0;
        error = null;
        if (string.IsNullOrEmpty(uriValue))
        {
            error = "The entry-point data flow location was missing.";
            return false;
        }
        Uri uri;
        try
        {
            uri = new Uri(uriValue, UriKind.Absolute);
        }
        catch (UriFormatException)
        {
            error = "The entry-point data flow location's URI could not be parsed.";
            return false;
        }
        if (!uri.IsFile)
        {
            error = "The entry-point data flow location does not refer to a local file.";
            return false;
        }
        if (range?.Start == null || range.End == null)
        {
            error = "The entry-point data flow location's range was missing.";
            return false;
        }
        if (!TryToInt32(range.Start.Line, out startLine) ||
            !TryToInt32(range.Start.Character, out startCharacter) ||
            !TryToInt32(range.End.Line, out endLine) ||
            !TryToInt32(range.End.Character, out endCharacter))
        {
            error = "The entry-point data flow location's range contained an invalid position.";
            return false;
        }
        if (endLine < startLine || (endLine == startLine && endCharacter < startCharacter))
        {
            error = "The entry-point data flow location's range was inverted.";
            return false;
        }
        filePath = uri.LocalPath;
        if (!File.Exists(filePath))
        {
            error = $"The file \"{filePath}\" could not be found.";
            return false;
        }
        return true;
    }

    private static bool TryToInt32(long value, out int result)
    {
        if (value < 0 || value > int.MaxValue)
        {
            result = 0;
            return false;
        }
        result = (int)value;
        return true;
    }
}
