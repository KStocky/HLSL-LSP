import assert from "node:assert/strict";
import test from "node:test";

import {
  healthDiagnosticsText,
  healthHtml,
  redactAbsolutePaths,
  redactDiagnosticMessage,
  summarizeHealth,
} from "../../src/health";

const healthy = {
  lifecycle: "running" as const,
  serverSource: "bundled",
  runtime: {
    source: "bundled",
    version: "1.10.0.1",
    requiresRestart: false,
  },
  context: {
    documentUri: "file:///C:/work/shader.hlsl",
    file: "C:\\work\\shader.hlsl",
    configurationUri: "file:///C:/work/shadertoolsconfig.json",
    activeVariant: "Debug",
    entryPoint: "main",
    targetProfile: "ps_6_6",
    origins: {},
  },
};

void test("health summary keeps healthy state compact", () => {
  assert.deepEqual(summarizeHealth(healthy), {
    level: "healthy",
    label: "HLSL ready",
    detail: "The language server and DXC runtime are available.",
  });
});

void test("health summary prioritizes actionable failures", () => {
  const summary = summarizeHealth({
    ...healthy,
    lastFailure: "DXC could not be loaded.",
  });
  assert.equal(summary.level, "warning");
  assert.equal(summary.detail, "DXC could not be loaded.");
});

void test("copied diagnostics omit source and absolute shader paths", () => {
  const diagnostics = healthDiagnosticsText(healthy);
  assert.match(diagnostics, /File: shader\.hlsl/);
  assert.match(diagnostics, /Configuration: shadertoolsconfig\.json/);
  assert.doesNotMatch(diagnostics, /C:\\work/);
  assert.doesNotMatch(diagnostics, /file:\/\/\//);
});

void test("health HTML escapes compiler and failure text", () => {
  const html = healthHtml({
    ...healthy,
    runtime: {
      source: "<custom>",
      version: "1&2",
      requiresRestart: true,
    },
    lastFailure: "<script>alert('x')</script>",
  });
  assert.match(html, /&lt;custom&gt;/);
  assert.match(html, /1&amp;2/);
  assert.match(html, /&lt;script&gt;alert\(&#39;x&#39;\)&lt;\/script&gt;/);
  assert.doesNotMatch(html, /<script>alert/);
  assert.match(html, /data-command="restart"/);
  assert.match(html, /data-command="copy"/);
});

void test("diagnostic redaction handles Windows and URI-style paths", () => {
  assert.equal(
    redactDiagnosticMessage(
      "Failed C:\\Users\\name\\project and C:/Users/name/project/file.hlsl",
      ["C:\\Users\\name\\project"],
    ),
    "Failed <redacted-path> and <redacted-path>/file.hlsl",
  );
});

void test("absolute paths outside known workspaces are redacted", () => {
  const message =
    "Failed to load D:\\Third Party SDK\\dxcompiler.dll\n" +
    "Configured by file:///C:/Users/name/project/shadertoolsconfig.json";
  const redacted = redactAbsolutePaths(message);
  assert.doesNotMatch(redacted, /Third Party SDK/);
  assert.doesNotMatch(redacted, /Users\/name/);
  assert.match(redacted, /<redacted-path>/);

  const diagnostics = healthDiagnosticsText({
    ...healthy,
    runtime: {
      ...healthy.runtime,
      error: message,
    },
  });
  assert.doesNotMatch(diagnostics, /Third Party SDK|Users\/name/);
});
