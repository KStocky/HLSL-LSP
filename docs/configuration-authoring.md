# Configuration authoring protocol

`hlsl/configurationAuthoring` is the editor-independent, versioned discovery
and preview API for guided `shadertoolsconfig.json` setup. The server only
reads workspace files and returns preview text. It never creates or modifies a
configuration file.

## Request

```text
hlsl/configurationAuthoring
```

```json
{
  "protocolVersion": 1,
  "workspaceFolder": {
    "uri": "file:///C:/work/project"
  },
  "existingConfiguration": {
    "content": "{\n  \"root\": true\n}\n",
    "version": 12,
    "contentHash": "sha256:..."
  },
  "selections": [
    {
      "relativePath": "Shaders/lighting.hlsl",
      "entryPoint": "MainPS",
      "targetProfile": "ps_6_6"
    }
  ],
  "draftContent": "{\n  ...\n}\n"
}
```

- `protocolVersion` is required and must be `1`.
- `workspaceFolder.uri` is required and must identify one of the workspace
  folders supplied during `initialize`.
- `existingConfiguration` is optional. `content`, `version`, and
  `contentHash` are independently optional. When content is omitted, the
  server reads the root `shadertoolsconfig.json`, if present. A supplied hash
  must match the supplied or on-disk content.
- `selections` is optional. Every selection must match a file, entry point, and
  target profile returned by compiler discovery. When omitted, the preview includes
  only candidates for which exactly one probed profile succeeded. Ambiguous
  candidates remain in the result for the client to resolve explicitly.
- `draftContent` is optional (maximum 2 MiB). After reviewing the generated
  preview, a client may submit the user's edited JSON to validate all supported
  settings, including include paths, mappings, defines, compiler options,
  variants, and pipelines. It is validated by the production parser without
  applying generation again. The expected version and hash still refer to the
  original file, not this draft; a client must compare them before applying.
  Invalid existing content can be repaired using a valid draft. Clients must
  make edited drafts visible and require confirmation before applying them.

Clients should normally send their current in-memory configuration content and
document version. Before applying the returned preview, compare
`configuration.expectedContentVersion` and
`configuration.expectedContentHash` with the current editor buffer. If either
no longer matches, discard the preview and request a new one.

Discovery uses the document's configured backend. Saved FXC configurations and
explicit FXC editor settings probe native SM5.0 profiles, not DXC's SM6.6
profiles. Set and save the backend before rediscovering candidates; editing a
preview does not change the compiler used for discovery.

## Result

```json
{
  "protocolVersion": 1,
  "configuration": {
    "uri": "file:///C:/work/project/shadertoolsconfig.json",
    "exists": true,
    "expectedContentVersion": 12,
    "expectedContentHash": "sha256:..."
  },
  "discovery": {
    "files": [
      {
        "uri": "file:///C:/work/project/Shaders/lighting.hlsl",
        "relativePath": "Shaders/lighting.hlsl",
        "size": 2048,
        "entryPoints": [
          {
            "name": "MainPS",
            "location": { "line": 18, "character": 7 },
            "targetProfiles": ["ps_6_6"],
            "state": "resolved",
            "explanation": "DXC validated exactly one probed shader profile."
          },
          {
            "name": "SharedMain",
            "location": { "line": 40, "character": 7 },
            "targetProfiles": ["vs_6_6", "ps_6_6"],
            "state": "ambiguous",
            "explanation": "DXC validated multiple shader profiles; the client must choose one."
          }
        ],
        "truncated": false,
        "analysisError": null
      }
    ],
    "nestedConfigurations": [
      "file:///C:/work/project/ThirdParty/shadertoolsconfig.json"
    ],
    "directoriesVisited": 14,
    "compilerProbes": 16,
    "truncated": false,
    "truncationReason": null,
    "limits": {
      "maxDirectories": 2048,
      "maxFiles": 256,
      "maxFileSize": 2097152,
      "maxEntryPointsPerFile": 32,
      "maxCompilerProbes": 96
    }
  },
  "preview": {
    "content": "{\n  ...\n}\n",
    "contentHash": "sha256:...",
    "valid": true,
    "changed": true,
    "errors": []
  }
}
```

Entry-point locations are zero-based. `state` is:

- `resolved`: exactly one probed target profile compiled successfully;
- `ambiguous`: multiple profiles compiled, multiple compiler definitions have
  the same entry-point name, or the probe budget prevented a complete answer;
- `unresolved`: every profile was probed and none compiled.

Entry-point enumeration uses DXC's cursor tree and admits only free-function
definitions at translation-unit or namespace scope. Target profiles are
reported only after a real DXC compilation succeeds. The bounded profile set is
`vs_6_6`, `ps_6_6`, `cs_6_6`, `gs_6_6`, `hs_6_6`, `ds_6_6`, `ms_6_6`, and
`as_6_6`. Compiler work runs through the normal isolated analysis workers and
honors JSON-RPC cancellation and worker deadlines.

`preview.valid` means the preview passed the production configuration parser.
Errors contain stable `code`, field-addressable `field`, and human-readable
`message` values. Malformed existing JSON is returned unchanged with
`valid: false`; the server does not replace content it cannot safely merge.

## Deterministic editing

For a new file, the preview adds the v1 `$schema`, `root: true`, and
`hlsl.languageVersion: "2021"`. Compiler-validated selections become ordered
`hlsl.fileGroups`. Files, candidates, profiles, and generated groups are
ordered deterministically.

For existing JSON, the server parses with the production JSON-with-comments
behavior, preserves all existing properties (including unknown top-level
properties), and appends only missing selected file groups. Existing file
groups, virtual mappings, variants, pipelines, and other managed settings are
not replaced. The normalized preview may change whitespace or remove comments;
clients must always show the preview before applying it.

## Discovery boundaries

Discovery skips files larger than 2 MiB and prunes common generated,
dependency, and output directories:

`.git`, `.hg`, `.svn`, `.vs`, `bin`, `Binaries`, `build`,
`cmake-build-debug`, `cmake-build-release`, `DerivedDataCache`, `dist`,
`external`, `generated`, `Intermediate`, `node_modules`, `obj`, `out`,
`packages`, `Saved`, `third_party`, `ThirdParty`, and `vendor`.

The comparison is ASCII case-insensitive. A directory below the requested
workspace containing its own `shadertoolsconfig.json` is reported in
`nestedConfigurations` and pruned, so root authoring never absorbs a nested
configuration's shaders. Hitting any directory, file, symbol, or compiler-probe
limit sets `discovery.truncated` and identifies the bound in
`truncationReason`.
