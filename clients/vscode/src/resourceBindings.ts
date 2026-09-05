import {
  CompilationCompatibility,
  CompilationInfo,
  CompilationResourceBinding,
  CompilationResourceSourceLocation,
  CompilationSourcePosition,
  CompilationSourceRange,
  escapeHtml,
  ResourceBindingCollision,
  ResourceBindingGroup,
  ResourceBindingRange,
  ResourceCompatibilityIssue,
  ResourceRegisterClass,
  RootSignatureDescriptorRange,
  RootSignatureDetails,
  RootSignatureInfo,
  RootSignatureParameter,
  RootSignatureRootConstants,
  RootSignatureRootDescriptor,
  RootSignatureStaticSampler,
} from "./compilationInfo";

// Dedicated **HLSL Resource Bindings** view: register-space/class grouping,
// collisions, embedded root-signature state, and root-signature
// compatibility. Kept separate from the Shader Compilation view (which shows
// a flat resource list alongside configuration/diagnostics/output) so
// neither view becomes an unreadable single page. Both views issue the same
// `hlsl/compilationInfo` request against the same current (possibly
// unsaved) document.

const registerClassOrder: readonly ResourceRegisterClass[] = [
  "cbv",
  "srv",
  "uav",
  "sampler",
  "unknown",
];

const registerClassLabels: Record<ResourceRegisterClass, string> = {
  cbv: "CBV",
  srv: "SRV",
  uav: "UAV",
  sampler: "Sampler",
  unknown: "Unknown",
};

function registerClassRank(value: ResourceRegisterClass): number {
  const index = registerClassOrder.indexOf(value);
  return index === -1 ? registerClassOrder.length : index;
}

// The server reports one group per (registerClass, space) pair in
// first-encountered order, not sorted. This view groups first by register
// space (ascending), then by register class in D3D-conventional order
// (CBV, SRV, UAV, Sampler), matching how engineers reason about binding
// layouts.
function sortedGroups(
  groups: readonly ResourceBindingGroup[],
): readonly ResourceBindingGroup[] {
  return [...groups].sort((left, right) => {
    if (left.space !== right.space) {
      return left.space - right.space;
    }
    return (
      registerClassRank(left.registerClass) -
      registerClassRank(right.registerClass)
    );
  });
}

// Full per-resource metadata (type/dimension/return type/sample count/usage)
// lives in `reflection.resources`, while grouping/range/collision data lives
// in `reflection.bindingAnalysis`. Both are computed by the server from the
// same reflected register data, so a (registerClass, space, name) key
// reliably joins them for display.
function resourceKey(
  registerClass: ResourceRegisterClass,
  space: number,
  name: string,
): string {
  return `${registerClass}|${String(space)}|${name}`;
}

// Excludes a key entirely (rather than keeping either candidate) when more
// than one resource shares the same (registerClass, space, name) key. This
// should not happen in practice -- reflected bindings are keyed by a real
// register/class/name triple -- but if it ever did, silently picking one
// candidate could point a collision participant or resource-row link at the
// wrong declaration; omitting the mapping instead means the row/participant
// simply renders as non-clickable text.
function resourceLookup(
  resources: readonly CompilationResourceBinding[],
): Map<string, CompilationResourceBinding> {
  const map = new Map<string, CompilationResourceBinding>();
  const ambiguousKeys = new Set<string>();
  for (const resource of resources) {
    const key = resourceKey(
      resource.registerClass,
      resource.space,
      resource.name,
    );
    if (map.has(key)) {
      ambiguousKeys.add(key);
      continue;
    }
    map.set(key, resource);
  }
  for (const key of ambiguousKeys) {
    map.delete(key);
  }
  return map;
}

// The one command this view's webview may invoke through a plain
// `command:` URI. Webview panels pass
// `enableCommandUris: [openResourceLocationCommand]` (never `true`) so no
// other command can ever be triggered from this view's static HTML, and
// `enableScripts` stays `false` throughout -- no script execution is
// needed at all for navigation.
export const openResourceLocationCommand = "hlsl.resourceBindings.openLocation";

function resourceLocationCommandUri(
  location: CompilationResourceSourceLocation,
): string {
  const args = encodeURIComponent(JSON.stringify([location]));
  return `command:${openResourceLocationCommand}?${args}`;
}

