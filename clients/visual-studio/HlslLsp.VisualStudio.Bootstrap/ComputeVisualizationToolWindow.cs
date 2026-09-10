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

[Guid("495a0395-bc57-4591-a201-b278475d516e")]
public sealed class ComputeVisualizationToolWindow : ToolWindowPane
{
    private readonly ComputeVisualizationControl control = new();
    private readonly ComputeVisualizationInteractionState interactionState = new();

    public ComputeVisualizationToolWindow()
        : base(null)
    {
        Caption = "HLSL Compute Visualization";
        control.ApplyRequested = options =>
        {
            interactionState.Submit(options);
            if (interactionState.RequestedDocumentUri != null)
            {
                ComputeVisualizationBridge.Show(
                    interactionState.RequestedDocumentUri,
                    options);
            }
            else
            {
                control.SetInputError(
                    "Open an HLSL document, then choose HLSL > Compute Visualization.");
            }
        };
        Content = control;
    }

    internal Uri DocumentUri => interactionState.RequestedDocumentUri;

    internal Uri DisplayedDocumentUri => interactionState.DisplayedDocumentUri;

    internal ComputeVisualizationOptions SubmittedOptions =>
        interactionState.LastSubmittedOptions;

    internal void TrackRequest(Uri uri)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        interactionState.TrackRequest(uri);
    }

    internal void SetReport(
        Uri uri,
        ComputeVisualizationOptions options,
        ComputeVisualizationModel report)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        interactionState.MarkDisplayed(uri);
        control.SetOptionsIfDocumentChanged(options);
        control.SetReport(report, options?.HardwareProfile);
    }

    internal void SetError(
        Uri uri,
        ComputeVisualizationOptions options,
        string message,
        bool preserveSameDocumentContent)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var preserve =
            preserveSameDocumentContent &&
            interactionState.ShouldPreserveDisplayedContentOnFailure(uri);
        interactionState.TrackRequest(uri);
        control.SetOptionsIfDocumentChanged(options);
        control.SetError(message, preserve);
    }

    internal void SetRequestError(string message)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        control.SetInputError(message);
    }
}

internal sealed class ComputeVisualizationControl : UserControl
{
    private readonly StackPanel content = new();
    private readonly StackPanel configuration = new();
    private readonly TextBox dispatchX = Input(string.Empty);
    private readonly TextBox dispatchY = Input(string.Empty);
    private readonly TextBox dispatchZ = Input(string.Empty);
    private readonly TextBox profileName = Input(string.Empty, 170);
    private readonly TextBox waveSize = Input(string.Empty);
    private readonly TextBox maxThreadsPerGroup = Input(string.Empty);
    private readonly TextBox maxThreadsPerComputeUnit = Input(string.Empty);
    private readonly TextBox maxGroupsPerComputeUnit = Input(string.Empty);
    private readonly TextBox sharedMemoryBytesPerComputeUnit = Input(string.Empty, 120);
    private readonly TextBlock inputError = new()
    {
        Foreground = Brushes.OrangeRed,
        TextWrapping = TextWrapping.Wrap,
    };
    private bool hasContent;

    internal Action<ComputeVisualizationOptions> ApplyRequested { get; set; }

