# Compute visualization protocol

`hlsl/computeVisualization` reports compiler-authoritative compute geometry
for an open HLSL document's effective configured entry point and active
variant. The client cannot select a different entry point, target, variant, or
source snapshot through this request.

## Request

```json
{
  "textDocument": { "uri": "file:///C:/shaders/compute.hlsl" },
  "dispatchDimensions": { "x": 1920, "y": 1080, "z": 1 },
  "hardwareProfile": {
    "name": "Example GPU",
    "waveSize": 32,
    "maxThreadsPerGroup": 1024,
    "maxThreadsPerComputeUnit": 2048,
    "maxGroupsPerComputeUnit": 32,
    "sharedMemoryBytesPerComputeUnit": 65536
  }
}
```

`dispatchDimensions` is an optional logical workload measured in threads, not
an already-rounded `Dispatch()` group count. When omitted, it defaults to one
reflected thread group (`numthreads`). `hardwareProfile` is optional and is
used only for the explicitly labelled occupancy estimate.

Every supplied dimension and numeric hardware limit must be a positive
integral JSON number no greater than `4294967295`. `hardwareProfile.name` must
be a non-empty string. Products are checked with 64-bit arithmetic and must
remain within JavaScript's exact integer range (`9007199254740991`), because
both editor clients consume JSON numbers. Malformed, non-positive,
out-of-range, or overflowing values produce JSON-RPC `InvalidParams`
(`-32602`).

Every aggregate serialized as a JSON number is subject to the same
`2^53 - 1` bound: `launchedThreads`, `inactiveThreads`,
`occupancy.estimatedResidentGroups`, `estimatedResidentThreads`,
`estimatedResidentWaves`, and any future non-null group-shared declaration or
total byte count. The server rejects request inputs when rounding to whole
thread groups would make a derived aggregate exceed that bound, even if the
unrounded logical-workload product itself is still safe. Numeric aggregates
are never silently rounded for IEEE-754 clients and are not encoded as
strings.

The document must be open. The request uses its current in-memory version,
effective `shadertoolsconfig.json`, and active variant. A concurrent edit,
include/configuration reanalysis, or variant change cancels or rejects a
superseded result rather than returning mixed-generation data.

## Response

```json
{
  "applicable": true,
  "found": true,
  "explanation": "Computed from the effective configured entry point and DXC reflection.",
  "entryPoint": "CSMain",
  "stage": "compute",
  "targetProfile": "cs_6_6",
  "threadGroupSize": { "x": 8, "y": 4, "z": 1 },
  "dispatchDimensions": { "x": 1920, "y": 1080, "z": 1 },
  "groupCount": { "x": 240, "y": 270, "z": 1 },
  "launchedThreads": 2073600,
  "inactiveThreads": 0,
  "systemValues": [
    {
      "semantic": "SV_DispatchThreadID",
      "name": "dispatchThreadId",
      "formula": "groupId * numthreads + groupThreadId",
      "description": "Global dispatch-space thread coordinate."
    }
  ],
  "barriers": {
    "available": true,
    "unavailableReason": "",
    "instructionCount": 1,
    "locationsAvailable": true,
    "locationsTruncated": false,
    "locationsUnavailableReason": "",
    "locations": [
      {
        "label": "GroupMemoryBarrierWithGroupSync",
        "uri": "file:///C:/shaders/compute.hlsl",
        "range": {
          "start": { "line": 12, "character": 4 },
          "end": { "line": 12, "character": 37 }
        }
      }
    ]
  },
  "groupShared": {
    "available": true,
    "unavailableReason": "",
    "totalBytes": 128,
    "totalBytesUnavailableReason": "",
    "truncated": false,
    "declarations": [
      {
        "name": "Tile",
        "type": "uint [32]",
        "declaration": "groupshared uint Tile[32]",
        "bytes": 128,
        "sizeUnavailableReason": "",
        "uri": "file:///C:/shaders/compute.hlsl",
        "range": {
          "start": { "line": 1, "character": 0 },
          "end": { "line": 1, "character": 25 }
        }
      }
    ]
  },
  "waveSize": {
    "known": true,
    "min": 32,
    "max": 64,
    "preferred": 64,
    "minMaxSource": "psv0",
    "preferredSource": "compilerFormattedEntryCursor",
    "explanation": "Minimum and maximum come from stable PSV0 runtime metadata; preferred comes from DXC's bounded compiler-formatted entry-point declaration."
  },
  "occupancy": null
}
```

