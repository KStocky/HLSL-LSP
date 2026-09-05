# HLSL code actions (quick fixes)

HLSL-LSP implements the standard LSP `textDocument/codeAction` request to
offer quick fixes and deterministic recovery actions for the current
document's DXC diagnostics. Unlike the other cross-editor views in this
project, this feature intentionally uses only the standard LSP capability —
`codeActionProvider` — so both editors' generic LSP clients surface it
through their native "lightbulb" UI (`Ctrl+.`) with no HLSL-LSP-specific
client code.

## Compiler authority

Every quick fix comes from a DXC `IDxcDiagnostic` fix-it
(`IDxcDiagnostic::GetNumFixIts`/`GetFixItAt`) that HLSL-LSP has empirically
verified DXC 1.9.2607.13 actually emits for a given diagnostic family (see
`tests/dxc/intellisense_tests.cpp` for the verified cases). HLSL-LSP never
infers a fix by parsing HLSL syntax or diagnostic message text, and it never
fabricates a fix DXC did not supply. Fix-its are not available for every
diagnostic family — for example, undeclared identifiers with no close-match
suggestion, missing includes, and several other categories carry no fix-it —
and code actions are correspondingly omitted for those diagnostics rather
than synthesized from guesswork.

DXC's diagnostic range APIs (`IDxcDiagnostic::GetNumRanges`/`GetRangeAt`)
crash in the pinned DXC release whenever a diagnostic has any ranges, so
HLSL-LSP never calls them; diagnostic display ranges continue to come from
the single stable point location DXC does report, while fix-it edit ranges
come only from `IDxcSourceRange::GetStart`/`GetEnd` on the fix-it itself,
which do not exhibit the crash.

## Diagnostic correlation and staleness

Every published diagnostic carries an opaque `data` correlation payload
identifying the document's URI/version, the analysis generation, and the
diagnostic's index within that analysis — this is present on *every*
diagnostic (not only ones with a fix-it), because it is also how a
diagnostic with no fix-it (for example an unresolved `#include`, a candidate
for the recovery action below) is re-located at `codeAction` request time. A
`code` of `"hlsl-lsp/dxc-fix-it"` is added only to diagnostics that carry at
least one DXC fix-it; diagnostics without a fix-it have `data` but no `code`.
HLSL-LSP never trusts a client-echoed diagnostic or edit back from
`context.diagnostics`: at `codeAction` request time, it looks up its own
server-owned diagnostics for the document's *current* version and generation
and derives actions only from that authoritative state. If the server's
cached diagnostics do not match the document's current version/generation
exactly (for example, an edit raced ahead of background analysis, or a
configuration/variant reanalysis has started but not yet completed), the
request omits actions for that document rather than guessing or erroring.

A reanalysis (for example, one triggered by an unrelated configuration or
variant change) that reproduces byte-for-byte identical diagnostics at the
same document version is a cache hit: the server always refreshes its
internal generation for that document — so `codeAction` never treats its own
unchanged diagnostics as stale — but does not send another
`textDocument/publishDiagnostics` notification for content the client
already has. A first publish, a new document version, or genuinely
different diagnostics always publishes.

Each fix-it is revalidated again immediately before building its edit:
offsets must fall within the current document's bounds, the fix-it's
attributed range must be same-file (never cross-file), and DXC's
replacement text must be well-formed UTF-8. A diagnostic's fix-its are
sorted deterministically by start offset with a stable sort — preserving
DXC's own reported relative order for edits that end up adjacent, rather
than depending on an unspecified tie-break — and checked for overlap; if any
two edits from the same diagnostic overlap, *including two edits that start
at the exact same offset* (their relative order is inherently ambiguous, so
they are treated as an overlap rather than arbitrarily ordered), the *entire*
fix (however many edits it has) is rejected — a multi-edit fix is only ever
offered, and only ever applied, as a single all-or-nothing unit.

## Included files

DXC can attribute a diagnostic's *display range* to a `#include`d file, but
HLSL-LSP's quick-fix feature is scoped to the requested document only: a
fix-it whose attributed file differs from the document named in the
`textDocument/codeAction` request is always rejected, even when an
authoritative open snapshot exists for that other file. This is a deliberate,
bounded scope rather than a workaround — extending edits across files would
require correlating diagnostics/generations for the included document's own
identity and building a second versioned `TextDocumentEdit`, which the
current diagnostics-cache and action-derivation code does not do. Rejecting
these fix-its (instead of silently modifying unversioned disk content, or
guessing at a client-side buffer) keeps every applied edit traceable to a
specific document version.

