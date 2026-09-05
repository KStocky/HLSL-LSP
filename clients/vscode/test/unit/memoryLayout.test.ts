import assert from "node:assert/strict";
import test from "node:test";

import { memoryLayoutHtml } from "../../src/memoryLayout";

void test("memory layout HTML renders nested offsets and escapes source names", () => {
  const html = memoryLayoutHtml({
    name: "Example<script>",
    type: "Example",
    mode: "constantBuffer",
    size: 20,
    alignment: 16,
    allocationSize: 32,
    diagnostics: ["Uses <native> 16-bit types"],
    members: [
      {
        name: "nested",
        type: "Inner",
        kind: "record",
        offset: 16,
        size: 4,
        alignment: 4,
        paddingBefore: 12,
        members: [
          {
            name: "value",
            type: "float",
            kind: "scalar",
            offset: 0,
            size: 4,
            alignment: 4,
            paddingBefore: 0,
            members: [],
          },
        ],
      },
    ],
  });

  assert.match(html, /Constant-buffer packing/);
  assert.match(html, /<td>16<\/td>/);
  assert.match(html, /allocation 32 bytes/);
  assert.doesNotMatch(html, /Example<script>/);
  assert.match(html, /Example&lt;script&gt;/);
});

void test("memory layout diagram qualifies expanded aggregate members", () => {
  const html = memoryLayoutHtml({
    name: "Constants",
    type: "cbuffer",
    mode: "constantBuffer",
    size: 80,
    alignment: 16,
    allocationSize: 80,
    diagnostics: [],
    members: [
      {
        name: "values",
        type: "float[2]",
        kind: "array",
        offset: 0,
        size: 32,
        alignment: 16,
        paddingBefore: 0,
        arrayStride: 16,
        arrayDimensions: [2],
        members: [
          {
            name: "[0]",
            type: "float",
            kind: "scalar",
            offset: 0,
            size: 4,
            alignment: 4,
            paddingBefore: 0,
            arrayIndex: 0,
            members: [],
          },
          {
            name: "[1]",
            type: "float",
            kind: "scalar",
            offset: 16,
            size: 4,
            alignment: 4,
            paddingBefore: 12,
            arrayIndex: 1,
            members: [],
          },
        ],
      },
      {
        name: "transform",
        type: "float2x2",
        kind: "matrix",
        offset: 32,
        size: 32,
        alignment: 16,
        paddingBefore: 0,
        matrixStride: 16,
        rowMajor: true,
        members: [
          {
            name: "[0]",
            type: "float2",
            kind: "vector",
            offset: 0,
            size: 8,
            alignment: 4,
            paddingBefore: 0,
            arrayIndex: 0,
            members: [],
          },
          {
            name: "[1]",
            type: "float2",
            kind: "vector",
            offset: 16,
            size: 8,
            alignment: 4,
            paddingBefore: 8,
            arrayIndex: 1,
            members: [],
          },
        ],
      },
      {
        name: "items",
        type: "Item[1]",
        kind: "array",
        offset: 64,
        size: 16,
        alignment: 16,
        paddingBefore: 0,
        arrayStride: 16,
        arrayDimensions: [1],
        members: [
          {
            name: "[0]",
            type: "Item",
            kind: "record",
            offset: 0,
            size: 16,
            alignment: 16,
            paddingBefore: 0,
            arrayIndex: 0,
            members: [
              {
                name: "material",
                type: "Material",
                kind: "record",
                offset: 0,
                size: 4,
                alignment: 4,
                paddingBefore: 0,
                members: [
                  {
                    name: "colour",
                    type: "float",
                    kind: "scalar",
                    offset: 0,
                    size: 4,
                    alignment: 4,
                    paddingBefore: 0,
                    members: [],
                  },
                ],
              },
            ],
          },
        ],
      },
    ],
  });

  assert.match(html, /values\[0\]: offset 0/);
  assert.match(html, /values\[1\]: offset 16/);
  assert.match(html, /transform\.row\[0\]: offset 32/);
  assert.match(html, /transform\.row\[1\]: offset 48/);
  assert.match(html, /items\[0\]\.material\.colour: offset 64/);
});
