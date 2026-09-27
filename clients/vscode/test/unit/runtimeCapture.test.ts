import assert from "node:assert/strict";
import test from "node:test";

import {
  CaptureEntry,
  CaptureSnapshot,
  captureEntryIsTransient,
  capturePreviewMatches,
  captureSessionMatches,
  captureStage,
  captureSummary,
  captureVariantRequired,
  hostPipelineCandidates,
} from "../../src/runtimeCaptureCore";

const entry: CaptureEntry = {
  id: "sha256:one",
  invocation: {
    source: "Shaders/main.hlsl",
    entryPoint: "Main",
    targetProfile: "ps_6_7",
    defines: [["QUALITY", "HIGH"]],
  },
  count: 4,
  review: {
    eligible: true,
    requiresConfirmation: true,
    warningCodes: [],
    warnings: [],
    selection: {
      relativePath: "Shaders/main.hlsl",
      entryPoint: "Main",
      targetProfile: "ps_6_7",
    },
    fileGroup: { files: ["Shaders/main.hlsl"] },
  },
};

void test("capture filter excludes ineligible, warning-coded, and transient metadata", () => {
  assert.equal(captureEntryIsTransient(entry), false);
  assert.equal(
    captureEntryIsTransient({
      ...entry,
      review: { ...entry.review, eligible: false },
    }),
    true,
  );
  assert.equal(
    captureEntryIsTransient({
      ...entry,
      review: {
        ...entry.review,
        warningCodes: ["resolvedOutsideWorkspace"],
      },
    }),
    true,
  );
  assert.equal(
    captureEntryIsTransient({
      ...entry,
      invocation: {
        ...entry.invocation,
        arguments: ["-Fo", "build/cache/output.dxil"],
      },
    }),
    true,
  );
  assert.equal(
    captureEntryIsTransient({
      ...entry,
      invocation: {
        ...entry.invocation,
        defines: [["SESSION", "${SESSION_ID}"]],
      },
    }),
    true,
  );
});

void test("capture summary reports bounded counts and stopped snapshots", () => {
  const snapshot: CaptureSnapshot = {
    protocolVersion: 1,
    sessionId: "sha256:session",
    active: false,
    accepted: 10,
    rejected: 2,
    overflow: 3,
    entries: [entry],
  };
  assert.match(captureSummary(snapshot), /Stopped · 10 accepted/);
  assert.match(captureSummary(snapshot), /3 overflow · 1 distinct/);
});

void test("same-file captures require explicit and distinct variant names", () => {
  const second: CaptureEntry = {
    ...entry,
    id: "sha256:two",
    invocation: { ...entry.invocation, targetProfile: "vs_6_7" },
  };
  assert.deepEqual(
    [...captureVariantRequired([entry, second])],
    ["sha256:one", "sha256:two"],
  );
  assert.equal(captureVariantRequired([entry]).size, 0);
});

void test("host pipeline candidates require correlated unambiguous stage pairs", () => {
  const vertex: CaptureEntry = {
    ...entry,
    id: "sha256:vertex",
    invocation: {
      ...entry.invocation,
      targetProfile: "vs_6_7",
      pipeline: "Forward",
      stage: "vertex",
    },
  };
  const pixel: CaptureEntry = {
    ...entry,
    id: "sha256:pixel",
    invocation: { ...entry.invocation, pipeline: "Forward", stage: "pixel" },
  };
  assert.equal(captureStage(vertex), "vertex");
  assert.deepEqual(hostPipelineCandidates([vertex, pixel]), [
    {
      name: "Forward",
      source: "host",
      stages: { vertex: vertex.id, pixel: pixel.id },
    },
  ]);
  assert.deepEqual(
    hostPipelineCandidates([vertex, pixel, { ...vertex, id: "sha256:other" }]),
    [],
  );
  assert.deepEqual(
    hostPipelineCandidates([
      vertex,
      { ...pixel, invocation: { ...pixel.invocation, stage: "vertex" } },
    ]),
    [],
  );
});

void test("preview and apply reject stale sessions and mismatched entry IDs", () => {
  const sessionId = "sha256:session";
  const snapshot: CaptureSnapshot = {
    protocolVersion: 1,
    sessionId,
    active: true,
    accepted: 1,
    rejected: 0,
    overflow: 0,
    entries: [entry],
  };
  const preview = {
    protocolVersion: 1 as const,
    sessionId,
    selectedEntryIds: [entry.id],
    configuration: {
      uri: "file:///project/shadertoolsconfig.json",
      exists: false,
      expectedContentVersion: null,
      expectedContentHash: null,
    },
    preview: {
      content: "{}",
      contentHash: "sha256:preview",
      valid: true,
      changed: true,
      errors: [],
    },
  };
  assert(
    capturePreviewMatches(
      snapshot,
      [entry],
      preview.configuration.uri,
      preview,
    ),
  );
  assert(
    capturePreviewMatches(
      snapshot,
      [entry],
      "file:///C:/Shader%20Workspace/shadertoolsconfig.json",
      {
        ...preview,
        configuration: {
          ...preview.configuration,
          uri: "file:///c:/Shader%20Workspace/shadertoolsconfig.json",
        },
      },
    ) ===
      (process.platform === "win32"),
  );
  assert(
    !capturePreviewMatches(
      snapshot,
      [entry],
      "file:///other/shadertoolsconfig.json",
      preview,
    ),
  );
  assert(
    !capturePreviewMatches(snapshot, [entry], preview.configuration.uri, {
      ...preview,
      configuration: { ...preview.configuration, uri: "file:///bad%FF" },
    }),
  );
  assert(
    !capturePreviewMatches(snapshot, [entry], preview.configuration.uri, {
      ...preview,
      selectedEntryIds: ["sha256:other"],
    }),
  );
  assert(
    !capturePreviewMatches(snapshot, [entry], preview.configuration.uri, {
      ...preview,
      sessionId: "sha256:stale",
    }),
  );
  assert(captureSessionMatches(sessionId, snapshot));
  assert(
    !captureSessionMatches(sessionId, {
      active: true,
      sessionId: "sha256:new",
    }),
  );
  assert(!captureSessionMatches(sessionId, { active: false, sessionId }));
});
