import { escapeHtml } from "./compilationInfo";

// Renders the `hlsl/entryPointDataFlow` response: the whole-program
// reachability walk from the document's already-configured entry point,
// plus the globals/resources reachable functions read or write, and the
// unreachable-function/unused-declaration dead-code signals. Every
// navigable location in this view (an entry point, a reachable/unreachable
// function, an unused declaration, or a global/resource access) comes
// verbatim from the server's own `CallHierarchyItem`/navigable-symbol
// shapes against the current (possibly unsaved) document snapshot -- this
// module never derives a source location by searching text or by matching
// a name itself. See docs/call-hierarchy.md for the full protocol shape.

export interface EntryPointDataFlowPosition {
  readonly line: number;
  readonly character: number;
}

export interface EntryPointDataFlowRange {
  readonly start: EntryPointDataFlowPosition;
  readonly end: EntryPointDataFlowPosition;
}

// The opaque, server-owned identity envelope a `CallHierarchyItem` carries
// so a later `callHierarchy/outgoingCalls` (or another
// `hlsl/entryPointDataFlow`) request can re-resolve the exact callable this
// item was resolved from. This view never reads or reinterprets any of
// these fields itself: it is round-tripped unmodified through the one
// `command:` URI this module defines, exactly like the other fields on
// `CallHierarchyItem`.
export interface CallHierarchyItemData {
  readonly rootUri: string;
  readonly rootIdentity: string;
  readonly rootVersion: number;
  // Pins the exact compiled snapshot this callable was resolved from. An
  // included-file edit or a configuration/active-variant change can
  // reparse the root document without changing `rootVersion`, so
  // `rootVersion` alone cannot detect a stale round-tripped item; the
  // server compares this against its own current analysis generation
  // when the item is later round-tripped back (e.g. through the
  // navigation command or a standard `callHierarchy/outgoingCalls`
  // request).
  readonly generation: number;
  readonly path: string;
  readonly line: number;
  readonly column: number;
  readonly startOffset: number;
  readonly cursorKind: number;
  readonly name: string;
}

export interface CallHierarchyItem {
  readonly name: string;
  readonly kind: number;
  readonly detail: string;
  readonly uri: string;
  readonly range: EntryPointDataFlowRange;
  readonly selectionRange: EntryPointDataFlowRange;
  readonly data: CallHierarchyItemData;
}

export interface ReachableFunctionEntry {
  readonly function: CallHierarchyItem;
  // Shortest call-graph distance from the entry point (the entry point
  // itself is depth 0).
  readonly depth: number;
  // True when this function is (transitively) part of a call cycle
  // reachable from the entry point, including direct self-recursion.
  readonly recursive: boolean;
}

// A plain navigable location for something that is not a callable (a
// top-level declaration or a global/resource access site) and so carries no
// round-trippable `data` envelope for a later call-hierarchy request.
export interface NavigableSymbol {
  readonly name: string;
  readonly kind: number;
  readonly uri: string;
  readonly range: EntryPointDataFlowRange;
  readonly selectionRange: EntryPointDataFlowRange;
}

export type GlobalAccessKind = "read" | "write" | "readWrite";

export interface GlobalAccessEntry extends NavigableSymbol {
  // Disambiguates fields with the same short `name` across different
  // cbuffers/structs; only shown when it differs from `name`.
  readonly qualifiedName: string;
  readonly access: GlobalAccessKind;
}

