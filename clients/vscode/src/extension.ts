import * as vscode from "vscode";
import {
  Executable,
  LanguageClient,
  LanguageClientOptions,
  RevealOutputChannelOn,
  ServerOptions,
  State,
  Trace,
} from "vscode-languageclient/node";

import {
  CompilationInfo,
  copyDisassemblyCommand,
  disassemblyFileName,
  escapeHtml,
  resolveCompilationInfoRefresh,
  saveDisassemblyCommand,
} from "./compilationInfo";
import {
  openResourceLocationCommand,
  parseResourceLocationCommandArg,
  resolveResourceBindingsRefresh,
} from "./resourceBindings";
import {
  openPreprocessorLocationCommand,
  parsePreprocessorLocationCommandArg,
  PreprocessorExplorerReport,
  resolvePreprocessorExplorerRefresh,
} from "./preprocessorExplorer";
import {
  EntryPointDataFlow,
  entryPointDataFlowRequestParams,
  EntryPointDataFlowNavigator,
  navigateEntryPointDataFlowLocation,
  openEntryPointDataFlowLocationCommand,
  resolveEntryPointDataFlowRefresh,
} from "./entryPointDataFlow";
import {
  ComputeHardwareProfile,
  ComputeVisualization,
  ComputeVisualizationOptions,
  computeVisualizationRequestParams,
  configureComputeVisualizationCommand,
  openComputeVisualizationLocationCommand,
  parseComputeVisualizationLocation,
  parsePositiveDimension,
  resolveComputeVisualizationRefresh,
} from "./computeVisualization";
import { Debouncer } from "./debouncer";
import {
  EffectiveShaderContext,
  effectiveContextTooltip,
  variantLabel,
} from "./effectiveContext";
import { nodeScheduler, PanelController } from "./panelController";
import {
  AnalysisFreshness,
  AnalysisFreshnessCause,
  AnalysisFreshnessState,
  AnalysisTrackingMode,
  analysisConfigurationChange,
  parseAnalysisTrackingCommand,
  withAnalysisFreshness,
} from "./analysisFreshness";
import {
  analysisTargetTransition,
  defaultAnalysisTrackingMode,
  isReopenedAnalysisTarget,
} from "./analysisTracking";
import {
  HlslServerSettings,
  readActiveVariant,
  readDefaultLanguageVersion,
  readServerSettings,
  readTraceSetting,
  TraceSetting,
} from "./configuration";
import {
  ClientLifecycle,
  ConnectionRecoveryTracker,
  ConnectionTransition,
  LifecycleClient,
  LifecycleState,
  runGuardedRecovery,
} from "./lifecycle";
import {
  MemoryLayout,
  memoryLayoutHtml,
  updateMemoryLayoutForDocumentChange,
} from "./memoryLayout";
import {
  resolveDxcRuntimeDirectory,
  resolveServerRuntime,
  RuntimeEnvironment,
  ServerRuntime,
} from "./runtime";
import { RunningSettingsSynchronizer } from "./settingsSynchronizer";
import {
  configurationFileGlob,
  externalWatchDirectories,
  shaderFileGlob,
} from "./watchers";
import { applicableVariants, VariantList } from "./variants";

const outputName = "HLSL-LSP";

interface ManagedClient extends LifecycleClient {
  readonly runtime: ServerRuntime;
  configurationChanged(): Promise<void>;
  memoryLayout(
    uri: vscode.Uri,
    position: vscode.Position,
  ): Promise<MemoryLayout | null>;
  compilationInfo(uri: vscode.Uri): Promise<CompilationInfo | null>;
  preprocessorExplorer(
    uri: vscode.Uri,
  ): Promise<PreprocessorExplorerReport | null>;
  entryPointDataFlow(uri: vscode.Uri): Promise<EntryPointDataFlow | null>;
  computeVisualization(
    uri: vscode.Uri,
    options: ComputeVisualizationOptions,
  ): Promise<ComputeVisualization | null>;
  effectiveContext(uri: vscode.Uri): Promise<EffectiveShaderContext | null>;
  dxcRuntime(): Promise<DxcRuntimeInfo | null>;
  variants(uri: vscode.Uri | undefined): Promise<VariantList | null>;
}

export interface HlslExtensionApi {
  readonly state: LifecycleState;
  // Exposed so other extensions (and this extension's own integration tests)
  // can read hlsl/compilationInfo results without scraping the webview panel.
  requestCompilationInfo(uri: vscode.Uri): Promise<CompilationInfo | null>;
}

interface DxcRuntimeInfo {
  readonly source: string;
  readonly directory: string;
  readonly libraryPath: string;
  readonly version: string;
  readonly requiresRestart: boolean;
  readonly error?: string;
}

interface RuntimeRestartRequest {
  readonly directory?: string;
  readonly reason?: string;
}

interface ActiveVariantChanged {
  readonly variant: string | null;
}

interface ClientSettings {
  readonly trace: TraceSetting;
  readonly languageVersion: string;
  readonly activeVariant: string;
  readonly server: HlslServerSettings;
}

interface MemoryLayoutTarget {
  readonly textDocument: { readonly uri: string };
  readonly position: { readonly line: number; readonly character: number };
}

let activeLifecycle: ClientLifecycle<ManagedClient> | undefined;

type AnalysisPanelKind =
  | "memoryLayout"
  | "compilationInfo"
  | "resourceBindings"
  | "preprocessorExplorer"
  | "entryPointDataFlow"
  | "computeVisualization";

interface AnalysisPanelTracking {
  readonly kind: AnalysisPanelKind;
  trackingMode: AnalysisTrackingMode;
  targetAvailable: boolean;
  uri: vscode.Uri;
}

interface PersistedAnalysisPanel {
  readonly mode: AnalysisTrackingMode;
  readonly uri: string;
  readonly line?: number;
  readonly character?: number;
  readonly options?: ComputeVisualizationOptions;
}

interface MemoryLayoutViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
  position: vscode.Position | undefined;
  hasContent: boolean;
}

let memoryLayoutState: MemoryLayoutViewState | undefined;
let memoryLayoutGeneration = 0;
let memoryLayoutDebounce: NodeJS.Timeout | undefined;

interface CompilationInfoViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
  // True once a successful hlsl/compilationInfo result has been rendered for
  // the current uri. A later failed or cancelled refresh must never regress
  // this panel to a placeholder or a perpetual loading state, so this flag
  // decides whether a failure keeps the last content or shows an explicit
  // error instead.
  hasContent: boolean;
  // The CompilationInfo most recently rendered into this panel, or
  // undefined before the first successful render (or after switching to a
  // different document, until its own first successful render). Copy/Save
  // Disassembly read the disassembly text from here rather than from
  // anything a `command:` URI could supply, so a stale or crafted link can
  // never exfiltrate or forge disassembly content for another document.
  lastInfo: CompilationInfo | undefined;
  lastInfoUri: vscode.Uri | undefined;
}

let compilationInfoState: CompilationInfoViewState | undefined;
let compilationInfoDebounce: NodeJS.Timeout | undefined;

// Independently tracked from CompilationInfoViewState: the two panels can be
// open for different documents at the same time, and neither refresh path
// may interfere with the other's generation counter or debounce timer.
interface ResourceBindingsViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
  hasContent: boolean;
}

let resourceBindingsState: ResourceBindingsViewState | undefined;
let resourceBindingsDebounce: NodeJS.Timeout | undefined;

// Independently tracked from the other two panels: all three can be open
// for different documents at the same time, and none may interfere with
// another's generation counter or debounce timer.
interface PreprocessorExplorerViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
  hasContent: boolean;
}

let preprocessorExplorerState: PreprocessorExplorerViewState | undefined;
let preprocessorExplorerDebounce: NodeJS.Timeout | undefined;

// Independently tracked from the other panels: all four can be open for
// different documents at the same time, and none may interfere with
// another's generation counter or debounce timer. Unlike the other three
// panels' hand-rolled uri/hasContent/generation/debounce fields, this one
// delegates that whole state machine to `PanelController` (see
// panelController.ts) so it has a single, unit-tested seam for exact
// request payload, out-of-order suppression, document switching, debounce
// timing, and disposal.
interface EntryPointDataFlowViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly controller: PanelController<EntryPointDataFlow>;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
}

let entryPointDataFlowState: EntryPointDataFlowViewState | undefined;

interface ComputeVisualizationViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly controller: PanelController<ComputeVisualization>;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
  options: ComputeVisualizationOptions;
}

let computeVisualizationState: ComputeVisualizationViewState | undefined;

// Debounces the refresh triggered by filesystem watcher events (external
// shader/header file or shadertoolsconfig.json create/change/delete) into
// one refresh of every open analysis panel, rather than one per event (a
// save-from-another-tool, a git checkout, or a rename can fire several
// events in a burst). A single shared instance across restarts: recreated
// only when the extension itself activates/deactivates, independent of the
// per-restart watchers whose events feed it.
let watchedFileRefreshDebouncer: Debouncer | undefined;
let watchedFileRefreshCause: AnalysisFreshnessCause = "Unknown";
let effectiveContextStatusDebouncer: Debouncer | undefined;

const refreshMemoryLayoutCommand = "hlsl.refreshMemoryLayout";
const refreshCompilationInfoCommand = "hlsl.refreshCompilationInfo";
const refreshResourceBindingsCommand = "hlsl.refreshResourceBindings";
const refreshPreprocessorExplorerCommand = "hlsl.refreshPreprocessorExplorer";
const refreshEntryPointDataFlowCommand = "hlsl.refreshEntryPointDataFlow";
const refreshComputeVisualizationCommand = "hlsl.refreshComputeVisualization";
const setAnalysisTrackingModeCommand = "hlsl.setAnalysisTrackingMode";
const analysisPanelStateKeyPrefix = "hlsl.analysisPanel.";

function analysisPanelCommandUris(kind: AnalysisPanelKind): string[] {
  const shared = [setAnalysisTrackingModeCommand];
  switch (kind) {
    case "memoryLayout":
      return [refreshMemoryLayoutCommand, ...shared];
    case "compilationInfo":
      return [
        copyDisassemblyCommand,
        saveDisassemblyCommand,
        refreshCompilationInfoCommand,
        ...shared,
      ];
    case "resourceBindings":
      return [
        openResourceLocationCommand,
        refreshResourceBindingsCommand,
        ...shared,
      ];
    case "preprocessorExplorer":
      return [
        openPreprocessorLocationCommand,
        refreshPreprocessorExplorerCommand,
        ...shared,
      ];
    case "entryPointDataFlow":
      return [
        openEntryPointDataFlowLocationCommand,
        refreshEntryPointDataFlowCommand,
        ...shared,
      ];
    case "computeVisualization":
      return [
        configureComputeVisualizationCommand,
        openComputeVisualizationLocationCommand,
        refreshComputeVisualizationCommand,
        ...shared,
      ];
  }
}

interface AnalysisPanelViewState extends AnalysisPanelTracking {
  readonly panel: vscode.WebviewPanel;
  readonly freshness: AnalysisFreshness;
  readonly refreshCommand: string;
  html: string;
}

function renderAnalysisPanel(state: AnalysisPanelViewState): void {
  state.panel.webview.html = withAnalysisFreshness(
    state.html,
    state.freshness.state,
    state.refreshCommand,
    {
      mode: state.trackingMode,
      target: vscode.workspace.asRelativePath(state.uri, false),
      command: setAnalysisTrackingModeCommand,
      panel: state.kind,
    },
  );
}

function persistedAnalysisPanel(
  context: vscode.ExtensionContext,
  kind: AnalysisPanelKind,
): PersistedAnalysisPanel | undefined {
  const value = context.workspaceState.get<unknown>(
    `${analysisPanelStateKeyPrefix}${kind}`,
  );
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  if (
    (candidate.mode !== "follow" && candidate.mode !== "pinned") ||
    typeof candidate.uri !== "string" ||
    candidate.uri === ""
  ) {
    return undefined;
  }
  const options = validComputeVisualizationOptions(candidate.options);
  return {
    mode: candidate.mode,
    uri: candidate.uri,
    ...(Number.isSafeInteger(candidate.line) && (candidate.line as number) >= 0
      ? { line: candidate.line as number }
      : {}),
    ...(Number.isSafeInteger(candidate.character) &&
    (candidate.character as number) >= 0
      ? { character: candidate.character as number }
      : {}),
    ...(options === undefined ? {} : { options }),
  };
}

function persistAnalysisPanel(
  context: vscode.ExtensionContext,
  state: AnalysisPanelViewState,
): void {
  const persisted: PersistedAnalysisPanel = {
    mode: state.trackingMode,
    uri: state.uri.toString(),
    ...(state.kind === "memoryLayout" &&
    memoryLayoutState?.position !== undefined
      ? {
          line: memoryLayoutState.position.line,
          character: memoryLayoutState.position.character,
        }
      : {}),
    ...(state.kind === "computeVisualization" &&
    computeVisualizationState !== undefined
      ? { options: computeVisualizationState.options }
      : {}),
  };
  void context.workspaceState.update(
    `${analysisPanelStateKeyPrefix}${state.kind}`,
    persisted,
  );
}

