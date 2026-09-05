export interface CompilationDiagnostic {
  readonly severity: string;
  readonly message: string;
  readonly path: string;
  readonly line: number;
  readonly column: number;
}

export interface CompilationOutput {
  readonly type: string;
  readonly size: number;
}

export interface CompilationSignatureParameter {
  readonly semanticName: string;
  readonly semanticIndex: number;
  readonly register: number;
  readonly systemValue: string;
  readonly componentType: string;
  readonly mask: number;
  readonly readWriteMask: number;
  readonly stream: number;
}

export interface CompilationResourceBinding {
  readonly name: string;
  readonly type: string;
  readonly bindPoint: number;
  readonly bindCount: number;
  readonly space: number;
  readonly dimension: string;
  readonly returnType: string;
}

export interface CompilationThreadGroupSize {
  readonly x: number;
  readonly y: number;
  readonly z: number;
}

export interface CompilationReflection {
  readonly available: boolean;
  readonly unavailableReason: string;
  readonly inputSignature: readonly CompilationSignatureParameter[];
  readonly outputSignature: readonly CompilationSignatureParameter[];
  readonly resources: readonly CompilationResourceBinding[];
  readonly threadGroupSize: CompilationThreadGroupSize | null;
}

export interface CompilationInfo {
  readonly entryPoint: string;
  readonly stage: string;
  readonly targetProfile: string;
  readonly languageVersion: string;
  readonly defines: readonly string[];
  readonly compilerArguments: readonly string[];
  readonly includeDirectories: readonly string[];
  readonly resolvedIncludePaths: readonly string[];
  readonly activeVariant: string | null;
  readonly success: boolean;
  readonly diagnostics: readonly CompilationDiagnostic[];
  readonly output: CompilationOutput | null;
  readonly reflection: CompilationReflection | null;
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

const maskLetters = ["x", "y", "z", "w"];

function componentMask(mask: number): string {
  let result = "";
  for (let bit = 0; bit < maskLetters.length; ++bit) {
    if ((mask & (1 << bit)) !== 0) {
      result += maskLetters[bit] ?? "";
    }
  }
  return result === "" ? "-" : result;
}

function listOrNone(values: readonly string[]): string {
  if (values.length === 0) {
    return `<p class="muted">(none)</p>`;
  }
  return `<ul>${values.map((value) => `<li><code>${escapeHtml(value)}</code></li>`).join("")}</ul>`;
}

function configurationSection(info: CompilationInfo): string {
  const rows: [string, string][] = [
    ["Entry point", info.entryPoint || "(none)"],
    ["Stage", info.stage || "(unknown)"],
    ["Target profile", info.targetProfile || "(none)"],
    ["Language version", info.languageVersion || "(default)"],
    ["Active variant", info.activeVariant ?? "(none)"],
  ];
  const table = `<table>${rows
    .map(
      ([label, value]) =>
        `<tr><th>${escapeHtml(label)}</th><td>${escapeHtml(value)}</td></tr>`,
    )
    .join("")}</table>`;
  return `<section>
<h2>Effective configuration</h2>
${table}
<h3>Preprocessor defines</h3>
${listOrNone(info.defines)}
<h3>Compiler arguments</h3>
${listOrNone(info.compilerArguments)}
<h3>Include directories</h3>
${listOrNone(info.includeDirectories)}
<h3>Resolved include paths</h3>
${listOrNone(info.resolvedIncludePaths)}
</section>`;
}

function diagnosticsSection(info: CompilationInfo): string {
  const statusClass = info.success ? "status-success" : "status-failure";
  const statusText = info.success
    ? "Compilation succeeded"
    : "Compilation failed";
  const rows =
    info.diagnostics.length === 0
      ? ""
      : `<table>
<thead><tr><th>Severity</th><th>Message</th><th>Path</th><th>Line</th><th>Column</th></tr></thead>
<tbody>${info.diagnostics
          .map(
            (diagnostic) =>
              `<tr><td>${escapeHtml(diagnostic.severity)}</td><td>${escapeHtml(diagnostic.message)}</td><td>${escapeHtml(diagnostic.path)}</td><td>${String(diagnostic.line)}</td><td>${String(diagnostic.column)}</td></tr>`,
          )
          .join("")}</tbody>
</table>`;
  return `<section>
<h2 class="${statusClass}">${statusText}</h2>
${rows}
</section>`;
}

function outputSection(info: CompilationInfo): string {
  const body =
    info.output === null
      ? `<p class="muted">No compiled output was produced.</p>`
      : `<table><tr><th>Type</th><td>${escapeHtml(info.output.type)}</td></tr><tr><th>Size</th><td>${String(info.output.size)} bytes</td></tr></table>`;
  return `<section>
<h2>Output</h2>
${body}
</section>`;
}

function signatureTable(
  title: string,
  parameters: readonly CompilationSignatureParameter[],
): string {
  if (parameters.length === 0) {
    return `<h3>${escapeHtml(title)}</h3><p class="muted">(none)</p>`;
  }
  const rows = parameters
    .map(
      (parameter) =>
        `<tr><td>${escapeHtml(parameter.semanticName)}</td><td>${String(parameter.semanticIndex)}</td><td>${String(parameter.register)}</td><td>${escapeHtml(parameter.systemValue)}</td><td>${escapeHtml(parameter.componentType)}</td><td>${componentMask(parameter.mask)}</td><td>${componentMask(parameter.readWriteMask)}</td><td>${String(parameter.stream)}</td></tr>`,
    )
    .join("");
  return `<h3>${escapeHtml(title)}</h3>
<table>
<thead><tr><th>Semantic</th><th>Index</th><th>Register</th><th>System value</th><th>Component type</th><th>Mask</th><th>Read/write mask</th><th>Stream</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

function resourcesTable(
  resources: readonly CompilationResourceBinding[],
): string {
  if (resources.length === 0) {
    return `<h3>Resources</h3><p class="muted">(none)</p>`;
  }
  const rows = resources
    .map(
      (resource) =>
        `<tr><td>${escapeHtml(resource.name)}</td><td>${escapeHtml(resource.type)}</td><td>${String(resource.bindPoint)}</td><td>${String(resource.bindCount)}</td><td>${String(resource.space)}</td><td>${escapeHtml(resource.dimension)}</td><td>${escapeHtml(resource.returnType)}</td></tr>`,
    )
    .join("");
  return `<h3>Resources</h3>
<table>
<thead><tr><th>Name</th><th>Type</th><th>Bind point</th><th>Bind count</th><th>Space</th><th>Dimension</th><th>Return type</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

function reflectionSection(info: CompilationInfo): string {
  const reflection = info.reflection;
  if (reflection === null) {
    return `<section>
<h2>Reflection</h2>
<p class="muted">Reflection metadata is not available because no compiled output was produced.</p>
</section>`;
  }
  if (!reflection.available) {
    return `<section>
<h2>Reflection</h2>
<p class="unavailable">Reflection is unavailable: ${escapeHtml(reflection.unavailableReason || "unknown reason")}</p>
</section>`;
  }
  const threadGroupSize =
    reflection.threadGroupSize === null
      ? ""
      : `<h3>Thread-group size</h3><table><tr><th>X</th><th>Y</th><th>Z</th></tr><tr><td>${String(reflection.threadGroupSize.x)}</td><td>${String(reflection.threadGroupSize.y)}</td><td>${String(reflection.threadGroupSize.z)}</td></tr></table>`;
  return `<section>
<h2>Reflection</h2>
${signatureTable("Input signature", reflection.inputSignature)}
${signatureTable("Output signature", reflection.outputSignature)}
${resourcesTable(reflection.resources)}
${threadGroupSize}
</section>`;
}

export function compilationInfoHtml(info: CompilationInfo): string {
  const title = info.entryPoint || "(default entry point)";
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  h1 { font-size: 1.35rem; margin: 0 0 .25rem; }
  h2 { font-size: 1.1rem; margin: 1.5rem 0 .5rem; }
  h3 { font-size: .95rem; margin: 1rem 0 .35rem; color: var(--vscode-descriptionForeground); }
  section { margin-bottom: 1rem; }
  table { border-collapse: collapse; width: 100%; max-width: 70rem; margin-bottom: .5rem; }
  th, td { border-bottom: 1px solid var(--vscode-panel-border); padding: .35rem .5rem; text-align: left; vertical-align: top; }
  th { color: var(--vscode-descriptionForeground); }
  .muted { color: var(--vscode-descriptionForeground); }
  .status-success { color: var(--vscode-testing-iconPassed, #73c991); }
  .status-failure { color: var(--vscode-testing-iconFailed, #f14c4c); }
  .unavailable { border-left: 3px solid var(--vscode-editorWarning-foreground); padding-left: .75rem; }
  code { font-family: var(--vscode-editor-font-family); }
  ul { margin: 0; padding-left: 1.25rem; }
</style>
</head>
<body>
<h1>${escapeHtml(title)}</h1>
${configurationSection(info)}
${diagnosticsSection(info)}
${outputSection(info)}
${reflectionSection(info)}
</body>
</html>`;
}

// Rendered only when a request fails or is cancelled and no prior successful
// result exists to keep showing instead. Never used to replace already
// displayed content, and never a perpetual "loading" placeholder.
export function compilationInfoErrorHtml(message: string): string {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  h1 { font-size: 1.35rem; margin: 0 0 .5rem; }
  p.unavailable { border-left: 3px solid var(--vscode-editorError-foreground, var(--vscode-editorWarning-foreground)); padding-left: .75rem; }
</style>
</head>
<body>
<h1>Shader compilation</h1>
<p class="unavailable">${escapeHtml(message)}</p>
</body>
</html>`;
}

export interface CompilationInfoRefreshOutcome {
  // undefined means "leave the webview's currently displayed HTML alone",
  // used to keep the last successful content on screen through a failed or
  // cancelled refresh instead of regressing to a placeholder or an
  // out-of-date loading message.
  readonly html: string | undefined;
  readonly hasContent: boolean;
  readonly title: string | undefined;
}

// Pure decision logic for how a Shader Compilation panel should react to one
// hlsl/compilationInfo attempt, kept separate from the VS Code webview calls
// in extension.ts so it can be unit tested directly. A failed or cancelled
// attempt (info is null/undefined) never regresses the panel: it keeps
// whatever is already on screen when hasContent is true, and otherwise shows
// an explicit error instead of a perpetual loading placeholder.
export function resolveCompilationInfoRefresh(
  hasContent: boolean,
  info: CompilationInfo | null | undefined,
  failureMessage: string | undefined,
): CompilationInfoRefreshOutcome {
  if (info === null || info === undefined) {
    if (hasContent) {
      return { html: undefined, hasContent: true, title: undefined };
    }
    return {
      html: compilationInfoErrorHtml(
        failureMessage ??
          "The HLSL language server is not currently available.",
      ),
      hasContent: false,
      title: undefined,
    };
  }
  return {
    html: compilationInfoHtml(info),
    hasContent: true,
    title: `Shader Compilation: ${info.entryPoint || "(default entry point)"}`,
  };
}
