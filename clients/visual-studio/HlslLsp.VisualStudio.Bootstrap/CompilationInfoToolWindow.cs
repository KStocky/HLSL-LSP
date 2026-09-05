using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.VisualStudio.Shell;

namespace HlslLsp.VisualStudio.Bootstrap;

[Guid("050603cd-412a-4cd8-b2f8-62bc4e854367")]
public sealed class CompilationInfoToolWindow : ToolWindowPane
{
    private readonly CompilationInfoControl control = new();

    public CompilationInfoToolWindow()
        : base(null)
    {
        Caption = "HLSL Shader Compilation";
        Content = control;
    }

    // Tracks the document the window currently shows so external refresh
    // triggers (active-variant selection, document save) know which document
    // to re-request without needing the caret or active-view context.
    internal Uri DocumentUri { get; private set; }

    internal void SetInfo(Uri uri, CompilationInfoModel info)
    {
        DocumentUri = uri;
        control.SetInfo(info);
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

internal sealed class CompilationInfoControl : UserControl
{
    private readonly StackPanel content = new();
    private bool hasContent;

    internal CompilationInfoControl()
    {
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        SetInfo(null);
    }

    internal void SetInfo(CompilationInfoModel info)
    {
        hasContent = info != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (info == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Open an HLSL document, then run " +
                       "Tools > HLSL Shader Compilation.",
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
        var title = string.IsNullOrEmpty(info.EntryPoint)
            ? info.TargetProfile
            : $"{info.EntryPoint} ({info.TargetProfile})";
        content.Children.Add(new TextBlock
        {
            Text = string.IsNullOrEmpty(title) ? "Shader compilation" : title,
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        var variant = string.IsNullOrEmpty(info.ActiveVariant)
            ? "(none)"
            : info.ActiveVariant;
        content.Children.Add(new TextBlock
        {
            Text = $"Stage {info.Stage} · language version {info.LanguageVersion} · " +
                   $"active variant {variant}",
            Margin = new Thickness(0, 3, 0, 12),
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
