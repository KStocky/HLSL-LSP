# Tutorial: switch shader compilation variants

This tutorial configures one HLSL file as either a vertex shader or a debug
pixel shader, then switches between those configurations without editing
compiler arguments by hand.

The finished workspace contains:

```text
workspace/
├── shadertoolsconfig.json
└── Shaders/
    └── VariantTutorial.hlsl
```

## 1. Create the shader

Create `Shaders/VariantTutorial.hlsl`:

```hlsl
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput MainVS(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = positions[vertexId] * 0.5 + 0.5;
    return output;
}

float4 MainPS(VertexOutput input) : SV_Target
{
#if DEBUG_TINT
    return float4(input.uv, 1.0, 1.0);
#else
    return float4(input.uv, 0.0, 1.0);
#endif
}
```

## 2. Declare the variants

Create `shadertoolsconfig.json` in the workspace root:

```jsonc
{
  "root": true,
  "hlsl.languageVersion": "2021",
  "hlsl.variantsVersion": 1,
  "hlsl.variants": [
    {
      "name": "Vertex",
      "description": "Compile the full-screen triangle vertex entry point.",
      "default": true,
      "files": ["Shaders/VariantTutorial.hlsl"],
      "hlsl.entryPoint": "MainVS",
      "hlsl.targetProfile": "vs_6_6"
    },
    {
      "name": "Pixel Debug",
      "description": "Compile the pixel entry point with its debug tint.",
      "files": ["Shaders/VariantTutorial.hlsl"],
      "hlsl.entryPoint": "MainPS",
      "hlsl.targetProfile": "ps_6_6",
      "hlsl.preprocessorDefinitions": {
        "DEBUG_TINT": 1
      }
    }
  ]
}
```

`files` is relative to the configuration file. A variant is only applied when
the active shader matches one of its patterns.

## 3. Select and verify a variant

1. Open `VariantTutorial.hlsl`.
2. Select a variant:
   - **Visual Studio Code:** run **HLSL: Select Shader Variant** from the
     Command Palette, or select the variant from the HLSL status-bar item.
   - **Visual Studio:** right-click the shader and choose
     **HLSL > Select Shader Variant**.
3. Choose **Vertex**.
4. Open Shader Compilation:
   - **Visual Studio Code:** run **HLSL: Show Shader Compilation**.
   - **Visual Studio:** right-click the shader and choose
     **HLSL > Shader Compilation**.
5. Confirm that the view reports:
   - active variant `Vertex`;
   - entry point `MainVS`;
   - target profile `vs_6_6`.
6. Select **Pixel Debug** and reopen or refresh Shader Compilation. It should
   report `MainPS`, `ps_6_6`, and `DEBUG_TINT=1`.

Diagnostics, completion, inlay hints, compilation, and resource reflection all
follow the selected variant. The server only restarts when the newly selected
variant chooses a different DXC runtime.

DXC compiles one selected configuration at a time. Selecting **Vertex** does
not also compile or diagnose `MainPS`; switch to **Pixel Debug** when you need
results for that permutation.

## 4. Exercise applicability

Create another shader outside `Shaders/VariantTutorial.hlsl`, then try to
select **Pixel Debug** while it is active. The variant picker only lists
variants available for the current document. If a configured selection becomes
undefined or inapplicable, that document uses its default configuration and
HLSL-LSP reports the unavailable selection instead of pretending it applied.

## Troubleshooting

- **The picker is empty:** confirm the document is open, the configuration is
  discoverable from its directory, and `hlsl.variantsVersion` is `1`.
- **The target or entry point did not change:** explicit editor settings have
  higher precedence than variants. Clear `hlsl.targetProfile`,
  `hlsl.entryPoint`, or related editor overrides when the variant should
  control them.
- **A variant is rejected:** names must be unique, inheritance must not contain
  unknown names or cycles, and every `files` pattern must be valid.
- **The server restarts:** this is expected only when a variant changes
  `hlsl.dxcRuntimeDirectory`.

For the complete schema, inheritance rules, and precedence order, see
[Named compilation variants](../shadertoolsconfig.md#named-compilation-variants).
