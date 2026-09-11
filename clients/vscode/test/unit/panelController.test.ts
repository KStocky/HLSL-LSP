import assert from "node:assert/strict";
import test from "node:test";

import { AnalysisFreshnessState } from "../../src/analysisFreshness";
import {
  PanelController,
  PanelHost,
  RefreshOutcome,
  Scheduler,
} from "../../src/panelController";

// A fully deterministic, manually-advanced scheduler standing in for
// setTimeout/clearTimeout, so debounce behavior can be asserted without
// depending on real wall-clock delays or fake-timer libraries.
class FakeScheduler implements Scheduler {
  private nextHandle = 1;
  private readonly pending = new Map<number, () => void>();
  public readonly scheduledDelays: number[] = [];

  public setTimeout(callback: () => void, delayMs: number): number {
    const handle = this.nextHandle;
    this.nextHandle += 1;
    this.pending.set(handle, callback);
    this.scheduledDelays.push(delayMs);
    return handle;
  }

  public clearTimeout(handle: number): void {
    this.pending.delete(handle);
  }

  public get pendingCount(): number {
    return this.pending.size;
  }

  // Fires every currently pending callback, in scheduling order -- mirrors
  // "enough wall-clock time has passed for every pending timer to fire".
  public flush(): void {
    const callbacks = [...this.pending.values()];
    this.pending.clear();
    for (const callback of callbacks) {
      callback();
    }
  }
}

class RecordingHost implements PanelHost {
  public readonly htmlCalls: string[] = [];
  public readonly titleCalls: string[] = [];
  public readonly freshnessCalls: AnalysisFreshnessState[] = [];

  public setHtml(html: string): void {
    this.htmlCalls.push(html);
  }

  public setTitle(title: string): void {
    this.titleCalls.push(title);
  }

  public setFreshness(state: AnalysisFreshnessState): void {
    this.freshnessCalls.push(state);
  }
}

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
  reject(error: unknown): void;
}

function deferred<T>(): Deferred<T> {
  let resolveFn!: (value: T) => void;
  let rejectFn!: (error: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolveFn = resolvePromise;
    rejectFn = rejectPromise;
  });
  return { promise, resolve: resolveFn, reject: rejectFn };
}

// A resolve() function whose output is derived entirely from its own
// inputs, so tests can assert -- via the rendered "html" -- exactly which
// uri and result the controller actually passed through for a given
// refresh, without needing a real analysis payload/renderer.
function testResolve(
  hasContent: boolean,
  uri: string,
  result: string | null | undefined,
  failureMessage: string | undefined,
): RefreshOutcome {
  if (result === null || result === undefined) {
    return hasContent
      ? { html: undefined, hasContent: true, title: undefined }
      : {
          html: `error:${failureMessage ?? "unavailable"}`,
          hasContent: false,
          title: undefined,
        };
  }
  return {
    html: `content:${result}@${uri}`,
    hasContent: true,
    title: `title:${result}`,
  };
}

function makeController(
  request: (uri: string) => Promise<string | null | undefined>,
  scheduler: Scheduler = new FakeScheduler(),
): { controller: PanelController<string>; host: RecordingHost } {
  const host = new RecordingHost();
  const controller = new PanelController<string>(
    host,
    request,
    testResolve,
    scheduler,
  );
  return { controller, host };
}

void test("open() reports switchingDocument true for a new document", () => {
  const { controller } = makeController(() => Promise.resolve("x"));
  const result = controller.open("file:///a.hlsl");
  assert.equal(result.switchingDocument, true);
  assert.equal(controller.hasSuccessfulContent, false);
  assert.equal(controller.trackedUri, "file:///a.hlsl");
});

void test("switching targets retains the last successful content until replacement", async () => {
  const pending = deferred<string | null>();
  let call = 0;
  const { controller, host } = makeController(() => {
    call += 1;
    return call === 1 ? Promise.resolve("old") : pending.promise;
  });
  controller.open("file:///a.hlsl");
  await controller.refresh("file:///a.hlsl");

  const switched = controller.open("file:///b.hlsl");
  const refresh = controller.refresh("file:///b.hlsl");
  pending.reject(new Error("failed"));
  await refresh;

  assert.equal(switched.switchingDocument, true);
  assert.equal(controller.hasSuccessfulContent, true);
  assert.deepEqual(host.htmlCalls, ["content:old@file:///a.hlsl"]);
  assert.equal(host.freshnessCalls.at(-1)?.status, "Refresh failed");
});

