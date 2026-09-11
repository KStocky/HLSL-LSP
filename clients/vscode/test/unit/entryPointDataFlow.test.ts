import assert from "node:assert/strict";
import test from "node:test";

import {
  CallHierarchyItem,
  EntryPointDataFlow,
  entryPointDataFlowErrorHtml,
  entryPointDataFlowHtml,
  entryPointDataFlowRequestParams,
  navigateEntryPointDataFlowLocation,
  openEntryPointDataFlowLocationCommand,
  parseEntryPointDataFlowLocationCommandArg,
  resolveEntryPointDataFlowRefresh,
  ValidatedEntryPointDataFlowLocation,
} from "../../src/entryPointDataFlow";

function callHierarchyItem(
  overrides: Partial<CallHierarchyItem> = {},
): CallHierarchyItem {
  return {
    name: "square",
    kind: 12,
    detail: "float square(float x)",
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 3, character: 0 },
      end: { line: 3, character: 40 },
    },
    selectionRange: {
      start: { line: 3, character: 6 },
      end: { line: 3, character: 12 },
    },
    data: {
      rootUri: "file:///c:/shaders/example.hlsl",
      rootIdentity: "identity",
      rootVersion: 1,
      generation: 1,
      context: {
        documentUri: "file:///c:/shaders/example.hlsl",
        file: "example.hlsl",
        activeVariant: null,
        entryPoint: "main",
        targetProfile: "ps_6_6",
        origins: {},
      },
      path: "c:/shaders/example.hlsl",
      line: 4,
      column: 7,
      startOffset: 87,
      cursorKind: 21,
      name: "square",
    },
    ...overrides,
  };
}

function baseFlow(
  overrides: Partial<EntryPointDataFlow> = {},
): EntryPointDataFlow {
  return {
    found: true,
    explanation: "",
    entryPoint: callHierarchyItem(),
    reachableFunctions: [
      { function: callHierarchyItem(), depth: 0, recursive: false },
    ],
    unreachableFunctions: [],
    unusedDeclarations: [],
    globalAccesses: [],
    truncated: false,
    functionsVisitedTruncated: false,
    definitionsTruncated: false,
    globalAccessesTruncated: false,
    unusedDeclarationsTruncated: false,
    functionsVisited: 1,
    ...overrides,
  };
}

const documentUri = "file:///c:/shaders/example.hlsl";

void test("entry point data flow HTML renders a reachable function with its own name and depth", () => {
  const html = entryPointDataFlowHtml(baseFlow(), documentUri);
  assert.match(html, /<a[^>]*>square<\/a>/);
  assert.match(html, /float square\(float x\)/);
  assert.match(html, /<td>0<\/td>/);
});

void test("entry point data flow HTML links a reachable function through the allowlisted command, using its own selectionRange", () => {
  const html = entryPointDataFlowHtml(baseFlow(), documentUri);
  const encoded = encodeURIComponent(
    JSON.stringify([
      {
        uri: "file:///c:/shaders/example.hlsl",
        range: {
          start: { line: 3, character: 6 },
          end: { line: 3, character: 12 },
        },
      },
    ]),
  );
  assert.match(
    html,
    new RegExp(
      `command:${escapeRegExp(openEntryPointDataFlowLocationCommand)}\\?${escapeRegExp(encoded)}`,
    ),
  );
});

void test("entry point data flow HTML marks a recursive reachable function with a recursion badge", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      reachableFunctions: [
        { function: callHierarchyItem(), depth: 2, recursive: true },
      ],
    }),
    documentUri,
  );
  assert.match(html, /class="badge recursive"/);
});

void test("entry point data flow HTML renders global accesses with an access badge and qualified name", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      globalAccesses: [
        {
          name: "value",
          qualifiedName: "PerFrame.value",
          kind: 8,
          uri: documentUri,
          range: {
            start: { line: 1, character: 0 },
            end: { line: 1, character: 34 },
          },
          selectionRange: {
            start: { line: 1, character: 20 },
            end: { line: 1, character: 32 },
          },
          access: "readWrite",
        },
      ],
    }),
    documentUri,
  );
  assert.match(html, /<a[^>]*>PerFrame\.value<\/a>/);
  assert.match(html, /class="badge access-readWrite"/);
  assert.match(html, /Read\/Write/);
});