`found` means an effective entry point is configured. `applicable` additionally
requires a compute target, successful DXC compilation, available DXIL shader
reflection, and a non-zero reflected thread-group size. When it is false,
`explanation` states the reason and geometry fields are `null`; identity fields
still describe the effective configuration when available.

`threadGroupSize` comes only from
`ID3D12ShaderReflection::GetThreadGroupSize`. The remaining geometry is
deterministic checked arithmetic:

- `groupCount.axis = ceil(dispatchDimensions.axis / threadGroupSize.axis)`
- `launchedThreads = product(groupCount) * product(threadGroupSize)`
- `inactiveThreads = launchedThreads - product(dispatchDimensions)`

The four system-value entries are stable protocol metadata describing HLSL's
defined mapping for `SV_DispatchThreadID`, `SV_GroupID`,
`SV_GroupThreadID`, and `SV_GroupIndex`; they are not discovered by parsing
source parameters.

## Compiler authority and availability

- `barriers.instructionCount` is
  `D3D12_SHADER_DESC::cBarrierInstructions`. `barriers.available` describes
  availability of that compiler count. Locations are independently collected
  from call-expression extents in functions reachable from the configured
  entry point. A call is accepted only when DXC resolves its callee to one of
  the six HLSL barrier intrinsics and that callee has no user source location;
  a user function reusing an intrinsic spelling is not classified as a
  barrier. `locationsTruncated` reports the dedicated 256-location bound.
  The compiled instruction count and cursor locations remain independent:
  optimization may make them differ, and an empty location array is not a
  substitute for `instructionCount`.
- `groupShared.declarations` enumerates global `VarDecl` cursors whose
  DXC-formatted declaration begins with normalized `groupshared `. Names,
  compiler type spellings, declarations, URIs, and source extents all come
  from DXC cursors. Each byte count is obtained by converting that
  compiler-formatted declaration into a member of a collision-resistant
  synthetic wrapper struct and compiling the existing StructuredBuffer
  reflection probe. No raw HLSL source is parsed. An unsupported or ambiguous
  compiler formatting leaves that declaration's `bytes` null and explains
  why in `sizeUnavailableReason`; `totalBytes` is null unless every retained
  declaration has an exact size, with `totalBytesUnavailableReason` explaining
  why. `truncated` reports the 256-declaration bound.
- `waveSize.min` and `waveSize.max` come from the public, stable
  `PSVRuntimeInfo0::MinimumExpectedWaveLaneCount` and
  `MaximumExpectedWaveLaneCount` fields in the compiled DXIL container's
  PSV0 part. `minMaxSource` is therefore `psv0` when known. PSV0 does not
  contain the preferred value. For that field only, the configured entry
  point cursor's compiler-formatted declaration is inspected; pinned DXC
  normalizes the attribute to `[wavesize(...)]`, and a bounded exact
  three-argument form supplies `preferred`. `preferredSource` is then
  `compilerFormattedEntryCursor`. The cursor's min/max must agree with PSV0
  before its preferred value is accepted. Missing/malformed PSV0 produces
  `known: false`; absent preferred metadata leaves only `preferred` null.
  Raw source and LLVM IR/disassembly are never parsed. A hardware profile's
  `waveSize` remains an occupancy assumption, not the shader requirement.
- Without `hardwareProfile`, `occupancy` is always null. The server never
  guesses a device.

When a hardware profile is supplied, occupancy is explicitly an estimate.
Resident groups are bounded by `maxGroupsPerComputeUnit`,
`maxThreadsPerComputeUnit / allocatedWaveLanesPerGroup`, where
`allocatedWaveLanesPerGroup = ceil(threadsPerGroup / waveSize) * waveSize`;
and, when `groupShared.totalBytes` is known and non-zero,
`sharedMemoryBytesPerComputeUnit / groupShared.totalBytes`. Partial waves
cannot be shared between groups. A group larger than
`maxThreadsPerGroup` yields zero resident groups. Resident active threads and
allocated waves are derived with checked arithmetic. The result always lists
limiting factors and assumptions. Register pressure is not available. Because
the shared-memory limit is applied only when every declaration size is known,
an incomplete or unsupported declaration set is reported as an unapplied
assumption rather than fabricating a limit.
