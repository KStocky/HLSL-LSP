# HLSLIntellisense

A proof of concept for driving DXC's `IDxcIntelliSense` API from modern C++.
It currently demonstrates:

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
- CMake 3.28 or newer
- A DXC build containing `dxc/dxcisense.h`, `dxcompiler.dll`, and `dxil.dll`

The checked-in preset defaults to the DXC copy bundled with a sibling
`UnrealEngine` checkout. For another DXC build, configure `DXC_INCLUDE_DIR` and
`DXC_RUNTIME_DIR`:

```powershell
cmake --preset vs2026 `
  -DDXC_INCLUDE_DIR=C:\path\to\dxc\include `
  -DDXC_RUNTIME_DIR=C:\path\to\dxc\bin
```

## Build and run

```powershell
cmake --preset vs2026
cmake --build --preset debug
ctest --preset debug
```

The target uses C++23, `/W4`, `/WX`, `/permissive-`, and `/Zc:__cplusplus`.
