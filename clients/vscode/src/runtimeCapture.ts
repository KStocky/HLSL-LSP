import * as vscode from "vscode";

import { RuntimeCaptureAuthoring } from "./runtimeCaptureAuthoring";
import {
  CaptureEntry,
  CapturePreview,
  CaptureSnapshot,
  CaptureStart,
  CaptureStatus,
  captureEntryIsTransient,
  captureSummary,
} from "./runtimeCaptureCore";

export const startCaptureCommand = "hlsl.startCapture";
export const stopCaptureCommand = "hlsl.stopCapture";
export const captureStatusCommand = "hlsl.captureStatus";
export const reviewCaptureCommand = "hlsl.reviewCapture";

type CaptureMethod = "start" | "stop" | "status" | "snapshot" | "preview";
type CaptureResult =
  CaptureStart | CaptureStatus | CaptureSnapshot | CapturePreview;

interface CapturePick extends vscode.QuickPickItem {
  readonly entry: CaptureEntry;
}

export class RuntimeCaptureController implements vscode.Disposable {
  private readonly disposables: vscode.Disposable[];
  private token: string | undefined;
  private endpoint: string | undefined;
  private generation = 0;
  private busy = false;
  private stoppedSnapshot:
    { readonly folder: string; readonly snapshot: CaptureSnapshot } | undefined;

  public constructor(
    private readonly request: (
      method: CaptureMethod,
      params: Record<string, unknown>,
    ) => Promise<CaptureResult | null | undefined>,
    private readonly authoring: RuntimeCaptureAuthoring,
  ) {
    this.disposables = [
      vscode.commands.registerCommand(startCaptureCommand, () =>
        this.run(() => this.start()),
      ),
      vscode.commands.registerCommand(stopCaptureCommand, () =>
        this.run(() => this.stop()),
      ),
      vscode.commands.registerCommand(captureStatusCommand, () =>
        this.run(() => this.status()),
      ),
      vscode.commands.registerCommand(reviewCaptureCommand, () =>
        this.run(() => this.review()),
      ),
    ];
  }

  public invalidate(): void {
    ++this.generation;
    this.token = undefined;
    this.endpoint = undefined;
    this.stoppedSnapshot = undefined;
  }

  public dispose(): void {
    this.invalidate();
    this.authoring.dispose();
    for (const disposable of this.disposables) {
      disposable.dispose();
    }
  }

  private async run(action: () => Promise<void>): Promise<void> {
    if (this.busy) {
      await vscode.window.showWarningMessage(
        "Finish the current capture action before starting another.",
      );
      return;
    }
    this.busy = true;
    try {
      await action();
    } catch {
      // Server errors can include request parameters; never display or log them.
      await vscode.window.showErrorMessage(
        "The capture request failed. Check the language server connection and try again.",
      );
    } finally {
      this.busy = false;
    }
  }

  private async start(): Promise<void> {
    if (this.token !== undefined) {
      const choice = await vscode.window.showWarningMessage(
        "Starting a new capture revokes the current engine connection and clears its snapshot.",
        { modal: true },
        "Replace Capture",
      );
      if (choice !== "Replace Capture") {
        return;
      }
    } else {
      const choice = await vscode.window.showWarningMessage(
        "Capture is opt-in. Only metadata your engine's own compile wrapper explicitly reports is collected in memory. Do not send secrets in defines or compiler arguments. Start a local capture session?",
        { modal: true },
        "Start Capture",
      );
      if (choice !== "Start Capture") {
        return;
      }
    }
    ++this.generation;
    const generation = this.generation;
    const result = (await this.request("start", {
      protocolVersion: 1,
    })) as CaptureStart | null | undefined;
    if (generation !== this.generation) {
      return;
    }
    if (!result) {
      await this.disconnected();
      return;
    }
    if (
      typeof result.sessionId !== "string" ||
      typeof result.endpoint !== "string" ||
      typeof result.token !== "string"
    ) {
      throw new Error("Invalid capture start response");
    }
    this.token = result.token;
    this.endpoint = result.endpoint;
    this.stoppedSnapshot = undefined;
    const choice = await vscode.window.showInformationMessage(
      "Capture started. In your own engine's opt-in C++ compile wrapper, connect the capture SDK with the local endpoint and token. Run your project, then Review Capture while the session is active; stop it after applying. Connection details are only revealed on your explicit request; never share them.",
      "Show Connection Details",
    );
    if (
      choice === "Show Connection Details" &&
      generation === this.generation
    ) {
      await this.showConnectionDetails();
    }
  }

