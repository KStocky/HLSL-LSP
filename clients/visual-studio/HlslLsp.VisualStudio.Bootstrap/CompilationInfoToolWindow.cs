using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.Win32;

namespace HlslLsp.VisualStudio.Bootstrap;

[Guid("050603cd-412a-4cd8-b2f8-62bc4e854367")]
public sealed class CompilationInfoToolWindow : ToolWindowPane, IAnalysisFreshnessView
{
    private readonly CompilationInfoControl control = new();
    private readonly AnalysisFreshnessTracker freshness = new();
    private readonly AnalysisFreshnessHeader freshnessHeader =
        new(AnalysisViewKind.CompilationInfo);

    public CompilationInfoToolWindow()
        : base(null)
    {
        Caption = "HLSL Shader Compilation";
        Content = AnalysisFreshnessHeader.Wrap(freshnessHeader, control);
    }

    // Tracks the document the window currently shows so external refresh
    // triggers (active-variant selection, document save) know which document
    // to re-request without needing the caret or active-view context.
    internal Uri DocumentUri { get; private set; }

    internal void SetInfo(Uri uri, CompilationInfoModel info)
    {
        DocumentUri = uri;
        freshness.Succeed();
        freshnessHeader.Update(freshness.State);
        control.SetInfo(uri, info);
    }

    // A failed or cancelled request must never regress this window to the
    // "open a document" placeholder or leave it stuck showing a previous
    // request's transient state: SetError keeps whatever the control
    // already renders when it has real content, and only replaces the
    // placeholder with an explicit error when there is no content yet.
    internal void SetError(Uri uri, string message, bool preserveSameDocumentContent)
    {
        DocumentUri = uri;
        freshness.Fail();
        freshnessHeader.Update(freshness.State);
        control.SetError(message, true);
    }

    public void BeginRefresh(AnalysisFreshnessCause cause)
    {
        freshness.Begin(cause);
        freshnessHeader.Update(freshness.State);
    }

    public void MarkStale(AnalysisFreshnessCause cause, bool refreshPending = false)
    {
        freshness.Invalidate(cause, refreshPending);
        freshnessHeader.Update(freshness.State);
    }
}

internal sealed class CompilationInfoControl : UserControl
{
    private readonly StackPanel content = new();
    private bool hasContent;

