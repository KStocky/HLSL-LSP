using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio.Bootstrap;

// A standard LSP CallHierarchyItem (see docs/call-hierarchy.md), reused for
// EntryPointDataFlowModel.EntryPoint/ReachableFunctionModel.Function/
// EntryPointDataFlowModel.UnreachableFunctions. Range/SelectionRange reuse
// CompilationSourceRangeModel/CompilationSourcePositionModel (defined in
// CompilationInfoModel.cs) rather than a second bespoke range shape.
public sealed class CallHierarchyItemModel
{
    public string Name { get; set; }

    public long Kind { get; set; }

    public string Detail { get; set; }

    public string Uri { get; set; }

    public CompilationSourceRangeModel Range { get; set; }

    public CompilationSourceRangeModel SelectionRange { get; set; }

    // Server-owned, opaque identity envelope (see docs/call-hierarchy.md,
    // "CallHierarchyItem.data: stable identity envelope"): must be
    // round-tripped unmodified by a client that later issues
    // callHierarchy/incomingCalls or callHierarchy/outgoingCalls for this
    // exact item. This tool window only displays entries -- it never issues
    // a follow-up call-hierarchy request -- so Data is never inspected here
    // and is kept as a raw JToken rather than a bespoke typed shape, the
    // same way PreprocessorSettingModel.Value keeps an opaque/variable JSON
    // shape as JToken.
    public JToken Data { get; set; }
}

public sealed class ReachableFunctionModel
{
    public CallHierarchyItemModel Function { get; set; }

    // Shortest call-graph distance from the entry point; 0 is the entry
    // point itself.
    public long Depth { get; set; }

    // True when this function is (transitively) part of a call cycle
    // reachable from the entry point, including direct self-recursion.
    public bool Recursive { get; set; }
}

// A plain navigable location for a top-level declaration DXC reports zero
// references to anywhere in the current unsaved snapshot -- not a callable
// CallHierarchyItem, so it carries no round-trippable Data envelope (see
// docs/call-hierarchy.md, "unusedDeclarations").
public sealed class UnusedDeclarationModel
{
    public string Name { get; set; }

    public long Kind { get; set; }

    public string Uri { get; set; }

    public CompilationSourceRangeModel Range { get; set; }

    public CompilationSourceRangeModel SelectionRange { get; set; }
}

// A global variable, cbuffer/tbuffer field, or resource read/written by any
// function in ReachableFunctions, merged by declaration identity (see
// docs/call-hierarchy.md, "globalAccesses").
public sealed class GlobalAccessModel
{
    public string Name { get; set; }

    public long Kind { get; set; }

    public string Uri { get; set; }

    public CompilationSourceRangeModel Range { get; set; }

    public CompilationSourceRangeModel SelectionRange { get; set; }

    // Disambiguates fields with the same short name across different
    // cbuffers/structs.
    public string QualifiedName { get; set; }

    // "read", "write", or "readWrite". See docs/call-hierarchy.md,
    // "Conservative read/write classification": whenever DXC's cursor tree
    // does not prove an access is read-only or write-only, it is reported
    // as "readWrite" rather than guessed narrower.
    public string Access { get; set; }
}

// The hlsl/entryPointDataFlow response (see docs/call-hierarchy.md). Every
// field the protocol defines is modelled type-safely; nothing is inferred
// client-side from a name or position that DXC did not itself report.
public sealed class EntryPointDataFlowModel
{
    // False when no entry point is configured, or a configured entry point
    // name does not resolve to any function definition in the current
    // unsaved snapshot; every list below is then empty and Explanation is a
    // human-readable reason suitable for direct display.
    public bool Found { get; set; }

    public string Explanation { get; set; }

    // Null exactly when Found is false.
    public CallHierarchyItemModel EntryPoint { get; set; }

    // Every function transitively reachable from the entry point, including
    // the entry point itself at depth 0, ordered by ascending depth then
    // source location (server-ordered; not re-sorted by this client).
    public IReadOnlyList<ReachableFunctionModel> ReachableFunctions { get; set; } =
        Array.Empty<ReachableFunctionModel>();

    // Every function definition in the translation unit not present in
    // ReachableFunctions -- dead code for the active entry point/variant.
    public IReadOnlyList<CallHierarchyItemModel> UnreachableFunctions { get; set; } =
        Array.Empty<CallHierarchyItemModel>();

    // Top-level declarations (functions and global variables/resources)
    // DXC reports zero references to anywhere in the current snapshot; a
    // compiler-verifiable, entry-point-independent signal distinct from
    // UnreachableFunctions.
    public IReadOnlyList<UnusedDeclarationModel> UnusedDeclarations { get; set; } =
        Array.Empty<UnusedDeclarationModel>();

