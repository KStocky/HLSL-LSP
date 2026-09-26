import assert from "node:assert/strict";
import test from "node:test";

import {
  analysisDetailsHtml,
  analysisFilterHtml,
  analysisSummaryHtml,
  analysisWebviewScript,
  filterableAttributes,
} from "../../src/webviewUi";

void test("analysis UI helpers render escaped, keyboard-native summaries and disclosures", () => {
  const summary = analysisSummaryHtml([
    {
      label: "Compilation <state>",
      value: "Succeeded & current",
      tone: "success",
    },
  ]);
  const details = analysisDetailsHtml(
    "stable-key",
    "Compiler <details>",
    "<p>trusted body</p>",
    { meta: "2 & retained", open: true },
  );

  assert.match(summary, /aria-label="Analysis summary"/);
  assert.match(summary, /summary-item success/);
  assert.match(summary, /Compilation &lt;state&gt;/);
  assert.match(summary, /Succeeded &amp; current/);
  assert.match(details, /^<details data-state-key="stable-key" open>/);
  assert.match(details, /<summary>/);
  assert.match(details, /Compiler &lt;details&gt;/);
  assert.match(details, /2 &amp; retained/);
});

void test("analysis filters are labelled, escaped, and expose live result counts", () => {
  const input = analysisFilterHtml(
    "resources",
    "Filter <resources>",
    'Name or "type"',
  );
  const attributes = filterableAttributes(
    "resources",
    'MainTexture <script> "quoted"',
  );

  assert.match(
    input,
    /<label for="resources">Filter &lt;resources&gt;<\/label>/,
  );
  assert.match(input, /type="search"/);
  assert.match(input, /role="status" aria-live="polite"/);
  assert.match(input, /Name or &quot;type&quot;/);
  assert.match(attributes, /data-filter-group="resources"/);
  assert.match(attributes, /maintexture &lt;script&gt; &quot;quoted&quot;/);
  assert.doesNotMatch(attributes, /<script>/);
});

void test("analysis state script restores disclosures, filters, and scroll without a message channel", () => {
  assert.match(analysisWebviewScript, /vscode\.getState\(\)/);
  assert.match(analysisWebviewScript, /vscode\.setState/);
  assert.match(analysisWebviewScript, /detailsState/);
  assert.match(analysisWebviewScript, /filters/);
  assert.match(analysisWebviewScript, /window\.scrollTo\(0, scrollY\)/);
  assert.doesNotMatch(analysisWebviewScript, /postMessage/);
});
