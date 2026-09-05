import assert from "node:assert/strict";
import test from "node:test";

import { CompilationInfo, CompilationReflection } from "../../src/compilationInfo";
import {
  parseResourceLocationCommandArg,
  resolveResourceBindingsRefresh,
  resourceBindingsErrorHtml,
  resourceBindingsHtml,
} from "../../src/resourceBindings";

function baseReflection(
  overrides: Partial<CompilationReflection> = {},
): CompilationReflection {
  return {
    available: true,
    unavailableReason: "",
    inputSignature: [],
    outputSignature: [],
    resources: [],
    threadGroupSize: null,
    bindingAnalysis: { groups: [], collisions: [] },
    ...overrides,
  };
}

function baseInfo(overrides: Partial<CompilationInfo> = {}): CompilationInfo {
  return {
    entryPoint: "PSMain",
    stage: "pixel",
    targetProfile: "ps_6_6",
    languageVersion: "2021",
    defines: [],
    compilerArguments: [],
    includeDirectories: [],
    resolvedIncludePaths: [],
    activeVariant: null,
    success: true,
    diagnostics: [],
    output: { type: "dxil", size: 1024 },
    reflection: baseReflection(),
    rootSignature: { availability: "absent", unavailableReason: "", details: null },
    compatibility: { status: "unknown", explanation: "No embedded root signature.", issues: [] },
    ...overrides,
  };
}

void test("resource bindings HTML groups resources by register space, then class", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "AlbedoTexture",
            type: "texture",
            bindPoint: 0,
            bindCount: 1,
            space: 1,
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
          {
            name: "SceneConstants",
            type: "cbuffer",
            bindPoint: 0,
            bindCount: 1,
            space: 0,
            dimension: "",
            returnType: "",
            registerClass: "cbv",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 0xffffffff,
            unbounded: false,
            systemReservedSpace: false,
            usage: "used",
            sourceLocation: null,
          },
          {
            name: "LinearSampler",
            type: "sampler",
            bindPoint: 0,
            bindCount: 1,
            space: 1,
            dimension: "",
            returnType: "",
            registerClass: "sampler",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 0xffffffff,
            unbounded: false,
            systemReservedSpace: false,
            usage: "used",
            sourceLocation: null,
          },
        ],
        bindingAnalysis: {
          groups: [
            {
              registerClass: "srv",
              space: 1,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "AlbedoTexture", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
            {
              registerClass: "cbv",
              space: 0,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "SceneConstants", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
            {
              registerClass: "sampler",
              space: 1,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "LinearSampler", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
          ],
          collisions: [],
        },
      }),
    }),
  );

  // Space 0 (CBV) must render before space 1's groups, and within space 1,
  // SRV must render before Sampler (CBV, SRV, UAV, Sampler order).
  const spaceZero = html.indexOf("Space 0");
  const spaceOneSrv = html.indexOf("Space 1 &mdash; SRV");
  const spaceOneSampler = html.indexOf("Space 1 &mdash; Sampler");
  assert(spaceZero >= 0 && spaceOneSrv >= 0 && spaceOneSampler >= 0);
  assert(spaceZero < spaceOneSrv);
  assert(spaceOneSrv < spaceOneSampler);
});

void test("resource bindings HTML renders collisions with class and space context", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        bindingAnalysis: {
          groups: [
            {
              registerClass: "srv",
              space: 0,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "A", baseRegister: 0, unbounded: false, endRegister: 1 },
                { resourceName: "B", baseRegister: 1, unbounded: false, endRegister: 2 },
              ],
            },
          ],
          collisions: [
            {
              firstResource: "A",
              secondResource: "B",
              registerClass: "srv",
              space: 0,
              message: "'A' (registers 0-1) and 'B' (registers 1-2) both occupy space 0 in the same register class",
            },
          ],
        },
      }),
    }),
  );

  assert.match(html, /<h2>Collisions<\/h2>/);
  assert.match(html, /SRV<\/strong> space 0/);
  assert.match(html, /both occupy space 0/);
});

