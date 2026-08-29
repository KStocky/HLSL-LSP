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
        offset: 16,
        size: 4,
        alignment: 4,
        paddingBefore: 12,
        members: [
          {
            name: "value",
            type: "float",
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
