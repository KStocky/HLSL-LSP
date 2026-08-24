# Bundled server layout

Release packaging stages these files into `win32-x64/`:

- `hlsl-lsp.exe`
- `dxcompiler.dll`
- `dxil.dll`

The files are generated or downloaded build artifacts and are intentionally not
stored in Git. `npm run stage:runtime -- --server-dir <directory>` validates
and stages them before `npm run package`.

The platform directory keeps runtime resolution explicit and allows a future
`linux-x64/` bundle without changing the extension layout. No Linux runtime is
currently published in the VSIX.