export interface EntryPointDataFlow {
  // False when no entry point is configured, or a configured entry point
  // name does not resolve to any function definition in the current
  // unsaved snapshot; every other array is then empty and `explanation` is
  // a human-readable reason suitable for direct display.
  readonly found: boolean;
  readonly explanation: string;
  readonly entryPoint: CallHierarchyItem | null;
  // Every function transitively reachable from the entry point, including
  // the entry point itself at depth 0, ordered by ascending depth then
  // source location.
  readonly reachableFunctions: readonly ReachableFunctionEntry[];
  // Every function definition in the translation unit not present in
  // `reachableFunctions` -- dead code for the active entry point/variant.
  // Guaranteed empty (never a false "definitely unreachable" claim) when
  // either `functionsVisitedTruncated` or `definitionsTruncated` is true:
  // with an incomplete traversal, "not yet visited" cannot be
  // distinguished from "provably unreachable"; with an incomplete
  // definition set, a never-collected definition cannot be told apart
  // from one collected and found reachable.
  readonly unreachableFunctions: readonly CallHierarchyItem[];
  // Top-level declarations (functions and global variables/resources) DXC
  // reports zero references to anywhere in the current unsaved snapshot.
  readonly unusedDeclarations: readonly NavigableSymbol[];
  // Every global variable, cbuffer/tbuffer field, and resource read or
  // written by any function in `reachableFunctions`, merged by declaration
  // identity.
  readonly globalAccesses: readonly GlobalAccessEntry[];
  // Convenience "was anything incomplete" summary: exactly
  // `functionsVisitedTruncated || definitionsTruncated ||
  // globalAccessesTruncated || unusedDeclarationsTruncated`. The four
  // phases are independent and gate different, non-overlapping parts of
  // the response, so rendering must read the four specific flags below --
  // not this one alone -- to show an accurate, section-specific
  // incomplete state instead of one blanket warning.
  readonly truncated: boolean;
  // True when the reachability traversal itself hit its function-visit
  // limit before exhausting the call graph. `reachableFunctions` is then a
  // conservative subset (never a superset) of the true reachable set, and
  // `unreachableFunctions` is always empty -- never render an empty
  // `unreachableFunctions` as "(no unreachable functions)" when this is
  // true; it means "unknown", not "none". `globalAccesses` may also be
  // incomplete when this is true, since access scanning only covers
  // visited functions.
  readonly functionsVisitedTruncated: boolean;
  // True when collecting the translation unit's top-level callable
  // *definitions* (used both to resolve/disambiguate the configured entry
  // point by name and to compute `unreachableFunctions`) hit its
  // definition-collection limit, independently of
  // `functionsVisitedTruncated` -- a translation unit can have a tiny,
  // fully-explored reachable call graph and still an enormous number of
  // unrelated dead function definitions. `unreachableFunctions` is always
  // empty when this is true, for the same reason as under
  // `functionsVisitedTruncated`: a definition that was never collected
  // cannot be told apart from one that was collected and found reachable.
  // Entry point resolution still uses whatever definitions were collected
  // before the budget was hit, so `reachableFunctions`/`globalAccesses`
  // are unaffected by this flag on its own.
  readonly definitionsTruncated: boolean;
  // True when the global/resource access scan hit its retention limit and
  // stopped retaining further distinct accesses (the reachability
  // traversal itself still ran to completion, independent of this flag).
  // `globalAccesses` is then a conservative subset of the true set of
  // accesses -- never render an empty `globalAccesses` as "(no global or
  // resource accesses)" when this (or `functionsVisitedTruncated`) is
  // true.
  readonly globalAccessesTruncated: boolean;
  // True when the unused-top-level-declaration scan hit its candidate
  // limit and stopped issuing further reference lookups.
  // `unusedDeclarations` is then a conservative subset of the true set of
  // unused declarations -- never render an empty `unusedDeclarations` as
  // "(no unused declarations)" when this is true. Independent of the
  // other three flags.
  readonly unusedDeclarationsTruncated: boolean;
  readonly functionsVisited: number;
}

// The one command this view's webview may invoke through a plain
// `command:` URI. Webview panels pass
// `enableCommandUris: [openEntryPointDataFlowLocationCommand]` (never
// `true`) so no other command can ever be triggered from this view's
// static HTML, and `enableScripts` stays `false` throughout -- no script
// execution is needed at all for navigation.
export const openEntryPointDataFlowLocationCommand =
  "hlsl.entryPointDataFlow.openLocation";

