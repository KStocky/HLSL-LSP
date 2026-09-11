using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Controls;

namespace HlslLsp.VisualStudio.Bootstrap;

public enum AnalysisFreshnessStatus
{
    Current,
    Refreshing,
    Stale,
    RefreshFailed,
}

public enum AnalysisFreshnessCause
{
    SourceEdit,
    VariantChange,
    ConfigurationChange,
    DisconnectedServer,
    ManualRefresh,
    Unknown,
}

public sealed class AnalysisFreshnessState
{
    public AnalysisFreshnessState(
        AnalysisFreshnessStatus status,
        AnalysisFreshnessCause cause)
    {
        Status = status;
        Cause = cause;
    }

    public AnalysisFreshnessStatus Status { get; }

    public AnalysisFreshnessCause Cause { get; }
}

public enum AnalysisFreshnessEventKind
{
    RefreshStarted,
    RefreshSucceeded,
    RefreshFailed,
    Invalidated,
}

public sealed class AnalysisFreshnessEvent
{
    public AnalysisFreshnessEvent(
        AnalysisFreshnessEventKind kind,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown,
        bool refreshPending = false)
    {
        Kind = kind;
        Cause = cause;
        RefreshPending = refreshPending;
    }

    public AnalysisFreshnessEventKind Kind { get; }

    public AnalysisFreshnessCause Cause { get; }

    public bool RefreshPending { get; }
}

public static class AnalysisFreshnessReducer
{
    public static AnalysisFreshnessState Initial { get; } =
        new(AnalysisFreshnessStatus.Stale, AnalysisFreshnessCause.Unknown);

    public static AnalysisFreshnessState Reduce(
        AnalysisFreshnessState state,
        AnalysisFreshnessEvent freshnessEvent)
    {
        if (state == null)
        {
            throw new ArgumentNullException(nameof(state));
        }

        if (freshnessEvent == null)
        {
            throw new ArgumentNullException(nameof(freshnessEvent));
        }
        return freshnessEvent.Kind switch
        {
            AnalysisFreshnessEventKind.RefreshStarted =>
                new AnalysisFreshnessState(
                    AnalysisFreshnessStatus.Refreshing,
                    freshnessEvent.Cause),
            AnalysisFreshnessEventKind.RefreshSucceeded =>
                new AnalysisFreshnessState(
                    AnalysisFreshnessStatus.Current,
                    AnalysisFreshnessCause.Unknown),
            AnalysisFreshnessEventKind.RefreshFailed =>
                new AnalysisFreshnessState(
                    AnalysisFreshnessStatus.RefreshFailed,
                    freshnessEvent.Cause == AnalysisFreshnessCause.Unknown
                        ? state.Cause
                        : freshnessEvent.Cause),
            AnalysisFreshnessEventKind.Invalidated =>
                new AnalysisFreshnessState(
                    freshnessEvent.RefreshPending &&
                    state.Status == AnalysisFreshnessStatus.Refreshing
                        ? AnalysisFreshnessStatus.Refreshing
                        : AnalysisFreshnessStatus.Stale,
                    freshnessEvent.Cause),
            _ => throw new ArgumentOutOfRangeException(nameof(freshnessEvent)),
        };
    }

    public static string Text(AnalysisFreshnessState state)
    {
        var status = state.Status switch
        {
            AnalysisFreshnessStatus.Current => "Current",
            AnalysisFreshnessStatus.Refreshing => "Refreshing",
            AnalysisFreshnessStatus.Stale => "Stale",
            AnalysisFreshnessStatus.RefreshFailed => "Refresh failed",
            _ => throw new ArgumentOutOfRangeException(nameof(state)),
        };
        if (state.Status == AnalysisFreshnessStatus.Current ||
            state.Cause == AnalysisFreshnessCause.Unknown)
        {
            return status;
        }
        var cause = state.Cause switch
        {
            AnalysisFreshnessCause.SourceEdit => "Source edit",
            AnalysisFreshnessCause.VariantChange => "Variant change",
            AnalysisFreshnessCause.ConfigurationChange => "Configuration change",
            AnalysisFreshnessCause.DisconnectedServer => "Disconnected server",
            AnalysisFreshnessCause.ManualRefresh => "Manual refresh",
            AnalysisFreshnessCause.Unknown => "Unknown",
            _ => throw new ArgumentOutOfRangeException(nameof(state)),
        };
        return status + " · " + cause;
    }
}

