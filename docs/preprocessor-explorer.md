# HLSL preprocessor explorer

The editor views retain the last good result across refreshes and expose the
shared tracked-document freshness/Refresh behavior described in
[analysis result freshness](analysis-freshness.md).

HLSL-LSP exposes the active shader's include graph, skipped conditional
regions, macro definitions, and effective compiler settings through the
`hlsl/preprocessorExplorer` request. The report uses the current unsaved
document snapshot and active shader variant.

In Visual Studio, right-click the shader and choose
**HLSL > Preprocessor Explorer**. In Visual Studio Code, run
**HLSL: Show Preprocessor Explorer** from the Command Palette.

## Compiler authority

Skipped regions and source macro definitions come from DXC's detailed
preprocessing record. Include resolution uses the same bounded workspace
resolver, effective configuration, and open-document snapshots supplied to
DXC. Besides literal quoted and angle includes, the resolver can expand a
bounded object-like alias chain when every alias comes from the effective
configured definitions and the final value is exactly one header-name token.
The normal relative, additional include, unsaved-buffer, and virtual mapping
rules then apply.

Resolved configured macro includes report their original expression, expanded
header, target, and configuration origin. Source-defined or function-like
macros, undefined aliases, cycles, malformed or multi-token values, and
oversized/deep expansions remain `dynamic` without a fabricated target. The
same applies when a later additional DXC `-D`, `/D`, `-U`, or `/U` argument can
modify any name in the configured alias chain. Response-file arguments and
unusually large argument lists are treated as opaque and also keep configured
macro includes dynamic. DXC remains authoritative for replacement values and
preprocessing, and receives rewritten source only when a safely resolved
virtual macro include must be mapped to its physical file.

DXC 1.9's Linux `GetSkippedRanges` API is unsafe when IntelliSense receives
source buffers with virtual include directives rewritten to physical paths.
For those snapshots only, HLSL-LSP does not call that API. The response marks
the skipped-region section unavailable and explains why, while retaining the
resolver include graph, expanded configured path and provenance, compiler
source macros, effective settings, and normal compiler diagnostics. Windows
continues to report skipped regions for the same source.

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
  "compilerAnalysis": {
    "skippedRegions": { "available": true },
    "compilerMacros": { "available": true }
  },
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
        },
        {
          "path": "PROJECT_HEADER",
          "line": 1,
          "character": 9,
          "kind": "macro",
          "status": "resolved",
          "expandedPath": "/Project/shared.hlsli",
          "resolvedUri": "file:///C:/project/shared.hlsli",
          "logicalPath": "/Project/shared.hlsli",
          "configurationMacro": "PROJECT_HEADER",
          "configurationOrigin": "C:/shaders/shadertoolsconfig.json",
          "configurationOriginUri": "file:///C:/shaders/shadertoolsconfig.json"
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
Resolved configured macro includes additionally report `expandedPath`,
`configurationMacro`, `configurationOrigin`, and, for file-backed
configuration, `configurationOriginUri`.

Macro `source` is `compiler` for DXC-reported source definitions or
`configuration` for effective configured definitions. File-backed
configuration macros and settings include `originUri`; editor settings and
built-in defaults do not.

`compilerAnalysis` explicitly identifies whether each compiler-derived
section is authoritative for the current snapshot. An unavailable section is
empty and includes a `reason`; clients must not present that as an
authoritative "none found" result.

All positions are zero-based UTF-16 LSP positions. Requests are cancelled or
rejected as content-modified when their document snapshot becomes stale.

## Refresh and navigation

Both editor views refresh after relevant edits, saves, configuration changes,
and active-variant changes. The last successful report remains visible during
a cancelled or failed refresh.

Files, include directives, resolved include targets, skipped regions, source
macro definitions, and file-backed configuration origins are navigable.
Missing and dynamic include targets remain non-clickable rather than guessing a
destination. File-backed origins of resolved configured macro includes are
navigable.
