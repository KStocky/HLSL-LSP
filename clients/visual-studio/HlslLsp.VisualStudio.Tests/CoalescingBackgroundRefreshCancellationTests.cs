using System;
using System.Threading;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

// Coverage for CoalescingBackgroundRefreshCancellation, the pure helper
// backing the 30s timeout + coalescing behavior required of background
// data-flow/call-hierarchy refreshes (see HlslBootstrapPackage's
// entryPointDataFlowBackgroundRefreshCancellation/
// callHierarchyBackgroundRefreshCancellation fields). Kept free of any
// VS/StreamJsonRpc dependency, so every scenario here is directly and
// deterministically testable without a real language server or UI host.
public sealed class CoalescingBackgroundRefreshCancellationTests
{
    [Fact]
    public void BeginNext_FirstCallReturnsANonCancelledToken()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));

        var first = coalescing.BeginNext(CancellationToken.None);

        Assert.False(first.IsCancellationRequested);
    }

    [Fact]
    public void BeginNext_SupersedingCallCancelsThePreviousToken()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        var first = coalescing.BeginNext(CancellationToken.None);

        var second = coalescing.BeginNext(CancellationToken.None);

        Assert.True(first.IsCancellationRequested);
        Assert.False(second.IsCancellationRequested);
    }

    [Fact]
    public void BeginNext_SupersedingCallDisposesThePreviousToken()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        var first = coalescing.BeginNext(CancellationToken.None);

        coalescing.BeginNext(CancellationToken.None);

        // A disposed CancellationTokenSource throws ObjectDisposedException
        // from further Cancel() calls -- the standard way to observe
        // disposal without a reflection-based check.
        Assert.Throws<ObjectDisposedException>(() => first.Cancel());
    }

    [Fact]
    public void BeginNext_PropagatesCancellationFromTheAmbientToken()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        using var ambient = new CancellationTokenSource();

        var next = coalescing.BeginNext(ambient.Token);
        ambient.Cancel();

        Assert.True(next.IsCancellationRequested);
    }

    [Fact]
    public void BeginNext_DoesNotCancelWhenTheAmbientTokenIsAlreadyCancelledAtDifferentInstances()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        using var ambientA = new CancellationTokenSource();
        using var ambientB = new CancellationTokenSource();

        var first = coalescing.BeginNext(ambientA.Token);
        var second = coalescing.BeginNext(ambientB.Token);
        ambientA.Cancel();

        // The first token is linked to ambientA, but it was already
        // cancelled/disposed by the second BeginNext call before ambientA
        // was ever cancelled -- only the second (current) token's linkage
        // to ambientB is meaningful going forward.
        Assert.False(second.IsCancellationRequested);
    }

    [Fact]
    public void BeginNext_ShortTimeoutEventuallyCancelsTheToken()
    {
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromMilliseconds(20));

        var token = coalescing.BeginNext(CancellationToken.None);

        Assert.True(
            SpinWait.SpinUntil(() => token.IsCancellationRequested, TimeSpan.FromSeconds(5)),
            "Expected the bounded token to become cancelled once its timeout elapsed.");
    }

    [Fact]
    public void BeginNext_CalledConcurrentlyNeverThrowsAndAlwaysLeavesExactlyOneNonDisposedToken()
    {
        // Coordinated multithreaded stress: many threads race to call
        // BeginNext on the same instance simultaneously. The
        // Interlocked.Exchange-based swap must never throw, double-dispose,
        // or leak more than the single most-recent token in a live state --
        // regression coverage for the kind of enter/exit race this
        // refactor's sibling fix (EntryPointDataFlowRefreshGate) was
        // designed to eliminate for the explicit/background gating.
        var coalescing = new CoalescingBackgroundRefreshCancellation(TimeSpan.FromSeconds(30));
        const int threadCount = 16;
        const int iterationsPerThread = 200;
        var barrier = new Barrier(threadCount);
        var tokens = new CancellationTokenSource[threadCount][];
        for (var i = 0; i < threadCount; i++)
        {
            tokens[i] = new CancellationTokenSource[iterationsPerThread];
        }
        Exception observedException = null;

        var threads = new Thread[threadCount];
        for (var t = 0; t < threadCount; t++)
        {
            var threadIndex = t;
            threads[t] = new Thread(() =>
            {
                try
                {
                    barrier.SignalAndWait();
                    for (var i = 0; i < iterationsPerThread; i++)
                    {
                        tokens[threadIndex][i] = coalescing.BeginNext(CancellationToken.None);
                    }
                }
                catch (Exception ex)
                {
                    Interlocked.CompareExchange(ref observedException, ex, null);
                }
            });
            threads[t].Start();
        }
        foreach (var thread in threads)
        {
            thread.Join();
        }

        Assert.Null(observedException);

        // Exactly one token (whichever call landed last) must remain
        // non-cancelled; every other token produced by any thread must have
        // been cancelled by whatever superseded it.
        var nonCancelledCount = 0;
        foreach (var perThread in tokens)
        {
            foreach (var token in perThread)
            {
                if (!token.IsCancellationRequested)
                {
                    nonCancelledCount++;
                }
            }
        }
        Assert.Equal(1, nonCancelledCount);
    }
}
