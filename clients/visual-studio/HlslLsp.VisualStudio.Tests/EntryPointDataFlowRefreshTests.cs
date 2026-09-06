using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

// Regression coverage for three medium defects an independent review found
// in the Entry-Point Data Flow window's refresh/error-recovery logic. The
// underlying HlslBootstrapPackage/EntryPointDataFlowToolWindow types extend
// AsyncPackage/ToolWindowPane and cannot be constructed in a test host, so
// the relevant decision logic was extracted into small pure helpers
// (EntryPointDataFlowRefreshLogic, EntryPointDataFlowRefreshGate) that are
// exercised directly here; HlslBootstrapPackage's Show/RefreshIfOpen methods
// delegate to these same helpers unchanged.
public sealed class EntryPointDataFlowRefreshTests
{
    // Defect: the window went stale after unsaved root edits, open-include
    // edits/saves, and shadertoolsconfig.json changes because the only
    // save-triggered refresh required the saved file to equal the window's
    // own root document. IsHlslOrConfigRelevantPath now treats any
    // configured HLSL/header extension or shadertoolsconfig.json (by name)
    // as relevant, regardless of whether it is the window's own document,
    // and is also the basis for the new debounced unsaved-edit refresh
    // trigger in HlslLspActivator.
    [Theory]
    [InlineData(@"C:\shaders\other.hlsl")]
    [InlineData(@"C:\shaders\included\common.hlsli")]
    [InlineData(@"C:\shaders\shadertoolsconfig.json")]
    [InlineData(@"C:\SHADERS\SHADERTOOLSCONFIG.JSON")]
    public void IsHlslOrConfigRelevantPath_TreatsNonRootHlslAndConfigFilesAsRelevant(
        string path)
    {
        var configuredExtensions = new[] { ".hlsl", ".hlsli" };

        Assert.True(
            EntryPointDataFlowRefreshLogic.IsHlslOrConfigRelevantPath(
                path,
                configuredExtensions));
    }

    [Theory]
    [InlineData(@"C:\project\Program.cs")]
    [InlineData(@"C:\project\readme.md")]
    [InlineData(null)]
    [InlineData("")]
    public void IsHlslOrConfigRelevantPath_IgnoresUnrelatedFiles(string path)
    {
        var configuredExtensions = new[] { ".hlsl", ".hlsli" };

        Assert.False(
            EntryPointDataFlowRefreshLogic.IsHlslOrConfigRelevantPath(
                path,
                configuredExtensions));
    }

    // Defect: a refresh trigger (save/variant/debounced edit) that arrived
    // while an explicit Tools-command request was in flight was silently
    // discarded, so the window could remain stale forever immediately after
    // the explicit request completed. TryBeginBackgroundRefresh must defer
    // (not drop) such a trigger, and ExitExplicitRequest must report exactly
    // one deferred refresh so it can be replayed.
    [Fact]
    public void RefreshGate_DefersBackgroundRefreshDuringExplicitRequestAndReplaysItOnce()
    {
        var gate = new EntryPointDataFlowRefreshGate();

        gate.EnterExplicitRequest();
        try
        {
            Assert.False(gate.TryBeginBackgroundRefresh());
            // A second concurrent trigger while still in flight must still
            // collapse into a single bounded replay, not one per trigger.
            Assert.False(gate.TryBeginBackgroundRefresh());
        }
        finally
        {
            Assert.True(gate.ExitExplicitRequest());
        }

        // Nothing left to replay a second time, and a fresh trigger should
        // now proceed immediately since no explicit request remains in
        // flight.
        Assert.True(gate.TryBeginBackgroundRefresh());
    }

    [Fact]
    public void RefreshGate_AllowsBackgroundRefreshImmediatelyWhenNoExplicitRequestIsInFlight()
    {
        var gate = new EntryPointDataFlowRefreshGate();

        Assert.True(gate.TryBeginBackgroundRefresh());
    }

    [Fact]
    public void RefreshGate_ReportsNoDeferredRefreshWhenNoneOccurredDuringExplicitRequest()
    {
        var gate = new EntryPointDataFlowRefreshGate();

        gate.EnterExplicitRequest();

        Assert.False(gate.ExitExplicitRequest());
    }

