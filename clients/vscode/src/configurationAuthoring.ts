import * as vscode from "vscode";

import {
  configurationCandidates,
  configurationContentHash,
  configurationDraftInitialContent,
  configurationDraftTooLarge,
  configurationIsStale,
  ConfigurationAuthoringParams,
  ConfigurationAuthoringResult,
  ConfigurationSelection,
  formatConfigurationErrors,
} from "./configurationAuthoringCore";

export const createConfigurationCommand = "hlsl.createConfiguration";
export const editConfigurationCommand = "hlsl.editConfiguration";
export const validateConfigurationDraftCommand =
  "hlsl.validateConfigurationDraft";
export const cancelConfigurationDraftCommand = "hlsl.cancelConfigurationDraft";

type AuthoringMode = "create" | "edit";

interface CandidateQuickPickItem extends vscode.QuickPickItem {
  readonly selection: ConfigurationSelection;
}

export async function openConfigurationDraft(
  content: string,
): Promise<vscode.TextDocument> {
  return vscode.workspace.openTextDocument({ language: "json", content });
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
  private running = false;
  private activeDraft:
    | {
        uri: string;
        signal: (action: "validate" | "cancel") => void;
      }
    | undefined;

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
      vscode.commands.registerCommand(validateConfigurationDraftCommand, () =>
        this.activeDraft?.signal("validate"),
      ),
      vscode.commands.registerCommand(cancelConfigurationDraftCommand, () =>
        this.activeDraft?.signal("cancel"),
      ),
    ];
  }

  public dispose(): void {
    this.activeDraft?.signal("cancel");
    for (const disposable of this.disposables) {
      disposable.dispose();
    }
  }

  private async run(mode: AuthoringMode): Promise<void> {
    if (this.running) {
      await vscode.window.showWarningMessage(
        "Finish or cancel the current configuration draft first.",
      );
      return;
    }
    this.running = true;
    let restart: AuthoringMode | undefined;
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
          restart = "edit";
        }
        return;
      }
      if (mode === "edit" && existingDocument === undefined) {
        const action = await vscode.window.showInformationMessage(
          "This workspace does not have shadertoolsconfig.json.",
          "Create Configuration",
        );
        if (action === "Create Configuration") {
          restart = "create";
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
      const validated = await this.editDraft(
        baseParams,
        selections,
        existingDocument,
        result,
      );
      if (validated === undefined) {
        return;
      }

      const { document: draft } = validated;
      let preview = validated.result;
      for (;;) {
        if (draft.isClosed) {
          return;
        }
        await this.showPreview(configurationUri, existingDocument, preview);
        const confirmation = await vscode.window.showWarningMessage(
          preview.preview.changed
            ? "Apply this shadertoolsconfig.json preview? Formatting and comments may change."
            : "The preview makes no changes. Open the configuration file?",
          { modal: true },
          preview.preview.changed
            ? "Apply Configuration"
            : "Open Configuration",
        );
        if (
          confirmation !== "Apply Configuration" &&
          confirmation !== "Open Configuration"
        ) {
          return;
        }
        if (draft.getText() !== preview.preview.content) {
          void vscode.window.showWarningMessage(
            "The draft changed after validation. Validate it again before applying.",
          );
          const updated = await this.validateDraft(
            baseParams,
            selections,
            draft,
          );
          if (updated === undefined) {
            return;
          }
          preview = updated;
          continue;
        }
        if (!preview.preview.changed) {
          if (existingDocument !== undefined) {
            await vscode.window.showTextDocument(existingDocument, {
              preview: false,
            });
          }
          return;
        }

        const currentDocument =
          await this.readExistingDocument(configurationUri);
        const stale =
          currentDocument === undefined
            ? preview.configuration.exists ||
              preview.configuration.expectedContentVersion !== null ||
              preview.configuration.expectedContentHash !== null
            : !preview.configuration.exists ||
              configurationIsStale(
                preview.configuration.expectedContentVersion,
                preview.configuration.expectedContentHash,
                currentDocument.version,
                currentDocument.getText(),
              );
        if (stale) {
          const action = await vscode.window.showWarningMessage(
            "shadertoolsconfig.json changed after the preview was generated. The stale preview was not applied.",
            "Restart Preview",
          );
          if (action === "Restart Preview") {
            restart = currentDocument === undefined ? "create" : "edit";
          }
          return;
        }

        const edit = new vscode.WorkspaceEdit();
        if (currentDocument === undefined) {
          edit.createFile(configurationUri);
          edit.insert(
            configurationUri,
            new vscode.Position(0, 0),
            preview.preview.content,
          );
        } else {
          edit.replace(
            configurationUri,
            new vscode.Range(
              new vscode.Position(0, 0),
              currentDocument.positionAt(currentDocument.getText().length),
            ),
            preview.preview.content,
          );
        }
        if (!(await vscode.workspace.applyEdit(edit))) {
          throw new Error("VS Code refused the configuration workspace edit");
        }
        const document =
          await vscode.workspace.openTextDocument(configurationUri);
        await vscode.window.showTextDocument(document, { preview: false });
        return;
      }
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
    } finally {
      this.running = false;
      this.activeDraft = undefined;
      await vscode.commands.executeCommand(
        "setContext",
        "hlsl.configurationDraftOpen",
        false,
      );
      if (restart !== undefined) {
        await this.run(restart);
      }
    }
  }

  private async editDraft(
    baseParams: ConfigurationAuthoringParams,
    selections: readonly ConfigurationSelection[],
    existingDocument: vscode.TextDocument | undefined,
    result: ConfigurationAuthoringResult,
  ): Promise<
    | { document: vscode.TextDocument; result: ConfigurationAuthoringResult }
    | undefined
  > {
    if (this.activeDraft !== undefined) {
      await vscode.window.showWarningMessage(
        "Finish or cancel the current configuration draft first.",
      );
      return undefined;
    }
    const draft = await openConfigurationDraft(
      configurationDraftInitialContent(result, existingDocument?.getText()),
    );
    await vscode.window.showTextDocument(draft, { preview: false });
    await vscode.commands.executeCommand(
      "setContext",
      "hlsl.configurationDraftOpen",
      true,
    );
    if (!result.preview.valid) {
      void this.showValidationErrors(result);
    }
    void vscode.window
      .showInformationMessage(
        "Edit the untitled JSON draft (not the original). Formatting and comments may change. Validate Draft before the final diff and confirmation.",
        "Validate Draft",
        "Cancel Draft",
      )
      .then((action) => {
        if (this.activeDraft?.uri !== draft.uri.toString()) {
          return;
        }
        if (action === "Validate Draft") {
          this.activeDraft.signal("validate");
        } else if (action === "Cancel Draft") {
          this.activeDraft.signal("cancel");
        }
      });
    const validated = await this.validateDraft(baseParams, selections, draft);
    return validated === undefined
      ? undefined
      : { document: draft, result: validated };
  }

  private async validateDraft(
    baseParams: ConfigurationAuthoringParams,
    selections: readonly ConfigurationSelection[],
    draft: vscode.TextDocument,
  ): Promise<ConfigurationAuthoringResult | undefined> {
    for (;;) {
      const action = await new Promise<"validate" | "cancel">((resolve) => {
        const close = vscode.workspace.onDidCloseTextDocument((document) => {
          if (document.uri.toString() === draft.uri.toString()) {
            close.dispose();
            this.activeDraft = undefined;
            resolve("cancel");
          }
        });
        this.activeDraft = {
          uri: draft.uri.toString(),
          signal: (value) => {
            close.dispose();
            this.activeDraft = undefined;
            resolve(value);
          },
        };
      });
      if (action === "cancel") {
        return undefined;
      }
      const content = draft.getText();
      if (configurationDraftTooLarge(content)) {
        void vscode.window.showErrorMessage(
          "The JSON draft exceeds the 2 MiB UTF-8 server limit. Reduce it and validate again.",
        );
        continue;
      }
      const version = draft.version;
      const result = await this.request({
        ...baseParams,
        selections,
        draftContent: content,
      });
      if (draft.isClosed) {
        return undefined;
      }
      if (result === undefined || result === null) {
        await vscode.window.showErrorMessage(
          "The HLSL language server is not running. The draft remains open.",
        );
        return undefined;
      }
      if (draft.version !== version) {
        void vscode.window.showWarningMessage(
          "The draft changed during validation. Validate the latest content again.",
        );
        continue;
      }
      if (!result.preview.valid) {
        void this.showValidationErrors(result);
        await vscode.window.showTextDocument(draft, { preview: false });
        continue;
      }
      return result;
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