function validComputeVisualizationOptions(
  value: unknown,
): ComputeVisualizationOptions | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Record<string, unknown>;
  const dimensions = candidate.dispatchDimensions;
  if (dimensions !== undefined) {
    if (typeof dimensions !== "object" || dimensions === null) {
      return undefined;
    }
    const values = dimensions as Record<string, unknown>;
    if (
      !Number.isSafeInteger(values.x) ||
      !Number.isSafeInteger(values.y) ||
      !Number.isSafeInteger(values.z) ||
      (values.x as number) <= 0 ||
      (values.y as number) <= 0 ||
      (values.z as number) <= 0
    ) {
      return undefined;
    }
  }
  const hardware = candidate.hardwareProfile;
  if (hardware !== undefined) {
    if (typeof hardware !== "object" || hardware === null) {
      return undefined;
    }
    const values = hardware as Record<string, unknown>;
    if (
      typeof values.name !== "string" ||
      values.name.trim() === "" ||
      ![
        values.waveSize,
        values.maxThreadsPerGroup,
        values.maxThreadsPerComputeUnit,
        values.maxGroupsPerComputeUnit,
        values.sharedMemoryBytesPerComputeUnit,
      ].every(
        (number) =>
          typeof number === "number" &&
          Number.isSafeInteger(number) &&
          number > 0,
      )
    ) {
      return undefined;
    }
  }
  return value;
}

function openAnalysisPanelStates(): AnalysisPanelViewState[] {
  const states: (AnalysisPanelViewState | undefined)[] = [
    memoryLayoutState,
    compilationInfoState,
    resourceBindingsState,
    preprocessorExplorerState,
    entryPointDataFlowState,
    computeVisualizationState,
  ];
  return states.filter(
    (state): state is AnalysisPanelViewState => state !== undefined,
  );
}

function analysisPanelState(
  kind: AnalysisPanelKind,
): AnalysisPanelViewState | undefined {
  return openAnalysisPanelStates().find((state) => state.kind === kind);
}

function setAnalysisPanelHtml(
  state: AnalysisPanelViewState,
  html: string,
): void {
  state.html = html;
  renderAnalysisPanel(state);
}

function setAnalysisPanelFreshness(
  state: AnalysisPanelViewState,
  freshness: AnalysisFreshnessState,
): void {
  if (freshness !== state.freshness.state) {
    return;
  }
  renderAnalysisPanel(state);
}

function invalidateOpenAnalysisPanels(
  cause: AnalysisFreshnessCause,
  refreshPending = false,
): void {
  ++memoryLayoutGeneration;
  for (const state of openAnalysisPanelStates()) {
    if (!state.targetAvailable) {
      continue;
    }
    state.freshness.invalidate(cause, refreshPending);
    renderAnalysisPanel(state);
  }
}

function cancelPendingAnalysisRefreshes(): void {
  if (memoryLayoutDebounce !== undefined) {
    clearTimeout(memoryLayoutDebounce);
    memoryLayoutDebounce = undefined;
  }
  if (compilationInfoDebounce !== undefined) {
    clearTimeout(compilationInfoDebounce);
    compilationInfoDebounce = undefined;
  }
  if (resourceBindingsDebounce !== undefined) {
    clearTimeout(resourceBindingsDebounce);
    resourceBindingsDebounce = undefined;
  }
  if (preprocessorExplorerDebounce !== undefined) {
    clearTimeout(preprocessorExplorerDebounce);
    preprocessorExplorerDebounce = undefined;
  }
  entryPointDataFlowState?.controller.cancelScheduledRefresh();
  computeVisualizationState?.controller.cancelScheduledRefresh();
  watchedFileRefreshDebouncer?.cancel();
  watchedFileRefreshCause = "Unknown";
}

function markAnalysisTargetUnavailable(
  state: AnalysisPanelViewState,
  cause: "Active shader unavailable" | "Tracked shader closed",
): void {
  state.targetAvailable = false;
  switch (state.kind) {
    case "memoryLayout":
      ++memoryLayoutGeneration;
      if (memoryLayoutDebounce !== undefined) {
        clearTimeout(memoryLayoutDebounce);
        memoryLayoutDebounce = undefined;
      }
      break;
    case "compilationInfo":
      if (compilationInfoDebounce !== undefined) {
        clearTimeout(compilationInfoDebounce);
        compilationInfoDebounce = undefined;
      }
      break;
    case "resourceBindings":
      if (resourceBindingsDebounce !== undefined) {
        clearTimeout(resourceBindingsDebounce);
        resourceBindingsDebounce = undefined;
      }
      break;
    case "preprocessorExplorer":
      if (preprocessorExplorerDebounce !== undefined) {
        clearTimeout(preprocessorExplorerDebounce);
        preprocessorExplorerDebounce = undefined;
      }
      break;
    case "entryPointDataFlow":
      entryPointDataFlowState?.controller.cancelScheduledRefresh();
      break;
    case "computeVisualization":
      computeVisualizationState?.controller.cancelScheduledRefresh();
      break;
  }
  state.freshness.invalidate(cause);
  renderAnalysisPanel(state);
}

function compilationInfoLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Compiling…</p></body></html>`;
}

function memoryLayoutErrorHtml(message: string): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><h1>Memory Layout</h1><p style="color:var(--vscode-editorWarning-foreground)">${escapeHtml(message)}</p></body></html>`;
}

async function refreshMemoryLayout(
  lifecycle: ClientLifecycle<ManagedClient>,
  uri: vscode.Uri,
  position: vscode.Position,
  cause: AnalysisFreshnessCause = "Manual refresh",
): Promise<void> {
  const state = memoryLayoutState;
  if (state?.targetAvailable !== true) {
    return;
  }
  const generation = state.freshness.beginRefresh(cause);
  renderAnalysisPanel(state);
  let layout: MemoryLayout | null | undefined;
  let failureMessage: string | undefined;
  try {
    layout = await lifecycle.withClient((client) =>
      client.memoryLayout(uri, position),
    );
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    memoryLayoutState !== state ||
    state.uri.toString() !== uri.toString() ||
    state.position?.line !== position.line ||
    state.position.character !== position.character
  ) {
    return;
  }
  if (layout !== null && layout !== undefined) {
    if (!state.freshness.succeed(generation)) {
      return;
    }
    state.hasContent = true;
    state.panel.title = `Memory Layout: ${layout.name || layout.type}`;
    setAnalysisPanelHtml(state, memoryLayoutHtml(layout));
  } else if (state.freshness.fail(generation)) {
    if (!state.hasContent) {
      setAnalysisPanelHtml(
        state,
        memoryLayoutErrorHtml(
          failureMessage ??
            "No compiler-authoritative memory layout is available at this position.",
        ),
      );
    } else {
      renderAnalysisPanel(state);
    }
  }
}

function resourceBindingsLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Analyzing resource bindings…</p></body></html>`;
}

function preprocessorExplorerLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Analyzing preprocessor state…</p></body></html>`;
}

function entryPointDataFlowLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Tracing entry-point data flow…</p></body></html>`;
}

function computeVisualizationLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Analyzing compute dispatch…</p></body></html>`;
}

async function promptComputeLimit(
  title: string,
  value: number,
  prompt?: string,
): Promise<number | undefined> {
  const input = await vscode.window.showInputBox({
    title,
    ...(prompt === undefined ? {} : { prompt }),
    value: String(value),
    validateInput: (candidate) =>
      parsePositiveDimension(candidate) === undefined
        ? "Enter an integer from 1 through 4,294,967,295."
        : undefined,
  });
  return input === undefined ? undefined : parsePositiveDimension(input);
}

async function configureComputeVisualization(
  current: ComputeVisualizationOptions,
): Promise<ComputeVisualizationOptions | null> {
  const currentDispatch = current.dispatchDimensions ?? { x: 1, y: 1, z: 1 };
  const workloadPrompt =
    "Total logical workload threads/elements, not D3D Dispatch() group counts.";
  const x = await promptComputeLimit(
    "Logical workload threads: X",
    currentDispatch.x,
    workloadPrompt,
  );
  if (x === undefined) {
    return null;
  }
  const y = await promptComputeLimit(
    "Logical workload threads: Y",
    currentDispatch.y,
    workloadPrompt,
  );
  if (y === undefined) {
    return null;
  }
  const z = await promptComputeLimit(
    "Logical workload threads: Z",
    currentDispatch.z,
    workloadPrompt,
  );
  if (z === undefined) {
    return null;
  }

  const profileChoice = await vscode.window.showQuickPick(
    [
      {
        label: "No hardware profile",
        description: "Do not estimate occupancy",
        profile: false,
      },
      {
        label: "Custom hardware profile",
        description: "Estimate occupancy from explicit limits",
        profile: true,
      },
    ],
    {
      title: "Compute occupancy profile",
      placeHolder:
        "Occupancy remains unknown unless explicit hardware limits are supplied.",
    },
  );
  if (profileChoice === undefined) {
    return null;
  }

  let hardwareProfile: ComputeHardwareProfile | undefined;
  if (profileChoice.profile) {
    const existing = current.hardwareProfile;
    const name = await vscode.window.showInputBox({
      title: "Hardware profile name",
      value: existing?.name ?? "Custom GPU",
      validateInput: (candidate) =>
        candidate.trim().length === 0 ? "Enter a profile name." : undefined,
    });
    if (name === undefined) {
      return null;
    }
    const waveSize = await promptComputeLimit(
      "Hardware wave size",
      existing?.waveSize ?? 32,
    );
    const maxThreadsPerGroup =
      waveSize === undefined
        ? undefined
        : await promptComputeLimit(
            "Maximum threads per group",
            existing?.maxThreadsPerGroup ?? 1024,
          );
    const maxThreadsPerComputeUnit =
      maxThreadsPerGroup === undefined
        ? undefined
        : await promptComputeLimit(
            "Maximum resident threads per compute unit",
            existing?.maxThreadsPerComputeUnit ?? 2048,
          );
    const maxGroupsPerComputeUnit =
      maxThreadsPerComputeUnit === undefined
        ? undefined
        : await promptComputeLimit(
            "Maximum resident groups per compute unit",
            existing?.maxGroupsPerComputeUnit ?? 32,
          );
    const sharedMemoryBytesPerComputeUnit =
      maxGroupsPerComputeUnit === undefined
        ? undefined
        : await promptComputeLimit(
            "Shared-memory bytes per compute unit",
            existing?.sharedMemoryBytesPerComputeUnit ?? 65536,
          );
    if (
      waveSize === undefined ||
      maxThreadsPerGroup === undefined ||
      maxThreadsPerComputeUnit === undefined ||
      maxGroupsPerComputeUnit === undefined ||
      sharedMemoryBytesPerComputeUnit === undefined
    ) {
      return null;
    }
    hardwareProfile = {
      name: name.trim(),
      waveSize,
      maxThreadsPerGroup,
      maxThreadsPerComputeUnit,
      maxGroupsPerComputeUnit,
      sharedMemoryBytesPerComputeUnit,
    };
  }

  return {
    dispatchDimensions: { x, y, z },
    ...(hardwareProfile === undefined ? {} : { hardwareProfile }),
  };
}

// Fetches the current compilation info for the tracked document and applies it
// to the open panel, guarded by a monotonic generation counter so a slower,
// superseded request can never overwrite a result from a newer one. A failed
// or cancelled request never regresses the panel to a placeholder or a
// perpetual loading state: it keeps the last successful content when one
// exists, and otherwise shows an explicit error.
async function refreshCompilationInfo(
  lifecycle: ClientLifecycle<ManagedClient>,
  uri: vscode.Uri,
  cause: AnalysisFreshnessCause = "Manual refresh",
): Promise<void> {
  const state = compilationInfoState;
  if (state?.targetAvailable !== true) return;
  const generation = state.freshness.beginRefresh(cause);
  renderAnalysisPanel(state);
  let info: CompilationInfo | null | undefined;
  let failureMessage: string | undefined;
  try {
    info = await lifecycle.withClient((client) => client.compilationInfo(uri));
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    compilationInfoState !== state ||
    state.uri.toString() !== uri.toString()
  ) {
    return;
  }
  const accepted =
    info !== null && info !== undefined
      ? state.freshness.succeed(generation)
      : state.freshness.fail(generation);
  if (!accepted) return;
  const outcome = resolveCompilationInfoRefresh(
    state.hasContent,
    info,
    failureMessage,
  );
  state.hasContent = outcome.hasContent;
  if (outcome.info !== undefined) {
    state.lastInfo = outcome.info;
    state.lastInfoUri = uri;
  }
  if (outcome.title !== undefined) {
    state.panel.title = outcome.title;
  }
  if (outcome.html !== undefined) {
    setAnalysisPanelHtml(state, outcome.html);
  } else {
    renderAnalysisPanel(state);
  }
}

