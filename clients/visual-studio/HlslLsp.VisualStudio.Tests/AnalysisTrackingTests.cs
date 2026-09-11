using System;
using System.Collections.Generic;
using System.Threading;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class AnalysisTrackingTests
{
    [Fact]
    public void TrackingPolicy_DefaultsReusableWindowsToPinned()
    {
        Assert.Equal(
            AnalysisTrackingMode.Pinned,
            AnalysisTrackingPolicy.DefaultMode);
        Assert.Equal(
            AnalysisTrackingMode.FollowActiveShader,
            AnalysisTrackingPolicy.Toggle(AnalysisTrackingMode.Pinned));
        Assert.Equal(
            AnalysisTrackingMode.Pinned,
            AnalysisTrackingPolicy.Toggle(
                AnalysisTrackingMode.FollowActiveShader));
    }

    [Theory]
    [InlineData("Pinned", true, "None")]
    [InlineData("Pinned", false, "None")]
    [InlineData("FollowActiveShader", true, "Retarget")]
    [InlineData("FollowActiveShader", false, "MarkUnavailable")]
    public void TrackingPolicy_RetargetsOnlyFollowingWindows(
        string modeName,
        bool activeShaderAvailable,
        string expectedName)
        => Assert.Equal(
            (AnalysisTrackingAction)Enum.Parse(
                typeof(AnalysisTrackingAction),
                expectedName),
            AnalysisTrackingPolicy.OnActiveViewChanged(
                (AnalysisTrackingMode)Enum.Parse(
                    typeof(AnalysisTrackingMode),
                    modeName),
                activeShaderAvailable));

    [Fact]
    public void TrackingPresentation_ProminentlyNamesModeAndFile()
    {
        var uri = new Uri("file:///C:/project/shader.hlsl");

        var pinned = AnalysisTrackingPolicy.Presentation(
            AnalysisTrackingMode.Pinned,
            uri);
        var following = AnalysisTrackingPolicy.Presentation(
            AnalysisTrackingMode.FollowActiveShader,
            uri);

        Assert.Equal("Pinned: shader.hlsl", pinned.TargetText);
        Assert.Equal("Follow active shader", pinned.ButtonText);
        Assert.Equal("Following: shader.hlsl", following.TargetText);
        Assert.Equal("Pin this shader", following.ButtonText);
        Assert.Contains("shader.hlsl", following.ToolTip);
    }

    [Fact]
    public void ClosedTargetMatching_NormalizesFilePathsAndRejectsOtherTargets()
    {
        var tracked = new Uri("file:///C:/project/shader.hlsl");

        Assert.True(
            AnalysisTrackingPolicy.IsSameDocument(
                tracked,
                new Uri("file:///c:/project/./shader.hlsl")));
        Assert.False(
            AnalysisTrackingPolicy.IsSameDocument(
                tracked,
                new Uri("file:///C:/project/other.hlsl")));
    }

    [Fact]
    public void TrackingBridge_DispatchesModeChangesWithoutDoingRpcInline()
    {
        var requested = new List<(AnalysisViewKind Kind, AnalysisTrackingMode Mode)>();
        AnalysisTrackingBridge.Register(
            (kind, mode) => requested.Add((kind, mode)));

        AnalysisTrackingBridge.RequestMode(
            AnalysisViewKind.MemoryLayout,
            AnalysisTrackingMode.FollowActiveShader);
        AnalysisTrackingBridge.RequestMode(
            AnalysisViewKind.CallHierarchy,
            AnalysisTrackingMode.Pinned);

        Assert.Equal(
            new[]
            {
                (
                    AnalysisViewKind.MemoryLayout,
                    AnalysisTrackingMode.FollowActiveShader),
                (
                    AnalysisViewKind.CallHierarchy,
                    AnalysisTrackingMode.Pinned),
            },
            requested);
    }

    [Fact]
    public void MemoryLayoutRetarget_SupersedesThePreviousCaretGeneration()
    {
        var gate = new MemoryLayoutRefreshGate();
        var oldTarget = gate.EnterExplicitRequest();
        var newTarget = gate.EnterExplicitRequest();

        Assert.False(gate.IsCurrent(oldTarget));
        Assert.True(gate.IsCurrent(newTarget));
        Assert.False(gate.ExitExplicitRequest());
        Assert.False(gate.ExitExplicitRequest());
    }

    [Fact]
    public void CancellingFollowRefresh_DemotesOnlyAnInFlightRefresh()
    {
        var tracker = new AnalysisFreshnessTracker();
        tracker.Succeed();

        tracker.CancelRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        Assert.Equal(AnalysisFreshnessStatus.Current, tracker.State.Status);

        tracker.Begin(AnalysisFreshnessCause.ActiveShaderChange);
        tracker.CancelRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        Assert.Equal(AnalysisFreshnessStatus.Stale, tracker.State.Status);
        Assert.Equal(
            AnalysisFreshnessCause.ActiveShaderChange,
            tracker.State.Cause);
    }

    [Fact]
    public void CancelCurrent_StopsSupersededFollowRequest()
    {
        var cancellation =
            new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        using var ambient = new CancellationTokenSource();
        var request = cancellation.BeginNext(ambient.Token);

        cancellation.CancelCurrent();

        Assert.True(request.IsCancellationRequested);
        Assert.False(ambient.IsCancellationRequested);
    }
}
