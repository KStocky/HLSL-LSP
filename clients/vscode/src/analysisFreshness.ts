export type AnalysisFreshnessStatus =
  "Current" | "Refreshing" | "Stale" | "Refresh failed";

export type AnalysisFreshnessCause =
  | "Source edit"
  | "Variant change"
  | "Configuration change"
  | "Disconnected server"
  | "Manual refresh"
  | "Unknown";

export interface AnalysisFreshnessState {
  readonly status: AnalysisFreshnessStatus;
  readonly cause: AnalysisFreshnessCause;
}

const compilationAffectingSettings = [
  "hlsl.entryPoint",
  "hlsl.targetProfile",
  "hlsl.preprocessorDefinitions",
  "hlsl.additionalArguments",
  "hlsl.languageVersion",
  "hlsl.activeVariant",
] as const;

const restartAffectingSettings = [
  "hlsl.dxcRuntimeDirectory",
  "hlsl.server.path",
  "hlsl.additionalIncludeDirectories",
  "hlsl.virtualDirectoryMappings",
] as const;

export interface AnalysisConfigurationChange {
  readonly action: "refresh" | "restart";
  readonly cause: AnalysisFreshnessCause;
}

export function analysisConfigurationChange(
  affectsConfiguration: (setting: string) => boolean,
): AnalysisConfigurationChange | undefined {
  if (restartAffectingSettings.some(affectsConfiguration)) {
    return { action: "restart", cause: "Configuration change" };
  }
  if (affectsConfiguration("hlsl.activeVariant")) {
    return { action: "refresh", cause: "Variant change" };
  }
  if (compilationAffectingSettings.some(affectsConfiguration)) {
    return { action: "refresh", cause: "Configuration change" };
  }
  return undefined;
}

export type AnalysisFreshnessEvent =
  | {
      readonly type: "refresh-started";
      readonly cause: AnalysisFreshnessCause;
    }
  | { readonly type: "refresh-succeeded" }
  | {
      readonly type: "refresh-failed";
      readonly cause?: AnalysisFreshnessCause;
    }
  | {
      readonly type: "invalidated";
      readonly cause: AnalysisFreshnessCause;
      readonly refreshPending: boolean;
    };

export const initialAnalysisFreshness: AnalysisFreshnessState = {
  status: "Stale",
  cause: "Unknown",
};

export function reduceAnalysisFreshness(
  state: AnalysisFreshnessState,
  event: AnalysisFreshnessEvent,
): AnalysisFreshnessState {
  switch (event.type) {
    case "refresh-started":
      return { status: "Refreshing", cause: event.cause };
    case "refresh-succeeded":
      return { status: "Current", cause: "Unknown" };
    case "refresh-failed":
      return {
        status: "Refresh failed",
        cause: event.cause ?? state.cause,
      };
    case "invalidated":
      return {
        status:
          event.refreshPending && state.status === "Refreshing"
            ? "Refreshing"
            : "Stale",
        cause: event.cause,
      };
  }
}

export class AnalysisFreshness {
  private stateValue = initialAnalysisFreshness;
  private generation = 0;

  public get state(): AnalysisFreshnessState {
    return this.stateValue;
  }

  public beginRefresh(cause: AnalysisFreshnessCause): number {
    const generation = ++this.generation;
    this.stateValue = reduceAnalysisFreshness(this.stateValue, {
      type: "refresh-started",
      cause,
    });
    return generation;
  }

  public invalidate(
    cause: AnalysisFreshnessCause,
    refreshPending = false,
  ): void {
    ++this.generation;
    this.stateValue = reduceAnalysisFreshness(this.stateValue, {
      type: "invalidated",
      cause,
      refreshPending,
    });
  }

  public succeed(generation: number): boolean {
    if (generation !== this.generation) {
      return false;
    }
    this.stateValue = reduceAnalysisFreshness(this.stateValue, {
      type: "refresh-succeeded",
    });
    return true;
  }

  public fail(generation: number, cause?: AnalysisFreshnessCause): boolean {
    if (generation !== this.generation) {
      return false;
    }
    this.stateValue = reduceAnalysisFreshness(this.stateValue, {
      type: "refresh-failed",
      ...(cause === undefined ? {} : { cause }),
    });
    return true;
  }
}

const freshnessStart = "<!-- hlsl-analysis-freshness:start -->";
const freshnessEnd = "<!-- hlsl-analysis-freshness:end -->";

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

export function analysisFreshnessText(state: AnalysisFreshnessState): string {
  return state.cause === "Unknown" || state.status === "Current"
    ? state.status
    : `${state.status} · ${state.cause}`;
}

export function withAnalysisFreshness(
  html: string,
  state: AnalysisFreshnessState,
  refreshCommand: string,
): string {
  const start = html.indexOf(freshnessStart);
  const end = html.indexOf(freshnessEnd);
  const clean =
    start >= 0 && end >= start
      ? html.slice(0, start) + html.slice(end + freshnessEnd.length)
      : html;
  const style = `<style>
.hlsl-analysis-freshness { display:flex; align-items:center; gap:.6rem; margin:0 0 .8rem; color:var(--vscode-descriptionForeground); font-size:.9em; }
.hlsl-analysis-freshness .state { white-space:nowrap; }
.hlsl-analysis-freshness a { color:var(--vscode-textLink-foreground); }
</style>`;
  const indicator = `${freshnessStart}${style}<div class="hlsl-analysis-freshness" role="status"><span class="state">${escapeHtml(analysisFreshnessText(state))}</span><span aria-hidden="true">·</span><a href="command:${encodeURIComponent(refreshCommand)}">Refresh</a></div>${freshnessEnd}`;
  return clean.replace(/<body([^>]*)>/i, `<body$1>${indicator}`);
}
