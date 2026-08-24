import assert from "node:assert/strict";
import path from "node:path";
import test from "node:test";

import {
  configurationFileGlob,
  externalWatchDirectories,
  shaderFileGlob,
} from "../../src/watchers";

void test("watcher globs cover supported shader and configuration files only", () => {
  assert.equal(shaderFileGlob, "**/*.{hlsl,hlsli,usf}");
  assert.equal(configurationFileGlob, "**/shadertoolsconfig.json");
  assert.notEqual(shaderFileGlob, "**/*");
});

void test("external editor include and mapping directories get targeted watchers", () => {
  const firstWorkspace = path.resolve("workspace-a");
  const secondWorkspace = path.resolve("workspace-b");
  const shared = path.resolve("shared");
  const mapped = path.resolve("mapped");

  const directories = externalWatchDirectories(
    {
      additionalIncludeDirectories: [
        path.join(firstWorkspace, "includes"),
        path.join("..", "shared"),
        shared,
      ],
      virtualDirectoryMappings: {
        "/Internal": path.join(secondWorkspace, "virtual"),
        "/External": mapped,
      },
    },
    [firstWorkspace, secondWorkspace],
  );

  assert.deepEqual(
    new Set(directories.map((directory) => path.normalize(directory))),
    new Set([shared, mapped]),
  );
});

void test("relative external paths are resolved for each workspace folder", () => {
  const firstWorkspace = path.resolve("projects", "first");
  const secondWorkspace = path.resolve("other", "second");

  const directories = externalWatchDirectories(
    {
      additionalIncludeDirectories: [path.join("..", "shared")],
    },
    [firstWorkspace, secondWorkspace],
  );

  assert.deepEqual(directories, [
    path.resolve(firstWorkspace, "..", "shared"),
    path.resolve(secondWorkspace, "..", "shared"),
  ]);
});
