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
  resolveCompilationInfoRefresh,
} from "./compilationInfo";
import {
  openResourceLocationCommand,
  parseResourceLocationCommandArg,
  resolveResourceBindingsRefresh,
} from "./resourceBindings";
import {
  HlslServerSettings,
  readActiveVariant,
  readDefaultLanguageVersion,
  readServerSettings,
  readTraceSetting,
  TraceSetting,
} from "./configuration";
import { ClientLifecycle, LifecycleClient, LifecycleState } from "./lifecycle";
import { MemoryLayout, memoryLayoutHtml } from "./memoryLayout";
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

const outputName = "HLSL-LSP";

interface ManagedClient extends LifecycleClient {
  readonly runtime: ServerRuntime;
  configurationChanged(): Promise<void>;
  memoryLayout(
    uri: vscode.Uri,
    position: vscode.Position,
  ): Promise<MemoryLayout | null>;
  compilationInfo(uri: vscode.Uri): Promise<CompilationInfo | null>;
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

interface VariantInfo {
  readonly name: string;
  readonly description: string;
  readonly default: boolean;
  readonly applicable: boolean;
}

interface VariantList {
  readonly activeVariant: string | null;
  readonly variants: readonly VariantInfo[];
}

interface RuntimeRestartRequest {
  readonly directory?: string;
  readonly reason?: string;
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

interface CompilationInfoViewState {
  readonly panel: vscode.WebviewPanel;
  uri: vscode.Uri;
  // True once a successful hlsl/compilationInfo result has been rendered for
  // the current uri. A later failed or cancelled refresh must never regress
  // this panel to a placeholder or a perpetual loading state, so this flag
  // decides whether a failure keeps the last content or shows an explicit
  // error instead.
  hasContent: boolean;
}

let compilationInfoState: CompilationInfoViewState | undefined;
let compilationInfoGeneration = 0;
let compilationInfoDebounce: NodeJS.Timeout | undefined;

// Independently tracked from CompilationInfoViewState: the two panels can be
// open for different documents at the same time, and neither refresh path
// may interfere with the other's generation counter or debounce timer.
interface ResourceBindingsViewState {
  readonly panel: vscode.WebviewPanel;
  uri: vscode.Uri;
  hasContent: boolean;
}

let resourceBindingsState: ResourceBindingsViewState | undefined;
let resourceBindingsGeneration = 0;
let resourceBindingsDebounce: NodeJS.Timeout | undefined;

function compilationInfoLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Compiling…</p></body></html>`;
}

function resourceBindingsLoadingHtml(): string {
  return `<!doctype html><html><body style="color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);padding:1rem 1.5rem;"><p>Analyzing resource bindings…</p></body></html>`;
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
): Promise<void> {
  const generation = ++compilationInfoGeneration;
  let info: CompilationInfo | null | undefined;
  let failureMessage: string | undefined;
  try {
    info = await lifecycle.withClient((client) => client.compilationInfo(uri));
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    generation !== compilationInfoGeneration ||
    compilationInfoState?.uri.toString() !== uri.toString()
  ) {
    return;
  }
  const outcome = resolveCompilationInfoRefresh(
    compilationInfoState.hasContent,
    info,
    failureMessage,
  );
  compilationInfoState.hasContent = outcome.hasContent;
  if (outcome.title !== undefined) {
    compilationInfoState.panel.title = outcome.title;
  }
  if (outcome.html !== undefined) {
    compilationInfoState.panel.webview.html = outcome.html;
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
): Promise<void> {
  const generation = ++resourceBindingsGeneration;
  let info: CompilationInfo | null | undefined;
  let failureMessage: string | undefined;
  try {
    info = await lifecycle.withClient((client) => client.compilationInfo(uri));
  } catch (error) {
    failureMessage =
      error instanceof Error ? error.message : "The request failed.";
  }
  if (
    generation !== resourceBindingsGeneration ||
    resourceBindingsState?.uri.toString() !== uri.toString()
  ) {
    return;
  }
  const outcome = resolveResourceBindingsRefresh(
    resourceBindingsState.hasContent,
    info,
    failureMessage,
  );
  resourceBindingsState.hasContent = outcome.hasContent;
  if (outcome.title !== undefined) {
    resourceBindingsState.panel.title = outcome.title;
  }
  if (outcome.html !== undefined) {
    resourceBindingsState.panel.webview.html = outcome.html;
  }
}

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
  private disposed = false;

  public constructor(
    public readonly runtime: ServerRuntime,
    outputChannel: vscode.LogOutputChannel,
    private readonly watchers: readonly vscode.FileSystemWatcher[],
    initialSettings: ClientSettings,
    serverArgs: readonly string[],
    onRuntimeRestartRequired: (request: RuntimeRestartRequest) => void,
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
    this.client.onNotification(
      "hlsl/dxcRuntimeRestartRequired",
      (params: unknown) => {
        onRuntimeRestartRequired(runtimeRestartRequest(params));
      },
    );
    this.settingsSynchronizer = new RunningSettingsSynchronizer<ClientSettings>(
      clientSettings,
      (settings, isCurrentConnection) =>
        this.applySettings(settings, isCurrentConnection),
    );
    this.stateSubscription = this.client.onDidChangeState((event) => {
      const synchronization = this.settingsSynchronizer.stateChanged(
        event.newState === State.Running,
      );
      void synchronization.catch((error: unknown) => {
        outputChannel.appendLine(
          `[error] Unable to synchronize HLSL settings after a language server state change: ${errorMessage(error)}`,
        );
      });
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
      );
    } catch (error) {
      for (const watcher of watchers) {
        watcher.dispose();
      }
      throw error;
    }
  });
  activeLifecycle = lifecycle;

  const updateVariantStatus = async (): Promise<void> => {
    const editor = vscode.window.activeTextEditor;
    if (editor?.document.languageId !== "hlsl") {
      variantStatus.hide();
      return;
    }
    const documentUri = editor.document.uri;
    let list: VariantList | null | undefined;
    try {
      list = await lifecycle.withClient((client) =>
        client.variants(documentUri),
      );
    } catch {
      list = undefined;
    }
    if (list === undefined || list === null || list.variants.length === 0) {
      variantStatus.hide();
      return;
    }
    const active = list.activeVariant ?? "";
    variantStatus.text = `$(versions) HLSL: ${active !== "" ? active : "Default"}`;
    variantStatus.tooltip =
      "Active HLSL shader compilation variant. Click to change.";
    variantStatus.show();
  };

  const restart = async (): Promise<void> => {
    try {
      await lifecycle.restart();
      outputChannel.appendLine("Language server restarted.");
    } catch (error) {
      await reportError(
        outputChannel,
        "Unable to restart the HLSL language server",
        error,
      );
    }
    await updateVariantStatus();
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

  context.subscriptions.push(
    vscode.commands.registerCommand("hlsl.restartServer", restart),
    vscode.commands.registerCommand("hlsl.stopServer", async () => {
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
        const layout = await lifecycle.withClient((client) =>
          client.memoryLayout(uri, position),
        );
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
          { enableScripts: false },
        );
        panel.webview.html = memoryLayoutHtml(layout);
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
        const switchingDocument =
          compilationInfoState.uri.toString() !== uri.toString();
        compilationInfoState.uri = uri;
        if (switchingDocument) {
          // Only the loading placeholder for a different document replaces
          // what is on screen; a same-document refresh keeps showing the
          // last successful content until the new result (or an explicit
          // error, on failure) is ready.
          compilationInfoState.hasContent = false;
          compilationInfoState.panel.webview.html =
            compilationInfoLoadingHtml();
        }
        compilationInfoState.panel.reveal(vscode.ViewColumn.Beside);
      } else {
        const panel = vscode.window.createWebviewPanel(
          "hlslCompilationInfo",
          "Shader Compilation",
          vscode.ViewColumn.Beside,
          { enableScripts: false },
        );
        panel.webview.html = compilationInfoLoadingHtml();
        panel.onDidDispose(() => {
          if (compilationInfoState?.panel === panel) {
            compilationInfoState = undefined;
          }
        });
        compilationInfoState = { panel, uri, hasContent: false };
      }
      await refreshCompilationInfo(lifecycle, uri);
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
        const switchingDocument =
          resourceBindingsState.uri.toString() !== uri.toString();
        resourceBindingsState.uri = uri;
        if (switchingDocument) {
          // Only the loading placeholder for a different document replaces
          // what is on screen; a same-document refresh keeps showing the
          // last successful content until the new result (or an explicit
          // error, on failure) is ready.
          resourceBindingsState.hasContent = false;
          resourceBindingsState.panel.webview.html =
            resourceBindingsLoadingHtml();
        }
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
            // only the one command they may invoke -- never `true` (which
            // would let static HTML trigger arbitrary commands).
            enableCommandUris: [openResourceLocationCommand],
          },
        );
        panel.webview.html = resourceBindingsLoadingHtml();
        panel.onDidDispose(() => {
          if (resourceBindingsState?.panel === panel) {
            resourceBindingsState = undefined;
          }
        });
        resourceBindingsState = { panel, uri, hasContent: false };
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
    vscode.commands.registerCommand("hlsl.selectVariant", async () => {
      const editor = vscode.window.activeTextEditor;
      const documentUri =
        editor?.document.languageId === "hlsl"
          ? editor.document.uri
          : undefined;
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
      const active = list.activeVariant ?? "";
      const items: (vscode.QuickPickItem & { value: string | null })[] = [
        {
          label: "$(circle-slash) No variant",
          description: active === "" ? "current" : "",
          value: null,
        },
      ];
      for (const variant of list.variants) {
        const notes: string[] = [];
        if (variant.name === active) {
          notes.push("current");
        }
        if (!variant.applicable) {
          notes.push("not applicable to this file");
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
    vscode.window.onDidChangeActiveTextEditor(() => {
      void updateVariantStatus();
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      const resource = configurationResource();
      if (!event.affectsConfiguration("hlsl", resource)) {
        return;
      }
      if (event.affectsConfiguration("hlsl.dxcRuntimeDirectory", resource)) {
        // An explicit editor runtime supersedes any workspace-driven selection.
        workspaceRuntimeDirectory = undefined;
        await restart();
        return;
      }
      if (
        event.affectsConfiguration("hlsl.server.path", resource) ||
        event.affectsConfiguration(
          "hlsl.additionalIncludeDirectories",
          resource,
        ) ||
        event.affectsConfiguration("hlsl.virtualDirectoryMappings", resource)
      ) {
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
      if (
        event.affectsConfiguration("hlsl.activeVariant", resource) &&
        compilationInfoState !== undefined
      ) {
        await refreshCompilationInfo(lifecycle, compilationInfoState.uri);
      }
      if (
        event.affectsConfiguration("hlsl.activeVariant", resource) &&
        resourceBindingsState !== undefined
      ) {
        await refreshResourceBindings(lifecycle, resourceBindingsState.uri);
      }
    }),
    vscode.workspace.onDidChangeWorkspaceFolders(restart),
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (document.uri.toString() === compilationInfoState?.uri.toString()) {
        void refreshCompilationInfo(lifecycle, compilationInfoState.uri);
      }
      if (document.uri.toString() === resourceBindingsState?.uri.toString()) {
        void refreshResourceBindings(lifecycle, resourceBindingsState.uri);
      }
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (
        event.document.uri.toString() === compilationInfoState?.uri.toString()
      ) {
        if (compilationInfoDebounce !== undefined) {
          clearTimeout(compilationInfoDebounce);
        }
        // Debounced so a burst of keystrokes triggers one compile, not a
        // storm of hlsl/compilationInfo requests.
        compilationInfoDebounce = setTimeout(() => {
          if (compilationInfoState !== undefined) {
            void refreshCompilationInfo(lifecycle, compilationInfoState.uri);
          }
        }, 500);
      }
      if (
        event.document.uri.toString() === resourceBindingsState?.uri.toString()
      ) {
        if (resourceBindingsDebounce !== undefined) {
          clearTimeout(resourceBindingsDebounce);
        }
        resourceBindingsDebounce = setTimeout(() => {
          if (resourceBindingsState !== undefined) {
            void refreshResourceBindings(lifecycle, resourceBindingsState.uri);
          }
        }, 500);
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
  if (compilationInfoDebounce !== undefined) {
    clearTimeout(compilationInfoDebounce);
    compilationInfoDebounce = undefined;
  }
  compilationInfoState = undefined;
  if (resourceBindingsDebounce !== undefined) {
    clearTimeout(resourceBindingsDebounce);
    resourceBindingsDebounce = undefined;
  }
  resourceBindingsState = undefined;
  await lifecycle?.stop();
}