export interface EntryPointDataFlowSourceLocation {
  readonly uri: string;
  readonly range: EntryPointDataFlowRange;
}

// The exact `hlsl/entryPointDataFlow` request params shape (see
// docs/call-hierarchy.md): only `textDocument.uri`, deliberately never a
// position or an entry-point/target override -- the server always reuses
// the compiler arguments it already resolved for this document. Exported
// as a pure function so its exact shape can be asserted directly in unit
// tests without a real `vscode-languageclient` connection.
export interface EntryPointDataFlowRequestParams {
  readonly textDocument: { readonly uri: string };
}

export function entryPointDataFlowRequestParams(
  uri: string,
): EntryPointDataFlowRequestParams {
  return { textDocument: { uri } };
}

function locationCommandUri(
  location: EntryPointDataFlowSourceLocation,
): string {
  const args = encodeURIComponent(JSON.stringify([location]));
  return `command:${openEntryPointDataFlowLocationCommand}?${args}`;
}

// Renders `label` as a plain escaped string, or -- only when `location` is
// provided -- as a link that invokes `openEntryPointDataFlowLocationCommand`
// through a `command:` URI. Callers must only pass a `location` sourced
// directly from the report (a `CallHierarchyItem`'s own uri/selectionRange,
// or a navigable symbol's own uri/selectionRange): never a value derived by
// matching text or a name.
function locationLink(
  label: string,
  location: EntryPointDataFlowSourceLocation | undefined,
): string {
  const text = escapeHtml(label);
  if (location === undefined) {
    return text;
  }
  return `<a href="${escapeHtml(locationCommandUri(location))}" title="Go to declaration">${text}</a>`;
}

function callableLink(item: CallHierarchyItem): string {
  return locationLink(item.name, { uri: item.uri, range: item.selectionRange });
}

function symbolLink(symbol: NavigableSymbol, label: string): string {
  return locationLink(label, {
    uri: symbol.uri,
    range: symbol.selectionRange,
  });
}

// A small, presentational-only subset of the standard LSP `SymbolKind`
// numbers this server's `symbol_kind()` can report for a callable/global
// declaration. Purely cosmetic: never used to decide identity or
// navigation, both of which always come from the server's own uri/range.
const kindLabels: Readonly<Record<number, string>> = {
  3: "Namespace",
  5: "Class",
  6: "Method",
  8: "Field",
  9: "Constructor",
  10: "Enum",
  12: "Function",
  13: "Variable",
  14: "Constant",
  22: "Enum member",
  23: "Struct",
  25: "Operator",
  26: "Type parameter",
};

function kindLabel(kind: number): string {
  return kindLabels[kind] ?? "Symbol";
}

const accessLabels: Record<GlobalAccessKind, string> = {
  read: "Read",
  write: "Write",
  readWrite: "Read/Write",
};

function reachableRow(entry: ReachableFunctionEntry): string {
  const recursiveBadge = entry.recursive
    ? ` <span class="badge recursive" title="Part of a call cycle reachable from the entry point">recursive</span>`
    : "";
  return `<tr><td>${callableLink(entry.function)}${recursiveBadge}</td><td>${escapeHtml(entry.function.detail)}</td><td>${String(entry.depth)}</td></tr>`;
}

function unreachableRow(item: CallHierarchyItem): string {
  return `<tr><td>${callableLink(item)}</td><td>${escapeHtml(item.detail)}</td></tr>`;
}

function unusedDeclarationRow(symbol: NavigableSymbol): string {
  return `<tr><td>${symbolLink(symbol, symbol.name)}</td><td>${escapeHtml(kindLabel(symbol.kind))}</td></tr>`;
}