void test("entry point data flow HTML omits the qualified name label when it equals the plain name", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      globalAccesses: [
        {
          name: "InputTexture",
          qualifiedName: "InputTexture",
          kind: 268,
          uri: documentUri,
          range: {
            start: { line: 1, character: 0 },
            end: { line: 1, character: 34 },
          },
          selectionRange: {
            start: { line: 1, character: 20 },
            end: { line: 1, character: 32 },
          },
          access: "read",
        },
      ],
    }),
    documentUri,
  );
  assert.match(html, /<a[^>]*>InputTexture<\/a>/);
  assert.doesNotMatch(html, /InputTexture\.InputTexture/);
});

void test("entry point data flow HTML renders unreachable functions and unused declarations sections", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      unreachableFunctions: [
        callHierarchyItem({ name: "deadCode", detail: "void deadCode()" }),
      ],
      unusedDeclarations: [
        {
          name: "unusedHelper",
          kind: 12,
          uri: documentUri,
          range: {
            start: { line: 10, character: 0 },
            end: { line: 12, character: 1 },
          },
          selectionRange: {
            start: { line: 10, character: 6 },
            end: { line: 10, character: 17 },
          },
        },
      ],
    }),
    documentUri,
  );
  assert.match(html, /<a[^>]*>deadCode<\/a>/);
  assert.match(html, /void deadCode\(\)/);
  assert.match(html, /<a[^>]*>unusedHelper<\/a>/);
  assert.match(html, /Function<\/td>/);
});

void test("entry point data flow HTML clearly distinguishes a not-found state, showing the explanation instead of any section", () => {
  const html = entryPointDataFlowHtml(
    {
      found: false,
      explanation: "No entry point is configured for this document.",
      entryPoint: null,
      reachableFunctions: [],
      unreachableFunctions: [],
      unusedDeclarations: [],
      globalAccesses: [],
      truncated: false,
      functionsVisitedTruncated: false,
      definitionsTruncated: false,
      globalAccessesTruncated: false,
      unusedDeclarationsTruncated: false,
      functionsVisited: 0,
    },
    documentUri,
  );
  assert.match(html, /class="not-found"/);
  assert.match(html, /No entry point is configured for this document\./);
  assert.doesNotMatch(html, /Reachable functions/);
  assert.doesNotMatch(html, /Unused declarations/);
});

void test("entry point data flow HTML shows no truncation indication anywhere when all four flags are false", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({ functionsVisited: 4096 }),
    documentUri,
  );
  assert.doesNotMatch(html, /class="truncated"/);
  assert.match(html, /Functions visited: 4096/);
  assert.match(html, /\(no unreachable functions\)/);
  assert.match(html, /\(no global or resource accesses\)/);
  assert.match(html, /\(no unused declarations\)/);
});

void test("entry point data flow HTML: functionsVisitedTruncated reports unreachable functions as unknown (never as none) and caveats reachable functions/global accesses, but not unused declarations", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      functionsVisitedTruncated: true,
      functionsVisited: 4096,
    }),
    documentUri,
  );
  // Header banner: names this specific cause, not the other two.
  const headerMatch = /<h1>[\s\S]*?<p class="truncated">(.*?)<\/p>/.exec(html);
  assert.ok(headerMatch, "expected a header truncated banner");
  const header = headerMatch[1] ?? "";
  assert.match(header, /reachability traversal stopped early/);
  assert.match(header, /unreachable functions could not be determined/);
  assert.doesNotMatch(header, /access-retention|retaining further accesses/);
  assert.doesNotMatch(header, /unused-declaration scan/);
  assert.match(header, /4096/);

  // Unreachable functions: an explicit "unknown" state, never "(no
  // unreachable functions)" -- that would misreport "not determined" as
  // "provably none".
  assert.doesNotMatch(html, /\(no unreachable functions\)/);
  assert.match(html, /Unknown: the reachability traversal stopped early/);

  // Global accesses: also caveated (access scanning only covers visited
  // functions), and its empty state must not claim completeness either.
  assert.doesNotMatch(html, /\(no global or resource accesses\)/);
  assert.match(
    html,
    /none retained before the scan stopped -- this list may be incomplete/,
  );

  // Unused declarations is an independent phase: unaffected, so its
  // ordinary empty state is still accurate here.
  assert.match(html, /\(no unused declarations\)/);
});

