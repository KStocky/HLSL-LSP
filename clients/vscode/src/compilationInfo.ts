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

// The register class a resource binds through. Several distinct
// D3D_SIT_* reflection types collapse onto the same class (e.g. both
// textures and structured buffers are "srv"); mirrors
// hlsl_intellisense::dxc::ResourceRegisterClass.
export type ResourceRegisterClass = "cbv" | "srv" | "uav" | "sampler" | "unknown";

// Whether a reflected resource is used by the compiled shader. Pinned DXC
// was empirically observed to omit unreferenced resources from reflection
// entirely rather than emit them "unused", so "unused" is not expected to
// occur for the single-stage profiles this server reflects; "unknown"
// covers any resource whose usage flag could not be established.
export type ResourceUsageStatus = "used" | "unused" | "unknown";

// A single UTF-16 source position, matching the LSP `Position` shape
// (`{line, character}`, both zero-based).
export interface CompilationSourcePosition {
  readonly line: number;
  readonly character: number;
}

// An LSP-shaped `{start, end}` source range.
export interface CompilationSourceRange {
  readonly start: CompilationSourcePosition;
  readonly end: CompilationSourcePosition;
}

// The authoritative declaration site of a reflected resource, resolved by
// the server from DXC's own cursor/symbol index against the current
// (possibly unsaved) document snapshot -- never guessed by this client from
// a resource's name or by any text search.
export interface CompilationResourceSourceLocation {
  readonly uri: string;
  readonly range: CompilationSourceRange;
}

export interface CompilationResourceBinding {
  readonly name: string;
  readonly type: string;
  readonly bindPoint: number;
  readonly bindCount: number;
  readonly space: number;
  readonly dimension: string;
  readonly returnType: string;
  readonly registerClass: ResourceRegisterClass;
  // Raw D3D_SHADER_INPUT_FLAGS bitmask exactly as reported by the compiler;
  // not interpreted beyond the derived `usage` field.
  readonly rawFlags: number;
  readonly rangeId: number;
  // Raw NumSamples as reported by the compiler. For structured/RWStructured
  // buffers the compiler reuses this field to store the byte stride rather
  // than a sample count (an empirically confirmed reflection ABI quirk);
  // this value is passed through unchanged with no semantics beyond what
  // the compiler reports.
  readonly sampleCount: number;
  // True for a shader-side unbounded resource array (bindCount sentinel 0),
  // e.g. `Texture2D T[] : register(t0, space1)`. Distinct from the
  // UINT_MAX sentinel a root-signature descriptor range uses for an
  // unbounded range (see RootSignatureDescriptorRange.unbounded below).
  readonly unbounded: boolean;
  // True when `space` falls in D3D12's reserved system range
  // [0xfffffff0, 0xffffffff]; never user-addressable.
  readonly systemReservedSpace: boolean;
  readonly usage: ResourceUsageStatus;
  // Populated only when the compiler-reflected resource name matches
  // exactly one declaration cursor in the current unsaved source snapshot;
  // `null` when the name cannot be found at all, or matches more than one
  // declaration (a genuine ambiguity DXC's own cursor tree cannot resolve).
  // A client must never substitute a name-based guess when this is `null`.
  readonly sourceLocation: CompilationResourceSourceLocation | null;
}

// One register range occupied by a single reflected resource within a
// ResourceBindingGroup. `endRegister` is `null` exactly when `unbounded` is
// true (it can never overflow).
export interface ResourceBindingRange {
  readonly resourceName: string;
  readonly baseRegister: number;
  readonly unbounded: boolean;
  readonly endRegister: number | null;
}

// A provable overlap between two distinct reflected resource bindings that
// share the same register class and space. Never reported for
// system-reserved-space groups, duplicate names, or self comparisons.
export interface ResourceBindingCollision {
  readonly firstResource: string;
  readonly secondResource: string;
  readonly registerClass: ResourceRegisterClass;
  readonly space: number;
  readonly message: string;
}