function globalAccessRow(entry: GlobalAccessEntry): string {
  const nameLabel =
    entry.qualifiedName !== "" && entry.qualifiedName !== entry.name
      ? entry.qualifiedName
      : entry.name;
  const link = symbolLink(entry, nameLabel);
  const accessBadge = `<span class="badge access-${entry.access}">${accessLabels[entry.access]}</span>`;
  return `<tr><td>${link}</td><td>${accessBadge}</td><td>${escapeHtml(kindLabel(entry.kind))}</td></tr>`;
}

function reachableSection(flow: EntryPointDataFlow): string {
  // The reachability traversal (never a superset when incomplete) also
  // gates this section: an empty result while it was cut short means
  // "none recorded yet", not "provably none".
  const truncatedNote = flow.functionsVisitedTruncated
    ? `<p class="truncated">The reachability traversal stopped early: this list may be an incomplete, conservative subset of the true reachable set.</p>`
    : "";
  const rows =
    flow.reachableFunctions.length === 0
      ? `<tr><td colspan="3" class="muted">${
          flow.functionsVisitedTruncated
            ? "(none recorded before the traversal stopped)"
            : "(no reachable functions)"
        }</td></tr>`
      : flow.reachableFunctions.map((entry) => reachableRow(entry)).join("");
  return `<section>
<h2>Reachable functions</h2>
${truncatedNote}
<table>
<thead><tr><th>Function</th><th>Signature</th><th>Depth</th></tr></thead>
<tbody>${rows}</tbody>
</table>
</section>`;
}

function globalAccessesSection(flow: EntryPointDataFlow): string {
  // Access scanning only covers visited functions, so an incomplete
  // reachability traversal can leave this incomplete too, independent of
  // its own retention limit -- either flag means this list must not be
  // presented as complete.
  const incomplete =
    flow.globalAccessesTruncated || flow.functionsVisitedTruncated;
  const truncatedNote = incomplete
    ? `<p class="truncated">${
        flow.globalAccessesTruncated
          ? "The global/resource access scan stopped retaining further accesses: this list may be an incomplete, conservative subset of the true set."
          : "The reachability traversal stopped early, so this scan (which only covers visited functions) may be an incomplete, conservative subset of the true set."
      }</p>`
    : "";
  const rows =
    flow.globalAccesses.length === 0
      ? `<tr><td colspan="3" class="muted">${
          incomplete
            ? "(none retained before the scan stopped -- this list may be incomplete)"
            : "(no global or resource accesses)"
        }</td></tr>`
      : flow.globalAccesses.map((entry) => globalAccessRow(entry)).join("");
  return `<section>
<h2>Global &amp; resource accesses</h2>
${truncatedNote}
<table>
<thead><tr><th>Name</th><th>Access</th><th>Kind</th></tr></thead>
<tbody>${rows}</tbody>
</table>
</section>`;
}

function unreachableSection(flow: EntryPointDataFlow): string {
  // The server always leaves `unreachableFunctions` empty when either the
  // reachability traversal or the definition-collection scan didn't
  // finish, specifically so a client never asserts a function is dead
  // code it merely never got to visit, or never collected as a candidate
  // in the first place. Rendering the ordinary empty state here
  // ("(no unreachable functions)") would misreport "unknown" as "none" --
  // show an explicit incomplete state instead, and skip the
  // (guaranteed-empty) table entirely.
  if (flow.functionsVisitedTruncated || flow.definitionsTruncated) {
    const reasons: string[] = [];
    if (flow.functionsVisitedTruncated) {
      reasons.push("the reachability traversal stopped early");
    }
    if (flow.definitionsTruncated) {
      reasons.push("the definition-collection scan stopped early");
    }
    return `<section>
<h2>Unreachable functions</h2>
<p class="muted">Function definitions in the translation unit not reachable from the entry point for the active variant.</p>
<p class="truncated">Unknown: ${reasons.join(" and ")}, so unreachable functions could not be determined.</p>
</section>`;
  }
  const rows =
    flow.unreachableFunctions.length === 0
      ? `<tr><td colspan="2" class="muted">(no unreachable functions)</td></tr>`
      : flow.unreachableFunctions.map((item) => unreachableRow(item)).join("");
  return `<section>
<h2>Unreachable functions</h2>
<p class="muted">Function definitions in the translation unit not reachable from the entry point for the active variant.</p>
<table>
<thead><tr><th>Function</th><th>Signature</th></tr></thead>
<tbody>${rows}</tbody>
</table>
</section>`;
}

