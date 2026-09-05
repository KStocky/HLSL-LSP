# HLSL shader compilation

HLSL-LSP can report the effective compiler configuration, diagnostics, and
DXC reflection for the active HLSL document through the combined **Shader
Compilation** view and its cross-editor `hlsl/compilationInfo` protocol.

In Visual Studio, run **Tools > HLSL Shader Compilation**. In Visual Studio
Code, run **HLSL: Show Shader Compilation** from the Command Palette. Both
commands operate on the active open HLSL document; there is no separate
compilation view and a separate reflection view, and neither command accepts
a variant argument.

## Protocol

Request:

```text
hlsl/compilationInfo
```

Parameters:

```json
{
  "textDocument": { "uri": "file:///C:/shaders/example.hlsl" }
}
```

The document must be open. The server compiles the document's current
(possibly unsaved) in-memory snapshot using its already-resolved
`shadertoolsconfig.json` configuration and the document's active variant, so
the client never sends a position or variant parameter. The result has this
shape:

```json
{
  "entryPoint": "PSMain",
  "stage": "pixel",
  "targetProfile": "ps_6_6",
  "languageVersion": "2021",
  "defines": ["USE_TINT=1"],
  "compilerArguments": ["-E", "PSMain", "-T", "ps_6_6", "-D", "USE_TINT=1"],
  "includeDirectories": ["C:/shaders/include"],
  "resolvedIncludePaths": ["C:/shaders/include/common.hlsli"],
  "activeVariant": "Tinted",
  "success": true,
  "diagnostics": [],
  "output": { "type": "dxil", "size": 2048 },
  "reflection": {
    "available": true,
    "unavailableReason": "",
    "inputSignature": [
      {
        "semanticName": "SV_Position",
        "semanticIndex": 0,
        "register": 0,
        "systemValue": "position",
        "componentType": "float32",
        "mask": 15,
        "readWriteMask": 0,
        "stream": 0
      }
    ],
    "outputSignature": [],
    "resources": [
      {
        "name": "AlbedoTexture",
        "type": "texture",
        "bindPoint": 0,
        "bindCount": 1,
        "space": 0,
        "returnType": "float",
        "dimension": "texture2D"
      }
    ],
    "threadGroupSize": null
  }
}
```

`entryPoint`, `targetProfile`, `languageVersion`, `defines`, and
`includeDirectories` are read back from the DXC command line the server
already resolved for this document (`-E`, `-T`, `-HV`, `-D`, and `-I`
respectively), so they reflect `shadertoolsconfig.json`, editor overrides, and
the active variant's contribution together. `stage` is derived from the
target profile's prefix (for example `ps_*` is `pixel`, `cs_*` is `compute`).
`activeVariant` is `null` when no variant is selected. `resolvedIncludePaths`
lists the include files the compilation actually resolved, not merely the
configured search directories.

### Compiler failures

When DXC fails to compile the document, `success` is `false`, `diagnostics`
contains one entry per compiler error or warning (`severity`, `message`,
`path`, `line`, and `column`), and both `output` and `reflection` are `null`
because no compiled output exists to describe or reflect. Editors should
treat a `null` `output`/`reflection` as "no output was produced", distinct
from a successful compile whose reflection is merely unavailable (see below).

### DXIL vs SPIR-V reflection availability

`output.type` is `"dxil"` or `"spirv"` depending on the target profile and
compiler arguments. Reflection is always attempted, but its availability
depends on the output:

- **DXIL** output supports full reflection: `reflection.available` is `true`,
  and `inputSignature`, `outputSignature`, `resources`, and (for compute
  shaders) `threadGroupSize` are populated from DXC's reflection API.
- **SPIR-V** output does not currently support this reflection path.
  `reflection.available` is `false` and `reflection.unavailableReason`
  explains why in prose; the signature and resource arrays are empty and
  `threadGroupSize` is `null`.

`threadGroupSize` is `null` for non-compute stages even when reflection is
otherwise available.

## Unsaved edits and active-variant refresh

Because the server always compiles the document's current in-memory
snapshot and current active variant, the client does not need to (and must
not) pass a variant or content parameter of its own:

- An unsaved edit in the active document is reflected the next time
  `hlsl/compilationInfo` is requested, without saving.
- Selecting a different active variant for the document is reflected the
  next time `hlsl/compilationInfo` is requested, without reopening the
  document or restarting the server.

Both clients always fetch fresh data when the Shader Compilation
command/view is invoked. If the view is already open for a document, both
clients also refresh it automatically:

- **Visual Studio Code** refreshes the open webview when the active variant
  changes, when the document is saved, and (debounced, to avoid a request per
  keystroke) shortly after the document is edited.
- **Visual Studio** refreshes the open tool window when the active variant
  changes and when the shown document is saved. Visual Studio does not
  currently refresh on every keystroke; run the Tools command again, or save
  the document, to see an in-progress edit reflected sooner.

Every refresh path is guarded by a monotonically increasing request
generation counter, mirroring the memory-layout implementation, so a stale
response from a superseded request can never overwrite a newer one.

Requests use the same versioned analysis queue as hover and other
interactive features. Cancellation returns the standard request-cancelled
error, and an edit that supersedes the analyzed source returns
`ContentModified`.
