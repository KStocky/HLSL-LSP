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
import {
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
}

export interface HlslExtensionApi {
  readonly state: LifecycleState;
}

interface ClientSettings {
  readonly trace: TraceSetting;
  readonly languageVersion: string;
  readonly server: HlslServerSettings;
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
  ) {
    const executable: Executable = {
      command: runtime.command,
      args: [],
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
      },
      outputChannel,
      traceOutputChannel: outputChannel,
      revealOutputChannelOn: RevealOutputChannelOn.Never,
      synchronize: {
        fileEvents: [...watchers],
      },
    };
    this.client = new LanguageClient(
      "hlsl",
      outputName,
      serverOptions,
      clientOptions,
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
    outputChannel.appendLine(
      `Starting ${runtime.source} server: ${runtime.command}`,
    );
    const watchers = createWatchers(initialSettings.server);
    try {
      return new VscodeLanguageClient(
        runtime,
        outputChannel,
        watchers,
        initialSettings,
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
      outputChannel.appendLine(
        `Editor overrides: ${JSON.stringify(readServerSettings(configuration()))}`,
      );
      outputChannel.show(true);
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      const resource = configurationResource();
      if (!event.affectsConfiguration("hlsl", resource)) {
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
