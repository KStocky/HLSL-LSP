# HLSL memory layout

HLSL-LSP provides memory-layout information for a deliberately supported,
unambiguous subset of HLSL. Hover shows the selected declaration's byte size,
alignment, and constant-buffer offset, and includes a `Memory Layout` command
link. Editors can request the complete tree with the custom protocol below.

The constant-buffer implementation follows the packing model documented by
[Maraneshi's HLSL Constant Buffer Layout
Visualizer](https://maraneshi.github.io/HLSL-ConstantBufferLayoutVisualizer/).
That reference is also the basis for the regression examples.

## Protocol

Request:

```text
hlsl/memoryLayout
```

Parameters use the standard text-document position shape:

```json
{
  "textDocument": { "uri": "file:///C:/shaders/example.hlsl" },
  "position": { "line": 4, "character": 12 }
}
```

The position is UTF-16, as required by this server. The document must be open.
The result is `null` when the position is not within a supported layout
declaration. Otherwise the result has this shape:

```json
{
  "name": "Constants",
  "type": "cbuffer",
  "mode": "constantBuffer",
  "size": 48,
  "allocationSize": 48,
  "alignment": 16,
  "diagnostics": [],
  "members": [
    {
      "name": "colour",
      "type": "float3",
      "kind": "vector",
      "offset": 0,
      "size": 12,
      "alignment": 4,
      "paddingBefore": 0,
      "members": []
    }
  ]
}
```

`mode` is `natural` for structure layouts and `constantBuffer` for cbuffers.
Every recursive member contains `name`, `type`, `offset`, `size`, `alignment`,
`paddingBefore`, and `members`. Expanded array elements may additionally
contain a nonnegative `arrayIndex`. A recognized but unsupported declaration
returns its explanation in `diagnostics`; offsets are never guessed.

The hover command URI invokes `hlsl.showMemoryLayout` with one argument having
the same `textDocument` and `position` members as the request.

## Layout rules

Natural layouts model StructuredBuffer-like storage:

- `bool`, 32-bit integer, and `float` scalars are 4 bytes; `double` and 64-bit
  integers are 8 bytes.
- Native `half`, `float16_t`, `int16_t`, and `uint16_t` are 2 bytes when
  `-enable-16bit-types` is active. Without it, `half` is 4 bytes and explicit
  16-bit types are rejected. Minimum-precision types occupy 4 bytes.
- Vectors have scalar alignment, including three-component vectors.
- Structures use their maximum member alignment and include natural tail
  padding.
- Arrays are tight, with each element advanced by the naturally aligned
  element stride. Multidimensional dimensions are retained in source order.
- Matrices are arrays of vectors selected by `row_major` or `column_major`.
  Unqualified matrices use the effective `-Zpr`/`-Zpc` compiler default and
  position-sensitive `#pragma pack_matrix(...)` directives.

Constant-buffer layouts additionally apply these rules:

- Scalars and vectors cannot cross a 16-byte row.
- Every array element begins on a new 16-byte row.
- A matrix is an array of row or column vectors, each beginning on a new row.
  A matrix containing only one vector is packed as that vector.
- Nested structures begin on a 16-byte row. Their reported size ends at the
  final member rather than adding artificial tail padding. A following
  enclosing member still begins on the next 16-byte row.
- `size` ends at the final cbuffer member; `allocationSize` includes padding
  through the final 16-byte row.

## Supported syntax and limitations

The parser accepts named `struct` and `cbuffer` declarations, scalar/vector/
matrix fields, previously declared named records, multiple declarators,
fixed-size multidimensional arrays, `row_major`/`column_major`, `-Zpr`/`-Zpc`,
and `#pragma pack_matrix`.

Arrays and matrices expand into indexed recursive members with `arrayIndex`
and offsets relative to their parent, allowing clients to display every
element and vector.

Resource and object types, recursive records, anonymous or inline records,
templates and aliases, non-literal or unsized arrays, initializers, methods,
bitfields, and explicit `packoffset` controls are reported as unsupported.
Records and fields overlapping conditional preprocessing are rejected rather
than combining declarations from multiple branches. A conditional
`#pragma pack_matrix` also rejects later unqualified matrix declarations until
an unconditional matrix-packing pragma establishes an unambiguous default.
The layout parser deliberately does not evaluate macros.

Requests use the same versioned analysis queue as hover and other interactive
features. Cancellation returns the standard request-cancelled error, and an
edit that supersedes the analyzed source returns `ContentModified`.
