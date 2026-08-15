# HLSL-LSP

An editor-independent HLSL language server built around DXC's
`IDxcIntelliSense` API. The current proof of concept demonstrates:

- Parsing an unsaved HLSL translation unit
- Enabling HLSL 2021 with `-HV 2021`
- Analyzing function templates and overloaded operators
- Reading diagnostics
- Requesting code completion
- Resolving a symbol definition
- Incrementally reparsing an edited buffer

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

## Code quality

```powershell
cmake --build out\build\windows-msvc --target format
cmake --build out\build\windows-msvc --target format-check

cmake --preset windows-clangcl -DHLSL_ENABLE_CLANG_TIDY=ON
cmake --build --preset windows-clangcl-debug
```

Pushes and pull requests build and run the Catch2 suite with MSVC and clang-cl
on Windows and Clang on Linux.
