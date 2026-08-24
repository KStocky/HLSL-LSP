import assert from "node:assert/strict";
import path from "node:path";
import test from "node:test";

import {
  resolveServerRuntime,
  RuntimeFileSystem,
  RuntimeResolutionError,
} from "../../src/runtime";

class FakeFileSystem implements RuntimeFileSystem {
  public readonly files = new Set<string>();
  public readonly inaccessible = new Set<string>();

  public access(filePath: string): Promise<void> {
    if (this.inaccessible.has(path.normalize(filePath))) {
      return Promise.reject(new Error("access denied"));
    }
    return Promise.resolve();
  }

  public isFile(filePath: string): Promise<boolean> {
    return Promise.resolve(this.files.has(path.normalize(filePath)));
  }

  public add(...filePaths: readonly string[]): void {
    for (const filePath of filePaths) {
      this.files.add(path.normalize(filePath));
    }
  }
}

void test("an explicit server and its runtime take precedence over the bundle", async () => {
  const fileSystem = new FakeFileSystem();
  const workspace = path.resolve("workspace");
  const command = path.join(workspace, "tools", "hlsl-lsp.exe");
  fileSystem.add(
    command,
    path.join(path.dirname(command), "dxcompiler.dll"),
    path.join(path.dirname(command), "dxil.dll"),
  );

  const runtime = await resolveServerRuntime("tools/hlsl-lsp.exe", {
    platform: "win32",
    architecture: "x64",
    extensionPath: path.resolve("extension"),
    workspaceFolders: [workspace],
    fileSystem,
  });

  assert.equal(runtime.command, command);
  assert.equal(runtime.source, "configured");
  assert.equal(runtime.runtimeFiles.length, 2);
});

void test("the Windows x64 bundle requires the executable and both DXC files", async () => {
  const fileSystem = new FakeFileSystem();
  const extensionPath = path.resolve("extension");
  const directory = path.join(extensionPath, "server", "win32-x64");
  fileSystem.add(
    path.join(directory, "hlsl-lsp.exe"),
    path.join(directory, "dxcompiler.dll"),
  );

  await assert.rejects(
    resolveServerRuntime(undefined, {
      platform: "win32",
      architecture: "x64",
      extensionPath,
      workspaceFolders: [],
      fileSystem,
    }),
    (error: unknown) =>
      error instanceof RuntimeResolutionError &&
      error.message.includes("dxil.dll"),
  );

  fileSystem.add(path.join(directory, "dxil.dll"));
  const runtime = await resolveServerRuntime("", {
    platform: "win32",
    architecture: "x64",
    extensionPath,
    workspaceFolders: [],
    fileSystem,
  });
  assert.equal(runtime.source, "bundled");
});

void test("the Linux x64 bundle requires an executable DXC runtime", async () => {
  const fileSystem = new FakeFileSystem();
  const extensionPath = path.resolve("extension");
  const directory = path.join(extensionPath, "server", "linux-x64");
  const command = path.join(directory, "hlsl-lsp");
  fileSystem.add(command);
  fileSystem.inaccessible.add(path.normalize(command));

  await assert.rejects(
    resolveServerRuntime(undefined, {
      platform: "linux",
      architecture: "x64",
      extensionPath,
      workspaceFolders: [],
      fileSystem,
    }),
    (error: unknown) =>
      error instanceof RuntimeResolutionError &&
      error.message.includes("not executable"),
  );

  fileSystem.inaccessible.delete(path.normalize(command));
  await assert.rejects(
    resolveServerRuntime(undefined, {
      platform: "linux",
      architecture: "x64",
      extensionPath,
      workspaceFolders: [],
      fileSystem,
    }),
    /libdxcompiler\.so/,
  );

  fileSystem.add(path.join(directory, "libdxcompiler.so"));
  const runtime = await resolveServerRuntime("", {
    platform: "linux",
    architecture: "x64",
    extensionPath,
    workspaceFolders: [],
    fileSystem,
  });
  assert.equal(runtime.command, command);
  assert.equal(runtime.source, "bundled");
  assert.deepEqual(runtime.runtimeFiles, [
    path.join(directory, "libdxcompiler.so"),
  ]);
});

void test("an external Linux server may resolve DXC through its loader", async () => {
  const fileSystem = new FakeFileSystem();
  const command = path.resolve("tools", "hlsl-lsp");
  fileSystem.add(command);

  const runtime = await resolveServerRuntime(command, {
    platform: "linux",
    architecture: "x64",
    extensionPath: path.resolve("extension"),
    workspaceFolders: [],
    fileSystem,
  });
  assert.equal(runtime.source, "configured");
  assert.deepEqual(runtime.runtimeFiles, []);
});

void test("a relative external path requires a workspace", async () => {
  await assert.rejects(
    resolveServerRuntime("hlsl-lsp", {
      platform: "linux",
      architecture: "x64",
      extensionPath: path.resolve("extension"),
      workspaceFolders: [],
      fileSystem: new FakeFileSystem(),
    }),
    /no workspace folder is open/,
  );
});