// Mirrors refreshCompilationInfo exactly, reusing the same
// hlsl/compilationInfo request (the Resource Bindings view is a different
// presentation of the identical response, not a different protocol
// request), but against its own independently tracked panel/generation so
// the two views can never interfere with each other.
async function refreshResourceBindings(
  lifecycle: ClientLifecycle<ManagedClient>,
  uri: vscode.Uri,
  cause: AnalysisFreshnessCause = "Manual refresh",
): Promise<void> {
  const state = resourceBindingsState;
  if (state?.targetAvailable !== true) return;
  const generation = state.freshness.beginRefresh(cause);
  renderAnalysisPanel(state);
  let info: CompilationInfo | null | undefined;
  let failureMessage: string | undefined;
  try {
    info = await lifecycle.withClient((client) => client.compilationInfo(uri));
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    resourceBindingsState !== state ||
    state.uri.toString() !== uri.toString()
  ) {
    return;
  }
  const accepted =
    info !== null && info !== undefined
      ? state.freshness.succeed(generation)
      : state.freshness.fail(generation);
  if (!accepted) return;
  const outcome = resolveResourceBindingsRefresh(
    state.hasContent,
    info,
    failureMessage,
  );
  state.hasContent = outcome.hasContent;
  if (outcome.title !== undefined) {
    state.panel.title = outcome.title;
  }
  if (outcome.html !== undefined) {
    setAnalysisPanelHtml(state, outcome.html);
  } else {
    renderAnalysisPanel(state);
  }
}

// Mirrors refreshCompilationInfo/refreshResourceBindings, but issues its
// own `hlsl/preprocessorExplorer` request (a distinct protocol request,
// unlike Resource Bindings which reuses hlsl/compilationInfo) against its
// own independently tracked panel/generation.
async function refreshPreprocessorExplorer(
  lifecycle: ClientLifecycle<ManagedClient>,
  uri: vscode.Uri,
  cause: AnalysisFreshnessCause = "Manual refresh",
): Promise<void> {
  const state = preprocessorExplorerState;
  if (state?.targetAvailable !== true) return;
  const generation = state.freshness.beginRefresh(cause);
  renderAnalysisPanel(state);
  let report: PreprocessorExplorerReport | null | undefined;
  let failureMessage: string | undefined;
  try {
    report = await lifecycle.withClient((client) =>
      client.preprocessorExplorer(uri),
    );
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    preprocessorExplorerState !== state ||
    state.uri.toString() !== uri.toString()
  ) {
    return;
  }
  const accepted =
    report !== null && report !== undefined
      ? state.freshness.succeed(generation)
      : state.freshness.fail(generation);
  if (!accepted) return;
  const outcome = resolvePreprocessorExplorerRefresh(
    state.hasContent,
    report,
    failureMessage,
  );
  state.hasContent = outcome.hasContent;
  if (outcome.title !== undefined) {
    state.panel.title = outcome.title;
  }
  if (outcome.html !== undefined) {
    setAnalysisPanelHtml(state, outcome.html);
  } else {
    renderAnalysisPanel(state);
  }
}

// Refreshes every currently open analysis panel (Shader Compilation,
// Resource Bindings, Preprocessor Explorer, Entry-Point Data Flow) against
// its own currently tracked document. Used for triggers relevant to *any*
// open panel regardless of which document changed: a compiler-affecting
// setting change (entry point, target profile, defines, arguments,
// language version, active variant), a successful restart (which can
// change include directories/mappings/server path/DXC runtime), and a
// save to *any* open HLSL document -- since every one of these panels
// analyzes its root document's current unsaved snapshot *plus* any
// #include'd file open elsewhere in the workspace, and the client has no
// dependency query telling it which open document a given root actually
// includes, refreshing conservatively is the safe choice (see
// docs/call-hierarchy.md's "Includes and unsaved edits").
async function refreshAllOpenAnalysisPanels(
  lifecycle: ClientLifecycle<ManagedClient>,
  cause: AnalysisFreshnessCause,
): Promise<void> {
  const tasks: Promise<void>[] = [];
  if (memoryLayoutState?.targetAvailable) {
    if (memoryLayoutState.position !== undefined) {
      tasks.push(
        refreshMemoryLayout(
          lifecycle,
          memoryLayoutState.uri,
          memoryLayoutState.position,
          cause,
        ),
      );
    }
  }
  if (compilationInfoState?.targetAvailable) {
    tasks.push(
      refreshCompilationInfo(lifecycle, compilationInfoState.uri, cause),
    );
  }
  if (resourceBindingsState?.targetAvailable) {
    tasks.push(
      refreshResourceBindings(lifecycle, resourceBindingsState.uri, cause),
    );
  }
  if (preprocessorExplorerState?.targetAvailable) {
    tasks.push(
      refreshPreprocessorExplorer(
        lifecycle,
        preprocessorExplorerState.uri,
        cause,
      ),
    );
  }
  const entryPointRefresh = entryPointDataFlowState?.targetAvailable
    ? entryPointDataFlowState.controller.refreshTracked(cause)
    : undefined;
  if (entryPointRefresh !== undefined) {
    tasks.push(entryPointRefresh);
  }
  const computeRefresh = computeVisualizationState?.targetAvailable
    ? computeVisualizationState.controller.refreshTracked(cause)
    : undefined;
  if (computeRefresh !== undefined) {
    tasks.push(computeRefresh);
  }
  await Promise.all(tasks);
}

async function restoreOpenedAnalysisTargets(
  context: vscode.ExtensionContext,
  lifecycle: ClientLifecycle<ManagedClient>,
  document: vscode.TextDocument,
): Promise<void> {
  const openedUri = document.uri.toString();
  const tasks: Promise<void>[] = [];
  for (const state of openAnalysisPanelStates()) {
    if (
      !isReopenedAnalysisTarget(
        state.trackingMode,
        state.uri.toString(),
        openedUri,
        state.targetAvailable,
      )
    ) {
      continue;
    }
    state.targetAvailable = true;
    persistAnalysisPanel(context, state);
    renderAnalysisPanel(state);
    switch (state.kind) {
      case "memoryLayout": {
        const current = memoryLayoutState;
        if (current === state && current.position !== undefined) {
          tasks.push(
            refreshMemoryLayout(
              lifecycle,
              current.uri,
              current.position,
              "Active shader change",
            ),
          );
        }
        break;
      }
      case "compilationInfo":
        tasks.push(
          refreshCompilationInfo(lifecycle, state.uri, "Active shader change"),
        );
        break;
      case "resourceBindings":
        tasks.push(
          refreshResourceBindings(lifecycle, state.uri, "Active shader change"),
        );
        break;
      case "preprocessorExplorer":
        tasks.push(
          refreshPreprocessorExplorer(
            lifecycle,
            state.uri,
            "Active shader change",
          ),
        );
        break;
      case "entryPointDataFlow": {
        const refresh =
          entryPointDataFlowState === state
            ? entryPointDataFlowState.controller.refreshTracked(
                "Active shader change",
              )
            : undefined;
        if (refresh !== undefined) tasks.push(refresh);
        break;
      }
      case "computeVisualization": {
        const refresh =
          computeVisualizationState === state
            ? computeVisualizationState.controller.refreshTracked(
                "Active shader change",
              )
            : undefined;
        if (refresh !== undefined) tasks.push(refresh);
        break;
      }
    }
  }
  await Promise.all(tasks);
}

function isAnalysisPanelKind(value: string): value is AnalysisPanelKind {
  return (
    value === "memoryLayout" ||
    value === "compilationInfo" ||
    value === "resourceBindings" ||
    value === "preprocessorExplorer" ||
    value === "entryPointDataFlow" ||
    value === "computeVisualization"
  );
}

function targetRequiresRefresh(state: AnalysisPanelViewState): boolean {
  return !state.targetAvailable;
}

async function retargetFollowingPanel(
  context: vscode.ExtensionContext,
  lifecycle: ClientLifecycle<ManagedClient>,
  kind: AnalysisPanelKind,
  editor: vscode.TextEditor,
): Promise<void> {
  const uri = editor.document.uri;
  switch (kind) {
    case "memoryLayout": {
      const state = memoryLayoutState;
      if (state?.trackingMode !== "follow") return;
      const position = editor.selection.active;
      if (
        state.uri.toString() === uri.toString() &&
        state.position?.line === position.line &&
        state.position.character === position.character &&
        !targetRequiresRefresh(state)
      ) {
        return;
      }
      ++memoryLayoutGeneration;
      if (memoryLayoutDebounce !== undefined) {
        clearTimeout(memoryLayoutDebounce);
        memoryLayoutDebounce = undefined;
      }
      state.uri = uri;
      state.position = position;
      state.targetAvailable = true;
      persistAnalysisPanel(context, state);
      await refreshMemoryLayout(
        lifecycle,
        uri,
        position,
        "Active shader change",
      );
      return;
    }
    case "compilationInfo": {
      const state = compilationInfoState;
      if (
        state?.trackingMode !== "follow" ||
        (state.uri.toString() === uri.toString() &&
          !targetRequiresRefresh(state))
      ) {
        return;
      }
      if (compilationInfoDebounce !== undefined) {
        clearTimeout(compilationInfoDebounce);
        compilationInfoDebounce = undefined;
      }
      state.uri = uri;
      state.targetAvailable = true;
      persistAnalysisPanel(context, state);
      await refreshCompilationInfo(lifecycle, uri, "Active shader change");
      return;
    }
    case "resourceBindings": {
      const state = resourceBindingsState;
      if (
        state?.trackingMode !== "follow" ||
        (state.uri.toString() === uri.toString() &&
          !targetRequiresRefresh(state))
      ) {
        return;
      }
      if (resourceBindingsDebounce !== undefined) {
        clearTimeout(resourceBindingsDebounce);
        resourceBindingsDebounce = undefined;
      }
      state.uri = uri;
      state.targetAvailable = true;
      persistAnalysisPanel(context, state);
      await refreshResourceBindings(lifecycle, uri, "Active shader change");
      return;
    }
    case "preprocessorExplorer": {
      const state = preprocessorExplorerState;
      if (
        state?.trackingMode !== "follow" ||
        (state.uri.toString() === uri.toString() &&
          !targetRequiresRefresh(state))
      ) {
        return;
      }
      if (preprocessorExplorerDebounce !== undefined) {
        clearTimeout(preprocessorExplorerDebounce);
        preprocessorExplorerDebounce = undefined;
      }
      state.uri = uri;
      state.targetAvailable = true;
      persistAnalysisPanel(context, state);
      await refreshPreprocessorExplorer(lifecycle, uri, "Active shader change");
      return;
    }
    case "entryPointDataFlow": {
      const state = entryPointDataFlowState;
      if (
        state?.trackingMode !== "follow" ||
        (state.uri.toString() === uri.toString() &&
          !targetRequiresRefresh(state))
      ) {
        return;
      }
      state.controller.cancelScheduledRefresh();
      state.uri = uri;
      state.targetAvailable = true;
      state.controller.open(uri.toString());
      persistAnalysisPanel(context, state);
      await state.controller.refresh(uri.toString(), "Active shader change");
      return;
    }
    case "computeVisualization": {
      const state = computeVisualizationState;
      if (
        state?.trackingMode !== "follow" ||
        (state.uri.toString() === uri.toString() &&
          !targetRequiresRefresh(state))
      ) {
        return;
      }
      state.controller.cancelScheduledRefresh();
      state.uri = uri;
      state.targetAvailable = true;
      state.controller.open(uri.toString());
      persistAnalysisPanel(context, state);
      await state.controller.refresh(uri.toString(), "Active shader change");
    }
  }
}

async function followActiveShader(
  context: vscode.ExtensionContext,
  lifecycle: ClientLifecycle<ManagedClient>,
  editor: vscode.TextEditor | undefined,
  kind?: AnalysisPanelKind,
): Promise<void> {
  const states =
    kind === undefined
      ? openAnalysisPanelStates().filter(
          (state) => state.trackingMode === "follow",
        )
      : [analysisPanelState(kind)].filter(
          (state): state is AnalysisPanelViewState =>
            state?.trackingMode === "follow",
        );
  if (editor?.document.languageId !== "hlsl") {
    for (const state of states) {
      if (
        analysisTargetTransition(
          state.trackingMode,
          state.uri.toString(),
          undefined,
          state.targetAvailable,
        ) === "unavailable"
      ) {
        markAnalysisTargetUnavailable(state, "Active shader unavailable");
      }
    }
    return;
  }
  await Promise.all(
    states.map((state) =>
      retargetFollowingPanel(context, lifecycle, state.kind, editor),
    ),
  );
}

