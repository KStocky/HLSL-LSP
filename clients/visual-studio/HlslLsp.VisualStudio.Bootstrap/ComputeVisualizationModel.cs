using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class ComputeDimensionsModel
{
    public uint X { get; set; }
    public uint Y { get; set; }
    public uint Z { get; set; }
}

public sealed class ComputeHardwareProfileModel
{
    public string Name { get; set; }
    public uint WaveSize { get; set; }
    public uint MaxThreadsPerGroup { get; set; }
    public uint MaxThreadsPerComputeUnit { get; set; }
    public uint MaxGroupsPerComputeUnit { get; set; }
    public ulong SharedMemoryBytesPerComputeUnit { get; set; }
}

public sealed class ComputeVisualizationOptions
{
    // Desired logical workload in total threads/elements. This is not the
    // group-count argument passed to D3D Dispatch(); the server derives that
    // count with ceil(dispatchDimensions / numthreads).
    public ComputeDimensionsModel DispatchDimensions { get; set; }
    public ComputeHardwareProfileModel HardwareProfile { get; set; }
}

public sealed class ComputeSystemValueModel
{
    public string Semantic { get; set; }
    public string Name { get; set; }
    public string Formula { get; set; }
    public string Description { get; set; }
}

public sealed class ComputeSourceLocationModel
{
    public string Label { get; set; }
    public string Uri { get; set; }
    public CompilationSourceRangeModel Range { get; set; }
}

public sealed class ComputeBarrierAnalysisModel
{
    public bool Available { get; set; }
    public string UnavailableReason { get; set; }
    public ulong? InstructionCount { get; set; }
    public IReadOnlyList<ComputeSourceLocationModel> Locations { get; set; } =
        Array.Empty<ComputeSourceLocationModel>();
}

public sealed class ComputeGroupSharedDeclarationModel
{
    public string Name { get; set; }
    public string Type { get; set; }
    public ulong? SizeBytes { get; set; }
    public string Uri { get; set; }
    public CompilationSourceRangeModel Range { get; set; }
}

public sealed class ComputeGroupSharedAnalysisModel
{
    public bool Available { get; set; }
    public string UnavailableReason { get; set; }
    public ulong? TotalBytes { get; set; }
    public IReadOnlyList<ComputeGroupSharedDeclarationModel> Declarations { get; set; } =
        Array.Empty<ComputeGroupSharedDeclarationModel>();
}

public sealed class ComputeWaveSizeModel
{
    public bool Known { get; set; }
    public uint? Min { get; set; }
    public uint? Max { get; set; }
    public uint? Preferred { get; set; }
    public string Explanation { get; set; }
}

public sealed class ComputeOccupancyModel
{
    public string HardwareProfile { get; set; }
    public ulong? EstimatedResidentGroups { get; set; }
    public ulong? EstimatedResidentThreads { get; set; }
    public ulong? EstimatedResidentWaves { get; set; }
    public IReadOnlyList<string> LimitingFactors { get; set; } = Array.Empty<string>();
    public IReadOnlyList<string> Assumptions { get; set; } = Array.Empty<string>();
}

public sealed class ComputeVisualizationModel
{
    public bool Applicable { get; set; }
    public string Explanation { get; set; }
    public string EntryPoint { get; set; }
    public string Stage { get; set; }
    public string TargetProfile { get; set; }
    public ComputeDimensionsModel ThreadGroupSize { get; set; }
    public ComputeDimensionsModel DispatchDimensions { get; set; }
    public ComputeDimensionsModel GroupCount { get; set; }
    public ulong? LaunchedThreads { get; set; }
    public ulong? InactiveThreads { get; set; }
    public IReadOnlyList<ComputeSystemValueModel> SystemValues { get; set; } =
        Array.Empty<ComputeSystemValueModel>();
    public ComputeBarrierAnalysisModel Barriers { get; set; }
    public ComputeGroupSharedAnalysisModel GroupShared { get; set; }
    public ComputeWaveSizeModel WaveSize { get; set; }
    public ComputeOccupancyModel Occupancy { get; set; }
}

internal static class ComputeVisualizationDisplay
{
    internal const string LogicalWorkloadHelp =
        "Desired logical workload in total threads/elements, not D3D Dispatch() " +
        "group counts. The server derives group counts using " +
        "ceil(workload / numthreads).";

    internal static string Dimensions(ComputeDimensionsModel value)
        => value == null ? "Unavailable" : $"{value.X} x {value.Y} x {value.Z}";

    internal static string Number(ulong? value)
        => value?.ToString("N0", System.Globalization.CultureInfo.InvariantCulture)
           ?? "Unavailable";

    internal static string UnavailableReason(string reason, string fallback)
        => string.IsNullOrWhiteSpace(reason) ? fallback : reason;

