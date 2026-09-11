// Renders the `hlsl/preprocessorExplorer` response: the resolved include
// graph, preprocessor-skipped regions, active macros, and effective
// configuration settings for one HLSL document. Every navigable location in
// this view (an #include directive's position, a resolved include file, a
// compiler-macro definition site, or a skipped region) comes verbatim from
// the server's own lexer/preprocessor/configuration state against the
// current (possibly unsaved) document snapshot -- this module never derives
// a source location by searching text itself.

export type PreprocessorFileSource = "open" | "disk";
export type PreprocessorIncludeKind = "quoted" | "angled" | "macro";
export type PreprocessorIncludeStatus =
  "resolved" | "missing" | "cyclic" | "dynamic";
export type PreprocessorMacroSource = "compiler" | "configuration";

export interface PreprocessorPosition {
  readonly line: number;
  readonly character: number;
}

export interface PreprocessorRange {
  readonly start: PreprocessorPosition;
  readonly end: PreprocessorPosition;
}

export interface PreprocessorInclude {
  readonly path: string;
  readonly line: number;
  readonly character: number;
  readonly kind: PreprocessorIncludeKind;
  readonly status: PreprocessorIncludeStatus;
  readonly resolvedUri?: string;
  readonly logicalPath?: string;
  readonly mapping?: string;
  readonly expandedPath?: string;
  readonly configurationMacro?: string;
  readonly configurationOrigin?: string;
  readonly configurationOriginUri?: string;
}

export interface PreprocessorFile {
  readonly uri: string;
  readonly logicalPath: string;
  readonly physicalPath: string;
  readonly source: PreprocessorFileSource;
  readonly includes: readonly PreprocessorInclude[];
}

export interface PreprocessorSkippedRegion {
  readonly uri: string;
  readonly start: PreprocessorPosition;
  readonly end: PreprocessorPosition;
}

export interface PreprocessorMacro {
  readonly name: string;
  readonly value: string;
  readonly source: PreprocessorMacroSource;
  readonly origin?: string;
  readonly originUri?: string;
  // Populated only when `source` is "compiler": the macro's own definition
  // site as reported by the compiler's preprocessor state. Never guessed
  // from `name` when absent (e.g. for a "configuration"-sourced macro,
  // which has no single definition site in source text).
  readonly uri?: string;
  readonly line?: number;
  readonly character?: number;
}

// Mirrors whatever JSON shape the underlying setting has: a scalar for a
// single-value setting (languageVersion, targetProfile, entryPoint), a
// string array for a list setting (includeDirectories,
// additionalArguments), or a string-keyed object for a mapping setting
// (virtualDirectoryMappings).
export type PreprocessorSettingValue =
  | string
  | number
  | boolean
  | readonly string[]
  | Readonly<Record<string, string>>;

export interface PreprocessorSetting {
  readonly name: string;
  readonly value: PreprocessorSettingValue;
  readonly origin: string;
  readonly originUri?: string;
}

export interface PreprocessorAnalysisCapability {
  readonly available: boolean;
  readonly reason?: string;
}

export interface PreprocessorCompilerAnalysis {
  readonly skippedRegions: PreprocessorAnalysisCapability;
  readonly compilerMacros: PreprocessorAnalysisCapability;
}

export interface PreprocessorExplorerReport {
  readonly context?: EffectiveShaderContext;
  readonly rootUri: string;
  readonly files: readonly PreprocessorFile[];
  readonly skippedRegions: readonly PreprocessorSkippedRegion[];
  readonly macros: readonly PreprocessorMacro[];
  readonly settings: readonly PreprocessorSetting[];
  readonly compilerAnalysis?: PreprocessorCompilerAnalysis;
  readonly diagnostics: readonly string[];
}

export function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

// The one command this view's webview may invoke through a plain
// `command:` URI. Webview panels pass
// `enableCommandUris: [openPreprocessorLocationCommand]` (never `true`) so
// no other command can ever be triggered from this view's static HTML, and
// `enableScripts` stays `false` throughout -- no script execution is needed
// at all for navigation.
export const openPreprocessorLocationCommand =
  "hlsl.preprocessorExplorer.openLocation";

export interface PreprocessorSourceLocation {
  readonly uri: string;
  readonly range: PreprocessorRange;
}

function pointRange(position: PreprocessorPosition): PreprocessorRange {
  return { start: position, end: position };
}

function locationCommandUri(location: PreprocessorSourceLocation): string {
  const args = encodeURIComponent(JSON.stringify([location]));
  return `command:${openPreprocessorLocationCommand}?${args}`;
}