void test("entry point data flow HTML: definitionsTruncated alone reports unreachable functions as unknown (never as none), naming definition collection specifically, without affecting reachable/global/unused sections", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      definitionsTruncated: true,
      functionsVisited: 200,
    }),
    documentUri,
  );
  // Header banner: names definition collection as the cause, not the
  // other three.
  const headerMatch = /<h1>[\s\S]*?<p class="truncated">(.*?)<\/p>/.exec(html);
  assert.ok(headerMatch, "expected a header truncated banner");
  const header = headerMatch[1] ?? "";
  assert.match(header, /definition-collection scan stopped early/);
  assert.doesNotMatch(header, /reachability traversal stopped early/);
  assert.doesNotMatch(header, /retaining further accesses/);
  assert.doesNotMatch(header, /unused-declaration scan/);

  // Unreachable functions: an explicit "unknown" state naming definition
  // collection, never "(no unreachable functions)".
  assert.doesNotMatch(html, /\(no unreachable functions\)/);
  assert.match(html, /Unknown: the definition-collection scan stopped early/);

  // Reachable functions, global accesses, and unused declarations are all
  // unaffected by definitionsTruncated alone (per protocol docs: entry
  // point resolution still uses whatever definitions were collected, and
  // the reachability traversal/access scan/unused scan are independent of
  // definition collection).
  assert.doesNotMatch(html, /class="truncated">The reachability traversal/);
  assert.match(html, /\(no global or resource accesses\)/);
  assert.match(html, /\(no unused declarations\)/);
});

void test("entry point data flow HTML: functionsVisitedTruncated and definitionsTruncated together name both reasons unreachable functions are unknown", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      functionsVisitedTruncated: true,
      definitionsTruncated: true,
      functionsVisited: 200,
    }),
    documentUri,
  );
  assert.doesNotMatch(html, /\(no unreachable functions\)/);
  assert.match(
    html,
    /Unknown: the reachability traversal stopped early and the definition-collection scan stopped early, so unreachable functions could not be determined\./,
  );
});

void test("entry point data flow HTML: globalAccessesTruncated alone caveats only the global accesses section", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      globalAccessesTruncated: true,
      functionsVisited: 200,
    }),
    documentUri,
  );
  const headerMatch = /<h1>[\s\S]*?<p class="truncated">(.*?)<\/p>/.exec(html);
  assert.ok(headerMatch, "expected a header truncated banner");
  const header = headerMatch[1] ?? "";
  assert.match(header, /retaining further accesses/);
  assert.doesNotMatch(header, /reachability traversal stopped early/);
  assert.doesNotMatch(header, /unused-declaration scan/);

  assert.doesNotMatch(html, /\(no global or resource accesses\)/);
  assert.match(
    html,
    /none retained before the scan stopped -- this list may be incomplete/,
  );

  // Unaffected sections keep their ordinary, non-caveated empty states.
  assert.match(html, /\(no unreachable functions\)/);
  assert.doesNotMatch(
    html,
    /Unknown: the reachability traversal stopped early/,
  );
  assert.match(html, /\(no unused declarations\)/);
});

void test("entry point data flow HTML: unusedDeclarationsTruncated alone caveats only the unused declarations section", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      unusedDeclarationsTruncated: true,
      functionsVisited: 200,
    }),
    documentUri,
  );
  const headerMatch = /<h1>[\s\S]*?<p class="truncated">(.*?)<\/p>/.exec(html);
  assert.ok(headerMatch, "expected a header truncated banner");
  const header = headerMatch[1] ?? "";
  assert.match(header, /unused-declaration scan stopped early/);
  assert.doesNotMatch(header, /reachability traversal stopped early/);
  assert.doesNotMatch(header, /retaining further accesses/);

  assert.doesNotMatch(html, /\(no unused declarations\)/);
  assert.match(
    html,
    /none found before the scan stopped -- this list may be incomplete/,
  );

  // Unaffected sections keep their ordinary, non-caveated empty states.
  assert.match(html, /\(no unreachable functions\)/);
  assert.match(html, /\(no global or resource accesses\)/);
});

void test("entry point data flow HTML: all four truncation flags together caveat every affected section and the header names all four causes", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      functionsVisitedTruncated: true,
      definitionsTruncated: true,
      globalAccessesTruncated: true,
      unusedDeclarationsTruncated: true,
      functionsVisited: 4096,
    }),
    documentUri,
  );
  const headerMatch = /<h1>[\s\S]*?<p class="truncated">(.*?)<\/p>/.exec(html);
  assert.ok(headerMatch, "expected a header truncated banner");
  const header = headerMatch[1] ?? "";
  assert.match(header, /reachability traversal stopped early/);
  assert.match(header, /definition-collection scan stopped early/);
  assert.match(header, /retaining further accesses/);
  assert.match(header, /unused-declaration scan stopped early/);

  assert.doesNotMatch(html, /\(no unreachable functions\)/);
  assert.doesNotMatch(html, /\(no global or resource accesses\)/);
  assert.doesNotMatch(html, /\(no unused declarations\)/);
  assert.match(
    html,
    /Unknown: the reachability traversal stopped early and the definition-collection scan stopped early/,
  );
  assert.match(
    html,
    /none retained before the scan stopped -- this list may be incomplete/,
  );
  assert.match(
    html,
    /none found before the scan stopped -- this list may be incomplete/,
  );
});

