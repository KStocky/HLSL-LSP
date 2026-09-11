import { escapeHtml } from "./compilationInfo";
import {
  EffectiveShaderContext,
  effectiveContextHeaderHtml,
} from "./effectiveContext";
import { RefreshOutcome } from "./panelController";

export interface ComputeDimensions {
  readonly x: number;
  readonly y: number;
  readonly z: number;
}

export interface ComputeHardwareProfile {
  readonly name: string;
  readonly waveSize: number;
  readonly maxThreadsPerGroup: number;
  readonly maxThreadsPerComputeUnit: number;
  readonly maxGroupsPerComputeUnit: number;
  readonly sharedMemoryBytesPerComputeUnit: number;
}

export interface ComputeSourcePosition {
  readonly line: number;
  readonly character: number;
}

export interface ComputeSourceRange {
  readonly start: ComputeSourcePosition;
  readonly end: ComputeSourcePosition;
}

export interface ComputeSourceLocation {
  readonly uri: string;
  readonly range: ComputeSourceRange;
}

export interface ComputeSystemValue {
  readonly semantic: string;
  readonly name: string;
  readonly formula: string;
  readonly description: string;
}

export interface ComputeBarrierLocation extends ComputeSourceLocation {
  readonly label?: string;
}

export interface ComputeBarrierAnalysis {
  readonly available: boolean;
  readonly unavailableReason: string;
  readonly instructionCount: number | null;
  readonly locationsAvailable: boolean;
  readonly locationsUnavailableReason: string;
  readonly locationsTruncated: boolean;
  readonly locations: readonly ComputeBarrierLocation[];
}

export interface ComputeGroupSharedDeclaration {
  readonly name: string;
  readonly type: string;
  readonly declaration: string;
  readonly bytes: number | null;
  readonly sizeUnavailableReason: string;
  readonly uri?: string;
  readonly range?: ComputeSourceRange;
}

export interface ComputeGroupSharedAnalysis {
  readonly available: boolean;
  readonly unavailableReason: string;
  readonly totalBytes: number | null;
  readonly totalBytesUnavailableReason: string;
  readonly truncated: boolean;
  readonly declarations: readonly ComputeGroupSharedDeclaration[];
}

export interface ComputeWaveSize {
  readonly known: boolean;
  readonly min: number | null;
  readonly max: number | null;
  readonly preferred: number | null;
  readonly minMaxSource: string | null;
  readonly preferredSource: string | null;
  readonly explanation: string;
}

export interface ComputeOccupancy {
  readonly hardwareProfile: string;
  readonly estimatedResidentGroups: number;
  readonly estimatedResidentThreads: number;
  readonly estimatedResidentWaves: number;
  readonly limitingFactors: readonly string[];
  readonly assumptions: readonly string[];
}

export interface ComputeVisualization {
  readonly context?: EffectiveShaderContext;
  readonly applicable: boolean;
  readonly explanation: string;
  readonly entryPoint: string;
  readonly stage: string;
  readonly targetProfile: string;
  readonly threadGroupSize: ComputeDimensions | null;
  readonly dispatchDimensions: ComputeDimensions | null;
  readonly groupCount: ComputeDimensions | null;
  readonly launchedThreads: number | null;
  readonly inactiveThreads: number | null;
  readonly systemValues: readonly ComputeSystemValue[];
  readonly barriers: ComputeBarrierAnalysis;
  readonly groupShared: ComputeGroupSharedAnalysis;
  readonly waveSize: ComputeWaveSize;
  readonly occupancy: ComputeOccupancy | null;
}

export interface ComputeVisualizationOptions {
  readonly dispatchDimensions?: ComputeDimensions;
  readonly hardwareProfile?: ComputeHardwareProfile;
}

export interface ComputeVisualizationRequestParams {
  readonly textDocument: { readonly uri: string };
  readonly dispatchDimensions?: ComputeDimensions;
  readonly hardwareProfile?: ComputeHardwareProfile;
}

export const configureComputeVisualizationCommand =
  "hlsl.computeVisualization.configure";
