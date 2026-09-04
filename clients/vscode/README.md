# HLSL-LSP for Visual Studio Code

This extension is a thin client for the repository's native `hlsl-lsp` stdio
server. It provides DXC-powered diagnostics, completion, semantic tokens,
hover, signature help, and navigation for `.hlsl`, `.hlsli`, and `.usf`
documents.

## Install

Download `hlsl-lsp-vscode.vsix` from the
[latest release](https://github.com/KStocky/HLSL-LSP/releases/latest), then run:

```console
code --install-extension hlsl-lsp-vscode.vsix
```

The release VSIX contains the Windows x64 `hlsl-lsp.exe`, `dxcompiler.dll`, and
`dxil.dll`, plus the Linux x64 `hlsl-lsp` and `libdxcompiler.so`. Opening an
HLSL document starts the matching bundled server automatically.
Additional extensions can be associated with the contributed `hlsl` language
through VS Code's built-in setting:

```json
"files.associations": {
  "*.fx": "hlsl",
  "*.ush": "hlsl"
}
```

## Build and package

Requirements are Node.js 22 or newer, npm, and Release runtime bundles for both
Windows x64 and Linux x64. Stage and package on Linux so the Linux executable's
mode is retained:

```console
cd clients/vscode
npm ci
npm run stage:runtime -- --platform win32-x64 \
  --server-dir ../../out/package/win32-x64
npm run stage:runtime -- --platform linux-x64 \
  --server-dir ../../out/package/linux-x64
npm run check
npm run package
```

The result is `clients\vscode\hlsl-lsp-vscode.vsix`. `package-lock.json` pins
the npm dependency graph, and each staging command fails unless all required
runtime files are present. The build and release workflows produce and combine
the two CMake-installed bundles.

Run the extension-host smoke test against the staged server with:

```console
xvfb-run -a npm run test:integration -- --bundled
```

The harness downloads the pinned VS Code 1.96.0 test instance, uses isolated
user-data and extension directories under `.test-data`, disables user
extensions and workspace trust prompts, and removes its profile afterward.
Set `HLSL_LSP_TEST_VSCODE_VERSION` only when deliberately testing another
editor version.

## Server selection

`hlsl.server.path` has strict precedence when it is non-empty. Relative paths
are resolved from the first workspace folder; use an absolute path when no
folder is open. On Windows, an external server must have matching
`dxcompiler.dll` and `dxil.dll` files beside it. On Linux, the server may find
`libdxcompiler.so` through a colocated file, its ELF RUNPATH, or the system
dynamic loader configuration. The extension validates bundled runtime files
and reports activation errors without silently trying another server.

Bundled runtimes are available for Windows x64 and glibc-based Linux x64.
Ubuntu 24.04 is tested; the Linux runtime requires glibc 2.38,
`GLIBCXX_3.4.29`, and `libz.so.1`. ARM, 32-bit, and musl Linux are not
supported. See [`../../docs/linux.md`](../../docs/linux.md) for complete Linux
requirements and limitations.

One language server is used for the entire VS Code window. In a multi-root
workspace, the first workspace folder deterministically selects both
`hlsl.server.path` and every `hlsl.*` editor setting; a relative server path is
also resolved from that folder. Changing the active editor never changes the
selected server or settings. Reordering, adding, or removing workspace folders
restarts the client so the first-folder selection remains consistent.

Use **HLSL: Restart Language Server**, **HLSL: Stop Language Server**, **HLSL:
Show Language Server Output**, and **HLSL: Show Client Diagnostics** to manage
and inspect the selected process. Shutdown is performed through the language
client and is scoped to the child process launched by this extension.

## Shader variants

When a `shadertoolsconfig.json` declares named compilation variants under
`hlsl.variants`, run **HLSL: Select Shader Variant** (or click the variant
indicator in the status bar) to choose the active variant. The picker lists the
variants the server reports for the active document. The choice is stored in the
`hlsl.activeVariant` setting; changing it reanalyzes open documents and restarts
the server only when the variant selects a different DXC runtime. See the
repository's
[named compilation variants](../../docs/shadertoolsconfig.md#named-compilation-variants)
reference for the schema and behavior.

## DXC runtime selection

By default the extension loads the bundled, pinned DXC runtime. Set
`hlsl.dxcRuntimeDirectory` to a directory containing a compatible DXC runtime
(`dxcompiler.dll` and `dxil.dll` on Windows, `libdxcompiler.so` on Linux) to
load that runtime instead. Relative values resolve from the first workspace
folder, so a checked-in path such as `Tools/dxc/bin` stays environment
independent. The directory and its compiler library are validated before the
server starts, and an incompatible selection is reported without repeatedly
retrying.

An explicit `hlsl.dxcRuntimeDirectory` overrides any `shadertoolsconfig.json`
runtime selection. When the editor setting is empty, the server resolves the
runtime from `shadertoolsconfig.json` and asks the client to restart so it
loads. Because DXC is loaded once per process, changing the runtime restarts the
language server; open documents are re-analyzed against the new runtime and no
restart loop can form. **HLSL: Show Client Diagnostics** reports the selected and
active runtime directory and the loaded DXC version.

## Configuration

`hlsl.languageVersion` defaults to `2021` as a _client default_. It remains
below `shadertoolsconfig.json` in the server's precedence order. The following
settings become higher-precedence editor overrides only when explicitly
configured:

- `hlsl.preprocessorDefinitions`
- `hlsl.additionalIncludeDirectories`
- `hlsl.virtualDirectoryMappings`
- `hlsl.languageVersion`
- `hlsl.targetProfile`
- `hlsl.entryPoint`
- `hlsl.additionalArguments`

Explicit arrays and objects replace file-derived values; an empty value clears
the inherited collection. Relative include directories and virtual mapping
targets are resolved by the server from the workspace folder containing each
shader. For files outside a workspace folder, they are resolved from the
shader's directory. Although those relative paths are resolved per shader,
their setting value is always read from the first workspace folder. Editor
overrides apply to the whole VS Code window; use `shadertoolsconfig.json` and
its file groups for folder- or shader-specific values. See the repository's
[`shadertoolsconfig.json` reference](../../docs/shadertoolsconfig.md) for file
configuration, merging, file groups, and full precedence details.

The client watches `shadertoolsconfig.json` and `.hlsl`, `.hlsli`, and `.usf`
files throughout open workspaces. It also creates targeted recursive watchers
for directories named explicitly by the editor's
`hlsl.additionalIncludeDirectories` and `hlsl.virtualDirectoryMappings`
settings, including external absolute directories and relative directories
resolved against each workspace folder. Changing either editor setting
rebuilds the watcher set by restarting the client.

Only the three built-in shader extensions are watched. A custom extension
associated through `files.associations` works while open, but changes to a
closed dependency with that custom extension may not invalidate an open root.
The client cannot discover external include or virtual-mapping trees declared
only in `shadertoolsconfig.json`; standard-extension files in such a tree are
watched only when the tree is inside an open workspace or is also named by an
explicit editor setting. Covering config-only external trees requires future
server-driven dynamic watcher registration.

`hlsl.trace.server` controls LSP message tracing in the HLSL-LSP output
channel. Do not put native server switches in `hlsl.additionalArguments`; that
setting is passed to DXC.

## Licenses

HLSL-LSP is MIT licensed. See [ThirdPartyNotices.txt](ThirdPartyNotices.txt)
and the exact license texts under `licenses/` for bundled DXC and production
npm dependency attribution.