void test("entry point data flow HTML's truncation messaging never invents a cause the protocol doesn't report (e.g. a numeric budget)", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      truncated: true,
      functionsVisitedTruncated: true,
      definitionsTruncated: true,
      globalAccessesTruncated: true,
      unusedDeclarationsTruncated: true,
      functionsVisited: 4096,
    }),
    documentUri,
  );
  assert.doesNotMatch(html, /budget/i);
});

void test("entry point data flow HTML escapes untrusted server-supplied content", () => {
  const html = entryPointDataFlowHtml(
    baseFlow({
      entryPoint: callHierarchyItem({
        name: "<script>alert(1)</script>",
        detail: "<b>evil</b>",
      }),
      reachableFunctions: [
        {
          function: callHierarchyItem({
            name: "<script>alert(1)</script>",
            detail: "<b>evil</b>",
          }),
          depth: 0,
          recursive: false,
        },
      ],
      explanation: "<img onerror=alert(1)>",
    }),
    documentUri,
  );
  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.doesNotMatch(html, /<b>evil<\/b>/);
  assert.doesNotMatch(html, /<img onerror=alert\(1\)>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt;/);
  assert.match(html, /&lt;b&gt;evil&lt;\/b&gt;/);
});

void test("entry point data flow error HTML escapes the failure message", () => {
  const html = entryPointDataFlowErrorHtml("<script>alert(1)</script> failed");
  assert.doesNotMatch(html, /<script>alert\(1\)<\/script>/);
  assert.match(html, /&lt;script&gt;alert\(1\)&lt;\/script&gt; failed/);
});

void test("a refresh with a successful found result renders content and titles the panel with the entry point name", () => {
  const flow = baseFlow();
  const outcome = resolveEntryPointDataFlowRefresh(
    false,
    documentUri,
    flow,
    undefined,
  );
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, "Entry-Point Data Flow: square");
  assert.equal(outcome.html, entryPointDataFlowHtml(flow, documentUri));
});

void test("a refresh with a not-found result still renders content, titled from the document instead of an entry point", () => {
  const flow: EntryPointDataFlow = {
    found: false,
    explanation: "No entry point is configured for this document.",
    entryPoint: null,
    reachableFunctions: [],
    unreachableFunctions: [],
    unusedDeclarations: [],
    globalAccesses: [],
    truncated: false,
    functionsVisitedTruncated: false,
    definitionsTruncated: false,
    globalAccessesTruncated: false,
    unusedDeclarationsTruncated: false,
    functionsVisited: 0,
  };
  const outcome = resolveEntryPointDataFlowRefresh(
    false,
    documentUri,
    flow,
    undefined,
  );
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, "Entry-Point Data Flow: example.hlsl");
});

void test("a failed refresh with no prior content shows an explicit error, not a placeholder", () => {
  const outcome = resolveEntryPointDataFlowRefresh(
    false,
    documentUri,
    undefined,
    "The request timed out.",
  );
  assert.equal(outcome.hasContent, false);
  assert.equal(outcome.title, undefined);
  assert.equal(
    outcome.html,
    entryPointDataFlowErrorHtml("The request timed out."),
  );
});

void test("a cancelled or failed refresh keeps prior successful content instead of erasing it", () => {
  const outcome = resolveEntryPointDataFlowRefresh(
    true,
    documentUri,
    undefined,
    "The request was cancelled.",
  );
  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
});

void test("a null result (e.g. the document could not be resolved) with prior content is treated the same as a failure: content is kept", () => {
  // Note: this exercises only resolveEntryPointDataFlowRefresh's own
  // null-handling in isolation. Generation-based staleness/out-of-order
  // suppression is a PanelController orchestration concern and is covered
  // separately in panelController.test.ts.
  const outcome = resolveEntryPointDataFlowRefresh(
    true,
    documentUri,
    null,
    undefined,
  );
  assert.equal(outcome.html, undefined);
  assert.equal(outcome.hasContent, true);
  assert.equal(outcome.title, undefined);
});