void test("resource bindings HTML reports no collisions when none are found", () => {
  const html = resourceBindingsHtml(baseInfo());

  assert.match(
    html,
    /No provable register-range collisions were found between distinct resources/,
  );
});

void test("resource bindings HTML renders unbounded arrays distinctly from bounded ones", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "Textures",
            type: "texture",
            bindPoint: 0,
            bindCount: 0,
            space: 1,
            dimension: "texture2d",
            returnType: "float",
            registerClass: "srv",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 0xffffffff,
            unbounded: true,
            systemReservedSpace: false,
            usage: "used",
            sourceLocation: null,
          },
        ],
        bindingAnalysis: {
          groups: [
            {
              registerClass: "srv",
              space: 1,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "Textures", baseRegister: 0, unbounded: true, endRegister: 0 },
              ],
            },
          ],
          collisions: [],
        },
      }),
    }),
  );

  assert.match(html, /and above \(unbounded\)/);
  assert.match(html, /<td>unbounded<\/td>/);
});

void test("resource bindings HTML renders byte stride, not sample count, for every structured-buffer type", () => {
  // The server's resource_type_name maps D3D_SIT_STRUCTURED to
  // "structured_buffer" (not "structured"), and every UAV
  // structured-buffer variant reuses NumSamples as the byte stride the
  // same way: RWStructuredBuffer, RWStructuredBuffer with an implicit
  // hidden counter, AppendStructuredBuffer, and ConsumeStructuredBuffer.
  const structuredTypes = [
    "structured_buffer",
    "uav_rwstructured",
    "uav_rwstructured_with_counter",
    "uav_append_structured",
    "uav_consume_structured",
  ] as const;

  for (const type of structuredTypes) {
    const registerClass = type === "structured_buffer" ? "srv" : "uav";
    const html = resourceBindingsHtml(
      baseInfo({
        reflection: baseReflection({
          resources: [
            {
              name: "Particles",
              type,
              bindPoint: 0,
              bindCount: 1,
              space: 0,
              dimension: "",
              returnType: "",
              registerClass,
              rawFlags: 0,
              rangeId: 0,
              sampleCount: 32,
              unbounded: false,
              systemReservedSpace: false,
              usage: "used",
              sourceLocation: null,
            },
          ],
          bindingAnalysis: {
            groups: [
              {
                registerClass,
                space: 0,
                systemReservedSpace: false,
                ranges: [
                  { resourceName: "Particles", baseRegister: 0, unbounded: false, endRegister: 0 },
                ],
              },
            ],
            collisions: [],
          },
        }),
      }),
    );

    assert.match(html, /<td>stride 32 bytes<\/td>/, `expected stride rendering for type "${type}"`);
    assert.doesNotMatch(html, /<td>32<\/td>/, `did not expect a raw sample-count cell for type "${type}"`);
  }
});

void test("resource bindings HTML renders the raw sample count, not a byte stride, for non-structured resource types", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "MsaaTarget",
            type: "uav_rwtyped",
            bindPoint: 0,
            bindCount: 1,
            space: 0,
            dimension: "texture2dms",
            returnType: "float",
            registerClass: "uav",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 4,
            unbounded: false,
            systemReservedSpace: false,
            usage: "used",
            sourceLocation: null,
          },
        ],
        bindingAnalysis: {
          groups: [
            {
              registerClass: "uav",
              space: 0,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "MsaaTarget", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
          ],
          collisions: [],
        },
      }),
    }),
  );

  assert.match(html, /<td>4<\/td>/);
  assert.doesNotMatch(html, /<td>stride/);
});

void test("resource bindings HTML classifies system-reserved register spaces distinctly", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "Internal",
            type: "cbuffer",
            bindPoint: 0,
            bindCount: 1,
            space: 0xfffffff0,
            dimension: "",
            returnType: "",
            registerClass: "cbv",
            rawFlags: 0,
            rangeId: 0,
            sampleCount: 0xffffffff,
            unbounded: false,
            systemReservedSpace: true,
            usage: "unknown",
            sourceLocation: null,
          },
        ],
        bindingAnalysis: {
          groups: [
            {
              registerClass: "cbv",
              space: 0xfffffff0,
              systemReservedSpace: true,
              ranges: [
                { resourceName: "Internal", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
          ],
          collisions: [],
        },
      }),
    }),
  );

  assert.match(html, /badge reserved/);
  assert.match(html, /system-reserved/);
});

