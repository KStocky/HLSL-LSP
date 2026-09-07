using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.TextManager.Interop;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio.Bootstrap;

// A dedicated **HLSL Preprocessor Explorer** tool window: the resolved
// include graph (per analyzed file), preprocessor-skipped regions, active
// macros, and effective configuration settings for the current (possibly
// unsaved) document. Requests its own hlsl/preprocessorExplorer RPC through
// PreprocessorExplorerBridge, mirroring CompilationInfoToolWindow's
// structure but against a distinct protocol request.
[Guid("7f8456a3-6b1d-4ed3-b535-a16b01a6c6d5")]
public sealed class PreprocessorExplorerToolWindow : ToolWindowPane
{
    private readonly PreprocessorExplorerControl control = new();

    public PreprocessorExplorerToolWindow()
        : base(null)
    {
        Caption = "HLSL Preprocessor Explorer";
        Content = control;
    }

    // Tracks the document the window currently shows so external refresh
    // triggers (active-variant selection, document save) know which document
    // to re-request without needing the caret or active-view context.
    internal Uri DocumentUri { get; private set; }

    internal void SetReport(Uri uri, PreprocessorExplorerModel report)
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

internal sealed class PreprocessorExplorerControl : UserControl
{
    private readonly StackPanel content = new();
    private bool hasContent;

    internal PreprocessorExplorerControl()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        SetReport(null);
    }