  private async showConnectionDetails(): Promise<void> {
    if (this.endpoint === undefined || this.token === undefined) {
      return;
    }
    await vscode.window.showInformationMessage(
      `Private local capture connection (give only to your own engine):\nEndpoint: ${this.endpoint}\nToken: ${this.token}\nDo not paste these into source control, logs, or shared terminals.`,
      { modal: true },
      "Close",
    );
  }

  private async stop(): Promise<void> {
    if (this.token === undefined) {
      await vscode.window.showInformationMessage(
        "No capture session started by this VS Code window is available to stop.",
      );
      return;
    }
    const generation = this.generation;
    const folders = vscode.workspace.workspaceFolders ?? [];
    const folder =
      folders.length === 1
        ? folders[0]
        : folders.length > 1
          ? (
              await vscode.window.showQuickPick(
                folders.map((item) => ({
                  label: item.name,
                  description: item.uri.fsPath,
                  folder: item,
                })),
                {
                  title: "Choose Workspace for Final Capture Snapshot",
                  placeHolder:
                    "Cancel to stop without retaining a review snapshot",
                },
              )
            )?.folder
          : undefined;
    let saved: CaptureSnapshot | undefined;
    if (folder) {
      try {
        saved = (await this.request("snapshot", {
          protocolVersion: 1,
          workspaceFolder: { uri: folder.uri.toString() },
        })) as CaptureSnapshot | undefined;
      } catch {
        // Revoking the credential takes precedence over retaining a snapshot.
      }
    }
    if (generation !== this.generation) {
      return;
    }
    const result = (await this.request("stop", {
      protocolVersion: 1,
      token: this.token,
    })) as CaptureStatus | null | undefined;
    if (generation !== this.generation) {
      return;
    }
    if (!result) {
      this.invalidate();
      await this.disconnected();
      return;
    }
    this.invalidate();
    if (saved && folder) {
      this.stoppedSnapshot = {
        folder: folder.uri.toString(),
        snapshot: { ...saved, active: false },
      };
    }
    await vscode.window.showInformationMessage(
      saved
        ? "Capture stopped. The final snapshot can be inspected read-only, but the revoked session cannot generate a configuration preview. Review an active capture to apply settings."
        : "Capture stopped. The server's in-memory snapshot was erased; no final review snapshot was retained.",
    );
  }

  private async status(): Promise<void> {
    const result = (await this.request("status", {
      protocolVersion: 1,
    })) as CaptureStatus | null | undefined;
    if (!result) {
      await this.disconnected();
      return;
    }
    if (!result.active && this.token !== undefined) {
      this.invalidate();
    }
    await vscode.window.showInformationMessage(
      `Runtime capture: ${result.active ? "active" : "stopped"} · ${String(result.accepted)} accepted · ${String(result.rejected)} rejected · ${String(result.overflow)} overflow. Capture is a bounded sample, not a complete compilation log.`,
    );
  }

