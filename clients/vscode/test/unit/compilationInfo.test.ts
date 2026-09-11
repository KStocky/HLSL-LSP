import assert from "node:assert/strict";
import test from "node:test";

import {
  CompilationInfo,
  compilationInfoErrorHtml,
  compilationInfoHtml,
  copyDisassemblyCommand,
  disassemblyFileName,
  resolveCompilationInfoRefresh,
  saveDisassemblyCommand,
} from "../../src/compilationInfo";

function baseInfo(overrides: Partial<CompilationInfo> = {}): CompilationInfo {
  return {
    entryPoint: "PSMain",
    stage: "pixel",
    targetProfile: "ps_6_6",
    languageVersion: "2021",
    defines: ["USE_TINT=1"],
    compilerArguments: ["-HV", "2021"],
    includeDirectories: ["Shaders/Includes"],
    resolvedIncludePaths: ["C:/project/Shaders/Includes"],
    activeVariant: "Prod",
    success: true,
    diagnostics: [],
    output: { type: "dxil", size: 1024 },
    disassembly: null,
    reflection: {
      available: true,
      unavailableReason: "",
      inputSignature: [
        {
          semanticName: "SV_Position",
          semanticIndex: 0,
          register: 0,
          systemValue: "position",
          componentType: "float32",
          mask: 15,
          readWriteMask: 15,
          stream: 0,
        },
      ],
      outputSignature: [
        {
          semanticName: "SV_Target",
          semanticIndex: 0,
          register: 0,
          systemValue: "target",
          componentType: "float32",
          mask: 15,
          readWriteMask: 0,
          stream: 0,
        },
      ],
      resources: [
        {
          name: "MainTexture",
          type: "texture",
          bindPoint: 0,
          bindCount: 1,
          space: 0,
          dimension: "texture2d",
          returnType: "float",
          registerClass: "srv",
          rawFlags: 0,
          rangeId: 0,
          sampleCount: 0xffffffff,
          unbounded: false,
          systemReservedSpace: false,
          usage: "used",
          sourceLocation: null,
        },
      ],
      threadGroupSize: null,
      bindingAnalysis: { groups: [], collisions: [] },
    },
    rootSignature: null,
    compatibility: null,
    ...overrides,
  };
}

void test("compilation info HTML renders the full successful result", () => {
  const html = compilationInfoHtml(baseInfo());

  assert.match(html, /PSMain/);
  assert.match(html, /Compilation succeeded/);
  assert.match(html, /ps_6_6/);
  assert.match(html, /Prod/);
  assert.match(html, /USE_TINT=1/);
  assert.match(html, /Shaders\/Includes/);
  assert.match(html, /dxil/);
  assert.match(html, /1024 bytes/);
  assert.match(html, /SV_Position/);
  assert.match(html, /MainTexture/);
  assert.match(html, /texture2d/);
  // Full-mask components render as letters rather than raw numbers.
  assert.match(html, /<td>xyzw<\/td>/);
});

void test("compilation info treats an explicit null effective variant as authoritative Default", () => {
  const html = compilationInfoHtml(
    baseInfo({
      activeVariant: "GloballySelectedButInapplicable",
      context: {
        documentUri: "file:///workspace/shader.hlsl",
        file: "shader.hlsl",
        activeVariant: null,
        entryPoint: "PSMain",
        targetProfile: "ps_6_6",
        origins: {},
      },
    }),
  );

  assert.match(html, /<th>Active variant<\/th><td>Default<\/td>/);
  assert.doesNotMatch(html, /GloballySelectedButInapplicable/);
});

void test("compilation info HTML renders a compiler failure with diagnostics", () => {
  const html = compilationInfoHtml(
    baseInfo({
      success: false,
      output: null,
      reflection: null,
      diagnostics: [
        {
          severity: "error",
          message: "undeclared identifier 'does_not_exist'",
          path: "shader.hlsl",
          line: 4,
          column: 10,
        },
      ],
    }),
  );

  assert.match(html, /Compilation failed/);
  assert.match(html, /undeclared identifier/);
  assert.match(html, /shader\.hlsl/);
  assert.match(html, /No compiled output was produced/);
  assert.match(
    html,
    /Reflection metadata is not available because no compiled output was produced/,
  );
  assert.doesNotMatch(html, /Compilation succeeded/);
});

