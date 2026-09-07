import assert from "node:assert/strict";
import test from "node:test";

import {
  parsePreprocessorLocationCommandArg,
  PreprocessorExplorerReport,
  preprocessorExplorerErrorHtml,
  preprocessorExplorerHtml,
  resolvePreprocessorExplorerRefresh,
} from "../../src/preprocessorExplorer";

function baseReport(
  overrides: Partial<PreprocessorExplorerReport> = {},
): PreprocessorExplorerReport {
  return {
    rootUri: "file:///c:/scene/shader.hlsl",
    files: [],
    skippedRegions: [],
    macros: [],
    settings: [],
    diagnostics: [],
    ...overrides,
  };
}

void test("preprocessor explorer HTML renders each file's includes with directive, kind, and status", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      files: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          logicalPath: "shader.hlsl",
          physicalPath: "c:/scene/shader.hlsl",
          source: "open",
          includes: [
            {
              path: "common.hlsli",
              line: 2,
              character: 9,
              kind: "quoted",
              status: "resolved",
              resolvedUri: "file:///c:/scene/common.hlsli",
              logicalPath: "common.hlsli",
            },
            {
              path: "missing.hlsli",
              line: 5,
              character: 9,
              kind: "quoted",
              status: "missing",
            },
          ],
        },
      ],
    }),
  );
  assert.match(html, /&quot;common\.hlsli&quot;/);
  assert.match(html, /class="status resolved"/);
  assert.match(html, /class="status missing"/);
  assert.match(html, /&quot;missing\.hlsli&quot;/);
  assert.match(html, /<td>-<\/td>/);
});

void test("preprocessor explorer HTML links an include directive to its own reported position, never a guessed one", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      files: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          logicalPath: "shader.hlsl",
          physicalPath: "c:/scene/shader.hlsl",
          source: "open",
          includes: [
            {
              path: "common.hlsli",
              line: 2,
              character: 9,
              kind: "quoted",
              status: "resolved",
              resolvedUri: "file:///c:/scene/common.hlsli",
              logicalPath: "common.hlsli",
            },
          ],
        },
      ],
    }),
  );
  const encoded = encodeURIComponent(
    JSON.stringify([
      {
        uri: "file:///c:/scene/shader.hlsl",
        range: {
          start: { line: 2, character: 9 },
          end: { line: 2, character: 9 },
        },
      },
    ]),
  );
  assert.match(
    html,
    new RegExp(
      `command:hlsl\\.preprocessorExplorer\\.openLocation\\?${escapeRegExp(encoded)}`,
    ),
  );
});

void test("preprocessor explorer HTML shows a macro-expanded include without a resolved target as dynamic, non-clickable text", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      files: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          logicalPath: "shader.hlsl",
          physicalPath: "c:/scene/shader.hlsl",
          source: "open",
          includes: [
            {
              path: "PLATFORM_HEADER",
              line: 1,
              character: 9,
              kind: "macro",
              status: "dynamic",
            },
          ],
        },
      ],
    }),
  );
  assert.match(html, /class="status dynamic"/);
  assert.match(html, /macro-expanded/);
});

void test("preprocessor explorer HTML shows a resolved configured macro expansion and navigable origin", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      files: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          logicalPath: "shader.hlsl",
          physicalPath: "c:/scene/shader.hlsl",
          source: "open",
          includes: [
            {
              path: "PROJECT_HEADER",
              line: 1,
              character: 9,
              kind: "macro",
              status: "resolved",
              expandedPath: "/Project/common.hlsli",
              resolvedUri: "file:///c:/project/common.hlsli",
              logicalPath: "/Project/common.hlsli",
              configurationMacro: "PROJECT_HEADER",
              configurationOrigin: "c:/scene/shadertoolsconfig.json",
              configurationOriginUri: "file:///c:/scene/shadertoolsconfig.json",
            },
          ],
        },
      ],
    }),
  );
  assert.match(html, /expands to \/Project\/common\.hlsli/);
  assert.match(html, /configured by/);
  assert.match(html, /shadertoolsconfig\.json/);
  assert.match(html, /command:hlsl\.preprocessorExplorer\.openLocation\?/);
});

void test("preprocessor explorer HTML shows a virtual directory mapping used to resolve an include", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      files: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          logicalPath: "shader.hlsl",
          physicalPath: "c:/scene/shader.hlsl",
          source: "open",
          includes: [
            {
              path: "/Shaders/common.hlsli",
              line: 1,
              character: 9,
              kind: "angled",
              status: "resolved",
              resolvedUri: "file:///c:/engine/shaders/common.hlsli",
              logicalPath: "/Shaders/common.hlsli",
              mapping: "/Shaders",
            },
          ],
        },
      ],
    }),
  );
  assert.match(html, /via \/Shaders mapping/);
});

void test("preprocessor explorer HTML links a compiler macro to its reported definition site", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      macros: [
        {
          name: "MAX_LIGHTS",
          value: "4",
          source: "compiler",
          origin: "c:/scene/shader.hlsl",
          uri: "file:///c:/scene/shader.hlsl",
          line: 0,
          character: 8,
        },
        {
          name: "PLATFORM_WIN",
          value: "1",
          source: "configuration",
          origin: "shadertoolsconfig.json",
          originUri: "file:///c:/scene/shadertoolsconfig.json",
        },
      ],
    }),
  );
  assert.match(html, /<a[^>]*>MAX_LIGHTS<\/a>/);
  assert.doesNotMatch(html, /<a[^>]*>PLATFORM_WIN<\/a>/);
  assert.match(html, /<a[^>]*>shadertoolsconfig\.json<\/a>/);
  assert.match(html, /Compiler<\/td>/);
  assert.match(html, /Configuration<\/td>/);
});

