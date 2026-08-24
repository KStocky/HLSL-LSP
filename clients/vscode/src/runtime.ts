import { constants } from "node:fs";
import { access, stat } from "node:fs/promises";
import path from "node:path";

export interface RuntimeFileSystem {
  access(filePath: string, mode: number): Promise<void>;
  isFile(filePath: string): Promise<boolean>;
}

export interface RuntimeEnvironment {
  platform: NodeJS.Platform;
  architecture: string;
  extensionPath: string;
  workspaceFolders: readonly string[];
  fileSystem?: RuntimeFileSystem;
}

export interface ServerRuntime {
  command: string;
  workingDirectory: string;
  source: "configured" | "bundled";
  runtimeFiles: readonly string[];
}

export class RuntimeResolutionError extends Error {
  public constructor(message: string) {
    super(message);
    this.name = "RuntimeResolutionError";
  }
}

const nodeFileSystem: RuntimeFileSystem = {
  access,
  async isFile(filePath: string): Promise<boolean> {
    try {
      return (await stat(filePath)).isFile();
    } catch {
      return false;
    }
  },
};

function configuredServerPath(
  value: string,
  workspaceFolders: readonly string[],
): string {
  const configured = value.trim();
  if (path.isAbsolute(configured)) {
    return path.normalize(configured);
  }
  const workspace = workspaceFolders[0];
  if (workspace === undefined) {
    throw new RuntimeResolutionError(
      "hlsl.server.path is relative, but no workspace folder is open. Configure an absolute path.",
    );
  }
  return path.resolve(workspace, configured);
}

async function requireFile(
  fileSystem: RuntimeFileSystem,
  filePath: string,
  label: string,
): Promise<void> {
  if (!(await fileSystem.isFile(filePath))) {
    throw new RuntimeResolutionError(`${label} was not found: ${filePath}`);
  }
}

async function validateExecutable(
  fileSystem: RuntimeFileSystem,
  filePath: string,
  platform: NodeJS.Platform,
): Promise<void> {
  await requireFile(fileSystem, filePath, "The HLSL-LSP executable");
  try {
    await fileSystem.access(
      filePath,
      platform === "win32" ? constants.F_OK : constants.X_OK,
    );
  } catch {
    throw new RuntimeResolutionError(
      `The HLSL-LSP executable is not executable: ${filePath}`,
    );
  }
}

async function validateWindowsRuntime(
  fileSystem: RuntimeFileSystem,
  executablePath: string,
): Promise<readonly string[]> {
  const directory = path.dirname(executablePath);
  const runtimeFiles = [
    path.join(directory, "dxcompiler.dll"),
    path.join(directory, "dxil.dll"),
  ];
  await requireFile(
    fileSystem,
    path.join(directory, "dxcompiler.dll"),
    "The DXC runtime",
  );
  await requireFile(
    fileSystem,
    path.join(directory, "dxil.dll"),
    "The DXIL runtime",
  );
  return runtimeFiles;
}

async function validateLinuxRuntime(
  fileSystem: RuntimeFileSystem,
  executablePath: string,
): Promise<readonly string[]> {
  const runtime = path.join(path.dirname(executablePath), "libdxcompiler.so");
  await requireFile(fileSystem, runtime, "The DXC runtime");
  return [runtime];
}

async function validateRuntimeFiles(
  fileSystem: RuntimeFileSystem,
  executablePath: string,
  platform: NodeJS.Platform,
): Promise<readonly string[]> {
  if (platform === "win32") {
    return validateWindowsRuntime(fileSystem, executablePath);
  }
  if (platform === "linux") {
    return validateLinuxRuntime(fileSystem, executablePath);
  }
  return [];
}

export async function resolveServerRuntime(
  configuredPath: string | undefined,
  environment: RuntimeEnvironment,
): Promise<ServerRuntime> {
  const fileSystem = environment.fileSystem ?? nodeFileSystem;
  const explicitPath = configuredPath?.trim();

  if (explicitPath) {
    const command = configuredServerPath(
      explicitPath,
      environment.workspaceFolders,
    );
    await validateExecutable(fileSystem, command, environment.platform);
    // External Linux servers may resolve DXC through RUNPATH or the system
    // loader. Bundled runtimes remain strictly validated below.
    const runtimeFiles =
      environment.platform === "linux"
        ? []
        : await validateRuntimeFiles(fileSystem, command, environment.platform);
    return {
      command,
      workingDirectory: path.dirname(command),
      source: "configured",
      runtimeFiles,
    };
  }

  const bundledPlatform =
    environment.architecture === "x64" &&
    (environment.platform === "win32" || environment.platform === "linux");
  if (!bundledPlatform) {
    const platform = `${environment.platform}-${environment.architecture}`;
    throw new RuntimeResolutionError(
      `No bundled HLSL-LSP server is published for ${platform}. Configure hlsl.server.path; no fallback server was started.`,
    );
  }

  const platformDirectory =
    environment.platform === "win32" ? "win32-x64" : "linux-x64";
  const workingDirectory = path.join(
    environment.extensionPath,
    "server",
    platformDirectory,
  );
  const executable =
    environment.platform === "win32" ? "hlsl-lsp.exe" : "hlsl-lsp";
  const command = path.join(workingDirectory, executable);
  await validateExecutable(fileSystem, command, environment.platform);
  const runtimeFiles = await validateRuntimeFiles(
    fileSystem,
    command,
    environment.platform,
  );
  return {
    command,
    workingDirectory,
    source: "bundled",
    runtimeFiles,
  };
}
