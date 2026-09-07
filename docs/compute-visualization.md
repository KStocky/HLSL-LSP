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
    "locationsAvailable": false,
    "locationsUnavailableReason": "DXC reflection reports the compiled barrier instruction count but does not expose reliable source locations for those instructions; locations are never guessed from source text.",
    "locations": []
  },
  "groupShared": {
    "available": false,
    "unavailableReason": "Compiler-authoritative groupshared declaration sizes are not exposed by the current DXC reflection, cursor, type, or layout interfaces; source text is never parsed or guessed.",
    "totalBytes": null,
    "declarations": []
  },
  "waveSize": {
    "known": false,
    "min": null,
    "max": null,
    "preferred": null,
    "explanation": "The current DXC shader reflection path does not expose compiler-authoritative wave-size requirements."
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
  availability of that compiler count. Source locations have their own
  `locationsAvailable` and `locationsUnavailableReason`; current DXC
  reflection/cursor APIs do not provide a safe instruction-to-source mapping,
  so the server returns no guessed locations. These are independent signals:
  `instructionCount: 0` authoritatively means the compiled shader contains no
  barrier instructions even when `locationsAvailable` is false. An empty
  `locations` array alone must never be interpreted as a zero barrier count.
- Current DXC cursor/type/reflection interfaces do not safely expose complete
  `groupshared` declarations and their layout. `groupShared.available` is
  therefore false, `totalBytes` is null, and declarations are empty. The
  schema reserves declaration names, types, optional sizes, URIs, and ranges
  for a future compiler-backed implementation.
- Current shader reflection does not expose authoritative wave-size
  requirements. `waveSize.known` remains false with an explanation. A
  hardware profile's `waveSize` is an occupancy assumption, not a shader
  requirement.
- Without `hardwareProfile`, `occupancy` is always null. The server never
  guesses a device.

When a hardware profile is supplied, occupancy is explicitly an estimate.
Resident groups are bounded by `maxGroupsPerComputeUnit` and
`maxThreadsPerComputeUnit / threadsPerGroup`; a group larger than
`maxThreadsPerGroup` yields zero resident groups. Resident threads and waves
are derived with checked arithmetic, using the supplied wave size. The result
always lists limiting factors and assumptions. Register pressure is not
available. Because compiler-authoritative groupshared usage is currently
unavailable, `sharedMemoryBytesPerComputeUnit` is reported as an unapplied
assumption and does not fabricate a shared-memory occupancy limit.
