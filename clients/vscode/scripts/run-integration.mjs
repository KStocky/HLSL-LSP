import { rm, mkdir } from "node:fs/promises";
import path from "node:path";

import { downloadAndUnzipVSCode, runTests } from "@vscode/test-electron";

function argument(name) {
  const index = process.argv.indexOf(name);
  return index < 0 ? undefined : process.argv[index + 1];
}

const root = path.resolve(import.meta.dirname, "..");
const serverPath = path.resolve(
  argument("--server-path") ??
    process.env.HLSL_LSP_TEST_SERVER ??
    path.join(root, "server", "win32-x64", "hlsl-lsp.exe"),
);
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
    extensionTestsEnv: {
      HLSL_LSP_TEST_SERVER: serverPath,
    },
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
