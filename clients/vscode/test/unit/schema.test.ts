import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";
import Ajv2020 from "ajv/dist/2020";

const packagedSchemaText = readFileSync(
  join(__dirname, "../../../schemas/v1/shadertoolsconfig.schema.json"),
  "utf8",
);
const authoritativeSchemaText = readFileSync(
  join(__dirname, "../../../../../schemas/v1/shadertoolsconfig.schema.json"),
  "utf8",
);
const schema = JSON.parse(packagedSchemaText) as {
  readonly $id: string;
  readonly properties: Record<string, unknown>;
  readonly $defs: Record<string, unknown>;
};
const validate = new Ajv2020({ allErrors: true }).compile(schema);

void test("shadertoolsconfig schema exposes root, fileGroups, and variants", () => {
  assert.equal(
    schema.$id,
    "https://raw.githubusercontent.com/KStocky/HLSL-LSP/main/schemas/v1/shadertoolsconfig.schema.json",
  );
  assert(schema.properties["hlsl.fileGroups"]);
  assert(schema.properties["hlsl.variantsVersion"]);
  assert(schema.properties["hlsl.variants"]);
  assert(schema.$defs.fileGroup);
  assert(schema.$defs.variant);
});

void test("packaged schema matches the authoritative repository schema", () => {
  assert.deepEqual(
    JSON.parse(packagedSchemaText),
    JSON.parse(authoritativeSchemaText),
  );
});

void test("shadertoolsconfig schema accepts representative supported settings", () => {
  assert.equal(
    validate({
      $schema: schema.$id,
      root: true,
      "hlsl.preprocessorDefinitions": {
        FEATURE: true,
        COUNT: 4,
        HEADER: '"Project/Common.hlsli"',
      },
      "hlsl.additionalIncludeDirectories": ["", "../Engine/Shaders"],
      "hlsl.virtualDirectoryMappings": {
        "/Project": "Shaders",
      },
      "hlsl.languageVersion": "2017",
      "hlsl.targetProfile": "cs_6_7",
      "hlsl.entryPoint": "Main",
      "hlsl.additionalArguments": ["-enable-16bit-types"],
      "hlsl.fileGroups": [
        {
          name: "Compute",
          files: ["Compute/**/*.hlsl"],
          "hlsl.targetProfile": "cs_6_7",
        },
      ],
      "hlsl.variantsVersion": 1,
      "hlsl.variants": [
        {
          name: "Debug",
          description: "Debug shader variant",
          default: true,
          inherits: ["Base[0]"],
          files: ["**/*.hlsl"],
          "hlsl.preprocessorDefinitions": { DEBUG: 1 },
          "hlsl.dxcRuntimeDirectory": "tools/dxc",
        },
      ],
    }),
    true,
    JSON.stringify(validate.errors),
  );
  for (const invalidPattern of [
    "../shader.hlsl",
    "/shader.hlsl",
    "foo[0].hlsl",
    "foo/**bar.hlsl",
  ]) {
    assert.equal(
      validate({
        "hlsl.fileGroups": [{ files: [invalidPattern] }],
      }),
      false,
      invalidPattern,
    );
    assert.equal(
      validate({
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [{ name: "Debug", files: [invalidPattern] }],
      }),
      false,
      `variant: ${invalidPattern}`,
    );
  }
});

void test("shadertoolsconfig schema rejects common structural mistakes", () => {
  assert.equal(
    validate({
      "hlsl.variants": [{ name: "Debug" }],
    }),
    false,
  );
  assert.equal(
    validate({
      "hlsl.fileGroups": [{ files: [], unsupported: true }],
    }),
    false,
  );
  assert.equal(
    validate({
      "hlsl.preprocessorDefinitions": { INVALID: null },
    }),
    false,
  );
});