    internal ComputeVisualizationControl()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        VisualStudioTheme.ApplyToolWindowTheme(this);
        Content = VisualStudioTheme.ApplyScrollViewerStyle(new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        });
        BuildConfiguration();
        SetReport(null, null);
    }

    internal void SetOptionsIfDocumentChanged(ComputeVisualizationOptions options)
    {
        if (hasContent || options == null)
        {
            return;
        }
        var dispatch = options.DispatchDimensions;
        if (dispatch != null)
        {
            dispatchX.Text = dispatch.X.ToString();
            dispatchY.Text = dispatch.Y.ToString();
            dispatchZ.Text = dispatch.Z.ToString();
        }
        var profile = options.HardwareProfile;
        if (profile != null)
        {
            profileName.Text = profile.Name ?? string.Empty;
            waveSize.Text = profile.WaveSize.ToString();
            maxThreadsPerGroup.Text = profile.MaxThreadsPerGroup.ToString();
            maxThreadsPerComputeUnit.Text = profile.MaxThreadsPerComputeUnit.ToString();
            maxGroupsPerComputeUnit.Text = profile.MaxGroupsPerComputeUnit.ToString();
            sharedMemoryBytesPerComputeUnit.Text =
                profile.SharedMemoryBytesPerComputeUnit.ToString();
        }
    }

    internal void SetReport(
        ComputeVisualizationModel report,
        ComputeHardwareProfileModel submittedHardwareProfile)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        inputError.Text = string.Empty;
        hasContent = report != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        AddConfiguration();
        if (report == null)
        {
            AddText(
                "Open an HLSL document, then choose HLSL > Compute Visualization.",
                Brushes.Gray);
            return;
        }
        AddText(
            $"Entry point: {report.EntryPoint ?? "(not resolved)"}   " +
            $"Target: {report.TargetProfile ?? "(not configured)"}",
            Brushes.Gray);
        if (!report.Applicable)
        {
            AddText(
                ComputeVisualizationDisplay.UnavailableReason(
                    report.Explanation,
                    "Compute visualization is not available for this document."),
                Brushes.Goldenrod);
            return;
        }

        AddSection("Dispatch geometry");
        AddRows(new[]
        {
            ("Threads per group", ComputeVisualizationDisplay.Dimensions(report.ThreadGroupSize)),
            ("Desired logical workload (total threads/elements)", ComputeVisualizationDisplay.Dimensions(report.DispatchDimensions)),
            ("Derived D3D Dispatch() group count", ComputeVisualizationDisplay.Dimensions(report.GroupCount)),
            ("Launched threads", ComputeVisualizationDisplay.Number(report.LaunchedThreads)),
            ("Inactive edge threads", ComputeVisualizationDisplay.Number(report.InactiveThreads)),
        });
        AddSystemValues(report.SystemValues);
        AddGroupShared(report.GroupShared);
        AddBarriers(report.Barriers);
        AddWaveSize(report.WaveSize);
        AddOccupancy(report.Occupancy, submittedHardwareProfile);
    }

    internal void SetError(string message, bool preserveContent)
    {
        if (hasContent && preserveContent)
        {
            return;
        }
        hasContent = false;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        AddConfiguration();
        AddText(message, Brushes.OrangeRed);
    }

    internal void SetInputError(string message)
        => inputError.Text = message ?? string.Empty;

    private void AddConfiguration()
    {
        content.Children.Add(configuration);
    }

    private void BuildConfiguration()
    {
        AddConfigurationSection("Desired logical workload (total threads/elements)");
        configuration.Children.Add(new TextBlock
        {
            Text = ComputeVisualizationDisplay.LogicalWorkloadHelp,
            Foreground = Brushes.Gray,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 5),
        });
        var dispatch = new StackPanel { Orientation = Orientation.Horizontal };
        dispatch.Children.Add(Label("X"));
        dispatch.Children.Add(dispatchX);
        dispatch.Children.Add(Label("Y"));
        dispatch.Children.Add(dispatchY);
        dispatch.Children.Add(Label("Z"));
        dispatch.Children.Add(dispatchZ);
        configuration.Children.Add(dispatch);

        AddConfigurationSection("Optional hardware profile");
        AddProfileRow("Name", profileName);
        AddProfileRow("Wave size", waveSize);
        AddProfileRow("Max threads / group", maxThreadsPerGroup);
        AddProfileRow("Max threads / compute unit", maxThreadsPerComputeUnit);
        AddProfileRow("Max groups / compute unit", maxGroupsPerComputeUnit);
        AddProfileRow("Shared memory bytes / compute unit", sharedMemoryBytesPerComputeUnit);
        var button = VisualStudioTheme.ApplyButtonStyle(new Button
        {
            Content = "Apply / Refresh",
            HorizontalAlignment = HorizontalAlignment.Left,
            Padding = new Thickness(10, 4, 10, 4),
            Margin = new Thickness(0, 8, 0, 4),
        });
        button.Click += (_, _) =>
        {
            if (!TryReadOptions(out var options, out var error))
            {
                inputError.Text = error;
                return;
            }
            inputError.Text = string.Empty;
            ApplyRequested?.Invoke(options);
        };
        configuration.Children.Add(button);
        configuration.Children.Add(inputError);
    }

    private bool TryReadOptions(out ComputeVisualizationOptions options, out string error)
    {
        options = null;
        error = null;
        var dispatchFieldsPresent =
            !string.IsNullOrWhiteSpace(dispatchX.Text) ||
            !string.IsNullOrWhiteSpace(dispatchY.Text) ||
            !string.IsNullOrWhiteSpace(dispatchZ.Text);
        ComputeDimensionsModel dispatch = null;
        uint x = 0;
        uint y = 0;
        uint z = 0;
        if (dispatchFieldsPresent &&
            (!ComputeVisualizationInput.TryParsePositiveUInt32(dispatchX.Text, out x) ||
             !ComputeVisualizationInput.TryParsePositiveUInt32(dispatchY.Text, out y) ||
             !ComputeVisualizationInput.TryParsePositiveUInt32(dispatchZ.Text, out z)))
        {
            error =
                "Logical workload dimensions must either all be blank or all be positive " +
                "32-bit integers.";
            return false;
        }
        if (dispatchFieldsPresent)
        {
            dispatch = new ComputeDimensionsModel { X = x, Y = y, Z = z };
        }
        var profileFieldsPresent =
            !string.IsNullOrWhiteSpace(profileName.Text) ||
            !string.IsNullOrWhiteSpace(waveSize.Text) ||
            !string.IsNullOrWhiteSpace(maxThreadsPerGroup.Text) ||
            !string.IsNullOrWhiteSpace(maxThreadsPerComputeUnit.Text) ||
            !string.IsNullOrWhiteSpace(maxGroupsPerComputeUnit.Text) ||
            !string.IsNullOrWhiteSpace(sharedMemoryBytesPerComputeUnit.Text);
        ComputeHardwareProfileModel profile = null;
        if (profileFieldsPresent)
        {
            if (string.IsNullOrWhiteSpace(profileName.Text) ||
                !ComputeVisualizationInput.TryParsePositiveUInt32(
                    waveSize.Text,
                    out var parsedWaveSize) ||
                !ComputeVisualizationInput.TryParsePositiveUInt32(
                    maxThreadsPerGroup.Text,
                    out var parsedMaxThreadsPerGroup) ||
                !ComputeVisualizationInput.TryParsePositiveUInt32(
                    maxThreadsPerComputeUnit.Text,
                    out var parsedMaxThreadsPerComputeUnit) ||
                !ComputeVisualizationInput.TryParsePositiveUInt32(
                    maxGroupsPerComputeUnit.Text,
                    out var parsedMaxGroupsPerComputeUnit) ||
                !ComputeVisualizationInput.TryParseOptionalPositiveUInt32(
                    sharedMemoryBytesPerComputeUnit.Text,
                    out var parsedSharedMemory) ||
                parsedSharedMemory == 0)
            {
                error =
                    "A custom hardware profile requires a name; numeric values must be " +
                    "positive integers.";
                return false;
            }
            profile = new ComputeHardwareProfileModel
            {
                Name = profileName.Text.Trim(),
                WaveSize = parsedWaveSize,
                MaxThreadsPerGroup = parsedMaxThreadsPerGroup,
                MaxThreadsPerComputeUnit = parsedMaxThreadsPerComputeUnit,
                MaxGroupsPerComputeUnit = parsedMaxGroupsPerComputeUnit,
                SharedMemoryBytesPerComputeUnit = parsedSharedMemory,
            };
        }
        options = new ComputeVisualizationOptions
        {
            DispatchDimensions = dispatch,
            HardwareProfile = profile,
        };
        return true;
    }

    private void AddSystemValues(IReadOnlyList<ComputeSystemValueModel> values)
    {
        AddSection("System-value mapping");
        if (values == null || values.Count == 0)
        {
            AddText("(no mappings reported)", Brushes.Gray);
            return;
        }
        foreach (var value in values)
        {
            AddText(
                $"{value.Semantic ?? string.Empty} ({value.Name ?? string.Empty}) = " +
                $"{value.Formula ?? string.Empty}\n{value.Description ?? string.Empty}");
        }
    }

    private void AddGroupShared(ComputeGroupSharedAnalysisModel groupShared)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        AddSection("Group-shared memory");
        if (groupShared == null || !groupShared.Available)
        {
            AddText(
                ComputeVisualizationDisplay.UnavailableReason(
                    groupShared?.UnavailableReason,
                    "Group-shared memory analysis is unavailable."),
                Brushes.Goldenrod);
            return;
        }
        AddText(
            "Total estimated bytes: " +
            (groupShared.TotalBytes.HasValue
                ? ComputeVisualizationDisplay.Number(groupShared.TotalBytes)
                : ComputeVisualizationDisplay.UnavailableReason(
                    groupShared.TotalBytesUnavailableReason,
                    "Unavailable")));
        if (groupShared.Truncated)
        {
            AddText("Group-shared declarations were truncated by the server limit.", Brushes.Goldenrod);
        }
        if (groupShared.Declarations == null || groupShared.Declarations.Count == 0)
        {
            AddText("(no group-shared declarations)", Brushes.Gray);
            return;
        }
        foreach (var declaration in groupShared.Declarations)
        {
            AddLocation(
                $"{declaration.Name ?? string.Empty} : {declaration.Type ?? string.Empty} " +
                $"({(declaration.Bytes.HasValue
                    ? ComputeVisualizationDisplay.Number(declaration.Bytes)
                    : ComputeVisualizationDisplay.UnavailableReason(
                        declaration.SizeUnavailableReason,
                        "Unavailable"))} bytes)\n" +
                $"{declaration.Declaration ?? string.Empty}",
                declaration.Uri,
                declaration.Range);
        }
    }

    private void AddBarriers(ComputeBarrierAnalysisModel barriers)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        AddSection("Compiler barriers");
        if (barriers == null || !barriers.Available)
        {
            AddText(
                ComputeVisualizationDisplay.UnavailableReason(
                    barriers?.UnavailableReason,
                    "Barrier analysis is unavailable."),
                Brushes.Goldenrod);
            return;
        }
        AddText(
            $"Compiler instruction count: " +
            $"{ComputeVisualizationDisplay.Number(barriers.InstructionCount)}");
        if (barriers.LocationsTruncated)
        {
            AddText("Barrier locations were truncated by the server limit.", Brushes.Goldenrod);
        }
        var locations = barriers.Locations ?? Array.Empty<ComputeSourceLocationModel>();
        var locationMessage = ComputeVisualizationDisplay.BarrierLocationsMessage(
            barriers.InstructionCount,
            barriers.LocationsAvailable,
            barriers.LocationsUnavailableReason,
            locations.Count);
        if (locationMessage != null)
        {
            AddText(locationMessage, Brushes.Gray);
            return;
        }
        var index = 1;
        foreach (var location in locations)
        {
            AddLocation(location.Label ?? $"Barrier {index++}", location.Uri, location.Range);
        }
    }

    private void AddWaveSize(ComputeWaveSizeModel wave)
    {
        AddSection("Wave size");
        if (wave == null || !wave.Known)
        {
            AddText(
                ComputeVisualizationDisplay.UnavailableReason(
                    wave?.Explanation,
                    "No compiler-authoritative wave-size requirement is available."),
                Brushes.Goldenrod);
            return;
        }
        AddRows(new[]
        {
            ("Minimum", wave.Min?.ToString() ?? "Unavailable"),
            ("Maximum", wave.Max?.ToString() ?? "Unavailable"),
            ("Preferred", wave.Preferred?.ToString() ?? "Unavailable"),
            ("Min/max source", wave.MinMaxSource ?? "Unavailable"),
            ("Preferred source", wave.PreferredSource ?? "Unavailable"),
        });
        if (!string.IsNullOrWhiteSpace(wave.Explanation))
        {
            AddText(wave.Explanation, Brushes.Gray);
        }
    }

    private void AddOccupancy(
        ComputeOccupancyModel occupancy,
        ComputeHardwareProfileModel submittedHardwareProfile)
    {
        AddSection(ComputeVisualizationDisplay.OccupancyHeading);
        if (occupancy == null)
        {
            AddText(
                ComputeVisualizationDisplay.OccupancyUnavailable(submittedHardwareProfile),
                Brushes.Goldenrod);
            return;
        }
        AddText(
            $"Estimate for {occupancy.HardwareProfile ?? "the supplied profile"}; " +
            "this is not compiler- or device-runtime-measured occupancy.",
            Brushes.Goldenrod);
        AddRows(new[]
        {
            ("Resident groups / compute unit", ComputeVisualizationDisplay.Number(occupancy.EstimatedResidentGroups)),
            ("Resident threads / compute unit", ComputeVisualizationDisplay.Number(occupancy.EstimatedResidentThreads)),
            ("Resident waves / compute unit", ComputeVisualizationDisplay.Number(occupancy.EstimatedResidentWaves)),
        });
        AddList("Limiting factors", occupancy.LimitingFactors);
        AddList("Assumptions", occupancy.Assumptions);
    }

    private void AddList(string title, IReadOnlyList<string> items)
    {
        AddText(title, Brushes.Gray);
        if (items == null || items.Count == 0)
        {
            AddText("  (none reported)", Brushes.Gray);
            return;
        }
        foreach (var item in items)
        {
            AddText($"  \u2022 {item}");
        }
    }

    private void AddLocation(string label, string uri, CompilationSourceRangeModel range)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var block = new TextBlock { Margin = new Thickness(0, 2, 0, 2) };
        if (string.IsNullOrWhiteSpace(uri) || !EntryPointDataFlowNavigation.IsWellFormedRange(range))
        {
            block.Text = label;
        }
        else
        {
            var link = new Hyperlink(new Run(label)) { ToolTip = "Go to server-reported location" };
            link.Click += (_, _) =>
            {
                ThreadHelper.ThrowIfNotOnUIThread();
                Navigate(uri, range);
            };
            block.Inlines.Add(link);
        }
        content.Children.Add(block);
    }

    private static void Navigate(string uriValue, CompilationSourceRangeModel range)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!Uri.TryCreate(uriValue, UriKind.Absolute, out var uri) ||
            !uri.IsFile ||
            !File.Exists(uri.LocalPath) ||
            !EntryPointDataFlowNavigation.IsWellFormedRange(range))
        {
            ShowNavigationError("The server-reported compute visualization location is invalid.");
            return;
        }
        try
        {
            VsShellUtilities.OpenDocument(
                ServiceProvider.GlobalProvider,
                uri.LocalPath,
                VSConstants.LOGVIEWID.TextView_guid,
                out _,
                out _,
                out var frame,
                out var view);
            frame?.Show();
            var startLine = checked((int)range.Start.Line);
            var startCharacter = checked((int)range.Start.Character);
            var endLine = checked((int)range.End.Line);
            var endCharacter = checked((int)range.End.Character);
            view?.SetSelection(startLine, startCharacter, endLine, endCharacter);
            view?.EnsureSpanVisible(new TextSpan
            {
                iStartLine = startLine,
                iStartIndex = startCharacter,
                iEndLine = endLine,
                iEndIndex = endCharacter,
            });
        }

        catch (Exception error)
        {
            ShowNavigationError(error.Message);
        }
    }

    private static void ShowNavigationError(string message)
        => VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Compute Visualization",
            OLEMSGICON.OLEMSGICON_WARNING,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);

    private void AddSection(string title)
        => content.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 12, 0, 5),
        });

    private void AddRows(IEnumerable<(string Label, string Value)> rows)
    {
        foreach (var row in rows)
        {
            AddText($"{row.Label}: {row.Value}");
        }
    }

    private void AddText(string value, Brush foreground = null)
    {
        var text = new TextBlock
        {
            Text = value ?? string.Empty,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 2, 0, 2),
        };
        if (foreground != null)
        {
            text.Foreground = foreground;
        }
        content.Children.Add(text);
    }

    private void AddProfileRow(string label, TextBox input)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        row.Children.Add(new TextBlock
        {
            Text = label,
            Width = 210,
            VerticalAlignment = VerticalAlignment.Center,
        });
        row.Children.Add(input);
        configuration.Children.Add(row);
    }

    private void AddConfigurationSection(string title)
        => configuration.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 12, 0, 5),
        });

    private static TextBlock Label(string value)
        => new()
        {
            Text = value,
            Margin = new Thickness(8, 0, 4, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };

    private static TextBox Input(string value, double width = 72)
        => VisualStudioTheme.ApplyTextBoxStyle(new TextBox
        {
            Text = value,
            Width = width,
            Margin = new Thickness(0, 2, 4, 2),
        });
}