function unusedDeclarationsSection(flow: EntryPointDataFlow): string {
  const truncatedNote = flow.unusedDeclarationsTruncated
    ? `<p class="truncated">The unused-declaration scan stopped early: this list may be an incomplete, conservative subset of the true set of unused declarations.</p>`
    : "";
  const rows =
    flow.unusedDeclarations.length === 0
      ? `<tr><td colspan="2" class="muted">${
          flow.unusedDeclarationsTruncated
            ? "(none found before the scan stopped -- this list may be incomplete)"
            : "(no unused declarations)"
        }</td></tr>`
      : flow.unusedDeclarations
          .map((symbol) => unusedDeclarationRow(symbol))
          .join("");
  return `<section>
<h2>Unused declarations</h2>
<p class="muted">Top-level declarations DXC reports zero references to anywhere in the current snapshot.</p>
${truncatedNote}
<table>
<thead><tr><th>Name</th><th>Kind</th></tr></thead>
<tbody>${rows}</tbody>
</table>
</section>`;
}

// Derives a short display label (the final path segment) from a document
// URI, purely for the header/title -- never used for navigation.
export function documentLabel(documentUri: string): string {
  const withoutQuery = documentUri.split(/[?#]/)[0] ?? documentUri;
  const segments = withoutQuery
    .split("/")
    .filter((segment) => segment.length > 0);
  const last = segments.at(-1);
  return last === undefined ? documentUri : decodeURIComponent(last);
}

// Builds the specific, independent list of what the header banner should
// say was cut short -- read from the three granular flags (never from
// `truncated` alone, which is only a same-as-any-of-these-three summary)
// so the banner names the actual affected section(s) instead of one vague
// "something may be incomplete" message.
function truncationCauses(flow: EntryPointDataFlow): string[] {
  const causes: string[] = [];
  if (flow.functionsVisitedTruncated) {
    causes.push(
      "the reachability traversal stopped early (unreachable functions could not be determined, and reachable functions/global accesses may be an incomplete subset)",
    );
  }
  if (flow.definitionsTruncated) {
    causes.push(
      "the definition-collection scan stopped early (unreachable functions could not be determined, and entry point resolution only saw the definitions collected before the limit)",
    );
  }
  if (flow.globalAccessesTruncated) {
    causes.push(
      "the global/resource access scan stopped retaining further accesses (that list may be an incomplete subset)",
    );
  }
  if (flow.unusedDeclarationsTruncated) {
    causes.push(
      "the unused-declaration scan stopped early (that list may be an incomplete subset)",
    );
  }
  return causes;
}

function headerSection(flow: EntryPointDataFlow, documentUri: string): string {
  const label = escapeHtml(documentLabel(documentUri));
  if (!flow.found) {
    return `<h1>Entry-Point Data Flow: ${label}</h1>
<p class="not-found">${escapeHtml(flow.explanation || "No entry point is configured for this document.")}</p>`;
  }
  const entryPointLine =
    flow.entryPoint === null
      ? ""
      : `<p class="summary">Entry point: ${callableLink(flow.entryPoint)} <span class="muted">${escapeHtml(flow.entryPoint.detail)}</span></p>`;
  const causes = truncationCauses(flow);
  const truncatedBanner =
    causes.length > 0
      ? `<p class="truncated">Analysis was truncated: ${causes.join("; ")}. Functions visited before truncation: ${String(flow.functionsVisited)}.</p>`
      : `<p class="muted">Functions visited: ${String(flow.functionsVisited)}</p>`;
  return `<h1>Entry-Point Data Flow: ${label}</h1>
${entryPointLine}
${truncatedBanner}`;
}

export function entryPointDataFlowHtml(
  flow: EntryPointDataFlow,
  documentUri: string,
): string {
  const body = flow.found
    ? `${reachableSection(flow)}
${globalAccessesSection(flow)}
${unreachableSection(flow)}
${unusedDeclarationsSection(flow)}`
    : "";
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  h1 { font-size: 1.35rem; margin: 0 0 .5rem; }
  h2 { font-size: 1.05rem; margin: 1.5rem 0 .35rem; }
  a { color: var(--vscode-textLink-foreground); }
  .summary { color: var(--vscode-foreground); margin: .25rem 0; }
  .muted { color: var(--vscode-descriptionForeground); }
  .not-found { color: var(--vscode-editorWarning-foreground); margin: .5rem 0 1rem; }
  .truncated { color: var(--vscode-editorWarning-foreground); border-left: 3px solid var(--vscode-editorWarning-foreground); padding-left: .75rem; margin: .5rem 0 1rem; }
  table { border-collapse: collapse; width: 100%; max-width: 70rem; margin-bottom: .5rem; }
  th, td { border-bottom: 1px solid var(--vscode-panel-border); padding: .4rem .5rem; text-align: left; vertical-align: top; }
  th { color: var(--vscode-descriptionForeground); }
  .badge { font-size: .75rem; border-radius: .75rem; padding: .05rem .5rem; border: 1px solid transparent; margin-left: .35rem; }
  .badge.recursive { color: var(--vscode-editorWarning-foreground); border-color: var(--vscode-editorWarning-foreground); }
  .badge.access-read { color: var(--vscode-terminal-ansiGreen); border-color: var(--vscode-terminal-ansiGreen); }
  .badge.access-write { color: var(--vscode-terminal-ansiYellow); border-color: var(--vscode-terminal-ansiYellow); }
  .badge.access-readWrite { color: var(--vscode-editorWarning-foreground); border-color: var(--vscode-editorWarning-foreground); }
</style>
</head>
<body>
${headerSection(flow, documentUri)}
${body}
</body>
</html>`;
}

export function entryPointDataFlowErrorHtml(message: string): string {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  .unavailable { color: var(--vscode-editorWarning-foreground); }
</style>
</head>
<body>
<h1>Entry-Point Data Flow</h1>
<p class="unavailable">${escapeHtml(message)}</p>
</body>
</html>`;
}

export interface EntryPointDataFlowRefreshOutcome {
  // undefined means "leave the webview's currently displayed HTML alone",
  // used to keep the last successful content on screen through a failed or
  // cancelled refresh instead of regressing to a placeholder or an
  // out-of-date loading message.
  readonly html: string | undefined;
  readonly hasContent: boolean;
  readonly title: string | undefined;
}

// Pure decision logic for how an Entry-Point Data Flow panel should react
// to one hlsl/entryPointDataFlow attempt, mirroring
// resolvePreprocessorExplorerRefresh/resolveResourceBindingsRefresh/
// resolveCompilationInfoRefresh so all panels share the same
// never-regress-on-failure behavior while remaining independently tracked
// panels/state. `documentUri` is the panel's own tracked document uri
// (never derived from the response, which has no top-level uri field of
// its own -- only `entryPoint.data.rootUri`, absent when `found` is
// `false`).
export function resolveEntryPointDataFlowRefresh(
  hasContent: boolean,
  documentUri: string,
  flow: EntryPointDataFlow | null | undefined,
  failureMessage: string | undefined,
): EntryPointDataFlowRefreshOutcome {
  if (flow === null || flow === undefined) {
    if (hasContent) {
      return { html: undefined, hasContent: true, title: undefined };
    }
    return {
      html: entryPointDataFlowErrorHtml(
        failureMessage ??
          "The HLSL language server is not currently available.",
      ),
      hasContent: false,
      title: undefined,
    };
  }
  const entryPointName =
    flow.found && flow.entryPoint !== null
      ? flow.entryPoint.name
      : documentLabel(documentUri);
  return {
    html: entryPointDataFlowHtml(flow, documentUri),
    hasContent: true,
    title: `Entry-Point Data Flow: ${entryPointName}`,
  };
}

// The validated shape an `openEntryPointDataFlowLocationCommand` invocation
// must have before any `vscode.Uri`/document API touches it.
export interface ValidatedEntryPointDataFlowLocation {
  readonly uri: string;
  readonly range: EntryPointDataFlowRange;
}

function isNonNegativeInteger(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

function parsePosition(value: unknown): EntryPointDataFlowPosition | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  if (
    !isNonNegativeInteger(candidate.line) ||
    !isNonNegativeInteger(candidate.character)
  ) {
    return undefined;
  }
  return { line: candidate.line, character: candidate.character };
}

function parseRange(value: unknown): EntryPointDataFlowRange | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  const start = parsePosition(candidate.start);
  const end = parsePosition(candidate.end);
  if (start === undefined || end === undefined) {
    return undefined;
  }
  if (
    end.line < start.line ||
    (end.line === start.line && end.character < start.character)
  ) {
    return undefined;
  }
  return { start, end };
}

// Validates an untrusted argument received through the
// `openEntryPointDataFlowLocationCommand` command-message boundary before
// any `vscode.Uri.parse`/`openTextDocument`/`showTextDocument` call touches
// it. Returns `undefined` for anything that does not exactly match the
// expected `{uri, range}` shape (including a missing/empty uri, a
// malformed range, or an inverted range) -- the command handler must treat
// that as a validation failure rather than falling back to a guess.
export function parseEntryPointDataFlowLocationCommandArg(
  value: unknown,
): ValidatedEntryPointDataFlowLocation | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  if (typeof candidate.uri !== "string" || candidate.uri.length === 0) {
    return undefined;
  }
  const range = parseRange(candidate.range);
  if (range === undefined) {
    return undefined;
  }
  return { uri: candidate.uri, range };
}