    public IReadOnlyList<GlobalAccessModel> GlobalAccesses { get; set; } =
        Array.Empty<GlobalAccessModel>();

    // Header-level convenience summary only -- equal to
    // FunctionsVisitedTruncated || DefinitionsTruncated ||
    // GlobalAccessesTruncated || UnusedDeclarationsTruncated. The four
    // causes below are independent and gate different, non-overlapping
    // parts of the response, so rendering must read the specific flag for
    // each section rather than blaming this aggregate on any one of them
    // (see docs/call-hierarchy.md, "truncated").
    public bool Truncated { get; set; }

    // True when the reachability traversal itself hit the function-visit
    // budget before exhausting the call graph. ReachableFunctions/
    // GlobalAccesses are then a conservative subset of the true sets, and
    // UnreachableFunctions is left completely empty by the server (an
    // unvisited function is never reported as dead code).
    public bool FunctionsVisitedTruncated { get; set; }

    // True when collecting the translation unit's top-level callable
    // *definitions* (used both to resolve/disambiguate the configured
    // entry point by name and to compute UnreachableFunctions) hit its own
    // budget, independently of FunctionsVisitedTruncated -- a translation
    // unit can have a tiny, fully-explored reachable call graph and still
    // an enormous number of unrelated dead function definitions.
    // UnreachableFunctions is left completely empty by the server whenever
    // this is true, for the same reason as under FunctionsVisitedTruncated:
    // a definition that was never collected cannot be told apart from one
    // that was collected and found reachable. Entry point resolution still
    // uses whatever definitions were collected before the budget was hit.
    public bool DefinitionsTruncated { get; set; }

    // True when the global/resource access scan hit its own budget and
    // stopped retaining further distinct accesses, independent of whether
    // the reachability traversal itself completed. GlobalAccesses is then a
    // conservative subset of the true set.
    public bool GlobalAccessesTruncated { get; set; }

    // True when the unused-top-level-declaration scan hit its own budget
    // and stopped issuing further reference lookups, independent of the
    // other two flags. UnusedDeclarations is then a conservative subset of
    // the true set (never a false "unused" claim).
    public bool UnusedDeclarationsTruncated { get; set; }

    public long FunctionsVisited { get; set; }
}

// Pure, unit-testable presentation logic for the Entry-Point Data Flow tool
// window: label text derived only from already-known, compiler-reported
// enum-like string/bool fields -- never from a symbol name or location.
// Kept alongside the model, mirroring MemoryLayoutDisplayName/
// MemoryLayoutByteScale in MemoryLayoutModel.cs.
internal static class EntryPointDataFlowDisplay
{
    internal static string AccessLabel(string access)
        => access switch
        {
            "read" => "Read",
            "write" => "Write",
            "readWrite" => "Read/Write",
            _ => access ?? string.Empty,
        };

    internal static string RecursionLabel(bool recursive)
        => recursive ? "Recursive" : string.Empty;

    internal static string DepthLabel(long depth)
        => depth == 0 ? "Entry point" : $"Depth {depth}";

    // functionsVisitedTruncated must be the specific
    // EntryPointDataFlowModel.FunctionsVisitedTruncated flag, not the
    // header-level aggregate Truncated: GlobalAccessesTruncated/
    // UnusedDeclarationsTruncated do not imply the reachability traversal
    // itself was incomplete, so using the aggregate here would falsely
    // blame the function graph for an unrelated section's truncation.
    internal static string FunctionsVisitedSummary(
        long functionsVisited,
        bool functionsVisitedTruncated)
        => functionsVisitedTruncated
            ? $"{functionsVisited} function(s) visited (truncated \u2013 the graph is larger " +
              "than the traversal budget; some unreachable functions may be missing)"
            : $"{functionsVisited} function(s) visited";

    internal static string NotFoundMessage(string explanation)
        => string.IsNullOrEmpty(explanation)
            ? "No entry-point data flow is available for the active document/variant."
            : explanation;

    // Section-specific warning for the Global & resource accesses section;
    // null when that section's own scan was not truncated (independent of
    // the other two truncation flags).
    internal static string GlobalAccessesTruncationWarning(bool globalAccessesTruncated)
        => globalAccessesTruncated
            ? "Truncated \u2013 the global/resource access scan hit its budget; some " +
              "accesses may be missing."
            : null;

    // Section-specific warning for the Unused declarations section; null
    // when that section's own scan was not truncated (independent of the
    // other two truncation flags).
    internal static string UnusedDeclarationsTruncationWarning(bool unusedDeclarationsTruncated)
        => unusedDeclarationsTruncated
            ? "Truncated \u2013 the unused-declaration scan hit its budget; some unused " +
              "declarations may be missing."
            : null;

