import { rm, mkdir } from "node:fs/promises";
import path from "node:path";

import { downloadAndUnzipVSCode, runTests } from "@vscode/test-electron";

function argument(name) {
  const index = process.argv.indexOf(name);
  return index < 0 ? undefined : process.argv[index + 1];
}

const root = path.resolve(import.meta.dirname, "..");
const bundledServer = process.argv.includes("--bundled");
const configuredServer =
  argument("--server-path") ?? process.env.HLSL_LSP_TEST_SERVER;
const defaultServer = path.join(
  root,
  "server",
  process.platform === "linux" ? "linux-x64" : "win32-x64",
  process.platform === "linux" ? "hlsl-lsp" : "hlsl-lsp.exe",
);
const serverPath = bundledServer
  ? undefined
  : path.resolve(configuredServer ?? defaultServer);
const testData = path.join(root, ".test-data");
const userData = path.join(testData, "user-data");
const extensions = path.join(testData, "extensions");
const vscodeVersion = process.env.HLSL_LSP_TEST_VSCODE_VERSION ?? "1.96.0";

await rm(testData, { recursive: true, force: true });
await Promise.all([
  mkdir(userData, { recursive: true }),
  mkdir(extensions, { recursive: true }),
]);

try {
  const vscodeExecutablePath = await downloadAndUnzipVSCode(vscodeVersion);
  await runTests({
    vscodeExecutablePath,
    extensionDevelopmentPath: root,
    extensionTestsPath: path.join(root, "dist", "test", "integration", "index"),
    extensionTestsEnv:
      serverPath === undefined ? {} : { HLSL_LSP_TEST_SERVER: serverPath },
    launchArgs: [
      path.join(root, "test", "fixture"),
      "--disable-extensions",
      "--disable-workspace-trust",
      `--user-data-dir=${userData}`,
      `--extensions-dir=${extensions}`,
    ],
  });
} finally {
  await rm(testData, { recursive: true, force: true });
}