void test("open() reports switchingDocument false for a re-invocation on the same document, preserving hasContent", async () => {
  const { controller } = makeController(() => Promise.resolve("x"));
  controller.open("file:///a.hlsl");
  await controller.refresh("file:///a.hlsl");
  assert.equal(controller.hasSuccessfulContent, true);
  const result = controller.open("file:///a.hlsl");
  assert.equal(result.switchingDocument, false);
  assert.equal(controller.hasSuccessfulContent, true);
});

void test("refresh() issues the request with the exact tracked uri and applies a successful outcome to the host", async () => {
  const requestedUris: string[] = [];
  const { controller, host } = makeController((uri) => {
    requestedUris.push(uri);
    return Promise.resolve("payload");
  });
  controller.open("file:///shader.hlsl");
  await controller.refresh("file:///shader.hlsl");
  assert.deepEqual(requestedUris, ["file:///shader.hlsl"]);
  assert.deepEqual(host.htmlCalls, ["content:payload@file:///shader.hlsl"]);
  assert.deepEqual(host.titleCalls, ["title:payload"]);
  assert.equal(controller.hasSuccessfulContent, true);
});

void test("refresh() passes the request's thrown error through to resolve() as a failure message", async () => {
  const { controller, host } = makeController(() =>
    Promise.reject(new Error("boom")),
  );
  controller.open("file:///shader.hlsl");
  await controller.refresh("file:///shader.hlsl");
  assert.deepEqual(host.htmlCalls, ["error:boom"]);
  assert.equal(controller.hasSuccessfulContent, false);
});

void test("refresh() treats a non-Error thrown value with a generic failure message", async () => {
  const { controller, host } = makeController(() =>
    // eslint-disable-next-line @typescript-eslint/prefer-promise-reject-errors
    Promise.reject("nope"),
  );
  controller.open("file:///shader.hlsl");
  await controller.refresh("file:///shader.hlsl");
  assert.deepEqual(host.htmlCalls, ["error:The request failed."]);
});

void test("an out-of-order (slower) refresh's result is discarded once a newer refresh has already completed", async () => {
  const first = deferred<string | null>();
  const second = deferred<string | null>();
  let call = 0;
  const { controller, host } = makeController(() => {
    call += 1;
    return call === 1 ? first.promise : second.promise;
  });
  controller.open("file:///shader.hlsl");

  const firstRefresh = controller.refresh("file:///shader.hlsl");
  const secondRefresh = controller.refresh("file:///shader.hlsl");

  // The second (newer) request resolves first.
  second.resolve("new");
  await secondRefresh;
  assert.deepEqual(host.htmlCalls, ["content:new@file:///shader.hlsl"]);

  // The first (older, superseded) request resolves after -- its result
  // must never overwrite the newer one already applied.
  first.resolve("stale");
  await firstRefresh;
  assert.deepEqual(host.htmlCalls, ["content:new@file:///shader.hlsl"]);
  assert.deepEqual(host.titleCalls, ["title:new"]);
  assert.equal(host.freshnessCalls.at(-1)?.status, "Current");
});

void test("manual refresh uses the tracked URI and exposes Manual refresh while retaining content", async () => {
  const pending = deferred<string | null>();
  const { controller, host } = makeController(() => pending.promise);
  controller.open("file:///tracked.hlsl");
  const refresh = controller.refreshTracked();
  assert.equal(host.freshnessCalls.at(-1)?.status, "Refreshing");
  assert.equal(host.freshnessCalls.at(-1)?.cause, "Manual refresh");
  pending.resolve("fresh");
  await refresh;
  assert.equal(host.freshnessCalls.at(-1)?.status, "Current");
});

void test("a refresh whose document has since switched away is discarded even though it is the latest generation", async () => {
  const pending = deferred<string | null>();
  const { controller, host } = makeController(() => pending.promise);
  controller.open("file:///a.hlsl");

  const refreshA = controller.refresh("file:///a.hlsl");
  // The panel is redirected to a different document while the request for
  // "a.hlsl" is still in flight (e.g. the command re-fired for a newly
  // focused editor).
  controller.open("file:///b.hlsl");

  pending.resolve("late-a-result");
  await refreshA;

  assert.deepEqual(host.htmlCalls, []);
  assert.deepEqual(host.titleCalls, []);
});

void test("refreshTracked() returns undefined and issues no request when no document is open", () => {
  let called = false;
  const { controller } = makeController(() => {
    called = true;
    return Promise.resolve("x");
  });
  assert.equal(controller.refreshTracked(), undefined);
  assert.equal(called, false);
});

