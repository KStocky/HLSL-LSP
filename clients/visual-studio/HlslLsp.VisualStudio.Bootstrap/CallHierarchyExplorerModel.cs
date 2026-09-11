using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class CallHierarchyRefreshFreshness
{
    internal static AnalysisFreshnessState AfterNavigationMismatch(
        AnalysisFreshnessState current,
        AnalysisFreshnessCause cause)
        => AnalysisFreshnessReducer.Reduce(
            current,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.Invalidated,
                cause));
}

internal static class CallHierarchyRootRefreshPolicy
{
    internal static (Uri DocumentUri, int Line, int Character)
        TrackedTargetAfterFailedReplacement(
            (Uri DocumentUri, int Line, int Character) displayedTarget,
            (Uri DocumentUri, int Line, int Character) requestedTarget)
    {
        _ = requestedTarget;
        return displayedTarget;
    }
}

// A standard LSP CallHierarchyIncomingCall (see docs/call-hierarchy.md,
// "callHierarchy/incomingCalls / callHierarchy/outgoingCalls"): the caller
// item plus every call-site range within its own body, in source order.
// From reuses CallHierarchyItemModel (defined alongside
// EntryPointDataFlowModel) rather than a second bespoke item shape, since
// it is the exact same wire shape.
public sealed class CallHierarchyIncomingCallModel
{
    public CallHierarchyItemModel From { get; set; }

    public IReadOnlyList<CompilationSourceRangeModel> FromRanges { get; set; } =
        Array.Empty<CompilationSourceRangeModel>();
}

// A standard LSP CallHierarchyOutgoingCall: the callee item plus every
// call-site range within the *root* item's own body (not the callee's)
// that invokes it, in source order -- "fromRanges" is named this way by the
// LSP spec itself for both incoming and outgoing calls.
public sealed class CallHierarchyOutgoingCallModel
{
    public CallHierarchyItemModel To { get; set; }

    public IReadOnlyList<CompilationSourceRangeModel> FromRanges { get; set; } =
        Array.Empty<CompilationSourceRangeModel>();
}

// Raised by the registered CallHierarchyBridge handlers (implemented in the
// Client assembly, which owns the actual StreamJsonRpc/RemoteInvocationException
// dependency) when the server rejects a previously-returned CallHierarchyItem
// with the standard LSP ContentModified error -- i.e. the item's opaque
// data.generation no longer matches the root's current generation because
// the root, an #include'd file it depends on, or the active variant changed
// since the item was produced (see docs/call-hierarchy.md,
// "CallHierarchyItem.data: stable identity envelope"). Kept as a plain
// Exception subclass with no StreamJsonRpc/LSP-specific members so the
// Bootstrap assembly -- which owns the tool window and must stay isolated
// from LSP wire-protocol dependencies -- can catch and render this without
// referencing RemoteInvocationException or any LSP error-code constant
// itself.
public sealed class CallHierarchyContentModifiedException : Exception
{
    public CallHierarchyContentModifiedException()
        : base("The call hierarchy item is stale.")
    {
    }

    public CallHierarchyContentModifiedException(string message)
        : base(message)
    {
    }
}

// Decouples the WPF tool window (Bootstrap assembly) from the language
// client (Client assembly), mirroring CompilationInfoBridge/
// PreprocessorExplorerBridge/EntryPointDataFlowBridge. Three independent
// request kinds are registered separately, matching the three distinct
// protocol requests this feature issues in sequence
// (textDocument/prepareCallHierarchy, then callHierarchy/incomingCalls and
// callHierarchy/outgoingCalls for the resolved or a drilled-into item).
public static class CallHierarchyBridge
{
    private static Func<Uri, int, int, CancellationToken, Task<IReadOnlyList<CallHierarchyItemModel>>> prepare;
    private static Func<CallHierarchyItemModel, CancellationToken, Task<IReadOnlyList<CallHierarchyIncomingCallModel>>> incoming;
    private static Func<CallHierarchyItemModel, CancellationToken, Task<IReadOnlyList<CallHierarchyOutgoingCallModel>>> outgoing;

