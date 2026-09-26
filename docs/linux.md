# Linux x64 runtime

Linux and Windows use KStocky's
[DXC `2609-kstocky.2` release](https://github.com/KStocky/DirectXShaderCompiler/releases/tag/2609-kstocky.2),
which fixes the uninitialized `ExpandTokPastingArg` option in
`IDxcIntelliSense`.

CMake downloads
`linux_dxc_2609-kstocky.2.x86_x64.tar.gz` and requires SHA-256
`1a1417fd24dced6ae7fb32097f029546b139167149f1c1ce22aaf5d6f9cfd93b`.
The archive contains `dxcisense.h`, `WinAdapter.h`, `libdxcompiler.so`, and the
LLVM license. The matching Windows archive is pinned independently by
`cmake/Dxc.cmake`.

The release's `libdxcompiler.so` exports `DxcCreateInstance`; the pinned DXC
source registers `CLSID_DxcIntelliSense` on non-Windows platforms. The
`[linux-runtime]` CI integration test proves creation of that interface and
exercises parsing, diagnostics, completion, symbol definition, hover,
signature help, unsaved include resolution, and reanalysis.

## Requirements

The prebuilt runtime supports x86-64 glibc systems only. Its ELF version
requirements include glibc 2.38 and `GLIBCXX_3.4.29`, and its direct system
dependencies include `libz.so.1`, `libstdc++.so.6`, `libgcc_s.so.1`, and the
standard glibc math, C, and loader libraries.

Ubuntu 24.04 x64 is tested in GitHub Actions. Other x86-64 distributions should
work when they provide those ABI versions and libraries, but are not tested.
Linux ARM, 32-bit systems, and musl-based distributions such as Alpine are not
supported by the bundled runtime.

## Build, test, and install

Install CMake 3.28 or newer, Ninja, Clang with C++23 support, and the runtime
libraries listed above. Then run:

```console
cmake --preset linux-clang
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug
cmake --build --preset linux-clang-release --target hlsl-lsp
cmake --install out/build/linux-clang --config Release \
  --prefix out/package/linux-x64
```

The installed directory is a relocatable bundle containing `hlsl-lsp`,
`libdxcompiler.so`, and license files. `libdxcompiler.so` has no dependency on
the archive's optional `libdxil.so`, so that library is not redistributed in
the Linux bundle. The executable uses an `$ORIGIN` install RPATH, so it can be
launched from any working directory without setting `LD_LIBRARY_PATH`:

```console
out/package/linux-x64/hlsl-lsp
```

The build-tree executables similarly use a CMake-managed build RPATH to the
checksum-verified runtime staging directory.

## Selecting a custom DXC runtime

By default each isolated analysis worker loads the bundled `libdxcompiler.so`
through its RPATH. An editor client or `shadertoolsconfig.json` can select a
different runtime with
`hlsl.dxcRuntimeDirectory` (see the
[`shadertoolsconfig.json` reference](shadertoolsconfig.md#dxc-runtime-selection)).
Each worker then loads `libdxcompiler.so` from that directory by absolute path
with `RTLD_NOW | RTLD_LOCAL`. The directory must contain a `libdxcompiler.so`
that satisfies the same glibc, `GLIBCXX`, and `IDxcIntelliSense` ABI
requirements as the bundled runtime; the directory is validated before the
server restarts, and an incompatible selection is reported without looping.

## Reparse limitation

Native `IDxcTranslationUnit::Reparse` has been observed to crash on Linux in
this DXC family. HLSL-LSP retains native `Reparse` on Windows but avoids that
known failure by rebuilding the Linux translation unit through the same
`IDxcIndex`, compiler arguments, and unsaved buffers inside the isolated worker.
The Linux runtime integration test verifies edits to both a root shader and an
unsaved include, including updated navigation, hover, signatures, and
diagnostics. No signal handler or other unsafe crash workaround is used.

## Licensing

The DXC redistributable's `ReleaseNotes.md` assigns `LICENSE-LLVM.txt` to all
files except `d3d12shader.h`; HLSL-LSP does not bundle that header in runtime
packages. The archive also contains `LICENSE-MS.txt`, which Microsoft's
redistributable license mapping assigns only to the optional DXIL signing
library; HLSL-LSP does not redistribute that library either. The LLVM license
permits source and binary redistribution when its notice and disclaimers
accompany the binary. CMake installs that license, and the VS Code extension
includes it under `licenses/dxc/`.
