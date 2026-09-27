import * as vscode from "vscode";

import {
  configurationContentHash,
  configurationDraftTooLarge,
  configurationIsStale,
  ConfigurationAuthoringParams,
  ConfigurationAuthoringResult,
  formatConfigurationErrors,
} from "./configurationAuthoringCore";
import { editConfigurationCommand } from "./configurationAuthoring";
import {
  CaptureEntry,
  CapturePipeline,
  CapturePreview,
  CaptureSnapshot,
  CaptureStage,
  CaptureVariant,
  captureStage,
  capturePreviewMatches,
  captureSessionMatches,
  captureVariantRequired,
  hostPipelineCandidates,
} from "./runtimeCaptureCore";

export const validateCaptureDraftCommand = "hlsl.validateCaptureDraft";
export const cancelCaptureDraftCommand = "hlsl.cancelCaptureDraft";

class PreviewDocuments implements vscode.TextDocumentContentProvider {
  private readonly contents = new Map<string, string>();

  public set(uri: vscode.Uri, content: string): void {
    this.contents.set(uri.toString(), content);
  }

  public provideTextDocumentContent(uri: vscode.Uri): string {
    return this.contents.get(uri.toString()) ?? "";
  }
}

export class RuntimeCaptureAuthoring implements vscode.Disposable {
  private readonly documents = new PreviewDocuments();
  private readonly disposables: vscode.Disposable[];
  private sequence = 0;
  private draftAction: ((action: "validate" | "cancel") => void) | undefined;

  public constructor(
    private readonly previewRequest: (
      params: Record<string, unknown>,
    ) => Promise<CapturePreview | null | undefined>,
    private readonly authoringRequest: (
      params: ConfigurationAuthoringParams,
    ) => Promise<ConfigurationAuthoringResult | null | undefined>,
    private readonly statusRequest: () => Promise<
      { active: boolean; sessionId: string | null } | null | undefined
    >,
  ) {
    this.disposables = [
      vscode.workspace.registerTextDocumentContentProvider(
        "hlsl-capture-preview",
        this.documents,
      ),
      vscode.commands.registerCommand(validateCaptureDraftCommand, () =>
        this.draftAction?.("validate"),
      ),
      vscode.commands.registerCommand(cancelCaptureDraftCommand, () =>
        this.draftAction?.("cancel"),
      ),
    ];
  }

  public dispose(): void {
    this.draftAction?.("cancel");
    for (const disposable of this.disposables) {
      disposable.dispose();
    }
  }