    // Section-specific warning for the definition-collection budget, shown
    // near the Unreachable functions section header (in addition to, not
    // instead of, the FunctionsVisitedTruncated placeholder/warning): a
    // definitions-collection budget hit is an independent cause from an
    // incomplete reachability traversal, even though both leave
    // UnreachableFunctions completely empty, so the two must be reported
    // as separate, precisely worded warnings rather than one conflated
    // message that would misattribute a definitions-collection limit to
    // "the graph is too big" or vice versa.
    internal static string DefinitionsTruncatedWarning(bool definitionsTruncated)
        => definitionsTruncated
            ? "Truncated \u2013 collecting the file's function definitions hit its budget; " +
              "entry-point resolution used only the definitions collected so far, and " +
              "unreachable functions cannot be identified yet."
            : null;

    // The server leaves UnreachableFunctions completely empty when EITHER
    // FunctionsVisitedTruncated OR DefinitionsTruncated is true (an
    // unvisited function is never reported as dead code, and a definition
    // that was never collected cannot be told apart from one that was
    // collected and found reachable), so an empty list must not be shown as
    // a plain "(none)" in either case -- that would falsely read as "the
    // graph was checked and nothing is unreachable" rather than "this could
    // not be determined yet". The two causes are independent, so the
    // message names whichever one(s) actually applied rather than a single
    // generic phrase.
    internal static string UnreachableFunctionsPlaceholder(
        bool functionsVisitedTruncated,
        bool definitionsTruncated)
    {
        if (functionsVisitedTruncated && definitionsTruncated)
        {
            return "Not determined \u2013 the reachability traversal did not finish and " +
                   "the function-definition collection hit its budget, so unreachable " +
                   "functions cannot be identified yet.";
        }
        if (functionsVisitedTruncated)
        {
            return "Not determined \u2013 the reachability traversal did not finish, so " +
                   "unreachable functions cannot be identified yet.";
        }
        if (definitionsTruncated)
        {
            return "Not determined \u2013 collecting the file's function definitions hit " +
                   "its budget, so unreachable functions cannot be identified yet.";
        }
        return "(none)";
    }
}

// Pure, unit-testable navigation-range selection: prefers the symbol-name
// SelectionRange over the full declaration Range for navigation (so the
// caret lands on the actual name, not the whole declaration span), falling
// back to Range only when SelectionRange is absent or not a well-formed,
// non-inverted range. No VS/file-system API is touched here -- the tool
// window still performs its own full validation (including URI/file
// existence) on whichever range this selects before ever navigating.
internal static class EntryPointDataFlowNavigation
{
    internal static CompilationSourceRangeModel SelectNavigationRange(
        CompilationSourceRangeModel range,
        CompilationSourceRangeModel selectionRange)
        => IsWellFormedRange(selectionRange) ? selectionRange : range;

    internal static bool IsWellFormedRange(CompilationSourceRangeModel range)
    {
        if (range?.Start == null || range.End == null)
        {
            return false;
        }
        if (!IsInt32Position(range.Start.Line) ||
            !IsInt32Position(range.Start.Character) ||
            !IsInt32Position(range.End.Line) ||
            !IsInt32Position(range.End.Character))
        {
            return false;
        }
        return range.End.Line > range.Start.Line ||
               (range.End.Line == range.Start.Line &&
                range.End.Character >= range.Start.Character);
    }

    private static bool IsInt32Position(long value) => value >= 0 && value <= int.MaxValue;
}

// The bridge decouples the WPF tool window (Bootstrap assembly) from the
// language client (Client assembly), mirroring CompilationInfoBridge and
// PreprocessorExplorerBridge. There is no hover trigger for this feature, so
// only a request handler is registered; presentation is driven entirely by
// the Tools command and its refresh hooks.
public static class EntryPointDataFlowBridge
{
    private static Func<Uri, CancellationToken, Task<EntryPointDataFlowModel>> request;

    public static void Register(
        Func<Uri, CancellationToken, Task<EntryPointDataFlowModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<EntryPointDataFlowModel> RequestAsync(
        Uri uri,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<EntryPointDataFlowModel>(null)
            : handler(uri, cancellationToken);
    }
}

// Pure decision helpers extracted from HlslBootstrapPackage's Entry-Point
// Data Flow show/refresh methods so they can be unit tested without a live
// VS instance (HlslBootstrapPackage itself extends AsyncPackage and cannot
// be constructed in a test host).
internal static class EntryPointDataFlowRefreshLogic
{
    // A failed request should only preserve the tool window's last
    // successful content when the window, before this request started,
    // already showed that exact same document -- never merely because the
    // caller happened to already hold a window reference. Relying on the
    // latter (existingWindow != null) previously erased good content on a
    // failed manual retry through the explicit Tools command, which always
    // constructs its own request without an existing-window reference even
    // when the window is already open and showing the requested document.
    internal static bool ShouldPreserveContentOnFailure(Uri priorDocumentUri, Uri requestedUri)
        => priorDocumentUri != null &&
           requestedUri != null &&
           priorDocumentUri.Equals(requestedUri);