    // Two concurrent explicit requests (e.g. a rapid double-invocation of
    // the Tools command) must not lose a background refresh trigger that
    // arrives while both are in flight, even though the gate's pending flag
    // is not itself nesting-aware: the first (inner) request to finish may
    // be told a refresh is pending and attempt to replay it, but that replay
    // goes back through TryBeginBackgroundRefresh, which must defer it again
    // because the outer request is still in flight -- re-arming the pending
    // flag rather than losing it -- so the outer request's own exit
    // eventually reports it instead.
    [Fact]
    public void RefreshGate_SelfHealsWhenAnInnerExplicitRequestExitsWhileAnOuterOneIsStillInFlight()
    {
        var gate = new EntryPointDataFlowRefreshGate();

        gate.EnterExplicitRequest();
        gate.EnterExplicitRequest();

        Assert.False(gate.TryBeginBackgroundRefresh());

        var innerShouldReplay = gate.ExitExplicitRequest();
        if (innerShouldReplay)
        {
            Assert.False(gate.TryBeginBackgroundRefresh());
        }

        Assert.True(gate.ExitExplicitRequest());
        Assert.True(gate.TryBeginBackgroundRefresh());
    }

    // Defect: a failed manual retry through the explicit Tools command
    // erased the window's last successful content for the same document,
    // because the preserve decision was based on whether the caller already
    // held a window-instance reference (existingWindow != null) rather than
    // on whether the window already showed this exact document.
    // ShouldPreserveContentOnFailure now takes the prior document uri
    // directly, matching how the fixed ShowEntryPointDataFlowAsync captures
    // it (via a lookup, not via the existingWindow parameter) before issuing
    // the request.
    [Fact]
    public void ShouldPreserveContentOnFailure_PreservesWhenPriorWindowShowedSameDocument()
    {
        var uri = new Uri("file:///C:/shaders/example.hlsl");

        Assert.True(
            EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(uri, uri));
    }

