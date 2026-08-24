# Bundled server layout

Release packaging stages these files into `win32-x64/`:

- `hlsl-lsp.exe`
- `dxcompiler.dll`
- `dxil.dll`

It stages these files into `linux-x64/`:

- `hlsl-lsp`
- `libdxcompiler.so`

The files are generated or downloaded build artifacts and are intentionally not
stored in Git. `npm run stage:runtime -- --platform <platform>
--server-dir <directory>` validates and stages them before `npm run package`.
Packaging runs on Linux so the Linux executable remains executable.