    internal void SetReport(PreprocessorExplorerModel report)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        hasContent = report != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (report == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Open an HLSL document, then run " +
                       "Tools > HLSL Preprocessor Explorer.",
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        AddHeader(report);
        foreach (var diagnostic in report.Diagnostics ?? Array.Empty<string>())
        {
            content.Children.Add(new TextBlock
            {
                Text = diagnostic,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            });
        }
        AddSection("Files", () => AddFiles(report.Files));
        AddSection(
            "Preprocessor-skipped regions",
            () => AddSkippedRegions(report.SkippedRegions));
        AddSection("Macros", () => AddMacros(report.Macros));
        AddSection("Effective settings", () => AddSettings(report.Settings));
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

    private void AddHeader(PreprocessorExplorerModel report)
    {
        content.Children.Add(new TextBlock
        {
            Text = "Preprocessor Explorer",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        content.Children.Add(new TextBlock
        {
            Text = report.RootUri,
            Margin = new Thickness(0, 3, 0, 12),
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

    // --- Files / includes -------------------------------------------------

    private void AddFiles(IReadOnlyList<PreprocessorFileModel> files)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (files == null || files.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        foreach (var file in files)
        {
            var heading = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 8, 0, 2),
            };
            var displayName = string.IsNullOrEmpty(file.LogicalPath)
                ? file.PhysicalPath
                : file.LogicalPath;
            var link = new Hyperlink(new Run(displayName ?? string.Empty))
            {
                ToolTip = "Go to file",
            };
            var uri = file.Uri;
            link.Click += (_, _) => NavigateToPoint(uri, 0, 0);
            heading.Inlines.Add(link);
            heading.Inlines.Add(new Run(
                file.Source == "open" ? "  (open in editor)" : "  (on disk)")
            {
                Foreground = Brushes.Gray,
            });
            content.Children.Add(heading);
            content.Children.Add(new TextBlock
            {
                Text = file.PhysicalPath,
                Opacity = 0.6,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            });
            AddIncludeTable(file);
        }
    }

    private void AddIncludeTable(PreprocessorFileModel file)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var includes = file.Includes ?? Array.Empty<PreprocessorIncludeModel>();
        if (includes.Count == 0)
        {
            content.Children.Add(new TextBlock
            {
                Text = "(no #include directives)",
                Opacity = 0.6,
                Margin = new Thickness(12, 0, 0, 8),
            });
            return;
        }
        var grid = new Grid { Margin = new Thickness(12, 0, 0, 8) };
        foreach (var width in new[] { 220d, 100d, 90d, 260d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableHeaderRow(grid, row++, new[] { "Directive", "Kind", "Status", "Target" });
        foreach (var include in includes)
        {
            var presentation = PreprocessorIncludePresentation.Create(include);
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var directiveCell = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(5, 4, 5, 4),
            };
            var directiveLink = new Hyperlink(new Run(DirectiveLabel(include)))
            {
                ToolTip = "Go to directive",
            };
            var fileUri = file.Uri;
            var line = include.Line;
            var character = include.Character;
            directiveLink.Click += (_, _) => NavigateToPoint(fileUri, line, character);
            directiveCell.Inlines.Add(directiveLink);
            if (!string.IsNullOrEmpty(presentation.ExpandedPath))
            {
                directiveCell.Inlines.Add(new Run($" (expands to {presentation.ExpandedPath})")
                {
                    Foreground = Brushes.Gray,
                });
            }
            Grid.SetRow(directiveCell, row);
            Grid.SetColumn(directiveCell, 0);
            grid.Children.Add(directiveCell);

            AddPlainCell(grid, row, 1, KindLabel(include.Kind));

            var statusCell = new TextBlock
            {
                Text = StatusLabel(include.Status),
                Foreground = StatusBrush(include.Status),
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(statusCell, row);
            Grid.SetColumn(statusCell, 2);
            grid.Children.Add(statusCell);

            var targetCell = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(5, 4, 5, 4),
            };
            if (!string.IsNullOrEmpty(presentation.ResolvedUri))
            {
                var targetLink = new Hyperlink(new Run(presentation.TargetLabel))
                {
                    ToolTip = "Go to resolved file",
                };
                var resolvedUri = presentation.ResolvedUri;
                targetLink.Click += (_, _) => NavigateToPoint(resolvedUri, 0, 0);
                targetCell.Inlines.Add(targetLink);
                if (!string.IsNullOrEmpty(presentation.Mapping))
                {
                    targetCell.Inlines.Add(new Run($" (via {presentation.Mapping} mapping)")
                    {
                        Foreground = Brushes.Gray,
                    });
                }
            }
            else
            {
                targetCell.Inlines.Add(new Run(presentation.TargetLabel));
            }
            if (!string.IsNullOrEmpty(presentation.ConfigurationOrigin))
            {
                targetCell.Inlines.Add(new Run(" (configured by ")
                {
                    Foreground = Brushes.Gray,
                });
                if (!string.IsNullOrEmpty(presentation.ConfigurationOriginUri))
                {
                    var originLink = new Hyperlink(new Run(presentation.ConfigurationOrigin))
                    {
                        ToolTip = "Go to configuration",
                    };
                    var originUri = presentation.ConfigurationOriginUri;
                    originLink.Click += (_, _) => NavigateToPoint(originUri, 0, 0);
                    targetCell.Inlines.Add(originLink);
                }
                else
                {
                    targetCell.Inlines.Add(new Run(presentation.ConfigurationOrigin));
                }
                targetCell.Inlines.Add(new Run(")")
                {
                    Foreground = Brushes.Gray,
                });
            }
            Grid.SetRow(targetCell, row);
            Grid.SetColumn(targetCell, 3);
            grid.Children.Add(targetCell);

            ++row;
        }
        content.Children.Add(grid);
    }

    private static string DirectiveLabel(PreprocessorIncludeModel include)
        => include.Kind switch
        {
            "quoted" => $"\"{include.Path}\"",
            "angled" => $"<{include.Path}>",
            _ => include.Path,
        };

    private static string KindLabel(string kind)
        => kind switch
        {
            "quoted" => "quoted",
            "angled" => "angled",
            "macro" => "macro-expanded",
            _ => kind,
        };

    private static string StatusLabel(string status)
        => status switch
        {
            "resolved" => "Resolved",
            "missing" => "Missing",
            "cyclic" => "Cyclic",
            "dynamic" => "Dynamic",
            _ => status,
        };

    private static Brush StatusBrush(string status)
        => status switch
        {
            "resolved" => Brushes.LimeGreen,
            "missing" => Brushes.OrangeRed,
            "cyclic" => Brushes.Goldenrod,
            _ => Brushes.Gray,
        };

    // --- Preprocessor-skipped regions --------------------------------------

    private void AddSkippedRegions(IReadOnlyList<PreprocessorSkippedRegionModel> regions)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (regions == null || regions.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        foreach (var region in regions)
        {
            var line = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            };
            var start = region.Start;
            var end = region.End;
            var label =
                $"{(start?.Line ?? 0) + 1}:{(start?.Character ?? 0) + 1}" +
                $" \u2013 {(end?.Line ?? 0) + 1}:{(end?.Character ?? 0) + 1}";
            var link = new Hyperlink(new Run(label)) { ToolTip = "Go to region" };
            var uri = region.Uri;
            var startLine = start?.Line ?? 0;
            var startCharacter = start?.Character ?? 0;
            var endLine = end?.Line ?? 0;
            var endCharacter = end?.Character ?? 0;
            link.Click += (_, _) => NavigateToRange(
                uri, startLine, startCharacter, endLine, endCharacter);
            line.Inlines.Add(link);
            line.Inlines.Add(new Run($"  {region.Uri}") { Foreground = Brushes.Gray });
            content.Children.Add(line);
        }
    }

    // --- Macros -------------------------------------------------------

    private void AddMacros(IReadOnlyList<PreprocessorMacroModel> macros)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (macros == null || macros.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        var grid = new Grid();
        foreach (var width in new[] { 180d, 220d, 100d, 220d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableHeaderRow(grid, row++, new[] { "Name", "Value", "Source", "Origin" });
        foreach (var macro in macros)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var nameCell = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(5, 4, 5, 4),
            };
            // Only ever links a "compiler"-sourced macro to the definition
            // site the compiler itself reported; a "configuration"-sourced
            // macro has no single definition site in source text and always
            // renders as plain text.
            if (string.Equals(macro.Source, "compiler", StringComparison.Ordinal) &&
                !string.IsNullOrEmpty(macro.Uri) &&
                macro.Line.HasValue &&
                macro.Character.HasValue)
            {
                var link = new Hyperlink(new Run(macro.Name ?? string.Empty))
                {
                    ToolTip = "Go to definition",
                };
                var uri = macro.Uri;
                var line = macro.Line.Value;
                var character = macro.Character.Value;
                link.Click += (_, _) => NavigateToPoint(uri, line, character);
                nameCell.Inlines.Add(link);
            }
            else
            {
                nameCell.Inlines.Add(new Run(macro.Name ?? string.Empty));
            }
            Grid.SetRow(nameCell, row);
            Grid.SetColumn(nameCell, 0);
            grid.Children.Add(nameCell);

            AddPlainCell(grid, row, 1, macro.Value ?? string.Empty);
            AddPlainCell(
                grid,
                row,
                2,
                string.Equals(macro.Source, "compiler", StringComparison.Ordinal)
                    ? "Compiler"
                    : "Configuration");
            AddOriginCell(grid, row, 3, macro.Origin, macro.OriginUri);

            ++row;
        }
        content.Children.Add(grid);
    }

    // --- Effective settings -------------------------------------------------

    private void AddSettings(IReadOnlyList<PreprocessorSettingModel> settings)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (settings == null || settings.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        var grid = new Grid();
        foreach (var width in new[] { 200d, 320d, 200d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableHeaderRow(grid, row++, new[] { "Setting", "Value", "Origin" });
        foreach (var setting in settings)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            AddPlainCell(grid, row, 0, setting.Name ?? string.Empty);
            AddPlainCell(grid, row, 1, SettingValueText(setting.Value));
            AddOriginCell(grid, row, 2, setting.Origin, setting.OriginUri);
            ++row;
        }
        content.Children.Add(grid);
    }

    // The server reports a setting's value in whatever raw JSON shape it
    // has (scalar, array, or object); this renders any of those shapes
    // legibly without needing a bespoke converter per setting name.
    private static string SettingValueText(JToken value)
    {
        if (value == null || value.Type == JTokenType.Null)
        {
            return "(none)";
        }
        if (value.Type == JTokenType.Array)
        {
            var items = value.Select(item => item.ToString()).ToArray();
            return items.Length == 0 ? "(none)" : string.Join(", ", items);
        }
        if (value.Type == JTokenType.Object)
        {
            var entries = ((JObject)value)
                .Properties()
                .Select(property => $"{property.Name} \u2192 {property.Value}")
                .ToArray();
            return entries.Length == 0 ? "(none)" : string.Join(", ", entries);
        }
        var text = value.ToString();
        return string.IsNullOrEmpty(text) ? "(none)" : text;
    }

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

    private void AddOriginCell(
        Grid grid,
        int row,
        int column,
        string origin,
        string originUri)
    {
        var block = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(5, 4, 5, 4),
        };
        if (string.IsNullOrEmpty(originUri))
        {
            block.Text = origin ?? string.Empty;
        }
        else
        {
            var link = new Hyperlink(new Run(origin ?? originUri))
            {
                ToolTip = "Open configuration",
            };
            link.Click += (_, _) =>
            {
                ThreadHelper.ThrowIfNotOnUIThread();
                NavigateToPoint(originUri, 0, 0);
            };
            block.Inlines.Add(link);
        }
        Grid.SetRow(block, row);
        Grid.SetColumn(block, column);
        grid.Children.Add(block);
    }

    // --- Navigation --------------------------------------------------

    // Opens the authoritative file/position on an explicit user click
    // (never from background save/variant refresh, which never calls this
    // method). Validates the untrusted uri/position before touching any
    // file or VS SDK API, and surfaces an explicit error dialog on any
    // failure rather than failing silently or guessing a fallback
    // location. Mirrors ResourceBindingsToolWindow.NavigateToResourceLocation,
    // generalized to the several kinds of location this view links to (a
    // file, an #include directive, a resolved include target, a macro
    // definition, or a skipped region) rather than one fixed model type.
    private void NavigateToPoint(string uri, long line, long character)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        NavigateToRange(uri, line, character, line, character);
    }

    private void NavigateToRange(
        string uri,
        long startLine,
        long startCharacter,
        long endLine,
        long endCharacter)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!TryValidateLocation(
                uri,
                startLine,
                startCharacter,
                endLine,
                endCharacter,
                out var filePath,
                out var vsStartLine,
                out var vsStartCharacter,
                out var vsEndLine,
                out var vsEndCharacter,
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
            view?.SetSelection(vsStartLine, vsStartCharacter, vsEndLine, vsEndCharacter);
            view?.EnsureSpanVisible(new TextSpan
            {
                iStartLine = vsStartLine,
                iStartIndex = vsStartCharacter,
                iEndLine = vsEndLine,
                iEndIndex = vsEndCharacter,
            });
        }
        catch (Exception ex)
        {
            ShowNavigationError(
                $"Unable to navigate to the preprocessor location: {ex.Message}");
        }
    }

    private static void ShowNavigationError(string message)
    {
        VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Preprocessor Explorer",
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
        long startLine,
        long startCharacter,
        long endLine,
        long endCharacter,
        out string filePath,
        out int vsStartLine,
        out int vsStartCharacter,
        out int vsEndLine,
        out int vsEndCharacter,
        out string error)
    {
        filePath = null;
        vsStartLine = vsStartCharacter = vsEndLine = vsEndCharacter = 0;
        error = null;
        if (string.IsNullOrEmpty(uriValue))
        {
            error = "The preprocessor location was missing.";
            return false;
        }
        Uri uri;
        try
        {
            uri = new Uri(uriValue, UriKind.Absolute);
        }
        catch (UriFormatException)
        {
            error = "The preprocessor location's URI could not be parsed.";
            return false;
        }
        if (!uri.IsFile)
        {
            error = "The preprocessor location does not refer to a local file.";
            return false;
        }
        if (!TryToInt32(startLine, out vsStartLine) ||
            !TryToInt32(startCharacter, out vsStartCharacter) ||
            !TryToInt32(endLine, out vsEndLine) ||
            !TryToInt32(endCharacter, out vsEndCharacter))
        {
            error = "The preprocessor location's range contained an invalid position.";
            return false;
        }
        if (vsEndLine < vsStartLine ||
            (vsEndLine == vsStartLine && vsEndCharacter < vsStartCharacter))
        {
            error = "The preprocessor location's range was inverted.";
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
