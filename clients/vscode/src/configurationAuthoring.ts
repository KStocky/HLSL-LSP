import * as vscode from "vscode";

import {
  configurationCandidates,
  configurationContentHash,
  configurationIsStale,
  ConfigurationAuthoringParams,
  ConfigurationAuthoringResult,
  ConfigurationSelection,
  formatConfigurationErrors,
} from "./configurationAuthoringCore";

export const createConfigurationCommand = "hlsl.createConfiguration";
export const editConfigurationCommand = "hlsl.editConfiguration";

type AuthoringMode = "create" | "edit";

interface CandidateQuickPickItem extends vscode.QuickPickItem {
  readonly selection: ConfigurationSelection;
}

class ConfigurationPreviewProvider
  implements vscode.TextDocumentContentProvider
{
  private readonly documents = new Map<string, string>();

  public set(uri: vscode.Uri, content: string): void {
    this.documents.set(uri.toString(), content);
  }

  public provideTextDocumentContent(uri: vscode.Uri): string {
    return this.documents.get(uri.toString()) ?? "";
  }
}

export class ConfigurationAuthoringController implements vscode.Disposable {
  private readonly previewProvider = new ConfigurationPreviewProvider();
  private readonly disposables: vscode.Disposable[];
  private previewSequence = 0;

  public constructor(
    private readonly request: (
      params: ConfigurationAuthoringParams,
    ) => Promise<ConfigurationAuthoringResult | null | undefined>,
    private readonly outputChannel: vscode.OutputChannel,
  ) {
    this.disposables = [
      vscode.workspace.registerTextDocumentContentProvider(
        "hlsl-config-preview",
        this.previewProvider,
      ),
      vscode.commands.registerCommand(createConfigurationCommand, () =>
        this.run("create"),
      ),
      vscode.commands.registerCommand(editConfigurationCommand, () =>
        this.run("edit"),
      ),
    ];
  }

  public dispose(): void {
    for (const disposable of this.disposables) {
      disposable.dispose();
    }
  }

  private async run(mode: AuthoringMode): Promise<void> {
    try {
      const workspaceFolder = await this.selectWorkspaceFolder();
      if (workspaceFolder === undefined) {
        return;
      }
      const configurationUri = vscode.Uri.joinPath(
        workspaceFolder.uri,
        "shadertoolsconfig.json",
      );
      const existingDocument =
        await this.readExistingDocument(configurationUri);
      if (mode === "create" && existingDocument !== undefined) {
        const action = await vscode.window.showInformationMessage(
          "This workspace already has shadertoolsconfig.json.",
          "Edit Configuration",
        );
        if (action === "Edit Configuration") {
          await this.run("edit");
        }
        return;
      }
      if (mode === "edit" && existingDocument === undefined) {
        const action = await vscode.window.showInformationMessage(
          "This workspace does not have shadertoolsconfig.json.",
          "Create Configuration",
        );
        if (action === "Create Configuration") {
          await this.run("create");
        }
        return;
      }

      const existingConfiguration =
        existingDocument === undefined
          ? undefined
          : {
              content: existingDocument.getText(),
              version: existingDocument.version,
              contentHash: configurationContentHash(existingDocument.getText()),
            };
      const baseParams: ConfigurationAuthoringParams = {
        protocolVersion: 1,
        workspaceFolder: { uri: workspaceFolder.uri.toString() },
        ...(existingConfiguration === undefined
          ? {}
          : { existingConfiguration }),
      };
      const discovery = await this.request(baseParams);
      if (discovery === undefined || discovery === null) {
        await vscode.window.showInformationMessage(
          "The HLSL language server is not running.",
        );
        return;
      }
      if (!discovery.preview.valid) {
        await this.showValidationErrors(discovery);
        return;
      }

      const selections = await this.selectCandidates(discovery);
      if (selections === undefined) {
        return;
      }
      const result = await this.request({ ...baseParams, selections });
      if (result === undefined || result === null) {
        await vscode.window.showInformationMessage(
          "The HLSL language server is not running.",
        );
        return;
      }
      if (!result.preview.valid) {
        await this.showValidationErrors(result);
        return;
      }

      await this.showPreview(configurationUri, existingDocument, result);
      const confirmation = await vscode.window.showWarningMessage(
        result.preview.changed
          ? "Apply this shadertoolsconfig.json preview?"
          : "The preview makes no changes. Open the configuration file?",
        { modal: true },
        result.preview.changed ? "Apply Configuration" : "Open Configuration",
      );
      if (
        confirmation !== "Apply Configuration" &&
        confirmation !== "Open Configuration"
      ) {
        return;
      }
      if (!result.preview.changed) {
        if (existingDocument !== undefined) {
          await vscode.window.showTextDocument(existingDocument, {
            preview: false,
          });
        }
        return;
      }

      const currentDocument = await this.readExistingDocument(configurationUri);
      const stale =
        currentDocument === undefined
          ? result.configuration.exists ||
            result.configuration.expectedContentVersion !== null ||
            result.configuration.expectedContentHash !== null
          : !result.configuration.exists ||
            configurationIsStale(
              result.configuration.expectedContentVersion,
              result.configuration.expectedContentHash,
              currentDocument.version,
              currentDocument.getText(),
            );
      if (stale) {
        const action = await vscode.window.showWarningMessage(
          "shadertoolsconfig.json changed after the preview was generated. The stale preview was not applied.",
          "Restart Preview",
        );
        if (action === "Restart Preview") {
          await this.run(currentDocument === undefined ? "create" : "edit");
        }
        return;
      }

      const edit = new vscode.WorkspaceEdit();
      if (currentDocument === undefined) {
        edit.createFile(configurationUri);
        edit.insert(
          configurationUri,
          new vscode.Position(0, 0),
          result.preview.content,
        );
      } else {
        edit.replace(
          configurationUri,
          new vscode.Range(
            new vscode.Position(0, 0),
            currentDocument.positionAt(currentDocument.getText().length),
          ),
          result.preview.content,
        );
      }
      if (!(await vscode.workspace.applyEdit(edit))) {
        throw new Error("VS Code refused the configuration workspace edit");
      }
      const document =
        await vscode.workspace.openTextDocument(configurationUri);
      await vscode.window.showTextDocument(document, { preview: false });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.outputChannel.appendLine(
        `[error] Configuration authoring failed: ${message}`,
      );
      const action = await vscode.window.showErrorMessage(
        `Unable to author shadertoolsconfig.json: ${message}`,
        "Show Output",
      );
      if (action === "Show Output") {
        this.outputChannel.show(true);
      }
    }
  }

