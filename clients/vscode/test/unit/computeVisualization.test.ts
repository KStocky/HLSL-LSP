import assert from "node:assert/strict";
import test from "node:test";

import {
  ComputeVisualization,
  computeVisualizationHtml,
  computeVisualizationRequestParams,
  configureComputeVisualizationCommand,
  openComputeVisualizationLocationCommand,
  parseComputeVisualizationLocation,
  parsePositiveDimension,
  resolveComputeVisualizationRefresh,
} from "../../src/computeVisualization";

const uri = "file:///c:/shaders/compute.hlsl";

function report(
  overrides: Partial<ComputeVisualization> = {},
): ComputeVisualization {
  return {
    applicable: true,
    explanation: "",
    entryPoint: "main",
    stage: "compute",
    targetProfile: "cs_6_6",
    threadGroupSize: { x: 8, y: 4, z: 1 },
    dispatchDimensions: { x: 17, y: 8, z: 1 },
    groupCount: { x: 3, y: 2, z: 1 },
    launchedThreads: 192,
    inactiveThreads: 56,
    systemValues: [
      {
        semantic: "SV_DispatchThreadID",
        name: "dispatchThreadId",
        formula: "groupId * numthreads + groupThreadId",
        description: "Global dispatch-space thread coordinate.",
      },
    ],
    barriers: {
      available: true,
      unavailableReason: "",
      instructionCount: 1,
      locationsAvailable: true,
      locationsUnavailableReason: "",
      locationsTruncated: false,
      locations: [
        {
          label: "GroupMemoryBarrierWithGroupSync",
          uri,
          range: {
            start: { line: 8, character: 2 },
            end: { line: 8, character: 34 },
          },
        },
      ],
    },
    groupShared: {
      available: true,
      unavailableReason: "",
      totalBytes: 128,
      totalBytesUnavailableReason: "",
      truncated: false,
      declarations: [
        {
          name: "tile",
          type: "float[32]",
          declaration: "groupshared float tile[32]",
          bytes: 128,
          sizeUnavailableReason: "",
          uri,
          range: {
            start: { line: 1, character: 0 },
            end: { line: 1, character: 28 },
          },
        },
      ],
    },
    waveSize: {
      known: false,
      min: null,
      max: null,
      preferred: null,
      minMaxSource: null,
      preferredSource: null,
      explanation: "No compiler-authoritative wave-size requirement.",
    },
    occupancy: null,
    ...overrides,
  };
}

void test("compute visualization request includes only explicitly configured inputs", () => {
  assert.deepEqual(computeVisualizationRequestParams(uri), {
    textDocument: { uri },
  });
  assert.deepEqual(
    computeVisualizationRequestParams(uri, {
      dispatchDimensions: { x: 1920, y: 1080, z: 1 },
      hardwareProfile: {
        name: "Explicit test GPU",
        waveSize: 32,
        maxThreadsPerGroup: 1024,
        maxThreadsPerComputeUnit: 2048,
        maxGroupsPerComputeUnit: 32,
        sharedMemoryBytesPerComputeUnit: 65536,
      },
    }),
    {
      textDocument: { uri },
      dispatchDimensions: { x: 1920, y: 1080, z: 1 },
      hardwareProfile: {
        name: "Explicit test GPU",
        waveSize: 32,
        maxThreadsPerGroup: 1024,
        maxThreadsPerComputeUnit: 2048,
        maxGroupsPerComputeUnit: 32,
        sharedMemoryBytesPerComputeUnit: 65536,
      },
    },
  );
});

void test("positive dimension parsing rejects unsafe or malformed values", () => {
  assert.equal(parsePositiveDimension("1"), 1);
  assert.equal(parsePositiveDimension("4294967295"), 0xffffffff);
  for (const value of ["", "0", "-1", "1.5", " 8", "8 ", "4294967296"]) {
    assert.equal(parsePositiveDimension(value), undefined);
  }
});