export const openComputeVisualizationLocationCommand =
  "hlsl.computeVisualization.openLocation";

export function parseComputeVisualizationLocation(
  value: unknown,
): ComputeSourceLocation | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  const range = candidate.range;
  if (
    typeof candidate.uri !== "string" ||
    candidate.uri.length === 0 ||
    typeof range !== "object" ||
    range === null
  ) {
    return undefined;
  }
  const validPosition = (
    position: unknown,
  ): position is ComputeSourcePosition =>
    typeof position === "object" &&
    position !== null &&
    Number.isSafeInteger((position as ComputeSourcePosition).line) &&
    (position as ComputeSourcePosition).line >= 0 &&
    Number.isSafeInteger((position as ComputeSourcePosition).character) &&
    (position as ComputeSourcePosition).character >= 0;
  const rangeCandidate = range as Record<string, unknown>;
  if (
    !validPosition(rangeCandidate.start) ||
    !validPosition(rangeCandidate.end)
  ) {
    return undefined;
  }
  const start = rangeCandidate.start;
  const end = rangeCandidate.end;
  if (
    end.line < start.line ||
    (end.line === start.line && end.character < start.character)
  ) {
    return undefined;
  }
  return { uri: candidate.uri, range: { start, end } };
}

export function computeVisualizationRequestParams(
  uri: string,
  options: ComputeVisualizationOptions = {},
): ComputeVisualizationRequestParams {
  return {
    textDocument: { uri },
    ...(options.dispatchDimensions === undefined
      ? {}
      : { dispatchDimensions: options.dispatchDimensions }),
    ...(options.hardwareProfile === undefined
      ? {}
      : { hardwareProfile: options.hardwareProfile }),
  };
}

export function parsePositiveDimension(value: string): number | undefined {
  if (!/^[1-9][0-9]*$/.test(value)) {
    return undefined;
  }
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed <= 0xffffffff
    ? parsed
    : undefined;
}

function dimensions(value: ComputeDimensions | null): string {
  return value === null
    ? "Unavailable"
    : `${String(value.x)} x ${String(value.y)} x ${String(value.z)}`;
}

function numberOrUnavailable(value: number | null): string {
  return value === null ? "Unavailable" : value.toLocaleString("en-US");
}

function commandUri(command: string, argument: unknown): string {
  return `command:${command}?${encodeURIComponent(JSON.stringify([argument]))}`;
}

function locationLink(location: ComputeSourceLocation, label: string): string {
  return `<a href="${escapeHtml(commandUri(openComputeVisualizationLocationCommand, location))}">${escapeHtml(label)}</a>`;
}

function geometrySection(report: ComputeVisualization): string {
  return `<section>
<h2>Dispatch geometry</h2>
<table>
<tbody>
<tr><th>Threads per group</th><td>${dimensions(report.threadGroupSize)}</td></tr>
<tr><th>Logical workload (threads)</th><td>${dimensions(report.dispatchDimensions)}</td></tr>
<tr><th>Required Dispatch() groups</th><td>${dimensions(report.groupCount)}</td></tr>
<tr><th>Launched threads</th><td>${numberOrUnavailable(report.launchedThreads)}</td></tr>
<tr><th>Inactive edge threads</th><td>${numberOrUnavailable(report.inactiveThreads)}</td></tr>
</tbody>
</table>
</section>`;
}

function systemValuesSection(values: readonly ComputeSystemValue[]): string {
  const rows =
    values.length === 0
      ? '<tr><td colspan="3" class="muted">(no mappings reported)</td></tr>'
      : values
          .map(
            (value) =>
              `<tr><td><code>${escapeHtml(value.semantic)}</code></td><td><code>${escapeHtml(value.formula)}</code></td><td>${escapeHtml(value.description)}</td></tr>`,
          )
          .join("");
  return `<section>
<h2>System-value mapping</h2>
<table><thead><tr><th>Semantic</th><th>Mapping</th><th>Description</th></tr></thead><tbody>${rows}</tbody></table>
</section>`;
}

