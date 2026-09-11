using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio;
using HlslLsp.VisualStudio.Bootstrap;
using Nerdbank.Streams;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

// Coverage for the custom Call Hierarchy surface added to correct the
// HIGH-severity architectural finding that Visual Studio 17.14's generic
// ILanguageClient does not route the editor's built-in View Call Hierarchy
// command to any language client (see clients/visual-studio/README.md and
// docs/call-hierarchy.md). HlslCallHierarchyExplorerToolWindow/
// HlslBootstrapPackage's Show/Refresh/drill-in methods extend
// ToolWindowPane/AsyncPackage and cannot be constructed in a test host, so
// coverage here targets the pure/testable layer this feature was
// deliberately factored into: CallHierarchyBridge (protocol dispatch),
// CallHierarchyItemModel/CallHierarchyIncomingCallModel/
// CallHierarchyOutgoingCallModel (deserialization), CallHierarchyExplorerDisplay
// (display-text helpers), CallHierarchyExplorerState (the navigation-stack
// logic backing drill-in/back), and HlslLanguageClient's three new RPC
// methods exercised through a real StreamJsonRpc connection (mirroring
// EntryPointDataFlowModelTests' protocol tests).
public sealed class CallHierarchyExplorerModelTests
{
    // Matches docs/call-hierarchy.md's prepareCallHierarchy response sample
    // verbatim.
    private const string SampleItemJson = @"
{
  ""name"": ""square"",
  ""kind"": 12,
  ""detail"": ""float square(float x)"",
  ""uri"": ""file:///C:/shaders/example.hlsl"",
  ""range"": { ""start"": { ""line"": 3, ""character"": 0 }, ""end"": { ""line"": 3, ""character"": 40 } },
  ""selectionRange"": { ""start"": { ""line"": 3, ""character"": 6 }, ""end"": { ""line"": 3, ""character"": 12 } },
  ""data"": {
    ""rootUri"": ""file:///C:/shaders/example.hlsl"",
    ""rootIdentity"": ""abc"",
    ""rootVersion"": 1,
    ""generation"": 3,
    ""path"": ""C:/shaders/example.hlsl"",
    ""line"": 4,
    ""column"": 7,
    ""startOffset"": 87,
    ""cursorKind"": 21,
    ""name"": ""square""
  }
}";

    private const string SampleCallerItemJson = @"
{
  ""name"": ""main"",
  ""kind"": 12,
  ""detail"": ""float4 main() : SV_Target"",
  ""uri"": ""file:///C:/shaders/example.hlsl"",
  ""range"": { ""start"": { ""line"": 10, ""character"": 0 }, ""end"": { ""line"": 14, ""character"": 1 } },
  ""selectionRange"": { ""start"": { ""line"": 10, ""character"": 8 }, ""end"": { ""line"": 10, ""character"": 12 } },
  ""data"": { ""rootUri"": ""file:///C:/shaders/example.hlsl"", ""generation"": 3, ""name"": ""main"" }
}";

    [Fact]
    public void Deserialize_CallHierarchyItem_MapsEveryDocumentedFieldIncludingOpaqueData()
    {
        var item = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);

        Assert.Equal("square", item.Name);
        Assert.Equal(12, item.Kind);
        Assert.Equal("float square(float x)", item.Detail);
        Assert.Equal("file:///C:/shaders/example.hlsl", item.Uri);
        Assert.Equal(3, item.Range.Start.Line);
        Assert.Equal(40, item.Range.End.Character);
        Assert.Equal(6, item.SelectionRange.Start.Character);
        Assert.Equal(12, item.SelectionRange.End.Character);
        Assert.IsAssignableFrom<JToken>(item.Data);
        Assert.Equal("abc", item.Data["rootIdentity"]?.Value<string>());
        Assert.Equal(3, item.Data["generation"]?.Value<int>());
        Assert.Equal(21, item.Data["cursorKind"]?.Value<int>());
    }

    [Fact]
    public void Deserialize_IncomingCall_MapsFromItemAndFromRanges()
    {
        var json = @"
{
  ""from"": " + SampleCallerItemJson + @",
  ""fromRanges"": [
    { ""start"": { ""line"": 11, ""character"": 10 }, ""end"": { ""line"": 11, ""character"": 16 } },
    { ""start"": { ""line"": 13, ""character"": 4 }, ""end"": { ""line"": 13, ""character"": 10 } }
  ]
}";

        var call = JsonConvert.DeserializeObject<CallHierarchyIncomingCallModel>(json);

        Assert.Equal("main", call.From.Name);
        Assert.Equal(2, call.FromRanges.Count);
        Assert.Equal(11, call.FromRanges[0].Start.Line);
        Assert.Equal(13, call.FromRanges[1].Start.Line);
    }

    [Fact]
    public void Deserialize_OutgoingCall_MapsToItemAndFromRanges()
    {
        var json = @"
{
  ""to"": " + SampleItemJson + @",
  ""fromRanges"": [
    { ""start"": { ""line"": 12, ""character"": 2 }, ""end"": { ""line"": 12, ""character"": 8 } }
  ]
}";

        var call = JsonConvert.DeserializeObject<CallHierarchyOutgoingCallModel>(json);

        Assert.Equal("square", call.To.Name);
        Assert.Single(call.FromRanges);
        Assert.Equal(12, call.FromRanges[0].Start.Line);
    }

    [Fact]
    public void Deserialize_IncomingCall_MissingFromRangesDefaultsToEmptyNotNull()
    {
        var json = @"{ ""from"": " + SampleCallerItemJson + @" }";

        var call = JsonConvert.DeserializeObject<CallHierarchyIncomingCallModel>(json);

        Assert.NotNull(call.FromRanges);
        Assert.Empty(call.FromRanges);
    }

    // --- CallHierarchyExplorerDisplay -----------------------------------

    [Fact]
    public void Display_NoCallableMessage_MentionsContextCommand()
        => Assert.Contains("HLSL > Call Hierarchy", CallHierarchyExplorerDisplay.NoCallableMessage());