void test("compute visualization renders geometry and explicit unknown sections", () => {
  const html = computeVisualizationHtml(report(), uri);
  assert.match(html, /8 x 4 x 1/);
  assert.match(html, /17 x 8 x 1/);
  assert.match(html, /Logical workload \(threads\)/);
  assert.match(html, /Required Dispatch\(\) groups/);
  assert.match(html, /192/);
  assert.match(html, /56/);
  assert.match(html, /SV_DispatchThreadID/);
  assert.match(html, /Total estimated bytes: 128/);
  assert.match(html, /Compiler instruction count: 1/);
  assert.match(html, /No compiler-authoritative wave-size requirement/);
  assert.match(html, /Occupancy is intentionally not guessed/);
  assert.match(
    html,
    new RegExp(`command:${configureComputeVisualizationCommand}`),
  );
  assert.match(
    html,
    new RegExp(`command:${openComputeVisualizationLocationCommand}`),
  );
});

void test("compute visualization keeps configuration available when analysis is not applicable", () => {
  const html = computeVisualizationHtml(
    report({
      applicable: false,
      explanation: "The active target is a pixel shader.",
    }),
    uri,
  );
  assert.match(html, /active target is a pixel shader/);
  assert.match(
    html,
    new RegExp(`command:${configureComputeVisualizationCommand}`),
  );
  assert.doesNotMatch(html, /System-value mapping/);
});

void test("zero compiler barriers is distinct from unavailable source locations", () => {
  const noBarriers = computeVisualizationHtml(
    report({
      barriers: {
        available: true,
        unavailableReason: "",
        instructionCount: 0,
        locationsAvailable: false,
        locationsUnavailableReason: "DXC does not expose locations.",
        locationsTruncated: false,
        locations: [],
      },
    }),
    uri,
  );
  assert.match(noBarriers, /no barrier instructions/);
  assert.doesNotMatch(noBarriers, /locations are not available/);

  const unknownLocations = computeVisualizationHtml(
    report({
      barriers: {
        available: true,
        unavailableReason: "",
        instructionCount: 2,
        locationsAvailable: false,
        locationsUnavailableReason: "Exact compiler reason.",
        locationsTruncated: false,
        locations: [],
      },
    }),
    uri,
  );
  assert.match(unknownLocations, /Exact compiler reason/);
});

void test("compute visualization labels occupancy as a hardware estimate", () => {
  const html = computeVisualizationHtml(
    report({
      occupancy: {
        hardwareProfile: "Explicit test GPU",
        estimatedResidentGroups: 8,
        estimatedResidentThreads: 256,
        estimatedResidentWaves: 8,
        limitingFactors: ["shared memory"],
        assumptions: ["register pressure is not available"],
      },
    }),
    uri,
  );
  assert.match(html, /Hardware-dependent occupancy/);
  assert.match(html, /not compiler- or device-runtime-measured/);
  assert.match(html, /shared memory/);
  assert.match(html, /register pressure is not available/);
});

void test("compute visualization escapes compiler and profile text", () => {
  const html = computeVisualizationHtml(
    report({
      entryPoint: "<script>alert(1)</script>",
      occupancy: {
        hardwareProfile: "<img src=x onerror=alert(1)>",
        estimatedResidentGroups: 1,
        estimatedResidentThreads: 32,
        estimatedResidentWaves: 1,
        limitingFactors: ["<script>"],
        assumptions: [],
      },
    }),
    uri,
  );
  assert.doesNotMatch(html, /<script>/);
  assert.doesNotMatch(html, /<img /);
  assert.match(html, /&lt;script&gt;/);
});

void test("compute location parser validates ranges", () => {
  const location = {
    uri,
    range: {
      start: { line: 2, character: 3 },
      end: { line: 2, character: 9 },
    },
  };
  assert.deepEqual(parseComputeVisualizationLocation(location), location);
  assert.equal(
    parseComputeVisualizationLocation({
      ...location,
      range: { start: location.range.end, end: location.range.start },
    }),
    undefined,
  );
  assert.equal(parseComputeVisualizationLocation({ uri }), undefined);
});

void test("compute refresh preserves successful content through failure", () => {
  const success = resolveComputeVisualizationRefresh(
    false,
    uri,
    report(),
    undefined,
  );
  assert.equal(success.hasContent, true);
  assert.match(success.html ?? "", /Compute Visualization/);

  const failure = resolveComputeVisualizationRefresh(
    true,
    uri,
    null,
    "cancelled",
  );
  assert.deepEqual(failure, {
    html: undefined,
    hasContent: true,
    title: undefined,
  });
});
