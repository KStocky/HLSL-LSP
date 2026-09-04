using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using Microsoft.VisualStudio.PlatformUI;

namespace HlslLsp.VisualStudio.Bootstrap;

// A small modal picker for the active shader compilation variant. The dialog is
// built programmatically so no XAML resource is required. Selecting an entry and
// confirming exposes the chosen variant name through SelectedVariant, or null to
// clear the active variant.
internal sealed class VariantSelectionDialog : DialogWindow
{
    private readonly ListBox listBox;

    public string SelectedVariant { get; private set; }

    public VariantSelectionDialog(VariantListModel variants)
    {
        Title = "Select HLSL Shader Variant";
        Width = 460;
        Height = 320;
        HasMinimizeButton = false;
        HasMaximizeButton = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var active = variants?.ActiveVariant ?? string.Empty;

        listBox = new ListBox { Margin = new Thickness(12) };
        listBox.Items.Add(new ListBoxItem { Content = "(No variant)", Tag = null });
        var selectedIndex = 0;
        if (variants?.Variants != null)
        {
            var index = 1;
            foreach (var variant in variants.Variants)
            {
                var description = string.IsNullOrEmpty(variant.Description)
                    ? string.Empty
                    : " \u2014 " + variant.Description;
                var note = variant.Applicable
                    ? string.Empty
                    : "  (not applicable to this file)";
                listBox.Items.Add(new ListBoxItem
                {
                    Content = variant.Name + description + note,
                    Tag = variant.Name,
                });
                if (string.Equals(variant.Name, active, StringComparison.Ordinal))
                {
                    selectedIndex = index;
                }
                ++index;
            }
        }
        listBox.SelectedIndex = selectedIndex;
        listBox.MouseDoubleClick += (_, _) => Accept();

        var ok = new Button
        {
            Content = "OK",
            Width = 84,
            Margin = new Thickness(0, 0, 8, 0),
            IsDefault = true,
        };
        ok.Click += (_, _) => Accept();
        var cancel = new Button { Content = "Cancel", Width = 84, IsCancel = true };

        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(12, 0, 12, 12),
        };
        buttons.Children.Add(ok);
        buttons.Children.Add(cancel);

        var grid = new Grid();
        grid.RowDefinitions.Add(
            new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetRow(listBox, 0);
        Grid.SetRow(buttons, 1);
        grid.Children.Add(listBox);
        grid.Children.Add(buttons);
        Content = grid;
    }

    private void Accept()
    {
        if (listBox.SelectedItem is ListBoxItem selected)
        {
            SelectedVariant = selected.Tag as string;
            DialogResult = true;
        }
    }
}