internal static class AnalysisRefreshCancellation
{
    internal static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(30);

    internal static CancellationTokenSource CreateLinked(
        CancellationToken ambientCancellationToken,
        TimeSpan? timeout = null)
    {
        var cancellation =
            CancellationTokenSource.CreateLinkedTokenSource(
                ambientCancellationToken);
        cancellation.CancelAfter(timeout ?? DefaultTimeout);
        return cancellation;
    }
}

internal static class AnalysisFreshnessCausePolicy
{
    // A coalesced refresh reports the most actionable invalidation reason,
    // independent of trigger order: disconnect > variant > configuration >
    // source edit > manual refresh > unknown.
    internal static AnalysisFreshnessCause Coalesce(
        AnalysisFreshnessCause current,
        AnalysisFreshnessCause incoming)
        => Priority(incoming) > Priority(current) ? incoming : current;

    private static int Priority(AnalysisFreshnessCause cause)
        => cause switch
        {
            AnalysisFreshnessCause.DisconnectedServer => 6,
            AnalysisFreshnessCause.VariantChange => 5,
            AnalysisFreshnessCause.ConfigurationChange => 4,
            AnalysisFreshnessCause.SourceEdit => 3,
            AnalysisFreshnessCause.ManualRefresh => 2,
            AnalysisFreshnessCause.Unknown => 1,
            _ => throw new ArgumentOutOfRangeException(nameof(cause)),
        };
}

internal sealed class AnalysisFreshnessTracker
{
    internal AnalysisFreshnessState State { get; private set; } =
        AnalysisFreshnessReducer.Initial;

    internal void Begin(AnalysisFreshnessCause cause)
        => State = AnalysisFreshnessReducer.Reduce(
            State,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.RefreshStarted,
                cause));

    internal void Succeed()
        => State = AnalysisFreshnessReducer.Reduce(
            State,
            new AnalysisFreshnessEvent(AnalysisFreshnessEventKind.RefreshSucceeded));

    internal void Fail(AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
        => State = AnalysisFreshnessReducer.Reduce(
            State,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.RefreshFailed,
                cause));

    internal void Invalidate(
        AnalysisFreshnessCause cause,
        bool refreshPending = false)
        => State = AnalysisFreshnessReducer.Reduce(
            State,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.Invalidated,
                cause,
                refreshPending));

    internal void Set(AnalysisFreshnessState state)
        => State = state ?? throw new ArgumentNullException(nameof(state));
}

internal sealed class AnalysisRefreshGate
{
    private readonly object gate = new();
    private int explicitRequests;
    private bool refreshPending;
    private AnalysisFreshnessCause pendingCause = AnalysisFreshnessCause.Unknown;

    internal void EnterExplicitRequest()
    {
        lock (gate)
        {
            ++explicitRequests;
        }
    }