    [Theory]
    [InlineData(0, "")]
    [InlineData(-1, "")]
    [InlineData(1, "1 call site")]
    [InlineData(2, "2 call sites")]
    [InlineData(5, "5 call sites")]
    public void Display_CallSiteCountLabel_PluralizesCorrectly(int count, string expected)
        => Assert.Equal(expected, CallHierarchyExplorerDisplay.CallSiteCountLabel(count));

    [Fact]
    public void Display_StaleItemMessage_AdvisesRerunningTheCommand()
        => Assert.Contains("HLSL > Call Hierarchy", CallHierarchyExplorerDisplay.StaleItemMessage());

    [Fact]
    public void Display_RequestFailedMessage_IncludesDetailWhenPresent()
        => Assert.Equal(
            "Could not retrieve call hierarchy information: boom",
            CallHierarchyExplorerDisplay.RequestFailedMessage("boom"));

    [Fact]
    public void Display_RequestFailedMessage_OmitsColonWhenDetailIsEmpty()
        => Assert.Equal(
            "Could not retrieve call hierarchy information.",
            CallHierarchyExplorerDisplay.RequestFailedMessage(string.Empty));

    // --- CallHierarchyExplorerState (drill-in/back navigation stack) ----

    private static CallHierarchyItemModel MakeItem(string name)
        => new() { Name = name, Kind = 12 };

    private static CallHierarchyFrame MakeFrame(string name)
        => new(
            MakeItem(name),
            Array.Empty<CallHierarchyIncomingCallModel>(),
            Array.Empty<CallHierarchyOutgoingCallModel>());

    private static CallHierarchyPathStep MakeStep(
        CallHierarchySection section = CallHierarchySection.Incoming,
        string path = "C:/shaders/example.hlsl",
        long startOffset = 87,
        long cursorKind = 21)
        => new(section, path, startOffset, cursorKind);

    [Fact]
    public void State_StartsEmptyWithNoCurrentFrameAndCannotGoBack()
    {
        var state = new CallHierarchyExplorerState();

        Assert.Null(state.Current);
        Assert.False(state.CanGoBack);
        Assert.Equal(0, state.Depth);
    }