void test("refreshTracked() refreshes the panel's own currently tracked document", async () => {
  const requestedUris: string[] = [];
  const { controller } = makeController((uri) => {
    requestedUris.push(uri);
    return Promise.resolve("x");
  });
  controller.open("file:///tracked.hlsl");
  await controller.refreshTracked();
  assert.deepEqual(requestedUris, ["file:///tracked.hlsl"]);
});

void test("scheduleDebouncedRefresh cancels a previously pending timer so only the latest fires", () => {
  const scheduler = new FakeScheduler();
  const requestedUris: string[] = [];
  const { controller } = makeController((uri) => {
    requestedUris.push(uri);
    return Promise.resolve("x");
  }, scheduler);
  controller.open("file:///shader.hlsl");

  controller.scheduleDebouncedRefresh();
  controller.scheduleDebouncedRefresh();
  controller.scheduleDebouncedRefresh();

  // Three scheduling calls, but only the last timer should still be
  // pending -- the first two were canceled by the later calls.
  assert.equal(scheduler.pendingCount, 1);
  scheduler.flush();
  assert.deepEqual(requestedUris, ["file:///shader.hlsl"]);
});

void test("an edit immediately invalidates an in-flight result before the debounced replacement starts", async () => {
  const scheduler = new FakeScheduler();
  const first = deferred<string | null>();
  const second = deferred<string | null>();
  let call = 0;
  const { controller, host } = makeController(() => {
    call += 1;
    return call === 1 ? first.promise : second.promise;
  }, scheduler);
  controller.open("file:///shader.hlsl");
  const initial = controller.refresh("file:///shader.hlsl");

  controller.scheduleDebouncedRefresh("Source edit");
  first.resolve("obsolete");
  await initial;
  assert.deepEqual(host.htmlCalls, []);
  assert.equal(host.freshnessCalls.at(-1)?.status, "Refreshing");

  scheduler.flush();
  second.resolve("fresh");
  await new Promise<void>((resolve) => setImmediate(resolve));
  assert.deepEqual(host.htmlCalls, ["content:fresh@file:///shader.hlsl"]);
  assert.equal(host.freshnessCalls.at(-1)?.status, "Current");
});

void test("edit then stop cancels the pending refresh and leaves disconnected state stale", () => {
  const scheduler = new FakeScheduler();
  let requested = false;
  const { controller, host } = makeController(() => {
    requested = true;
    return Promise.resolve("unexpected");
  }, scheduler);
  controller.open("file:///shader.hlsl");

  controller.scheduleDebouncedRefresh("Source edit");
  controller.cancelScheduledRefresh();
  controller.markStale("Disconnected server");
  scheduler.flush();

  assert.equal(requested, false);
  assert.equal(host.freshnessCalls.at(-1)?.status, "Stale");
  assert.equal(host.freshnessCalls.at(-1)?.cause, "Disconnected server");
});

void test("scheduleDebouncedRefresh does nothing when no document is open", () => {
  const scheduler = new FakeScheduler();
  const { controller } = makeController(() => Promise.resolve("x"), scheduler);
  controller.scheduleDebouncedRefresh();
  assert.equal(scheduler.pendingCount, 0);
});

void test("dispose cancels a pending debounce timer so it never fires", () => {
  const scheduler = new FakeScheduler();
  let called = false;
  const { controller } = makeController(() => {
    called = true;
    return Promise.resolve("x");
  }, scheduler);
  controller.open("file:///shader.hlsl");
  controller.scheduleDebouncedRefresh();
  assert.equal(scheduler.pendingCount, 1);

  controller.dispose();

  assert.equal(scheduler.pendingCount, 0);
  scheduler.flush();
  assert.equal(called, false);
});

void test("dispose resets tracked state so a request already in flight is discarded on completion", async () => {
  const pending = deferred<string | null>();
  const { controller, host } = makeController(() => pending.promise);
  controller.open("file:///shader.hlsl");
  const refresh = controller.refresh("file:///shader.hlsl");

  controller.dispose();
  assert.equal(controller.trackedUri, undefined);
  assert.equal(controller.hasSuccessfulContent, false);

  pending.resolve("late");
  await refresh;

  assert.deepEqual(host.htmlCalls, []);
  assert.deepEqual(host.titleCalls, []);
});

void test("dispose after a successful refresh, then re-open, starts from a clean hasContent state", async () => {
  const { controller } = makeController(() => Promise.resolve("x"));
  controller.open("file:///shader.hlsl");
  await controller.refresh("file:///shader.hlsl");
  assert.equal(controller.hasSuccessfulContent, true);

  controller.dispose();
  const result = controller.open("file:///shader.hlsl");

  assert.equal(result.switchingDocument, true);
  assert.equal(controller.hasSuccessfulContent, false);
});