    // Whether ShowEntryPointDataFlowAsync must invoke ShowToolWindowAsync
    // (which both creates-if-missing and reveals/activates an
    // existing-but-hidden pane) rather than reusing a window reference it
    // already resolved for the preserve-on-failure decision above. A prior
    // version used "existingWindow ?? priorWindow" as the window to present
    // with, which let a non-showing FindToolWindowAsync lookup (done only
    // to read DocumentUri for the preserve decision) silently stand in for
    // the caller's own reference and skip ShowToolWindowAsync entirely --
    // leaving an already-open-but-hidden pane hidden after an explicit
    // Tools-command invocation. The rule is simply: reveal whenever the
    // caller did not supply its own already-resolved window (every explicit
    // command invocation passes none); a background refresh always supplies
    // its own and must never force a reveal/steal focus.
    internal static bool ShouldRevealToolWindow(bool existingWindowSupplied)
        => !existingWindowSupplied;

    // Conservative relevance test for a saved file path: a configured
    // HLSL/header extension, or a file literally named
    // shadertoolsconfig.json regardless of its folder. Both can change the
    // reported data flow for the currently shown document even when neither
    // is that document itself (an included header's declarations/global
    // accesses are reported against their own uri, and the config file
    // drives the active variant's entry point/defines/include paths), so a
    // save is treated as relevant by file type rather than requiring an
    // exact match against the window's own root document.
    internal static bool IsHlslOrConfigRelevantPath(
        string path,
        IEnumerable<string> configuredExtensions)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return false;
        }
        try
        {
            if (string.Equals(
                    Path.GetFileName(path),
                    "shadertoolsconfig.json",
                    StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
            var extension = Path.GetExtension(path);
            if (string.IsNullOrEmpty(extension) || configuredExtensions == null)
            {
                return false;
            }
            foreach (var configured in configuredExtensions)
            {
                if (string.Equals(configured, extension, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }
            return false;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }
}

// Coordinates the race between an explicit Tools-command request and a
// concurrent background refresh trigger (save, active-variant change, or a
// debounced unsaved edit): an explicit request must never be superseded by
// a background refresh, but a background refresh that arrives while the
// explicit request is in flight must not simply be dropped either, or the
// window can be left permanently stale immediately after the explicit
// request completes. Callers record a deferred refresh via
// TryBeginBackgroundRefresh returning false, then replay it once via
// ExitExplicitRequest's return value once the explicit request finishes.
//
// All three operations are linearized under a single lock rather than
// using independent Interlocked operations on separate fields: a prior
// version used Interlocked.Read/Exchange on two separate fields, which left
// a genuine check-then-act race -- TryBeginBackgroundRefresh could read the
// explicit-request count as non-zero, then be preempted before it recorded
// the deferred flag; if ExitExplicitRequest ran to completion (consuming a
// still-false deferred flag) during that window, the preempted call would
// then resume and set the deferred flag with no explicit request left in
// flight to ever consume it, silently losing the refresh. A single lock
// around the whole read-then-write sequence of each operation makes that
// interleaving impossible: the state (count, deferred flag) only ever
// changes as one atomic transition. Thread-safe and independent of any VS
// API, so it is directly unit testable.
internal sealed class EntryPointDataFlowRefreshGate
{
    private readonly object gate = new();
    private int explicitRequestsInFlight;
    private bool refreshPending;

    internal void EnterExplicitRequest()
    {
        lock (gate)
        {
            ++explicitRequestsInFlight;
        }
    }

    // Decrements the explicit-request count and returns whether a
    // background refresh was deferred while any explicit request was in
    // flight (clearing the deferred flag as it does so). A caller that
    // receives true must issue exactly one bounded follow-up refresh.
    internal bool ExitExplicitRequest()
    {
        lock (gate)
        {
            if (explicitRequestsInFlight > 0)
            {
                --explicitRequestsInFlight;
            }
            if (!refreshPending)
            {
                return false;
            }
            refreshPending = false;
            return true;
        }
    }

    // Returns true if a background refresh may proceed immediately. Returns
    // false, having recorded the refresh as pending, if an explicit request
    // is currently in flight.
    internal bool TryBeginBackgroundRefresh()
    {
        lock (gate)
        {
            if (explicitRequestsInFlight != 0)
            {
                refreshPending = true;
                return false;
            }
            return true;
        }
    }
}