function barriersSection(barriers: ComputeBarrierAnalysis): string {
  if (!barriers.available) {
    return `<section><h2>Barriers</h2><p class="unavailable">${escapeHtml(barriers.unavailableReason || "Barrier analysis is unavailable.")}</p></section>`;
  }
  const locations =
    barriers.instructionCount === 0
      ? '<li class="muted">(no barrier instructions)</li>'
      : !barriers.locationsAvailable
        ? `<li class="muted">${escapeHtml(barriers.locationsUnavailableReason || "Barrier source locations are not available.")}</li>`
        : barriers.locations.length === 0
          ? '<li class="muted">(no barrier locations reported)</li>'
          : barriers.locations
              .map((location, index) => {
                const label = location.label ?? `Barrier ${String(index + 1)}`;
                return `<li>${locationLink(location, label)}</li>`;
              })
              .join("");
  return `<section>
<h2>Barriers</h2>
<p>Compiler instruction count: ${numberOrUnavailable(barriers.instructionCount)}</p>
${barriers.locationsTruncated ? '<p class="unavailable">Barrier locations were truncated by the server limit.</p>' : ""}
<ul>${locations}</ul>
</section>`;
}

function groupSharedSection(groupShared: ComputeGroupSharedAnalysis): string {
  if (!groupShared.available) {
    return `<section><h2>Group-shared memory</h2><p class="unavailable">${escapeHtml(groupShared.unavailableReason || "Group-shared memory analysis is unavailable.")}</p></section>`;
  }
  const rows =
    groupShared.declarations.length === 0
      ? '<tr><td colspan="3" class="muted">(no group-shared declarations)</td></tr>'
      : groupShared.declarations
          .map((declaration) => {
            const name =
              declaration.uri !== undefined && declaration.range !== undefined
                ? locationLink(
                    { uri: declaration.uri, range: declaration.range },
                    declaration.name,
                  )
                : escapeHtml(declaration.name);
            const bytes =
              declaration.bytes === null && declaration.sizeUnavailableReason
                ? escapeHtml(declaration.sizeUnavailableReason)
                : numberOrUnavailable(declaration.bytes);
            return `<tr><td>${name}</td><td><code>${escapeHtml(declaration.type)}</code><br><code>${escapeHtml(declaration.declaration)}</code></td><td>${bytes}</td></tr>`;
          })
          .join("");
  return `<section>
<h2>Group-shared memory</h2>
  <p>Total estimated bytes: ${
    groupShared.totalBytes === null && groupShared.totalBytesUnavailableReason
      ? escapeHtml(groupShared.totalBytesUnavailableReason)
      : numberOrUnavailable(groupShared.totalBytes)
  }</p>
  ${groupShared.truncated ? '<p class="unavailable">Group-shared declarations were truncated by the server limit.</p>' : ""}
  <table><thead><tr><th>Name</th><th>Type</th><th>Bytes</th></tr></thead><tbody>${rows}</tbody></table>
</section>`;
}

function waveSection(waveSize: ComputeWaveSize): string {
  if (!waveSize.known) {
    return `<section><h2>Wave size</h2><p class="unavailable">${escapeHtml(waveSize.explanation || "No compiler-authoritative wave-size requirement is available.")}</p></section>`;
  }
  return `<section>
<h2>Wave size</h2>
<table><tbody>
<tr><th>Minimum</th><td>${numberOrUnavailable(waveSize.min)}</td></tr>
<tr><th>Maximum</th><td>${numberOrUnavailable(waveSize.max)}</td></tr>
<tr><th>Preferred</th><td>${numberOrUnavailable(waveSize.preferred)}</td></tr>
<tr><th>Min/max source</th><td>${escapeHtml(waveSize.minMaxSource ?? "Unavailable")}</td></tr>
<tr><th>Preferred source</th><td>${escapeHtml(waveSize.preferredSource ?? "Unavailable")}</td></tr>
</tbody></table>
<p class="muted">${escapeHtml(waveSize.explanation)}</p>
</section>`;
}

