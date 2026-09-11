import { strict as assert } from "node:assert";
import { test } from "node:test";

import {
  effectiveContextHeaderHtml,
  effectiveContextTooltip,
  EffectiveShaderContext,
} from "../../src/effectiveContext";

const context: EffectiveShaderContext = {
  documentUri: "file:///workspace/shader.hlsl",
  file: "shader.hlsl",
  activeVariant: null,
  entryPoint: "",
  targetProfile: "ps_6_6",
  origins: {
    targetProfile: {
      label: "variant <Prod>",
      setting: "targetProfile",
      uri: "file:///workspace/shadertoolsconfig.json",
    },
  },
};

void test("effective context header uses shared labels and default terminology", () => {
  const html = effectiveContextHeaderHtml(context);

  assert.match(html, /<strong>File:<\/strong> shader\.hlsl/);
  assert.match(html, /<strong>Variant:<\/strong> Default/);
  assert.match(html, /<strong>Entry point:<\/strong> Not configured/);
  assert.match(html, /<strong>Target profile:<\/strong> ps_6_6/);
  assert.match(html, /Target profile — variant &lt;Prod&gt;/);
});

void test("effective context tooltip includes useful compilation context", () => {
  const tooltip = effectiveContextTooltip(context);

  assert.match(tooltip, /File: shader\.hlsl/);
  assert.match(tooltip, /Variant: Default/);
  assert.match(tooltip, /Entry point: Not configured/);
  assert.match(tooltip, /Target profile: ps_6_6/);
  assert.match(tooltip, /Click to select a shader variant/);
});