  public async run(
    folder: vscode.WorkspaceFolder,
    snapshot: CaptureSnapshot,
    entries: readonly CaptureEntry[],
    isCurrent: () => boolean,
  ): Promise<void> {
    if (!snapshot.active || !snapshot.sessionId || !isCurrent()) {
      await vscode.window.showWarningMessage(
        "This snapshot is no longer active. Start a new capture before previewing configuration.",
      );
      return;
    }
    if (
      entries.length > 64 ||
      entries.some((entry) => !entry.review.eligible || !entry.id)
    ) {
      await vscode.window.showWarningMessage(
        "Select 1–64 eligible captured invocations before previewing.",
      );
      return;
    }
    const variants = await this.chooseVariants(entries);
    if (!variants || !isCurrent()) {
      return;
    }
    const pipelines = await this.choosePipelines(entries);
    if (!pipelines || !isCurrent()) {
      return;
    }
    const uri = vscode.Uri.joinPath(folder.uri, "shadertoolsconfig.json");
    const existing = await this.readExisting(uri);
    if (!isCurrent()) {
      return;
    }
    const existingConfiguration = existing
      ? {
          content: existing.getText(),
          version: existing.version,
          contentHash: configurationContentHash(existing.getText()),
        }
      : undefined;
    if (
      existingConfiguration &&
      configurationDraftTooLarge(existingConfiguration.content)
    ) {
      await vscode.window.showWarningMessage(
        "The existing configuration exceeds the 2 MiB server limit. No preview was requested.",
      );
      return;
    }
    const request = {
      protocolVersion: 1,
      workspaceFolder: { uri: folder.uri.toString() },
      sessionId: snapshot.sessionId,
      selectedEntryIds: entries.map((entry) => entry.id),
      ...(existingConfiguration ? { existingConfiguration } : {}),
      ...(variants.length ? { variants } : {}),
      ...(pipelines.length ? { pipelines } : {}),
    };
    const preview = await this.previewRequest(request);
    if (!isCurrent()) {
      return;
    }
    if (!preview) {
      await vscode.window.showWarningMessage(
        "The HLSL language server disconnected before the capture preview. Nothing was applied.",
      );
      return;
    }
    if (!capturePreviewMatches(snapshot, entries, uri.toString(), preview)) {
      await vscode.window.showWarningMessage(
        "Capture changed while generating the preview. Nothing was applied.",
      );
      return;
    }
    if (!preview.preview.valid) {
      const action = await vscode.window.showErrorMessage(
        `Capture merge could not produce a valid configuration:\n${formatConfigurationErrors(preview.preview.errors)}\nNo captured settings were applied. Correct the existing configuration, then review this active capture again.`,
        "Edit Configuration",
      );
      if (action === "Edit Configuration") {
        await vscode.commands.executeCommand(editConfigurationCommand);
      }
      return;
    }
    const baseParams: ConfigurationAuthoringParams = {
      protocolVersion: 1,
      workspaceFolder: { uri: folder.uri.toString() },
      ...(existingConfiguration ? { existingConfiguration } : {}),
    };
    const warnings = entries.flatMap((entry) => entry.review.warnings);
    let validated: CapturePreview | ConfigurationAuthoringResult = preview;
    let draft: vscode.TextDocument | undefined;
    let validatedDraftContent: string | undefined;
    try {
      for (;;) {
        if (!isCurrent()) {
          return;
        }
        if (draft?.isClosed) {
          return;
        }
        await this.showDiff(uri, existing, validated.preview.content);
        const errors = formatConfigurationErrors(validated.preview.errors);
        const explanation = [
          `Capture preview: ${String(entries.length)} selected invocations, ${String(variants.length)} named variants, ${String(pipelines.length)} pipelines.`,
          warnings.length
            ? `Warnings:\n${warnings.join("\n")}`
            : "No capture warnings.",
          validated.preview.valid
            ? "Production validation passed. Inspect the full JSON diff before applying."
            : `Validation failed:\n${errors}`,
        ].join("\n\n");
        const ready = await vscode.window.showInformationMessage(
          "Inspect the complete JSON diff before deciding whether to edit or apply this captured configuration.",
          "Review Decisions",
        );
        if (ready !== "Review Decisions" || !isCurrent()) {
          return;
        }
        const choice = await vscode.window.showWarningMessage(
          explanation,
          { modal: true },
          ...(validated.preview.valid && validated.preview.changed
            ? ["Apply Configuration", "Edit Draft"]
            : ["Edit Draft"]),
        );
        if (!choice || !isCurrent()) {
          return;
        }
        if (choice === "Edit Draft") {
          draft ??= await vscode.workspace.openTextDocument({
            language: "json",
            content: preview.preview.content,
          });
          validatedDraftContent = undefined;
          await vscode.window.showTextDocument(draft, { preview: false });
          void vscode.window
            .showInformationMessage(
              "Edit the untitled JSON draft, then choose Validate Capture Draft in its editor title. Cancel Capture Draft leaves the configuration unchanged.",
              "Validate Capture Draft",
              "Cancel Capture Draft",
            )
            .then((action) => {
              if (action === "Validate Capture Draft") {
                this.draftAction?.("validate");
              } else if (action === "Cancel Capture Draft") {
                this.draftAction?.("cancel");
              }
            });
          const result = await this.validateDraft(baseParams, draft, isCurrent);
          if (!result) {
            return;
          }
          validated = result;
          validatedDraftContent = draft.getText();
          continue;
        }
        if (
          !validated.preview.valid ||
          !validated.preview.changed ||
          (draft && validatedDraftContent !== draft.getText())
        ) {
          await vscode.window.showWarningMessage(
            "The draft changed after validation. Validate again before applying.",
          );
          continue;
        }
        if (
          validated.preview.contentHash !==
          configurationContentHash(validated.preview.content)
        ) {
          await vscode.window.showWarningMessage(
            "The preview content hash did not match its JSON. Nothing was applied.",
          );
          return;
        }
        const status = await this.statusRequest();
        if (
          !isCurrent() ||
          !captureSessionMatches(snapshot.sessionId, status)
        ) {
          await vscode.window.showWarningMessage(
            "The capture session changed. Start a new preview before applying.",
          );
          return;
        }
        const current = await this.readExisting(uri);
        const state = validated.configuration;
        const stale = current
          ? !state.exists ||
            state.expectedContentVersion === null ||
            state.expectedContentHash === null ||
            configurationIsStale(
              state.expectedContentVersion,
              state.expectedContentHash,
              current.version,
              current.getText(),
            )
          : state.exists ||
            state.expectedContentVersion !== null ||
            state.expectedContentHash !== null;
        if (stale || !isCurrent()) {
          await vscode.window.showWarningMessage(
            "shadertoolsconfig.json changed after preview. Nothing was applied; review a fresh snapshot.",
          );
          return;
        }
        const edit = new vscode.WorkspaceEdit();
        if (current) {
          edit.replace(
            uri,
            new vscode.Range(
              new vscode.Position(0, 0),
              current.positionAt(current.getText().length),
            ),
            validated.preview.content,
          );
        } else {
          edit.createFile(uri);
          edit.insert(
            uri,
            new vscode.Position(0, 0),
            validated.preview.content,
          );
        }
        if (!isCurrent() || !(await vscode.workspace.applyEdit(edit))) {
          await vscode.window.showErrorMessage(
            "VS Code refused the configuration workspace edit.",
          );
          return;
        }
        await vscode.window.showTextDocument(
          await vscode.workspace.openTextDocument(uri),
          { preview: false },
        );
        return;
      }
    } finally {
      this.draftAction?.("cancel");
      this.draftAction = undefined;
      await vscode.commands.executeCommand(
        "setContext",
        "hlsl.captureDraftOpen",
        false,
      );
    }
  }

