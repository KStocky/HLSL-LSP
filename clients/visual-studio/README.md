# Visual Studio client

This VSIX supplies DXC diagnostics and IntelliSense through a bundled
`hlsl-lsp.exe`.

Visual Studio's HLSL Tools extension claims the same file extensions and is not
currently compatible with HLSL-LSP. Disable or uninstall HLSL Tools before
installing this VSIX.
Go-to-definition for symbols and `#include` paths is provided by the server
through LSP. LSP semantic tokens are disabled in Visual Studio because its
classification pipeline can hang the editor while applying them; the language
server still provides richer semantic tokens to other clients. A separate MEF-only assembly provides immediate lexical classification for HLSL keywords,
preprocessor directives, types, functions, comments, strings, and numbers.
Each category appears as an `HLSL ...` item under **Environment > Fonts and
Colors**.

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

Use **Tools > Options > HLSL-LSP > General** to set:

- **HLSL file extensions**: a semicolon-separated list such as
  `.hlsl;.hlsli;.usf`. This is also the default. The built-in `.hlsl` and
  `.hlsli` mappings are always available.
- **Default HLSL language version**: the default DXC `-HV` value. A
  `hlsl.languageVersion` value in `shadertoolsconfig.json` takes precedence.

Changes apply to open and future documents. Shutdown never waits for a failed
LSP broker operation and bounds child-process cleanup.

## Build

Build the native server first:

```powershell
cmake --build --preset windows-msvc-debug --target hlsl-lsp
```

Then build the VSIX with Visual Studio MSBuild:

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
  Select-Object -First 1
& $msbuild clients\visual-studio\HlslLsp.VisualStudio\HlslLsp.VisualStudio.csproj `
  /restore /p:Configuration=Debug
```

The VSIX is written under `clients\visual-studio\HlslLsp.VisualStudio\bin`.
Opening the project in Visual Studio and pressing F5 installs it into the
experimental instance. Open an `.hlsl`, `.hlsli`, or configured HLSL file; the
server starts automatically once the host workspace is ready.

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
