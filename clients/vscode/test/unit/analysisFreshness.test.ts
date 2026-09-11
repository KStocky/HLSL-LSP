import assert from "node:assert/strict";
import test from "node:test";

import {
  AnalysisFreshness,
  analysisConfigurationChange,
  analysisFreshnessText,
  initialAnalysisFreshness,
  reduceAnalysisFreshness,
  withAnalysisFreshness,
} from "../../src/analysisFreshness";

function configurationChange(...changed: string[]) {
  return analysisConfigurationChange((setting) => changed.includes(setting));
}

void test("configuration impact only invalidates analysis for compilation or restart settings", () => {
  assert.equal(configurationChange("hlsl.trace.server"), undefined);
  assert.equal(configurationChange("hlsl.inlayHints.types"), undefined);
  assert.deepEqual(configurationChange("hlsl.entryPoint"), {
    action: "refresh",
    cause: "Configuration change",
  });
  assert.deepEqual(configurationChange("hlsl.activeVariant"), {
    action: "refresh",
    cause: "Variant change",
  });
  assert.deepEqual(configurationChange("hlsl.server.path"), {
    action: "restart",
    cause: "Configuration change",
  });
  assert.deepEqual(
    configurationChange("hlsl.activeVariant", "hlsl.server.path"),
    {
      action: "restart",
      cause: "Configuration change",
    },
  );
});

void test("freshness reducer uses the shared deterministic states and causes", () => {
  const refreshing = reduceAnalysisFreshness(initialAnalysisFreshness, {
    type: "refresh-started",
    cause: "Manual refresh",
  });
  assert.deepEqual(refreshing, {
    status: "Refreshing",
    cause: "Manual refresh",
  });
  assert.deepEqual(
    reduceAnalysisFreshness(refreshing, { type: "refresh-succeeded" }),
    { status: "Current", cause: "Unknown" },
  );
  assert.deepEqual(
    reduceAnalysisFreshness(refreshing, { type: "refresh-failed" }),
    { status: "Refresh failed", cause: "Manual refresh" },
  );
  assert.deepEqual(
    reduceAnalysisFreshness(refreshing, {
      type: "invalidated",
      cause: "Source edit",
      refreshPending: true,
    }),
    { status: "Refreshing", cause: "Source edit" },
  );
  assert.deepEqual(
    reduceAnalysisFreshness(
      { status: "Current", cause: "Unknown" },
      {
        type: "invalidated",
        cause: "Disconnected server",
        refreshPending: false,
      },
    ),
    { status: "Stale", cause: "Disconnected server" },
  );
});

void test("superseded requests cannot regress freshness", () => {
  const freshness = new AnalysisFreshness();
  const first = freshness.beginRefresh("Source edit");
  const second = freshness.beginRefresh("Variant change");
  assert.equal(freshness.succeed(first), false);
  assert.deepEqual(freshness.state, {
    status: "Refreshing",
    cause: "Variant change",
  });
  assert.equal(freshness.succeed(second), true);
  assert.equal(freshness.state.status, "Current");
});

void test("invalidation rejects an in-flight result without flickering out of Refreshing", () => {
  const freshness = new AnalysisFreshness();
  const request = freshness.beginRefresh("Source edit");
  freshness.invalidate("Source edit", true);
  assert.deepEqual(freshness.state, {
    status: "Refreshing",
    cause: "Source edit",
  });
  assert.equal(freshness.succeed(request), false);
});

void test("status renderer is script-free, escaped, replaceable, and exposes only its refresh command", () => {
  const original =
    "<!doctype html><html><head></head><body><h1>Result</h1></body></html>";
  const stale = withAnalysisFreshness(
    original,
    { status: "Stale", cause: "Configuration change" },
    "hlsl.refreshMemoryLayout",
  );
  assert.match(stale, /Stale · Configuration change/);
  assert.match(stale, /command:hlsl\.refreshMemoryLayout/);
  assert.doesNotMatch(stale, /<script/i);

  const current = withAnalysisFreshness(
    stale,
    { status: "Current", cause: "Unknown" },
    "hlsl.refreshMemoryLayout",
  );
  assert.equal(current.match(/hlsl-analysis-freshness:start/g)?.length, 1);
  assert.equal(
    analysisFreshnessText({ status: "Current", cause: "Unknown" }),
    "Current",
  );
});
