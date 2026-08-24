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

## Properties

| Property | Type | Behavior |
| --- | --- | --- |
| `root` | Boolean | Stops discovery above this file when `true`. |
| `hlsl.fileGroups` | Array of file-group objects | Applies file-specific settings using the ordered matching and precedence rules above. |
| `hlsl.preprocessorDefinitions` | Object | Adds DXC defines. Values may be strings, numbers, or Booleans. An empty string emits a value-less define. |
| `hlsl.additionalIncludeDirectories` | String array | Adds existing directories to DXC's include search path. Relative paths are resolved from this config file. |
| `hlsl.virtualDirectoryMappings` | Object of string paths | Maps virtual include roots to existing directories, primarily for Unreal-style paths. Each virtual key must begin with `/` or `\`. |
| `hlsl.languageVersion` | String | Sets DXC `-HV`, for example `2016`, `2018`, `2021`, or `202x`. The built-in default is `2021`. |
| `hlsl.targetProfile` | String | Sets the DXC target profile, such as `ps_6_0`, `cs_6_7`, or `lib_6_8`. |
| `hlsl.entryPoint` | String | Sets the shader entry point supplied to DXC. |
| `hlsl.additionalArguments` | String array | Replaces the inherited list of extra DXC arguments, such as `-spirv` or `-enable-16bit-types`. |

Use a Shader Model 6.6 or newer profile when code references
`ResourceDescriptorHeap` or `SamplerDescriptorHeap`.

## Editor-setting precedence

Editors can send the same properties as members of the `hlsl` object through
`workspace/didChangeConfiguration`. The effective precedence, from lowest to
highest, is:

1. Built-in defaults.
2. Client defaults, such as the Visual Studio default language version.
3. Normal settings from discovered configuration files, outermost to nearest.
4. Matching file groups, by config from outermost to nearest and then by
   declaration order.
5. Editor workspace settings.

An editor property replaces the corresponding file-derived property. Empty
arrays and objects deliberately clear inherited values, while omitted
properties leave them unchanged. Relative editor paths resolve from the
containing workspace folder.
