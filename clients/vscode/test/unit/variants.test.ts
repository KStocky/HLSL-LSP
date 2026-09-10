import assert from "node:assert/strict";
import test from "node:test";

import { applicableVariants, VariantInfo } from "../../src/variants";

const variant = (
  name: string,
  applicable: boolean,
  description = "",
): VariantInfo => ({
  name,
  description,
  default: false,
  applicable,
});

void test("variant picker excludes variants unrelated to the active document", () => {
  const variants = [
    variant("PixelDebug", true),
    variant("ComputeDebug", false),
    variant("PixelRelease", true),
  ];

  assert.deepEqual(
    applicableVariants(variants).map((item) => item.name),
    ["PixelDebug", "PixelRelease"],
  );
});

void test("variant filtering leaves the server response unchanged", () => {
  const variants = [variant("Applicable", true), variant("OtherFile", false)];

  applicableVariants(variants);

  assert.deepEqual(
    variants.map((item) => item.name),
    ["Applicable", "OtherFile"],
  );
});

void test("variant filtering represents an empty applicable set", () => {
  assert.deepEqual(
    applicableVariants([
      variant("VertexOnly", false),
      variant("ComputeOnly", false),
    ]),
    [],
  );
});