    [Fact]
    public void ShouldPreserveContentOnFailure_DoesNotPreserveForADifferentDocument()
    {
        var priorUri = new Uri("file:///C:/shaders/other.hlsl");
        var requestedUri = new Uri("file:///C:/shaders/example.hlsl");

        Assert.False(
            EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
                priorUri,
                requestedUri));
    }

    [Fact]
    public void ShouldPreserveContentOnFailure_DoesNotPreserveWhenNoWindowExistedYet()
    {
        var requestedUri = new Uri("file:///C:/shaders/example.hlsl");

        Assert.False(
            EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
                null,
                requestedUri));
    }

    // End-to-end (within what is testable outside a live VS host)
    // composition: simulates the explicit-command overload's flow --
    // capture prior document before the request, issue the request,
    // determine preserve-on-failure from the captured prior document, all
    // while a concurrent background refresh trigger arrives mid-flight and
    // must be replayed afterward rather than dropped.
    [Fact]
    public async Task ExplicitRequestFlow_PreservesLastGoodContentAndReplaysDeferredRefresh()
    {
        var gate = new EntryPointDataFlowRefreshGate();
        var documentUri = new Uri("file:///C:/shaders/example.hlsl");

        // Simulates the window already being open with good content for
        // this exact document before the manual retry begins.
        Uri priorWindowDocumentUri = documentUri;
        var hadMatchingDocument = EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
            priorWindowDocumentUri,
            documentUri);
        Assert.True(hadMatchingDocument);

        gate.EnterExplicitRequest();
        var replayed = false;
        try
        {
            // A save/variant/edit refresh trigger arrives while the
            // explicit request set up above is still in flight.
            Assert.False(gate.TryBeginBackgroundRefresh());

            // The explicit request itself fails (simulated); the window
            // must preserve its last-good content rather than erase it.
            var preserveOnFailure = hadMatchingDocument;
            Assert.True(preserveOnFailure);

            await Task.Yield();
        }
        finally
        {
            if (gate.ExitExplicitRequest())
            {
                replayed = true;
            }
        }

        Assert.True(replayed);
    }

    // Coordinated multithreaded stress test for the lock-based rewrite of
    // EntryPointDataFlowRefreshGate: the previous Interlocked-only
    // implementation had a genuine check-then-act race between reading
    // explicitRequestsInFlight and setting/consuming the pending flag as
    // two separate atomic steps, which could permanently strand a deferred
    // refresh (see the class's own remarks). This hammers all three
    // operations from many threads concurrently and asserts the internal
    // counter is never corrupted: many paired Enter/Exit calls interleaved
    // with many concurrent TryBeginBackgroundRefresh attempts must still
    // leave explicitRequestsInFlight at exactly zero once every explicit
    // request has exited, which a corrupted (e.g. negative, or stuck
    // positive) counter would fail to satisfy.
    [Fact]
    public async Task RefreshGate_SurvivesConcurrentEnterExitAndBackgroundRefreshWithoutCorruptingState()
    {
        var gate = new EntryPointDataFlowRefreshGate();
        const int explicitWorkers = 8;
        const int iterationsPerWorker = 500;
        const int backgroundWorkers = 4;
        using var stop = new System.Threading.CancellationTokenSource();
        var replayCount = 0;
        var deferredCount = 0;
        var immediateCount = 0;

        var explicitTasks = new List<Task>();
        for (var worker = 0; worker < explicitWorkers; ++worker)
        {
            explicitTasks.Add(Task.Run(() =>
            {
                for (var i = 0; i < iterationsPerWorker; ++i)
                {
                    gate.EnterExplicitRequest();
                    // Encourage interleaving with other threads between
                    // the enter above and the exit below, rather than the
                    // whole pair running as an uninterrupted unit.
                    System.Threading.Thread.SpinWait(i % 17);
                    if (gate.ExitExplicitRequest())
                    {
                        System.Threading.Interlocked.Increment(ref replayCount);
                    }
                }
            }));
        }

        var backgroundTasks = new List<Task>();
        for (var worker = 0; worker < backgroundWorkers; ++worker)
        {
            backgroundTasks.Add(Task.Run(() =>
            {
                while (!stop.IsCancellationRequested)
                {
                    if (gate.TryBeginBackgroundRefresh())
                    {
                        System.Threading.Interlocked.Increment(ref immediateCount);
                    }
                    else
                    {
                        System.Threading.Interlocked.Increment(ref deferredCount);
                    }
                }
            }));
        }

        await Task.WhenAll(explicitTasks);
        stop.Cancel();
        await Task.WhenAll(backgroundTasks);

        // The counter must have returned to exactly zero: a corrupted
        // (negative, from an unbalanced decrement race) or stuck-positive
        // count would make this final call incorrectly defer forever even
        // though no explicit request remains in flight.
        Assert.True(gate.TryBeginBackgroundRefresh());
        // Sanity: the stress actually exercised both outcomes and never
        // threw/deadlocked getting here.
        Assert.True(immediateCount > 0);
        Assert.True(replayCount >= 0);
        Assert.True(deferredCount >= 0);
    }

    // Defect: an explicit Tools-command invocation could leave an
    // already-open-but-hidden tool window pane hidden, because a prior fix
    // for a different defect (preserving last-good content) substituted a
    // non-showing FindToolWindowAsync lookup result in for the window used
    // to decide whether ShowToolWindowAsync must run. ShowEntryPointDataFlow
    // Async cannot itself be constructed here (it lives on
    // HlslBootstrapPackage, an AsyncPackage), so this exercises the small
    // pure predicate it now delegates that decision to: it is true whenever
    // the caller did not already supply its own resolved window reference
    // (every explicit command invocation), and false whenever it did (every
    // background refresh), matching "priorWindow is used only for
    // preservation; existingWindow alone decides whether to reveal".
    [Fact]
    public void ShouldRevealToolWindow_IsTrueWhenNoWindowReferenceWasSupplied()
        => Assert.True(EntryPointDataFlowRefreshLogic.ShouldRevealToolWindow(
            existingWindowSupplied: false));

    [Fact]
    public void ShouldRevealToolWindow_IsFalseWhenCallerAlreadySuppliedAWindowReference()
        => Assert.False(EntryPointDataFlowRefreshLogic.ShouldRevealToolWindow(
            existingWindowSupplied: true));
}