    internal bool ExitExplicitRequest(out AnalysisFreshnessCause cause)
    {
        lock (gate)
        {
            if (explicitRequests > 0)
            {
                --explicitRequests;
            }
            if (explicitRequests != 0 || !refreshPending)
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

    internal bool TryBeginBackgroundRefresh(AnalysisFreshnessCause cause)
    {
        lock (gate)
        {
            if (explicitRequests == 0)
            {
                return true;
            }
            refreshPending = true;
            pendingCause =
                AnalysisFreshnessCausePolicy.Coalesce(pendingCause, cause);
            return false;
        }
    }

    internal void RecordRefreshNeeded(AnalysisFreshnessCause cause)
    {
        lock (gate)
        {
            if (explicitRequests != 0)
            {
                refreshPending = true;
                pendingCause =
                    AnalysisFreshnessCausePolicy.Coalesce(pendingCause, cause);
            }
        }
    }
}

internal enum AnalysisViewKind
{
    MemoryLayout,
    CompilationInfo,
    ResourceBindings,
    PreprocessorExplorer,
    EntryPointDataFlow,
    ComputeVisualization,
    CallHierarchy,
}

internal static class AnalysisSaveRefreshPolicy
{
    internal static bool ShouldRefresh(
        AnalysisViewKind view,
        Uri trackedUri,
        string savedFilePath,
        IEnumerable<string> configuredExtensions)
    {
        if (savedFilePath == null)
        {
            return true;
        }
        return view switch
        {
            AnalysisViewKind.CompilationInfo or
            AnalysisViewKind.ResourceBindings or
            AnalysisViewKind.PreprocessorExplorer =>
                IsTrackedDocument(trackedUri, savedFilePath),
            AnalysisViewKind.MemoryLayout or
            AnalysisViewKind.EntryPointDataFlow or
            AnalysisViewKind.ComputeVisualization or
            AnalysisViewKind.CallHierarchy =>
                IsHlslOrConfiguration(savedFilePath, configuredExtensions),
            _ => throw new ArgumentOutOfRangeException(nameof(view)),
        };
    }

    private static bool IsTrackedDocument(Uri trackedUri, string savedFilePath)
        => trackedUri != null &&
           Uri.TryCreate(savedFilePath, UriKind.Absolute, out var savedUri) &&
           savedUri.IsFile &&
           trackedUri.Equals(savedUri);

    private static bool IsHlslOrConfiguration(
        string path,
        IEnumerable<string> configuredExtensions)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return false;
        }
        if (string.Equals(
                Path.GetFileName(path),
                "shadertoolsconfig.json",
                StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        var extension = Path.GetExtension(path);
        if (string.Equals(extension, ".hlsl", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(extension, ".hlsli", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (configuredExtensions == null)
        {
            return false;
        }
        foreach (var configuredExtension in configuredExtensions)
        {
            if (string.Equals(
                    extension,
                    configuredExtension,
                    StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }
}

internal static class AnalysisFreshnessBridge
{
    private static Action<AnalysisViewKind> refresh;

    internal static void Register(Action<AnalysisViewKind> handler)
        => Volatile.Write(
            ref refresh,
            handler ?? throw new ArgumentNullException(nameof(handler)));

    internal static void RequestRefresh(AnalysisViewKind kind)
        => Volatile.Read(ref refresh)?.Invoke(kind);
}

internal sealed class AnalysisFreshnessHeader : UserControl
{
    private readonly TextBlock status = new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        Opacity = 0.75,
    };

    internal AnalysisFreshnessHeader(AnalysisViewKind kind)
    {
        VisualStudioTheme.ApplyToolWindowTheme(this);
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        row.Children.Add(status);
        row.Children.Add(new TextBlock
        {
            Text = "  ·  ",
            VerticalAlignment = VerticalAlignment.Center,
            Opacity = 0.55,
        });
        var refreshButton = VisualStudioTheme.ApplyButtonStyle(new Button
        {
            Content = "Refresh",
            Padding = new Thickness(7, 1, 7, 1),
        });
        refreshButton.Click += (_, _) =>
            AnalysisFreshnessBridge.RequestRefresh(kind);
        row.Children.Add(refreshButton);
        Content = new Border
        {
            Padding = new Thickness(12, 6, 12, 6),
            Child = row,
        };
        Update(AnalysisFreshnessReducer.Initial);
    }

    internal void Update(AnalysisFreshnessState state)
        => status.Text = AnalysisFreshnessReducer.Text(state);

    internal static UIElement Wrap(
        AnalysisFreshnessHeader header,
        UIElement content)
    {
        var root = new DockPanel();
        DockPanel.SetDock(header, Dock.Top);
        root.Children.Add(header);
        root.Children.Add(content);
        return root;
    }
}

internal interface IAnalysisFreshnessView
{
    void BeginRefresh(AnalysisFreshnessCause cause);

    void MarkStale(AnalysisFreshnessCause cause, bool refreshPending = false);
}
