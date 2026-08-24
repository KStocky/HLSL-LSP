import { rm } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(import.meta.dirname, "..");
await Promise.all(
  ["dist", ".test-data", "hlsl-lsp-vscode.vsix"].map((entry) =>
    rm(path.join(root, entry), { recursive: true, force: true }),
  ),
);