// Renders `name` as a plain escaped label, or -- only when `location` is a
// compiler-supplied, unambiguous declaration site for this exact resource
// entry -- as a link that invokes `openResourceLocationCommand` through a
// `command:` URI. This is never guessed from `name`: `location` must come
// from the resource's own `sourceLocation` field (or, for a collision
// participant, from the matching entry found via resourceLookup above).
function resourceNameLabel(
  name: string,
  location: CompilationResourceSourceLocation | null,
): string {
  const label = escapeHtml(name);
  if (location === null) {
    return label;
  }
  return `<a href="${escapeHtml(resourceLocationCommandUri(location))}" title="Go to declaration">${label}</a>`;
}

function rangeText(range: ResourceBindingRange): string {
  return range.unbounded
    ? `${String(range.baseRegister)} and above (unbounded)`
    : range.endRegister === range.baseRegister
      ? String(range.baseRegister)
      : `${String(range.baseRegister)}-${String(range.endRegister)}`;
}

function arrayText(resource: CompilationResourceBinding | undefined): string {
  if (resource === undefined) {
    return "-";
  }
  if (resource.unbounded) {
    return "unbounded";
  }
  return resource.bindCount === 1
    ? "scalar"
    : `[${String(resource.bindCount)}]`;
}

// Resource types whose reflected `sampleCount` field is repurposed by the
// compiler to report the structured-buffer element byte stride rather than
// a real multisample count. Mirrors dxc::resource_type_name's exact server
// strings for D3D_SIT_STRUCTURED and every UAV structured-buffer variant
// (RWStructuredBuffer, RWStructuredBuffer with an implicit hidden counter,
// AppendStructuredBuffer, ConsumeStructuredBuffer): all of these declare a
// struct stride, so all of them reuse NumSamples the same way.
const structuredBufferTypes: ReadonlySet<string> = new Set([
  "structured_buffer",
  "uav_rwstructured",
  "uav_rwstructured_with_counter",
  "uav_append_structured",
  "uav_consume_structured",
]);

function sampleCountText(
  resource: CompilationResourceBinding | undefined,
): string {
  if (resource === undefined) {
    return "-";
  }
  if (structuredBufferTypes.has(resource.type)) {
    return `stride ${String(resource.sampleCount)} bytes`;
  }
  return resource.sampleCount === 0xffffffff
    ? "n/a"
    : String(resource.sampleCount);
}

function orDash(value: string | undefined): string {
  return value === undefined || value === "" ? "-" : value;
}

