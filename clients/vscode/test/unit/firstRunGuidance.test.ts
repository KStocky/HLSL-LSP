import assert from "node:assert/strict";
import test from "node:test";

import {
  shouldShowFirstRunGuidance,
  shouldShowHlslToolsConflict,
} from "../../src/firstRunGuidance";

void test("first-run guidance appears once for HLSL", () => {
  assert.equal(shouldShowFirstRunGuidance("hlsl", false), true);
  assert.equal(shouldShowFirstRunGuidance("hlsl", true), false);
  assert.equal(shouldShowFirstRunGuidance("cpp", false), false);
  assert.equal(shouldShowFirstRunGuidance(undefined, false), false);
});

void test("HLSL Tools conflict appears only when relevant", () => {
  assert.equal(shouldShowHlslToolsConflict("hlsl", false, true), true);
  assert.equal(shouldShowHlslToolsConflict("hlsl", true, true), false);
  assert.equal(shouldShowHlslToolsConflict("hlsl", false, false), false);
  assert.equal(shouldShowHlslToolsConflict("cpp", false, true), false);
});
