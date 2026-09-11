using System;
using System.Threading;

namespace HlslLsp.VisualStudio.Bootstrap;

// Bounds each analysis request to a fixed timeout and ensures at most one is
// meaningfully in flight per reusable window. Starting a new explicit,
// background, or follow-mode retarget cancels and disposes the previous
// request token, while each view's generation guard prevents a superseded
// response from being applied even if cancellation races with completion.
// Kept free of VS/StreamJsonRpc dependencies so it is directly unit testable.
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

    internal void CancelCurrent()
    {
        var previous = Interlocked.Exchange(ref current, null);
        if (previous != null)
        {
            previous.Cancel();
            previous.Dispose();
        }
    }
}