  private async chooseVariants(
    entries: readonly CaptureEntry[],
  ): Promise<readonly CaptureVariant[] | undefined> {
    const required = captureVariantRequired(entries);
    const named = new Set<string>();
    const variants: CaptureVariant[] = [];
    for (const entry of entries) {
      if (!required.has(entry.id)) {
        continue;
      }
      const name = await vscode.window.showInputBox({
        title: `Name captured variant — ${entry.review.selection?.relativePath ?? entry.invocation.source}`,
        prompt: `${entry.invocation.entryPoint} (${entry.invocation.targetProfile}); a distinct variant name is required for every captured invocation of this file.`,
        validateInput: (value) =>
          !value.trim() || value.length > 128 || named.has(value.trim())
            ? "Enter a unique variant name of 1–128 characters."
            : undefined,
      });
      if (name === undefined) {
        return undefined;
      }
      const trimmed = name.trim();
      named.add(trimmed);
      variants.push({ entryId: entry.id, name: trimmed });
    }
    return variants;
  }

  private async choosePipelines(
    entries: readonly CaptureEntry[],
  ): Promise<readonly CapturePipeline[] | undefined> {
    const choice = await vscode.window.showQuickPick(
      [
        { label: "No pipelines", value: "none" as const },
        { label: "Select host-correlated pipelines", value: "host" as const },
        {
          label: "Create an explicit vertex/pixel pipeline group",
          value: "user" as const,
        },
      ],
      {
        title: "Explicit Pipeline Selection",
        placeHolder:
          "Capture does not automatically create pipeline relationships",
      },
    );
    if (!choice) {
      return undefined;
    }
    if (choice.value === "none") {
      return [];
    }
    if (choice.value === "host") {
      const candidates = hostPipelineCandidates(entries).slice(0, 16);
      if (!candidates.length) {
        await vscode.window.showWarningMessage(
          "No unambiguous host-correlated vertex/pixel pipeline pairs were selected.",
        );
        return [];
      }
      const selected = await vscode.window.showQuickPick(
        candidates.map((pipeline) => ({
          label: pipeline.name,
          description: Object.keys(pipeline.stages).join(", "),
          pipeline,
        })),
        { canPickMany: true, title: "Select Named Host Pipelines" },
      );
      return selected?.map((item) => item.pipeline);
    }
    const stages: Partial<Record<CaptureStage, string>> = {};
    for (const stage of ["vertex", "pixel"] as const) {
      const candidates = entries.filter(
        (entry) =>
          captureStage(entry) === stage &&
          !Object.values(stages).includes(entry.id),
      );
      if (!candidates.length) {
        await vscode.window.showWarningMessage(
          `A selected ${stage} shader is required to create a pipeline.`,
        );
        return undefined;
      }
      const selected = await vscode.window.showQuickPick(
        candidates.map((entry) => ({
          label: `${entry.invocation.entryPoint} · ${entry.invocation.targetProfile}`,
          description:
            entry.review.selection?.relativePath ?? entry.invocation.source,
          entry,
        })),
        { title: `Choose ${stage} Stage (Explicit User Grouping)` },
      );
      if (!selected) {
        return undefined;
      }
      stages[stage] = selected.entry.id;
    }
    const name = await vscode.window.showInputBox({
      title: "Name Pipeline (Explicit User Grouping)",
      validateInput: (value) =>
        !value.trim() || value.length > 128
          ? "Enter a pipeline name of 1–128 characters."
          : undefined,
    });
    return name === undefined
      ? undefined
      : [{ name: name.trim(), source: "user", stages }];
  }