  private async review(): Promise<void> {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
      await vscode.window.showInformationMessage(
        "Open a workspace folder to review runtime capture entries.",
      );
      return;
    }
    const folder =
      folders.length === 1
        ? folders[0]
        : (
            await vscode.window.showQuickPick(
              folders.map((item) => ({
                label: item.name,
                description: item.uri.fsPath,
                folder: item,
              })),
              { title: "Select Capture Review Workspace" },
            )
          )?.folder;
    if (!folder) {
      return;
    }
    const generation = this.generation;
    const snapshot =
      this.stoppedSnapshot?.folder === folder.uri.toString()
        ? this.stoppedSnapshot.snapshot
        : ((await this.request("snapshot", {
            protocolVersion: 1,
            workspaceFolder: { uri: folder.uri.toString() },
          })) as CaptureSnapshot | null | undefined);
    if (generation !== this.generation) {
      return;
    }
    if (!snapshot) {
      await this.disconnected();
      return;
    }
    if (snapshot.entries.length === 0) {
      await vscode.window.showInformationMessage(
        `${captureSummary(snapshot)}. Start capture, connect your engine, and run your project before reviewing.`,
      );
      return;
    }
    const filter = await vscode.window.showQuickPick(
      [
        {
          label: "Show eligible, non-transient entries",
          value: "safe" as const,
        },
        {
          label: "Show all entries (inspect warnings and transient values)",
          value: "all" as const,
        },
      ],
      {
        title: `Runtime Capture — ${captureSummary(snapshot)}`,
        placeHolder: "Filter transient/generated values before selecting",
      },
    );
    if (!filter || generation !== this.generation) {
      return;
    }
    const entries =
      filter.value === "safe"
        ? snapshot.entries.filter((entry) => !captureEntryIsTransient(entry))
        : snapshot.entries;
    if (!entries.length) {
      await vscode.window.showInformationMessage(
        "No eligible non-transient entries. Review all entries to inspect the warning codes, or run your project again.",
      );
      return;
    }
    const picks: CapturePick[] = entries.map((entry) => ({
      label: `${entry.review.eligible ? "$(check)" : "$(warning)"} ${entry.invocation.entryPoint.length ? entry.invocation.entryPoint : "(no entry point)"} · ${entry.invocation.targetProfile.length ? entry.invocation.targetProfile : "(no profile)"}`,
      description: `${entry.invocation.source} · ${String(entry.count)} invocation${entry.count === 1 ? "" : "s"}`,
      detail: `Pipeline: ${entry.invocation.pipeline?.length ? entry.invocation.pipeline : "(unnamed)"} · Stage: ${entry.invocation.stage?.length ? entry.invocation.stage : "(unspecified)"} · Warnings: ${entry.review.warningCodes.join(", ") || "none"}`,
      picked: false,
      entry,
    }));
    const selected = await vscode.window.showQuickPick(picks, {
      canPickMany: true,
      title: `Review Captured Invocations — ${captureSummary(snapshot)}`,
      placeHolder:
        "Select entries to inspect; nothing is written to configuration",
      matchOnDescription: true,
      matchOnDetail: true,
    });
    if (!selected || generation !== this.generation || !selected.length) {
      return;
    }
    const details = selected.map(({ entry }) => ({
      invocation: entry.invocation,
      count: entry.count,
      eligible: entry.review.eligible,
      warningCodes: entry.review.warningCodes,
      warnings: entry.review.warnings,
      proposedFileGroup: entry.review.fileGroup,
    }));
    const document = await vscode.workspace.openTextDocument({
      language: "json",
      content: JSON.stringify(
        {
          note: "Read-only capture review; do not save this document. For eligible entries in an active session, explicitly request a server-side preview before applying any configuration.",
          summary: captureSummary(snapshot),
          selected: details,
        },
        null,
        2,
      ),
    });
    if (generation !== this.generation) {
      return;
    }
    await vscode.window.showTextDocument(document, { preview: true });
    if (
      !snapshot.active ||
      !snapshot.sessionId ||
      selected.some(({ entry }) => !entry.review.eligible)
    ) {
      await vscode.window.showInformationMessage(
        "This review is read-only: previews require an active session and eligible entries. Close the unsaved review document when finished.",
      );
      return;
    }
    const action = await vscode.window.showInformationMessage(
      "Inspect the unsaved JSON review first. Request a server-side configuration preview for these selected entries?",
      "Preview Selected Entries",
    );
    if (
      action === "Preview Selected Entries" &&
      generation === this.generation
    ) {
      await this.authoring.run(
        folder,
        snapshot,
        selected.map(({ entry }) => entry),
        () => generation === this.generation,
      );
    }
  }

  private async disconnected(): Promise<void> {
    await vscode.window.showInformationMessage(
      "The HLSL language server is not running. Start or restart it before using capture.",
    );
  }
}
