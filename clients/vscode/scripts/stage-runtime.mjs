import { chmod, copyFile, mkdir, stat } from "node:fs/promises";
import path from "node:path";

function argument(name) {
  const index = process.argv.indexOf(name);
  return index < 0 ? undefined : process.argv[index + 1];
}

const serverDirectoryValue = argument("--server-dir");
const platform = argument("--platform") ?? "win32-x64";
const runtimes = {
  "win32-x64": ["hlsl-lsp.exe", "dxcompiler.dll", "dxil.dll"],
  "linux-x64": ["hlsl-lsp", "libdxcompiler.so"],
};
const files = runtimes[platform];
if (!serverDirectoryValue || files === undefined) {
  throw new Error(
    "Usage: npm run stage:runtime -- --platform <win32-x64|linux-x64> --server-dir <Release server directory>",
  );
}

const serverDirectory = path.resolve(serverDirectoryValue);
const destination = path.resolve(import.meta.dirname, "..", "server", platform);

for (const file of files) {
  const source = path.join(serverDirectory, file);
  try {
    if (!(await stat(source)).isFile()) {
      throw new Error("not a file");
    }
  } catch {
    throw new Error(
      `Required ${platform} runtime file was not found: ${source}`,
    );
  }
}

await mkdir(destination, { recursive: true });
await Promise.all(
  files.map((file) =>
    copyFile(path.join(serverDirectory, file), path.join(destination, file)),
  ),
);
if (platform === "linux-x64") {
  await chmod(path.join(destination, "hlsl-lsp"), 0o755);
}
console.log(`Staged ${files.join(", ")} in ${destination}`);
