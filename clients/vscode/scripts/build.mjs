import { build } from "esbuild";
import path from "node:path";

const root = path.resolve(import.meta.dirname, "..");
await build({
  entryPoints: [path.join(root, "src", "extension.ts")],
  outfile: path.join(root, "dist", "extension.js"),
  bundle: true,
  external: ["vscode"],
  format: "cjs",
  platform: "node",
  target: "node20",
  sourcemap: true,
  logLevel: "info",
});
