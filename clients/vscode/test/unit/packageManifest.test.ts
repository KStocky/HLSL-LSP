import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

interface MenuEntry {
  readonly command?: string;
  readonly submenu?: string;
  readonly when?: string;
  readonly group?: string;
}

interface PackageManifest {
  readonly activationEvents: readonly string[];
  readonly contributes: {
    readonly jsonValidation: readonly {
      readonly fileMatch: readonly string[];
      readonly url: string;
    }[];
    readonly commands: readonly {
      readonly command: string;
      readonly shortTitle?: string;
    }[];
    readonly submenus: readonly {
      readonly id: string;
      readonly label: string;
    }[];
    readonly menus: Readonly<Record<string, readonly MenuEntry[]>>;
  };
}

const manifest = JSON.parse(
  readFileSync(join(__dirname, "../../../package.json"), "utf8"),
) as PackageManifest;

void test("restorable analysis panels activate their serializers", () => {
  assert.deepEqual(
    manifest.activationEvents.filter((event) =>
      event.startsWith("onWebviewPanel:hlsl"),
    ),
    [
      "onWebviewPanel:hlslMemoryLayout",
      "onWebviewPanel:hlslCompilationInfo",
      "onWebviewPanel:hlslResourceBindings",
      "onWebviewPanel:hlslPreprocessorExplorer",
      "onWebviewPanel:hlslEntryPointDataFlow",
      "onWebviewPanel:hlslComputeVisualization",
    ],
  );
});

void test("package contributes an HLSL-only editor context submenu", () => {
  assert.deepEqual(manifest.contributes.submenus, [
    { id: "hlsl.editorContext", label: "HLSL" },
  ]);
  assert.deepEqual(manifest.contributes.menus["editor/context"], [
    {
      submenu: "hlsl.editorContext",
      when: "editorLangId == hlsl",
      group: "navigation@50",
    },
  ]);
});

void test("package contributes JSON validation for shadertoolsconfig.json", () => {
  assert.deepEqual(manifest.contributes.jsonValidation, [
    {
      fileMatch: ["**/shadertoolsconfig.json"],
      url: "./schemas/v1/shadertoolsconfig.schema.json",
    },
  ]);
});

void test("HLSL submenu exposes every custom shader workflow in stable groups", () => {
  const entries = manifest.contributes.menus["hlsl.editorContext"];
  assert(entries);
  assert.deepEqual(
    entries.map((entry) => [entry.command, entry.group]),
    [
      ["hlsl.openEffectiveConfigurationFile", "1_configuration@0"],
      ["hlsl.selectVariant", "1_configuration@1"],
      ["hlsl.showCompilationInfo", "1_configuration@2"],
      ["hlsl.showPreprocessorExplorer", "1_configuration@3"],
      ["hlsl.expandMacro", "2_inspection@0"],
      ["hlsl.showMemoryLayout", "2_inspection@1"],
      ["hlsl.showResourceBindings", "2_inspection@2"],
      ["hlsl.showComputeVisualization", "2_inspection@3"],
      ["hlsl.showEntryPointDataFlow", "3_analysis@1"],
    ],
  );
  assert.equal(
    entries.find((entry) => entry.command === "hlsl.expandMacro")?.when,
    "editorLangId == hlsl && hlsl.macroExpansionAvailable",
  );
  assert(
    entries
      .filter((entry) => entry.command !== "hlsl.expandMacro")
      .every((entry) => entry.when === "editorLangId == hlsl"),
  );
  assert(
    entries.every((entry) => entry.command !== "hlsl.showCallHierarchy"),
    "VS Code's native call hierarchy must not be duplicated",
  );
});

void test("HLSL submenu uses concise command labels", () => {
  const shortTitles = new Map(
    manifest.contributes.commands.map((command) => [
      command.command,
      command.shortTitle,
    ]),
  );

  assert.deepEqual(
    manifest.contributes.menus["hlsl.editorContext"]?.map((entry) =>
      shortTitles.get(entry.command ?? ""),
    ),
    [
      "Open Effective Configuration",
      "Select Shader Variant",
      "Shader Compilation",
      "Preprocessor Explorer",
      "Expand Macro",
      "Memory Layout",
      "Resource Bindings",
      "Compute Visualization",
      "Entry-Point Data Flow",
    ],
  );
});
