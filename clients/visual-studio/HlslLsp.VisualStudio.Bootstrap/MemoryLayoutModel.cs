using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class MemoryLayoutModel
{
    public EffectiveShaderContextModel Context { get; set; }

    public string Name { get; set; }

    public string Type { get; set; }

    public string Mode { get; set; }

    public long Size { get; set; }

    public long AllocationSize { get; set; }

    public long Alignment { get; set; }

    public IReadOnlyList<MemoryLayoutMemberModel> Members { get; set; } =
        Array.Empty<MemoryLayoutMemberModel>();

    public IReadOnlyList<string> Diagnostics { get; set; } =
        Array.Empty<string>();
}

public sealed class MemoryLayoutMemberModel
{
    public string Name { get; set; }

    public string Type { get; set; }

    public string Kind { get; set; }

    public long Offset { get; set; }

    public long Size { get; set; }

    public long AllocationSize { get; set; }

    public long Alignment { get; set; }

    public long PaddingBefore { get; set; }

    public long? ArrayStride { get; set; }

    public IReadOnlyList<long> ArrayDimensions { get; set; } =
        Array.Empty<long>();

    public long? MatrixStride { get; set; }

    public bool? RowMajor { get; set; }

    public IReadOnlyList<MemoryLayoutMemberModel> Members { get; set; } =
        Array.Empty<MemoryLayoutMemberModel>();
}

internal static class MemoryLayoutDisplayName
{
    internal static string Qualify(
        string parentName,
        string childName,
        string parentKind,
        bool parentRowMajor)
    {
        if (string.IsNullOrEmpty(parentName))
        {
            return childName;
        }

        if (!childName.StartsWith("[", StringComparison.Ordinal))
        {
            return parentName + "." + childName;
        }

        return string.Equals(parentKind, "matrix", StringComparison.Ordinal)
            ? $"{parentName}.{(parentRowMajor ? "row" : "column")}{childName}"
            : parentName + childName;
    }
}

internal static class MemoryLayoutByteScale
{
    internal static IReadOnlyList<long> Labels(long rowStart)
        => new[]
        {
            rowStart,
            rowStart + 4,
            rowStart + 8,
            rowStart + 12,
            rowStart + 16,
        };
}

internal static class MemoryLayoutTrackingBuffer
{
    internal static bool IsCurrent(object liveBuffer, object trackedBuffer)
        => liveBuffer != null && ReferenceEquals(liveBuffer, trackedBuffer);
}

// Serializes explicit target selections with background refresh starts.
// Allocating the request generation while holding the same lock as the
// explicit-request count closes the race where a refresh passed a gate just
// before an explicit selection, then incremented its generation afterward.
internal sealed class MemoryLayoutRefreshGate
{
    private readonly object gate = new();
    private int explicitRequestsInFlight;
    private bool refreshPending;
    private AnalysisFreshnessCause pendingCause = AnalysisFreshnessCause.Unknown;
    private long requestGeneration;

    internal long EnterExplicitRequest()
    {
        lock (gate)
        {
            ++explicitRequestsInFlight;
            return ++requestGeneration;
        }
    }

    internal bool ExitExplicitRequest(out AnalysisFreshnessCause cause)
    {
        lock (gate)
        {
            if (explicitRequestsInFlight > 0)
            {
                --explicitRequestsInFlight;
            }
            if (explicitRequestsInFlight != 0 || !refreshPending)
            {
                cause = AnalysisFreshnessCause.Unknown;
                return false;
            }
            refreshPending = false;
            cause = pendingCause;
            pendingCause = AnalysisFreshnessCause.Unknown;
            return true;
        }
    }

    internal bool ExitExplicitRequest()
        => ExitExplicitRequest(out _);

    internal bool TryBeginBackgroundRefresh(
        AnalysisFreshnessCause cause,
        out long generation)
    {
        lock (gate)
        {
            if (explicitRequestsInFlight != 0)
            {
                refreshPending = true;
                pendingCause =
                    AnalysisFreshnessCausePolicy.Coalesce(pendingCause, cause);
                generation = 0;
                return false;
            }

            generation = ++requestGeneration;
            return true;
        }
    }

    internal bool TryBeginBackgroundRefresh(out long generation)
        => TryBeginBackgroundRefresh(
            AnalysisFreshnessCause.Unknown,
            out generation);

    internal bool IsCurrent(long generation)
    {
        lock (gate)
        {
            return requestGeneration == generation;
        }
    }

    internal void InvalidateForRefresh(
        AnalysisFreshnessCause cause,
        bool replacementPending)
    {
        lock (gate)
        {
            ++requestGeneration;
            if (replacementPending && explicitRequestsInFlight != 0)
            {
                refreshPending = true;
                pendingCause =
                    AnalysisFreshnessCausePolicy.Coalesce(pendingCause, cause);
            }
        }
    }

    internal void InvalidateForRefresh()
        => InvalidateForRefresh(
            AnalysisFreshnessCause.Unknown,
            true);

    internal void InvalidateForRefresh(AnalysisFreshnessCause cause)
        => InvalidateForRefresh(cause, true);
}

public static class MemoryLayoutBridge
{
    private static Func<Uri, int, int, CancellationToken, Task<MemoryLayoutModel>> request;
    private static Action<Uri, int, int> present;

    public static void Register(
        Func<Uri, int, int, CancellationToken, Task<MemoryLayoutModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static void RegisterPresenter(Action<Uri, int, int> handler)
    {
        Volatile.Write(
            ref present,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<MemoryLayoutModel> RequestAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<MemoryLayoutModel>(null)
            : handler(uri, line, character, cancellationToken);
    }

    public static void Show(Uri uri, int line, int character)
        => Volatile.Read(ref present)?.Invoke(uri, line, character);
}
