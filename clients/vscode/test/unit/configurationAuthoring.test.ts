import assert from "node:assert/strict";
import test from "node:test";

import {
  configurationCandidates,
  configurationContentHash,
  configurationDraftInitialContent,
  configurationDraftTooLarge,
  configurationIsStale,
  formatConfigurationErrors,
  type ConfigurationAuthoringResult,
} from "../../src/configurationAuthoringCore";

const result = {
  protocolVersion: 1,
  configuration: {
    uri: "file:///workspace/shadertoolsconfig.json",
    exists: false,
    expectedContentVersion: null,
    expectedContentHash: null,
  },
  discovery: {
    files: [
      {
        uri: "file:///workspace/Shaders/main.hlsl",
        relativePath: "Shaders/main.hlsl",
        entryPoints: [
          {
            name: "Main",
            location: { line: 3, character: 5 },
            targetProfiles: ["cs_6_6"],
            state: "resolved",
            explanation: "Exactly one profile compiled.",
          },
          {
            name: "Shared",
            location: { line: 8, character: 5 },
            targetProfiles: ["vs_6_6", "ps_6_6"],
            state: "ambiguous",
            explanation: "Multiple profiles compiled.",
          },
          {
            name: "Broken",
            location: { line: 12, character: 5 },
            targetProfiles: [],
            state: "unresolved",
            explanation: "No profile compiled.",
          },
        ],
        truncated: false,
        analysisError: null,
      },
    ],
    nestedConfigurations: [],
    directoriesVisited: 2,
    compilerProbes: 3,
    truncated: false,
    truncationReason: null,
  },
  preview: {
    content: "{}\n",
    contentHash: "sha256:preview",
    valid: true,
    changed: true,
    errors: [],
  },
} as const satisfies ConfigurationAuthoringResult;

void test("configuration authoring hashes UTF-8 content deterministically", () => {
  assert.equal(
    configurationContentHash("{}"),
    "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
  );
});

void test("configuration candidates preselect only unambiguous profiles", () => {
  const candidates = configurationCandidates(result);
  assert.equal(candidates.length, 3);
  assert.deepEqual(
    candidates.map((candidate) => [
      candidate.selection.entryPoint,
      candidate.selection.targetProfile,
      candidate.picked,
    ]),
    [
      ["Main", "cs_6_6", true],
      ["Shared", "vs_6_6", false],
      ["Shared", "ps_6_6", false],
    ],
  );
  const ambiguous = candidates[1];
  assert(ambiguous);
  assert.match(ambiguous.detail, /Choice required/);
});

void test("configuration stale checks compare both version and content hash", () => {
  const content = '{\n  "root": true\n}\n';
  const hash = configurationContentHash(content);
  assert.equal(configurationIsStale(7, hash, 7, content), false);
  assert.equal(configurationIsStale(7, hash, 8, content), true);
  assert.equal(configurationIsStale(7, hash, 7, `${content} `), true);
  assert.equal(configurationIsStale(null, null, 99, content), false);
});

void test("configuration validation errors retain field addresses and codes", () => {
  assert.equal(
    formatConfigurationErrors([
      {
        field: "hlsl.fileGroups[0].hlsl.targetProfile",
        message: "Unsupported target profile.",
        code: "invalid_target_profile",
      },
    ]),
    "hlsl.fileGroups[0].hlsl.targetProfile: Unsupported target profile. [invalid_target_profile]",
  );
});

void test("invalid generated previews retain malformed original content for repair", () => {
  assert.equal(
    configurationDraftInitialContent(
      {
        ...result,
        preview: { ...result.preview, valid: false, content: "{}" },
      },
      '{"hlsl":',
    ),
    '{"hlsl":',
  );
  assert.equal(
    configurationDraftInitialContent(result, '{"old":true}'),
    "{}\n",
  );
});

void test("draft limit measures UTF-8 bytes, including multibyte characters", () => {
  assert.equal(configurationDraftTooLarge("a".repeat(2 * 1024 * 1024)), false);
  assert.equal(configurationDraftTooLarge("é".repeat(1024 * 1024) + "a"), true);
});