function occupancySection(occupancy: ComputeOccupancy | null): string {
  if (occupancy === null) {
    return `<section>
<h2>Hardware-dependent occupancy</h2>
<p class="unavailable">No hardware profile was supplied. Occupancy is intentionally not guessed.</p>
</section>`;
  }
  const limitingFactors = occupancy.limitingFactors
    .map((factor) => `<li>${escapeHtml(factor)}</li>`)
    .join("");
  const assumptions = occupancy.assumptions
    .map((assumption) => `<li>${escapeHtml(assumption)}</li>`)
    .join("");
  return `<section>
<h2>Hardware-dependent occupancy</h2>
<p class="estimate">Estimate for ${escapeHtml(occupancy.hardwareProfile)}; this is not compiler- or device-runtime-measured occupancy.</p>
<table><tbody>
<tr><th>Resident groups / compute unit</th><td>${String(occupancy.estimatedResidentGroups)}</td></tr>
<tr><th>Resident threads / compute unit</th><td>${String(occupancy.estimatedResidentThreads)}</td></tr>
<tr><th>Resident waves / compute unit</th><td>${String(occupancy.estimatedResidentWaves)}</td></tr>
</tbody></table>
<h3>Limiting factors</h3><ul>${limitingFactors || '<li class="muted">(none reported)</li>'}</ul>
<h3>Assumptions</h3><ul>${assumptions || '<li class="muted">(none reported)</li>'}</ul>
</section>`;
}

export function computeVisualizationHtml(
  report: ComputeVisualization,
  documentUri: string,
): string {
  const label = escapeHtml(
    decodeURIComponent(
      documentUri.split(/[?#]/)[0]?.split("/").filter(Boolean).at(-1) ??
        documentUri,
    ),
  );
  const body = report.applicable
    ? `${geometrySection(report)}
${systemValuesSection(report.systemValues)}
${groupSharedSection(report.groupShared)}
${barriersSection(report.barriers)}
${waveSection(report.waveSize)}
${occupancySection(report.occupancy)}`
    : `<p class="unavailable">${escapeHtml(report.explanation || "Compute visualization is not available for this document.")}</p>`;
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
h1 { font-size: 1.35rem; margin: 0 0 .35rem; }
h2 { font-size: 1.05rem; margin: 1.5rem 0 .35rem; }
h3 { font-size: .95rem; margin: 1rem 0 .25rem; }
a { color: var(--vscode-textLink-foreground); }
table { border-collapse: collapse; width: 100%; max-width: 70rem; }
th, td { border-bottom: 1px solid var(--vscode-panel-border); padding: .4rem .5rem; text-align: left; vertical-align: top; }
th { color: var(--vscode-descriptionForeground); }
.muted { color: var(--vscode-descriptionForeground); }
.unavailable { color: var(--vscode-editorWarning-foreground); }
.estimate { border-left: 3px solid var(--vscode-editorWarning-foreground); padding-left: .75rem; }
</style>
</head>
<body>
<h1>Compute Visualization: ${label}</h1>
${effectiveContextHeaderHtml(report.context)}
<p><a href="command:${configureComputeVisualizationCommand}">Configure logical workload and hardware profile</a></p>
${body}
</body>
</html>`;
}

export function computeVisualizationErrorHtml(message: string): string {
  return `<!doctype html><html lang="en"><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><h1>Compute Visualization</h1><p style="color:var(--vscode-editorWarning-foreground)">${escapeHtml(message)}</p></body></html>`;
}

export function resolveComputeVisualizationRefresh(
  hasContent: boolean,
  documentUri: string,
  report: ComputeVisualization | null | undefined,
  failureMessage: string | undefined,
): RefreshOutcome {
  if (report !== null && report !== undefined) {
    return {
      html: computeVisualizationHtml(report, documentUri),
      hasContent: true,
      title: "Compute Visualization",
    };
  }
  if (hasContent) {
    return { html: undefined, hasContent: true, title: undefined };
  }
  return {
    html: computeVisualizationErrorHtml(
      failureMessage ?? "Compute visualization is unavailable.",
    ),
    hasContent: false,
    title: "Compute Visualization",
  };
}
