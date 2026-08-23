# HLSL-LSP

This project was generated with GitHub Copilot.

An editor-independent HLSL language server built around DXC's
`IDxcIntelliSense` API. The `hlsl-lsp` executable is an LSP 3.17 stdio server
with:

- UTF-16 incremental document synchronization
- DXC diagnostics for open HLSL documents
- DXC-backed code completion
- DXC-backed semantic colouring and go-to-definition for symbols and include paths
- Hierarchical `shadertoolsconfig.json` compiler configuration
- Transitive, virtual, open-buffer, and dependency-aware include handling
- A Visual Studio 2026 VSIX prototype under `clients/visual-studio`

The Visual Studio extension serves a similar purpose to
[Tim Jones' HLSL Tools](https://github.com/tgjones/HlslTools), while using
DXC's IntelliSense API so it can understand modern HLSL language and shader
model features, including HLSL 2021 and Shader Model 6.6 resource descriptor
heap types.

The Visual Studio client disables LSP semantic tokens and uses a native lexical
classifier for configurable HLSL-specific colours. Its startup-safe listener has
no LSP dependency and loads the isolated language client only after CMake package
loading has completed, keeping LSP broker composition out of restored-document
initialization. Completion, diagnostics, and go-to-definition remain enabled;
richer semantic tokens remain available to other LSP clients.
- HLSL 2021 enabled by default

## Requirements

- Windows
- Visual Studio 2026 with the MSVC C++ workload
- LLVM with `clang-cl`, `clang-format`, and `clang-tidy`
- CMake 3.28 or newer
- A DXC build containing `dxcisense.h`, `dxcompiler.dll`, and `dxil.dll`

Linux builds use Clang and require a DXC distribution containing
`libdxcompiler.so`. The checked-in Windows presets default to the DXC copy
bundled with a sibling `UnrealEngine` checkout. For another DXC build, configure
`DXC_INCLUDE_DIR` and `DXC_RUNTIME_DIR`. `DXC_INCLUDE_DIR` is the directory that
directly contains `dxcisense.h`.

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
HLSL Tools, together with additional DXC-oriented settings. See the
[`shadertoolsconfig.json` reference](docs/shadertoolsconfig.md) for discovery,
merging, every supported property, path handling, examples, and editor-setting
precedence.
