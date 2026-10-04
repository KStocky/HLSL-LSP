# FXC / D3DCompiler backend

DXC remains the default. On Windows, HLSL-LSP can use native D3DCompiler APIs
for projects whose production shaders target legacy Shader Model 4/5.
The backend is available in both Visual Studio and Visual Studio Code.

## Configuration

Add the existing profile and entry-point settings alongside the compiler choice:

```json
{
  "root": true,
  "hlsl.compilerBackend": "fxc",
  "hlsl.targetProfile": "ps_5_0",
  "hlsl.entryPoint": "PSMain",
  "hlsl.additionalArguments": ["/O3", "/Ges"]
}
```

```hlsl
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target
{
    return float4(uv, 0.0, 1.0);
}
```

The default runtime is `D3DCompiler_47.dll` loaded from the Windows system
directory, not the working directory or `PATH`. To use a production-matched
runtime, set `"hlsl.fxcRuntimePath": "Tools/D3DCompiler_47.dll"`. This is a DLL
file path, not an `fxc.exe` path or directory. Relative paths resolve beside
the configuration file. Missing files, missing required exports, unsupported
profiles, and unsupported arguments are explicit errors.

These settings also work in `hlsl.fileGroups` and `hlsl.variants`. For example,
a legacy variant may select FXC and `ps_5_0`, while another variant selects DXC
and `ps_6_6`. Switching invalidates cached analysis, including unsaved buffers.
The editor settings override project configuration only when explicitly set.
Clearing them restores project inheritance.

FXC does not implement DXC's `-HV` switch. `hlsl.languageVersion` does not
select HLSL 2021 for FXC; compilation reports `FXC legacy HLSL`. SM6 and library
profiles are rejected rather than compiled by DXC. Use the existing definition,
include-directory, and entry-point settings instead of placing command-line
`/D`, `/I`, `/E`, or `/T` switches in `hlsl.additionalArguments`.

Accepted argument names are `WX`, `Zi`, `Od`, `O0` through `O3`, `Zpr`, `Zpc`,
`Ges`, `Gec`, `Gis`, `Gfa`, `Gfp`, `Vd`,
`enable_unbounded_descriptor_tables`, `res_may_alias`, and
`all_resources_bound`, each with a `/` or `-` prefix and the exact casing shown.
Conflicting optimization, matrix, flow-control, and strictness/compatibility
settings are rejected. `res_may_alias` requires a supported compute profile.
Other switches are rejected, not discarded.

Supported profile spellings use `vs`, `ps`, `gs`, `hs`, `ds`, or `cs` with
`4_0`, `4_1`, `5_0`, or `5_1`, plus `vs`/`ps` `4_0_level_9_1` and
`4_0_level_9_3`. D3DCompiler validates the actual stage/profile combination.
The default is `ps_5_0`. SM2/3 legacy DX9 bytecode is deliberately unsupported;
this backend's compilation and inspection contract is DXBC.

## Authority and capability limits

| Surface | FXC behavior |
|---|---|
| Errors and warnings | Native `D3DCompile` diagnostics, including successful-compilation warnings; published with source `fxc` |
| Compilation and disassembly | Native DXBC compilation and `D3DDisassemble`; actual selected DLL identified in the result |
| Unsaved includes | Snapshot-backed `ID3DInclude`, including nested parent-relative resolution; no unrelated disk fallback |
| Macro expansion | Native `D3DPreprocess` applied to the existing compiler-owned invocation probe |
| Signatures, resources, thread-group size, statistics | Native reflection only; SM5.1 register spaces require native D3D12 reflection |
| Interpolation and SM5.0 resource range IDs | Unavailable, not inferred from HLSL or fabricated as zero |
| Cbuffer layout | Partial native reflected offsets and sizes; alignment and complete array/matrix strides remain unavailable |
| Natural structure layout | Unavailable; no custom packing engine or DXC layout substitution |
| Embedded root signature | Native blob-part query reports presence; detailed decoding and compatibility remain unknown |
| Source macro enumeration and inactive-region discovery | Unavailable; configured definitions and include-resolver metadata remain visible |
| Entry-point data flow, compute source probes, wave metadata | Unavailable; DXC-only analyses are not presented as FXC results |
| Source-symbol editor features | DXC HLSL 2016 source index for shared legacy declarations; builtin completion, hover, and signatures are unavailable rather than presumed FXC-compatible |

Source-symbol completion, hover, signatures, and definition lookup are covered
by shared-legacy tests against both compilation paths. This source index is not
an FXC AST API: compilation, warnings, preprocessing, and reflected metadata
always come from FXC. The DXC runtime is still required to provide the source
index. No additional HLSL parser or lexer is maintained for this backend.

Pipeline validation continues checking native signatures and resource
bindings. It reports interpolation matching as unavailable rather than
claiming a complete cross-stage check. Compiler-derived layout hints are
suppressed when complete layout information is unavailable.

Guided configuration authoring uses native SM5.0 probes for a configured FXC
document. Save the backend choice before rediscovery. Runtime capture still
captures DXC production calls; it is not an FXC capture hook.
Captured file groups and variants explicitly select DXC, so they do not
accidentally inherit an FXC project default.

FXC is not available on Linux. Selecting it reports the unsupported platform;
there is no silent DXC fallback. Return to `"hlsl.compilerBackend": "dxc"` for
cross-platform compilation.