function resourcesByGroupTable(
  group: ResourceBindingGroup,
  lookup: Map<string, CompilationResourceBinding>,
): string {
  const rows = group.ranges
    .map((range) => {
      const resource = lookup.get(
        resourceKey(group.registerClass, group.space, range.resourceName),
      );
      return `<tr>
<td>${resourceNameLabel(range.resourceName, resource?.sourceLocation ?? null)}</td>
<td>${escapeHtml(rangeText(range))}</td>
<td>${escapeHtml(arrayText(resource))}</td>
<td>${escapeHtml(resource?.type ?? "-")}</td>
<td>${escapeHtml(orDash(resource?.dimension))}</td>
<td>${escapeHtml(orDash(resource?.returnType))}</td>
<td>${escapeHtml(sampleCountText(resource))}</td>
<td>${escapeHtml(resource?.usage ?? "unknown")}</td>
</tr>`;
    })
    .join("");
  return `<table>
<thead><tr><th>Name</th><th>Register range</th><th>Array</th><th>Type</th><th>Dimension</th><th>Return type</th><th>Sample count / stride</th><th>Usage</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

function groupsSection(info: CompilationInfo): string {
  const reflection = info.reflection;
  if (reflection === null) {
    return `<section>
<h2>Resources</h2>
<p class="muted">Resource bindings are not available because no compiled output was produced.</p>
</section>`;
  }
  if (!reflection.available) {
    return `<section>
<h2>Resources</h2>
<p class="unavailable">Resource bindings are unavailable: ${escapeHtml(reflection.unavailableReason || "unknown reason")}</p>
</section>`;
  }
  const groups = sortedGroups(reflection.bindingAnalysis.groups);
  if (groups.length === 0) {
    return `<section>
<h2>Resources</h2>
<p class="muted">The compiled shader has no bound resources.</p>
</section>`;
  }
  const lookup = resourceLookup(reflection.resources);
  const body = groups
    .map((group) => {
      const reserved = group.systemReservedSpace
        ? ` <span class="badge reserved">system-reserved</span>`
        : "";
      return `<h3>Space ${String(group.space)} &mdash; ${escapeHtml(registerClassLabels[group.registerClass])}${reserved}</h3>
${resourcesByGroupTable(group, lookup)}`;
    })
    .join("");
  return `<section>
<h2>Resources</h2>
<p class="muted">Grouped by register space, then register class. System-reserved spaces (0xfffffff0&ndash;0xffffffff) are compiler/driver-internal and are excluded from collision detection.</p>
${body}
</section>`;
}

function collisionText(
  collision: ResourceBindingCollision,
  lookup: Map<string, CompilationResourceBinding>,
): string {
  // Collision participants are resolved only through the exact resource
  // entries in this same response (matched by registerClass+space+name,
  // the same key groupsSection uses to join range data back to a
  // resource) -- never by searching source text for the name.
  const first = lookup.get(
    resourceKey(
      collision.registerClass,
      collision.space,
      collision.firstResource,
    ),
  );
  const second = lookup.get(
    resourceKey(
      collision.registerClass,
      collision.space,
      collision.secondResource,
    ),
  );
  const firstLabel = resourceNameLabel(
    collision.firstResource,
    first?.sourceLocation ?? null,
  );
  const secondLabel = resourceNameLabel(
    collision.secondResource,
    second?.sourceLocation ?? null,
  );
  return `<li><strong>${escapeHtml(registerClassLabels[collision.registerClass])}</strong> space ${String(collision.space)}: ${escapeHtml(collision.message)} <span class="muted">(${firstLabel} &harr; ${secondLabel})</span></li>`;
}

function collisionsSection(info: CompilationInfo): string {
  const reflection = info.reflection;
  if (!reflection?.available) {
    return "";
  }
  const collisions = reflection.bindingAnalysis.collisions;
  const lookup = resourceLookup(reflection.resources);
  return `<section>
<h2>Collisions</h2>
${
  collisions.length === 0
    ? `<p class="muted">No provable register-range collisions were found between distinct resources.</p>`
    : `<ul>${collisions.map((collision) => collisionText(collision, lookup)).join("")}</ul>`
}
</section>`;
}

function rootSignatureRangeType(
  type: RootSignatureDescriptorRange["type"],
): string {
  return type.toUpperCase();
}

function descriptorRangeRow(range: RootSignatureDescriptorRange): string {
  const count = range.unbounded ? "unbounded" : String(range.numDescriptors);
  return `<tr>
<td>${escapeHtml(rootSignatureRangeType(range.type))}</td>
<td>${String(range.baseRegister)}</td>
<td>${String(range.space)}</td>
<td>${escapeHtml(count)}</td>
<td>${String(range.offsetInDescriptorsFromTableStart)}</td>
<td>0x${range.rawFlags.toString(16)}</td>
</tr>`;
}

function rootConstantsText(constants: RootSignatureRootConstants): string {
  return `root constants: register b${String(constants.shaderRegister)}, space ${String(constants.space)}, ${String(constants.num32BitValues)} x 32-bit values`;
}

function rootDescriptorText(descriptor: RootSignatureRootDescriptor): string {
  return `root descriptor: ${escapeHtml(rootSignatureRangeType(descriptor.type))} register ${String(descriptor.shaderRegister)}, space ${String(descriptor.space)}, flags 0x${descriptor.rawFlags.toString(16)}`;
}

function parameterSection(
  parameter: RootSignatureParameter,
  index: number,
): string {
  const header = `<h4>Parameter ${String(index)} &mdash; ${escapeHtml(parameter.kind)} (visibility: ${escapeHtml(parameter.visibility)})</h4>`;
  if (parameter.kind === "descriptorTable") {
    return `${header}
<table>
<thead><tr><th>Type</th><th>Base register</th><th>Space</th><th>Count</th><th>Offset</th><th>Flags</th></tr></thead>
<tbody>${parameter.descriptorTableRanges.map(descriptorRangeRow).join("")}</tbody>
</table>`;
  }
  if (parameter.kind === "constants" && parameter.constants !== null) {
    return `${header}<p>${rootConstantsText(parameter.constants)}</p>`;
  }
  if (
    parameter.kind === "rootDescriptor" &&
    parameter.rootDescriptor !== null
  ) {
    return `${header}<p>${rootDescriptorText(parameter.rootDescriptor)}</p>`;
  }
  return header;
}

function staticSamplerRow(sampler: RootSignatureStaticSampler): string {
  return `<tr>
<td>${String(sampler.shaderRegister)}</td>
<td>${String(sampler.space)}</td>
<td>${escapeHtml(sampler.visibility)}</td>
<td>0x${sampler.filter.toString(16)}</td>
<td>${String(sampler.addressU)}/${String(sampler.addressV)}/${String(sampler.addressW)}</td>
<td>${String(sampler.minLod)}&ndash;${String(sampler.maxLod)}</td>
</tr>`;
}

function rootSignatureDetailsHtml(details: RootSignatureDetails): string {
  const parameters =
    details.parameters.length === 0
      ? `<p class="muted">(no root parameters)</p>`
      : details.parameters.map(parameterSection).join("");
  const staticSamplers =
    details.staticSamplers.length === 0
      ? `<p class="muted">(no static samplers)</p>`
      : `<table>
<thead><tr><th>Register</th><th>Space</th><th>Visibility</th><th>Filter</th><th>Address U/V/W</th><th>LOD range</th></tr></thead>
<tbody>${details.staticSamplers.map(staticSamplerRow).join("")}</tbody>
</table>`;
  return `<table>
<tr><th>Version</th><td>${escapeHtml(details.version)}</td></tr>
<tr><th>Flags</th><td>0x${details.rawFlags.toString(16)}</td></tr>
<tr><th>CBV/SRV/UAV heap directly indexed</th><td>${details.cbvSrvUavHeapDirectlyIndexed ? "yes" : "no"}</td></tr>
<tr><th>Sampler heap directly indexed</th><td>${details.samplerHeapDirectlyIndexed ? "yes" : "no"}</td></tr>
</table>
<h3>Root parameters</h3>
${parameters}
<h3>Static samplers</h3>
${staticSamplers}`;
}

function rootSignatureSection(rootSignature: RootSignatureInfo | null): string {
  if (rootSignature === null) {
    return `<section>
<h2>Root signature</h2>
<p class="muted">Root-signature information is not available because compilation did not produce any output (for example, a failed compilation).</p>
</section>`;
  }
  switch (rootSignature.availability) {
    case "absent":
      return `<section>
<h2>Root signature</h2>
<p class="muted">No embedded root signature was found in this compiled output (for example, no <code>[RootSignature(...)]</code> attribute or matching compiler argument).</p>
</section>`;
    case "notApplicable":
      return `<section>
<h2>Root signature</h2>
<p class="muted">${escapeHtml(rootSignature.unavailableReason || "Root signatures do not apply to this compilation target.")}</p>
</section>`;
    case "presentDetailsUnavailable":
      return `<section>
<h2>Root signature</h2>
<p class="unavailable">An embedded root signature is present, but its details could not be retrieved on this platform: ${escapeHtml(rootSignature.unavailableReason || "unknown reason")}</p>
</section>`;
    case "present":
      return `<section>
<h2>Root signature</h2>
${rootSignature.details === null ? `<p class="unavailable">An embedded root signature is reported present, but no details were captured.</p>` : rootSignatureDetailsHtml(rootSignature.details)}
</section>`;
  }
}

function compatibilityIssueRow(issue: ResourceCompatibilityIssue): string {
  return `<li><strong>${escapeHtml(issue.resourceName)}</strong> (${escapeHtml(registerClassLabels[issue.registerClass])}, space ${String(issue.space)}): ${escapeHtml(issue.message)}</li>`;
}

function compatibilitySection(
  compatibility: CompilationCompatibility | null,
): string {
  const bindlessNote = `<p class="muted">Bindless accesses through <code>ResourceDescriptorHeap</code>/<code>SamplerDescriptorHeap</code> are invisible to compiler reflection and can never be enumerated or checked here; this analysis only covers resources DXC reflected as explicit bound-resource declarations.</p>`;
  if (compatibility === null) {
    return `<section>
<h2>Compatibility</h2>
<p class="muted">Compatibility information is not available because compilation did not produce any output (for example, a failed compilation).</p>
${bindlessNote}
</section>`;
  }
  if (compatibility.status === "unknown") {
    return `<section>
<h2>Compatibility</h2>
<p class="unavailable">Compatibility is unknown: ${escapeHtml(compatibility.explanation || "unknown reason")}. Unavailable root-signature details are never treated as compatible.</p>
${bindlessNote}
</section>`;
  }
  if (compatibility.status === "incompatible") {
    return `<section>
<h2 class="status-failure">Incompatible</h2>
<ul>${compatibility.issues.map(compatibilityIssueRow).join("")}</ul>
${bindlessNote}
</section>`;
  }
  return `<section>
<h2 class="status-success">Compatible</h2>
<p>All reflected, user-addressable resources are covered by the embedded root signature for the compiled entry point's stage.</p>
${bindlessNote}
</section>`;
}

function headerSection(info: CompilationInfo): string {
  const rows: [string, string][] = [
    ["Entry point", info.entryPoint || "(none)"],
    ["Stage", info.stage || "(unknown)"],
    ["Target profile", info.targetProfile || "(none)"],
    ["Active variant", info.activeVariant ?? "(none)"],
  ];
  const table = `<table>${rows
    .map(
      ([label, value]) =>
        `<tr><th>${escapeHtml(label)}</th><td>${escapeHtml(value)}</td></tr>`,
    )
    .join("")}</table>`;
  const status = info.success
    ? ""
    : `<p class="unavailable">Compilation failed; resource bindings reflect the last successfully compiled output, if any. See Shader Compilation for diagnostics.</p>`;
  return `<section>
${table}
${status}
<p class="muted">Resource names and collision participants are clickable only when DXC's reflection supplies an unambiguous declaration location for them; a resource whose name cannot be found, or that matches more than one declaration, renders as plain text instead of a guessed link. Embedded root-signature entries have no declaration location in this protocol, so they are never clickable.</p>
</section>`;
}

export function resourceBindingsHtml(info: CompilationInfo): string {
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
  h4 { font-size: .85rem; margin: .75rem 0 .25rem; color: var(--vscode-descriptionForeground); }
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
  .badge { font-size: .75rem; border-radius: .75rem; padding: .05rem .5rem; border: 1px solid var(--vscode-panel-border); }
  .badge.reserved { color: var(--vscode-editorWarning-foreground); }
</style>
</head>
<body>
<h1>Resource Bindings: ${escapeHtml(title)}</h1>
${headerSection(info)}
${groupsSection(info)}
${collisionsSection(info)}
${rootSignatureSection(info.rootSignature)}
${compatibilitySection(info.compatibility)}
</body>
</html>`;
}