function createEntryPointDataFlowController(
  lifecycle: ClientLifecycle<ManagedClient>,
  panel: vscode.WebviewPanel,
  freshness: AnalysisFreshness,
): PanelController<EntryPointDataFlow> {
  return new PanelController<EntryPointDataFlow>(
    {
      setHtml: (html) => {
        if (entryPointDataFlowState?.panel === panel) {
          setAnalysisPanelHtml(entryPointDataFlowState, html);
        }
      },
      setTitle: (title) => {
        panel.title = title;
      },
      setFreshness: (state) => {
        if (entryPointDataFlowState?.panel === panel) {
          setAnalysisPanelFreshness(entryPointDataFlowState, state);
        }
      },
    },
    (uriString) =>
      lifecycle.withClient((client) =>
        client.entryPointDataFlow(vscode.Uri.parse(uriString)),
      ),
    resolveEntryPointDataFlowRefresh,
    nodeScheduler,
    500,
    freshness,
  );
}

function createComputeVisualizationController(
  lifecycle: ClientLifecycle<ManagedClient>,
  panel: vscode.WebviewPanel,
  freshness: AnalysisFreshness,
): PanelController<ComputeVisualization> {
  return new PanelController<ComputeVisualization>(
    {
      setHtml: (html) => {
        if (computeVisualizationState?.panel === panel) {
          setAnalysisPanelHtml(computeVisualizationState, html);
        }
      },
      setTitle: (title) => {
        panel.title = title;
      },
      setFreshness: (state) => {
        if (computeVisualizationState?.panel === panel) {
          setAnalysisPanelFreshness(computeVisualizationState, state);
        }
      },
    },
    (uriString) =>
      lifecycle.withClient((client) =>
        client.computeVisualization(
          vscode.Uri.parse(uriString),
          computeVisualizationState?.options ?? {},
        ),
      ),
    resolveComputeVisualizationRefresh,
    nodeScheduler,
    500,
    freshness,
  );
}

// Editor settings that become higher-precedence overrides affecting what
// the server actually compiles/analyzes (see the Configuration section of
// README.md) without requiring a restart -- a change to any of these
// leaves every open analysis panel showing content resolved under the
// *previous* value unless it is explicitly refreshed here.
// `hlsl.server.path`/`hlsl.additionalIncludeDirectories`/
// `hlsl.virtualDirectoryMappings`/`hlsl.dxcRuntimeDirectory` are handled
// separately: they restart the client, and every panel is refreshed after
// that restart completes instead.
function configurationResource(): vscode.Uri | undefined {
  return vscode.workspace.workspaceFolders?.[0]?.uri;
}

function configuration(): vscode.WorkspaceConfiguration {
  return vscode.workspace.getConfiguration("hlsl", configurationResource());
}

function clientSettings(): ClientSettings {
  const reader = configuration();
  return {
    trace: readTraceSetting(reader),
    languageVersion: readDefaultLanguageVersion(reader),
    activeVariant: readActiveVariant(reader),
    server: readServerSettings(reader),
  };
}

function runtimeEnvironment(
  context: vscode.ExtensionContext,
): RuntimeEnvironment {
  return {
    platform: process.platform,
    architecture: process.arch,
    extensionPath: context.extensionPath,
    workspaceFolders:
      vscode.workspace.workspaceFolders?.map((folder) => folder.uri.fsPath) ??
      [],
  };
}

function traceValue(value: TraceSetting): Trace {
  switch (value) {
    case "messages":
      return Trace.Messages;
    case "verbose":
      return Trace.Verbose;
    case "off":
      return Trace.Off;
  }
}

function createWatchers(
  settings: HlslServerSettings,
): vscode.FileSystemWatcher[] {
  const watchers = [
    vscode.workspace.createFileSystemWatcher(shaderFileGlob),
    vscode.workspace.createFileSystemWatcher(configurationFileGlob),
  ];
  const workspaceFolders =
    vscode.workspace.workspaceFolders?.map((folder) => folder.uri.fsPath) ?? [];
  for (const directory of externalWatchDirectories(
    settings,
    workspaceFolders,
  )) {
    watchers.push(
      vscode.workspace.createFileSystemWatcher(
        new vscode.RelativePattern(vscode.Uri.file(directory), shaderFileGlob),
      ),
    );
  }
  return watchers;
}

class VscodeLanguageClient implements ManagedClient {
  private readonly client: LanguageClient;
  private readonly settingsSynchronizer: RunningSettingsSynchronizer<ClientSettings>;
  private readonly stateSubscription: vscode.Disposable;
  private readonly watcherSubscriptions: vscode.Disposable[];
  private readonly recoveryTracker = new ConnectionRecoveryTracker();
  private disposed = false;

  public constructor(
    public readonly runtime: ServerRuntime,
    outputChannel: vscode.LogOutputChannel,
    private readonly watchers: readonly vscode.FileSystemWatcher[],
    initialSettings: ClientSettings,
    serverArgs: readonly string[],
    onRuntimeRestartRequired: (request: RuntimeRestartRequest) => void,
    onActiveVariantChanged: (variant: string | null) => void,
    onWatchedFileEvent: (uri: vscode.Uri) => void,
    onConnectionTransition: (transition: ConnectionTransition) => void,
  ) {
    const executable: Executable = {
      command: runtime.command,
      args: [...serverArgs],
      options: {
        cwd: runtime.workingDirectory,
      },
    };
    const serverOptions: ServerOptions = {
      run: executable,
      debug: executable,
    };
    const clientOptions: LanguageClientOptions = {
      documentSelector: [{ language: "hlsl", scheme: "file" }],
      initializationOptions: {
        hlsl: {
          languageVersion: initialSettings.languageVersion,
          activeVariant: initialSettings.activeVariant || undefined,
        },
        commandLinks: true,
      },
      outputChannel,
      traceOutputChannel: outputChannel,
      revealOutputChannelOn: RevealOutputChannelOn.Never,
      synchronize: {
        fileEvents: [...watchers],
      },
      middleware: {
        provideHover: async (document, position, token, next) => {
          const hover = await next(document, position, token);
          if (hover !== null && hover !== undefined) {
            for (const content of hover.contents) {
              if (
                content instanceof vscode.MarkdownString &&
                content.value.includes("command:hlsl.showMemoryLayout")
              ) {
                content.isTrusted = {
                  enabledCommands: ["hlsl.showMemoryLayout"],
                };
              }
            }
          }
          return hover;
        },
      },
    };
    this.client = new LanguageClient(
      "hlsl",
      outputName,
      serverOptions,
      clientOptions,
    );
    // `synchronize.fileEvents` above only forwards these same watcher events
    // to the server (as `workspace/didChangeWatchedFiles`) so it stays in
    // sync -- it does not, by itself, refresh anything the client itself has
    // rendered. An external edit/create/delete of a shader/header file or of
    // shadertoolsconfig.json can change what every open analysis panel
    // should show (a changed #include, a changed compiler configuration),
    // so each such event also asks the caller to (debounce-)refresh them.
    this.watcherSubscriptions = watchers.flatMap((watcher) => [
      watcher.onDidCreate(onWatchedFileEvent),
      watcher.onDidChange(onWatchedFileEvent),
      watcher.onDidDelete(onWatchedFileEvent),
    ]);
    this.client.onNotification(
      "hlsl/dxcRuntimeRestartRequired",
      (params: unknown) => {
        onRuntimeRestartRequired(runtimeRestartRequest(params));
      },
    );
    // The recovery code action's hlsl-lsp.selectVariant command changes only
    // the server's in-memory active variant; this notification is how the
    // server reports the resulting authoritative value back so the client can
    // durably persist it (matching the manual "Select HLSL Shader Variant"
    // picker's own persistence), instead of leaving the selection
    // session-only and silently diverging from hlsl.activeVariant.
    this.client.onNotification(
      "hlsl/activeVariantChanged",
      (params: unknown) => {
        onActiveVariantChanged(activeVariantChanged(params));
      },
    );
    this.settingsSynchronizer = new RunningSettingsSynchronizer<ClientSettings>(
      clientSettings,
      (settings, isCurrentConnection) =>
        this.applySettings(settings, isCurrentConnection),
    );
    this.stateSubscription = this.client.onDidChangeState((event) => {
      const running = event.newState === State.Running;
      const observation = this.disposed
        ? undefined
        : this.recoveryTracker.observe(running);
      const synchronization = this.settingsSynchronizer.stateChanged(running);
      const reportSynchronizationError = (error: unknown): void => {
        outputChannel.appendLine(
          `[error] Unable to synchronize HLSL settings after a language server state change: ${errorMessage(error)}`,
        );
      };
      if (observation?.transition === "recovered") {
        void runGuardedRecovery(
          synchronization,
          () =>
            new Promise<void>((resolve) => {
              setTimeout(resolve, 0);
            }),
          () =>
            !this.disposed &&
            this.client.state === State.Running &&
            this.recoveryTracker.isCurrentRecovery(observation.epoch),
          () => {
            onConnectionTransition(observation.transition);
          },
        ).catch(reportSynchronizationError);
        return;
      }
      if (observation !== undefined) {
        onConnectionTransition(observation.transition);
      }
      void synchronization.catch(reportSynchronizationError);
    });
  }

  public async start(): Promise<void> {
    await this.client.start();
    await this.settingsSynchronizer.stateChanged(
      this.client.state === State.Running,
    );
    await this.settingsSynchronizer.ensureSynchronized();
  }

  public async stop(): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    await this.settingsSynchronizer.stateChanged(false);
    try {
      if (this.client.needsStop()) {
        await this.client.dispose();
      }
    } finally {
      this.stateSubscription.dispose();
      for (const subscription of this.watcherSubscriptions) {
        subscription.dispose();
      }
      for (const watcher of this.watchers) {
        watcher.dispose();
      }
    }
  }

  public configurationChanged(): Promise<void> {
    return this.settingsSynchronizer.configurationChanged();
  }

  public memoryLayout(
    uri: vscode.Uri,
    position: vscode.Position,
  ): Promise<MemoryLayout | null> {
    return this.client.sendRequest<MemoryLayout | null>("hlsl/memoryLayout", {
      textDocument: { uri: uri.toString() },
      position: { line: position.line, character: position.character },
    });
  }

  public compilationInfo(uri: vscode.Uri): Promise<CompilationInfo | null> {
    return this.client.sendRequest<CompilationInfo | null>(
      "hlsl/compilationInfo",
      { textDocument: { uri: uri.toString() } },
    );
  }

  public effectiveContext(
    uri: vscode.Uri,
  ): Promise<EffectiveShaderContext | null> {
    return this.client.sendRequest<EffectiveShaderContext | null>(
      "hlsl/effectiveContext",
      { textDocument: { uri: uri.toString() } },
    );
  }

  public preprocessorExplorer(
    uri: vscode.Uri,
  ): Promise<PreprocessorExplorerReport | null> {
    return this.client.sendRequest<PreprocessorExplorerReport | null>(
      "hlsl/preprocessorExplorer",
      { textDocument: { uri: uri.toString() } },
    );
  }

  public entryPointDataFlow(
    uri: vscode.Uri,
  ): Promise<EntryPointDataFlow | null> {
    return this.client.sendRequest<EntryPointDataFlow | null>(
      "hlsl/entryPointDataFlow",
      entryPointDataFlowRequestParams(uri.toString()),
    );
  }

  public computeVisualization(
    uri: vscode.Uri,
    options: ComputeVisualizationOptions,
  ): Promise<ComputeVisualization | null> {
    return this.client.sendRequest<ComputeVisualization | null>(
      "hlsl/computeVisualization",
      computeVisualizationRequestParams(uri.toString(), options),
    );
  }

  public dxcRuntime(): Promise<DxcRuntimeInfo | null> {
    return this.client.sendRequest<DxcRuntimeInfo | null>(
      "hlsl/dxcRuntime",
      {},
    );
  }

  public variants(uri: vscode.Uri | undefined): Promise<VariantList | null> {
    return this.client.sendRequest<VariantList | null>(
      "hlsl/variants",
      uri ? { textDocument: { uri: uri.toString() } } : {},
    );
  }

  private async applySettings(
    settings: ClientSettings,
    isCurrentConnection: () => boolean,
  ): Promise<boolean> {
    if (!isCurrentConnection() || !this.clientIsRunning()) {
      return false;
    }
    await this.client.setTrace(traceValue(settings.trace));
    if (!isCurrentConnection() || !this.clientIsRunning()) {
      return false;
    }
    await this.client.sendNotification("hlsl/didChangeClientDefaults", {
      hlsl: { languageVersion: settings.languageVersion },
    });
    if (!isCurrentConnection() || !this.clientIsRunning()) {
      return false;
    }
    await this.client.sendNotification("workspace/didChangeConfiguration", {
      settings: { hlsl: settings.server },
    });
    if (!isCurrentConnection() || !this.clientIsRunning()) {
      return false;
    }
    await this.client.sendNotification("hlsl/didChangeActiveVariant", {
      variant: settings.activeVariant !== "" ? settings.activeVariant : null,
    });
    return isCurrentConnection() && this.clientIsRunning();
  }

  private clientIsRunning(): boolean {
    return this.client.state === State.Running;
  }
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function runtimeRestartRequest(value: unknown): RuntimeRestartRequest {
  if (typeof value !== "object" || value === null) {
    return {};
  }
  const candidate = value as Partial<RuntimeRestartRequest>;
  const result: { directory?: string; reason?: string } = {};
  if (typeof candidate.directory === "string") {
    result.directory = candidate.directory;
  }
  if (typeof candidate.reason === "string") {
    result.reason = candidate.reason;
  }
  return result;
}