// Renders `label` as a plain escaped string, or -- only when `location` is
// provided -- as a link that invokes `openPreprocessorLocationCommand`
// through a `command:` URI. Callers must only pass a `location` sourced
// directly from the report (a file's own uri, an include's own
// line/character, a macro's own definition site, or a skipped region's own
// range): never a value derived by matching text.
function locationLink(
  label: string,
  location: PreprocessorSourceLocation | undefined,
): string {
  const text = escapeHtml(label);
  if (location === undefined) {
    return text;
  }
  return `<a href="${escapeHtml(locationCommandUri(location))}" title="Go to location">${text}</a>`;
}

const statusLabels: Record<PreprocessorIncludeStatus, string> = {
  resolved: "Resolved",
  missing: "Missing",
  cyclic: "Cyclic",
  dynamic: "Dynamic",
};

const kindLabels: Record<PreprocessorIncludeKind, string> = {
  quoted: "quoted",
  angled: "angled",
  macro: "macro-expanded",
};

function includeDirectiveLabel(include: PreprocessorInclude): string {
  switch (include.kind) {
    case "quoted":
      return `"${include.path}"`;
    case "angled":
      return `<${include.path}>`;
    case "macro":
      return include.path;
  }
}

function includeRow(fileUri: string, include: PreprocessorInclude): string {
  const directiveLocation: PreprocessorSourceLocation = {
    uri: fileUri,
    range: pointRange({ line: include.line, character: include.character }),
  };
  const directiveLink = locationLink(
    includeDirectiveLabel(include),
    directiveLocation,
  );
  const target =
    include.resolvedUri !== undefined
      ? locationLink(include.logicalPath ?? include.resolvedUri, {
          uri: include.resolvedUri,
          range: pointRange({ line: 0, character: 0 }),
        })
      : "-";
  const mapping =
    include.mapping !== undefined
      ? ` <span class="muted">(via ${escapeHtml(include.mapping)} mapping)</span>`
      : "";
  const expansion =
    include.expandedPath !== undefined
      ? ` <span class="muted">(expands to ${escapeHtml(include.expandedPath)})</span>`
      : "";
  const configurationOrigin =
    include.configurationOrigin === undefined
      ? ""
      : include.configurationOriginUri === undefined
        ? ` <span class="muted">(configured by ${escapeHtml(include.configurationOrigin)})</span>`
        : ` <span class="muted">(configured by ${locationLink(
            include.configurationOrigin,
            {
              uri: include.configurationOriginUri,
              range: pointRange({ line: 0, character: 0 }),
            },
          )})</span>`;
  return `<tr><td>${directiveLink}${expansion}</td><td>${escapeHtml(kindLabels[include.kind])}</td><td><span class="status ${include.status}">${escapeHtml(statusLabels[include.status])}</span></td><td>${target}${mapping}${configurationOrigin}</td></tr>`;
}

function fileSection(file: PreprocessorFile): string {
  const heading = locationLink(file.logicalPath || file.physicalPath, {
    uri: file.uri,
    range: pointRange({ line: 0, character: 0 }),
  });
  const sourceBadge = `<span class="source ${file.source}">${file.source === "open" ? "open in editor" : "on disk"}</span>`;
  const rows =
    file.includes.length === 0
      ? `<tr><td colspan="4" class="muted">(no #include directives)</td></tr>`
      : file.includes.map((include) => includeRow(file.uri, include)).join("");
  return `<section class="file">
<h2>${heading} ${sourceBadge}</h2>
<p class="physical-path">${escapeHtml(file.physicalPath)}</p>
<table>
<thead><tr><th>Directive</th><th>Kind</th><th>Status</th><th>Target</th></tr></thead>
<tbody>${rows}</tbody>
</table>
</section>`;
}

function skippedRegionRow(region: PreprocessorSkippedRegion): string {
  const location: PreprocessorSourceLocation = {
    uri: region.uri,
    range: { start: region.start, end: region.end },
  };
  const label =
    `${String(region.start.line + 1)}:${String(region.start.character + 1)}` +
    ` \u2013 ${String(region.end.line + 1)}:${String(region.end.character + 1)}`;
  return `<tr><td>${locationLink(region.uri, { uri: region.uri, range: pointRange(region.start) })}</td><td>${locationLink(label, location)}</td></tr>`;
}

