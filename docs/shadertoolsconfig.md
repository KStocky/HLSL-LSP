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
  ]
}
```

## Discovery and merging

For each shader, HLSL-LSP looks for `shadertoolsconfig.json` in the shader's
directory and then each parent directory. Discovery stops at the filesystem
root or the first file containing `"root": true`.

Files are merged from the outermost directory toward the shader:

- Definition and virtual-mapping entries in a nearer file override entries
  with the same key in an outer file.
- `languageVersion`, `targetProfile`, `entryPoint`, and
  `additionalArguments` are replaced by the nearest file that declares them.
- Include directories from all discovered files are combined, with nearer
  directories searched first and duplicate resolved paths removed.

Relative include directories and virtual-mapping targets are resolved from the
directory containing the configuration file that declares them. Every
configured path must already exist and must be a directory; configuration
errors are reported rather than silently ignored.

## Properties

| Property | Type | Behavior |
| --- | --- | --- |
| `root` | Boolean | Stops discovery above this file when `true`. |
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
3. Discovered configuration files, from outermost to nearest.
4. Editor workspace settings.

An editor property replaces the corresponding file-derived property. Empty
arrays and objects deliberately clear inherited values, while omitted
properties leave them unchanged. Relative editor paths resolve from the
containing workspace folder.
