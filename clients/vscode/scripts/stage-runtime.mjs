import { copyFile, mkdir, stat } from "node:fs/promises";
import path from "node:path";

function argument(name) {
  const index = process.argv.indexOf(name);
  return index < 0 ? undefined : process.argv[index + 1];
}

const serverDirectoryValue = argument("--server-dir");
if (!serverDirectoryValue) {
  throw new Error(
    "Usage: npm run stage:runtime -- --server-dir <Release server directory>",
  );
}

const serverDirectory = path.resolve(serverDirectoryValue);
const destination = path.resolve(
  import.meta.dirname,
  "..",
  "server",
  "win32-x64",
);
const files = ["hlsl-lsp.exe", "dxcompiler.dll", "dxil.dll"];

for (const file of files) {
  const source = path.join(serverDirectory, file);
  try {
    if (!(await stat(source)).isFile()) {
      throw new Error("not a file");
    }
  } catch {
    throw new Error(`Required Windows runtime file was not found: ${source}`);
  }
}

await mkdir(destination, { recursive: true });
await Promise.all(
  files.map((file) =>
    copyFile(path.join(serverDirectory, file), path.join(destination, file)),
  ),
);
console.log(`Staged ${files.join(", ")} in ${destination}`);
