import assert from "node:assert/strict";
import { TextEncoder } from "node:util";

import * as vscode from "vscode";

import type { HlslExtensionApi } from "../../src/extension";

const timeoutMilliseconds = 30_000;

async function waitFor<T>(
  description: string,
  value: () => T | undefined | Promise<T | undefined>,
): Promise<T> {
  const deadline = Date.now() + timeoutMilliseconds;
  while (Date.now() < deadline) {
    const result = await value();
    if (result !== undefined) {
      return result;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${description}`);
}

async function openFixture(name: string): Promise<vscode.TextDocument> {
  const folder = vscode.workspace.workspaceFolders?.[0];
  assert(folder, "The integration fixture workspace was not opened");
  const document = await vscode.workspace.openTextDocument(
    vscode.Uri.joinPath(folder.uri, name),
  );
  await vscode.window.showTextDocument(document);
  return document;
}

function positionInLast(
  document: vscode.TextDocument,
  text: string,
  offset: number,
): vscode.Position {
  const index = document.getText().lastIndexOf(text);
  assert(index >= 0, `Fixture text was not found: ${text}`);
  return document.positionAt(index + offset);
}

export async function run(): Promise<void> {
  const serverPath = process.env.HLSL_LSP_TEST_SERVER;
  assert(serverPath, "HLSL_LSP_TEST_SERVER was not provided");
  await vscode.workspace
    .getConfiguration("hlsl")
    .update("server.path", serverPath, vscode.ConfigurationTarget.Global);
  await vscode.workspace
    .getConfiguration("hlsl")
    .update(
      "preprocessorDefinitions",
      { EDITOR_RESTART_SETTING: 1 },
      vscode.ConfigurationTarget.Global,
    );

  const extension =
    vscode.extensions.getExtension<HlslExtensionApi>("KStocky.hlsl-lsp");
  assert(extension, "The HLSL-LSP extension was not found");
  const api = await extension.activate();

  const valid = await openFixture("valid.hlsl");
  await waitFor("server initialization", () =>
    api.state === "running" ? true : undefined,
  );
  const completions = await waitFor("Number completion", async () => {
    try {
      const current =
        await vscode.commands.executeCommand<vscode.CompletionList>(
          "vscode.executeCompletionItemProvider",
          valid.uri,
          new vscode.Position(valid.lineCount - 1, 0),
        );
      return current.items.some((item) => item.label === "Number")
        ? current
        : undefined;
    } catch {
      return undefined;
    }
  });
  assert(
    completions.items.some((item) => item.label === "Number"),
    "Completion did not include Number",
  );

  const definitions = await waitFor("definition provider", async () => {
    const current = await vscode.commands.executeCommand<
      readonly (vscode.Location | vscode.LocationLink)[]
    >(
      "vscode.executeDefinitionProvider",
      valid.uri,
      positionInLast(valid, "shade(", 1),
    );
    return current.length > 0 ? current : undefined;
  });
  assert(definitions.length > 0, "Go to definition returned no locations");

  const hovers = await waitFor("hover provider", async () => {
    const current = await vscode.commands.executeCommand<
      readonly vscode.Hover[]
    >(
      "vscode.executeHoverProvider",
      valid.uri,
      positionInLast(valid, "shade(", 1),
    );
    return current.length > 0 ? current : undefined;
  });
  assert(hovers.length > 0, "Hover returned no information");

  const signatures = await waitFor("signature help provider", async () => {
    const current = await vscode.commands.executeCommand<vscode.SignatureHelp>(
      "vscode.executeSignatureHelpProvider",
      valid.uri,
      positionInLast(valid, "shade(", "shade(".length),
      "(",
    );
    return current.signatures.length > 0 ? current : undefined;
  });
  assert(
    signatures.signatures.length > 0,
    "Signature help returned no signatures",
  );

  const invalid = await openFixture("invalid.hlsl");
  const diagnostics = await waitFor("DXC diagnostics", () => {
    const current = vscode.languages.getDiagnostics(invalid.uri);
    return current.some((item) => item.source === "dxc") ? current : undefined;
  });
  assert(
    diagnostics.some((item) => item.message.includes("does_not_exist")),
    "Expected unknown-identifier diagnostic was not published",
  );

  await vscode.commands.executeCommand("hlsl.restartServer");
  await waitFor("language-server restart", () =>
    api.state === "running" ? true : undefined,
  );
  const restartedCompletions = await waitFor(
    "completion after settings reapplication",
    async () => {
      try {
        const current =
          await vscode.commands.executeCommand<vscode.CompletionList>(
            "vscode.executeCompletionItemProvider",
            valid.uri,
            new vscode.Position(valid.lineCount - 1, 0),
          );
        return current.items.some((item) => item.label === "Number")
          ? current
          : undefined;
      } catch {
        return undefined;
      }
    },
  );
  assert(restartedCompletions.items.some((item) => item.label === "Number"));
  assert(
    !vscode.languages
      .getDiagnostics(valid.uri)
      .some((item) => item.message.includes("missing_editor_restart_setting")),
    "Editor configuration was not reapplied after restart",
  );

  const watched = await openFixture("watched.hlsl");
  await waitFor("watched fixture analysis", async () => {
    const symbols = await vscode.commands.executeCommand<
      readonly vscode.DocumentSymbol[]
    >("vscode.executeDocumentSymbolProvider", watched.uri);
    return symbols.some((symbol) => symbol.name === "watchedMain")
      ? true
      : undefined;
  });
  const folder = vscode.workspace.workspaceFolders?.[0];
  assert(folder);
  const dependency = vscode.Uri.joinPath(folder.uri, "watched.hlsli");
  const originalDependency = await vscode.workspace.fs.readFile(dependency);
  try {
    await vscode.workspace.fs.writeFile(
      dependency,
      new TextEncoder().encode("// watchedValue removed\n"),
    );
    const dependencyDiagnostics = await waitFor(
      "closed dependency change diagnostics",
      () => {
        const current = vscode.languages.getDiagnostics(watched.uri);
        return current.some((item) => item.message.includes("watchedValue"))
          ? current
          : undefined;
      },
    );
    assert(
      dependencyDiagnostics.some((item) =>
        item.message.includes("watchedValue"),
      ),
      "Changing a closed include dependency did not invalidate its open root",
    );
  } finally {
    await vscode.workspace.fs.writeFile(dependency, originalDependency);
  }

  await vscode.commands.executeCommand("hlsl.stopServer");
  await waitFor("clean language-server shutdown", () =>
    api.state === "stopped" ? true : undefined,
  );
}
