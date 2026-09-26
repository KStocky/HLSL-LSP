import { EffectiveShaderContext } from "./effectiveContext";
import { LifecycleState } from "./lifecycle";

export interface HealthRuntime {
  readonly source: string;
  readonly version: string;
  readonly requiresRestart: boolean;
  readonly error?: string;
}

export interface HlslHealthSnapshot {
  readonly lifecycle: LifecycleState;
  readonly serverSource?: string;
  readonly runtime?: HealthRuntime;
  readonly context?: EffectiveShaderContext;
  readonly lastFailure?: string;
}

export type HlslHealthLevel = "healthy" | "busy" | "unavailable" | "warning";

export interface HlslHealthSummary {
  readonly level: HlslHealthLevel;
  readonly label: string;
  readonly detail: string;
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function valueOrDefault(
  value: string | null | undefined,
  fallback: string,
): string {
  return value === null || value === undefined || value === ""
    ? fallback
    : value;
}

function safeFileName(value: string): string {
  const normalized = value.replaceAll("\\", "/");
  const separator = normalized.lastIndexOf("/");
  return separator < 0 ? normalized : normalized.slice(separator + 1);
}

function configurationSummary(
  context: EffectiveShaderContext | undefined,
): string {
  if (context === undefined) {
    return "Unavailable";
  }
  const origins: string[] = [];
  const addOrigin = (
    label: string,
    origin: { readonly label: string } | undefined,
  ): void => {
    if (origin !== undefined) {
      origins.push(`${label}: ${safeFileName(origin.label)}`);
    }
  };
  addOrigin("Variant", context.origins.variant);
  addOrigin("Entry point", context.origins.entryPoint);
  addOrigin("Target profile", context.origins.targetProfile);
  if (origins.length > 0) {
    return origins.join("; ");
  }
  return context.configurationUri
    ? safeFileName(context.configurationUri)
    : "No file-backed overrides";
}

export function summarizeHealth(
  snapshot: HlslHealthSnapshot,
): HlslHealthSummary {
  if (snapshot.lifecycle === "starting" || snapshot.lifecycle === "stopping") {
    return {
      level: "busy",
      label: "HLSL starting",
      detail: `Language server is ${snapshot.lifecycle}.`,
    };
  }
  if (snapshot.lifecycle !== "running") {
    return {
      level: "unavailable",
      label: "HLSL stopped",
      detail: snapshot.lastFailure ?? "The language server is not running.",
    };
  }
  if (
    snapshot.lastFailure !== undefined ||
    snapshot.runtime?.error !== undefined ||
    snapshot.runtime?.requiresRestart === true
  ) {
    return {
      level: "warning",
      label: "HLSL attention",
      detail:
        snapshot.lastFailure ??
        snapshot.runtime?.error ??
        "A DXC runtime change is waiting for a language server restart.",
    };
  }
  return {
    level: "healthy",
    label: "HLSL ready",
    detail: "The language server and DXC runtime are available.",
  };
}

export function redactDiagnosticMessage(
  message: string,
  sensitivePaths: readonly string[],
): string {
  let result = message;
  for (const sensitivePath of sensitivePaths) {
    const trimmed = sensitivePath.trim();
    if (trimmed === "") {
      continue;
    }
    result = result.replaceAll(trimmed, "<redacted-path>");
    result = result.replaceAll(
      trimmed.replaceAll("\\", "/"),
      "<redacted-path>",
    );
  }
  return result;
}

export function redactAbsolutePaths(message: string): string {
  return message.replace(
    /(?:file:\/\/\/|[A-Za-z]:[\\/]|\\\\)[^"'<>|\r\n]*/g,
    "<redacted-path>",
  );
}

export function healthDiagnosticsText(snapshot: HlslHealthSnapshot): string {
  const summary = summarizeHealth(snapshot);
  const lines = [
    "HLSL-LSP diagnostics",
    `Status: ${summary.label}`,
    `Lifecycle: ${snapshot.lifecycle}`,
    `Server: ${snapshot.serverSource ?? "unavailable"}`,
    `DXC source: ${snapshot.runtime?.source ?? "unavailable"}`,
    `DXC version: ${snapshot.runtime?.version ?? "unavailable"}`,
  ];
  if (snapshot.runtime?.requiresRestart === true) {
    lines.push("DXC restart pending: yes");
  }
  if (snapshot.context !== undefined) {
    lines.push(
      `File: ${safeFileName(snapshot.context.file)}`,
      `Variant: ${valueOrDefault(snapshot.context.activeVariant, "Default")}`,
      `Entry point: ${valueOrDefault(snapshot.context.entryPoint, "Not configured")}`,
      `Target profile: ${valueOrDefault(snapshot.context.targetProfile, "Not configured")}`,
    );
    lines.push(`Configuration: ${configurationSummary(snapshot.context)}`);
  } else {
    lines.push("Active shader: unavailable");
  }
  const failure = snapshot.lastFailure ?? snapshot.runtime?.error;
  if (failure !== undefined) {
    lines.push(`Last failure: ${redactAbsolutePaths(failure)}`);
  }
  return lines.join("\n");
}

export function healthHtml(snapshot: HlslHealthSnapshot): string {
  const summary = summarizeHealth(snapshot);
  const context = snapshot.context;
  const failure = snapshot.lastFailure ?? snapshot.runtime?.error;
  const statusClass =
    summary.level === "healthy"
      ? "healthy"
      : summary.level === "unavailable"
        ? "error"
        : "warning";
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:var(--vscode-font-family);color:var(--vscode-foreground);padding:1rem 1.25rem;line-height:1.45}
.summary{border-left:4px solid var(--vscode-panel-border);padding:.6rem .8rem;margin-bottom:1rem;background:var(--vscode-editor-inactiveSelectionBackground)}
.summary.healthy{border-color:var(--vscode-testing-iconPassed)}
.summary.warning{border-color:var(--vscode-editorWarning-foreground)}
.summary.error{border-color:var(--vscode-editorError-foreground)}
.grid{display:grid;grid-template-columns:max-content minmax(0,1fr);gap:.35rem .9rem;margin:0 0 1rem}
dt{font-weight:600}dd{margin:0;overflow-wrap:anywhere}
.actions{display:flex;flex-wrap:wrap;gap:.5rem;margin-top:1rem}
button{color:var(--vscode-button-foreground);background:var(--vscode-button-background);border:0;padding:.4rem .8rem;cursor:pointer}
button:hover{background:var(--vscode-button-hoverBackground)}
.failure{border:1px solid var(--vscode-editorError-foreground);padding:.6rem .8rem;margin-top:1rem}
</style>
</head>
<body>
<h1>HLSL-LSP Status</h1>
<section class="summary ${statusClass}">
<strong>${escapeHtml(summary.label)}</strong><br>
${escapeHtml(summary.detail)}
</section>
<dl class="grid">
<dt>Lifecycle</dt><dd>${escapeHtml(snapshot.lifecycle)}</dd>
<dt>Server</dt><dd>${escapeHtml(snapshot.serverSource ?? "Unavailable")}</dd>
<dt>DXC</dt><dd>${escapeHtml(snapshot.runtime?.source ?? "Unavailable")} · ${escapeHtml(snapshot.runtime?.version ?? "Unknown version")}${snapshot.runtime?.requiresRestart === true ? " · Restart pending" : ""}</dd>
<dt>File</dt><dd>${escapeHtml(context === undefined ? "No active HLSL document" : safeFileName(context.file))}</dd>
<dt>Variant</dt><dd>${escapeHtml(valueOrDefault(context?.activeVariant, "Default"))}</dd>
<dt>Entry point</dt><dd>${escapeHtml(valueOrDefault(context?.entryPoint, "Not configured"))}</dd>
<dt>Target profile</dt><dd>${escapeHtml(valueOrDefault(context?.targetProfile, "Not configured"))}</dd>
<dt>Configuration</dt><dd>${escapeHtml(configurationSummary(context))}</dd>
</dl>
${failure === undefined ? "" : `<section class="failure"><strong>Last actionable failure</strong><br>${escapeHtml(redactAbsolutePaths(failure))}</section>`}
<div class="actions">
<button data-command="restart">Restart</button>
<button data-command="output">Open Output</button>
<button data-command="configuration">Open Configuration</button>
<button data-command="copy">Copy Diagnostics</button>
<button data-command="refresh">Refresh</button>
</div>
<script>
const vscode = acquireVsCodeApi();
for (const button of document.querySelectorAll("button[data-command]")) {
  button.addEventListener("click", () => vscode.postMessage({ command: button.dataset.command }));
}
</script>
</body>
</html>`;
}