function macroRow(macro: PreprocessorMacro): string {
  const label =
    macro.uri !== undefined &&
    macro.line !== undefined &&
    macro.character !== undefined
      ? locationLink(macro.name, {
          uri: macro.uri,
          range: pointRange({ line: macro.line, character: macro.character }),
        })
      : escapeHtml(macro.name);
  const sourceLabel =
    macro.source === "compiler" ? "Compiler" : "Configuration";
  const origin =
    macro.originUri === undefined
      ? escapeHtml(macro.origin ?? "")
      : locationLink(macro.origin ?? macro.originUri, {
          uri: macro.originUri,
          range: pointRange({ line: 0, character: 0 }),
        });
  return `<tr><td>${label}</td><td><code>${escapeHtml(macro.value)}</code></td><td>${escapeHtml(sourceLabel)}</td><td>${origin}</td></tr>`;
}

function settingValueText(setting: PreprocessorSetting): string {
  const value = setting.value;
  if (Array.isArray(value)) {
    return value.length === 0 ? "(none)" : value.join(", ");
  }
  if (typeof value === "object") {
    const entries = Object.entries(value as Record<string, string>);
    return entries.length === 0
      ? "(none)"
      : entries.map(([key, entry]) => `${key} \u2192 ${entry}`).join(", ");
  }
  const text = String(value);
  return text.length === 0 &&
    (setting.name === "entryPoint" || setting.name === "targetProfile")
    ? "Not configured"
    : text.length === 0
      ? "(none)"
      : text;
}

function settingRow(setting: PreprocessorSetting): string {
  const origin =
    setting.originUri === undefined
      ? escapeHtml(setting.origin)
      : locationLink(setting.origin, {
          uri: setting.originUri,
          range: pointRange({ line: 0, character: 0 }),
        });
  return `<tr><td>${escapeHtml(setting.name)}</td><td>${escapeHtml(settingValueText(setting))}</td><td>${origin}</td></tr>`;
}

export function preprocessorExplorerHtml(
  report: PreprocessorExplorerReport,
): string {
  const diagnostics =
    report.diagnostics.length === 0
      ? ""
      : `<section class="diagnostics">${report.diagnostics.map((message) => `<p>${escapeHtml(message)}</p>`).join("")}</section>`;

  const files =
    report.files.length === 0
      ? `<p class="muted">No files were analyzed.</p>`
      : report.files.map((file) => fileSection(file)).join("");

  const skippedRegionsCapability = report.compilerAnalysis?.skippedRegions;
  const skippedRegions =
    skippedRegionsCapability?.available === false
      ? `<p class="unavailable">Unavailable: ${escapeHtml(skippedRegionsCapability.reason ?? "compiler analysis is not supported for this source snapshot.")}</p>`
      : report.skippedRegions.length === 0
        ? `<p class="muted">No preprocessor-skipped regions were reported.</p>`
        : `<table>
<thead><tr><th>File</th><th>Range</th></tr></thead>
<tbody>${report.skippedRegions.map((region) => skippedRegionRow(region)).join("")}</tbody>
</table>`;

  const compilerMacrosCapability = report.compilerAnalysis?.compilerMacros;
  const macros =
    compilerMacrosCapability?.available === false
      ? `<p class="unavailable">Unavailable: ${escapeHtml(compilerMacrosCapability.reason ?? "compiler macro analysis is not supported for this source snapshot.")}</p>`
      : report.macros.length === 0
        ? `<p class="muted">No macros were reported.</p>`
        : `<table>
<thead><tr><th>Name</th><th>Value</th><th>Source</th><th>Origin</th></tr></thead>
<tbody>${report.macros.map((macro) => macroRow(macro)).join("")}</tbody>
</table>`;

  const settings =
    report.settings.length === 0
      ? `<p class="muted">No settings were reported.</p>`
      : `<table>
<thead><tr><th>Setting</th><th>Value</th><th>Origin</th></tr></thead>
<tbody>${report.settings.map((setting) => settingRow(setting)).join("")}</tbody>
</table>`;

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  h1 { font-size: 1.35rem; margin: 0 0 .25rem; }
  h2 { font-size: 1.05rem; margin: 1.5rem 0 .35rem; }
  a { color: var(--vscode-textLink-foreground); }
  .summary { color: var(--vscode-descriptionForeground); margin-bottom: 1.25rem; word-break: break-all; }
  .muted { color: var(--vscode-descriptionForeground); }
  .unavailable { color: var(--vscode-editorWarning-foreground); }
  .physical-path { color: var(--vscode-descriptionForeground); margin: 0 0 .5rem; word-break: break-all; }
  table { border-collapse: collapse; width: 100%; max-width: 70rem; margin-bottom: .5rem; }
  th, td { border-bottom: 1px solid var(--vscode-panel-border); padding: .4rem .5rem; text-align: left; vertical-align: top; }
  th { color: var(--vscode-descriptionForeground); }
  code { font-family: var(--vscode-editor-font-family); }
  .diagnostics { border-left: 3px solid var(--vscode-editorWarning-foreground); padding-left: .75rem; margin: 1rem 0; }
  .status { padding: .05rem .4rem; border-radius: .2rem; border: 1px solid transparent; }
  .status.resolved { color: var(--vscode-terminal-ansiGreen); border-color: var(--vscode-terminal-ansiGreen); }
  .status.missing { color: var(--vscode-editorError-foreground); border-color: var(--vscode-editorError-foreground); }
  .status.cyclic { color: var(--vscode-editorWarning-foreground); border-color: var(--vscode-editorWarning-foreground); }
  .status.dynamic { color: var(--vscode-descriptionForeground); border-color: var(--vscode-descriptionForeground); }
  .source { color: var(--vscode-descriptionForeground); font-size: .85em; margin-left: .35rem; }
</style>
</head>
<body>
<h1>Preprocessor Explorer</h1>
${effectiveContextHeaderHtml(report.context)}
${report.context === undefined ? `<div class="summary">${escapeHtml(report.rootUri)}</div>` : ""}
${diagnostics}
${files}
<h2>Preprocessor-skipped regions</h2>
${skippedRegions}
<h2>Macros</h2>
${macros}
<h2>Effective settings</h2>
${settings}
</body>
</html>`;
}

export function preprocessorExplorerErrorHtml(message: string): string {
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
<h1>Preprocessor Explorer</h1>
<p class="unavailable">${escapeHtml(message)}</p>
</body>
</html>`;
}

