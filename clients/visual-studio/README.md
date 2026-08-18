# Visual Studio client

This VSIX registers an `HLSL-LSP` content type and launches the bundled
`hlsl-lsp.exe` to supply DXC diagnostics and IntelliSense.

Visual Studio's HLSL Tools extension claims the same file extensions and is not
currently compatible with HLSL-LSP. Disable or uninstall HLSL Tools before
installing this VSIX.
Go-to-definition for symbols and `#include` paths is provided by the server through LSP. The VSIX provides safe
native lexical colouring for HLSL keywords, built-in and declared types,
functions, preprocessor directives, comments, strings, and numbers. LSP semantic
tokens are disabled only in Visual Studio
because its LSP client can hang the editor while applying them; the language
server still provides richer semantic tokens to other clients.

Language-client startup is joinable but does not block extension loading, and
shutdown never waits for a failed LSP broker operation. Native classification
returns cached results synchronously and performs tokenization in the background.

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
experimental instance. Open an `.hlsl` or `.hlsli` file to start the server.

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