    public static void RegisterPrepare(
        Func<Uri, int, int, CancellationToken, Task<IReadOnlyList<CallHierarchyItemModel>>> handler)
    {
        Volatile.Write(ref prepare, handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static void RegisterIncomingCalls(
        Func<CallHierarchyItemModel, CancellationToken, Task<IReadOnlyList<CallHierarchyIncomingCallModel>>> handler)
    {
        Volatile.Write(ref incoming, handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static void RegisterOutgoingCalls(
        Func<CallHierarchyItemModel, CancellationToken, Task<IReadOnlyList<CallHierarchyOutgoingCallModel>>> handler)
    {
        Volatile.Write(ref outgoing, handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<IReadOnlyList<CallHierarchyItemModel>> PrepareAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref prepare);
        return handler == null
            ? Task.FromResult<IReadOnlyList<CallHierarchyItemModel>>(null)
            : handler(uri, line, character, cancellationToken);
    }

    public static Task<IReadOnlyList<CallHierarchyIncomingCallModel>> RequestIncomingCallsAsync(
        CallHierarchyItemModel item,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref incoming);
        return handler == null
            ? Task.FromResult<IReadOnlyList<CallHierarchyIncomingCallModel>>(null)
            : handler(item, cancellationToken);
    }

    public static Task<IReadOnlyList<CallHierarchyOutgoingCallModel>> RequestOutgoingCallsAsync(
        CallHierarchyItemModel item,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref outgoing);
        return handler == null
            ? Task.FromResult<IReadOnlyList<CallHierarchyOutgoingCallModel>>(null)
            : handler(item, cancellationToken);
    }
}

// One level of a Call Hierarchy tool window's navigation stack: the
// callable item currently selected/drilled into, plus the last
// successfully fetched incoming/outgoing calls for it. Incoming/Outgoing
// are never null (always at least an empty list) so a transient refresh
// failure can leave them exactly as last displayed without special-casing
// null throughout the renderer.
internal sealed class CallHierarchyFrame
{
    internal CallHierarchyFrame(
        CallHierarchyItemModel item,
        IReadOnlyList<CallHierarchyIncomingCallModel> incoming,
        IReadOnlyList<CallHierarchyOutgoingCallModel> outgoing)
    {
        Item = item ?? throw new ArgumentNullException(nameof(item));
        Incoming = incoming ?? Array.Empty<CallHierarchyIncomingCallModel>();
        Outgoing = outgoing ?? Array.Empty<CallHierarchyOutgoingCallModel>();
    }

    internal CallHierarchyItemModel Item { get; }

    internal IReadOnlyList<CallHierarchyIncomingCallModel> Incoming { get; }

    internal IReadOnlyList<CallHierarchyOutgoingCallModel> Outgoing { get; }
}

// Which of a frame's two call lists a drilled-into item was found in --
// needed to relocate that same item within a freshly re-fetched frame
// after a background refresh re-resolves the root (see
// CallHierarchyItemIdentity/CallHierarchyPathStep below).
internal enum CallHierarchySection
{
    Incoming,
    Outgoing,
}

// Enough identity to relocate a previously drilled-into item within a
// freshly fetched incoming/outgoing list after the underlying source
// changed and the root had to be re-resolved from scratch (see
// docs/call-hierarchy.md, "CallHierarchyItem.data: stable identity
// envelope"). (Path, StartOffset, CursorKind) is the same identity key the
// server itself uses to distinguish overloaded functions sharing one name,
// so it is the most robust cross-generation match available to the client.
// RootIdentity (the *discovering* root's own analysis cache key -- see
// docs/call-hierarchy.md) is also captured and compared: callHierarchy/
// incomingCalls expands across every currently open root whose translation
// unit includes the callee's source file, so the exact same physical
// (path, startOffset, cursorKind) declaration can legitimately appear as a
// distinct incoming-call entry discovered from two different root
// contexts (two open documents sharing a #include'd header, or the same
// document re-analyzed under a different active variant/config) --
// matching on (path, startOffset, cursorKind) alone would conflate those
// as "the same" drilled item and could relocate to the wrong one.
// Generation/rootVersion are still deliberately excluded, since those are
// expected to differ after exactly the kind of change this is recovering
// from (an edit/config change to the *same* root/context).
internal sealed class CallHierarchyPathStep
{
    internal CallHierarchyPathStep(
        CallHierarchySection section,
        string path,
        long startOffset,
        long cursorKind,
        string rootIdentity = "")
    {
        Section = section;
        Path = path ?? string.Empty;
        StartOffset = startOffset;
        CursorKind = cursorKind;
        RootIdentity = rootIdentity ?? string.Empty;
    }

    internal CallHierarchySection Section { get; }

    internal string Path { get; }

    internal long StartOffset { get; }

    internal long CursorKind { get; }

    internal string RootIdentity { get; }
}

// Pure matching/capture logic for CallHierarchyPathStep, kept free of any
// WPF/VS/network dependency so it is directly unit testable: capturing a
// step never fails (an item with missing/malformed data simply produces a
// step that will not match anything later, rather than throwing), and
// matching is a straightforward structural comparison against a freshly
// fetched frame's own items.
internal static class CallHierarchyItemIdentity
{
    internal static CallHierarchyPathStep CapturePathStep(
        CallHierarchySection section,
        CallHierarchyItemModel item)
    {
        var data = item?.Data as JObject;
        return new CallHierarchyPathStep(
            section,
            data?.Value<string>("path") ?? string.Empty,
            data?.Value<long?>("startOffset") ?? -1,
            data?.Value<long?>("cursorKind") ?? -1,
            data?.Value<string>("rootIdentity") ?? string.Empty);
    }

    internal static bool Matches(CallHierarchyItemModel candidate, CallHierarchyPathStep step)
    {
        if (step == null || candidate?.Data is not JObject data)
        {
            return false;
        }
        var startOffset = data.Value<long?>("startOffset") ?? -1;
        if (startOffset < 0 || startOffset != step.StartOffset)
        {
            return false;
        }
        var cursorKind = data.Value<long?>("cursorKind") ?? -1;
        if (cursorKind != step.CursorKind)
        {
            return false;
        }
        var path = data.Value<string>("path") ?? string.Empty;
        if (!string.Equals(path, step.Path, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        // Guards against conflating the same physical (path, startOffset,
        // cursorKind) declaration discovered from two different root/config
        // contexts (see this class's CallHierarchyPathStep doc comment) --
        // an opaque server cache key, so compared exactly rather than
        // case-insensitively like a filesystem path.
        var rootIdentity = data.Value<string>("rootIdentity") ?? string.Empty;
        return string.Equals(rootIdentity, step.RootIdentity, StringComparison.Ordinal);
    }

    // Searches the section of frame the step was originally drilled from
    // (frame's Incoming callers, or Outgoing callees) for an item whose
    // identity matches. Returns null -- never throws -- when the item
    // cannot be relocated (it was removed, renamed to a different
    // declaration, or the frame itself has no candidates), letting the
    // caller fall back to resetting the view to a fresh root with an
    // explanation instead of silently showing a wrong or stale item.
    internal static CallHierarchyItemModel FindMatch(CallHierarchyFrame frame, CallHierarchyPathStep step)
    {
        if (frame == null || step == null)
        {
            return null;
        }
        if (step.Section == CallHierarchySection.Incoming)
        {
            foreach (var call in frame.Incoming)
            {
                if (Matches(call?.From, step))
                {
                    return call.From;
                }
            }
            return null;
        }
        foreach (var call in frame.Outgoing)
        {
            if (Matches(call?.To, step))
            {
                return call.To;
            }
        }
        return null;
    }

    // Confirms that a freshly re-prepared root item still represents the
    // *same* underlying declaration as the previously displayed root item,
    // before a background refresh (see
    // HlslBootstrapPackage.RefreshCallHierarchyIfOpenAsync) is allowed to
    // silently replace it. Deliberately does NOT require StartOffset to
    // match: an edit made anywhere before the root's own declaration in the
    // same file legitimately shifts its start offset without changing which
    // declaration it is (see CallHierarchyRootPositionResolver, which
    // exists precisely to still find the *same* declaration across such a
    // shift by tracking its position rather than trusting a now-stale
    // absolute offset). Instead requires the callable's own canonical
    // (path, cursorKind, name) -- the parts of the server's identity
    // envelope that are NOT expected to shift merely from surrounding
    // edits -- to all agree.
    //
    // This cannot perfectly disambiguate two overloads that happen to
    // share every one of those three fields: HLSL permits overloading by
    // parameter types alone, so two overloads commonly DO share
    // name/cursorKind/path, and the server itself only fully disambiguates
    // overloads by additionally comparing StartOffset (see
    // docs/call-hierarchy.md) -- which this check cannot safely require
    // here, since StartOffset legitimately shifts for the very same
    // declaration. Any missing/malformed identity data on either side is
    // treated as inability to confirm, not a pass: this never guesses when
    // identity is genuinely ambiguous or unavailable, matching the
    // "ambiguity rejected" contract -- callers must preserve the last
    // Confirms that a freshly re-prepared root item still represents the
    // *same* underlying declaration as the previously displayed root item,
    // before a background refresh (see
    // HlslBootstrapPackage.RefreshCallHierarchyIfOpenAsync) is allowed to
    // silently replace it. Deliberately does NOT require StartOffset to
    // match: an edit made anywhere before the root's own declaration in the
    // same file legitimately shifts its start offset without changing which
    // declaration it is (see CallHierarchyRootPositionResolver, which
    // exists precisely to still find the *same* declaration across such a
    // shift by tracking its position rather than trusting a now-stale
    // absolute offset). Instead requires the callable's own canonical
    // (path, cursorKind, name) -- the parts of the server's identity
    // envelope that are NOT expected to shift merely from surrounding
    // edits -- to all agree, PLUS the compiler-authored Detail (the
    // callable's own signature, e.g. "float square(float x)"): HLSL permits
    // overloading a name by parameter types alone, so two distinct
    // overloads commonly DO share (path, cursorKind, name) -- Detail is
    // what actually distinguishes them, since it differs by parameter/
    // return types even when every other field coincides. Detail must be
    // present and equal on both sides; a missing/empty Detail on either
    // side is treated as inability to confirm identity, not a pass -- this
    // never guesses when identity is genuinely ambiguous or unavailable.
    // Any missing/malformed identity data anywhere in this check means
    // callers must preserve the last successful content and surface a
    // relocation/stale banner rather than silently switching roots.
    internal static bool IsSameCallable(CallHierarchyItemModel previous, CallHierarchyItemModel candidate)
    {
        if (previous?.Data is not JObject previousData || candidate?.Data is not JObject candidateData)
        {
            return false;
        }
        var previousPath = previousData.Value<string>("path");
        var candidatePath = candidateData.Value<string>("path");
        if (string.IsNullOrEmpty(previousPath) ||
            !string.Equals(previousPath, candidatePath, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        var previousCursorKind = previousData.Value<long?>("cursorKind");
        var candidateCursorKind = candidateData.Value<long?>("cursorKind");
        if (previousCursorKind == null || previousCursorKind != candidateCursorKind)
        {
            return false;
        }
        var previousName = previousData.Value<string>("name") ?? previous.Name;
        var candidateName = candidateData.Value<string>("name") ?? candidate.Name;
        if (string.IsNullOrEmpty(previousName) ||
            !string.Equals(previousName, candidateName, StringComparison.Ordinal))
        {
            return false;
        }
        // Distinguishes same-name overloads: (path, cursorKind, name) alone
        // cannot tell "float square(float x)" apart from "float
        // square(int x)", but their compiler-authored Detail differs.
        return !string.IsNullOrEmpty(previous.Detail) &&
            !string.IsNullOrEmpty(candidate.Detail) &&
            string.Equals(previous.Detail, candidate.Detail, StringComparison.Ordinal);
    }
}

// The pure navigation-stack logic backing the Call Hierarchy tool window's
// "drill in" / "back" behavior: pushing a frame for a clicked
// caller/callee re-centers the view on it (an explicit, opt-in way to
// inspect *its* calls, since the server only returns one level of
// incoming/outgoing per request), and Back pops to the previously fetched
// frame without any new request -- it was already fetched, so there is
// nothing to re-query and nothing that can go stale by going back. Each
// pushed frame's own CallHierarchyPathStep (how it was found within its
// parent frame) is tracked alongside it, so a background refresh can
// attempt to relocate the whole path underneath a freshly re-resolved root
// (see CallHierarchyItemIdentity above). Kept free of any WPF/VS dependency
// so it is directly unit testable.
internal sealed class CallHierarchyExplorerState
{
    private readonly List<CallHierarchyFrame> frames = new();
    private readonly List<CallHierarchyPathStep> steps = new();

    internal CallHierarchyFrame Current => frames.Count == 0 ? null : frames[frames.Count - 1];

    // The bottom-of-stack frame (the entry point the whole view is
    // centered under), distinct from Current (the top of the stack, which
    // may be a drilled-into caller/callee): used to recover a compiler-
    // verified fallback location for the root itself when a live source
    // position can no longer be tracked (see CallHierarchyRootPositionResolver).
    internal CallHierarchyFrame Root => frames.Count == 0 ? null : frames[0];

    internal bool CanGoBack => frames.Count > 1;

    internal int Depth => frames.Count;

    // Bumped by every operation that changes the navigation stack's shape
    // or position (Reset/Push/GoBack/ReplaceAll/Clear) -- never by
    // ReplaceCurrent, which updates in-place content for the same logical
    // position. An in-flight asynchronous operation (background refresh,
    // explicit drill-in) that captures this value before starting can
    // compare it again just before committing its own result, and discard
    // that result outright if the user navigated (most importantly, went
    // Back) in the meantime: applying a stale operation's result on top of
    // a since-changed navigation position would silently misrepresent, or
    // outright undo, what the user just did.
    internal long Revision { get; private set; }

    // The step for each frame above the root, in root-to-current order
    // (Count == Depth - 1 whenever a root exists). Used by a background
    // refresh to attempt to rebuild the current drill-in path underneath a
    // freshly re-resolved root; never mutated by the caller.
    internal IReadOnlyList<CallHierarchyPathStep> CapturePath() => steps.ToArray();

    internal void Reset(CallHierarchyFrame root)
    {
        frames.Clear();
        steps.Clear();
        frames.Add(root ?? throw new ArgumentNullException(nameof(root)));
        Revision++;
    }

    internal void Clear()
    {
        frames.Clear();
        steps.Clear();
        Revision++;
    }

    internal void Push(CallHierarchyFrame frame, CallHierarchyPathStep step)
    {
        if (frames.Count == 0)
        {
            throw new InvalidOperationException(
                "Cannot drill into a call hierarchy item before a root frame exists.");
        }
        frames.Add(frame ?? throw new ArgumentNullException(nameof(frame)));
        steps.Add(step ?? throw new ArgumentNullException(nameof(step)));
        Revision++;
    }

    internal void ReplaceCurrent(CallHierarchyFrame frame)
    {
        if (frames.Count == 0)
        {
            throw new InvalidOperationException(
                "Cannot replace the current call hierarchy frame before one exists.");
        }
        frames[frames.Count - 1] = frame ?? throw new ArgumentNullException(nameof(frame));
    }

    // Wholesale-replaces the entire stack, used after a background refresh
    // successfully re-resolves the root and relocates every frame the user
    // had drilled into underneath it (see CallHierarchyItemIdentity). The
    // new root frame is required; newSteps may be null/empty for a
    // root-only stack.
    internal void ReplaceAll(
        IReadOnlyList<CallHierarchyFrame> newFrames,
        IReadOnlyList<CallHierarchyPathStep> newSteps)
    {
        if (newFrames == null || newFrames.Count == 0)
        {
            throw new ArgumentException(
                "A rebuilt call hierarchy stack must contain at least a root frame.",
                nameof(newFrames));
        }
        frames.Clear();
        frames.AddRange(newFrames);
        steps.Clear();
        if (newSteps != null)
        {
            steps.AddRange(newSteps);
        }
        Revision++;
    }

    internal bool GoBack()
    {
        if (!CanGoBack)
        {
            return false;
        }
        frames.RemoveAt(frames.Count - 1);
        if (steps.Count > 0)
        {
            steps.RemoveAt(steps.Count - 1);
        }
        Revision++;
        return true;
    }
}

// Pure decision logic for the position a background call-hierarchy refresh
// should re-run textDocument/prepareCallHierarchy at (see
// HlslBootstrapPackage.RefreshCallHierarchyIfOpenAsync). Deliberately kept
// free of ITrackingPoint/ITextBuffer/any VS dependency -- callers translate
// their own live tracking-point/selection-range/raw-caret sources into
// plain (line, character) tuples first, making this orchestration directly
// unit testable (including simulated insert/delete-before-root scenarios,
// by varying the "tracked" tuple across calls) without a real editor host.
internal static class CallHierarchyRootPositionResolver
{
    // Priority order:
    //  1. trackedPosition -- the root's original caret position, re-read
    //     from a live ITrackingPoint in the SAME open buffer the root was
    //     established against, translated to the current snapshot. This is
    //     the only source that correctly follows unsaved inserts/deletes
    //     made anywhere before the root's position since it was
    //     established, and must be preferred whenever available.
    //  2. lastResolvedSelectionRangeStart -- the last successfully resolved
    //     root item's own compiler-supplied SelectionRange.Start, used only
    //     when the tracking point itself is unavailable (for example, the
    //     buffer that was open when the root was established is no longer
    //     the live buffer backing this document -- the file was closed and
    //     reopened, or externally reloaded from disk, producing a new
    //     buffer identity the old tracking point cannot follow). A
    //     compiler-verified location for the exact same declaration is far
    //     more trustworthy here than blindly re-issuing the original raw
    //     caret line/character against a buffer whose identity and content
    //     history have since diverged -- doing so risks resolving a
    //     different callable entirely, or none at all, rather than merely
    //     being imprecise.
    //  3. originalLine/originalCharacter -- the raw line/character
    //     originally captured when the root was established, used only
    //     when neither of the above is available (in practice, only
    //     reachable immediately after root establishment, before any
    //     tracking point could plausibly have become unavailable and
    //     before this root ever produced a resolved item to fall back on).
    // Never invents or infers a different callable than the one already
    // anchored by one of these three sources.
    internal static (int Line, int Character) ResolveRefreshPosition(
        (int Line, int Character)? trackedPosition,
        (int Line, int Character)? lastResolvedSelectionRangeStart,
        int originalLine,
        int originalCharacter)
    {
        if (trackedPosition.HasValue)
        {
            return trackedPosition.Value;
        }
        if (lastResolvedSelectionRangeStart.HasValue)
        {
            return lastResolvedSelectionRangeStart.Value;
        }
        return (originalLine, originalCharacter);
    }
}

// Pure presentation-logic helpers for the Call Hierarchy tool window,
// tested directly without any WPF/VS dependency (mirrors
// EntryPointDataFlowDisplay).
internal static class CallHierarchyExplorerDisplay
{
    internal static string NoCallableMessage()
        => "No callable symbol was found at the selected position. Right-click " +
           "a function name, then choose HLSL > Call Hierarchy.";

    // "fromRanges" is always non-empty for a genuine call edge, but the
    // count itself (multiple call sites for one caller/callee pair) is
    // still useful to surface rather than silently collapsing to one row
    // per source-order range.
    internal static string CallSiteCountLabel(int count)
        => count switch
        {
            <= 0 => string.Empty,
            1 => "1 call site",
            _ => $"{count} call sites",
        };

    internal static string StaleItemMessage()
        => "This call hierarchy is stale \u2013 the containing code or " +
           "active variant changed since it was resolved. Right-click the " +
           "function and choose HLSL > Call Hierarchy to refresh it.";

    internal static string RequestFailedMessage(string detail)
        => string.IsNullOrEmpty(detail)
            ? "Could not retrieve call hierarchy information."
            : $"Could not retrieve call hierarchy information: {detail}";

    // Shown after a background refresh successfully re-resolves the root
    // but cannot relocate the previously drilled-into item(s) underneath
    // it (the declaration was removed, renamed, or otherwise no longer
    // appears among its parent's freshly fetched callers/callees). The
    // view resets to the fresh root rather than showing a stale or
    // possibly-wrong drilled item.
    internal static string PathNotRelocatedMessage()
        => "The previously explored call could not be located after the " +
           "source changed; showing the entry point's call hierarchy again.";

    // Shown after a background refresh re-runs prepareCallHierarchy but the
    // freshly resolved item cannot be confirmed to be the *same*
    // declaration as the root currently displayed (see
    // CallHierarchyItemIdentity.IsSameCallable) -- for example the root
    // function was deleted and replaced by an unrelated one, or the
    // document was closed and reopened with different content at that
    // position. The last successful content is preserved rather than
    // silently switching to a possibly-unrelated root.
    internal static string RootIdentityChangedMessage()
        => "The call hierarchy root could not be confirmed to still be the " +
           "same function after this change; showing the last known result. " +
           "Right-click the function and choose HLSL > Call Hierarchy to re-anchor it.";
}