    internal CompilationInfoControl()
    {
        VisualStudioTheme.ApplyToolWindowTheme(this);
        Content = VisualStudioTheme.ApplyScrollViewerStyle(new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        });
        SetInfo(null, null);
    }

    internal void SetInfo(Uri documentUri, CompilationInfoModel info)
    {
        hasContent = info != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (info == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Right-click an HLSL document, then choose " +
                       "HLSL > Shader Compilation.",
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        AddHeader(info);
        AddStatus(info);
        AddSection("Effective configuration", () => AddConfiguration(info));
        if (info.Output != null)
        {
            AddSection("Output", () => AddOutput(info.Output));
        }
        AddSection("Disassembly", () => AddDisassembly(documentUri, info.Disassembly));
        AddSection("Reflection", () => AddReflection(info.Reflection));
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

    private void AddHeader(CompilationInfoModel info)
    {
        EffectiveShaderContextDisplay.AddHeader(
            content,
            "Shader Compilation",
            info.Context);
        content.Children.Add(new TextBlock
        {
            Text = $"Stage: {EffectiveShaderContextDisplay.Value(info.Stage)} · " +
                   $"Language version: {EffectiveShaderContextDisplay.Value(info.LanguageVersion)}",
            Margin = new Thickness(0, 0, 0, 12),
            Opacity = 0.75,
        });
    }

    private void AddStatus(CompilationInfoModel info)
    {
        content.Children.Add(new TextBlock
        {
            Text = info.Success ? "Compilation succeeded" : "Compilation failed",
            Foreground = info.Success ? Brushes.LimeGreen : Brushes.OrangeRed,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 0, 0, 6),
        });
        foreach (var diagnostic in info.Diagnostics ??
                                   Array.Empty<CompilationDiagnosticModel>())
        {
            var location = string.IsNullOrEmpty(diagnostic.Path)
                ? string.Empty
                : $"{diagnostic.Path}:{diagnostic.Line}:{diagnostic.Column} - ";
            content.Children.Add(new TextBlock
            {
                Text = $"{location}[{diagnostic.Severity}] {diagnostic.Message}",
                Foreground = SeverityBrush(diagnostic.Severity),
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            });
        }
    }

    private static Brush SeverityBrush(string severity)
        => severity switch
        {
            "error" => Brushes.OrangeRed,
            "warning" => Brushes.Goldenrod,
            _ => Brushes.DodgerBlue,
        };

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

    private void AddConfiguration(CompilationInfoModel info)
    {
        AddKeyValue("Compiler arguments", JoinOrNone(info.CompilerArguments));
        AddKeyValue("Preprocessor defines", JoinOrNone(info.Defines));
        AddKeyValue("Include directories", JoinOrNone(info.IncludeDirectories));
        AddKeyValue(
            "Resolved include paths",
            JoinOrNone(info.ResolvedIncludePaths));
    }

    private void AddOutput(CompilationOutputModel output)
    {
        AddKeyValue("Type", output.Type);
        AddKeyValue("Size", $"{output.Size} bytes");
    }

    // Renders the compiler-generated disassembly text produced by DXC's own
    // disassembler (see docs/compilation-info.md). Never reconstructed,
    // decoded, or annotated by this client; shown verbatim in a bounded,
    // scrollable read-only text box so an unusually large listing cannot
    // blow out the rest of the window. Copy/Save buttons are added only
    // when disassembly text actually exists to act on, and always close
    // over this exact rendered CompilationDisassemblyModel instance -- a
    // later refresh replaces the whole content tree (and its buttons)
    // rather than mutating it, so a button can never act on a stale or
    // different document's text.
    private void AddDisassembly(Uri documentUri, CompilationDisassemblyModel disassembly)
    {
        if (disassembly == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Disassembly is unavailable because compilation did not " +
                       "produce output.",
                TextWrapping = TextWrapping.Wrap,
                Opacity = 0.75,
            });
            return;
        }
        if (!disassembly.Available)
        {
            content.Children.Add(new TextBlock
            {
                Text = string.IsNullOrEmpty(disassembly.UnavailableReason)
                    ? "Disassembly is unavailable for this output."
                    : disassembly.UnavailableReason,
                TextWrapping = TextWrapping.Wrap,
                Foreground = Brushes.Goldenrod,
            });
            return;
        }

        AddKeyValue("Format", disassembly.Format);
        AddKeyValue(
            "Size",
            disassembly.Truncated
                ? $"{disassembly.DisplayedSize} of {disassembly.OriginalSize} bytes (truncated for display)"
                : $"{disassembly.DisplayedSize} bytes");
        if (disassembly.Truncated)
        {
            content.Children.Add(new TextBlock
            {
                Text = "The compiler's disassembly output was truncated; Copy " +
                       "and Save operate on the same retained text shown " +
                       "below, not the compiler's full output.",
                TextWrapping = TextWrapping.Wrap,
                Foreground = Brushes.Goldenrod,
                Margin = new Thickness(0, 0, 0, 6),
            });
        }

        if (!string.IsNullOrEmpty(disassembly.Text))
        {
            var buttons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Margin = new Thickness(0, 0, 0, 6),
            };
            var copyButton = VisualStudioTheme.ApplyButtonStyle(new Button
            {
                Content = "Copy Disassembly",
                Padding = new Thickness(8, 2, 8, 2),
                Margin = new Thickness(0, 0, 8, 0),
            });
            copyButton.Click += (_, _) => CopyDisassembly(disassembly.Text);
            var saveButton = VisualStudioTheme.ApplyButtonStyle(new Button
            {
                Content = "Save Disassembly\u2026",
                Padding = new Thickness(8, 2, 8, 2),
            });
            saveButton.Click += (_, _) => SaveDisassembly(documentUri, disassembly);
            buttons.Children.Add(copyButton);
            buttons.Children.Add(saveButton);
            content.Children.Add(buttons);
        }

        content.Children.Add(VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = disassembly.Text,
            IsReadOnly = true,
            IsReadOnlyCaretVisible = true,
            TextWrapping = TextWrapping.NoWrap,
            AcceptsReturn = true,
            FontFamily = new FontFamily("Consolas"),
            MaxHeight = 320,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Margin = new Thickness(0, 0, 0, 4),
        }));
    }

    // Clipboard access can throw (for example, another process transiently
    // holding the clipboard open), so failure is surfaced explicitly rather
    // than silently discarded.
    private static void CopyDisassembly(string text)
    {
        try
        {
            Clipboard.SetText(text);
        }
        catch (Exception ex) when (ex is COMException or ExternalException)
        {
            ShowDisassemblyError($"Unable to copy disassembly to the clipboard: {ex.Message}");
        }
    }

    // documentUri may be null (or non-file) for an unsaved/virtual buffer;
    // DisassemblyFileNaming.SuggestedFileName handles that by falling back
    // to a generic name rather than throwing.
    private static void SaveDisassembly(Uri documentUri, CompilationDisassemblyModel disassembly)
    {
        var suggestedName = DisassemblyFileNaming.SuggestedFileName(
            documentUri?.IsFile == true ? documentUri.LocalPath : null,
            disassembly.Format);
        var isSpirv = string.Equals(disassembly.Format, "spirv", StringComparison.OrdinalIgnoreCase);
        var dialog = new SaveFileDialog
        {
            FileName = suggestedName,
            DefaultExt = isSpirv
                ? DisassemblyFileNaming.SpirvExtension
                : DisassemblyFileNaming.DxilExtension,
            Filter = isSpirv
                ? "SPIR-V Assembly (*.spvasm)|*.spvasm|All Files (*.*)|*.*"
                : "DXIL Disassembly (*.ll)|*.ll|All Files (*.*)|*.*",
        };
        if (dialog.ShowDialog() != true)
        {
            return;
        }
        try
        {
            File.WriteAllBytes(dialog.FileName, DisassemblyFileContent.Encode(disassembly.Text));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            ShowDisassemblyError($"Unable to save disassembly: {ex.Message}");
        }
    }

    private static void ShowDisassemblyError(string message)
    {
        VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Shader Compilation",
            OLEMSGICON.OLEMSGICON_WARNING,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    private void AddReflection(CompilationReflectionModel reflection)
    {
        if (reflection == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Reflection is unavailable because compilation did not " +
                       "produce output.",
                TextWrapping = TextWrapping.Wrap,
                Opacity = 0.75,
            });
            return;
        }
        if (!reflection.Available)
        {
            content.Children.Add(new TextBlock
            {
                Text = string.IsNullOrEmpty(reflection.UnavailableReason)
                    ? "Reflection is unavailable for this output."
                    : reflection.UnavailableReason,
                TextWrapping = TextWrapping.Wrap,
                Foreground = Brushes.Goldenrod,
            });
            return;
        }

        if (reflection.ThreadGroupSize != null)
        {
            AddKeyValue(
                "Thread-group size",
                $"{reflection.ThreadGroupSize.X} x " +
                $"{reflection.ThreadGroupSize.Y} x {reflection.ThreadGroupSize.Z}");
        }
        AddSignatureTable("Input signature", reflection.InputSignature);
        AddSignatureTable("Output signature", reflection.OutputSignature);
        AddResourceTable(reflection.Resources);
    }

    private void AddSignatureTable(
        string title,
        IReadOnlyList<CompilationSignatureParameterModel> parameters)
    {
        content.Children.Add(new TextBlock
        {
            Text = title,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 8, 0, 4),
        });
        if (parameters == null || parameters.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }

        var grid = new Grid();
        foreach (var width in new[] { 140d, 60d, 70d, 120d, 110d, 60d, 90d, 70d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(
            grid,
            row++,
            new[]
            {
                "Semantic", "Index", "Register", "System value", "Component type",
                "Mask", "R/W mask", "Stream",
            },
            true);
        foreach (var parameter in parameters)
        {
            AddTableRow(
                grid,
                row++,
                new[]
                {
                    parameter.SemanticName,
                    parameter.SemanticIndex.ToString(),
                    parameter.Register.ToString(),
                    parameter.SystemValue,
                    parameter.ComponentType,
                    parameter.Mask.ToString(),
                    parameter.ReadWriteMask.ToString(),
                    parameter.Stream.ToString(),
                },
                false);
        }
        content.Children.Add(grid);
    }

    private void AddResourceTable(IReadOnlyList<CompilationResourceBindingModel> resources)
    {
        content.Children.Add(new TextBlock
        {
            Text = "Resources",
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 8, 0, 4),
        });
        if (resources == null || resources.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }

        var grid = new Grid();
        foreach (var width in new[] { 160d, 100d, 80d, 80d, 60d, 100d, 100d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(
            grid,
            row++,
            new[]
            {
                "Name", "Type", "Bind point", "Bind count", "Space", "Return type",
                "Dimension",
            },
            true);
        foreach (var resource in resources)
        {
            AddTableRow(
                grid,
                row++,
                new[]
                {
                    resource.Name,
                    resource.Type,
                    resource.BindPoint.ToString(),
                    resource.BindCount.ToString(),
                    resource.Space.ToString(),
                    resource.ReturnType,
                    resource.Dimension,
                },
                false);
        }
        content.Children.Add(grid);
    }

    private static void AddTableRow(Grid grid, int row, string[] values, bool header)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (var column = 0; column < values.Length; ++column)
        {
            var text = new TextBlock
            {
                Text = values[column] ?? string.Empty,
                FontWeight = header ? FontWeights.SemiBold : FontWeights.Normal,
                TextTrimming = TextTrimming.CharacterEllipsis,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(text, row);
            Grid.SetColumn(text, column);
            grid.Children.Add(text);
        }
    }

    private void AddKeyValue(string key, string value)
    {
        var line = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 4),
        };
        line.Inlines.Add(new System.Windows.Documents.Run($"{key}: ")
        {
            FontWeight = FontWeights.SemiBold,
        });
        line.Inlines.Add(new System.Windows.Documents.Run(value));
        content.Children.Add(line);
    }

    private static string JoinOrNone(IReadOnlyList<string> values)
        => values == null || values.Count == 0
            ? "(none)"
            : string.Join(", ", values);
}
