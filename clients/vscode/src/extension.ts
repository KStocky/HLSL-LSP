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
  HlslServerSettings,
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
  dxcRuntime(): Promise<DxcRuntimeInfo | null>;
}

export interface HlslExtensionApi {
  readonly state: LifecycleState;
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

interface ClientSettings {
  readonly trace: TraceSetting;
  readonly languageVersion: string;
  readonly server: HlslServerSettings;
}

interface MemoryLayoutTarget {
  readonly textDocument: { readonly uri: string };
  readonly position: { readonly line: number; readonly character: number };
}

let activeLifecycle: ClientLifecycle<ManagedClient> | undefined;

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

  public dxcRuntime(): Promise<DxcRuntimeInfo | null> {
    return this.client.sendRequest<DxcRuntimeInfo | null>(
      "hlsl/dxcRuntime",
      {},
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

  let workspaceRuntimeDirectory: string | undefined;

  const lifecycle = new ClientLifecycle<ManagedClient>(async () => {
    const reader = configuration();
    const configuredPath = reader.get<string>("server.path");
    const initialSettings: ClientSettings = {
      trace: readTraceSetting(reader),
      languageVersion: readDefaultLanguageVersion(reader),
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
    }),
    vscode.workspace.onDidChangeWorkspaceFolders(restart),
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

  return {
    get state(): LifecycleState {
      return lifecycle.state;
    },
  };
}

export async function deactivate(): Promise<void> {
  const lifecycle = activeLifecycle;
  activeLifecycle = undefined;
  await lifecycle?.stop();
}
