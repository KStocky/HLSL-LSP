# Visual Studio client

This VSIX extends Visual Studio's built-in `HLSL` content type with remote-code
support, starts the bundled `hlsl-lsp.exe` over standard input/output, forwards
the `hlsl` configuration section, and watches shader and configuration files.
Go-to-definition is provided by the server through LSP. Semantic tokens are
temporarily disabled in Visual Studio because its LSP client can hang the editor
while applying them; the language server still provides them to other clients.

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
