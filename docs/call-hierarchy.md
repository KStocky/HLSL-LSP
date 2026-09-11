# Call hierarchy and entry-point data flow

The custom editor views retain the last good result across refreshes and expose
the shared tracked-root/document freshness/Refresh behavior described in
[analysis result freshness](analysis-freshness.md). A not-callable or
not-found response is still **Current**.

HLSL-LSP implements the standard LSP call hierarchy (`textDocument/
prepareCallHierarchy`, `callHierarchy/incomingCalls`,
`callHierarchy/outgoingCalls`) plus one custom request,
`hlsl/entryPointDataFlow`, that traces every function reachable from the
document's configured entry point and reports which globals/resources it
reads or writes. Both are compiler-authoritative: every symbol, call edge,
and read/write classification comes directly from DXC's `IDxcIntelliSense`
cursor trees (`GetReferencedCursor`, `GetDefinitionCursor`,
`FindReferencesInFile`, and the cursor-kind/expression-shape rules described
in `TranslationUnit::entry_point_data_flow`'s implementation comments).
HLSL-LSP does not parse HLSL itself, so anything DXC cannot expose (for
example, a template/generic instantiation DXC does not materialize a cursor
for) is absent rather than guessed.

The server advertises `callHierarchyProvider: true` in
`initialize`'s `capabilities`.

## Standard call hierarchy

### `textDocument/prepareCallHierarchy`

Parameters are the standard `TextDocumentPositionParams`. The document must
be open; the server analyzes its current (possibly unsaved) snapshot and
resolves the callable declaration/definition at the position. Positions that
do not resolve to a callable cursor (whitespace, a non-callable expression)
return `null`, matching the base LSP spec.

On success the result is a one-element array containing a
`CallHierarchyItem`:

```json
[
  {
    "name": "square",
    "kind": 12,
    "detail": "float square(float x)",
    "uri": "file:///C:/shaders/example.hlsl",
    "range": { "start": { "line": 3, "character": 0 }, "end": { "line": 3, "character": 40 } },
    "selectionRange": { "start": { "line": 3, "character": 6 }, "end": { "line": 3, "character": 12 } },
    "data": {
      "rootUri": "file:///C:/shaders/example.hlsl",
      "rootIdentity": "...",
      "rootVersion": 1,
      "generation": 3,
      "context": {
        "documentUri": "file:///C:/shaders/example.hlsl",
        "file": "example.hlsl",
        "activeVariant": null,
        "entryPoint": "main",
        "targetProfile": "ps_6_6"
      },
      "path": "C:/shaders/example.hlsl",
      "line": 4,
      "column": 7,
      "startOffset": 87,
      "cursorKind": 21,
      "name": "square"
    }
  }
]
```

### `callHierarchy/incomingCalls` / `callHierarchy/outgoingCalls`

Both accept `{ "item": <a CallHierarchyItem previously returned by this
server> }` and return an array of `CallHierarchyIncomingCall`
(`{ "from": CallHierarchyItem, "fromRanges": Range[] }`) or
`CallHierarchyOutgoingCall` (`{ "to": CallHierarchyItem, "fromRanges":
Range[] }`) respectively, exactly per the LSP spec. `fromRanges` are always
call-site ranges within the *caller's* body, in source order.

`callHierarchy/incomingCalls` expands across **every currently open root**
whose translation unit includes the callee's own source file, not only the
root the item happened to be prepared from -- the same cross-root expansion
`textDocument/references` already performs. This means opening two shader
files that both `#include` a shared header reports callers from both files
for a function defined in that header.

Because different roots can compile the very same shared source location
under different configurations (different active variants, `#define`s, or
entry points), a caller's *textual* location (`path` + offset) is **not**
sufficient to treat two roots' results for it as the same call site: the
compiler context that produced each result may differ in ways that change
what "the same code" even means (e.g. one root's config `#if`-excludes the
call, or resolves an overload differently). `incomingCalls` therefore never
merges results across roots -- each open root that reaches the callee
contributes its own, fully independent `CallHierarchyIncomingCall` entry
(with its own `from.data.rootIdentity`/`generation`), even when its `from`
happens to share a textual location with another root's entry. Only
*within* a single root's own contribution are duplicate call-site ranges to
the same caller collapsed into one entry with deduplicated `fromRanges`.
Clients that want to visually group or de-duplicate across roots should key
on `(path, startOffset, rootIdentity)`, not `(path, startOffset)` alone.

## `CallHierarchyItem.data`: stable identity envelope

Unlike a name or a bare source location, DXC's cursor trees have no
separate USR/identity concept, and HLSL freely overloads functions by
parameter type. To let a later `incomingCalls`/`outgoingCalls` request
re-resolve the *exact* callable a `prepareCallHierarchy` (or
`hlsl/entryPointDataFlow`) call found earlier -- not merely "whatever is at
this name/position now" -- every `CallHierarchyItem` carries an opaque
`data` object with:

| Field | Meaning |
| --- | --- |
| `rootUri` | The open document URI whose analysis produced this item. |
| `rootIdentity` | That document's canonical identity, used as the analysis cache key. |
| `rootVersion` | That document's version at analysis time. |
| `generation` | A process-wide monotonic counter bumped every time the root's translation unit is actually recompiled -- unlike `rootVersion`, this also changes when an `#include`d file the root depends on is edited, or when the active variant/compiler configuration changes, neither of which bumps the root document's own LSP version. |
| `context` | The effective shader context captured from the exact server submission/root that produced the item. Clients round-trip it unchanged so expanded outgoing items retain the same context; incoming items carry the independently captured context of their own candidate root. |
| `path` | The compiler-canonical (forward-slash) source path of the callable's own definition/declaration, as reported by DXC -- may differ from `rootUri`'s path when the callable lives in an `#include`d file. |
| `line`, `column` | The 1-based DXC source position of the callable's definition/declaration cursor. |
| `startOffset` | The UTF-8 byte offset of the callable's definition/declaration extent -- combined with `path` and `cursorKind`, this is the callable's identity key, matching how overloaded functions sharing one name are distinguished internally (`(location.path, start_offset, cursor_kind)`). |
| `cursorKind` | The DXC `DxcCursorKind` of the resolved cursor. |
| `name` | The callable's own name, used only for re-deriving ranges from the current snapshot text; never used for identity/lookup by itself, since names collide across overloads. |

`data` is server-owned and opaque to clients: it must be round-tripped
unmodified. Every handler that consumes it re-validates the item in two
independent ways before trusting it, and rejects a stale item with the
standard LSP `ContentModified` error rather than silently resolving
whatever now happens to be at that name or position -- this is the same
staleness contract `textDocument/rename`'s prepare step already uses:

1. **Generation check**: the root is re-analyzed (if needed) and its
   *current* `generation` must equal `data.generation`. This alone catches
   every case that actually invalidates the item -- root edits, included-file
   edits, and active-variant/config changes -- even when none of the fields
   below happen to change (for example, an included file gains a new
   trailing declaration that does not shift the callable's own offset).
2. **Identity re-resolution**: the server re-resolves a cursor at
   `(path, startOffset, cursorKind)` against the current snapshot and
   confirms it is still the *same* callable, guarding against the
   (already generation-covered, but independently checked in depth) case of
   an edit that reuses a stale generation for some other reason.

A stale item is always rejected outright; the server never falls back to
"whatever is now at this name/position" as a next-best guess.

## Bounded traversal, recursion, and cancellation

`outgoingCalls`/`incomingCalls` scan a single function body/reference set
per request and are inherently bounded by that function's own size.
`hlsl/entryPointDataFlow` (below) additionally bounds its whole-program
work with explicit budgets across every phase --
`EntryPointDataFlowLimits::max_functions_visited` (default 4096) for the
reachability traversal, `max_definitions_collected` (default 16384) for
how many top-level callable definitions are collected (used both to
resolve/disambiguate the configured entry point by name and to compute
`unreachableFunctions`), `max_global_accesses` (default 16384) for distinct
retained global/resource accesses, and
`max_unused_declaration_candidates` (default 4096) for how many top-level
declarations get an (expensive, per-declaration) `FindReferencesInFile`
unused-check -- and checks the request's cancellation token throughout each
of these phases (definition collection, the reachability BFS, cycle
detection, result conversion, unreachable-function classification, and the
unused-declaration scan), so a pathological or enormous call graph cannot
make a single request run unbounded work or become unresponsive to
cancellation in any phase, not only the initial traversal. Recursion
(direct or through a cycle) always terminates because each callable is
visited at most once; a recursive function is reported exactly once, at
its shortest discovered call-graph depth, rather than dropped or repeated
per cycle iteration.

## `hlsl/entryPointDataFlow`

This is the one custom request the issue calls for: given the document's
*already-configured* entry point (`hlsl.entryPoint` / the active variant's
entry point -- the same configuration `hlsl/compilationInfo` already
resolves), trace every function transitively reachable from it and report
the globals/resources those functions read or write, plus functions and
top-level declarations that are unused for the active variant. It exists
because this information spans the whole translation unit rather than one
position, so it does not fit the position-based call hierarchy request
shape.

### Design: no client-supplied entry point or duplicate compiler config

The request takes only `textDocument`, deliberately not a position or an
entry-point name/target-profile override:

```json
{ "textDocument": { "uri": "file:///C:/shaders/example.hlsl" } }
```

The server reuses the exact compiler arguments it already resolved for this
document (the same ones `hlsl/compilationInfo` reports back), so the graph
walk and the compiled program are always consistent -- a client can never
send a different profile/defines/entry point than what the document was
actually compiled with. This mirrors `hlsl/compilationInfo`'s existing
"no duplicate compiler config" contract. Unsaved edits and active-variant
changes are picked up automatically the next time this request is made, the
same way `hlsl/compilationInfo` already behaves.

Only translation-unit-level function *definitions* are considered valid
entry-point candidates: a name that resolves solely to a struct/class
method, constructor, conversion function, or function template is not a
valid entry point and is reported via `found: false`, even if its
unqualified name matches the configured entry point exactly (avoiding a
`Foo::main` method being silently mistaken for a top-level `main`). If more
than one valid top-level definition shares the configured name (for
example two differently-typed overloads), the request rejects the
ambiguity outright rather than picking one by traversal order, again via
`found: false` with an explanatory message -- the same "never guess" rule
`hlsl/entryPointDataFlow` applies everywhere else.

### Response shape

```json
{
  "found": true,
  "explanation": "",
  "entryPoint": { "...": "a CallHierarchyItem, see above" },
  "reachableFunctions": [
    {
      "function": { "...": "a CallHierarchyItem" },
      "depth": 0,
      "recursive": false
    }
  ],
  "unreachableFunctions": [
    { "...": "a CallHierarchyItem" }
  ],
  "unusedDeclarations": [
    {
      "name": "unusedHelper",
      "kind": 12,
      "uri": "file:///C:/shaders/example.hlsl",
      "range": { "start": { "line": 10, "character": 0 }, "end": { "line": 12, "character": 1 } },
      "selectionRange": { "start": { "line": 10, "character": 6 }, "end": { "line": 10, "character": 17 } }
    }
  ],
  "globalAccesses": [
    {
      "name": "InputTexture",
      "kind": 268,
      "uri": "file:///C:/shaders/example.hlsl",
      "range": { "start": { "line": 1, "character": 0 }, "end": { "line": 1, "character": 34 } },
      "selectionRange": { "start": { "line": 1, "character": 20 }, "end": { "line": 1, "character": 32 } },
      "qualifiedName": "InputTexture",
      "access": "read"
    }
  ],
  "truncated": false,
  "functionsVisitedTruncated": false,
  "definitionsTruncated": false,
  "globalAccessesTruncated": false,
  "unusedDeclarationsTruncated": false,
  "functionsVisited": 4
}
```

- **`found`** is `false` when no entry point is configured, a configured
  entry point name does not resolve to any function definition in the
  current unsaved snapshot, the only name matches are not valid
  translation-unit-level entry candidates (for example a struct/class
  method or constructor sharing the configured name), or the name is
  *ambiguous* -- shared by more than one valid top-level function
  definition (for example two overloads). In every case every other array
  is then empty and `explanation` is a human-readable reason suitable for
  direct display in an editor tool window (including which case applied,
  e.g. "ambiguous" vs "not found"). The "not found"/"ambiguous" cases are
  resolved from whatever top-level definitions `definitionsTruncated`
  reports as collected: if that budget was hit, `explanation` says so
  explicitly, since the configured entry point (or a second, colliding
  definition) may simply not have been examined yet -- `found: false` is
  never a claim that the *entire* document was searched when
  `definitionsTruncated` is `true`.
- **`entryPoint`** is a full `CallHierarchyItem` (identical shape to
  `prepareCallHierarchy`'s), so a client can feed it straight into
  `callHierarchy/outgoingCalls` to explore from there, or `null` when
  `found` is `false`.
- **`reachableFunctions`** lists every function transitively reachable from
  the entry point, including the entry point itself at depth 0, ordered by
  ascending depth then source location. `depth` is the shortest call-graph
  distance from the entry point. `recursive` is `true` when the function is
  (transitively) part of a call cycle reachable from the entry point,
  including direct self-recursion.
- **`unreachableFunctions`** lists every function *definition* in the
  translation unit not present in `reachableFunctions` -- dead code for the
  active entry point/variant specifically (a function can be unreachable
  for one variant and reachable for another). This list is only populated
  when both `functionsVisitedTruncated` and `definitionsTruncated` are
  `false` (see below) -- an unvisited function is never reported as
  unreachable when it might still be reachable, and dead-code
  classification is never attempted against an incomplete definition set.
- **`unusedDeclarations`** lists top-level declarations (functions and
  global variables/resources) that DXC reports zero references to anywhere
  in the current unsaved snapshot -- a compiler-verifiable,
  entry-point-independent dead-code signal distinct from
  `unreachableFunctions`. These entries are plain navigable locations
  (`name`/`kind`/`uri`/`range`/`selectionRange`), not `CallHierarchyItem`s,
  since they are not callables and do not need a round-trippable `data`
  envelope for a later call-hierarchy request.
- **`globalAccesses`** lists every global variable, cbuffer/tbuffer field,
  and resource read or written by any function in `reachableFunctions`,
  merged by declaration identity: a global touched from multiple reachable
  functions and/or access kinds is reported once with the most conservative
  combined kind. `qualifiedName` disambiguates fields with the same short
  name across different cbuffers/structs.
- **`truncated`** is `true` when any of four independent bounded phases
  stopped early; it is a convenience "was anything incomplete" summary
  equal to `functionsVisitedTruncated || definitionsTruncated ||
  globalAccessesTruncated || unusedDeclarationsTruncated`. Because the four
  causes are independent and gate different, non-overlapping parts of the
  response, a client that needs to know *which* section(s) may be
  incomplete -- to render an accurate, section-specific warning instead of
  one blanket "results may be incomplete" banner -- **should read the four
  specific fields below, not `truncated` alone**:
  - **`functionsVisitedTruncated`** is `true` when the reachability
    traversal itself hit
    `EntryPointDataFlowLimits::max_functions_visited` before exhausting the
    call graph. `reachableFunctions` is then a conservative subset of the
    true reachable set (never a superset), and **`unreachableFunctions` is
    left completely empty** -- with an incomplete reachability traversal,
    "not yet visited" cannot be distinguished from "provably unreachable",
    so no function is reported as dead code unless the full call graph
    from the entry point was exhausted. `globalAccesses` may also be
    incomplete when this is true, since access scanning only covers
    visited functions.
  - **`definitionsTruncated`** is `true` when collecting the translation
    unit's top-level callable *definitions* (used both to resolve/
    disambiguate the configured entry point by name and to compute
    `unreachableFunctions`) hit
    `EntryPointDataFlowLimits::max_definitions_collected`, independently of
    `functionsVisitedTruncated` -- a translation unit can have a tiny,
    fully-explored reachable call graph and still an enormous number of
    unrelated dead function definitions. **`unreachableFunctions` is left
    completely empty** whenever this is `true`, for the same reason as
    under `functionsVisitedTruncated`: a definition that was never
    collected cannot be told apart from one that was collected and found
    reachable. Entry point resolution still uses whatever definitions were
    collected before the budget was hit -- this includes the two failure
    responses below: `definitionsTruncated`/`truncated` are set as soon as
    definition collection finishes, *before* entry point resolution is
    attempted, so a `found: false` response (entry point name did not
    resolve to a top-level function definition, or resolved ambiguously)
    is never presented as a definitive, complete result when it was in
    fact reached with an incomplete definition set: the configured entry
    point (or a second, colliding definition that would have made the
    result ambiguous instead) may simply not have been among the
    definitions collected before the budget was hit. `explanation` notes
    this explicitly in that case.
  - **`globalAccessesTruncated`** is `true` when the global/resource access
    scan hit `EntryPointDataFlowLimits::max_global_accesses` and stopped
    *retaining* further distinct accesses (the reachability traversal
    itself still runs to completion, independent of this flag).
    `globalAccesses` is then a conservative subset of the true set of
    accesses made by `reachableFunctions` (never a superset). This is
    independent of whether `reachableFunctions`/`unreachableFunctions` are
    themselves complete.
  - **`unusedDeclarationsTruncated`** is `true` when the
    unused-top-level-declaration scan hit
    `EntryPointDataFlowLimits::max_unused_declaration_candidates` and
    stopped issuing further `FindReferencesInFile` lookups.
    `unusedDeclarations` is then a conservative subset of the true set of
    unused declarations (never a false "unused" claim -- only possible
    under-reporting), independent of the other three flags.

  `functionsVisited` reports how many functions the reachability traversal
  actually visited.

### Conservative read/write classification

`access` is `"read"`, `"write"`, or `"readWrite"`. Classification follows
one rule throughout: **whenever DXC's cursor tree does not prove an access
is read-only or write-only, it is reported as `readWrite`** rather than
guessed narrower. This is a deliberate, documented over-approximation, not
an implementation shortcut -- callers that need a conservative "could this
be written" answer for hazard analysis, resource barrier placement, or
similar tooling can rely on `"write"`/`"readWrite"` never omitting a real
write, and `"read"`/`"readWrite"` never omitting a real read. Concretely
(see `TranslationUnit::entry_point_data_flow`'s implementation comments for
the full empirically-derived rule set): a plain load use is a read; an
assignment target or `inout`/`out` argument is a write; a value used as
both (for example written then read back, or passed to an `inout`
parameter whose callee both reads and writes it) is `readWrite`; and any
access DXC's cursor shape does not clearly disambiguate is `readWrite` by
default. A method call's read-only-by-name inference (e.g. `tex.Sample(...)`,
`buf.Load(...)`) is applied only once the receiver's own declared type is
proven to be one of HLSL's compiler builtin resource/sampler types --
never merely by method name -- so a user-defined struct or class declaring
its own method that happens to share one of these names (which may mutate
state in ways the cursor tree cannot disprove) is always conservatively
`readWrite`, regardless of its name.

### Includes and unsaved edits

Because the request always re-analyzes the document's current in-memory
snapshot (including unsaved edits to the root document and to any
`#include`d files open elsewhere in the workspace), no separate
"unsaved buffer" parameter is needed. Locations in `unusedDeclarations` and
`globalAccesses` may point into an `#include`d file distinct from the
requested document's own `uri`; each entry's own `uri` reflects that.

### Cancellation and staleness

The request checks the standard cancellation token at explicit checkpoints
in every phase of the traversal (definition collection, the reachability
BFS, cycle detection, result conversion, unreachable-function
classification, and the unused-declaration scan), not only in the initial
walk, and returns the standard LSP request-cancelled error if cancelled at
any of them. If the document -- or an `#include`d file it depends on, or
the active variant/compiler configuration -- changes while the request's
analysis is in flight, the request returns `ContentModified` rather than a
result computed against a snapshot that no longer matches what the client
has open, using the same `generation` mechanism described above -- the same
safeguard `hlsl/compilationInfo` and other analysis requests already
apply.

## Known DXC limitations

- Template/generic HLSL constructs are only represented to the extent DXC's
  `IDxcIntelliSense` exposes an instantiated/referenced cursor for them; a
  construct DXC does not materialize a cursor for is simply absent from the
  reported graph rather than textually guessed at.
- `unusedDeclarations` relies on `FindReferencesInFile` reporting zero
  references; a declaration referenced only from a file DXC did not need
  to parse for the active variant (for example, code behind an inactive
  `#if`) may be under-reported as unused, or -- if that inactive code is
  never compiled in for any variant -- correctly reported as such.
- `unusedDeclarations` is also bounded by
  `EntryPointDataFlowLimits::max_unused_declaration_candidates` (default
  4096), since each candidate requires its own `FindReferencesInFile` call;
  a translation unit with more top-level declarations than that limit may
  have `truncated: true` with some declarations never checked (and
  therefore never reported, even if actually unused). This never produces
  a false positive -- an entry present in `unusedDeclarations` is always
  genuinely unreferenced -- only possible under-reporting.
