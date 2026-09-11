import { escapeHtml } from "./compilationInfo";
import {
  EffectiveShaderContext,
  effectiveContextHeaderHtml,
} from "./effectiveContext";

export interface MacroExpansion {
  readonly name: string;
  readonly invocation: string;
  readonly expansion: string;
  readonly range?: {
    readonly start: { readonly line: number; readonly character: number };
    readonly end: { readonly line: number; readonly character: number };
  };
  readonly definitionLocation?: {
    readonly uri: string;
    readonly range: {
      readonly start: { readonly line: number; readonly character: number };
      readonly end: { readonly line: number; readonly character: number };
    };
  } | null;
  readonly context?: EffectiveShaderContext;
}

export interface MacroExpansionTarget {
  readonly textDocument: { readonly uri: string };
  readonly position: { readonly line: number; readonly character: number };
}

export function macroExpansionTarget(
  value: unknown,
): MacroExpansionTarget | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as {
    readonly textDocument?: { readonly uri?: unknown };
    readonly position?: {
      readonly line?: unknown;
      readonly character?: unknown;
    };
  };
  const uri = candidate.textDocument?.uri;
  const line = candidate.position?.line;
  const character = candidate.position?.character;
  if (
    typeof uri !== "string" ||
    uri === "" ||
    typeof line !== "number" ||
    !Number.isInteger(line) ||
    line < 0 ||
    typeof character !== "number" ||
    !Number.isInteger(character) ||
    character < 0
  ) {
    return undefined;
  }
  return {
    textDocument: { uri },
    position: { line, character },
  };
}

export function macroExpansionHtml(result: MacroExpansion): string {
  const context =
    result.context === undefined
      ? ""
      : effectiveContextHeaderHtml(result.context);
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Macro Expansion</title>
<style>
body { color:var(--vscode-foreground); background:var(--vscode-editor-background); font-family:var(--vscode-font-family); padding:1rem 1.5rem; }
h1 { font-size:1.35rem; margin:0 0 1rem; }
h2 { font-size:1rem; margin:1.1rem 0 .4rem; }
pre { margin:0; padding:.75rem; overflow:auto; border:1px solid var(--vscode-panel-border); border-radius:4px; background:var(--vscode-textCodeBlock-background); }
code { font-family:var(--vscode-editor-font-family); font-size:var(--vscode-editor-font-size); }
</style>
</head>
<body>
<h1>Macro Expansion: ${escapeHtml(result.name)}</h1>
${context}
<h2>Invocation</h2>
<pre><code>${escapeHtml(result.invocation)}</code></pre>
<h2>Expanded result</h2>
<pre><code>${escapeHtml(result.expansion)}</code></pre>
</body>
</html>`;
}

export function macroExpansionMessageHtml(message: string): string {
  return `<!doctype html><html lang="en"><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><h1>Macro Expansion</h1><p>${escapeHtml(message)}</p></body></html>`;
}
