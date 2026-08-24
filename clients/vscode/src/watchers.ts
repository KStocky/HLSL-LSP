import path from "node:path";

import { HlslServerSettings } from "./configuration";

export const shaderFileGlob = "**/*.{hlsl,hlsli,usf}";
export const configurationFileGlob = "**/shadertoolsconfig.json";

function pathKey(filePath: string): string {
  const normalized = path.normalize(filePath);
  return process.platform === "win32" ? normalized.toLowerCase() : normalized;
}

function isWithin(candidate: string, directory: string): boolean {
  const relative = path.relative(directory, candidate);
  return (
    relative === "" ||
    (relative !== ".." &&
      !relative.startsWith(`..${path.sep}`) &&
      !path.isAbsolute(relative))
  );
}

export function externalWatchDirectories(
  settings: HlslServerSettings,
  workspaceFolders: readonly string[],
): readonly string[] {
  const configuredDirectories: string[] = [];
  if (settings.additionalIncludeDirectories !== undefined) {
    configuredDirectories.push(...settings.additionalIncludeDirectories);
  }
  if (settings.virtualDirectoryMappings !== undefined) {
    configuredDirectories.push(
      ...Object.values(settings.virtualDirectoryMappings),
    );
  }

  const workspaceDirectories = workspaceFolders.map((folder) =>
    path.resolve(folder),
  );
  const result = new Map<string, string>();
  for (const configuredDirectory of configuredDirectories) {
    if (
      typeof configuredDirectory !== "string" ||
      configuredDirectory.trim() === ""
    ) {
      continue;
    }
    const directories = path.isAbsolute(configuredDirectory)
      ? [path.normalize(configuredDirectory)]
      : workspaceDirectories.map((workspace) =>
          path.resolve(workspace, configuredDirectory),
        );
    for (const directory of directories) {
      if (
        workspaceDirectories.some((workspace) => isWithin(directory, workspace))
      ) {
        continue;
      }
      const key = pathKey(directory);
      if (!result.has(key)) {
        result.set(key, directory);
      }
    }
  }
  return [...result.values()];
}
