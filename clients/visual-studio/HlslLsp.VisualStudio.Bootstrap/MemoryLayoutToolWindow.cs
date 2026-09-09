using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.VisualStudio.Shell;

namespace HlslLsp.VisualStudio.Bootstrap;

[Guid("9d208088-c1e2-451d-907c-6e7f825b9714")]
public sealed class MemoryLayoutToolWindow : ToolWindowPane
{
    private readonly MemoryLayoutControl control = new();

    public MemoryLayoutToolWindow()
        : base(null)
    {
        Caption = "HLSL Memory Layout";
        Content = control;
    }

    internal void SetLayout(MemoryLayoutModel layout)
        => control.SetLayout(layout);
}

internal sealed class MemoryLayoutControl : UserControl
{
    private static readonly Brush[] MemberBrushes =
    {
        Brushes.DodgerBlue,
        Brushes.MediumOrchid,
        Brushes.OrangeRed,
        Brushes.LimeGreen,
        Brushes.DeepPink,
        Brushes.Goldenrod,
    };

    private readonly StackPanel content = new();

    internal MemoryLayoutControl()
    {
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        SetLayout(null);
    }

    internal void SetLayout(MemoryLayoutModel layout)
    {
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (layout == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Right-click an HLSL type, cbuffer, or member, then choose " +
                       "HLSL > Memory Layout.",
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        content.Children.Add(new TextBlock
        {
            Text = string.IsNullOrEmpty(layout.Name) ? layout.Type : layout.Name,
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        content.Children.Add(new TextBlock
        {
            Text = $"{ModeName(layout.Mode)} · size {layout.Size} bytes · " +
                   $"alignment {layout.Alignment} bytes · allocation {layout.AllocationSize} bytes",
            Margin = new Thickness(0, 3, 0, 12),
            Opacity = 0.75,
        });
        foreach (var diagnostic in layout.Diagnostics ?? Array.Empty<string>())
        {
            content.Children.Add(new TextBlock
            {
                Text = diagnostic,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            });
        }
        AddDiagram(layout);
        AddMemberTable(layout);
    }

    private void AddDiagram(MemoryLayoutModel layout)
    {
        const int bytesPerRow = 16;
        const double pixelsPerByte = 34;
        var segments = Flatten(
                layout.Members ?? Array.Empty<MemoryLayoutMemberModel>(),
                0,
                0,
                null,
                string.Empty,
                layout.AllocationSize)
            .ToArray();
        var total = Math.Max(bytesPerRow, Math.Max(layout.Size, layout.AllocationSize));
        var rows = (int)((total + bytesPerRow - 1) / bytesPerRow);
        for (var row = 0; row < rows; ++row)
        {
            var start = row * bytesPerRow;
            var line = new Grid { Margin = new Thickness(0, 2, 0, 2) };
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(52) });
            line.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(bytesPerRow * pixelsPerByte) });
            var offset = new TextBlock
            {
                Text = start.ToString(),
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 8, 0),
                FontFamily = new FontFamily("Consolas"),
            };
            line.Children.Add(offset);
            var lane = new StackPanel();
            var scale = new Canvas
            {
                Width = bytesPerRow * pixelsPerByte,
                Height = 18,
            };
            var labels = MemoryLayoutByteScale.Labels(start);
            for (var labelIndex = 0; labelIndex < labels.Count; ++labelIndex)
            {
                var marker = labelIndex * 4;
                var label = new TextBlock
                {
                    Text = labels[labelIndex].ToString(),
                    FontFamily = new FontFamily("Consolas"),
                    FontSize = 11,
                    Opacity = 0.75,
                };
                scale.Children.Add(label);
                Canvas.SetLeft(
                    label,
                    Math.Max(0, Math.Min(
                        marker * pixelsPerByte - (marker == 0 ? 0 : 8),
                        bytesPerRow * pixelsPerByte - 24)));
            }
            lane.Children.Add(scale);
            var canvas = new Canvas
            {
                Width = bytesPerRow * pixelsPerByte,
                Height = 38,
                Background = new SolidColorBrush(Color.FromArgb(25, 128, 128, 128)),
            };
            for (var marker = 2; marker < bytesPerRow; marker += 2)
            {
                var guide = new Border
                {
                    BorderBrush = new SolidColorBrush(Color.FromArgb(
                        marker % 4 == 0 ? (byte)70 : (byte)35,
                        128,
                        128,
                        128)),
                    BorderThickness = new Thickness(1, 0, 0, 0),
                    Width = 1,
                    Height = 38,
                };
                canvas.Children.Add(guide);
                Canvas.SetLeft(guide, marker * pixelsPerByte);
            }
            foreach (var segment in segments.Where(
                         item => item.Size > 0 &&
                                 item.Offset < start + bytesPerRow &&
                                 item.Offset + item.Size > start))
            {
                var segmentStart = Math.Max(segment.Offset, start);
                var segmentEnd = Math.Min(segment.Offset + segment.Size, start + bytesPerRow);
                var border = new Border
                {
                    BorderBrush = segment.IsPadding
                        ? Brushes.Gray
                        : MemberBrushes[segment.Depth % MemberBrushes.Length],
                    BorderThickness = new Thickness(segment.IsPadding ? 1 : 2),
                    Background = new SolidColorBrush(
                        segment.IsPadding
                            ? Color.FromArgb(22, 128, 128, 128)
                            : Color.FromArgb(35, 128, 128, 128)),
                    Width = Math.Max(2, (segmentEnd - segmentStart) * pixelsPerByte),
                    Height = 38,
                    ToolTip = $"{segment.Name}: offset {segment.Offset}, {segment.Size} bytes",
                    Child = new TextBlock
                    {
                        Text = segment.Name,
                        TextAlignment = TextAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                        VerticalAlignment = VerticalAlignment.Center,
                        Margin = new Thickness(4, 0, 4, 0),
                    },
                };
                canvas.Children.Add(border);
                Canvas.SetLeft(border, (segmentStart - start) * pixelsPerByte);
            }
            lane.Children.Add(canvas);
            Grid.SetColumn(lane, 1);
            line.Children.Add(lane);
            content.Children.Add(line);
        }
    }

    private void AddMemberTable(MemoryLayoutModel layout)
    {
        var grid = new Grid { Margin = new Thickness(0, 14, 0, 0) };
        foreach (var width in new[] { 220d, 180d, 70d, 60d, 75d, 95d })
        {
            grid.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(grid, row++, new[] { "Member", "Type", "Offset", "Size", "Align", "Padding" },
            true);
        AddMembers(grid, layout.Members ?? Array.Empty<MemoryLayoutMemberModel>(), 0, 0, ref row);
        content.Children.Add(grid);
    }

    private static void AddMembers(
        Grid grid,
        IEnumerable<MemoryLayoutMemberModel> members,
        long baseOffset,
        int depth,
        ref int row)
    {
        foreach (var member in members)
        {
            var absoluteOffset = baseOffset + member.Offset;
            AddTableRow(
                grid,
                row++,
                new[]
                {
                    new string(' ', depth * 2) + member.Name,
                    member.Type,
                    absoluteOffset.ToString(),
                    member.Size.ToString(),
                    member.Alignment.ToString(),
                    member.PaddingBefore.ToString(),
                },
                false);
            AddMembers(
                grid,
                member.Members ?? Array.Empty<MemoryLayoutMemberModel>(),
                absoluteOffset,
                depth + 1,
                ref row);
        }
    }

    private static void AddTableRow(Grid grid, int row, string[] values, bool header)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (var column = 0; column < values.Length; ++column)
        {
            var text = new TextBlock
            {
                Text = values[column],
                FontWeight = header ? FontWeights.SemiBold : FontWeights.Normal,
                FontFamily = column == 1 ? new FontFamily("Consolas") : null,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(text, row);
            Grid.SetColumn(text, column);
            grid.Children.Add(text);
        }
    }

    private static IEnumerable<LayoutSegment> Flatten(
        IEnumerable<MemoryLayoutMemberModel> members,
        long baseOffset,
        int depth,
        MemoryLayoutMemberModel parent,
        string parentName,
        long extent)
    {
        long previousEnd = 0;
        foreach (var member in members)
        {
            var offset = baseOffset + member.Offset;
            if (member.Offset > previousEnd)
            {
                var internalPadding = string.Equals(parent?.Kind, "array", StringComparison.Ordinal)
                    || string.Equals(parent?.Kind, "matrix", StringComparison.Ordinal);
                yield return new LayoutSegment(
                    internalPadding ? "internal padding" : "inter-member padding",
                    baseOffset + previousEnd,
                    member.Offset - previousEnd,
                    depth,
                    true);
            }
            var name = MemoryLayoutDisplayName.Qualify(
                parentName,
                member.Name,
                parent?.Kind,
                parent?.RowMajor == true);
            if (member.Members == null || member.Members.Count == 0)
            {
                yield return new LayoutSegment(name, offset, member.Size, depth, false);
                if (member.AllocationSize > member.Size)
                {
                    yield return new LayoutSegment(
                        "internal padding",
                        offset + member.Size,
                        member.AllocationSize - member.Size,
                        depth + 1,
                        true);
                }
            }
            else
            {
                foreach (var nested in Flatten(
                             member.Members,
                             offset,
                             depth + 1,
                             member,
                             name,
                             member.AllocationSize))
                {
                    yield return nested;
                }
            }
            previousEnd = Math.Max(previousEnd, member.Offset + member.AllocationSize);
        }
        if (extent > previousEnd)
        {
            yield return new LayoutSegment(
                "trailing padding",
                baseOffset + previousEnd,
                extent - previousEnd,
                depth,
                true);
        }
    }

    private static string ModeName(string mode)
        => string.Equals(mode, "constantBuffer", StringComparison.Ordinal)
            ? "Constant-buffer packing"
            : "Natural / structured-buffer layout";

    private sealed class LayoutSegment
    {
        internal LayoutSegment(string name, long offset, long size, int depth, bool isPadding)
        {
            Name = name;
            Offset = offset;
            Size = size;
            Depth = depth;
            IsPadding = isPadding;
        }

        internal string Name { get; }

        internal long Offset { get; }

        internal long Size { get; }

        internal int Depth { get; }

        internal bool IsPadding { get; }
    }
}