    [Fact]
    public void State_Reset_EstablishesANewRootAndDiscardsAnyPriorStack()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());

        state.Reset(MakeFrame("newRoot"));

        Assert.Equal("newRoot", state.Current.Item.Name);
        Assert.Equal(1, state.Depth);
        Assert.False(state.CanGoBack);
        Assert.Empty(state.CapturePath());
    }

    [Fact]
    public void FailedNewRootRequest_PreservesDisplayedRootAsRefreshAndDrillInTarget()
    {
        var displayed = (
            DocumentUri: new Uri("file:///displayed.hlsl"),
            Line: 3,
            Character: 7);
        var requested = (
            DocumentUri: new Uri("file:///requested.hlsl"),
            Line: 12,
            Character: 4);

        var tracked =
            CallHierarchyRootRefreshPolicy.TrackedTargetAfterFailedReplacement(
                displayed,
                requested);
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("displayedRoot"));
        state.Push(MakeFrame("displayedChild"), MakeStep());

        Assert.Equal(displayed, tracked);
        Assert.Equal("displayedRoot", state.Root.Item.Name);
        Assert.Equal("displayedChild", state.Current.Item.Name);
    }

    [Fact]
    public void State_Push_AddsAFrameOnTopAndEnablesGoBack()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));

        state.Push(MakeFrame("callee"), MakeStep());

        Assert.Equal("callee", state.Current.Item.Name);
        Assert.Equal(2, state.Depth);
        Assert.True(state.CanGoBack);
        Assert.Single(state.CapturePath());
    }

    [Fact]
    public void State_Push_BeforeARootExistsThrows()
    {
        var state = new CallHierarchyExplorerState();

        Assert.Throws<InvalidOperationException>(
            () => state.Push(MakeFrame("callee"), MakeStep()));
    }

    [Fact]
    public void State_GoBack_PopsToThePreviousFrameWithoutANewRequest()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());

        var wentBack = state.GoBack();

        Assert.True(wentBack);
        Assert.Equal("root", state.Current.Item.Name);
        Assert.False(state.CanGoBack);
        Assert.Empty(state.CapturePath());
    }

    [Fact]
    public void State_GoBack_AtTheRootReturnsFalseAndLeavesTheRootInPlace()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));

        var wentBack = state.GoBack();

        Assert.False(wentBack);
        Assert.Equal("root", state.Current.Item.Name);
    }

    // A background refresh (variant change / save) replaces the current
    // frame's content in place, without changing the drill-in stack depth
    // -- the user's position in the hierarchy must survive a refresh.
    [Fact]
    public void State_ReplaceCurrent_UpdatesTheTopFrameWithoutChangingDepth()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());

        state.ReplaceCurrent(MakeFrame("calleeRefreshed"));

        Assert.Equal("calleeRefreshed", state.Current.Item.Name);
        Assert.Equal(2, state.Depth);
        Assert.True(state.CanGoBack);
    }

    [Fact]
    public void State_ReplaceCurrent_BeforeARootExistsThrows()
    {
        var state = new CallHierarchyExplorerState();

        Assert.Throws<InvalidOperationException>(
            () => state.ReplaceCurrent(MakeFrame("root")));
    }

    [Fact]
    public void State_Clear_RemovesEveryFrame()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());

        state.Clear();

        Assert.Null(state.Current);
        Assert.Equal(0, state.Depth);
        Assert.Empty(state.CapturePath());
    }

    [Fact]
    public void State_CapturePath_IsEmptyForARootOnlyStack()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));

        Assert.Empty(state.CapturePath());
    }

    [Fact]
    public void State_CapturePath_ReflectsEachPushedStepInOrder()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        var firstStep = MakeStep(CallHierarchySection.Outgoing, "a.hlsl", 1, 2);
        var secondStep = MakeStep(CallHierarchySection.Incoming, "b.hlsl", 3, 4);

        state.Push(MakeFrame("callee1"), firstStep);
        state.Push(MakeFrame("callee2"), secondStep);

        var path = state.CapturePath();
        Assert.Equal(2, path.Count);
        Assert.Same(firstStep, path[0]);
        Assert.Same(secondStep, path[1]);
    }

    [Fact]
    public void State_GoBack_AlsoPopsTheCorrespondingPathStep()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee1"), MakeStep());
        state.Push(MakeFrame("callee2"), MakeStep());

        state.GoBack();

        Assert.Single(state.CapturePath());
    }

    [Fact]
    public void State_ReplaceAll_ReplacesTheEntireStackAndPathAtomically()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("stale"), MakeStep());

        var freshStep = MakeStep(CallHierarchySection.Outgoing, "fresh.hlsl", 5, 6);
        state.ReplaceAll(
            new[] { MakeFrame("freshRoot"), MakeFrame("freshChild") },
            new[] { freshStep });

        Assert.Equal(2, state.Depth);
        Assert.Equal("freshChild", state.Current.Item.Name);
        Assert.True(state.CanGoBack);
        Assert.Single(state.CapturePath());
        Assert.Same(freshStep, state.CapturePath()[0]);
    }

    [Fact]
    public void State_ReplaceAll_AcceptsNullStepsForARootOnlyRebuild()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("stale"), MakeStep());

        state.ReplaceAll(new[] { MakeFrame("freshRoot") }, null);

        Assert.Equal(1, state.Depth);
        Assert.False(state.CanGoBack);
        Assert.Empty(state.CapturePath());
    }

    [Fact]
    public void State_ReplaceAll_RejectsNullOrEmptyFrames()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));

        Assert.Throws<ArgumentException>(() => state.ReplaceAll(null, null));
        Assert.Throws<ArgumentException>(
            () => state.ReplaceAll(Array.Empty<CallHierarchyFrame>(), null));
    }

    [Fact]
    public void State_Root_IsNullBeforeAnyRootIsEstablished()
    {
        var state = new CallHierarchyExplorerState();

        Assert.Null(state.Root);
    }

    [Fact]
    public void State_Root_ReturnsTheBottomOfStackFrameEvenAfterDrillingIn()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee1"), MakeStep());
        state.Push(MakeFrame("callee2"), MakeStep());

        Assert.Equal("root", state.Root.Item.Name);
        Assert.Equal("callee2", state.Current.Item.Name);
    }

    [Fact]
    public void State_Root_TracksTheFreshRootAfterReplaceAll()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("stale"), MakeStep());

        state.ReplaceAll(
            new[] { MakeFrame("freshRoot"), MakeFrame("freshChild") },
            new[] { MakeStep() });

        Assert.Equal("freshRoot", state.Root.Item.Name);
    }

    [Fact]
    public void State_Revision_StartsAtZero()
    {
        var state = new CallHierarchyExplorerState();

        Assert.Equal(0, state.Revision);
    }

    [Fact]
    public void State_Revision_IsBumpedByReset()
    {
        var state = new CallHierarchyExplorerState();

        state.Reset(MakeFrame("root"));

        Assert.Equal(1, state.Revision);
    }

    [Fact]
    public void State_Revision_IsBumpedByPush()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        var before = state.Revision;

        state.Push(MakeFrame("callee"), MakeStep());

        Assert.Equal(before + 1, state.Revision);
    }

    [Fact]
    public void State_Revision_IsBumpedByGoBack()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());
        var before = state.Revision;

        state.GoBack();

        Assert.Equal(before + 1, state.Revision);
    }

    [Fact]
    public void State_Revision_IsNotBumpedByAFailedGoBackAtTheRoot()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        var before = state.Revision;

        state.GoBack();

        Assert.Equal(before, state.Revision);
    }

    [Fact]
    public void State_Revision_IsBumpedByReplaceAll()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        var before = state.Revision;

        state.ReplaceAll(new[] { MakeFrame("freshRoot") }, null);

        Assert.Equal(before + 1, state.Revision);
    }

    [Fact]
    public void State_Revision_IsBumpedByClear()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        var before = state.Revision;

        state.Clear();

        Assert.Equal(before + 1, state.Revision);
    }

    // ReplaceCurrent is an in-place content refresh for the *same* logical
    // navigation position (a background refresh re-fetching the current
    // item's calls) -- it must never look like a navigation change to a
    // caller comparing Revision before/after an in-flight refresh (see
    // HlslBootstrapPackage's NavigationRevision guard in
    // RefreshCallHierarchyIfOpenAsync/PerformCallHierarchyDrillInAsync).
    [Fact]
    public void State_Revision_IsNotBumpedByReplaceCurrent()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());
        var before = state.Revision;

        state.ReplaceCurrent(MakeFrame("calleeRefreshed"));

        Assert.Equal(before, state.Revision);
    }

    // Deterministic proxy for the "Back-during-refresh" race: an in-flight
    // refresh captures Revision before starting; if the user presses Back
    // before the refresh is ready to commit, the revision it re-checks
    // against will have moved on, and the caller (HlslBootstrapPackage)
    // must discard the refresh's result rather than applying it on top of
    // the user's Back. This test exercises the mechanism itself, since the
    // full async race requires a live tool window/network round-trip that
    // cannot be constructed in this test host (see this file's own header
    // comment).
    [Fact]
    public void State_Revision_DetectsANavigationChangeThatHappenedDuringAnInFlightOperation()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));
        state.Push(MakeFrame("callee"), MakeStep());

        // Simulates an async refresh capturing the revision right before
        // starting its network round-trip.
        var revisionAtStart = state.Revision;

        // Simulates the user clicking Back while the refresh is still in
        // flight.
        var wentBack = state.GoBack();

        // Simulates the refresh's final pre-commit check.
        var wouldBeStale = state.Revision != revisionAtStart;

        Assert.True(wentBack);
        Assert.True(wouldBeStale);
    }

    [Fact]
    public void State_Revision_MatchesWhenNoNavigationHappenedDuringAnInFlightOperation()
    {
        var state = new CallHierarchyExplorerState();
        state.Reset(MakeFrame("root"));

        var revisionAtStart = state.Revision;

        // No navigation occurred: ReplaceCurrent (a same-position content
        // refresh) does not bump Revision.
        state.ReplaceCurrent(MakeFrame("rootRefreshed"));

        var wouldBeStale = state.Revision != revisionAtStart;

        Assert.False(wouldBeStale);
    }

    // --- CallHierarchyRootPositionResolver (root-anchor fallback) --------

    [Fact]
    public void PositionResolver_PrefersTheLiveTrackedPositionWhenAvailable()
    {
        var result = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedPosition: (10, 4),
            lastResolvedSelectionRangeStart: (3, 6),
            originalLine: 3,
            originalCharacter: 6);

        Assert.Equal((10, 4), result);
    }

    // Simulates the exact insert-before-root scenario the tracking point
    // exists for: the user typed several lines above the root, shifting it
    // from its originally captured line 3 down to line 10 -- the tracked
    // position must win over both the stale original and the (also
    // originally-captured, now outdated) selection-range fallback.
    [Fact]
    public void PositionResolver_TrackedPositionReflectsAnInsertBeforeTheRoot()
    {
        var originalLine = 3;
        var originalCharacter = 6;
        // Simulates 7 lines inserted above the root before this refresh
        // ran, as reported by translating a live ITrackingPoint.
        var trackedAfterInsert = (originalLine + 7, originalCharacter);

        var result = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedAfterInsert,
            lastResolvedSelectionRangeStart: (originalLine, originalCharacter),
            originalLine,
            originalCharacter);

        Assert.Equal(trackedAfterInsert, result);
    }

    // Simulates a delete-before-root scenario: lines were removed above the
    // root, shifting it up from its original line.
    [Fact]
    public void PositionResolver_TrackedPositionReflectsADeleteBeforeTheRoot()
    {
        var originalLine = 10;
        var originalCharacter = 4;
        var trackedAfterDelete = (originalLine - 4, originalCharacter);

        var result = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedAfterDelete,
            lastResolvedSelectionRangeStart: (originalLine, originalCharacter),
            originalLine,
            originalCharacter);

        Assert.Equal(trackedAfterDelete, result);
    }

    [Fact]
    public void PositionResolver_FallsBackToTheLastResolvedSelectionRangeWhenNoTrackedPositionIsAvailable()
    {
        var result = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedPosition: null,
            lastResolvedSelectionRangeStart: (5, 2),
            originalLine: 3,
            originalCharacter: 6);

        Assert.Equal((5, 2), result);
    }

    [Fact]
    public void PositionResolver_FallsBackToTheOriginalRawPositionWhenNeitherOtherSourceIsAvailable()
    {
        var result = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedPosition: null,
            lastResolvedSelectionRangeStart: null,
            originalLine: 3,
            originalCharacter: 6);

        Assert.Equal((3, 6), result);
    }

    [Fact]
    public void Frame_RejectsNullItem()
        => Assert.Throws<ArgumentNullException>(
            () => new CallHierarchyFrame(
                null,
                Array.Empty<CallHierarchyIncomingCallModel>(),
                Array.Empty<CallHierarchyOutgoingCallModel>()));

    [Fact]
    public void Frame_NullIncomingAndOutgoingDefaultToEmptyLists()
    {
        var frame = new CallHierarchyFrame(MakeItem("square"), null, null);

        Assert.NotNull(frame.Incoming);
        Assert.Empty(frame.Incoming);
        Assert.NotNull(frame.Outgoing);
        Assert.Empty(frame.Outgoing);
    }

    // --- CallHierarchyItemIdentity (cross-generation path relocation) ----

    [Fact]
    public void Identity_CapturePathStep_ExtractsPathStartOffsetAndCursorKindFromData()
    {
        var item = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);

        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Outgoing, item);

        Assert.Equal(CallHierarchySection.Outgoing, step.Section);
        Assert.Equal("C:/shaders/example.hlsl", step.Path);
        Assert.Equal(87, step.StartOffset);
        Assert.Equal(21, step.CursorKind);
        Assert.Equal("abc", step.RootIdentity);
    }

    [Fact]
    public void Identity_CapturePathStep_MissingOrMalformedDataProducesAStepThatNeverMatches()
    {
        var item = new CallHierarchyItemModel { Name = "orphan", Data = null };

        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, item);

        Assert.Equal(string.Empty, step.Path);
        Assert.Equal(-1, step.StartOffset);
        Assert.Equal(-1, step.CursorKind);
        Assert.Equal(string.Empty, step.RootIdentity);
        Assert.False(CallHierarchyItemIdentity.Matches(item, step));
    }

    [Fact]
    public void Identity_Matches_TrueForSameGenerationChangedItemSharingPathOffsetAndCursorKind()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        // A re-fetched item after recompilation: same declaration (same
        // path/startOffset/cursorKind), but a bumped generation/rootVersion
        // -- exactly what should still match.
        var refetchedJson = SampleItemJson
            .Replace("\"generation\": 3", "\"generation\": 4")
            .Replace("\"rootVersion\": 1", "\"rootVersion\": 2");
        var refetched = JsonConvert.DeserializeObject<CallHierarchyItemModel>(refetchedJson);

        Assert.True(CallHierarchyItemIdentity.Matches(refetched, step));
    }

    [Fact]
    public void Identity_Matches_FalseWhenStartOffsetDiffers()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        var movedJson = SampleItemJson.Replace("\"startOffset\": 87", "\"startOffset\": 200");
        var moved = JsonConvert.DeserializeObject<CallHierarchyItemModel>(movedJson);

        Assert.False(CallHierarchyItemIdentity.Matches(moved, step));
    }

    [Fact]
    public void Identity_Matches_FalseWhenCursorKindDiffers()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        var retypedJson = SampleItemJson.Replace("\"cursorKind\": 21", "\"cursorKind\": 6");
        var retyped = JsonConvert.DeserializeObject<CallHierarchyItemModel>(retypedJson);

        Assert.False(CallHierarchyItemIdentity.Matches(retyped, step));
    }

    [Fact]
    public void Identity_Matches_FalseWhenPathDiffers()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        var elsewhereJson = SampleItemJson.Replace(
            "\"path\": \"C:/shaders/example.hlsl\"",
            "\"path\": \"C:/shaders/other.hlsl\"");
        var elsewhere = JsonConvert.DeserializeObject<CallHierarchyItemModel>(elsewhereJson);

        Assert.False(CallHierarchyItemIdentity.Matches(elsewhere, step));
    }

    // Regression: callHierarchy/incomingCalls expands across every
    // currently open root whose translation unit includes the callee's
    // source file (see docs/call-hierarchy.md), so the exact same physical
    // (path, startOffset, cursorKind) declaration can legitimately be
    // discovered from two different root/config contexts -- e.g. the same
    // active document re-analyzed under two different active variants, or
    // two open documents that both #include the same header. Matching on
    // (path, startOffset, cursorKind) alone would conflate those two
    // distinct incoming-call entries as "the same" drilled item and could
    // silently relocate a drilled-in frame to the wrong physical caller.
    [Fact]
    public void Identity_Matches_FalseWhenRootIdentityDiffersAcrossTwoRootContexts()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        // Same path/startOffset/cursorKind/name as the captured step, but
        // discovered from a *different* root context (a different
        // rootIdentity analysis cache key) -- e.g. the same shared header
        // function, cross-referenced from a second open root document, or
        // the same root re-analyzed under a different active variant.
        var otherContextJson = SampleItemJson.Replace("\"rootIdentity\": \"abc\"", "\"rootIdentity\": \"xyz\"");
        var otherContext = JsonConvert.DeserializeObject<CallHierarchyItemModel>(otherContextJson);

        Assert.False(CallHierarchyItemIdentity.Matches(otherContext, step));
    }

    [Fact]
    public void Identity_Matches_TrueWhenRootIdentityMatchesEvenAfterGenerationBumps()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        // Same root context (rootIdentity unchanged), only the process-wide
        // generation counter bumped -- this must still match (see
        // Identity_Matches_TrueForSameGenerationChangedItemSharingPathOffsetAndCursorKind).
        var refetchedJson = SampleItemJson.Replace("\"generation\": 3", "\"generation\": 4");
        var refetched = JsonConvert.DeserializeObject<CallHierarchyItemModel>(refetchedJson);

        Assert.True(CallHierarchyItemIdentity.Matches(refetched, step));
    }

    [Fact]
    public void Identity_FindMatch_RelocatesToTheCandidateFromTheSameRootContextNotAMatchingOffsetFromAnother()
    {
        var sameContextCaller = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var capturedStep = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, sameContextCaller);

        // A second candidate sharing the exact same path/startOffset/
        // cursorKind (the same physical declaration) but discovered from a
        // different root context -- must never be selected as the match.
        var otherContextJson = SampleItemJson.Replace("\"rootIdentity\": \"abc\"", "\"rootIdentity\": \"xyz\"");
        var otherContextCaller = JsonConvert.DeserializeObject<CallHierarchyItemModel>(otherContextJson);
        var freshSameContextCaller =
            JsonConvert.DeserializeObject<CallHierarchyItemModel>(
                SampleItemJson.Replace("\"generation\": 3", "\"generation\": 5"));

        var frame = new CallHierarchyFrame(
            MakeItem("root"),
            new List<CallHierarchyIncomingCallModel>
            {
                new() { From = otherContextCaller, FromRanges = Array.Empty<CompilationSourceRangeModel>() },
                new() { From = freshSameContextCaller, FromRanges = Array.Empty<CompilationSourceRangeModel>() },
            },
            Array.Empty<CallHierarchyOutgoingCallModel>());

        var found = CallHierarchyItemIdentity.FindMatch(frame, capturedStep);

        Assert.Same(freshSameContextCaller, found);
    }

    [Fact]
    public void Identity_Matches_FalseForNullCandidateOrNullStep()
    {
        var original = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, original);

        Assert.False(CallHierarchyItemIdentity.Matches(null, step));
        Assert.False(CallHierarchyItemIdentity.Matches(original, null));
    }

    [Fact]
    public void Identity_FindMatch_LocatesTheMatchingCallerWithinTheIncomingSection()
    {
        var caller = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var capturedStep = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Incoming, caller);
        var frame = new CallHierarchyFrame(
            MakeItem("root"),
            new List<CallHierarchyIncomingCallModel>
            {
                new() { From = caller, FromRanges = Array.Empty<CompilationSourceRangeModel>() },
            },
            Array.Empty<CallHierarchyOutgoingCallModel>());

        var found = CallHierarchyItemIdentity.FindMatch(frame, capturedStep);

        Assert.Same(caller, found);
    }

    [Fact]
    public void Identity_FindMatch_LocatesTheMatchingCalleeWithinTheOutgoingSection()
    {
        var callee = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var capturedStep = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Outgoing, callee);
        var frame = new CallHierarchyFrame(
            MakeItem("root"),
            Array.Empty<CallHierarchyIncomingCallModel>(),
            new List<CallHierarchyOutgoingCallModel>
            {
                new() { To = callee, FromRanges = Array.Empty<CompilationSourceRangeModel>() },
            });

        var found = CallHierarchyItemIdentity.FindMatch(frame, capturedStep);

        Assert.Same(callee, found);
    }

    [Fact]
    public void Identity_FindMatch_ReturnsNullWhenNoCandidateMatches()
    {
        var callee = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var missingStep = new CallHierarchyPathStep(CallHierarchySection.Outgoing, "gone.hlsl", 999, 21);
        var frame = new CallHierarchyFrame(
            MakeItem("root"),
            Array.Empty<CallHierarchyIncomingCallModel>(),
            new List<CallHierarchyOutgoingCallModel>
            {
                new() { To = callee, FromRanges = Array.Empty<CompilationSourceRangeModel>() },
            });

        Assert.Null(CallHierarchyItemIdentity.FindMatch(frame, missingStep));
    }

    [Fact]
    public void Identity_FindMatch_ReturnsNullForNullFrameOrNullStep()
    {
        var callee = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var step = CallHierarchyItemIdentity.CapturePathStep(CallHierarchySection.Outgoing, callee);
        var frame = new CallHierarchyFrame(
            MakeItem("root"),
            Array.Empty<CallHierarchyIncomingCallModel>(),
            Array.Empty<CallHierarchyOutgoingCallModel>());

        Assert.Null(CallHierarchyItemIdentity.FindMatch(null, step));
        Assert.Null(CallHierarchyItemIdentity.FindMatch(frame, null));
    }


    // PreprocessorExplorerBridge/EntryPointDataFlowBridge) -----------------

    [Fact]
    public async Task Bridge_PrepareForwardsUriPositionAndCancellationTokenToRegisteredHandler()
    {
        Uri observedUri = null;
        int observedLine = -1;
        int observedCharacter = -1;
        CancellationToken observedToken = default;
        var expected = new List<CallHierarchyItemModel> { MakeItem("square") };
        CallHierarchyBridge.RegisterPrepare(
            (uri, line, character, token) =>
            {
                observedUri = uri;
                observedLine = line;
                observedCharacter = character;
                observedToken = token;
                return Task.FromResult<IReadOnlyList<CallHierarchyItemModel>>(expected);
            });

        using var cancellation = new CancellationTokenSource();
        var requestUri = new Uri("file:///C:/shaders/example.hlsl");
        var result = await CallHierarchyBridge.PrepareAsync(requestUri, 3, 7, cancellation.Token);

        Assert.Same(expected, result);
        Assert.Equal(requestUri, observedUri);
        Assert.Equal(3, observedLine);
        Assert.Equal(7, observedCharacter);
        Assert.Equal(cancellation.Token, observedToken);
    }

    [Fact]
    public async Task Bridge_IncomingCallsForwardsItemAndCancellationTokenToRegisteredHandler()
    {
        CallHierarchyItemModel observedItem = null;
        var expected = new List<CallHierarchyIncomingCallModel>();
        CallHierarchyBridge.RegisterIncomingCalls(
            (item, _) =>
            {
                observedItem = item;
                return Task.FromResult<IReadOnlyList<CallHierarchyIncomingCallModel>>(expected);
            });

        var requestItem = MakeItem("square");
        var result = await CallHierarchyBridge.RequestIncomingCallsAsync(
            requestItem,
            CancellationToken.None);

        Assert.Same(expected, result);
        Assert.Same(requestItem, observedItem);
    }

    [Fact]
    public async Task Bridge_OutgoingCallsForwardsItemAndCancellationTokenToRegisteredHandler()
    {
        CallHierarchyItemModel observedItem = null;
        var expected = new List<CallHierarchyOutgoingCallModel>();
        CallHierarchyBridge.RegisterOutgoingCalls(
            (item, _) =>
            {
                observedItem = item;
                return Task.FromResult<IReadOnlyList<CallHierarchyOutgoingCallModel>>(expected);
            });

        var requestItem = MakeItem("square");
        var result = await CallHierarchyBridge.RequestOutgoingCallsAsync(
            requestItem,
            CancellationToken.None);

        Assert.Same(expected, result);
        Assert.Same(requestItem, observedItem);
    }

    [Fact]
    public void Bridge_RegisterPrepare_RejectsNullHandler()
        => Assert.Throws<ArgumentNullException>(() => CallHierarchyBridge.RegisterPrepare(null));

    [Fact]
    public void Bridge_RegisterIncomingCalls_RejectsNullHandler()
        => Assert.Throws<ArgumentNullException>(
            () => CallHierarchyBridge.RegisterIncomingCalls(null));

    [Fact]
    public void Bridge_RegisterOutgoingCalls_RejectsNullHandler()
        => Assert.Throws<ArgumentNullException>(
            () => CallHierarchyBridge.RegisterOutgoingCalls(null));

    // --- CallHierarchyItemIdentity.IsSameCallable (root re-identity) -----
    // Coverage for the guard added to RefreshCallHierarchyIfOpenAsync: a
    // background refresh must never silently switch the displayed root to
    // an unrelated callable just because *something* was found at the
    // tracked/fallback position it re-queried.

    [Fact]
    public void IsSameCallable_TrueForTheExactSameItemAcrossAGenerationBump()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var refetchedJson = SampleItemJson.Replace("\"generation\": 3", "\"generation\": 9");
        var refetched = JsonConvert.DeserializeObject<CallHierarchyItemModel>(refetchedJson);

        Assert.True(CallHierarchyItemIdentity.IsSameCallable(previous, refetched));
    }

    // Simulates an insertion made earlier in the file shifting the same
    // declaration's own start offset forward, without changing which
    // declaration it is -- StartOffset is deliberately not required to
    // match (see IsSameCallable's own doc comment).
    [Fact]
    public void IsSameCallable_TrueForIdenticalPathCursorKindAndNameEvenWhenStartOffsetShifted()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var shiftedJson = SampleItemJson.Replace("\"startOffset\": 87", "\"startOffset\": 250");
        var shifted = JsonConvert.DeserializeObject<CallHierarchyItemModel>(shiftedJson);

        Assert.True(CallHierarchyItemIdentity.IsSameCallable(previous, shifted));
    }

    // Deletion/replacement regression: the root function was deleted and an
    // unrelated, differently-named function now resolves at the tracked
    // position -- must never be silently accepted as a continuation.
    [Fact]
    public void IsSameCallable_FalseWhenTheDeclarationWasDeletedAndReplacedByADifferentlyNamedFunction()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var replacedJson = SampleItemJson.Replace("\"name\": \"square\"", "\"name\": \"unrelatedHelper\"");
        var replaced = JsonConvert.DeserializeObject<CallHierarchyItemModel>(replacedJson);

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, replaced));
    }

    // Regression: the declaration's own DXC cursor kind changed at the same
    // name/path (e.g. the tracked position now resolves to a variable or
    // macro rather than a function) -- must not be treated as the same
    // callable.
    [Fact]
    public void IsSameCallable_FalseWhenCursorKindDiffers()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var retypedJson = SampleItemJson.Replace("\"cursorKind\": 21", "\"cursorKind\": 6");
        var retyped = JsonConvert.DeserializeObject<CallHierarchyItemModel>(retypedJson);

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, retyped));
    }

    // Reopen regression: the document was closed and reopened (or the
    // fallback position otherwise resolved) against different file content
    // entirely.
    [Fact]
    public void IsSameCallable_FalseWhenPathDiffers()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var elsewhereJson = SampleItemJson.Replace(
            "\"path\": \"C:/shaders/example.hlsl\"",
            "\"path\": \"C:/shaders/other.hlsl\"");
        var elsewhere = JsonConvert.DeserializeObject<CallHierarchyItemModel>(elsewhereJson);

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, elsewhere));
    }

    // Never guesses when identity data is missing/malformed on either
    // side -- "ambiguity rejected", not "ambiguity assumed benign".
    [Fact]
    public void IsSameCallable_FalseWhenEitherItemIsMissingItsIdentityDataOrIsNull()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var orphan = new CallHierarchyItemModel { Name = "square", Data = null };

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, orphan));
        Assert.False(CallHierarchyItemIdentity.IsSameCallable(orphan, previous));
        Assert.False(CallHierarchyItemIdentity.IsSameCallable(null, previous));
        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, null));
        Assert.False(CallHierarchyItemIdentity.IsSameCallable(null, null));
    }

    // Overload disambiguation: HLSL permits overloading a name by
    // parameter types alone, so two distinct overloads can share
    // (path, cursorKind, name) while differing only in StartOffset (which
    // this check cannot safely require, since StartOffset also
    // legitimately shifts for a genuine continuation of the *same*
    // declaration -- see the "StartOffsetShifted" test above). Their
    // compiler-authored Detail (signature) differs, though, and is what
    // actually distinguishes them -- issue #24 explicitly requires overload
    // identity, so this must be rejected, not accepted as an unavoidable
    // limitation.
    [Fact]
    public void IsSameCallable_FalseForADifferentOverloadSharingNameCursorKindAndPathButDifferentDetail()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        Assert.Equal("float square(float x)", previous.Detail);

        // Same name/path/cursorKind (and even the same startOffset here,
        // to isolate Detail as the only differing signal), but a distinct
        // overload's own signature.
        var otherOverloadJson = SampleItemJson.Replace(
            "\"detail\": \"float square(float x)\"",
            "\"detail\": \"float square(int x)\"");
        var otherOverload = JsonConvert.DeserializeObject<CallHierarchyItemModel>(otherOverloadJson);

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, otherOverload));
    }

    // The genuine continuation case: same Detail (signature unchanged),
    // combined with a generation bump AND a StartOffset shift (an edit
    // earlier in the file moved the declaration) -- must still be accepted.
    [Fact]
    public void IsSameCallable_TrueWhenDetailMatchesEvenAcrossAGenerationBumpAndStartOffsetShift()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var refreshedJson = SampleItemJson
            .Replace("\"generation\": 3", "\"generation\": 9")
            .Replace("\"startOffset\": 87", "\"startOffset\": 250");
        var refreshed = JsonConvert.DeserializeObject<CallHierarchyItemModel>(refreshedJson);

        Assert.True(CallHierarchyItemIdentity.IsSameCallable(previous, refreshed));
    }

    // Never guesses when Detail itself is missing/empty on either side,
    // even if path/cursorKind/name all agree.
    [Fact]
    public void IsSameCallable_FalseWhenEitherItemHasNoDetail()
    {
        var previous = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);
        var noDetailJson = SampleItemJson.Replace("\"detail\": \"float square(float x)\",", string.Empty);
        var noDetail = JsonConvert.DeserializeObject<CallHierarchyItemModel>(noDetailJson);
        Assert.True(string.IsNullOrEmpty(noDetail.Detail));

        Assert.False(CallHierarchyItemIdentity.IsSameCallable(previous, noDetail));
        Assert.False(CallHierarchyItemIdentity.IsSameCallable(noDetail, previous));
    }

    // --- Protocol dispatch: real StreamJsonRpc wiring --------------------
    //
    // Drives HlslLanguageClient's three new RPC methods through the exact
    // production wiring (AttachForCustomMessageAsync + a fake server
    // target), asserting the exact three requests
    // (textDocument/prepareCallHierarchy, callHierarchy/incomingCalls,
    // callHierarchy/outgoingCalls) and that the opaque `data` envelope is
    // round-tripped byte-for-byte from the item PrepareCallHierarchyAsync
    // returned into the item GetIncomingCallsAsync/GetOutgoingCallsAsync
    // send back -- concrete protocol-level evidence, not an assumption
    // about the manual wire-serialization helpers.
    [Fact]
    public async Task CallHierarchyRpc_SendsExactThreeRequestsAndRoundTripsOpaqueData()
    {
        var (serverStream, clientStream) = FullDuplexStream.CreatePair();
        var target = new FakeCallHierarchyServer();
        using var serverRpc = new JsonRpc(serverStream, serverStream, target);
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        var documentUri = new Uri("file:///C:/shaders/example.hlsl");
        var prepared = await client.PrepareCallHierarchyAsync(
            documentUri,
            3,
            7,
            CancellationToken.None);

        Assert.NotNull(target.PrepareParams);
        Assert.Equal("file:///C:/shaders/example.hlsl", target.PrepareParams.TextDocument?.Uri);
        Assert.Equal(3, target.PrepareParams.Position.Line);
        Assert.Equal(7, target.PrepareParams.Position.Character);
        Assert.Single(prepared);
        var item = prepared[0];
        Assert.Equal("square", item.Name);

        var incoming = await client.GetIncomingCallsAsync(item, CancellationToken.None);
        var outgoing = await client.GetOutgoingCallsAsync(item, CancellationToken.None);

        Assert.NotNull(target.IncomingCallsItem);
        Assert.NotNull(target.OutgoingCallsItem);
        // The exact same opaque data JToken the server produced for
        // `prepareCallHierarchy` must reach both follow-up requests
        // unmodified -- this is the identity envelope the server relies on
        // to re-resolve the exact callable (see docs/call-hierarchy.md).
        Assert.Equal(3, target.IncomingCallsItem["data"]?["generation"]?.Value<int>());
        Assert.Equal("abc", target.IncomingCallsItem["data"]?["rootIdentity"]?.Value<string>());
        Assert.Equal(3, target.OutgoingCallsItem["data"]?["generation"]?.Value<int>());
        Assert.Equal("abc", target.OutgoingCallsItem["data"]?["rootIdentity"]?.Value<string>());

        Assert.Single(incoming);
        Assert.Equal("main", incoming[0].From.Name);
        Assert.Single(incoming[0].FromRanges);
        Assert.Single(outgoing);
        Assert.Equal("square", outgoing[0].To.Name);

        serverStream.Dispose();
        clientStream.Dispose();
    }

    // The server's standard LSP ContentModified error (-32801, see
    // docs/call-hierarchy.md) must surface to the caller as the
    // Bootstrap-safe CallHierarchyContentModifiedException, never as a raw
    // StreamJsonRpc RemoteInvocationException, keeping the Bootstrap
    // assembly free of any LSP wire-protocol dependency.
    [Fact]
    public async Task CallHierarchyRpc_TranslatesContentModifiedErrorForAllThreeRequests()
    {
        var (serverStream, clientStream) = FullDuplexStream.CreatePair();
        var target = new FakeContentModifiedServer();
        using var serverRpc = new JsonRpc(serverStream, serverStream, target);
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        var documentUri = new Uri("file:///C:/shaders/example.hlsl");
        var item = JsonConvert.DeserializeObject<CallHierarchyItemModel>(SampleItemJson);

        await Assert.ThrowsAsync<CallHierarchyContentModifiedException>(
            () => client.PrepareCallHierarchyAsync(documentUri, 0, 0, CancellationToken.None));
        await Assert.ThrowsAsync<CallHierarchyContentModifiedException>(
            () => client.GetIncomingCallsAsync(item, CancellationToken.None));
        await Assert.ThrowsAsync<CallHierarchyContentModifiedException>(
            () => client.GetOutgoingCallsAsync(item, CancellationToken.None));

        serverStream.Dispose();
        clientStream.Dispose();
    }

    private sealed class FakeCallHierarchyServer
    {
        internal FakePrepareParams PrepareParams { get; private set; }

        internal JObject IncomingCallsItem { get; private set; }

        internal JObject OutgoingCallsItem { get; private set; }

        [JsonRpcMethod(
            "textDocument/prepareCallHierarchy",
            UseSingleObjectParameterDeserialization = true)]
        public JArray PrepareCallHierarchy(FakePrepareParams parameters)
        {
            PrepareParams = parameters;
            var array = new JArray { JObject.Parse(SampleItemJson) };
            return array;
        }

        [JsonRpcMethod(
            "callHierarchy/incomingCalls",
            UseSingleObjectParameterDeserialization = true)]
        public JArray IncomingCalls(FakeItemParams parameters)
        {
            IncomingCallsItem = parameters.Item;
            var call = new JObject
            {
                ["from"] = JObject.Parse(SampleCallerItemJson),
                ["fromRanges"] = new JArray
                {
                    new JObject
                    {
                        ["start"] = new JObject { ["line"] = 11, ["character"] = 10 },
                        ["end"] = new JObject { ["line"] = 11, ["character"] = 16 },
                    },
                },
            };
            return new JArray { call };
        }

        [JsonRpcMethod(
            "callHierarchy/outgoingCalls",
            UseSingleObjectParameterDeserialization = true)]
        public JArray OutgoingCalls(FakeItemParams parameters)
        {
            OutgoingCallsItem = parameters.Item;
            var call = new JObject
            {
                ["to"] = JObject.Parse(SampleItemJson),
                ["fromRanges"] = new JArray
                {
                    new JObject
                    {
                        ["start"] = new JObject { ["line"] = 12, ["character"] = 2 },
                        ["end"] = new JObject { ["line"] = 12, ["character"] = 8 },
                    },
                },
            };
            return new JArray { call };
        }
    }

    private sealed class FakeContentModifiedServer
    {
        [JsonRpcMethod(
            "textDocument/prepareCallHierarchy",
            UseSingleObjectParameterDeserialization = true)]
        public JArray PrepareCallHierarchy(FakePrepareParams parameters)
            => throw new LocalRpcException("The call hierarchy item is stale.")
            {
                ErrorCode = -32801,
            };

        [JsonRpcMethod(
            "callHierarchy/incomingCalls",
            UseSingleObjectParameterDeserialization = true)]
        public JArray IncomingCalls(FakeItemParams parameters)
            => throw new LocalRpcException("The call hierarchy item is stale.")
            {
                ErrorCode = -32801,
            };

        [JsonRpcMethod(
            "callHierarchy/outgoingCalls",
            UseSingleObjectParameterDeserialization = true)]
        public JArray OutgoingCalls(FakeItemParams parameters)
            => throw new LocalRpcException("The call hierarchy item is stale.")
            {
                ErrorCode = -32801,
            };
    }

    private sealed class FakePrepareParams
    {
        public FakeTextDocumentIdentifier TextDocument { get; set; }

        public FakePosition Position { get; set; }
    }

    private sealed class FakeItemParams
    {
        public JObject Item { get; set; }
    }

    private sealed class FakeTextDocumentIdentifier
    {
        public string Uri { get; set; }
    }

    private sealed class FakePosition
    {
        public int Line { get; set; }

        public int Character { get; set; }
    }
}