  private async selectWorkspaceFolder(): Promise<
    vscode.WorkspaceFolder | undefined
  > {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
      await vscode.window.showInformationMessage(
        "Open a workspace folder before creating shadertoolsconfig.json.",
      );
      return undefined;
    }
    if (folders.length === 1) {
      return folders[0];
    }
    const selected = await vscode.window.showQuickPick(
      folders.map((folder) => ({
        label: folder.name,
        description: folder.uri.fsPath,
        folder,
      })),
      {
        title: "Select HLSL Configuration Workspace",
        placeHolder: "Choose where shadertoolsconfig.json will be authored",
      },
    );
    return selected?.folder;
  }

  private async readExistingDocument(
    uri: vscode.Uri,
  ): Promise<vscode.TextDocument | undefined> {
    const open = vscode.workspace.textDocuments.find(
      (document) => document.uri.toString() === uri.toString(),
    );
    if (open !== undefined) {
      return open;
    }
    try {
      return await vscode.workspace.openTextDocument(uri);
    } catch {
      return undefined;
    }
  }

  private async selectCandidates(
    result: ConfigurationAuthoringResult,
  ): Promise<readonly ConfigurationSelection[] | undefined> {
    const candidates = configurationCandidates(result);
    if (candidates.length === 0) {
      const proceed = await vscode.window.showWarningMessage(
        "No compiler-validated shader entry points were discovered. Continue with a base configuration preview?",
        "Continue",
      );
      return proceed === "Continue" ? [] : undefined;
    }
    const items: CandidateQuickPickItem[] = candidates.map((candidate) => ({
      label: candidate.label,
      description: candidate.description,
      detail: candidate.detail,
      picked: candidate.picked,
      selection: candidate.selection,
    }));
    const selected = await vscode.window.showQuickPick(items, {
      canPickMany: true,
      title: "Select Compiler-Validated Shader Entries",
      placeHolder:
        "Resolved entries are preselected; explicitly select profiles for ambiguous entries",
      matchOnDescription: true,
      matchOnDetail: true,
    });
    if (selected === undefined) {
      return undefined;
    }
    if (result.discovery.truncated) {
      await vscode.window.showWarningMessage(
        `Configuration discovery was truncated${
          result.discovery.truncationReason
            ? `: ${result.discovery.truncationReason}`
            : "."
        } Review the preview carefully.`,
      );
    }
    return selected.map((item) => item.selection);
  }

  private async showValidationErrors(
    result: ConfigurationAuthoringResult,
  ): Promise<void> {
    const details = formatConfigurationErrors(result.preview.errors);
    this.outputChannel.appendLine(
      `[error] Configuration preview validation failed:\n${details}`,
    );
    const action = await vscode.window.showErrorMessage(
      `Configuration preview validation failed:\n${details}`,
      "Show Output",
    );
    if (action === "Show Output") {
      this.outputChannel.show(true);
    }
  }

  private async showPreview(
    configurationUri: vscode.Uri,
    existingDocument: vscode.TextDocument | undefined,
    result: ConfigurationAuthoringResult,
  ): Promise<void> {
    const sequence = ++this.previewSequence;
    const baselineUri =
      existingDocument?.uri ??
      vscode.Uri.parse(
        `hlsl-config-preview:/before-${String(sequence)}/shadertoolsconfig.json`,
      );
    if (existingDocument === undefined) {
      this.previewProvider.set(baselineUri, "");
    }
    const previewUri = vscode.Uri.parse(
      `hlsl-config-preview:/after-${String(sequence)}/shadertoolsconfig.json`,
    );
    this.previewProvider.set(previewUri, result.preview.content);
    await vscode.commands.executeCommand(
      "vscode.diff",
      baselineUri,
      previewUri,
      `${configurationUri.fsPath} — Configuration Preview`,
      { preview: true },
    );
  }
}