export interface PreprocessorExplorerRefreshOutcome {
  // undefined means "leave the webview's currently displayed HTML alone",
  // used to keep the last successful content on screen through a failed or
  // cancelled refresh instead of regressing to a placeholder or an
  // out-of-date loading message.
  readonly html: string | undefined;
  readonly hasContent: boolean;
  readonly title: string | undefined;
}

// Pure decision logic for how a Preprocessor Explorer panel should react to
// one hlsl/preprocessorExplorer attempt, mirroring
// resolveResourceBindingsRefresh/resolveCompilationInfoRefresh so all three
// panels share the same never-regress-on-failure behavior while remaining
// independently tracked panels/state.
export function resolvePreprocessorExplorerRefresh(
  hasContent: boolean,
  report: PreprocessorExplorerReport | null | undefined,
  failureMessage: string | undefined,
): PreprocessorExplorerRefreshOutcome {
  if (report === null || report === undefined) {
    if (hasContent) {
      return { html: undefined, hasContent: true, title: undefined };
    }
    return {
      html: preprocessorExplorerErrorHtml(
        failureMessage ??
          "The HLSL language server is not currently available.",
      ),
      hasContent: false,
      title: undefined,
    };
  }
  return {
    html: preprocessorExplorerHtml(report),
    hasContent: true,
    title: `Preprocessor Explorer: ${rootUriLabel(report.rootUri)}`,
  };
}

function rootUriLabel(rootUri: string): string {
  const withoutQuery = rootUri.split(/[?#]/)[0] ?? rootUri;
  const segments = withoutQuery
    .split("/")
    .filter((segment) => segment.length > 0);
  const last = segments.at(-1);
  return last === undefined ? rootUri : decodeURIComponent(last);
}

// The validated shape an `openPreprocessorLocationCommand` invocation must
// have before any `vscode.Uri`/document API touches it.
export interface ValidatedPreprocessorLocation {
  readonly uri: string;
  readonly range: PreprocessorRange;
}

function isNonNegativeInteger(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

function parsePosition(value: unknown): PreprocessorPosition | undefined {
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

function parseRange(value: unknown): PreprocessorRange | undefined {
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
// `openPreprocessorLocationCommand` command-message boundary before any
// `vscode.Uri.parse`/`openTextDocument`/`showTextDocument` call touches it.
// Returns `undefined` for anything that does not exactly match the
// expected `{uri, range}` shape (including a missing/empty uri, a
// malformed range, or an inverted range) -- the command handler must treat
// that as a validation failure rather than falling back to a guess.
export function parsePreprocessorLocationCommandArg(
  value: unknown,
): ValidatedPreprocessorLocation | undefined {
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
import {
  EffectiveShaderContext,
  effectiveContextHeaderHtml,
} from "./effectiveContext";