In practice this has not been observed to narrow real fixes: every DXC
fix-it family verified against the pinned 1.9.2607.13 release (see
`tests/dxc/intellisense_tests.cpp`) reports its fix-it range against the root
translation unit's own text, not an included file's. If a future DXC release
is found to emit genuinely cross-file fix-its, this section — and the
implementation's root-only scope — will need to be revisited together.

## Protocol

Request:

```text
textDocument/codeAction
```

Parameters follow the standard LSP shape: `textDocument`, `range`, and
`context` (`diagnostics`, optional `only`). `context.only` is honored using
the LSP `CodeActionKind` hierarchy; since HLSL-LSP only ever produces the
unqualified `"quickfix"` kind, a request is matched when `"quickfix"` itself
starts with a requested kind (so requesting `"quickfix"` matches, and so does
a broader ancestor request, but a narrower request like `"quickfix.foo"` does
not). Returned actions are restricted to those whose diagnostic overlaps the
requested range and that still match a diagnostic in the server's current
state for that document.

Two kinds of action are returned:

- **Fix-it quick fixes** — `CodeAction.edit.documentChanges` containing a
  single versioned `TextDocumentEdit` (`textDocument.version` set to the
  snapshot's current version) with one or more non-overlapping `TextEdit`s.
- **Include/configuration recovery** — when a diagnostic's location falls on
  an unresolved `#include` path (detected structurally through the same
  include-resolution machinery used for go-to-definition, never by parsing
  the diagnostic message) and a different, already-known compilation variant
  is confirmed to resolve that same include, a `CodeAction.command` is
  returned instead of an edit: `"hlsl-lsp.selectVariant"` with the variant's
  name as its argument. HLSL-LSP never fabricates an include path that isn't
  already a known, resolvable variant configuration.

Every returned action includes the originating diagnostic in its
`diagnostics` array.

## Executing the recovery command

The `"hlsl-lsp.selectVariant"` command is advertised through the standard
`executeCommandProvider` capability and handled by `workspace/executeCommand`.
It applies the same active-variant switch as the editors' variant picker (see
[shadertoolsconfig.md](shadertoolsconfig.md)), so selecting it from a
lightbulb re-analyzes the document under the new variant exactly as if the
user had changed the setting directly.

Unlike a `hlsl/didChangeActiveVariant` notification sent *from* a client
(which already owns and durably records that state before telling the
server), `workspace/executeCommand` changes only the server's in-memory
active variant: nothing else durably records the new selection unless the
client is told. After applying the change, the server sends a
`hlsl/activeVariantChanged` notification (`{ "variant": string | null }`)
back to the client with its resulting authoritative value. Both shipped
clients treat this exactly like a user action through their existing
picker, so a lightbulb-driven selection is durable and equivalent, not
session-only:

- **VS Code** persists `hlsl.activeVariant` at the same configuration scope
  the picker itself would use (`Workspace` when a workspace folder is open,
  `Global` otherwise) and refreshes its status bar item. Persisting the
  setting goes through the same generic `onDidChangeConfiguration` pipeline
  the manual picker already uses, which resynchronizes *all* HLSL settings
  back to the server, including a `hlsl/didChangeActiveVariant` echo of the
  value the server itself just reported. This is intentional, harmless
  convergence, not a feedback loop: the server does not emit another
  `hlsl/activeVariantChanged` for the same value. Resynchronizing the other
  settings can schedule cache-hit reanalysis, but identical diagnostics are
  not republished.
- **Visual Studio** updates the language client's cached active variant (the
  same field the manual **Tools > HLSL Select Shader Variant** picker
  writes, reapplied through `InitializationOptions` after a controlled
  runtime restart) and refreshes the Shader Compilation / Resource Bindings
  windows if open, again without re-notifying the server.

See `clients/vscode/test/integration/index.ts` (the durable-variant recovery
test) and
`clients/visual-studio/HlslLsp.VisualStudio.Tests/HlslLanguageClientCustomMessageTests.cs`
(real `StreamJsonRpc` wire-level dispatch of `hlsl/activeVariantChanged`) for
the corresponding coverage.

## Lifecycle and cancellation

`textDocument/codeAction` validates its parameters like every other request,
participates in the same cancellation (`$/cancelRequest`) handling as the
rest of the server, and never blocks on background analysis: if analysis for
the requested version/generation has not completed yet, the request simply
returns no actions for the affected diagnostics rather than waiting or
guessing.