void test("resource bindings HTML explains an absent root signature", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      rootSignature: { availability: "absent", unavailableReason: "", details: null },
    }),
  );

  assert.match(html, /No embedded root signature was found/);
});

void test("resource bindings HTML never claims a root signature is absent merely because output is null", () => {
  // rootSignature is null only when compilation produced no output at all
  // (for example, a failed compilation) -- distinct from availability
  // "absent", which means output exists but carries no root signature.
  // compatibility is null together with rootSignature in this case too
  // (never independently), so both null-case messages are asserted here.
  const html = resourceBindingsHtml(
    baseInfo({
      output: null,
      reflection: null,
      rootSignature: null,
      compatibility: null,
    }),
  );

  assert.match(html, /did not produce any output/);
  assert.doesNotMatch(html, /No embedded root signature was found/);
  assert.match(
    html,
    /Compatibility information is not available because compilation did not produce any output/,
  );
});

void test("resource bindings HTML never claims compatibility is unavailable while a root signature is present", () => {
  // The normalized contract: compatibility is non-null whenever
  // rootSignature is non-null. When DXIL reflection metadata itself is
  // unavailable (for example, GetDesc/CreateReflection failed) but a root
  // signature is still present, compatibility reports status "unknown"
  // with an explanation rather than being null -- reporting null here
  // would risk a client silently treating an unanalyzed result as
  // compatible.
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        available: false,
        unavailableReason:
          "DXIL reflection metadata is unavailable for this compiled output.",
      }),
      rootSignature: {
        availability: "present",
        unavailableReason: "",
        details: {
          version: "1.1",
          rawFlags: 0,
          cbvSrvUavHeapDirectlyIndexed: false,
          samplerHeapDirectlyIndexed: false,
          parameters: [],
          staticSamplers: [],
        },
      },
      compatibility: {
        status: "unknown",
        explanation:
          "Resource/root-signature compatibility could not be determined because DXIL reflection metadata is unavailable for this compiled output.",
        issues: [],
      },
    }),
  );

  assert.match(html, /Compatibility is unknown/);
  assert.match(
    html,
    /reflection metadata is unavailable for this compiled output/,
  );
  assert.doesNotMatch(html, /did not produce any output/);
});

void test("resource bindings HTML explains SPIR-V not-applicable root signatures", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      output: { type: "spirv", size: 512 },
      reflection: baseReflection({
        available: false,
        unavailableReason: "Reflection requires DXIL output.",
      }),
      rootSignature: {
        availability: "notApplicable",
        unavailableReason:
          "SPIR-V output has no root signature concept; root signatures are a Direct3D 12 binding-model construct",
        details: null,
      },
      compatibility: {
        status: "unknown",
        explanation: "Root signature compatibility does not apply to this compilation target (for example, SPIR-V).",
        issues: [],
      },
    }),
  );

  assert.match(html, /SPIR-V output has no root signature concept/);
});

void test("resource bindings HTML explains present-but-unavailable root signature details, including a Linux reason", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      rootSignature: {
        availability: "presentDetailsUnavailable",
        unavailableReason:
          "An embedded root signature is present, but detailed inspection requires the Windows D3D12 runtime's ID3D12VersionedRootSignatureDeserializer, which is unavailable on this platform; only presence could be determined",
        details: null,
      },
    }),
  );

  assert.match(html, /details could not be retrieved on this platform/);
  assert.match(html, /unavailable on this platform; only presence could be determined/);
});