void test("parseEntryPointDataFlowLocationCommandArg accepts a well-formed location", () => {
  const parsed = parseEntryPointDataFlowLocationCommandArg({
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 1, character: 0 },
      end: { line: 1, character: 5 },
    },
  });
  assert.deepEqual(parsed, {
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 1, character: 0 },
      end: { line: 1, character: 5 },
    },
  });
});

void test("parseEntryPointDataFlowLocationCommandArg rejects a missing or empty uri", () => {
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({
      range: {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({
      uri: "",
      range: {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
});

void test("parseEntryPointDataFlowLocationCommandArg rejects malformed, non-integer, negative, or inverted ranges", () => {
  const uri = "file:///c:/shaders/example.hlsl";
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({ uri, range: undefined }),
    undefined,
  );
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({
      uri,
      range: {
        start: { line: 0.5, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({
      uri,
      range: {
        start: { line: -1, character: 0 },
        end: { line: 0, character: 0 },
      },
    }),
    undefined,
  );
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg({
      uri,
      range: {
        start: { line: 3, character: 0 },
        end: { line: 2, character: 0 },
      },
    }),
    undefined,
  );
});

void test("parseEntryPointDataFlowLocationCommandArg rejects non-object and null arguments", () => {
  assert.equal(parseEntryPointDataFlowLocationCommandArg(null), undefined);
  assert.equal(parseEntryPointDataFlowLocationCommandArg(undefined), undefined);
  assert.equal(
    parseEntryPointDataFlowLocationCommandArg(
      "file:///c:/shaders/example.hlsl",
    ),
    undefined,
  );
  assert.equal(parseEntryPointDataFlowLocationCommandArg(42), undefined);
});

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

void test("entryPointDataFlowRequestParams builds the exact hlsl/entryPointDataFlow request shape (only textDocument.uri)", () => {
  const params = entryPointDataFlowRequestParams(
    "file:///c:/shaders/example.hlsl",
  );
  assert.deepEqual(params, {
    textDocument: { uri: "file:///c:/shaders/example.hlsl" },
  });
  assert.deepEqual(Object.keys(params), ["textDocument"]);
});

void test("navigateEntryPointDataFlowLocation rejects a malformed argument without invoking the navigator", async () => {
  let invoked = false;
  const result = await navigateEntryPointDataFlowLocation(
    { uri: "file:///a.hlsl" }, // missing range
    {
      open(): Promise<void> {
        invoked = true;
        return Promise.resolve();
      },
    },
  );
  assert.equal(result.success, false);
  assert.match(result.errorMessage ?? "", /missing or malformed/);
  assert.equal(invoked, false);
});

void test("navigateEntryPointDataFlowLocation passes the exact validated location through to the navigator on success", async () => {
  const rawArgument = {
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 3, character: 1 },
      end: { line: 3, character: 9 },
    },
  };
  let received: ValidatedEntryPointDataFlowLocation | undefined;
  const result = await navigateEntryPointDataFlowLocation(rawArgument, {
    open(location): Promise<void> {
      received = location;
      return Promise.resolve();
    },
  });
  assert.equal(result.success, true);
  assert.equal(result.errorMessage, undefined);
  assert.deepEqual(received, {
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 3, character: 1 },
      end: { line: 3, character: 9 },
    },
  });
});

void test("navigateEntryPointDataFlowLocation reports a failure when the navigator itself throws", async () => {
  const rawArgument = {
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 0, character: 0 },
      end: { line: 0, character: 1 },
    },
  };
  const result = await navigateEntryPointDataFlowLocation(rawArgument, {
    open(): Promise<void> {
      return Promise.reject(new Error("document is no longer available"));
    },
  });
  assert.equal(result.success, false);
  assert.match(result.errorMessage ?? "", /document is no longer available/);
});

void test("navigateEntryPointDataFlowLocation wraps a non-Error thrown by the navigator with a generic message", async () => {
  const rawArgument = {
    uri: "file:///c:/shaders/example.hlsl",
    range: {
      start: { line: 0, character: 0 },
      end: { line: 0, character: 1 },
    },
  };
  const result = await navigateEntryPointDataFlowLocation(rawArgument, {
    open(): Promise<void> {
      // eslint-disable-next-line @typescript-eslint/prefer-promise-reject-errors
      return Promise.reject("disk error");
    },
  });
  assert.equal(result.success, false);
  assert.match(result.errorMessage ?? "", /disk error/);
});