void test("preprocessor explorer HTML renders array and object setting values legibly", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      settings: [
        {
          name: "includeDirectories",
          value: ["c:/engine/shaders", "c:/scene/shaders"],
          origin: "shadertoolsconfig.json",
        },
        {
          name: "virtualDirectoryMappings",
          value: { "/Shaders": "c:/engine/shaders" },
          origin: "shadertoolsconfig.json",
        },
        {
          name: "additionalArguments",
          value: [],
          origin: "built-in defaults",
        },
        {
          name: "languageVersion",
          value: "2021",
          origin: "client defaults",
        },
        {
          name: "targetProfile",
          value: "ps_6_7",
          origin: "c:/scene/shadertoolsconfig.json",
          originUri: "file:///c:/scene/shadertoolsconfig.json",
        },
      ],
    }),
  );
  assert.match(html, /c:\/engine\/shaders, c:\/scene\/shaders/);
  assert.match(html, /\/Shaders \u2192 c:\/engine\/shaders/);
  assert.match(html, /<td>\(none\)<\/td>/);
  assert.match(html, /<td>2021<\/td>/);
  assert.match(html, /<a[^>]*>c:\/scene\/shadertoolsconfig\.json<\/a>/);
});

void test("preprocessor explorer HTML links a skipped region to its own reported range", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      skippedRegions: [
        {
          uri: "file:///c:/scene/shader.hlsl",
          start: { line: 3, character: 0 },
          end: { line: 6, character: 1 },
        },
      ],
    }),
  );
  assert.match(html, /4:1/);
  assert.match(html, /7:2/);
  const encoded = encodeURIComponent(
    JSON.stringify([
      {
        uri: "file:///c:/scene/shader.hlsl",
        range: {
          start: { line: 3, character: 0 },
          end: { line: 6, character: 1 },
        },
      },
    ]),
  );
  assert.match(html, new RegExp(escapeRegExp(encoded)));
});

void test("preprocessor explorer HTML renders diagnostics when reported", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      diagnostics: [
        "Macro-based includes are compiler-owned; their expressions are shown without fabricating a resolved path.",
      ],
    }),
  );
  assert.match(html, /Macro-based includes are compiler-owned/);
});

void test("preprocessor explorer HTML escapes untrusted text", () => {
  const html = preprocessorExplorerHtml(
    baseReport({
      rootUri: "file:///c:/<script>alert(1)</script>.hlsl",
      diagnostics: ["<evil>"],
    }),
  );
  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.doesNotMatch(html, /<evil>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
  assert.match(html, /&lt;evil&gt;/);
});

void test("preprocessor explorer error HTML escapes the failure message", () => {
  const html = preprocessorExplorerErrorHtml(
    "<script>alert(1)</script> failed",
  );
  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt; failed/);
});

void test("a refresh with a successful result renders content and remembers it", () => {
  const report = baseReport();
  const outcome = resolvePreprocessorExplorerRefresh(false, report, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, "Preprocessor Explorer: shader.hlsl");
  assert.equal(outcome.html, preprocessorExplorerHtml(report));
});

void test("a failed refresh with no prior content shows an explicit error, not a placeholder", () => {
  const outcome = resolvePreprocessorExplorerRefresh(
    false,
    undefined,
    "The request timed out.",
  );
  assert.equal(outcome.hasContent, false);
  assert.equal(outcome.title, undefined);
  assert.equal(
    outcome.html,
    preprocessorExplorerErrorHtml("The request timed out."),
  );
});

void test("a cancelled or failed refresh keeps prior successful content instead of erasing it", () => {
  const outcome = resolvePreprocessorExplorerRefresh(
    true,
    undefined,
    "The request was cancelled.",
  );
  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
});

void test("a null result with prior content is treated the same as a failure: content is kept", () => {
  const outcome = resolvePreprocessorExplorerRefresh(true, null, undefined);
  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
});

void test("parsePreprocessorLocationCommandArg accepts a well-formed location", () => {
  const parsed = parsePreprocessorLocationCommandArg({
    uri: "file:///c:/scene/common.hlsli",
    range: {
      start: { line: 1, character: 0 },
      end: { line: 1, character: 5 },
    },
  });
  assert.deepEqual(parsed, {
    uri: "file:///c:/scene/common.hlsli",
    range: {
      start: { line: 1, character: 0 },
      end: { line: 1, character: 5 },
    },
  });
});

void test("parsePreprocessorLocationCommandArg rejects a missing or empty uri", () => {
  assert.equal(
    parsePreprocessorLocationCommandArg({
      range: {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parsePreprocessorLocationCommandArg({
      uri: "",
      range: {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
});

void test("parsePreprocessorLocationCommandArg rejects malformed, non-integer, negative, or inverted ranges", () => {
  const uri = "file:///c:/scene/common.hlsli";
  assert.equal(
    parsePreprocessorLocationCommandArg({ uri, range: undefined }),
    undefined,
  );
  assert.equal(
    parsePreprocessorLocationCommandArg({
      uri,
      range: {
        start: { line: 0.5, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parsePreprocessorLocationCommandArg({
      uri,
      range: {
        start: { line: -1, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parsePreprocessorLocationCommandArg({
      uri,
      range: {
        start: { line: 3, character: 0 },
        end: { line: 2, character: 0 },
      },
    }),
    undefined,
  );
});

void test("parsePreprocessorLocationCommandArg rejects non-object and null arguments", () => {
  assert.equal(parsePreprocessorLocationCommandArg(null), undefined);
  assert.equal(parsePreprocessorLocationCommandArg(undefined), undefined);
  assert.equal(
    parsePreprocessorLocationCommandArg("file:///c:/scene/shader.hlsl"),
    undefined,
  );
  assert.equal(parsePreprocessorLocationCommandArg(42), undefined);
});

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