void test("resource bindings HTML renders full root signature details", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      rootSignature: {
        availability: "present",
        unavailableReason: "",
        details: {
          version: "1.1",
          rawFlags: 1,
          cbvSrvUavHeapDirectlyIndexed: true,
          samplerHeapDirectlyIndexed: false,
          parameters: [
            {
              kind: "descriptorTable",
              visibility: "pixel",
              descriptorTableRanges: [
                {
                  type: "srv",
                  numDescriptors: 4,
                  unbounded: false,
                  baseRegister: 0,
                  space: 0,
                  rawFlags: 0,
                  offsetInDescriptorsFromTableStart: 0,
                },
                {
                  type: "uav",
                  numDescriptors: null,
                  unbounded: true,
                  baseRegister: 0,
                  space: 2,
                  rawFlags: 0,
                  offsetInDescriptorsFromTableStart: 0xffffffff,
                },
              ],
              constants: null,
              rootDescriptor: null,
            },
            {
              kind: "constants",
              visibility: "all",
              descriptorTableRanges: [],
              constants: { shaderRegister: 0, space: 0, num32BitValues: 4 },
              rootDescriptor: null,
            },
            {
              kind: "rootDescriptor",
              visibility: "vertex",
              descriptorTableRanges: [],
              constants: null,
              rootDescriptor: { type: "cbv", shaderRegister: 1, space: 0, rawFlags: 0 },
            },
          ],
          staticSamplers: [
            {
              shaderRegister: 0,
              space: 0,
              visibility: "pixel",
              filter: 0x55,
              addressU: 1,
              addressV: 1,
              addressW: 1,
              mipLodBias: 0,
              maxAnisotropy: 0,
              comparisonFunc: 0,
              borderColor: 0,
              minLod: 0,
              maxLod: 3.402823466e38,
            },
          ],
        },
      },
      compatibility: { status: "compatible", explanation: "", issues: [] },
    }),
  );

  assert.match(html, /<th>Version<\/th><td>1\.1<\/td>/);
  assert.match(html, /CBV\/SRV\/UAV heap directly indexed<\/th><td>yes<\/td>/);
  assert.match(html, /Sampler heap directly indexed<\/th><td>no<\/td>/);
  assert.match(html, /descriptorTable/);
  assert.match(html, /unbounded<\/td>/);
  assert.match(html, /root constants: register b0, space 0, 4 x 32-bit values/);
  assert.match(html, /root descriptor: CBV register 1, space 0/);
  assert.match(html, /Static samplers/);
});

void test("resource bindings HTML reports compatible, incompatible, and unknown compatibility distinctly", () => {
  const compatibleHtml = resourceBindingsHtml(
    baseInfo({
      compatibility: { status: "compatible", explanation: "", issues: [] },
    }),
  );
  assert.match(compatibleHtml, /<h2 class="status-success">Compatible<\/h2>/);
  assert.match(compatibleHtml, /covered by the embedded root signature/);

  const incompatibleHtml = resourceBindingsHtml(
    baseInfo({
      compatibility: {
        status: "incompatible",
        explanation: "",
        issues: [
          {
            resourceName: "AlbedoTexture",
            registerClass: "srv",
            space: 0,
            message: "'AlbedoTexture' has no corresponding root signature entry",
          },
        ],
      },
    }),
  );
  assert.match(incompatibleHtml, /<h2 class="status-failure">Incompatible<\/h2>/);
  assert.match(incompatibleHtml, /AlbedoTexture<\/strong> \(SRV, space 0\)/);
  assert.match(incompatibleHtml, /has no corresponding root signature entry/);

  const unknownHtml = resourceBindingsHtml(
    baseInfo({
      compatibility: {
        status: "unknown",
        explanation:
          "An embedded root signature is present, but its details are unavailable on this platform, so compatibility cannot be determined.",
        issues: [],
      },
    }),
  );
  assert.match(unknownHtml, /Compatibility is unknown/);
  assert.match(unknownHtml, /Unavailable root-signature details are never treated as compatible/);
});

void test("resource bindings HTML always explains the bindless descriptor-heap limitation", () => {
  const html = resourceBindingsHtml(baseInfo());

  assert.match(
    html,
    /Bindless accesses through <code>ResourceDescriptorHeap<\/code>\/<code>SamplerDescriptorHeap<\/code> are invisible to compiler reflection/,
  );
});