// Resources grouped by register class and register space, each carrying the
// register range(s) it occupies. Group order as reported by the server is
// first-encountered order, not sorted by space/class; clients that want a
// space-major, class-minor presentation (as this extension does) must sort
// this array themselves.
export interface ResourceBindingGroup {
  readonly registerClass: ResourceRegisterClass;
  readonly space: number;
  readonly systemReservedSpace: boolean;
  readonly ranges: readonly ResourceBindingRange[];
}

// Deterministic grouping/collision analysis computed purely from already
// reflected register data; never HLSL parsing or inference.
export interface ResourceBindingAnalysis {
  readonly groups: readonly ResourceBindingGroup[];
  readonly collisions: readonly ResourceBindingCollision[];
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
  readonly bindingAnalysis: ResourceBindingAnalysis;
}

// Whether an embedded root signature is present in the compiled DXIL
// container, and if so, whether this platform can deserialize its details.
// Presence/absence detection works on every platform; only detailed
// deserialization requires the Windows D3D12 runtime (never a custom RTS0
// parser). Mirrors hlsl_intellisense::dxc::RootSignatureAvailability.
export type RootSignatureAvailability =
  | "present"
  | "absent"
  | "notApplicable"
  | "presentDetailsUnavailable";

// Mirrors D3D12_SHADER_VISIBILITY.
export type RootSignatureVisibility =
  | "all"
  | "vertex"
  | "hull"
  | "domain"
  | "geometry"
  | "pixel"
  | "amplification"
  | "mesh"
  | "unknown";

// The register class a root-signature entry grants access through. Mirrors
// D3D12_DESCRIPTOR_RANGE_TYPE for ranges; root descriptors/constants are
// always cbv/srv/uav (never sampler).
export type RootSignatureRangeType = "srv" | "uav" | "cbv" | "sampler" | "unknown";

// One descriptor range within a descriptor-table root parameter, mirroring
// D3D12_DESCRIPTOR_RANGE1. `numDescriptors` is `null` exactly when
// `unbounded` is true (NumDescriptors == UINT_MAX in the deserialized root
// signature) - a *different* convention from the shader-side BindCount == 0
// sentinel (see CompilationResourceBinding.unbounded).
export interface RootSignatureDescriptorRange {
  readonly type: RootSignatureRangeType;
  readonly numDescriptors: number | null;
  readonly unbounded: boolean;
  readonly baseRegister: number;
  readonly space: number;
  readonly rawFlags: number; // D3D12_DESCRIPTOR_RANGE_FLAGS (version 1.1)
  readonly offsetInDescriptorsFromTableStart: number;
}

export interface RootSignatureRootConstants {
  readonly shaderRegister: number;
  readonly space: number;
  readonly num32BitValues: number;
}

// A root CBV/SRV/UAV descriptor bound directly in the root signature,
// rather than through a descriptor table.
export interface RootSignatureRootDescriptor {
  readonly type: RootSignatureRangeType;
  readonly shaderRegister: number;
  readonly space: number;
  readonly rawFlags: number; // D3D12_ROOT_DESCRIPTOR_FLAGS (version 1.1)
}

export type RootSignatureParameterKind =
  | "descriptorTable"
  | "constants"
  | "rootDescriptor";

// One root parameter, mirroring D3D12_ROOT_PARAMETER1; exactly one of
// `constants`/`rootDescriptor` is non-null, or `descriptorTableRanges` is
// non-empty, matching `kind`.
export interface RootSignatureParameter {
  readonly kind: RootSignatureParameterKind;
  readonly visibility: RootSignatureVisibility;
  readonly descriptorTableRanges: readonly RootSignatureDescriptorRange[];
  readonly constants: RootSignatureRootConstants | null;
  readonly rootDescriptor: RootSignatureRootDescriptor | null;
}