function activeVariantChanged(value: unknown): string | null {
  if (typeof value !== "object" || value === null) {
    return null;
  }
  const candidate = value as Partial<ActiveVariantChanged>;
  return typeof candidate.variant === "string" ? candidate.variant : null;
}

function memoryLayoutTarget(value: unknown): MemoryLayoutTarget | undefined {
  if (typeof value !== "object" || value === null) {
    return undefined;
  }
  const candidate = value as Partial<MemoryLayoutTarget>;
  if (
    typeof candidate.textDocument?.uri !== "string" ||
    typeof candidate.position?.line !== "number" ||
    typeof candidate.position.character !== "number"
  ) {
    return undefined;
  }
  return candidate as MemoryLayoutTarget;
}

async function reportError(
  outputChannel: vscode.OutputChannel,
  action: string,
  error: unknown,
): Promise<void> {
  const message = `${action}: ${errorMessage(error)}`;
  outputChannel.appendLine(`[error] ${message}`);
  const selection = await vscode.window.showErrorMessage(
    message,
    "Open Settings",
    "Show Output",
  );
  if (selection === "Open Settings") {
    await vscode.commands.executeCommand(
      "workbench.action.openSettings",
      "hlsl.server.path",
    );
  } else if (selection === "Show Output") {
    outputChannel.show(true);
  }
}

