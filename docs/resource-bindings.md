# HLSL resource bindings

HLSL-LSP can report a grouped view of a shader's resource bindings, the
embedded root signature (if any), and whether the two are compatible,
through the combined **HLSL Resource Bindings** view/command. This view is
deliberately separate from the
[Shader Compilation](compilation-info.md) view: both request the same
`hlsl/compilationInfo` protocol and analyze the same current (possibly
unsaved) document, but Resource Bindings focuses on a single, denser concern
(bindings, collisions, root-signature state, and compatibility) that would
make the Shader Compilation view unreadable if merged into it.

In Visual Studio, run **Tools > HLSL Resource Bindings**. In Visual Studio
Code, run **HLSL: Show Resource Bindings** from the Command Palette. Both
commands operate on the active open HLSL document; there is no separate
variant argument, matching Shader Compilation.

## Compiler authority and optimization-dependent resource lists

Every field in this view is read back from what DXC itself compiled and
reflected for the document's current in-memory snapshot, active variant, and
effective compiler arguments; nothing here is inferred by parsing HLSL
source. In particular, resources DXC's optimizer determines are unused by
the compiled entry point may be **omitted entirely** from `reflection`
(pinned DXC 1.9.2607.13 was empirically observed to drop declared-but-
unreferenced resources rather than emit them with a "used" flag set to
false). This means the resource list — and everything derived from it, such
as grouping, collisions, and compatibility — reflects what the shader
actually binds *after* optimization, not the full set of resources declared
in source. Compiling with different optimization levels or defines can
therefore change what appears here even for the same source file.

## Protocol

This view reuses `hlsl/compilationInfo` (see
[compilation-info.md](compilation-info.md) for the request shape and
unsaved-edit/active-variant refresh semantics) and reads three additional
parts of the response: `reflection.bindingAnalysis`, the enriched fields on
each `reflection.resources[]` entry, and the top-level `rootSignature` and
`compatibility` fields.

### Enriched resource fields

Each entry in `reflection.resources[]` includes, in addition to the fields
documented in compilation-info.md:

```json
{
  "name": "AlbedoTexture",
  "type": "texture",
  "bindPoint": 0,
  "bindCount": 1,
  "space": 1,
  "dimension": "texture2d",
  "returnType": "float",
  "registerClass": "srv",
  "rawFlags": 0,
  "rangeId": 0,
  "sampleCount": 4294967295,
  "unbounded": false,
  "systemReservedSpace": false,
  "usage": "used",
  "sourceLocation": {
    "uri": "file:///c:/project/shader.hlsl",
    "range": {
      "start": { "line": 3, "character": 19 },
      "end": { "line": 3, "character": 32 }
    }
  }
}
```

- `registerClass` is `"cbv"`, `"srv"`, `"uav"`, `"sampler"`, or `"unknown"` —
  the register class the resource binds through. Several distinct
  compiler-reported input types map to the same class (for example, both
  textures and structured buffers bind as SRVs through `t` registers, and
  `tbuffer` — though a constant-buffer-like declaration in HLSL — was
  empirically confirmed to bind as an SRV through a `t` register, not a `b`
  register).
- `rawFlags` and `rangeId` are the compiler's raw `D3D_SHADER_INPUT_FLAGS`
  bitmask and range identifier, exposed unchanged for clients that need bits
  this server does not itself interpret.
- `sampleCount` is the compiler's raw `NumSamples` value, passed through
  unchanged. For `type` values `"structured_buffer"`, `"uav_rwstructured"`,
  `"uav_rwstructured_with_counter"`, `"uav_append_structured"`, and
  `"uav_consume_structured"` (every structured-buffer-family `type`: DXC's
  `resource_type_name` maps `D3D_SIT_STRUCTURED` to `"structured_buffer"`,
  not `"structured"`) the compiler reuses this field to store the
  structured-buffer **byte stride**, not a sample count — an empirically
  confirmed DXC/D3D reflection ABI quirk. For ordinary non-multisampled
  textures it reads `4294967295` (`0xFFFFFFFF`, "not applicable").
- `usage` is `"used"`, `"unused"`, or `"unknown"`, derived strictly from the
  raw `D3D_SIF_UNUSED` reflection flag. Because pinned DXC omits
  declared-but-unreferenced resources from reflection entirely (see above),
  `"unused"` is not expected to occur in practice for the shader profiles
  this server reflects; `"unknown"` covers any case where the flag's meaning
  cannot be established, rather than guessing.
