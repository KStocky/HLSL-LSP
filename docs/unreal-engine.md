# Unreal Engine shaders

HLSL-LSP can analyze shaders directly from an Unreal Engine source tree when
its physical include directories and Unreal virtual shader roots are described
in `shadertoolsconfig.json`. A source checkout is sufficient for navigation,
preprocessor inspection, diagnostics, and many semantic features, but it is not
a complete Unreal shader compilation environment: Unreal generates and injects
some shader sources while compiling a project.

## Source-tree configuration

The following example assumes the configuration file is in the Unreal checkout
root. Add only mappings whose target directories exist; HLSL-LSP reports a
configuration error for a missing mapping target.

```jsonc
{
  "root": true,

  "hlsl.additionalIncludeDirectories": [
    "Engine/Shaders",
    "Engine/Shaders/Private",
    "Engine/Shaders/Public",
    "Engine/Source/Runtime/Core/Public"
  ],

  "hlsl.virtualDirectoryMappings": {
    "/Engine": "Engine/Shaders",
    "/Plugin/Water": "Engine/Plugins/Experimental/Water/Shaders",
    "/Plugin/DynamicWind": "Engine/Plugins/Experimental/DynamicWind/Shaders",
    "/Plugin/FX/Niagara": "Engine/Plugins/FX/Niagara/Shaders",
    "/Plugin/GPULightmass": "Engine/Plugins/Experimental/GPULightmass/Shaders"
  },

  "hlsl.languageVersion": "2021",
  "hlsl.targetProfile": "lib_6_6"
}
```

Unreal assigns a virtual root to each shader-bearing plugin. Add the roots for
the plugins being edited using the same pattern. `/Project` can similarly map
to a project's `Shaders` directory when that directory exists.

Physical C++ headers included by shared shader headers, including
`Math/MathFwd.h` and `Misc/LargeWorldRenderPosition.h`, resolve through
`Engine/Source/Runtime/Core/Public`.

Platform shader overlays are selected by Unreal for a shader format. A generic
source checkout may not contain the effective `/Platform/Public` and
`/Platform/Private` trees, so those paths can remain unresolved until the
corresponding platform sources are available.

## Compilation variants

Many `.usf` files contain several stages or permutations under mutually
exclusive preprocessor branches. Define named variants instead of applying one
entry point and macro set to the entire tree:

```jsonc
{
  "hlsl.variants": [
    {
      "name": "Water quadtree traversal",
      "files": [
        "Engine/Plugins/Experimental/Water/Shaders/Private/WaterQuadTreeDraws.usf"
      ],
      "hlsl.targetProfile": "cs_6_6",
      "hlsl.entryPoint": "MainCS",
      "hlsl.preprocessorDefinitions": {
        "QUAD_TREE_TRAVERSE": 1
      }
    },
    {
      "name": "Volumetric cloud render view",
      "files": ["Engine/Shaders/Private/VolumetricCloud.usf"],
      "hlsl.targetProfile": "cs_6_6",
      "hlsl.entryPoint": "MainCS",
      "hlsl.preprocessorDefinitions": {
        "SHADER_RENDERVIEW_CS": 1
      }
    }
  ]
}
```

Real permutations usually require additional platform, feature-level, and
material definitions. Copy those values from the Unreal compile environment
being investigated rather than treating this minimal example as a complete
shipping permutation.

## Generated shader sources

The following common paths are generated or injected at runtime and may be
absent from a source checkout:

- `/Engine/Generated/Material.ush`
- `/Engine/Generated/VertexFactory.ush`
- `/Engine/Generated/GeneratedUniformBuffers.ush`
- `/Engine/Generated/UniformBuffers/*.ush`
- `/Engine/Generated/NiagaraEmitterInstance.ush`
- `/Engine/Generated/ShaderAutogen/*`

Preprocessor Explorer reports these as unresolved edges without failing the
server. Do not map them to placeholder or nonexistent directories. For an
authoritative material, Niagara, or vertex-factory compilation, capture an
Unreal shader debug dump for the desired project, platform, and permutation,
then open the dumped source tree with its generated includes and compiler
definitions.

Diagnostics from a source-only Niagara configuration can be numerous because
the translated graph and generated uniform buffers do not exist. That is an
incomplete compilation environment, not by itself an HLSL-LSP parser failure.

## Representative results

Validation against ten current Unreal shader roots covered BasePass, Nanite,
VolumetricCloud, Lumen, Virtual Shadow Maps, path tracing, GPU Lightmass,
Niagara, Water, and DynamicWind.

| Operation | Representative result |
|---|---|
| Initial diagnostics on ordinary roots | 0.24-1.24 seconds |
| Preprocessor graph | 57-279 files in 1.1-5.4 seconds |
| Unsaved-edit refresh on ordinary roots | 0.8-2.2 seconds |
| Working set | 34-136 MB |
| DynamicWind document symbols | About 3.3 seconds, bounded at 1,024 declarations |
| Water references | About 2.1 seconds |
| Water entry-point data flow | About 1.9 seconds with bounded dead-code analysis |

Document-symbol collection emits a `window/logMessage` warning when compiler
expansion produces more than 1,024 declarations. Entry-point data flow keeps
the active entry point and reachable graph when the all-definition budget is
exhausted, but marks the response truncated and omits claims that require a
complete definition set.

Two pathological roots, `NaniteRasterizer.usf` and
`PathTracingMaterialHitShader.usf`, did not finish DXC initial analysis within
180 seconds. Their resolved source footprints were smaller than a tested
BasePass translation unit that completed normally, so a source-count or byte
cutoff would reject valid shaders without reliably detecting this failure.
This remains tracked as
[issue #77](https://github.com/KStocky/HLSL-LSP/issues/77); safely enforcing a
hard compiler deadline requires isolating uninterruptible DXC work rather than
abandoning a thread inside the server process.

