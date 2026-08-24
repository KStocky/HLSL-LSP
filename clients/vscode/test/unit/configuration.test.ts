import assert from "node:assert/strict";
import test from "node:test";

import {
  ConfigurationInspection,
  ConfigurationReader,
  readDefaultLanguageVersion,
  readServerSettings,
} from "../../src/configuration";

class FakeConfiguration implements ConfigurationReader {
  public readonly values = new Map<string, unknown>();
  public readonly inspections = new Map<
    string,
    ConfigurationInspection<unknown>
  >();

  public get(section: string): unknown {
    return this.values.get(section);
  }

  public inspect(
    section: string,
  ): ConfigurationInspection<unknown> | undefined {
    return this.inspections.get(section);
  }
}

void test("schema defaults are client defaults, not editor overrides", () => {
  const reader = new FakeConfiguration();
  reader.values.set("languageVersion", "2021");
  reader.inspections.set("languageVersion", {});
  reader.inspections.set("additionalIncludeDirectories", {});

  assert.equal(readDefaultLanguageVersion(reader), "2021");
  assert.deepEqual(readServerSettings(reader), {});
});

void test("explicit empty editor values are preserved to clear inherited settings", () => {
  const reader = new FakeConfiguration();
  reader.inspections.set("preprocessorDefinitions", { workspaceValue: {} });
  reader.inspections.set("additionalIncludeDirectories", {
    workspaceFolderValue: [],
  });
  reader.inspections.set("additionalArguments", { globalValue: [] });

  assert.deepEqual(readServerSettings(reader), {
    preprocessorDefinitions: {},
    additionalIncludeDirectories: [],
    additionalArguments: [],
  });
});

void test("resource and language-specific settings follow VS Code precedence", () => {
  const reader = new FakeConfiguration();
  reader.inspections.set("targetProfile", {
    globalValue: "lib_6_0",
    workspaceValue: "ps_6_6",
    workspaceFolderLanguageValue: "cs_6_8",
  });

  assert.deepEqual(readServerSettings(reader), {
    targetProfile: "cs_6_8",
  });
});
