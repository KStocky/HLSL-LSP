# Shader-aware inlay hints

HLSL-LSP provides native `textDocument/inlayHint` results derived from the
current DXC translation unit, active compilation variant, compiler reflection,
and memory-layout data. Visual Studio and Visual Studio Code render the hints
through their standard editor surfaces; no separate tool window is required.

The default view enables inferred-type and unambiguous parameter-name hints.
An active compilation variant is also shown when one is selected. More detailed
layout and binding annotations are opt-in because they can make dense shader
code harder to read:

| Setting | Default | Information |
|---|---:|---|
| `hlsl.inlayHints.types` | `true` | Concrete types for declarations whose source type is inferred |
| `hlsl.inlayHints.parameters` | `true` | Parameter names when the selected overloads agree |
| `hlsl.inlayHints.matrixOrientation` | `false` | Compiler-derived row-major or column-major orientation |
| `hlsl.inlayHints.registers` | `false` | Reflected register class, index, and space |
| `hlsl.inlayHints.packedOffsets` | `false` | Constant-buffer packed byte offsets |
| `hlsl.inlayHints.arrayStrides` | `false` | Compiler-derived array strides |
| `hlsl.inlayHints.activeVariant` | `true` | The selected named compilation variant |

Visual Studio exposes the same categories under **Tools > Options > HLSL-LSP >
General**. VS Code exposes them in normal user, workspace, and language-specific
settings.

Hints are returned only for the requested source range and current document
version. Editing the document, changing configuration, or selecting another
variant invalidates prior results. The server does not infer register bindings,
layout, overloads, or types by parsing HLSL text independently: if DXC cannot
provide an unambiguous result, the corresponding hint is omitted.
