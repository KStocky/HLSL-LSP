using System;
using System.Threading;

namespace HlslLsp.VisualStudio.Bootstrap;

// Bounds each background refresh (variant change, save, or debounced
// unsaved-edit trigger) to a fixed timeout and ensures at most one is
// meaningfully in flight per refreshable window at a time: starting a new
// background refresh cancels and disposes whatever earlier one this same
// instance produced, so a burst of overlapping triggers (for example a
// save arriving while a debounced-edit refresh for the same window is
// still in flight) coalesces into just the most recent request rather than
// letting superseded requests accumulate and keep running to completion
// for no benefit -- their result would be discarded by the generation
// guard on completion anyway, so cancelling them promptly only saves
// wasted server work and avoids an unbounded pile-up of in-flight
// requests. Kept free of any VS/StreamJsonRpc dependency so it is directly
// unit testable; HlslBootstrapPackage owns one instance per refreshable
// window (Entry-Point Data Flow, Call Hierarchy).
internal sealed class CoalescingBackgroundRefreshCancellation
{
    private readonly TimeSpan timeout;
    private CancellationTokenSource current;

    internal CoalescingBackgroundRefreshCancellation(TimeSpan timeout)
    {
        this.timeout = timeout;
    }

    // Cancels and disposes whatever token this instance most recently
    // produced (if any), then returns a fresh token linked to
    // ambientCancellationToken (so package disposal always cancels the
    // newest refresh too) and bounded by this instance's fixed timeout.
    // The returned source is intentionally not disposed here: ownership of
    // disposing it passes to whichever later call supersedes it (or, for
    // the very last one produced, to eventual finalization) -- the caller
    // must not dispose it itself, since a caller reference can outlive the
    // point where a concurrent newer call swaps it out.
    internal CancellationTokenSource BeginNext(CancellationToken ambientCancellationToken)
    {
        var next = CancellationTokenSource.CreateLinkedTokenSource(ambientCancellationToken);
        next.CancelAfter(timeout);
        var previous = Interlocked.Exchange(ref current, next);
        if (previous != null)
        {
            previous.Cancel();
            previous.Dispose();
        }
        return next;
    }
}
