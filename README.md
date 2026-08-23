# HLSL-LSP

This project was generated with GitHub Copilot.

An editor-independent HLSL language server built around DXC's
`IDxcIntelliSense` API. The `hlsl-lsp` executable is an LSP 3.17 stdio server
with:

- UTF-16 incremental document synchronization
- DXC diagnostics for open HLSL documents
- DXC-backed code completion
- DXC-backed semantic colouring and go-to-definition for symbols and include paths
- Workspace symbol support for Visual Studio's All-In-One Search
- Hierarchical `shadertoolsconfig.json` compiler configuration
- Transitive, virtual, open-buffer, and dependency-aware include handling
- A Visual Studio 2026 VSIX client under `clients/visual-studio`

The Visual Studio extension serves a similar purpose to
[Tim Jones' HLSL Tools](https://github.com/tgjones/HlslTools), while using
DXC's IntelliSense API so it can understand modern HLSL language and shader
model features, including HLSL 2021 and Shader Model 6.6 resource descriptor
heap types.

## Features

### Code completion

HLSL-LSP provides context-aware completion for language constructs, user
symbols, templates, and DXC intrinsics. Trigger it with Visual Studio's normal
`Ctrl+J` or `Ctrl+Space` shortcuts.

![HLSL code completion in Visual Studio](art/completion.png)

### Semantic colouring

Types, functions, variables, templates, preprocessor directives, and other
HLSL constructs receive dedicated classifications that integrate with Visual
Studio's Fonts and Colors settings.

![Semantic colouring for modern HLSL](art/semantic-colouring.png)

### Go to definition

Press `F12` to navigate to symbol definitions or resolved `#include` files,
including files reached through virtual directory mappings.

![Go to definition for an HLSL include](art/go-to-definition.png)

### Navigation bar

The native Visual Studio navigation bar tracks namespaces, types, functions,
and the current symbol in HLSL documents.

![Visual Studio navigation bar for HLSL](art/navigation-bar.png)

### All-In-One Search

Press `Ctrl+T` to find HLSL types and members across the workspace through
Visual Studio's All-In-One Search.

![HLSL symbols in Visual Studio All-In-One Search](art/all-in-one-search.png)

## Install the Visual Studio extension

### Download a release

The easiest way to install HLSL-LSP is to download
`HlslLsp.VisualStudio.vsix` from the
[latest GitHub release](https://github.com/KStocky/HLSL-LSP/releases/latest).
Close Visual Studio, run the downloaded VSIX, and follow the installer prompts.
Restart Visual Studio and open an `.hlsl` or `.hlsli` file; the bundled language
server starts automatically.

HLSL Tools claims the same Visual Studio file types and cannot currently run
alongside HLSL-LSP. Disable or uninstall HLSL Tools before installing this
extension.

> [!NOTE]
> Release artifacts are not yet code-signed. Windows Smart App Control may
> block the extension or language server unless Developer Mode is enabled.

### Build the VSIX from source

Building requires Windows, Visual Studio 2026 with the MSVC C++ workload, and
CMake 3.28 or newer. From a Developer PowerShell prompt, build the Release
server first:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target hlsl-lsp
```

Then locate Visual Studio's MSBuild and package that server into the VSIX:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * `
  -requires Microsoft.Component.MSBuild `
  -find MSBuild\**\Bin\MSBuild.exe |
  Select-Object -First 1
$serverDir = (Resolve-Path 'out\build\windows-msvc\Release').Path

& $msbuild `
  clients\visual-studio\HlslLsp.VisualStudio\HlslLsp.VisualStudio.csproj `
  /restore `
  /p:Configuration=Release `
  "/p:HlslLspServerDir=$serverDir"
```

The finished installer is:

```text
clients\visual-studio\HlslLsp.VisualStudio\bin\Release\net472\HlslLsp.VisualStudio.vsix
```

Close Visual Studio and run that file to install the extension.

## Development requirements

- Windows
- Visual Studio 2026 with the MSVC C++ workload
- LLVM with `clang-cl`, `clang-format`, and `clang-tidy`
- CMake 3.28 or newer
- Internet access for CMake's first configuration

CMake downloads the pinned official Microsoft DXC `1.9.2607.13` package,
including matching headers and runtime binaries. Downloads are checksum
verified and cached in the build directory.

To use a custom DXC build instead, set both `DXC_INCLUDE_DIR` and
`DXC_RUNTIME_DIR`. `DXC_INCLUDE_DIR` must directly contain `dxcisense.h`.

```powershell
cmake --preset windows-msvc `
  -DDXC_INCLUDE_DIR=C:\path\to\dxc\include\dxc `
  -DDXC_RUNTIME_DIR=C:\path\to\dxc\bin
```

## Build and test

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug

cmake --preset windows-clangcl
cmake --build --preset windows-clangcl-debug
ctest --preset windows-clangcl-debug
```

All C++ tests use Catch2. First-party targets use C++23 and warnings as errors:
`/W4 /WX` with MSVC-compatible frontends and
`-Wall -Wextra -Wpedantic -Werror` with Linux Clang.

## Configuration

HLSL-LSP supports the `shadertoolsconfig.json` format created by Tim Jones for
[HLSL Tools](https://github.com/tgjones/HlslTools), together with additional
DXC-oriented settings. See the
[`shadertoolsconfig.json` reference](docs/shadertoolsconfig.md) for discovery,
merging, every supported property, path handling, examples, and editor-setting
precedence.