  private async validateDraft(
    params: ConfigurationAuthoringParams,
    draft: vscode.TextDocument,
    isCurrent: () => boolean,
  ): Promise<ConfigurationAuthoringResult | undefined> {
    await vscode.commands.executeCommand(
      "setContext",
      "hlsl.captureDraftOpen",
      true,
    );
    for (;;) {
      const action = await new Promise<"validate" | "cancel">((resolve) => {
        const close = vscode.workspace.onDidCloseTextDocument((document) => {
          if (document.uri.toString() === draft.uri.toString()) {
            close.dispose();
            this.draftAction = undefined;
            resolve("cancel");
          }
        });
        this.draftAction = (value) => {
          close.dispose();
          this.draftAction = undefined;
          resolve(value);
        };
      });
      if (action === "cancel" || !isCurrent() || draft.isClosed) {
        return undefined;
      }
      const content = draft.getText();
      if (configurationDraftTooLarge(content)) {
        await vscode.window.showErrorMessage(
          "The draft exceeds the 2 MiB server limit. Reduce it and validate again.",
        );
        continue;
      }
      const version = draft.version;
      const result = await this.authoringRequest({
        ...params,
        draftContent: content,
      });
      if (!isCurrent() || !this.draftOpen(draft)) {
        return undefined;
      }
      if (!result) {
        await vscode.window.showWarningMessage(
          "The HLSL language server disconnected during draft validation. Nothing was applied.",
        );
        return undefined;
      }
      if (draft.version !== version) {
        await vscode.window.showWarningMessage(
          "The draft changed during validation. Validate again.",
        );
        continue;
      }
      if (!result.preview.valid) {
        await vscode.window.showErrorMessage(
          `Configuration validation failed:\n${formatConfigurationErrors(result.preview.errors)}`,
        );
        await vscode.window.showTextDocument(draft, { preview: false });
        continue;
      }
      return result;
    }
  }

  private async readExisting(
    uri: vscode.Uri,
  ): Promise<vscode.TextDocument | undefined> {
    const open = vscode.workspace.textDocuments.find(
      (document) => document.uri.toString() === uri.toString(),
    );
    if (open) {
      return open;
    }

    try {
      const stat = await vscode.workspace.fs.stat(uri);
      if (stat.type !== vscode.FileType.File) {
        throw new Error("Configuration path is not a regular file");
      }
      return await vscode.workspace.openTextDocument(uri);
    } catch (error) {
      if (
        error instanceof vscode.FileSystemError &&
        error.code === "FileNotFound"
      ) {
        return undefined;
      }
      throw error;
    }
  }

  private draftOpen(draft: vscode.TextDocument): boolean {
    return !draft.isClosed;
  }

  private async showDiff(
    uri: vscode.Uri,
    existing: vscode.TextDocument | undefined,
    content: string,
  ): Promise<void> {
    const sequence = ++this.sequence;
    const before =
      existing?.uri ??
      vscode.Uri.parse(
        `hlsl-capture-preview:/before-${String(sequence)}/shadertoolsconfig.json`,
      );
    if (!existing) {
      this.documents.set(before, "");
    }
    const after = vscode.Uri.parse(
      `hlsl-capture-preview:/after-${String(sequence)}/shadertoolsconfig.json`,
    );
    this.documents.set(after, content);
    await vscode.commands.executeCommand(
      "vscode.diff",
      before,
      after,
      `${uri.fsPath} — Captured Configuration Preview`,
      { preview: true },
    );
  }
}
