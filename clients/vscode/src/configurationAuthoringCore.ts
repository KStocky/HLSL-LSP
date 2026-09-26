import { createHash } from "node:crypto";

export interface ConfigurationSelection {
  readonly relativePath: string;
  readonly entryPoint: string;
  readonly targetProfile: string;
}

export interface ConfigurationAuthoringParams {
  readonly protocolVersion: 1;
  readonly workspaceFolder: { readonly uri: string };
  readonly existingConfiguration?: {
    readonly content: string;
    readonly version: number;
    readonly contentHash: string;
  };
  readonly selections?: readonly ConfigurationSelection[];
}

interface ConfigurationEntryPoint {
  readonly name: string;
  readonly location: { readonly line: number; readonly character: number };
  readonly targetProfiles: readonly string[];
  readonly state: "resolved" | "ambiguous" | "unresolved";
  readonly explanation: string;
}

interface ConfigurationDiscoveryFile {
  readonly uri: string;
  readonly relativePath: string;
  readonly entryPoints: readonly ConfigurationEntryPoint[];
  readonly truncated: boolean;
  readonly analysisError: string | null;
}

interface ConfigurationValidationError {
  readonly code: string;
  readonly field: string;
  readonly message: string;
}

export interface ConfigurationAuthoringResult {
  readonly protocolVersion: 1;
  readonly configuration: {
    readonly uri: string;
    readonly exists: boolean;
    readonly expectedContentVersion: number | null;
    readonly expectedContentHash: string | null;
  };
  readonly discovery: {
    readonly files: readonly ConfigurationDiscoveryFile[];
    readonly nestedConfigurations: readonly string[];
    readonly directoriesVisited: number;
    readonly compilerProbes: number;
    readonly truncated: boolean;
    readonly truncationReason: string | null;
  };
  readonly preview: {
    readonly content: string;
    readonly contentHash: string;
    readonly valid: boolean;
    readonly changed: boolean;
    readonly errors: readonly ConfigurationValidationError[];
  };
}

export interface ConfigurationCandidate {
  readonly label: string;
  readonly description: string;
  readonly detail: string;
  readonly selection: ConfigurationSelection;
  readonly picked: boolean;
}

export function configurationContentHash(content: string): string {
  return `sha256:${createHash("sha256").update(content, "utf8").digest("hex")}`;
}

export function configurationCandidates(
  result: ConfigurationAuthoringResult,
): readonly ConfigurationCandidate[] {
  return result.discovery.files.flatMap((file) =>
    file.entryPoints.flatMap((entryPoint) =>
      entryPoint.targetProfiles.map((targetProfile) => ({
        label: `$(symbol-method) ${entryPoint.name}`,
        description: `${targetProfile} — ${file.relativePath}`,
        detail:
          entryPoint.state === "ambiguous"
            ? `Choice required: ${entryPoint.explanation}`
            : entryPoint.explanation,
        selection: {
          relativePath: file.relativePath,
          entryPoint: entryPoint.name,
          targetProfile,
        },
        picked:
          entryPoint.state === "resolved" &&
          entryPoint.targetProfiles.length === 1,
      })),
    ),
  );
}

export function formatConfigurationErrors(
  errors: readonly ConfigurationValidationError[],
): string {
  return errors
    .map((error) => `${error.field}: ${error.message} [${error.code}]`)
    .join("\n");
}

export function configurationIsStale(
  expectedVersion: number | null,
  expectedHash: string | null,
  currentVersion: number,
  currentContent: string,
): boolean {
  return (
    (expectedVersion !== null && expectedVersion !== currentVersion) ||
    (expectedHash !== null &&
      expectedHash !== configurationContentHash(currentContent))
  );
}
