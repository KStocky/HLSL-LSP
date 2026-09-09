# Tutorial: inspect shader compilation

This tutorial compiles a compute shader and uses Shader Compilation to inspect
the effective command line, diagnostics, output, reflection, and
compiler-generated disassembly.

The finished workspace contains:

```text
workspace/
├── shadertoolsconfig.json
└── Shaders/
    └── CompilationTutorial.hlsl
```

## 1. Create the shader

Create `Shaders/CompilationTutorial.hlsl`:

```hlsl
cbuffer DispatchConstants : register(b0)
{
    uint ElementCount;
};

RWStructuredBuffer<float> Output : register(u0);

[numthreads(8, 4, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.y * 8 + dispatchThreadId.x;
    if (index < ElementCount)
    {
        Output[index] = float(index);
    }
}
```

Create `shadertoolsconfig.json` in the workspace root:

```jsonc
{
  "root": true,
  "hlsl.languageVersion": "2021",
  "hlsl.targetProfile": "cs_6_6",
  "hlsl.entryPoint": "CSMain",
  "hlsl.preprocessorDefinitions": {
    "TUTORIAL_BUILD": 1
  },
  "hlsl.additionalArguments": ["-Zi"]
}
```

## 2. Compile the active document

1. Open `CompilationTutorial.hlsl`.
2. Open Shader Compilation:
   - **Visual Studio Code:** run **HLSL: Show Shader Compilation** from the
     Command Palette.
   - **Visual Studio:** right-click the shader and choose
     **HLSL > Shader Compilation**.
3. Verify the effective configuration:
   - entry point `CSMain`;
   - stage `compute`;
   - target `cs_6_6`;
   - language version `2021`;
   - define `TUTORIAL_BUILD=1`;
   - additional compiler argument `-Zi`.
4. Verify the successful result:
   - output type `dxil` and a nonzero byte size;
   - reflected CBV `DispatchConstants` and UAV `Output`;
   - thread-group size `8 x 4 x 1`;
   - DXC-generated DXIL disassembly.

Use **Copy** to place the retained disassembly on the clipboard or **Save** to
write it as an `.ll` file. HLSL-LSP retains at most 4 MiB and explicitly marks
truncated output.

## 3. Observe an unsaved compiler error

Without saving, remove the semicolon after `Output[index] = float(index)`.
Refresh behavior differs slightly by editor:

- **Visual Studio Code** refreshes an open compilation webview shortly after
  an edit.
- **Visual Studio** refreshes after save; run the context command again to inspect
  an earlier unsaved edit immediately.

The view should report compilation failure with the DXC diagnostic. Output,
reflection, and disassembly are absent because no valid object was produced.
Restore the semicolon and refresh to return to the successful result.

## 4. Confirm configuration precedence

Set `hlsl.targetProfile` to `cs_6_7` in the editor's workspace settings and
reopen the view. The reported target changes because explicit editor settings
override `shadertoolsconfig.json`. Clear the editor value to restore
`cs_6_6`.

If an active shader variant supplies an entry point, target, or defines, the
view reports the merged effective configuration. It never accepts a separate
variant argument, so the displayed data cannot disagree with the configuration
used for diagnostics and language features.

## 5. Compare DXIL and SPIR-V

Add `"-spirv"` to `hlsl.additionalArguments` and reopen the view. Successful
output is identified as SPIR-V. With the pinned DXC runtime:

- the reflection path is unavailable for SPIR-V;
- `IDxcCompiler3::Disassemble` rejects the SPIR-V binary.

The view reports both limitations explicitly. It does not parse SPIR-V or
invent substitute reflection/disassembly. Remove `-spirv` to restore the DXIL
sections used earlier.

For the response schema, refresh rules, and all availability states, see the
[Shader Compilation reference](../compilation-info.md).