void test("compilation info HTML explains unavailable SPIR-V reflection", () => {
  const html = compilationInfoHtml(
    baseInfo({
      output: { type: "spirv", size: 512 },
      reflection: {
        available: false,
        unavailableReason: "Reflection requires DXIL output.",
        inputSignature: [],
        outputSignature: [],
        resources: [],
        threadGroupSize: null,
        bindingAnalysis: { groups: [], collisions: [] },
      },
    }),
  );

  assert.match(html, /spirv/);
  assert.match(
    html,
    /Reflection is unavailable: Reflection requires DXIL output\./,
  );
  assert.doesNotMatch(html, /<h3>Resources<\/h3>\s*<table>/);
});

void test("compilation info HTML renders compute thread-group size", () => {
  const html = compilationInfoHtml(
    baseInfo({
      stage: "compute",
      reflection: {
        available: true,
        unavailableReason: "",
        inputSignature: [],
        outputSignature: [],
        resources: [],
        threadGroupSize: { x: 8, y: 8, z: 1 },
        bindingAnalysis: { groups: [], collisions: [] },
      },
    }),
  );

  assert.match(html, /Thread-group size/);
  assert.match(html, /<td>8<\/td><td>8<\/td><td>1<\/td>/);
});

void test("compilation info HTML escapes untrusted diagnostic and resource text", () => {
  const html = compilationInfoHtml(
    baseInfo({
      entryPoint: "Main<script>",
      diagnostics: [
        {
          severity: "error",
          message: "<script>alert(1)</script>",
          path: "<injected>.hlsl",
          line: 1,
          column: 1,
        },
      ],
      reflection: {
        available: true,
        unavailableReason: "",
        inputSignature: [],
        outputSignature: [],
        resources: [
          {
            name: "<evil>",
            type: "texture",
            bindPoint: 0,
            bindCount: 1,
            space: 0,
            dimension: "texture2d",
            returnType: "float",
            registerClass: "srv",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 0xffffffff,
            unbounded: false,
            systemReservedSpace: false,
            usage: "used",
            sourceLocation: null,
          },
        ],
        threadGroupSize: null,
        bindingAnalysis: { groups: [], collisions: [] },
      },
    }),
  );

  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.doesNotMatch(html, /<evil>/);
  assert.doesNotMatch(html, /Main<script>/);
  assert.match(html, /Main&lt;script&gt;/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
  assert.match(html, /&lt;evil&gt;/);
  assert.match(html, /&lt;injected&gt;\.hlsl/);
});

void test("compilation info error HTML escapes the failure message", () => {
  const html = compilationInfoErrorHtml("<script>alert(1)</script> failed");

  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt; failed/);
});

void test("compilation info HTML reports no disassembly when there is no output", () => {
  const html = compilationInfoHtml(baseInfo({ disassembly: null }));

  assert.match(
    html,
    /Disassembly is not available because no compiled output was produced/,
  );
  assert.doesNotMatch(html, /Copy Disassembly/);
  assert.doesNotMatch(html, /<pre class="disassembly">/);
});

void test("compilation info HTML explains unavailable disassembly without offering Copy/Save", () => {
  const html = compilationInfoHtml(
    baseInfo({
      disassembly: {
        available: false,
        text: "",
        unavailableReason: "DXC could not disassemble the compiled output.",
        truncated: false,
        originalSize: 0,
        displayedSize: 0,
        format: "spirv",
      },
    }),
  );

  assert.match(
    html,
    /Disassembly is unavailable: DXC could not disassemble the compiled output\./,
  );
  assert.doesNotMatch(html, /Copy Disassembly/);
  assert.doesNotMatch(html, /Save Disassembly/);
  assert.doesNotMatch(html, /<pre class="disassembly">/);
});