void test("resource bindings HTML explains that navigation is clickable only for unambiguous compiler-supplied locations", () => {
  const html = resourceBindingsHtml(baseInfo());

  assert.match(
    html,
    /clickable only when DXC's reflection supplies an unambiguous declaration location/,
  );
  assert.match(html, /renders as plain text instead of a guessed link/);
  assert.doesNotMatch(html, /not yet available/);
});

void test("resource bindings HTML escapes untrusted resource and collision text", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      entryPoint: "Main<script>",
      reflection: baseReflection({
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
        bindingAnalysis: {
          groups: [
            {
              registerClass: "srv",
              space: 0,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "<evil>", baseRegister: 0, unbounded: false, endRegister: 0 },
              ],
            },
          ],
          collisions: [
            {
              firstResource: "<evil>",
              secondResource: "<also-evil>",
              registerClass: "srv",
              space: 0,
              message: "<script>alert(1)</script>",
            },
          ],
        },
      }),
    }),
  );

  assert.doesNotMatch(html, /Main<script>/);
  assert.doesNotMatch(html, /<evil>/);
  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /Main&lt;script&gt;/);
  assert.match(html, /&lt;evil&gt;/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
});

void test("resource bindings HTML links a resource row only when it has a compiler-supplied source location", () => {
  const html = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "AlbedoTexture",
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
            sourceLocation: {
              uri: "file:///c:/scene/shader.hlsl",
              range: { start: { line: 3, character: 0 }, end: { line: 3, character: 20 } },
            },
          },
          {
            name: "NormalTexture",
            type: "texture",
            bindPoint: 1,
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
        bindingAnalysis: {
          groups: [
            {
              registerClass: "srv",
              space: 0,
              systemReservedSpace: false,
              ranges: [
                { resourceName: "AlbedoTexture", baseRegister: 0, unbounded: false, endRegister: 0 },
                { resourceName: "NormalTexture", baseRegister: 1, unbounded: false, endRegister: 1 },
              ],
            },
          ],
          collisions: [],
        },
      }),
    }),
  );

  assert.match(
    html,
    /<a href="command:hlsl\.resourceBindings\.openLocation\?[^"]*" title="Go to declaration">AlbedoTexture<\/a>/,
  );
  assert.match(html, /<td>NormalTexture<\/td>/);
  assert.doesNotMatch(html, /<a[^>]*>NormalTexture<\/a>/);
});

