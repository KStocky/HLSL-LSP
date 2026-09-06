# HLSL preprocessor explorer

HLSL-LSP exposes the active shader's include graph, skipped conditional
regions, macro definitions, and effective compiler settings through the
`hlsl/preprocessorExplorer` request. The report uses the current unsaved
document snapshot and active shader variant.

In Visual Studio, run **Tools > HLSL Preprocessor Explorer**. In Visual Studio
Code, run **HLSL: Show Preprocessor Explorer** from the Command Palette.

## Compiler authority

Skipped regions and source macro definitions come from DXC's detailed
preprocessing record. Include resolution uses the same bounded workspace
resolver and open-document snapshots supplied to DXC. The resolver recognizes
the structural forms of `#include "file"` and `#include <file>`, but does not
evaluate conditional compilation or macro expressions.

Macro-based include expressions are therefore reported as `dynamic` without a
fabricated target. Configuration macros are shown separately from source
macros because they have configuration provenance rather than a source
definition location.

## Protocol

Request:

```text
hlsl/preprocessorExplorer
```

Parameters:

```json
{
  "textDocument": { "uri": "file:///C:/shaders/example.hlsl" }
}
```

The document must be open. The response has this shape:

```json
{
  "rootUri": "file:///C:/shaders/example.hlsl",
  "files": [
    {
      "uri": "file:///C:/shaders/example.hlsl",
      "logicalPath": "C:/shaders/example.hlsl",
      "physicalPath": "C:/shaders/example.hlsl",
      "source": "open",
      "includes": [
        {
          "path": "common.hlsli",
          "line": 0,
          "character": 10,
          "kind": "quoted",
          "status": "resolved",
          "resolvedUri": "file:///C:/shaders/common.hlsli",
          "logicalPath": "C:/shaders/common.hlsli"
        }
      ]
    }
  ],
  "skippedRegions": [
    {
      "uri": "file:///C:/shaders/example.hlsl",
      "start": { "line": 4, "character": 0 },
      "end": { "line": 6, "character": 6 }
    }
  ],
  "macros": [
    {
      "name": "FEATURE_LEVEL",
      "value": "2",
      "source": "compiler",
      "origin": "C:/shaders/example.hlsl",
      "uri": "file:///C:/shaders/example.hlsl",
      "line": 2,
      "character": 8
    }
  ],
  "settings": [
    {
      "name": "languageVersion",
      "value": "2021",
      "origin": "built-in default"
    }
  ],
  "diagnostics": []
}
```

Include `kind` is `quoted`, `angled`, or `macro`. Include `status` is
`resolved`, `missing`, `cyclic`, or `dynamic`. A resolved virtual include also
reports the mapping prefix in `mapping`.

Macro `source` is `compiler` for DXC-reported source definitions or
`configuration` for effective configured definitions. File-backed
configuration macros and settings include `originUri`; editor settings and
built-in defaults do not.

All positions are zero-based UTF-16 LSP positions. Requests are cancelled or
rejected as content-modified when their document snapshot becomes stale.

## Refresh and navigation

Both editor views refresh after relevant edits, saves, configuration changes,
and active-variant changes. The last successful report remains visible during
a cancelled or failed refresh.

Files, include directives, resolved include targets, skipped regions, source
macro definitions, and file-backed configuration origins are navigable.
Missing and dynamic include targets remain non-clickable rather than guessing a
destination.
