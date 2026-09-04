# Visual Studio client

This VSIX supplies DXC diagnostics and IntelliSense through a bundled
`hlsl-lsp.exe`.

Visual Studio's HLSL Tools extension claims the same file extensions and is not
currently compatible with HLSL-LSP. Disable or uninstall HLSL Tools before
installing this VSIX.
Go-to-definition for symbols and `#include` paths is provided by the server
through LSP. Workspace symbols also integrate with Visual Studio's All-In-One
Search (`Ctrl+T`). LSP semantic tokens are disabled in Visual Studio because
its classification pipeline can hang the editor while applying them; the
language server still provides richer semantic tokens to other clients. A
separate MEF-only assembly provides immediate lexical classification for HLSL keywords,
preprocessor directives, types, functions, comments, strings, and numbers.
Each category appears as an `HLSL ...` item under **Environment > Fonts and
Colors**, using the same Visual Studio colour picker as HLSL Tools.

The VSIX deliberately does not export a remote content type or language client
through MEF. Those exports can change Visual Studio's workspace-composition graph
during startup and have caused the shell to deadlock. A lightweight text-view
listener records filenames without referencing Visual Studio's LSP APIs; the
bootstrap matches them against the configured HLSL extensions.
In CMake workspaces it waits for the CMake package's asynchronous load to finish;
it then loads an isolated package and language-client assembly through the public
broker and promotes open shaders to dynamic remote subtypes.
Before a folder or solution closes, the package demotes every shader back to its
native content type, so restored documents never begin the next startup as
LSP-backed buffers. Later HLSL documents activate automatically. No user action
is required.

Use the native **Tools > Options > HLSL-LSP > General** Unified Settings page
to set:

- **HLSL file extensions**: a semicolon-separated list such as
  `.hlsl;.hlsli;.usf`. This is also the default. The built-in `.hlsl` and
  `.hlsli` mappings are always available.
- **Default HLSL language version**: the default DXC `-HV` value. A
  `hlsl.languageVersion` value in `shadertoolsconfig.json` takes precedence.

Changes apply to open and future documents. Shutdown never waits for a failed
LSP broker operation and bounds child-process cleanup.

When a `shadertoolsconfig.json` declares named compilation variants under
`hlsl.variants`, run **Tools > HLSL Select Shader Variant** to pick the active
variant for the current document. Changing the active variant reanalyzes open
documents and restarts the language server only if the variant selects a
different DXC runtime. See the repository's
[named compilation variants](../../docs/shadertoolsconfig.md#named-compilation-variants)
reference for details.

## Install

Download `HlslLsp.VisualStudio.vsix` from the
[latest GitHub release](https://github.com/KStocky/HLSL-LSP/releases/latest).
Close Visual Studio, run the VSIX, and follow the installer prompts.

Release artifacts are not yet code-signed. Windows Smart App Control may block
the extension or language server unless Developer Mode is enabled.

## Build from source

Build the native server first:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target hlsl-lsp
```

Then build the VSIX with Visual Studio MSBuild:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * `
  -requires Microsoft.Component.MSBuild `
  -find MSBuild\**\Bin\MSBuild.exe |
  Select-Object -First 1
$serverDir = (Resolve-Path 'out\build\windows-msvc\Release').Path

& $msbuild clients\visual-studio\HlslLsp.VisualStudio\HlslLsp.VisualStudio.csproj `
  /restore `
  /p:Configuration=Release `
  "/p:HlslLspServerDir=$serverDir"
```

The VSIX is written to
`clients\visual-studio\HlslLsp.VisualStudio\bin\Release\net472\HlslLsp.VisualStudio.vsix`.
Close Visual Studio and run that file to install the extension. Open an
`.hlsl`, `.hlsli`, or configured HLSL file; the server starts automatically
once the host workspace is ready.

Use `.vs\VSWorkspaceSettings.json` for editor overrides:

```json
{
  "hlsl.preprocessorDefinitions": {
    "EDITOR_BUILD": 1
  },
  "hlsl.additionalIncludeDirectories": [
    "Shaders/Includes"
  ]
}
```

Visual Studio LSP traces can be enabled with `"hlsl.trace.server": "Verbose"`.