- `sourceLocation` is `{ uri, range: { start, end } }` (0-based UTF-16 LSP
  positions) or absent/`null`, populated only when the reflected resource
  name matches exactly one declaration in DXC's own cursor/declaration
  index for the current unsaved document snapshot; see
  [Source navigation](#source-navigation) below.

### Unbounded resource arrays

`unbounded` is `true` when the compiler reports `bindCount == 0`, the
sentinel `ID3D12ShaderReflection` uses for an unbounded shader-side resource
array (for example `Texture2D T[] : register(t0, space1)`). **This is a
different convention** from the one root-signature descriptor ranges use for
their own unbounded ranges (see below) — do not conflate the two when
reading raw JSON.

### System-reserved register spaces

`systemReservedSpace` is `true` when a resource's `space` falls within
`[0xfffffff0, 0xffffffff]`, the D3D12-reserved system space range used
internally by the runtime/driver and never user-addressable. Groups and
resources in this range are still reported (so nothing is silently hidden),
but they are **excluded from collision detection**, since they are not
under application control.

### Grouping and collisions: `reflection.bindingAnalysis`

```json
{
  "groups": [
    {
      "registerClass": "srv",
      "space": 1,
      "systemReservedSpace": false,
      "ranges": [
        { "resourceName": "AlbedoTexture", "baseRegister": 0, "unbounded": false, "endRegister": 0 }
      ]
    }
  ],
  "collisions": [
    {
      "firstResource": "A",
      "secondResource": "B",
      "registerClass": "srv",
      "space": 0,
      "message": "'A' (registers 0-1) and 'B' (registers 1-2) both occupy space 0 in the same register class"
    }
  ]
}
```

`groups` is one entry per distinct `(registerClass, space)` pair actually
occupied by reflected resources, each carrying the register ranges its
member resources occupy. **The server does not sort `groups`** — they are
reported in first-encountered order. Both clients sort them for display, by
register space ascending, then by register class in the conventional
CBV, SRV, UAV, Sampler order, so resource layouts read the way engineers
usually reason about them. A range's `endRegister` is `null` when
`unbounded` is `true` (it would otherwise have to overflow to represent
"every register from here on").

`collisions` reports a provable overlap between two *distinct* reflected
resources that share the same register class and space. A collision is only
reported when it can be established purely from the reflected register
ranges; duplicate/self comparisons are never reported, and — as noted above
— system-reserved space is excluded entirely.

### Embedded root signature: `rootSignature`

```json
{
  "availability": "present",
  "unavailableReason": "",
  "details": {
    "version": "1.1",
    "rawFlags": 1,
    "cbvSrvUavHeapDirectlyIndexed": false,
    "samplerHeapDirectlyIndexed": false,
    "parameters": [
      {
        "kind": "descriptorTable",
        "visibility": "pixel",
        "descriptorTableRanges": [
          {
            "type": "srv",
            "numDescriptors": 4,
            "unbounded": false,
            "baseRegister": 0,
            "space": 0,
            "rawFlags": 0,
            "offsetInDescriptorsFromTableStart": 0
          }
        ],
        "constants": null,
        "rootDescriptor": null
      }
    ],
    "staticSamplers": []
  }
}
```

`rootSignature` is `null` only when compilation did not produce DXIL/SPIR-V
output at all (for example, a failed compile). Otherwise `availability` is
always one of four distinct states, and clients present each state
distinctly rather than collapsing them:

- **`"absent"`** — presence/absence detection ran and found no embedded
  root signature (no `[RootSignature(...)]` attribute or matching compiler
  argument). Detection works on every platform, since it only inspects the
  DXIL container's `RTS0` part via `IDxcUtils::GetDxilContainerPart`.
- **`"notApplicable"`** — the output is SPIR-V, which has no root-signature
  concept (root signatures are a Direct3D 12 binding-model construct).
  `unavailableReason` explains this in prose.
- **`"presentDetailsUnavailable"`** — a root signature **is** embedded, but
  this platform could not deserialize its contents. `unavailableReason`
  explains why. On non-Windows platforms (Linux), the reason reads
  approximately: *"An embedded root signature is present, but detailed
  inspection requires the Windows D3D12 runtime's
  `ID3D12VersionedRootSignatureDeserializer`, which is unavailable on this
  platform; only presence could be determined."* This server never ships a
  custom RTS0 binary parser as a substitute — detailed deserialization is
  Windows-only by design, using only the official
  `D3D12CreateVersionedRootSignatureDeserializer` API.
- **`"present"`** — a root signature is embedded and its details were
  successfully deserialized; `details` is populated (see below). This is
  the only state where per-parameter details are shown.

**A present-details-unavailable root signature must never be presented or
treated as if it were compatible.** The client-side compatibility section
(below) enforces this explicitly.

#### Root signature details

When `availability` is `"present"`, `details` reports, always through the
version-1.1 shape (flags default to `NONE` for a signature actually
authored/serialized as version 1.0, while `version` itself always reports
the signature's true, unconverted version so clients are never misled about
what was actually embedded):

- `version`, `rawFlags` (raw `D3D12_ROOT_SIGNATURE_FLAGS`),
  `cbvSrvUavHeapDirectlyIndexed`, `samplerHeapDirectlyIndexed` — whether the
  signature declares that shaders may directly index into the
  CBV/SRV/UAV and/or sampler descriptor heaps (bindless access), mirroring
  the corresponding `D3D12_ROOT_SIGNATURE_FLAGS` bits.
- `parameters[]` — one entry per root parameter
  (`D3D12_ROOT_PARAMETER1`), each with a `kind`
  (`"descriptorTable"`/`"constants"`/`"rootDescriptor"`) and a `visibility`
  (`"all"`, `"vertex"`, `"hull"`, `"domain"`, `"geometry"`, `"pixel"`,
  `"amplification"`, `"mesh"`, or `"unknown"`, mirroring
  `D3D12_SHADER_VISIBILITY`). Exactly one of `descriptorTableRanges`
  (non-empty), `constants`, or `rootDescriptor` is populated, matching
  `kind`:
  - **Descriptor tables** (`descriptorTableRanges[]`): each range has a
    `type` (`"srv"`/`"uav"`/`"cbv"`/`"sampler"`/`"unknown"`),
    `baseRegister`, `space`, `rawFlags` (`D3D12_DESCRIPTOR_RANGE_FLAGS`,
    version 1.1), `offsetInDescriptorsFromTableStart`, and `numDescriptors`.
    `numDescriptors` is `null` when `unbounded` is `true` — root-signature
    descriptor ranges use **`NumDescriptors == UINT_MAX`** as their
    unbounded sentinel, a *different* convention from the shader-side
    resource `bindCount == 0` sentinel described above; do not conflate the
    two.
  - **Root constants** (`constants`): `shaderRegister`, `space`,
    `num32BitValues`.
  - **Root descriptors** (`rootDescriptor`): a root CBV/SRV/UAV bound
    directly (not through a table), with `type`, `shaderRegister`, `space`,
    and `rawFlags` (`D3D12_ROOT_DESCRIPTOR_FLAGS`, version 1.1).
- `staticSamplers[]` — samplers declared directly in the root signature
  (`D3D12_STATIC_SAMPLER_DESC`), never requiring a descriptor-heap slot:
  `shaderRegister`, `space`, `visibility`, raw `filter`/`addressU`/
  `addressV`/`addressW`/`comparisonFunc`/`borderColor` values, `mipLodBias`,
  `maxAnisotropy`, `minLod`, `maxLod`.

### Compatibility: `compatibility`

```json
{
  "status": "incompatible",
  "explanation": "",
  "issues": [
    {
      "resourceName": "AlbedoTexture",
      "registerClass": "srv",
      "space": 0,
      "message": "'AlbedoTexture' has no corresponding root signature entry"
    }
  ]
}
```

`compatibility` reports whether the reflected shader resources are provably
covered by the deserialized root signature for the compiled entry point's
stage, comparing only register class/space/range coverage, active-stage
shader visibility, and static samplers. `status` is one of:

- **`"compatible"`** — every reflected, user-addressable resource is
  covered by the root signature for the active stage.
- **`"incompatible"`** — `issues[]` lists at least one specific, provable
  mismatch (for example, a resource with no corresponding root-signature
  entry, or one visible only to a different stage).
- **`"unknown"`** — compatibility could not be determined; `explanation`
  says why (for example, no embedded root signature, or the root signature
  is present but its details are unavailable on this platform). **`"unknown"`
  must never be presented or interpreted as compatible** — both clients
  render it as a distinct, visually-flagged state rather than silently
  defaulting to "compatible" or omitting the section.

`compatibility` is populated once compatibility analysis actually runs:
unconditionally for SPIR-V output (where it always reports `"unknown"`,
since root-signature compatibility does not apply), and for DXIL output
regardless of whether reflection metadata itself is available. When DXIL
reflection is unavailable (for example, no root signature is present, or
the reflected resource list needed to compare against a present root
signature could not be obtained), `status` reads `"unknown"` with an
`explanation` rather than the field being `null` — reporting `null`
compatibility while a root signature is present would risk a client
silently treating an unanalyzed result as compatible. `compatibility` is
**non-null whenever `rootSignature` is non-null**; both fields are `null`
only when compilation produced no output at all (for example, a failed
compilation).

#### Bindless limitation

**True bindless accesses through `ResourceDescriptorHeap` /
`SamplerDescriptorHeap` are invisible to the compiler's reflection API and
can never be enumerated or checked here.** DXC's shader reflection only
describes resources declared as explicit bound-resource declarations (for
example, `Texture2D T : register(t0)`); it has no way to enumerate
individual heap indices a shader may compute and access dynamically at
runtime. Both clients render a persistent note to this effect alongside
every compatibility verdict, so a `"compatible"` result is never
misread as "this shader has no bindless accesses" or "every possible
resource access was checked."

## Source navigation

**Compiler-located resource bindings are clickable.** Each reflected
resource in `reflection.resources[]` carries an optional `sourceLocation` —
`{ uri, range: { start, end } }` in standard LSP coordinates — populated
only when DXC's own cursor/declaration index (the same index used for
hover/go-to-definition/document symbols) matches the resource's reflected
name to **exactly one** declaration in the current unsaved document
snapshot. It is left absent when the name cannot be found at all, or when
it matches more than one declaration (a genuine ambiguity, e.g. a
same-named field elsewhere) — a location is only ever reported when it is
compiler-unambiguous. `ResourceBindingCollision` entries carry no location
of their own (only `firstResource`/`secondResource` name strings), so both
clients resolve each collision participant by looking it up in the exact
`(registerClass, space, name)`-keyed resource entries of the same response
— never by searching source text for the name — and leave a participant
non-clickable if that lookup does not resolve to exactly one resource
entry.