void test("compilation info HTML renders available disassembly with Copy/Save links and no truncation notice", () => {
  const html = compilationInfoHtml(
    baseInfo({
      disassembly: {
        available: true,
        text: "; DXIL disassembly\ntarget datalayout = ...",
        unavailableReason: "",
        truncated: false,
        originalSize: 42,
        displayedSize: 42,
        format: "dxil",
      },
    }),
  );

  assert.match(html, /; DXIL disassembly/);
  assert.match(html, /42 bytes/);
  assert.match(html, new RegExp(`href="command:${copyDisassemblyCommand}"`));
  assert.match(html, new RegExp(`href="command:${saveDisassemblyCommand}"`));
  assert.doesNotMatch(html, /truncated for display/);
});

void test("compilation info HTML reports truncation and still exposes Copy/Save for the retained text", () => {
  const html = compilationInfoHtml(
    baseInfo({
      disassembly: {
        available: true,
        text: "partial text",
        unavailableReason: "",
        truncated: true,
        originalSize: 4194304 + 100,
        displayedSize: 4194304,
        format: "spirv",
      },
    }),
  );

  assert.match(html, /partial text/);
  assert.match(html, /4194304 of 4194404 bytes/);
  assert.match(html, /truncated for display/);
  assert.match(html, new RegExp(`href="command:${copyDisassemblyCommand}"`));
});

void test("compilation info HTML escapes untrusted disassembly text", () => {
  const html = compilationInfoHtml(
    baseInfo({
      disassembly: {
        available: true,
        text: "<script>alert(1)</script>",
        unavailableReason: "",
        truncated: false,
        originalSize: 26,
        displayedSize: 26,
        format: "dxil",
      },
    }),
  );

  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
});

void test("disassemblyFileName prefers .ll for dxil and swaps the document's extension", () => {
  assert.equal(
    disassemblyFileName("/workspace/shaders/lighting.hlsl", "dxil"),
    "lighting.ll",
  );
});

void test("disassemblyFileName prefers .spvasm for spirv", () => {
  assert.equal(
    disassemblyFileName("/workspace/shaders/lighting.hlsl", "spirv"),
    "lighting.spvasm",
  );
});

void test("disassemblyFileName falls back to a generic name for an unnamed document", () => {
  assert.equal(disassemblyFileName("", "dxil"), "shader.ll");
});

void test("a refresh with a successful result renders content and remembers it", () => {
  const info = baseInfo();

  const outcome = resolveCompilationInfoRefresh(false, info, undefined);

  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, "Shader Compilation: PSMain");
  assert.equal(outcome.html, compilationInfoHtml(info));
  // A caller (extension.ts) stores this on the panel's state so Copy/Save
  // Disassembly always reads the most recently rendered result, never a
  // stale value from before this refresh.
  assert.equal(outcome.info, info);
});

void test("a failed refresh with no prior content shows an explicit error, not a placeholder", () => {
  const outcome = resolveCompilationInfoRefresh(
    false,
    undefined,
    "The language server crashed.",
  );

  assert.equal(outcome.hasContent, false);
  assert.equal(outcome.title, undefined);
  assert.equal(
    outcome.html,
    compilationInfoErrorHtml("The language server crashed."),
  );
  assert.doesNotMatch(outcome.html, /Compiling…/);
});

void test("a failed refresh with no prior content and no message uses a generic explanation", () => {
  const outcome = resolveCompilationInfoRefresh(false, null, undefined);

  assert.match(
    outcome.html ?? "",
    /The HLSL language server is not currently available\./,
  );
});

void test("a cancelled or failed refresh keeps prior successful content instead of erasing it", () => {
  const outcome = resolveCompilationInfoRefresh(
    true,
    undefined,
    "Request cancelled",
  );

  // undefined html means "leave the panel's current content untouched": the
  // last successful render must survive a subsequent failure or
  // cancellation rather than being replaced by an error or a placeholder.
  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
  // A caller must not overwrite its stored "last successful info" (used by
  // Copy/Save Disassembly) with anything from this cancelled attempt.
  assert.equal(outcome.info, undefined);
});

void test("a null result with prior content is treated the same as a failure: content is kept", () => {
  const outcome = resolveCompilationInfoRefresh(true, null, undefined);

  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
});