export async function activate(
  context: vscode.ExtensionContext,
): Promise<HlslExtensionApi> {
  const outputChannel = vscode.window.createOutputChannel(outputName, {
    log: true,
  });
  context.subscriptions.push(outputChannel);

  const variantStatus = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Right,
    100,
  );
  variantStatus.command = "hlsl.selectVariant";
  context.subscriptions.push(variantStatus);

  let workspaceRuntimeDirectory: string | undefined;

  const lifecycle = new ClientLifecycle<ManagedClient>(async () => {
    const reader = configuration();
    const configuredPath = reader.get<string>("server.path");
    const initialSettings: ClientSettings = {
      trace: readTraceSetting(reader),
      languageVersion: readDefaultLanguageVersion(reader),
      activeVariant: readActiveVariant(reader),
      server: readServerSettings(reader),
    };
    const runtime = await resolveServerRuntime(
      configuredPath,
      runtimeEnvironment(context),
    );
    const configuredRuntime = reader.get<string>("dxcRuntimeDirectory");
    const selectedRuntime =
      configuredRuntime && configuredRuntime.trim() !== ""
        ? configuredRuntime
        : workspaceRuntimeDirectory;
    const dxcRuntime = await resolveDxcRuntimeDirectory(
      selectedRuntime,
      runtimeEnvironment(context),
    );
    const serverArgs = dxcRuntime
      ? ["--dxc-runtime", dxcRuntime.directory]
      : [];
    outputChannel.appendLine(
      `Starting ${runtime.source} server: ${runtime.command}`,
    );
    if (dxcRuntime) {
      outputChannel.appendLine(`Selected DXC runtime: ${dxcRuntime.directory}`);
    }
    const watchers = createWatchers(initialSettings.server);
    try {
      return new VscodeLanguageClient(
        runtime,
        outputChannel,
        watchers,
        initialSettings,
        serverArgs,
        handleRuntimeRestartRequired,
        handleActiveVariantChanged,
        (uri) => {
          const cause: AnalysisFreshnessCause = uri.path
            .toLowerCase()
            .endsWith("/shadertoolsconfig.json")
            ? "Configuration change"
            : "Source edit";
          watchedFileRefreshCause =
            watchedFileRefreshCause === "Configuration change"
              ? watchedFileRefreshCause
              : cause;
          invalidateOpenAnalysisPanels(cause, true);
          watchedFileRefreshDebouncer?.schedule();
          effectiveContextStatusDebouncer?.schedule();
        },
        (transition) => {
          if (transition === "disconnected") {
            cancelPendingAnalysisRefreshes();
            invalidateOpenAnalysisPanels("Disconnected server");
            return;
          }
          if (lifecycle.state === "running") {
            void refreshAllOpenAnalysisPanels(lifecycle, "Disconnected server");
          }
        },
      );
    } catch (error) {
      for (const watcher of watchers) {
        watcher.dispose();
      }
      throw error;
    }
  });
  activeLifecycle = lifecycle;
  watchedFileRefreshDebouncer = new Debouncer(() => {
    const cause = watchedFileRefreshCause;
    watchedFileRefreshCause = "Unknown";
    void refreshAllOpenAnalysisPanels(lifecycle, cause);
  });

  let variantStatusGeneration = 0;
  let variantStatusDocument: string | undefined;
  const updateVariantStatus = async (): Promise<void> => {
    const generation = ++variantStatusGeneration;
    const editor = vscode.window.activeTextEditor;
    if (editor?.document.languageId !== "hlsl") {
      variantStatus.hide();
      return;
    }
    const documentUri = editor.document.uri;
    const documentKey = documentUri.toString();
    if (variantStatusDocument !== documentKey) {
      variantStatus.hide();
      variantStatusDocument = undefined;
    }
    let shaderContext: EffectiveShaderContext | null | undefined;
    try {
      shaderContext = await lifecycle.withClient((client) =>
        client.effectiveContext(documentUri),
      );
    } catch {
      shaderContext = undefined;
    }
    if (
      generation !== variantStatusGeneration ||
      vscode.window.activeTextEditor?.document.uri.toString() !==
        documentUri.toString()
    ) {
      return;
    }
    if (shaderContext === undefined || shaderContext === null) {
      if (variantStatusDocument !== documentKey) {
        variantStatus.hide();
      }
      return;
    }
    variantStatusDocument = documentKey;
    variantStatus.text = `$(versions) HLSL: ${variantLabel(shaderContext.activeVariant)}`;
    const tooltip = new vscode.MarkdownString(
      effectiveContextTooltip(shaderContext),
    );
    variantStatus.tooltip = tooltip;
    variantStatus.show();
  };
  effectiveContextStatusDebouncer = new Debouncer(() => {
    void updateVariantStatus();
  });

  const restart = async (): Promise<void> => {
    cancelPendingAnalysisRefreshes();
    invalidateOpenAnalysisPanels("Disconnected server", true);
    try {
      await lifecycle.restart();
      outputChannel.appendLine("Language server restarted.");
      // A restart can change what the server compiles/analyzes (a
      // different DXC runtime/version, server path, include
      // directories/mappings, or a reloaded workspace-folder
      // configuration), so every open analysis panel is refreshed here --
      // once, after the restart actually succeeds -- rather than left
      // showing content resolved by the previous server instance. Shared
      // by every restart trigger (the manual "Restart Server" command,
      // workspace-folder changes, a server-requested runtime restart, and
      // the restart-triggering configuration branches below) so none of
      // them needs its own duplicate post-restart refresh.
      await refreshAllOpenAnalysisPanels(lifecycle, "Disconnected server");
    } catch (error) {
      await reportError(
        outputChannel,
        "Unable to restart the HLSL language server",
        error,
      );
    }
    await updateVariantStatus();
  };

  // Persists the resulting active variant a hlsl-lsp.selectVariant command
  // reported back (see the comment above the client's onNotification
  // registration) into hlsl.activeVariant, matching the manual "Select HLSL
  // Shader Variant" picker's own scope choice, so the recovery action's
  // effect survives a later settings resync or restart instead of being
  // session-only server state that silently diverges from the setting.
  const handleActiveVariantChanged = (variant: string | null): void => {
    const normalized = variant ?? "";
    if (readActiveVariant(configuration()) === normalized) {
      // Already converged: either this is the settings-resync echo of a
      // change this client just persisted itself, or the value already
      // matches for another reason. Skip the write to avoid a redundant
      // configuration change (and the resync it would otherwise retrigger).
      return;
    }
    const target =
      vscode.workspace.workspaceFolders &&
      vscode.workspace.workspaceFolders.length > 0
        ? vscode.ConfigurationTarget.Workspace
        : vscode.ConfigurationTarget.Global;
    void (async () => {
      try {
        await configuration().update("activeVariant", normalized, target);
        await updateVariantStatus();
      } catch (error) {
        await reportError(
          outputChannel,
          "Unable to persist the HLSL shader variant selection reported by the language server",
          error,
        );
      }
    })();
  };

  // The server requests a controlled restart when shadertoolsconfig.json selects
  // a different DXC runtime. An explicit editor setting takes precedence, and an
  // already-applied selection is ignored, so no restart loop can form.
  const handleRuntimeRestartRequired = (
    request: RuntimeRestartRequest,
  ): void => {
    const editorRuntime = configuration().get<string>("dxcRuntimeDirectory");
    if (editorRuntime && editorRuntime.trim() !== "") {
      return;
    }
    const requested =
      request.directory && request.directory.trim() !== ""
        ? request.directory.trim()
        : undefined;
    if (workspaceRuntimeDirectory === requested) {
      return;
    }
    workspaceRuntimeDirectory = requested;
    outputChannel.appendLine(
      `Applying workspace DXC runtime (${requested ?? "bundled default"})` +
        (request.reason ? `: ${request.reason}` : ""),
    );
    void restart();
  };

  const restoreAnalysisPanel = async (
    kind: AnalysisPanelKind,
    panel: vscode.WebviewPanel,
  ): Promise<void> => {
    const persisted = persistedAnalysisPanel(context, kind);
    if (persisted === undefined) {
      panel.dispose();
      return;
    }
    let uri: vscode.Uri;
    try {
      uri = vscode.Uri.parse(persisted.uri, true);
    } catch {
      panel.dispose();
      return;
    }
    const activeEditor = vscode.window.activeTextEditor;
    if (
      persisted.mode === "follow" &&
      activeEditor?.document.languageId === "hlsl"
    ) {
      uri = activeEditor.document.uri;
    }
    const targetAvailable =
      persisted.mode === "follow"
        ? activeEditor?.document.languageId === "hlsl"
        : vscode.workspace.textDocuments.some(
            (document) => document.uri.toString() === uri.toString(),
          );
    panel.webview.options = {
      enableScripts: false,
      enableCommandUris: analysisPanelCommandUris(kind),
    };

    switch (kind) {
      case "memoryLayout": {
        const position =
          persisted.mode === "follow" &&
          activeEditor?.document.languageId === "hlsl"
            ? activeEditor.selection.active
            : new vscode.Position(
                persisted.line ?? 0,
                persisted.character ?? 0,
              );
        memoryLayoutState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshMemoryLayoutCommand,
          html: memoryLayoutErrorHtml(
            "Restoring the tracked memory-layout position…",
          ),
          uri,
          position,
          hasContent: false,
        };
        renderAnalysisPanel(memoryLayoutState);
        panel.onDidDispose(() => {
          if (memoryLayoutState?.panel === panel) {
            ++memoryLayoutGeneration;
            memoryLayoutState = undefined;
          }
        });
        persistAnalysisPanel(context, memoryLayoutState);
        if (targetAvailable) {
          await refreshMemoryLayout(lifecycle, uri, position);
        } else {
          memoryLayoutState.freshness.invalidate(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
          renderAnalysisPanel(memoryLayoutState);
        }
        return;
      }
      case "compilationInfo":
        compilationInfoState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshCompilationInfoCommand,
          html: compilationInfoLoadingHtml(),
          uri,
          hasContent: false,
          lastInfo: undefined,
          lastInfoUri: undefined,
        };
        renderAnalysisPanel(compilationInfoState);
        panel.onDidDispose(() => {
          if (compilationInfoState?.panel === panel) {
            compilationInfoState = undefined;
          }
        });
        persistAnalysisPanel(context, compilationInfoState);
        if (targetAvailable) {
          await refreshCompilationInfo(lifecycle, uri);
        } else {
          compilationInfoState.freshness.invalidate(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
          renderAnalysisPanel(compilationInfoState);
        }
        return;
      case "resourceBindings":
        resourceBindingsState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshResourceBindingsCommand,
          html: resourceBindingsLoadingHtml(),
          uri,
          hasContent: false,
        };
        renderAnalysisPanel(resourceBindingsState);
        panel.onDidDispose(() => {
          if (resourceBindingsState?.panel === panel) {
            resourceBindingsState = undefined;
          }
        });
        persistAnalysisPanel(context, resourceBindingsState);
        if (targetAvailable) {
          await refreshResourceBindings(lifecycle, uri);
        } else {
          resourceBindingsState.freshness.invalidate(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
          renderAnalysisPanel(resourceBindingsState);
        }
        return;
      case "preprocessorExplorer":
        preprocessorExplorerState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshPreprocessorExplorerCommand,
          html: preprocessorExplorerLoadingHtml(),
          uri,
          hasContent: false,
        };
        renderAnalysisPanel(preprocessorExplorerState);
        panel.onDidDispose(() => {
          if (preprocessorExplorerState?.panel === panel) {
            preprocessorExplorerState = undefined;
          }
        });
        persistAnalysisPanel(context, preprocessorExplorerState);
        if (targetAvailable) {
          await refreshPreprocessorExplorer(lifecycle, uri);
        } else {
          preprocessorExplorerState.freshness.invalidate(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
          renderAnalysisPanel(preprocessorExplorerState);
        }
        return;
      case "entryPointDataFlow": {
        const freshness = new AnalysisFreshness();
        const controller = createEntryPointDataFlowController(
          lifecycle,
          panel,
          freshness,
        );
        entryPointDataFlowState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          uri,
          panel,
          controller,
          freshness,
          refreshCommand: refreshEntryPointDataFlowCommand,
          html: entryPointDataFlowLoadingHtml(),
        };
        controller.open(uri.toString());
        renderAnalysisPanel(entryPointDataFlowState);
        panel.onDidDispose(() => {
          if (entryPointDataFlowState?.panel === panel) {
            entryPointDataFlowState.controller.dispose();
            entryPointDataFlowState = undefined;
          }
        });
        persistAnalysisPanel(context, entryPointDataFlowState);
        if (targetAvailable) {
          await controller.refresh(uri.toString());
        } else {
          controller.markStale(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
        }
        return;
      }
      case "computeVisualization": {
        const freshness = new AnalysisFreshness();
        const controller = createComputeVisualizationController(
          lifecycle,
          panel,
          freshness,
        );
        computeVisualizationState = {
          kind,
          trackingMode: persisted.mode,
          targetAvailable,
          uri,
          panel,
          controller,
          freshness,
          refreshCommand: refreshComputeVisualizationCommand,
          html: computeVisualizationLoadingHtml(),
          options: persisted.options ?? {},
        };
        controller.open(uri.toString());
        renderAnalysisPanel(computeVisualizationState);
        panel.onDidDispose(() => {
          if (computeVisualizationState?.panel === panel) {
            computeVisualizationState.controller.dispose();
            computeVisualizationState = undefined;
          }
        });
        persistAnalysisPanel(context, computeVisualizationState);
        if (targetAvailable) {
          await controller.refresh(uri.toString());
        } else {
          controller.markStale(
            persisted.mode === "follow"
              ? "Active shader unavailable"
              : "Tracked shader closed",
          );
        }
      }
    }
  };

  const serializerKinds: readonly (readonly [string, AnalysisPanelKind])[] = [
    ["hlslMemoryLayout", "memoryLayout"],
    ["hlslCompilationInfo", "compilationInfo"],
    ["hlslResourceBindings", "resourceBindings"],
    ["hlslPreprocessorExplorer", "preprocessorExplorer"],
    ["hlslEntryPointDataFlow", "entryPointDataFlow"],
    ["hlslComputeVisualization", "computeVisualization"],
  ];
  for (const [viewType, kind] of serializerKinds) {
    context.subscriptions.push(
      vscode.window.registerWebviewPanelSerializer(viewType, {
        deserializeWebviewPanel: (panel) => restoreAnalysisPanel(kind, panel),
      }),
    );
  }

  context.subscriptions.push(
    vscode.commands.registerCommand("hlsl.restartServer", restart),
    vscode.commands.registerCommand("hlsl.stopServer", async () => {
      cancelPendingAnalysisRefreshes();
      invalidateOpenAnalysisPanels("Disconnected server");
      await lifecycle.stop();
      outputChannel.appendLine("Language server stopped.");
    }),
    vscode.commands.registerCommand("hlsl.showOutput", () => {
      outputChannel.show(true);
    }),
    vscode.commands.registerCommand("hlsl.showDiagnostics", async () => {
      outputChannel.appendLine("--- HLSL-LSP client diagnostics ---");
      outputChannel.appendLine(
        `Platform: ${process.platform}-${process.arch}; VS Code: ${vscode.version}`,
      );
      outputChannel.appendLine(`Lifecycle: ${lifecycle.state}`);
      try {
        const runtime = await resolveServerRuntime(
          configuration().get<string>("server.path"),
          runtimeEnvironment(context),
        );
        outputChannel.appendLine(
          `Runtime: ${runtime.source}; executable: ${runtime.command}`,
        );
        for (const runtimeFile of runtime.runtimeFiles) {
          outputChannel.appendLine(`Runtime file: ${runtimeFile}`);
        }
      } catch (error) {
        outputChannel.appendLine(`Runtime error: ${errorMessage(error)}`);
      }
      try {
        const configuredRuntime = configuration().get<string>(
          "dxcRuntimeDirectory",
        );
        const selectedRuntime =
          configuredRuntime && configuredRuntime.trim() !== ""
            ? configuredRuntime
            : workspaceRuntimeDirectory;
        const dxcRuntime = await resolveDxcRuntimeDirectory(
          selectedRuntime,
          runtimeEnvironment(context),
        );
        outputChannel.appendLine(
          `Selected DXC runtime: ${dxcRuntime ? dxcRuntime.directory : "bundled default"}`,
        );
      } catch (error) {
        outputChannel.appendLine(`DXC runtime error: ${errorMessage(error)}`);
      }
      const activeRuntime = await lifecycle
        .withClient((client) => client.dxcRuntime())
        .catch((error: unknown) => {
          outputChannel.appendLine(
            `Active DXC runtime error: ${errorMessage(error)}`,
          );
          return undefined;
        });
      if (activeRuntime !== null && activeRuntime !== undefined) {
        outputChannel.appendLine(
          `Active DXC runtime: ${activeRuntime.source}; version ${activeRuntime.version}` +
            (activeRuntime.libraryPath
              ? `; ${activeRuntime.libraryPath}`
              : "") +
            (activeRuntime.requiresRestart ? "; restart pending" : ""),
        );
      }
      outputChannel.appendLine(
        `Editor overrides: ${JSON.stringify(readServerSettings(configuration()))}`,
      );
      outputChannel.show(true);
    }),
    vscode.commands.registerCommand(
      setAnalysisTrackingModeCommand,
      async (rawArgument: unknown) => {
        const command = parseAnalysisTrackingCommand(rawArgument);
        if (command === undefined || !isAnalysisPanelKind(command.panel)) {
          return;
        }
        const state = analysisPanelState(command.panel);
        if (state === undefined) {
          return;
        }
        state.trackingMode = command.mode;
        persistAnalysisPanel(context, state);
        renderAnalysisPanel(state);
        if (command.mode === "follow") {
          await followActiveShader(
            context,
            lifecycle,
            vscode.window.activeTextEditor,
            command.panel,
          );
        }
      },
    ),
    vscode.commands.registerCommand(
      "hlsl.showMemoryLayout",
      async (commandArgument: unknown) => {
        const editor = vscode.window.activeTextEditor;
        const target = memoryLayoutTarget(commandArgument);
        let uri: vscode.Uri;
        let position: vscode.Position;
        if (target === undefined) {
          if (editor?.document.languageId !== "hlsl") {
            await vscode.window.showInformationMessage(
              "Open an HLSL document and place the caret on a type, cbuffer, or member.",
            );
            return;
          }
          uri = editor.document.uri;
          position = editor.selection.active;
        } else {
          uri = vscode.Uri.parse(target.textDocument.uri);
          position = new vscode.Position(
            target.position.line,
            target.position.character,
          );
        }
        if (memoryLayoutState === undefined) {
          const generation = ++memoryLayoutGeneration;
          const layout = await lifecycle.withClient((client) =>
            client.memoryLayout(uri, position),
          );
          if (generation !== memoryLayoutGeneration) {
            return;
          }
          if (layout === null || layout === undefined) {
            await vscode.window.showInformationMessage(
              "No supported HLSL memory layout is available at the caret.",
            );
            return;
          }
          const panel = vscode.window.createWebviewPanel(
            "hlslMemoryLayout",
            `Memory Layout: ${layout.name || layout.type}`,
            vscode.ViewColumn.Beside,
            {
              enableScripts: false,
              enableCommandUris: analysisPanelCommandUris("memoryLayout"),
            },
          );
          memoryLayoutState = {
            kind: "memoryLayout",
            trackingMode: defaultAnalysisTrackingMode,
            targetAvailable: true,
            panel,
            freshness: new AnalysisFreshness(),
            refreshCommand: refreshMemoryLayoutCommand,
            html: memoryLayoutHtml(layout),
            uri,
            position,
            hasContent: true,
          };
          const initialGeneration =
            memoryLayoutState.freshness.beginRefresh("Manual refresh");
          memoryLayoutState.freshness.succeed(initialGeneration);
          persistAnalysisPanel(context, memoryLayoutState);
          renderAnalysisPanel(memoryLayoutState);
          panel.onDidDispose(() => {
            if (memoryLayoutState?.panel === panel) {
              ++memoryLayoutGeneration;
              if (memoryLayoutDebounce !== undefined) {
                clearTimeout(memoryLayoutDebounce);
                memoryLayoutDebounce = undefined;
              }
              memoryLayoutState = undefined;
            }
          });
          return;
        } else {
          memoryLayoutState.uri = uri;
          memoryLayoutState.position = position;
          memoryLayoutState.targetAvailable = true;
          persistAnalysisPanel(context, memoryLayoutState);
          memoryLayoutState.panel.reveal(vscode.ViewColumn.Beside);
        }
        if (memoryLayoutState.uri.toString() === uri.toString()) {
          await refreshMemoryLayout(lifecycle, uri, position);
        }
      },
    ),
    vscode.commands.registerCommand("hlsl.showCompilationInfo", async () => {
      const editor = vscode.window.activeTextEditor;
      if (editor?.document.languageId !== "hlsl") {
        await vscode.window.showInformationMessage(
          "Open an HLSL document to inspect its shader compilation.",
        );
        return;
      }
      const uri = editor.document.uri;
      if (compilationInfoState !== undefined) {
        compilationInfoState.uri = uri;
        compilationInfoState.targetAvailable = true;
        persistAnalysisPanel(context, compilationInfoState);
        compilationInfoState.panel.reveal(vscode.ViewColumn.Beside);
      } else {
        const panel = vscode.window.createWebviewPanel(
          "hlslCompilationInfo",
          "Shader Compilation",
          vscode.ViewColumn.Beside,
          {
            enableScripts: false,
            // No script execution is used for Copy/Save: the panel's
            // action links go through plain `command:` URIs, and this
            // allowlists only Copy, Save, and this panel's Refresh -- never
            // `true` (which would let static HTML trigger arbitrary
            // commands).
            enableCommandUris: analysisPanelCommandUris("compilationInfo"),
          },
        );
        panel.onDidDispose(() => {
          if (compilationInfoState?.panel === panel) {
            compilationInfoState = undefined;
          }
        });
        compilationInfoState = {
          kind: "compilationInfo",
          trackingMode: defaultAnalysisTrackingMode,
          targetAvailable: true,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshCompilationInfoCommand,
          html: compilationInfoLoadingHtml(),
          uri,
          hasContent: false,
          lastInfo: undefined,
          lastInfoUri: undefined,
        };
        persistAnalysisPanel(context, compilationInfoState);
        renderAnalysisPanel(compilationInfoState);
      }
      await refreshCompilationInfo(lifecycle, uri);
    }),
    vscode.commands.registerCommand(copyDisassemblyCommand, async () => {
      const disassembly = compilationInfoState?.lastInfo?.disassembly;
      if (
        disassembly === null ||
        disassembly === undefined ||
        !disassembly.available ||
        disassembly.text === ""
      ) {
        await vscode.window.showInformationMessage(
          "No disassembly is available to copy for the current shader compilation.",
        );
        return;
      }
      await vscode.env.clipboard.writeText(disassembly.text);
      await vscode.window.showInformationMessage(
        "Disassembly copied to the clipboard.",
      );
    }),
    vscode.commands.registerCommand(saveDisassemblyCommand, async () => {
      const state = compilationInfoState;
      const disassembly = state?.lastInfo?.disassembly;
      if (
        state === undefined ||
        disassembly === null ||
        disassembly === undefined ||
        !disassembly.available ||
        disassembly.text === ""
      ) {
        await vscode.window.showInformationMessage(
          "No disassembly is available to save for the current shader compilation.",
        );
        return;
      }
      const suggestedName = disassemblyFileName(
        (state.lastInfoUri ?? state.uri).path,
        disassembly.format,
      );
      const defaultUri = vscode.Uri.joinPath(
        state.lastInfoUri ?? state.uri,
        "..",
        suggestedName,
      );
      const filters =
        disassembly.format === "spirv"
          ? { "SPIR-V Assembly": ["spvasm"] }
          : { "DXIL Disassembly": ["ll"] };
      const target = await vscode.window.showSaveDialog({
        defaultUri,
        filters,
      });
      if (target === undefined) {
        return;
      }
      try {
        await vscode.workspace.fs.writeFile(
          target,
          Buffer.from(disassembly.text, "utf8"),
        );
      } catch (error) {
        await vscode.window.showErrorMessage(
          `Unable to save disassembly: ${errorMessage(error)}`,
        );
      }
    }),
    vscode.commands.registerCommand("hlsl.showResourceBindings", async () => {
      const editor = vscode.window.activeTextEditor;
      if (editor?.document.languageId !== "hlsl") {
        await vscode.window.showInformationMessage(
          "Open an HLSL document to inspect its resource bindings.",
        );
        return;
      }
      const uri = editor.document.uri;
      if (resourceBindingsState !== undefined) {
        resourceBindingsState.uri = uri;
        resourceBindingsState.targetAvailable = true;
        persistAnalysisPanel(context, resourceBindingsState);
        resourceBindingsState.panel.reveal(vscode.ViewColumn.Beside);
      } else {
        const panel = vscode.window.createWebviewPanel(
          "hlslResourceBindings",
          "Resource Bindings",
          vscode.ViewColumn.Beside,
          {
            enableScripts: false,
            // No script execution is used for navigation: resource/collision
            // labels link through plain `command:` URIs, and this allowlists
            // only navigation and this panel's Refresh -- never `true` (which
            // would let static HTML trigger arbitrary commands).
            enableCommandUris: analysisPanelCommandUris("resourceBindings"),
          },
        );
        panel.onDidDispose(() => {
          if (resourceBindingsState?.panel === panel) {
            resourceBindingsState = undefined;
          }
        });
        resourceBindingsState = {
          kind: "resourceBindings",
          trackingMode: defaultAnalysisTrackingMode,
          targetAvailable: true,
          panel,
          freshness: new AnalysisFreshness(),
          refreshCommand: refreshResourceBindingsCommand,
          html: resourceBindingsLoadingHtml(),
          uri,
          hasContent: false,
        };
        persistAnalysisPanel(context, resourceBindingsState);
        renderAnalysisPanel(resourceBindingsState);
      }
      await refreshResourceBindings(lifecycle, uri);
    }),
    vscode.commands.registerCommand(
      openResourceLocationCommand,
      async (rawArgument: unknown) => {
        const location = parseResourceLocationCommandArg(rawArgument);
        if (location === undefined) {
          await vscode.window.showErrorMessage(
            "Unable to navigate: the resource location was missing or malformed.",
          );
          return;
        }
        let targetUri: vscode.Uri;
        try {
          targetUri = vscode.Uri.parse(location.uri, true);
        } catch {
          await vscode.window.showErrorMessage(
            "Unable to navigate: the resource location's URI could not be parsed.",
          );
          return;
        }
        const range = new vscode.Range(
          new vscode.Position(
            location.range.start.line,
            location.range.start.character,
          ),
          new vscode.Position(
            location.range.end.line,
            location.range.end.character,
          ),
        );
        try {
          const document = await vscode.workspace.openTextDocument(targetUri);
          const editor = await vscode.window.showTextDocument(document, {
            preserveFocus: false,
            selection: range,
          });
          editor.revealRange(
            range,
            vscode.TextEditorRevealType.InCenterIfOutsideViewport,
          );
        } catch (error) {
          await vscode.window.showErrorMessage(
            `Unable to navigate to the resource declaration: ${
              error instanceof Error ? error.message : String(error)
            }`,
          );
        }
      },
    ),
    vscode.commands.registerCommand(
      "hlsl.showPreprocessorExplorer",
      async () => {
        const editor = vscode.window.activeTextEditor;
        if (editor?.document.languageId !== "hlsl") {
          await vscode.window.showInformationMessage(
            "Open an HLSL document to inspect its preprocessor state.",
          );
          return;
        }
        const uri = editor.document.uri;
        if (preprocessorExplorerState !== undefined) {
          preprocessorExplorerState.uri = uri;
          preprocessorExplorerState.targetAvailable = true;
          persistAnalysisPanel(context, preprocessorExplorerState);
          preprocessorExplorerState.panel.reveal(vscode.ViewColumn.Beside);
        } else {
          const panel = vscode.window.createWebviewPanel(
            "hlslPreprocessorExplorer",
            "Preprocessor Explorer",
            vscode.ViewColumn.Beside,
            {
              enableScripts: false,
              // No script execution is used for navigation: file/include/
              // macro/skipped-region links go through plain `command:`
              // URIs, and this allowlists only navigation and this panel's
              // Refresh -- never `true` (which would let static HTML trigger
              // arbitrary commands).
              enableCommandUris: analysisPanelCommandUris(
                "preprocessorExplorer",
              ),
            },
          );
          panel.onDidDispose(() => {
            if (preprocessorExplorerState?.panel === panel) {
              preprocessorExplorerState = undefined;
            }
          });
          preprocessorExplorerState = {
            kind: "preprocessorExplorer",
            trackingMode: defaultAnalysisTrackingMode,
            targetAvailable: true,
            panel,
            freshness: new AnalysisFreshness(),
            refreshCommand: refreshPreprocessorExplorerCommand,
            html: preprocessorExplorerLoadingHtml(),
            uri,
            hasContent: false,
          };
          persistAnalysisPanel(context, preprocessorExplorerState);
          renderAnalysisPanel(preprocessorExplorerState);
        }
        await refreshPreprocessorExplorer(lifecycle, uri);
      },
    ),
    vscode.commands.registerCommand(
      openPreprocessorLocationCommand,
      async (rawArgument: unknown) => {
        const location = parsePreprocessorLocationCommandArg(rawArgument);
        if (location === undefined) {
          await vscode.window.showErrorMessage(
            "Unable to navigate: the preprocessor location was missing or malformed.",
          );
          return;
        }
        let targetUri: vscode.Uri;
        try {
          targetUri = vscode.Uri.parse(location.uri, true);
        } catch {
          await vscode.window.showErrorMessage(
            "Unable to navigate: the preprocessor location's URI could not be parsed.",
          );
          return;
        }
        const range = new vscode.Range(
          new vscode.Position(
            location.range.start.line,
            location.range.start.character,
          ),
          new vscode.Position(
            location.range.end.line,
            location.range.end.character,
          ),
        );
        try {
          const document = await vscode.workspace.openTextDocument(targetUri);
          const editor = await vscode.window.showTextDocument(document, {
            preserveFocus: false,
            selection: range,
          });
          editor.revealRange(
            range,
            vscode.TextEditorRevealType.InCenterIfOutsideViewport,
          );
        } catch (error) {
          await vscode.window.showErrorMessage(
            `Unable to navigate to the preprocessor location: ${
              error instanceof Error ? error.message : String(error)
            }`,
          );
        }
      },
    ),
    vscode.commands.registerCommand("hlsl.showEntryPointDataFlow", async () => {
      const editor = vscode.window.activeTextEditor;
      if (editor?.document.languageId !== "hlsl") {
        await vscode.window.showInformationMessage(
          "Open an HLSL document to trace its entry-point data flow.",
        );
        return;
      }
      const uri = editor.document.uri;
      if (entryPointDataFlowState !== undefined) {
        const { panel, controller } = entryPointDataFlowState;
        controller.open(uri.toString());
        entryPointDataFlowState.uri = uri;
        entryPointDataFlowState.targetAvailable = true;
        persistAnalysisPanel(context, entryPointDataFlowState);
        panel.reveal(vscode.ViewColumn.Beside);
      } else {
        const panel = vscode.window.createWebviewPanel(
          "hlslEntryPointDataFlow",
          "Entry-Point Data Flow",
          vscode.ViewColumn.Beside,
          {
            enableScripts: false,
            // No script execution is used for navigation: entry
            // point/function/global-access links go through plain
            // `command:` URIs, and this allowlists only navigation and this
            // panel's Refresh -- never `true` (which would let static
            // HTML trigger arbitrary commands).
            enableCommandUris: analysisPanelCommandUris("entryPointDataFlow"),
          },
        );
        const freshness = new AnalysisFreshness();
        const controller = createEntryPointDataFlowController(
          lifecycle,
          panel,
          freshness,
        );
        const viewState: EntryPointDataFlowViewState = {
          kind: "entryPointDataFlow",
          trackingMode: defaultAnalysisTrackingMode,
          targetAvailable: true,
          uri,
          panel,
          controller,
          freshness,
          refreshCommand: refreshEntryPointDataFlowCommand,
          html: entryPointDataFlowLoadingHtml(),
        };
        persistAnalysisPanel(context, viewState);
        renderAnalysisPanel(viewState);
        controller.open(uri.toString());
        panel.onDidDispose(() => {
          if (entryPointDataFlowState?.panel === panel) {
            entryPointDataFlowState.controller.dispose();
            entryPointDataFlowState = undefined;
          }
        });
        entryPointDataFlowState = viewState;
      }
      await entryPointDataFlowState.controller.refresh(uri.toString());
    }),
    vscode.commands.registerCommand(
      openEntryPointDataFlowLocationCommand,
      async (rawArgument: unknown) => {
        const navigator: EntryPointDataFlowNavigator = {
          open: async (location) => {
            const targetUri = vscode.Uri.parse(location.uri, true);
            const range = new vscode.Range(
              new vscode.Position(
                location.range.start.line,
                location.range.start.character,
              ),
              new vscode.Position(
                location.range.end.line,
                location.range.end.character,
              ),
            );
            const document = await vscode.workspace.openTextDocument(targetUri);
            const editor = await vscode.window.showTextDocument(document, {
              preserveFocus: false,
              selection: range,
            });
            editor.revealRange(
              range,
              vscode.TextEditorRevealType.InCenterIfOutsideViewport,
            );
          },
        };
        const result = await navigateEntryPointDataFlowLocation(
          rawArgument,
          navigator,
        );
        if (!result.success && result.errorMessage !== undefined) {
          await vscode.window.showErrorMessage(result.errorMessage);
        }
      },
    ),
    vscode.commands.registerCommand(
      "hlsl.showComputeVisualization",
      async () => {
        const editor = vscode.window.activeTextEditor;
        if (editor?.document.languageId !== "hlsl") {
          await vscode.window.showInformationMessage(
            "Open an HLSL compute shader to visualize its dispatch.",
          );
          return;
        }
        const uri = editor.document.uri;
        if (computeVisualizationState !== undefined) {
          const { panel, controller } = computeVisualizationState;
          controller.open(uri.toString());
          computeVisualizationState.uri = uri;
          computeVisualizationState.targetAvailable = true;
          persistAnalysisPanel(context, computeVisualizationState);
          panel.reveal(vscode.ViewColumn.Beside);
        } else {
          const panel = vscode.window.createWebviewPanel(
            "hlslComputeVisualization",
            "Compute Visualization",
            vscode.ViewColumn.Beside,
            {
              enableScripts: false,
              enableCommandUris: analysisPanelCommandUris(
                "computeVisualization",
              ),
            },
          );
          const freshness = new AnalysisFreshness();
          const controller = createComputeVisualizationController(
            lifecycle,
            panel,
            freshness,
          );
          const viewState: ComputeVisualizationViewState = {
            kind: "computeVisualization",
            trackingMode: defaultAnalysisTrackingMode,
            targetAvailable: true,
            uri,
            panel,
            controller,
            freshness,
            refreshCommand: refreshComputeVisualizationCommand,
            html: computeVisualizationLoadingHtml(),
            options: {},
          };
          persistAnalysisPanel(context, viewState);
          renderAnalysisPanel(viewState);
          controller.open(uri.toString());
          panel.onDidDispose(() => {
            if (computeVisualizationState?.panel === panel) {
              computeVisualizationState.controller.dispose();
              computeVisualizationState = undefined;
            }
          });
          computeVisualizationState = viewState;
        }
        await computeVisualizationState.controller.refresh(uri.toString());
      },
    ),
    vscode.commands.registerCommand(
      configureComputeVisualizationCommand,
      async () => {
        const state = computeVisualizationState;
        if (state === undefined) {
          return;
        }
        const options = await configureComputeVisualization(state.options);
        if (options === null || computeVisualizationState !== state) {
          return;
        }
        state.options = options;
        persistAnalysisPanel(context, state);
        if (state.targetAvailable) {
          await state.controller.refreshTracked("Configuration change");
        }
      },
    ),
    vscode.commands.registerCommand(
      openComputeVisualizationLocationCommand,
      async (rawArgument: unknown) => {
        const location = parseComputeVisualizationLocation(rawArgument);
        if (location === undefined) {
          await vscode.window.showErrorMessage(
            "Unable to navigate: the compute-analysis location is invalid.",
          );
          return;
        }
        try {
          const targetUri = vscode.Uri.parse(location.uri, true);
          const range = new vscode.Range(
            new vscode.Position(
              location.range.start.line,
              location.range.start.character,
            ),
            new vscode.Position(
              location.range.end.line,
              location.range.end.character,
            ),
          );
          const document = await vscode.workspace.openTextDocument(targetUri);
          const targetEditor = await vscode.window.showTextDocument(document, {
            preserveFocus: false,
            selection: range,
          });
          targetEditor.revealRange(
            range,
            vscode.TextEditorRevealType.InCenterIfOutsideViewport,
          );
        } catch (error) {
          await vscode.window.showErrorMessage(
            `Unable to navigate to the compute-analysis location: ${
              error instanceof Error ? error.message : String(error)
            }`,
          );
        }
      },
    ),
    vscode.commands.registerCommand(refreshMemoryLayoutCommand, async () => {
      const state = memoryLayoutState;
      if (state?.position !== undefined) {
        await refreshMemoryLayout(
          lifecycle,
          state.uri,
          state.position,
          "Manual refresh",
        );
      }
    }),
    vscode.commands.registerCommand(refreshCompilationInfoCommand, async () => {
      const state = compilationInfoState;
      if (state !== undefined) {
        await refreshCompilationInfo(lifecycle, state.uri, "Manual refresh");
      }
    }),
    vscode.commands.registerCommand(
      refreshResourceBindingsCommand,
      async () => {
        const state = resourceBindingsState;
        if (state !== undefined) {
          await refreshResourceBindings(lifecycle, state.uri, "Manual refresh");
        }
      },
    ),
    vscode.commands.registerCommand(
      refreshPreprocessorExplorerCommand,
      async () => {
        const state = preprocessorExplorerState;
        if (state !== undefined) {
          await refreshPreprocessorExplorer(
            lifecycle,
            state.uri,
            "Manual refresh",
          );
        }
      },
    ),
    vscode.commands.registerCommand(
      refreshEntryPointDataFlowCommand,
      async () => {
        if (entryPointDataFlowState?.targetAvailable) {
          await entryPointDataFlowState.controller.refreshTracked(
            "Manual refresh",
          );
        }
      },
    ),
    vscode.commands.registerCommand(
      refreshComputeVisualizationCommand,
      async () => {
        if (computeVisualizationState?.targetAvailable) {
          await computeVisualizationState.controller.refreshTracked(
            "Manual refresh",
          );
        }
      },
    ),
    vscode.commands.registerCommand("hlsl.selectVariant", async () => {
      const editor = vscode.window.activeTextEditor;
      if (editor?.document.languageId !== "hlsl") {
        await vscode.window.showInformationMessage(
          "Open an HLSL document to select an applicable shader variant.",
        );
        return;
      }
      const documentUri = editor.document.uri;
      let list: VariantList | null | undefined;
      try {
        list = await lifecycle.withClient((client) =>
          client.variants(documentUri),
        );
      } catch (error) {
        await reportError(
          outputChannel,
          "Unable to list HLSL shader variants",
          error,
        );
        return;
      }
      if (list === undefined || list === null) {
        await vscode.window.showInformationMessage(
          "The HLSL language server is not running.",
        );
        return;
      }
      if (list.variants.length === 0) {
        await vscode.window.showInformationMessage(
          "No shader variants are declared under hlsl.variants in shadertoolsconfig.json.",
        );
        return;
      }
      const applicable = applicableVariants(list.variants);
      if (applicable.length === 0) {
        await vscode.window.showInformationMessage(
          "No declared shader variants apply to the active HLSL document.",
        );
        return;
      }
      const active = list.activeVariant ?? "";
      const items: (vscode.QuickPickItem & { value: string | null })[] = [
        {
          label: "$(circle-slash) No variant",
          description: active === "" ? "current" : "",
          value: null,
        },
      ];
      for (const variant of applicable) {
        const notes: string[] = [];
        if (variant.name === active) {
          notes.push("current");
        }
        const item: vscode.QuickPickItem & { value: string | null } = {
          label: variant.name,
          description: notes.join(", "),
          value: variant.name,
        };
        if (variant.description !== "") {
          item.detail = variant.description;
        }
        items.push(item);
      }
      const selection = await vscode.window.showQuickPick(items, {
        title: "Select HLSL Shader Variant",
        placeHolder: "Choose the active shader compilation variant",
      });
      if (selection === undefined) {
        return;
      }
      const target =
        vscode.workspace.workspaceFolders &&
        vscode.workspace.workspaceFolders.length > 0
          ? vscode.ConfigurationTarget.Workspace
          : vscode.ConfigurationTarget.Global;
      try {
        await configuration().update(
          "activeVariant",
          selection.value ?? "",
          target,
        );
      } catch (error) {
        await reportError(
          outputChannel,
          "Unable to update the active HLSL shader variant",
          error,
        );
      }
    }),
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      void updateVariantStatus();
      void followActiveShader(context, lifecycle, editor);
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      const resource = configurationResource();
      if (!event.affectsConfiguration("hlsl", resource)) {
        return;
      }
      const analysisChange = analysisConfigurationChange((setting) =>
        event.affectsConfiguration(setting, resource),
      );
      if (analysisChange !== undefined) {
        invalidateOpenAnalysisPanels(analysisChange.cause);
      }
      if (analysisChange?.action === "restart") {
        if (event.affectsConfiguration("hlsl.dxcRuntimeDirectory", resource)) {
          // An explicit editor runtime supersedes any workspace-driven selection.
          workspaceRuntimeDirectory = undefined;
        }
        await restart();
        return;
      }
      try {
        await lifecycle.withClient(async (client) => {
          await client.configurationChanged();
        });
      } catch (error) {
        await reportError(
          outputChannel,
          "Unable to update HLSL language server settings",
          error,
        );
      }
      await updateVariantStatus();
      if (analysisChange?.action === "refresh") {
        await refreshAllOpenAnalysisPanels(lifecycle, analysisChange.cause);
      }
    }),
    vscode.workspace.onDidChangeWorkspaceFolders(restart),
    vscode.workspace.onDidSaveTextDocument((document) => {
      // Conservative: every panel's analysis covers its root document's
      // current snapshot *plus* any #include'd file open elsewhere in the
      // workspace (see docs/call-hierarchy.md's "Includes and unsaved
      // edits"), and the client has no dependency query telling it which
      // open document a given root actually includes. So saving *any*
      // open HLSL document refreshes every currently open analysis panel,
      // not only one whose own tracked root exactly matches the saved
      // document.
      if (document.languageId !== "hlsl") {
        return;
      }
      invalidateOpenAnalysisPanels("Source edit", true);
      void refreshAllOpenAnalysisPanels(lifecycle, "Source edit");
      effectiveContextStatusDebouncer?.schedule();
    }),
    vscode.workspace.onDidOpenTextDocument((document) => {
      if (document.languageId === "hlsl") {
        void restoreOpenedAnalysisTargets(context, lifecycle, document);
      }
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      const closedUri = document.uri.toString();
      for (const state of openAnalysisPanelStates()) {
        if (state.uri.toString() !== closedUri) {
          continue;
        }
        markAnalysisTargetUnavailable(state, "Tracked shader closed");
      }
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      // Same conservative "any open HLSL document" trigger as the save
      // handler above, but debounced per panel so a burst of keystrokes
      // triggers one request per panel, not a storm of them.
      if (event.document.languageId !== "hlsl") {
        return;
      }
      ++memoryLayoutGeneration;
      if (
        vscode.window.activeTextEditor?.document.uri.toString() ===
        event.document.uri.toString()
      ) {
        effectiveContextStatusDebouncer?.schedule();
      }
      if (memoryLayoutState?.targetAvailable) {
        if (memoryLayoutDebounce !== undefined) {
          clearTimeout(memoryLayoutDebounce);
        }
        const update = updateMemoryLayoutForDocumentChange(
          memoryLayoutState.uri.toString(),
          event.document.uri.toString(),
          memoryLayoutState.position,
          event.contentChanges,
        );
        if (update.invalidated) {
          memoryLayoutState.freshness.invalidate("Source edit");
          renderAnalysisPanel(memoryLayoutState);
          memoryLayoutDebounce = undefined;
        } else if (update.position !== undefined) {
          memoryLayoutState.position = new vscode.Position(
            update.position.line,
            update.position.character,
          );
          persistAnalysisPanel(context, memoryLayoutState);
          if (update.shouldRefresh) {
            memoryLayoutState.freshness.invalidate("Source edit", true);
            renderAnalysisPanel(memoryLayoutState);
            memoryLayoutDebounce = setTimeout(() => {
              if (memoryLayoutState?.position !== undefined) {
                void refreshMemoryLayout(
                  lifecycle,
                  memoryLayoutState.uri,
                  memoryLayoutState.position,
                  "Source edit",
                );
              }
            }, 500);
          }
        }
      }
      if (compilationInfoState?.targetAvailable) {
        compilationInfoState.freshness.invalidate("Source edit", true);
        renderAnalysisPanel(compilationInfoState);
        if (compilationInfoDebounce !== undefined) {
          clearTimeout(compilationInfoDebounce);
        }
        compilationInfoDebounce = setTimeout(() => {
          if (compilationInfoState !== undefined) {
            void refreshCompilationInfo(
              lifecycle,
              compilationInfoState.uri,
              "Source edit",
            );
          }
        }, 500);
      }
      if (resourceBindingsState?.targetAvailable) {
        resourceBindingsState.freshness.invalidate("Source edit", true);
        renderAnalysisPanel(resourceBindingsState);
        if (resourceBindingsDebounce !== undefined) {
          clearTimeout(resourceBindingsDebounce);
        }
        resourceBindingsDebounce = setTimeout(() => {
          if (resourceBindingsState !== undefined) {
            void refreshResourceBindings(
              lifecycle,
              resourceBindingsState.uri,
              "Source edit",
            );
          }
        }, 500);
      }
      if (preprocessorExplorerState?.targetAvailable) {
        preprocessorExplorerState.freshness.invalidate("Source edit", true);
        renderAnalysisPanel(preprocessorExplorerState);
        if (preprocessorExplorerDebounce !== undefined) {
          clearTimeout(preprocessorExplorerDebounce);
        }
        preprocessorExplorerDebounce = setTimeout(() => {
          if (preprocessorExplorerState !== undefined) {
            void refreshPreprocessorExplorer(
              lifecycle,
              preprocessorExplorerState.uri,
              "Source edit",
            );
          }
        }, 500);
      }
      if (entryPointDataFlowState?.targetAvailable) {
        entryPointDataFlowState.controller.scheduleDebouncedRefresh();
      }
      if (computeVisualizationState?.targetAvailable) {
        computeVisualizationState.controller.scheduleDebouncedRefresh();
      }
    }),
    {
      dispose(): void {
        void lifecycle.stop();
      },
    },
  );

  try {
    await lifecycle.start();
  } catch (error) {
    void reportError(
      outputChannel,
      "Unable to activate the HLSL language server",
      error,
    ).catch((reportingError: unknown) => {
      outputChannel.appendLine(
        `[error] Unable to report activation failure: ${errorMessage(reportingError)}`,
      );
    });
  }
  await updateVariantStatus();

  return {
    get state(): LifecycleState {
      return lifecycle.state;
    },
    requestCompilationInfo(uri: vscode.Uri): Promise<CompilationInfo | null> {
      return lifecycle
        .withClient((client) => client.compilationInfo(uri))
        .then((info) => info ?? null);
    },
  };
}

export async function deactivate(): Promise<void> {
  const lifecycle = activeLifecycle;
  activeLifecycle = undefined;
  cancelPendingAnalysisRefreshes();
  ++memoryLayoutGeneration;
  memoryLayoutState = undefined;
  compilationInfoState = undefined;
  resourceBindingsState = undefined;
  preprocessorExplorerState = undefined;
  entryPointDataFlowState?.controller.dispose();
  entryPointDataFlowState = undefined;
  computeVisualizationState?.controller.dispose();
  computeVisualizationState = undefined;
  watchedFileRefreshDebouncer?.dispose();
  watchedFileRefreshDebouncer = undefined;
  effectiveContextStatusDebouncer?.dispose();
  effectiveContextStatusDebouncer = undefined;
  await lifecycle?.stop();
}
