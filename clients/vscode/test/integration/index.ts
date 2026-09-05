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
  if (serverPath !== undefined) {
    await vscode.workspace
      .getConfiguration("hlsl")
      .update("server.path", serverPath, vscode.ConfigurationTarget.Global);
  }
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

  const variantDirectory = vscode.Uri.joinPath(folder.uri, "variants");
  const variantConfig = vscode.Uri.joinPath(
    variantDirectory,
    "shadertoolsconfig.json",
  );
  const variantShader = vscode.Uri.joinPath(
    variantDirectory,
    "variantShader.hlsl",
  );
  const encoder = new TextEncoder();
  await vscode.workspace.fs.createDirectory(variantDirectory);
  await vscode.workspace.fs.writeFile(
    variantConfig,
    encoder.encode(
      JSON.stringify({
        root: true,
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [
          {
            name: "Alpha",
            description: "Alpha permutation",
            "hlsl.additionalArguments": ["-D", "VARIANT_ALPHA=1"],
          },
          {
            name: "Beta",
            "hlsl.additionalArguments": ["-D", "VARIANT_BETA=1"],
          },
        ],
      }),
    ),
  );
  await vscode.workspace.fs.writeFile(
    variantShader,
    encoder.encode(
      "float4 Main() : SV_Target {\n" +
        "#if defined(VARIANT_ALPHA)\n    return marker_alpha;\n" +
        "#elif defined(VARIANT_BETA)\n    return marker_beta;\n" +
        "#else\n    return marker_none;\n#endif\n" +
        "}\n",
    ),
  );
  const setActiveVariant = (value: string): Thenable<void> =>
    vscode.workspace
      .getConfiguration("hlsl")
      .update("activeVariant", value, vscode.ConfigurationTarget.Global);
  const waitForVariantMarker = (marker: string): Promise<unknown> =>
    waitFor(`variant reanalysis for ${marker}`, () =>
      vscode.languages
        .getDiagnostics(variantShader)
        .some((item) => item.message.includes(marker))
        ? true
        : undefined,
    );
  try {
    await setActiveVariant("Alpha");
    const variantDocument =
      await vscode.workspace.openTextDocument(variantShader);
    await vscode.window.showTextDocument(variantDocument);
    // The active variant's macros must reach DXC; switching variants must
    // reanalyze the open document with the new definitions.
    await waitForVariantMarker("marker_alpha");
    await setActiveVariant("Beta");
    await waitForVariantMarker("marker_beta");
    await setActiveVariant("");
    await waitForVariantMarker("marker_none");
  } finally {
    await setActiveVariant("");
    await vscode.workspace.fs.delete(variantDirectory, { recursive: true });
  }

  const compilationDirectory = vscode.Uri.joinPath(
    folder.uri,
    "compilation-info",
  );
  const compilationConfig = vscode.Uri.joinPath(
    compilationDirectory,
    "shadertoolsconfig.json",
  );
  const compilationShader = vscode.Uri.joinPath(
    compilationDirectory,
    "shader.hlsl",
  );
  await vscode.workspace.fs.createDirectory(compilationDirectory);
  await vscode.workspace.fs.writeFile(
    compilationConfig,
    encoder.encode(
      JSON.stringify({
        root: true,
        "hlsl.targetProfile": "ps_6_6",
        "hlsl.entryPoint": "PSMain",
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [
          {
            name: "Tinted",
            // additionalArguments (not preprocessorDefinitions) so the
            // variant's define survives the global preprocessorDefinitions
            // editor override this run applies for every HLSL document.
            "hlsl.additionalArguments": ["-D", "USE_TINT=1"],
          },
        ],
      }),
    ),
  );
  await vscode.workspace.fs.writeFile(
    compilationShader,
    encoder.encode(
      "Texture2D<float4> AlbedoTexture : register(t0);\n" +
        "float4 PSMain(float4 position : SV_Position) : SV_Target {\n" +
        "#if defined(USE_TINT)\n    return float4(1.0, 0.0, 0.0, 1.0) * AlbedoTexture.Load(int3(0, 0, 0));\n" +
        "#else\n    return float4(0.0, 1.0, 0.0, 1.0) * AlbedoTexture.Load(int3(0, 0, 0));\n#endif\n" +
        "}\n",
    ),
  );
  try {
    const compilationDocument =
      await vscode.workspace.openTextDocument(compilationShader);
    let compilationEditor =
      await vscode.window.showTextDocument(compilationDocument);

    // A compilation request racing an edit or variant change can be
    // superseded by the server (reported as a cancelled/content-modified
    // error); the poll simply retries rather than failing the whole run.
    const pollCompilationInfo = async () => {
      try {
        return await api.requestCompilationInfo(compilationShader);
      } catch {
        return undefined;
      }
    };

    const initialInfo = await waitFor(
      "initial hlsl/compilationInfo result",
      async () => {
        const current = await pollCompilationInfo();
        return current?.success === true ? current : undefined;
      },
    );
    assert.strictEqual(initialInfo.entryPoint, "PSMain");
    assert.strictEqual(initialInfo.stage, "pixel");
    assert.strictEqual(initialInfo.targetProfile, "ps_6_6");
    assert.strictEqual(initialInfo.activeVariant, null);
    assert(
      !initialInfo.defines.some((define) => define.startsWith("USE_TINT")),
      "USE_TINT should not be defined before selecting the Tinted variant",
    );
    assert(initialInfo.output?.type === "dxil");
    assert(
      initialInfo.reflection?.available,
      "DXIL output should report available reflection",
    );
    assert(
      Array.isArray(initialInfo.reflection.bindingAnalysis.groups),
      "reflection.bindingAnalysis.groups should be present for available reflection",
    );
    assert(
      Array.isArray(initialInfo.reflection.bindingAnalysis.collisions),
      "reflection.bindingAnalysis.collisions should be present for available reflection",
    );
    assert.strictEqual(
      initialInfo.rootSignature?.availability,
      "absent",
      "This fixture shader has no embedded root signature",
    );
    assert(
      typeof initialInfo.compatibility?.status === "string" &&
        initialInfo.compatibility.explanation.length > 0,
      "compatibility should report a status and a non-empty explanation even without a root signature",
    );

    // The fixture declares exactly one resource with a single, unambiguous
    // declaration in the current document, so DXC's reflection should
    // resolve a sourceLocation for it that points back at this same file.
    const albedoResource = initialInfo.reflection.resources.find(
      (resource) => resource.name === "AlbedoTexture",
    );
    assert(
      albedoResource !== undefined,
      "AlbedoTexture should be reflected as a bound resource",
    );
    assert(
      albedoResource.sourceLocation !== null,
      "AlbedoTexture has exactly one declaration, so its sourceLocation should not be null",
    );
    const albedoLocation = albedoResource.sourceLocation;
    assert.strictEqual(
      vscode.Uri.parse(albedoLocation.uri).fsPath,
      compilationShader.fsPath,
      "AlbedoTexture's sourceLocation should point back at the fixture document",
    );
    assert.strictEqual(albedoLocation.range.start.line, 0);
    assert(
      albedoLocation.range.end.line >= albedoLocation.range.start.line &&
        (albedoLocation.range.end.line > albedoLocation.range.start.line ||
          albedoLocation.range.end.character >=
            albedoLocation.range.start.character),
      "sourceLocation range should not be inverted",
    );

    // Exercising the actual navigation command end-to-end (not just the
    // pure validation helpers covered by unit tests) confirms the webview's
    // command-URI handler really opens and selects the declaration.
    await vscode.commands.executeCommand(
      "hlsl.resourceBindings.openLocation",
      albedoLocation,
    );
    const navigatedEditor = await waitFor(
      "editor navigation to AlbedoTexture's declaration",
      () => {
        const editor = vscode.window.activeTextEditor;
        return editor?.document.uri.fsPath === compilationShader.fsPath
          ? editor
          : undefined;
      },
    );
    assert.strictEqual(
      navigatedEditor.selection.start.line,
      albedoLocation.range.start.line,
      "The navigation command should select the declaration's start line",
    );
    // Reassign the tracked editor to the one navigation just made active, so
    // the later unsaved-edit step below does not operate on a stale
    // TextEditor handle for a tab that navigation may have refocused.
    compilationEditor = navigatedEditor;

    // The Resource Bindings command must reuse the same hlsl/compilationInfo
    // request and open its own independently tracked panel without throwing,
    // regardless of the order in which it is invoked relative to Shader
    // Compilation.
    await vscode.commands.executeCommand("hlsl.showResourceBindings");
    await vscode.commands.executeCommand("hlsl.showCompilationInfo");

    // Selecting a variant must be reflected on the next request without
    // reopening the document or restarting the server.
    await setActiveVariant("Tinted");
    const variantInfo = await waitFor(
      "hlsl/compilationInfo after selecting a variant",
      async () => {
        const current = await pollCompilationInfo();
        return current?.activeVariant === "Tinted" ? current : undefined;
      },
    );
    assert(
      variantInfo.defines.includes("USE_TINT=1"),
      "The active variant's defines were not applied to hlsl/compilationInfo",
    );

    // An unsaved edit must be reflected without saving the document, since the
    // server analyzes the client's current in-memory snapshot.
    await compilationEditor.edit((builder) => {
      const end = compilationDocument.lineAt(compilationDocument.lineCount - 1)
        .range.end;
      builder.insert(end, "\nfloat broken = does_not_exist;\n");
    });
    const unsavedInfo = await waitFor(
      "hlsl/compilationInfo after an unsaved edit",
      async () => {
        const current = await pollCompilationInfo();
        return current?.success === false ? current : undefined;
      },
    );
    assert(
      unsavedInfo.diagnostics.some((diagnostic) =>
        diagnostic.message.includes("does_not_exist"),
      ),
      "The unsaved edit was not reflected in hlsl/compilationInfo diagnostics",
    );
  } finally {
    await setActiveVariant("");
    await vscode.commands.executeCommand("workbench.action.files.revert");
    await vscode.workspace.fs.delete(compilationDirectory, {
      recursive: true,
    });
  }

  await vscode.commands.executeCommand("hlsl.stopServer");
  await waitFor("clean language-server shutdown", () =>
    api.state === "stopped" ? true : undefined,
  );
}