    internal static string OccupancyHeading => "Hardware-dependent occupancy estimate";

    internal static string OccupancyUnavailable(ComputeHardwareProfileModel profile)
        => profile == null
            ? "No hardware profile was supplied. Occupancy is intentionally not guessed."
            : "The server could not produce an occupancy estimate for the supplied hardware profile.";

    internal static string BarrierLocationsMessage(
        ulong? instructionCount,
        int locationCount)
    {
        if (instructionCount == 0)
        {
            return "(no barrier instructions)";
        }
        if (locationCount == 0)
        {
            return instructionCount.HasValue
                ? "Barrier instructions were found, but compiler source locations are unavailable."
                : "Barrier instruction count and compiler source locations are unavailable.";
        }
        return null;
    }
}

internal static class ComputeVisualizationInput
{
    internal static bool TryParsePositiveUInt32(string value, out uint parsed)
        => uint.TryParse(
               value,
               System.Globalization.NumberStyles.None,
               System.Globalization.CultureInfo.InvariantCulture,
               out parsed) &&
           parsed != 0;

    internal static bool TryParseOptionalPositiveUInt32(string value, out uint parsed)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            parsed = 0;
            return true;
        }
        return TryParsePositiveUInt32(value, out parsed);
    }

    internal static bool TryParseOptionalPositiveUInt64(string value, out ulong parsed)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            parsed = 0;
            return true;
        }
        return ulong.TryParse(
                   value,
                   System.Globalization.NumberStyles.None,
                   System.Globalization.CultureInfo.InvariantCulture,
                   out parsed) &&
               parsed != 0;
    }
}

public static class ComputeVisualizationBridge
{
    private static Func<
        Uri,
        ComputeVisualizationOptions,
        CancellationToken,
        Task<ComputeVisualizationModel>> request;
    private static Action<Uri, ComputeVisualizationOptions> presenter;

    public static void Register(
        Func<
            Uri,
            ComputeVisualizationOptions,
            CancellationToken,
            Task<ComputeVisualizationModel>> handler)
        => Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));

    public static Task<ComputeVisualizationModel> RequestAsync(
        Uri uri,
        ComputeVisualizationOptions options,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<ComputeVisualizationModel>(null)
            : handler(uri, options ?? new ComputeVisualizationOptions(), cancellationToken);
    }

    public static void RegisterPresenter(Action<Uri, ComputeVisualizationOptions> handler)
        => Volatile.Write(
            ref presenter,
            handler ?? throw new ArgumentNullException(nameof(handler)));

    public static void Show(Uri uri, ComputeVisualizationOptions options)
        => Volatile.Read(ref presenter)?.Invoke(uri, options ?? new ComputeVisualizationOptions());
}

internal static class ComputeVisualizationRefreshLogic
{
    internal static ComputeVisualizationOptions OptionsForBackgroundRefresh(
        ComputeVisualizationOptions current)
        => current ?? new ComputeVisualizationOptions();

    internal static bool ShouldPreserveContentOnFailure(Uri priorDocumentUri, Uri requestedUri)
        => EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
            priorDocumentUri,
            requestedUri);

    internal static bool ShouldRevealToolWindow(bool existingWindowSupplied)
        => EntryPointDataFlowRefreshLogic.ShouldRevealToolWindow(existingWindowSupplied);

    internal static bool IsHlslOrConfigRelevantPath(
        string path,
        IEnumerable<string> configuredExtensions)
        => EntryPointDataFlowRefreshLogic.IsHlslOrConfigRelevantPath(
            path,
            configuredExtensions);
}

internal sealed class ComputeVisualizationInteractionState
{
    internal ComputeVisualizationInteractionState()
    {
        LastSubmittedOptions = new ComputeVisualizationOptions
        {
            DispatchDimensions = new ComputeDimensionsModel { X = 1, Y = 1, Z = 1 },
        };
    }

    internal Uri RequestedDocumentUri { get; private set; }

    internal Uri DisplayedDocumentUri { get; private set; }

    internal ComputeVisualizationOptions LastSubmittedOptions { get; private set; }

    internal void TrackRequest(Uri uri)
        => RequestedDocumentUri = uri ?? throw new ArgumentNullException(nameof(uri));

    internal void MarkDisplayed(Uri uri)
    {
        TrackRequest(uri);
        DisplayedDocumentUri = uri;
    }

    internal void Submit(ComputeVisualizationOptions options)
        => LastSubmittedOptions =
            options ?? throw new ArgumentNullException(nameof(options));

    internal bool ShouldPreserveDisplayedContentOnFailure(Uri requestedUri)
        => ComputeVisualizationRefreshLogic.ShouldPreserveContentOnFailure(
            DisplayedDocumentUri,
            requestedUri);
}
