# Tutorial: inspect shader resource bindings

This tutorial compiles a small pixel shader with an embedded root signature,
then uses the Resource Bindings view to inspect registers, spaces, root
parameters, compatibility, and source navigation.

The finished workspace contains:

```text
workspace/
├── shadertoolsconfig.json
└── Shaders/
    └── BindingTutorial.hlsl
```

## 1. Create the shader

Create `Shaders/BindingTutorial.hlsl`:

```hlsl
#define TutorialRootSignature \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "DescriptorTable(SRV(t0, numDescriptors=1))," \
    "DescriptorTable(Sampler(s0, numDescriptors=1))," \
    "CBV(b0)"

Texture2D<float4> AlbedoTexture : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer FrameConstants : register(b0)
{
    float4 Tint;
};

[RootSignature(TutorialRootSignature)]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target
{
    return AlbedoTexture.Sample(LinearSampler, uv) * Tint;
}
```

Create `shadertoolsconfig.json` in the workspace root:

```jsonc
{
  "root": true,
  "hlsl.languageVersion": "2021",
  "hlsl.targetProfile": "ps_6_6",
  "hlsl.entryPoint": "PSMain"
}
```

## 2. Open the Resource Bindings view

1. Open `BindingTutorial.hlsl`.
2. Open the view:
   - **Visual Studio Code:** run **HLSL: Show Resource Bindings** from the
     Command Palette.
   - **Visual Studio:** run **Tools > HLSL Resource Bindings**.
3. Confirm that the resource groups contain:

| Class | Space | Register | Resource |
| --- | ---: | ---: | --- |
| CBV | 0 | `b0` | `FrameConstants` |
| SRV | 0 | `t0` | `AlbedoTexture` |
| Sampler | 0 | `s0` | `LinearSampler` |

The root-signature section should report a present signature. On Windows it
also displays the descriptor tables, root CBV, flags, and a compatible result.
Resource rows are navigable when DXC supplies one unambiguous declaration
location.

## 3. Understand what the view proves

The list comes from DXC reflection for the compiled entry point, not from a
text scan:

- Register classes and spaces are the bindings in the compiled shader.
- A finite array occupies a register range; an unbounded array occupies every
  register from its base onward.
- Collisions are only shown when two reflected ranges provably overlap in the
  same class and space.
- Root-signature compatibility checks register coverage and shader visibility.
- `unknown` means compatibility could not be proved; it never means
  compatible.

DXC may remove unused resources during optimization. If a declaration is not
used by `PSMain`, it may be absent from the view entirely. Add it to the return
expression when you want it retained for this exercise.

## 4. Explore root-signature states

Temporarily remove `[RootSignature(TutorialRootSignature)]` and reopen the
view. Compilation still succeeds, but the root signature is reported as
absent and compatibility becomes unknown.

Restore the attribute, then change `CBV(b0)` in the root-signature string to
`CBV(b1)`. DXC normally rejects the shader because `FrameConstants` is not
fully bound. In that case Resource Bindings shows the compiler failure rather
than fabricating reflection from invalid output.

On Linux, HLSL-LSP can detect that a DXIL root signature is present, but full
deserialization requires the Windows D3D12 runtime. The view reports
`presentDetailsUnavailable` and leaves compatibility unknown.

## 5. Check spaces and structured-buffer stride

Change the texture declaration and corresponding root-signature range to
`space1`:

```hlsl
Texture2D<float4> AlbedoTexture : register(t0, space1);
```

```text
DescriptorTable(SRV(t0, numDescriptors=1, space=1))
```

The view now creates a separate SRV group for space 1. For structured buffers,
the displayed byte stride comes from DXC's reflected `NumSamples` field; for
ordinary textures that raw field is not a byte stride.

For every reflected field and compatibility state, see the
[Resource Bindings reference](../resource-bindings.md).