// The one effect the `openEntryPointDataFlowLocationCommand` handler needs
// from the editor: open `location.uri` and reveal/select `location.range`.
// Kept as a narrow, injectable interface (rather than calling
// `vscode.workspace.openTextDocument`/`vscode.window.showTextDocument`
// directly from this module) so the command handler's own validation +
// error-reporting logic -- not just the pure argument parser above -- has
// a seam unit tests can drive without a real editor.
export interface EntryPointDataFlowNavigator {
  open(location: ValidatedEntryPointDataFlowLocation): Promise<void>;
}

export interface EntryPointDataFlowNavigationResult {
  readonly success: boolean;
  // Present exactly when `success` is `false`: the message the command
  // handler should surface to the user (e.g. through
  // `vscode.window.showErrorMessage`).
  readonly errorMessage: string | undefined;
}

// Validates an `openEntryPointDataFlowLocationCommand` invocation's raw
// argument and, only when it is well-formed, asks `navigator` to perform
// the actual navigation. Never falls back to guessing a location from a
// name: a malformed/missing argument, or a navigation failure the
// navigator itself reports (a parse error, a document that can no longer
// be opened, etc.), is reported as a failure instead.
export async function navigateEntryPointDataFlowLocation(
  rawArgument: unknown,
  navigator: EntryPointDataFlowNavigator,
): Promise<EntryPointDataFlowNavigationResult> {
  const location = parseEntryPointDataFlowLocationCommandArg(rawArgument);
  if (location === undefined) {
    return {
      success: false,
      errorMessage:
        "Unable to navigate: the entry-point data flow location was missing or malformed.",
    };
  }
  try {
    await navigator.open(location);
    return { success: true, errorMessage: undefined };
  } catch (error) {
    return {
      success: false,
      errorMessage: `Unable to navigate to the entry-point data flow location: ${
        error instanceof Error ? error.message : String(error)
      }`,
    };
  }
}