void test("resource bindings HTML links both collision participants only when each maps to exactly one resource entry", () => {
  const htmlWithLocations = resourceBindingsHtml(
    baseInfo({
      reflection: baseReflection({
        resources: [
          {
            name: "A",
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
            sourceLocation: {
              uri: "file:///c:/scene/shader.hlsl",
              range: { start: { line: 1, character: 0 }, end: { line: 1, character: 5 } },
            },
          },
          {
            name: "B",
            type: "texture",
            bindPoint: 1,
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
        bindingAnalysis: {
          groups: [],
          collisions: [
            {
              firstResource: "A",
              secondResource: "B",
              registerClass: "srv",
              space: 0,
              message: "'A' and 'B' collide",
            },
          ],
        },
      }),
    }),
  );

  assert.match(
    htmlWithLocations,
    /<a href="command:hlsl\.resourceBindings\.openLocation\?[^"]*" title="Go to declaration">A<\/a> &harr; B/,
  );
  assert.doesNotMatch(htmlWithLocations, /<a[^>]*>B<\/a>/);
});

void test("resource bindings HTML never links a collision participant that maps to more than one resource entry", () => {
  const duplicateResource: CompilationInfo["reflection"] = baseReflection({
    resources: [
      {
        name: "Dup",
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
        sourceLocation: {
          uri: "file:///c:/scene/shader.hlsl",
          range: { start: { line: 1, character: 0 }, end: { line: 1, character: 5 } },
        },
      },
      {
        name: "Dup",
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
        sourceLocation: {
          uri: "file:///c:/scene/shader2.hlsl",
          range: { start: { line: 4, character: 0 }, end: { line: 4, character: 5 } },
        },
      },
    ],
    bindingAnalysis: {
      groups: [],
      collisions: [
        {
          firstResource: "Dup",
          secondResource: "Dup",
          registerClass: "srv",
          space: 0,
          message: "'Dup' collides with itself across ranges",
        },
      ],
    },
  });

  const html = resourceBindingsHtml(baseInfo({ reflection: duplicateResource }));

  assert.doesNotMatch(html, /<a[^>]*>Dup<\/a>/);
  assert.match(html, /\(Dup &harr; Dup\)/);
});

void test("parseResourceLocationCommandArg accepts a well-formed location", () => {
  const parsed = parseResourceLocationCommandArg({
    uri: "file:///c:/scene/shader.hlsl",
    range: { start: { line: 1, character: 2 }, end: { line: 1, character: 8 } },
  });

  assert.deepEqual(parsed, {
    uri: "file:///c:/scene/shader.hlsl",
    range: { start: { line: 1, character: 2 }, end: { line: 1, character: 8 } },
  });
});

void test("parseResourceLocationCommandArg rejects a missing or empty uri", () => {
  assert.equal(
    parseResourceLocationCommandArg({
      uri: "",
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
    }),
    undefined,
  );
  assert.equal(
    parseResourceLocationCommandArg({
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
    }),
    undefined,
  );
});

void test("parseResourceLocationCommandArg rejects malformed, non-integer, negative, or inverted ranges", () => {
  const uri = "file:///c:/scene/shader.hlsl";

  assert.equal(parseResourceLocationCommandArg({ uri, range: undefined }), undefined);
  assert.equal(
    parseResourceLocationCommandArg({
      uri,
      range: { start: { line: 1.5, character: 0 }, end: { line: 1, character: 1 } },
    }),
    undefined,
  );
  assert.equal(
    parseResourceLocationCommandArg({
      uri,
      range: { start: { line: -1, character: 0 }, end: { line: 1, character: 1 } },
    }),
    undefined,
  );
  assert.equal(
    parseResourceLocationCommandArg({
      uri,
      range: { start: { line: 2, character: 0 }, end: { line: 1, character: 0 } },
    }),
    undefined,
  );
  assert.equal(
    parseResourceLocationCommandArg({
      uri,
      range: { start: { line: 1, character: 5 }, end: { line: 1, character: 1 } },
    }),
    undefined,
  );
});

void test("parseResourceLocationCommandArg rejects non-object and null arguments", () => {
  assert.equal(parseResourceLocationCommandArg(null), undefined);
  assert.equal(parseResourceLocationCommandArg(undefined), undefined);
  assert.equal(parseResourceLocationCommandArg("file:///c:/scene/shader.hlsl"), undefined);
  assert.equal(parseResourceLocationCommandArg(42), undefined);
});

void test("resource bindings error HTML escapes the failure message", () => {
  const html = resourceBindingsErrorHtml("<script>alert(1)</script> failed");

  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt; failed/);
});

void test("a refresh with a successful result renders content and remembers it", () => {
  const info = baseInfo();

  const outcome = resolveResourceBindingsRefresh(false, info, undefined);

  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, "Resource Bindings: PSMain");
  assert.equal(outcome.html, resourceBindingsHtml(info));
});

void test("a failed refresh with no prior content shows an explicit error, not a placeholder", () => {
  const outcome = resolveResourceBindingsRefresh(
    false,
    undefined,
    "The language server crashed.",
  );

  assert.equal(outcome.hasContent, false);
  assert.equal(outcome.title, undefined);
  assert.equal(
    outcome.html,
    resourceBindingsErrorHtml("The language server crashed."),
  );
});

void test("a cancelled or failed refresh keeps prior successful content instead of erasing it", () => {
  const outcome = resolveResourceBindingsRefresh(
    true,
    undefined,
    "Request cancelled",
  );

  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
});

void test("a null result with prior content is treated the same as a failure: content is kept", () => {
  const outcome = resolveResourceBindingsRefresh(true, null, undefined);

  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
});
