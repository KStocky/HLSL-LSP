export interface CaptureStatus {
  readonly protocolVersion: 1;
  readonly sessionId: string | null;
  readonly active: boolean;
  readonly accepted: number;
  readonly rejected: number;
  readonly overflow: number;
}

export interface CaptureInvocation {
  readonly source: string;
  readonly entryPoint: string;
  readonly targetProfile: string;
  readonly languageVersion?: string;
  readonly defines?: readonly (readonly [string, string])[];
  readonly includeDirectories?: readonly string[];
  readonly virtualMappings?: readonly (readonly [string, string])[];
  readonly arguments?: readonly string[];
  readonly outputMode?: string;
  readonly pipeline?: string;
  readonly stage?: string;
}

export interface CaptureEntry {
  readonly id: string;
  readonly invocation: CaptureInvocation;
  readonly count: number;
  readonly review: {
    readonly eligible: boolean;
    readonly requiresConfirmation: boolean;
    readonly warningCodes: readonly string[];
    readonly warnings: readonly string[];
    readonly selection: {
      readonly relativePath: string;
      readonly entryPoint: string;
      readonly targetProfile: string;
    } | null;
    readonly fileGroup: Record<string, unknown> | null;
  };
}

export interface CaptureSnapshot extends CaptureStatus {
  readonly entries: readonly CaptureEntry[];
}

export interface CaptureStart {
  readonly protocolVersion: 1;
  readonly sessionId: string;
  readonly endpoint: string;
  readonly token: string;
}

export interface CaptureVariant {
  readonly entryId: string;
  readonly name: string;
}

export type CaptureStage = "vertex" | "pixel" | "geometry" | "hull" | "domain";

export interface CapturePipeline {
  readonly name: string;
  readonly source: "host" | "user";
  readonly stages: Readonly<Partial<Record<CaptureStage, string>>>;
}

export interface CapturePreview {
  readonly protocolVersion: 1;
  readonly sessionId: string;
  readonly selectedEntryIds: readonly string[];
  readonly configuration: {
    readonly uri: string;
    readonly exists: boolean;
    readonly expectedContentVersion: number | null;
    readonly expectedContentHash: string | null;
  };
  readonly preview: {
    readonly content: string;
    readonly contentHash: string;
    readonly valid: boolean;
    readonly changed: boolean;
    readonly errors: readonly {
      readonly code: string;
      readonly field: string;
      readonly message: string;
    }[];
  };
}

export function capturePreviewMatches(
  snapshot: CaptureSnapshot,
  entries: readonly CaptureEntry[],
  configurationUri: string,
  result: CapturePreview,
): boolean {
  const selected = new Set(entries.map((entry) => entry.id));
  return (
    snapshot.active &&
    snapshot.sessionId !== null &&
    result.sessionId === snapshot.sessionId &&
    result.configuration.uri === configurationUri &&
    selected.size === entries.length &&
    result.selectedEntryIds.length === entries.length &&
    result.selectedEntryIds.every((id) => selected.has(id))
  );
}

export function captureSessionMatches(
  sessionId: string,
  status: Pick<CaptureStatus, "active" | "sessionId"> | null | undefined,
): boolean {
  return status?.active === true && status.sessionId === sessionId;
}

export function captureVariantRequired(
  entries: readonly CaptureEntry[],
): ReadonlySet<string> {
  const paths = new Map<string, string[]>();
  for (const entry of entries) {
    const path = entry.review.selection?.relativePath;
    if (path === undefined) {
      continue;
    }
    const key = process.platform === "win32" ? path.toLowerCase() : path;
    const ids = paths.get(key) ?? [];
    ids.push(entry.id);
    paths.set(key, ids);
  }
  return new Set([...paths.values()].filter((ids) => ids.length > 1).flat());
}

export function captureStage(entry: CaptureEntry): CaptureStage | undefined {
  const prefix = entry.invocation.targetProfile.slice(0, 3);
  const stage = {
    vs_: "vertex",
    ps_: "pixel",
    gs_: "geometry",
    hs_: "hull",
    ds_: "domain",
  } as const;
  return stage[prefix as keyof typeof stage];
}

export function hostPipelineCandidates(
  entries: readonly CaptureEntry[],
): readonly CapturePipeline[] {
  const groups = new Map<string, Partial<Record<CaptureStage, string>>>();
  const ambiguous = new Set<string>();
  for (const entry of entries) {
    const name = entry.invocation.pipeline;
    const stage = captureStage(entry);
    if (!name || !stage || entry.invocation.stage !== stage) {
      continue;
    }
    const stages = groups.get(name) ?? {};
    if (stages[stage] !== undefined) {
      ambiguous.add(name);
    }
    stages[stage] = entry.id;
    groups.set(name, stages);
  }
  return [...groups]
    .filter(
      ([name, stages]) =>
        !ambiguous.has(name) &&
        stages.vertex !== undefined &&
        stages.pixel !== undefined &&
        (stages.hull === undefined) === (stages.domain === undefined),
    )
    .map(([name, stages]) => ({ name, source: "host", stages }));
}

export function captureEntryIsTransient(entry: CaptureEntry): boolean {
  const invocation = entry.invocation;
  const values = [
    invocation.source,
    invocation.pipeline ?? "",
    ...(invocation.arguments ?? []),
    ...(invocation.includeDirectories ?? []),
    ...(invocation.defines ?? []).flat(),
    ...(invocation.virtualMappings ?? []).flat(),
  ];
  return (
    !entry.review.eligible ||
    entry.review.warningCodes.some((code) =>
      [
        "logicalIdentity",
        "networkPath",
        "outsideWorkspace",
        "missingFile",
        "resolvedOutsideWorkspace",
        "conflictingKeys",
        "nestedConfiguration",
      ].includes(code),
    ) ||
    values.some((value) =>
      /(?:\$\{|%[a-z_][\w]*%|<[^>]+>|(?:^|[\\/])(?:temp|tmp|cache|generated|intermediate)(?:[\\/]|$)|(?:^|[\\/])(?:build|out)[\\/])/i.test(
        value,
      ),
    )
  );
}

export function captureSummary(snapshot: CaptureSnapshot): string {
  return `${snapshot.active ? "Active" : "Stopped"} · ${String(snapshot.accepted)} accepted · ${String(snapshot.rejected)} rejected · ${String(snapshot.overflow)} overflow · ${String(snapshot.entries.length)} distinct (maximum 256)`;
}
