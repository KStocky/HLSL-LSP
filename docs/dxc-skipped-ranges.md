# Linux DXC skipped-range regression (#70)

The first verified safe release is
[KStocky DXC `2609-kstocky.3`](https://github.com/KStocky/DirectXShaderCompiler/releases/tag/2609-kstocky.3).
It includes [fix `d37aff1`](https://github.com/KStocky/DirectXShaderCompiler/commit/d37aff1276cd1222bac32d030e9f496fb41ef9a3),
which installs DXC's per-thread filesystem before `GetSkippedRanges` accesses
a virtual unsaved file. The released runtime reports commit `97d967e0`.

## Standalone reproduction

[`tests/dxc/skipped_ranges_repro.cpp`](../tests/dxc/skipped_ranges_repro.cpp)
uses only `dxcisense.h`, the system dynamic loader, and the C++ standard library.
It does not link HLSL-LSP or use its capability fallback.

The reproduction parses three unsaved buffers: a root whose virtual include
has been rewritten to a physical path, the unused original virtual alias,
and the physical include. It queries every file's skipped ranges and checks
that the root has exactly one range while the two includes have none.

On Linux, with an extracted DXC package, compile and run it directly:

```sh
clang++ -std=c++23 -Wall -Wextra -Werror \
  -isystem /path/to/dxc/include tests/dxc/skipped_ranges_repro.cpp \
  -ldl -o skipped-ranges-repro
./skipped-ranges-repro /path/to/dxc/lib/libdxcompiler.so
```

Alternatively, build the opt-in CMake target against the pinned headers:

```sh
cmake --preset linux-clang
cmake --build --preset linux-clang-debug --target hlsl-dxc-skipped-ranges-repro
out/build/linux-clang/Debug/hlsl-dxc-skipped-ranges-repro \
  /absolute/path/to/libdxcompiler.so
```

The same executable was run against both published packages on Linux x64:

| Runtime | Result |
|---|---|
| `2609-kstocky.2` | Root returns one range; querying `/virtual/include.hlsli` segfaults (exit 139). |
| `2609-kstocky.3` | Root returns one range, virtual alias zero, physical include zero; exit 0. |

## Capability and regression tests

Windows remains enabled. Linux uses the runtime's `IDxcVersionInfo2` commit
identity, not a broad DXC version threshold or the fact it is called "bundled".
Only the verified `97d967e0` build is currently allowed for rewritten-source
skipped-range queries. Older, custom, unknown, and unverified newer builds
retain the fallback until separately validated and added to the capability
check.

The DXC tests exercise the exact three-buffer case, skipped-range coordinates,
and entry-point data flow. The LSP virtual-macro-include integration test
requires available skipped regions and no fallback diagnostic on a safe
runtime. Capability tests explicitly reject older/unknown Linux runtime
versions, and the same rewritten-source test checks the fallback when run
with an older custom runtime.

The Windows package is pinned to SHA-256
`1f4d60f2c5f0ce2a8b07049071611e5174f0bb486c792307aef32b1c50bbaeff`;
the Linux package to
`8db20e74260ffcdfe2285a7bc5ad052530505479c4c2467d1195dbf2056950d4`.