// Rendered only when a request fails or is cancelled and no prior
// successful result exists to keep showing instead, mirroring
// compilationInfoErrorHtml.
export function resourceBindingsErrorHtml(message: string): string {
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
<h1>HLSL Resource Bindings</h1>
<p class="unavailable">${escapeHtml(message)}</p>
</body>
</html>`;
}

export interface ResourceBindingsRefreshOutcome {
  // undefined means "leave the webview's currently displayed HTML alone",
  // used to keep the last successful content on screen through a failed or
  // cancelled refresh instead of regressing to a placeholder or an
  // out-of-date loading message.
  readonly html: string | undefined;
  readonly hasContent: boolean;
  readonly title: string | undefined;
}

// Pure decision logic for how a Resource Bindings panel should react to one
// hlsl/compilationInfo attempt, mirroring resolveCompilationInfoRefresh so
// both panels share the same never-regress-on-failure behavior while
// remaining independently tracked panels/state.
export function resolveResourceBindingsRefresh(
  hasContent: boolean,
  info: CompilationInfo | null | undefined,
  failureMessage: string | undefined,
): ResourceBindingsRefreshOutcome {
  if (info === null || info === undefined) {
    if (hasContent) {
      return { html: undefined, hasContent: true, title: undefined };
    }
    return {
      html: resourceBindingsErrorHtml(
        failureMessage ??
          "The HLSL language server is not currently available.",
      ),
      hasContent: false,
      title: undefined,
    };
  }
  return {
    html: resourceBindingsHtml(info),
    hasContent: true,
    title: `Resource Bindings: ${info.entryPoint || "(default entry point)"}`,
  };
}

// The validated shape a `openResourceLocationCommand` invocation must have
// before any `vscode.Uri`/document API touches it.
export interface ValidatedResourceLocation {
  readonly uri: string;
  readonly range: CompilationSourceRange;
}

function isNonNegativeInteger(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

function parseSourcePosition(
  value: unknown,
): CompilationSourcePosition | undefined {
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

function parseSourceRange(value: unknown): CompilationSourceRange | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  const start = parseSourcePosition(candidate.start);
  const end = parseSourcePosition(candidate.end);
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
// `openResourceLocationCommand` command-message boundary before any
// `vscode.Uri.parse`/`openTextDocument`/`showTextDocument` call touches it.
// Returns `undefined` for anything that does not exactly match the
// expected `{uri, range}` shape (including a missing/empty uri, a
// malformed range, or an inverted range) -- the command handler must treat
// that as a validation failure rather than falling back to a guess.
export function parseResourceLocationCommandArg(
  value: unknown,
): ValidatedResourceLocation | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  if (typeof candidate.uri !== "string" || candidate.uri.length === 0) {
    return undefined;
  }
  const range = parseSourceRange(candidate.range);
  if (range === undefined) {
    return undefined;
  }
  return { uri: candidate.uri, range };
}