- **Visual Studio Code** renders a resource's Name cell (and each
  clickable collision participant) as a plain `command:` URI link, invoked
  through a webview `enableCommandUris` allowlist of exactly one command
  (`hlsl.resourceBindings.openLocation`); `enableScripts` stays `false`
  throughout, so no script execution is used for navigation at all. The
  command handler defensively re-validates the decoded argument's
  `uri`/`range` shape (non-negative integer positions, non-inverted range)
  before calling `vscode.Uri.parse`, `openTextDocument`, and
  `showTextDocument` with a selection, and shows an explicit error message
  on any failure rather than failing silently.
- **Visual Studio** renders a resource's Name cell (and each clickable
  collision participant) as a WPF `Hyperlink`. An explicit click validates
  the location (absolute local-file URI, file exists, well-formed
  non-inverted range) before opening the document via
  `VsShellUtilities.OpenDocument` and selecting/revealing the range through
  `IVsTextView`; validation failures surface an explicit message box.
  Background save/variant refresh never calls this navigation path and
  never steals focus.

Root-signature entries (parameters, ranges, static samplers) and
`ResourceCompatibilityIssue` entries carry no declaration-location
information in the current protocol, so they are never clickable in either
client — this is not a bug, simply the absence of a location to navigate
to.

## Refresh behavior

Resource Bindings reuses exactly the same refresh triggers as Shader
Compilation (see [compilation-info.md](compilation-info.md#unsaved-edits-and-active-variant-refresh)),
against an independently tracked panel/tool window:

- **Visual Studio Code** refreshes the open webview when the active variant
  changes, when the document is saved, and (debounced) shortly after the
  document is edited — all without stealing focus from the editor.
- **Visual Studio** refreshes the open tool window when the active variant
  changes and when the shown document is saved, without activating the
  window or stealing focus. As with Shader Compilation, there is no
  keystroke-level refresh; save the document or run the command again to see
  an in-progress edit reflected sooner.

Both refresh paths use their own generation counters, entirely independent
from the Shader Compilation view's, so requests from the two views can never
interfere with or supersede each other.
