import assert from "node:assert/strict";
import test from "node:test";

import {
  analysisTargetTransition,
  defaultAnalysisTrackingMode,
  isReopenedAnalysisTarget,
} from "../../src/analysisTracking";

void test("analysis panels default to pinned targets", () => {
  assert.equal(defaultAnalysisTrackingMode, "pinned");
});

void test("pinned targets ignore active-editor changes", () => {
  assert.equal(
    analysisTargetTransition(
      "pinned",
      "file:///pinned.hlsl",
      "file:///active.hlsl",
      true,
    ),
    "unchanged",
  );
});

void test("follow mode retargets only for a different or newly available shader", () => {
  assert.equal(
    analysisTargetTransition(
      "follow",
      "file:///old.hlsl",
      "file:///active.hlsl",
      true,
    ),
    "retarget",
  );
  assert.equal(
    analysisTargetTransition(
      "follow",
      "file:///active.hlsl",
      "file:///active.hlsl",
      false,
    ),
    "retarget",
  );
  assert.equal(
    analysisTargetTransition(
      "follow",
      "file:///active.hlsl",
      "file:///active.hlsl",
      true,
    ),
    "unchanged",
  );
});

void test("follow mode reports an unavailable active target", () => {
  assert.equal(
    analysisTargetTransition(
      "follow",
      "file:///old.hlsl",
      undefined,
      true,
    ),
    "unavailable",
  );
});

void test("a matching reopened document restores an unavailable target", () => {
  assert.equal(
    isReopenedAnalysisTarget(
      "pinned",
      "file:///shader.hlsl",
      "file:///shader.hlsl",
      false,
    ),
    true,
  );
  assert.equal(
    isReopenedAnalysisTarget(
      "pinned",
      "file:///shader.hlsl",
      "file:///other.hlsl",
      false,
    ),
    false,
  );
  assert.equal(
    isReopenedAnalysisTarget(
      "pinned",
      "file:///shader.hlsl",
      "file:///shader.hlsl",
      true,
    ),
    false,
  );
  assert.equal(
    isReopenedAnalysisTarget(
      "follow",
      "file:///shader.hlsl",
      "file:///shader.hlsl",
      false,
    ),
    false,
  );
});