// A static sampler declared directly in the root signature; never requires
// a descriptor-heap slot.
export interface RootSignatureStaticSampler {
  readonly shaderRegister: number;
  readonly space: number;
  readonly visibility: RootSignatureVisibility;
  readonly filter: number; // raw D3D12_FILTER
  readonly addressU: number; // raw D3D12_TEXTURE_ADDRESS_MODE
  readonly addressV: number;
  readonly addressW: number;
  readonly mipLodBias: number;
  readonly maxAnisotropy: number;
  readonly comparisonFunc: number; // raw D3D12_COMPARISON_FUNC
  readonly borderColor: number; // raw D3D12_STATIC_BORDER_COLOR
  readonly minLod: number;
  readonly maxLod: number;
}

// The deserialized contents of an embedded root signature, always exposed
// through the version-1.1 shape (root descriptors/ranges default flags to
// NONE for a version-1.0 signature) while `version` reports the true,
// unconverted version.
export interface RootSignatureDetails {
  readonly version: string; // "1.0" or "1.1"
  readonly rawFlags: number; // D3D12_ROOT_SIGNATURE_FLAGS, unchanged
  readonly cbvSrvUavHeapDirectlyIndexed: boolean;
  readonly samplerHeapDirectlyIndexed: boolean;
  readonly parameters: readonly RootSignatureParameter[];
  readonly staticSamplers: readonly RootSignatureStaticSampler[];
}

// The compiler-authoritative embedded root-signature result for a single
// compilation. Deserialization uses only the official Windows D3D12 API;
// there is no custom RTS0 binary parser in this codebase.
export interface RootSignatureInfo {
  readonly availability: RootSignatureAvailability;
  // Populated for "notApplicable" and "presentDetailsUnavailable",
  // explaining why (e.g. SPIR-V has no root signature concept, or this
  // platform lacks the Windows D3D12 root-signature deserializer).
  readonly unavailableReason: string;
  // Populated only when availability === "present".
  readonly details: RootSignatureDetails | null;
}

// Whether reflected shader resources are provably covered by the
// deserialized root signature for the compiled entry point's stage. Only
// register class/space/range coverage, active-stage shader visibility, and
// static samplers are compared; bindless (ResourceDescriptorHeap /
// SamplerDescriptorHeap) accesses are invisible to reflection and are never
// guessed at.
export type ResourceCompatibilityStatus = "compatible" | "incompatible" | "unknown";

export interface ResourceCompatibilityIssue {
  readonly resourceName: string;
  readonly registerClass: ResourceRegisterClass;
  readonly space: number;
  readonly message: string;
}

export interface CompilationCompatibility {
  readonly status: ResourceCompatibilityStatus;
  // Populated when status === "unknown", explaining why compatibility
  // could not be determined (e.g. no embedded root signature, or its
  // details are unavailable on this platform).
  readonly explanation: string;
  readonly issues: readonly ResourceCompatibilityIssue[];
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
  // Populated whenever `output` exists, for both DXIL and SPIR-V output;
  // SPIR-V is represented as availability "notApplicable" rather than by
  // this field being `null`. `null` only when there is no output at all
  // (for example, a failed compilation).
  readonly rootSignature: RootSignatureInfo | null;
  // Non-null whenever `rootSignature` is non-null: unconditionally for
  // SPIR-V output, and for DXIL output regardless of whether reflection
  // metadata itself is available. When DXIL reflection is unavailable (for
  // example, no root signature, or the reflected resource list needed to
  // compare against a present root signature could not be obtained),
  // `status` reads "unknown" with an explanation rather than the field
  // being `null` -- reporting `null` compatibility while a root signature
  // is present would risk a client silently treating an unanalyzed result
  // as compatible. Both `rootSignature` and `compatibility` are `null`
  // only when compilation produced no output at all (for example, a
  // failed compilation).
  readonly compatibility: CompilationCompatibility | null;
}

export function escapeHtml(value: string): string {
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
</table>
<p class="muted">Run <strong>HLSL: Show Resource Bindings</strong> for register spaces, collisions, root-signature details, and compatibility.</p>`;
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
