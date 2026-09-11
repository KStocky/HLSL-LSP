import assert from "node:assert/strict";
import test from "node:test";

import {
  macroExpansionHtml,
  macroExpansionMessageHtml,
  macroExpansionTarget,
} from "../../src/macroExpansion";

void test("macro expansion target accepts only a complete document position", () => {
  assert.deepEqual(
    macroExpansionTarget({
      textDocument: { uri: "file:///shader.hlsl" },
      position: { line: 3, character: 7 },
    }),
    {
      textDocument: { uri: "file:///shader.hlsl" },
      position: { line: 3, character: 7 },
    },
  );
  assert.equal(macroExpansionTarget(undefined), undefined);
  assert.equal(
    macroExpansionTarget({
      textDocument: { uri: "file:///shader.hlsl" },
      position: { line: -1, character: 0 },
    }),
    undefined,
  );
});

void test("macro expansion HTML is theme-aware and escapes compiler text", () => {
  const html = macroExpansionHtml({
    name: "WRAP",
    invocation: "WRAP(<value>)",
    expansion: 'float4("<&>")',
  });

  assert.match(html, /var\(--vscode-editor-background\)/);
  assert.match(html, /WRAP\(&lt;value&gt;\)/);
  assert.match(html, /float4\(&quot;&lt;&amp;&gt;&quot;\)/);
  assert.doesNotMatch(html, /WRAP\(<value>\)/);
});

void test("macro expansion status messages escape failure text", () => {
  const html = macroExpansionMessageHtml("<compiler failure>");

  assert.match(html, /&lt;compiler failure&gt;/);
  assert.doesNotMatch(html, /<compiler failure>/);
});
