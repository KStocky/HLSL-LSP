# `shadertoolsconfig.json`

HLSL-LSP supports the `shadertoolsconfig.json` format created by
[Tim Jones for HLSL Tools](https://github.com/tgjones/HlslTools#custom-preprocessor-definitions-and-additional-include-directories).
The shared fields make existing HLSL Tools configurations reusable. HLSL-LSP
also adds DXC-oriented language-version, target, entry-point, and argument
settings.

## Example

JSON comments are accepted.

```jsonc
{
  // Do not inspect parent directories above this file.
  "root": true,

  "hlsl.preprocessorDefinitions": {
    "PLATFORM_WINDOWS": 1,
    "USE_RAYTRACING": true,
    "BARE_DEFINE": ""
  },

  "hlsl.additionalIncludeDirectories": [
    "Shaders/Shared",
    "../Engine/Shaders"
  ],

  "hlsl.virtualDirectoryMappings": {
    "/Project": "Shaders",
    "/Engine": "../Engine/Shaders"
  },

  "hlsl.languageVersion": "2021",
  "hlsl.targetProfile": "lib_6_6",
  "hlsl.entryPoint": "Main",

  "hlsl.additionalArguments": [
    "-enable-16bit-types"
  ],

  // Optional: load a checked-in DXC runtime instead of the bundled default.
  "hlsl.dxcRuntimeDirectory": "Tools/dxc/bin",

  "hlsl.fileGroups": [
    {
      "name": "Compute shaders",
      "files": ["Compute/*.hlsl", "*-compute.hlsl"],
      "hlsl.targetProfile": "cs_6_7",
      "hlsl.entryPoint": "CSMain",
      "hlsl.preprocessorDefinitions": {
        "COMPUTE_SHADER": true
      }
    },
    {
      "name": "Pixel shaders",
      "files": ["Materials/**/*.ps.hlsl"],
      "hlsl.targetProfile": "ps_6_6",
      "hlsl.entryPoint": "PSMain",
      "hlsl.preprocessorDefinitions": {
        "PIXEL_SHADER": true
      }
    }
  ]
}
```

## Discovery and merging

For each shader file, HLSL-LSP looks for `shadertoolsconfig.json` in the
shader's directory and then each parent directory. Discovery stops at the
filesystem root or the first file containing `"root": true`. Configurations
without `hlsl.fileGroups` keep their existing behavior.

Settings are applied in this order, from lowest to highest precedence:

1. Normal settings in discovered files, outermost to nearest.
2. Matching file groups, processing each declaring file outermost to nearest.
   Within one file, groups are processed in array order. A shader may match
   more than one group, so a later matching group has higher precedence.

## Configured macro includes

An object-like configured definition may be used as an `#include` expression
when its fully expanded value is exactly one quoted or angle header-name token:

```jsonc
{
  "hlsl.preprocessorDefinitions": {
    "STF_ASSERTIONS": "\"/Test/STF/AssertionsV1/Framework.hlsli\"",
    "PROJECT_HEADER": "PROJECT_HEADER_VALUE",
    "PROJECT_HEADER_VALUE": "<Project/Shared.hlsli>"
  }
}
```

The backslashes before the quotes are JSON escaping; the effective
`STF_ASSERTIONS` value passed to DXC is
`"/Test/STF/AssertionsV1/Framework.hlsli"`. HLSL-LSP resolves bounded alias
chains only when every alias comes from the document's effective configuration,
then applies the normal relative, additional-include-directory, open-buffer,
and virtual-directory-mapping rules. Active variants and editor overrides are
therefore honored automatically.

DXC remains the preprocessing authority. Source-defined or function-like
macros, undefined aliases, cycles, malformed header names, multi-token values,
and expansions beyond 32 expansion steps or a 4 KiB definition value remain
dynamic and are not guessed by the workspace resolver. The resolver also
leaves an include dynamic when a later `hlsl.additionalArguments` `-D`, `/D`,
`-U`, or `/U` switch can modify any macro in its alias chain. Split and joined
switch spellings are recognized without interpreting their replacement values.
Response-file arguments and unusually large argument lists are treated as
opaque because they can hide macro mutations; configured macro include
expansion is conservatively disabled for that compilation.

Across those layers:

- Definition and virtual-mapping objects merge by key; a higher-precedence
  value replaces the same key.
- `languageVersion`, `targetProfile`, `entryPoint`, and
  `additionalArguments` are replaced as a whole when a higher-precedence layer
  declares them.
- Include directories combine rather than replace. Higher-precedence
  directories are searched first, and duplicate resolved paths are removed.

Relative include directories and virtual-mapping targets are resolved from the
directory containing the configuration file that declares them. Every
configured path must already exist and must be a directory; configuration
errors are reported rather than silently ignored.

## File groups

`hlsl.fileGroups` is an ordered array of objects. Each object has this shape:

```jsonc
{
  "name": "Optional display name",
  "files": ["required-*.hlsl", "Shaders/**/*.hlsli"],

  // Any normal HLSL settings may appear directly in the group.
  "hlsl.preprocessorDefinitions": {},
  "hlsl.additionalIncludeDirectories": [],
  "hlsl.virtualDirectoryMappings": {},
  "hlsl.languageVersion": "2021",
  "hlsl.targetProfile": "lib_6_8",
  "hlsl.entryPoint": "Main",
  "hlsl.additionalArguments": []
}
```

`files` is required and must contain at least one string. A group matches when
any of its patterns matches the shader:

- Patterns are relative to the directory containing the declaring
  `shadertoolsconfig.json`.
- `/` and `\` are both accepted as separators and normalized.
- `*` matches zero or more characters other than a path separator.
- `?` matches exactly one character other than a path separator.
- `**`, when used as a complete path segment, matches zero or more path
  segments.
- Character classes such as `[0-9]` are not supported.
- A pattern with no separator, such as `*.hlsl`, matches the filename at any
  depth inside the config's directory.
- Matching uses Unicode ordinal case-insensitive comparison on Windows and is
  case-sensitive elsewhere.
- A file outside the declaring config's directory cannot match. Patterns must
  be relative and cannot contain `.` or `..` path segments.

Invalid group objects, patterns, or settings are configuration errors even
when the current shader would not match that group. Changes to a watched
`shadertoolsconfig.json` cause open shaders in its directory tree to be
reanalyzed so their effective groups are refreshed.

## Named compilation variants

A single shader file often supports several compilation permutations, such as
distinct entry points, stage/target profiles, macro sets, or platform switches.
`hlsl.variants` declares those permutations by name so an editor can select the
active one instead of hand-editing settings. The active variant is applied when
analyzing each document it applies to, and changing it reanalyzes open
documents.

For a complete shader/configuration walkthrough in both editors, see the
[Shader variants tutorial](tutorials/shader-variants.md).

```jsonc
{
  "root": true,

  // Required whenever hlsl.variants is present. Only version 1 is supported.
  "hlsl.variantsVersion": 1,

  "hlsl.variants": [
    {
      "name": "Base",
      "description": "Shared defines for every permutation.",
      "hlsl.preprocessorDefinitions": { "PLATFORM_PC": 1 }
    },
    {
      "name": "Vertex",
      "inherits": "Base",
      "hlsl.entryPoint": "MainVS",
      "hlsl.targetProfile": "vs_6_6"
    },
    {
      "name": "Pixel Debug",
      "inherits": ["Base", "Vertex"],
      "default": true,
      "files": ["Materials/**/*.hlsl"],
      "hlsl.entryPoint": "MainPS",
      "hlsl.targetProfile": "ps_6_6",
      "hlsl.preprocessorDefinitions": { "DEBUG": 1 },
      "hlsl.additionalArguments": ["-Zi"]
    }
  ]
}
```

Each variant object has this shape:

- `name` is required, non-empty, and unique across every discovered
  configuration file. It is the identifier used to select the variant.
- `description` is an optional human-readable string shown by the editor
  pickers.
- `default` is an optional Boolean that marks a suggested default variant.
- `inherits` is an optional variant name, or array of names, whose resolved
  settings are applied before this variant's own settings. Later entries and the
  variant's own settings win. Inheritance is resolved deterministically;
  unknown bases and inheritance cycles are configuration errors.
- `files` is an optional, non-empty array of the same glob patterns used by
  `hlsl.fileGroups`. When present, the variant only applies to matching shaders
  (relative to the declaring configuration's directory). When omitted, the
  variant applies to every shader under that directory.
- Any normal HLSL setting may appear directly in the variant, including
  `hlsl.dxcRuntimeDirectory`. Unlike file groups, a variant may select a DXC
  runtime because selecting a variant is a workspace-wide action, not a per-file
  one.

Selecting a variant applies its settings on top of the file-derived
configuration but below explicit editor overrides:

- Definition and virtual-mapping objects merge by key.
- `languageVersion`, `targetProfile`, `entryPoint`, and `additionalArguments`
  are replaced as a whole when the variant declares them.
- Include directories combine, with the variant's directories searched first.
- A variant's `dxcRuntimeDirectory` replaces the file-derived selection.

Changing the active variant reanalyzes open documents. It does not restart the
language server unless the newly selected variant chooses a different DXC
runtime, in which case the same controlled restart used by
[DXC runtime selection](#dxc-runtime-selection) loads it.

### Selecting the active variant

- **Visual Studio Code:** run **HLSL: Select Shader Variant** (also available in
  the status bar) or set `hlsl.activeVariant`.
- **Visual Studio:** right-click the shader and choose
  **HLSL > Select Shader Variant**.

Both clients read the available variants from the server, so the picker only
lists variants declared for the current document.

### Invalid and conflicting variants

Problems are reported clearly rather than applied silently:

- Missing or unsupported `hlsl.variantsVersion`, duplicate variant names,
  unknown or cyclic `inherits`, and malformed variant objects are configuration
  errors surfaced as editor messages.
- Selecting a variant that is not defined for, or not applicable to, the open
  documents leaves those documents on their default configuration and reports
  the selection as unavailable.
- If open documents resolve the active variant to different DXC runtimes, the
  active runtime is left unchanged and the conflict is reported, exactly as for
  `hlsl.dxcRuntimeDirectory`.

## Properties

| Property | Type | Behavior |
| --- | --- | --- |
| `root` | Boolean | Stops discovery above this file when `true`. |
| `hlsl.fileGroups` | Array of file-group objects | Applies file-specific settings using the ordered matching and precedence rules above. |
| `hlsl.variantsVersion` | Integer | Declares the named-variants schema version. Required when `hlsl.variants` is present; only `1` is supported. |
| `hlsl.variants` | Array of variant objects | Declares selectable named compilation variants; see [Named compilation variants](#named-compilation-variants). |
| `hlsl.preprocessorDefinitions` | Object | Adds DXC defines. Values may be strings, numbers, or Booleans. An empty string emits a value-less define. Object-like values that expand to exactly one header-name token may resolve `#include MACRO`; quoted header values require JSON-escaped quotes. |
| `hlsl.additionalIncludeDirectories` | String array | Adds existing directories to DXC's include search path. Relative paths are resolved from this config file. |
| `hlsl.virtualDirectoryMappings` | Object of string paths | Maps virtual include roots to existing directories, primarily for Unreal-style paths. Each virtual key must begin with `/` or `\`. |
| `hlsl.languageVersion` | String | Sets DXC `-HV`, for example `2016`, `2018`, `2021`, or `202x`. The built-in default is `2021`. |
| `hlsl.targetProfile` | String | Sets the DXC target profile, such as `ps_6_0`, `cs_6_7`, or `lib_6_8`. |
| `hlsl.entryPoint` | String | Sets the shader entry point supplied to DXC. |
| `hlsl.additionalArguments` | String array | Replaces the inherited list of extra DXC arguments, such as `-spirv` or `-enable-16bit-types`. |
| `hlsl.dxcRuntimeDirectory` | String | Selects a compatible DXC runtime directory to load instead of the bundled default. Process-wide; see [DXC runtime selection](#dxc-runtime-selection). |

Use a Shader Model 6.6 or newer profile when code references
`ResourceDescriptorHeap` or `SamplerDescriptorHeap`.

## DXC runtime selection

By default HLSL-LSP loads the bundled, pinned DXC runtime. `hlsl.dxcRuntimeDirectory`
selects a different compatible DXC runtime for a project without changing the
bundled default for other workspaces.

DXC IntelliSense is loaded only in the language server's isolated analysis
worker processes. Every worker is launched with the same selected runtime, so
the runtime cannot vary per file. The selection therefore has *process-wide,
workspace-level* semantics:

- The value is a directory that must contain the platform DXC compiler library
  (`dxcompiler.dll`, plus `dxil.dll`, on Windows; `libdxcompiler.so` on Linux).
  The directory and its matching compiler library are validated before the
  server restarts.
- Relative values resolve from the directory containing the declaring
  configuration file, so checked-in, workspace-relative paths remain
  environment independent.
- `hlsl.dxcRuntimeDirectory` may only appear at the top level of a
  configuration file. It is rejected inside `hlsl.fileGroups`, because a runtime
  cannot be chosen per file.
- If configuration files discovered for a shader disagree on the runtime
  directory, HLSL-LSP reports an actionable configuration error instead of
  silently switching runtimes. Nested files may repeat the same resolved
  directory, but they must not select different ones.

Changing the effective runtime triggers a single controlled restart of the
language server; open documents are re-analyzed against the new runtime. Invalid,
missing, incompatible, or conflicting selections are reported without restarting,
so a bad value cannot cause a restart loop. The active runtime path and version
are available through the `hlsl/dxcRuntime` request and each client's diagnostics
command.

## Live configuration updates

Creating, changing, deleting, or moving a `shadertoolsconfig.json` file
reanalyzes affected open shaders without requiring the editor or document to be
restarted. Visual Studio refreshes open configuration-dependent HLSL tool windows
only after that reanalysis completes, so compilation, reflection, preprocessing,
data-flow, compute-visualization, call-hierarchy, and navigation results do not
race against stale settings.

## Editor-setting precedence

Editors can send the same properties as members of the `hlsl` object through
`workspace/didChangeConfiguration`. The effective precedence, from lowest to
highest, is:

1. Built-in defaults.
2. Client defaults, such as the Visual Studio default language version.
3. Normal settings from discovered configuration files, outermost to nearest.
4. Matching file groups, by config from outermost to nearest and then by
   declaration order.
5. The active named variant, when one is selected and applies to the document.
6. Editor workspace settings.

An editor property replaces the corresponding file-derived property. Empty
arrays and objects deliberately clear inherited values, while omitted
properties leave them unchanged. Relative editor paths resolve from the
containing workspace folder.

For `hlsl.dxcRuntimeDirectory`, an explicit editor setting (Visual Studio Code's
`hlsl.dxcRuntimeDirectory` or Visual Studio's **Tools > Options > HLSL-LSP > DXC
runtime directory**) overrides any `shadertoolsconfig.json` selection. Clearing
the editor setting restores the `shadertoolsconfig.json`-driven selection, or the
bundled default when none is configured.
