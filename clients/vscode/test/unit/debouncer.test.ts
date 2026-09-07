import assert from "node:assert/strict";
import test from "node:test";

import { Debouncer } from "../../src/debouncer";
import { Scheduler } from "../../src/panelController";

// A fully deterministic, manually-advanced scheduler standing in for
// setTimeout/clearTimeout, mirroring panelController.test.ts's fake so
// debounce behavior can be asserted without depending on real wall-clock
// delays.
class FakeScheduler implements Scheduler {
  private nextHandle = 1;
  private readonly pending = new Map<number, () => void>();

  public setTimeout(callback: () => void, delayMs: number): number {
    const handle = this.nextHandle;
    this.nextHandle += 1;
    this.pending.set(handle, callback);
    void delayMs;
    return handle;
  }

  public clearTimeout(handle: number): void {
    this.pending.delete(handle);
  }

  public get pendingCount(): number {
    return this.pending.size;
  }

  public flush(): void {
    const callbacks = [...this.pending.values()];
    this.pending.clear();
    for (const callback of callbacks) {
      callback();
    }
  }
}

void test("Debouncer invokes the callback once after schedule() and a delay", () => {
  const scheduler = new FakeScheduler();
  let calls = 0;
  const debouncer = new Debouncer(() => {
    calls += 1;
  }, scheduler);

  debouncer.schedule();
  assert.equal(calls, 0);
  scheduler.flush();
  assert.equal(calls, 1);
});

void test("Debouncer collapses a burst of schedule() calls into exactly one invocation", () => {
  const scheduler = new FakeScheduler();
  let calls = 0;
  const debouncer = new Debouncer(() => {
    calls += 1;
  }, scheduler);

  // Simulates a burst of filesystem watcher events (e.g. a multi-file save
  // or a git checkout) arriving before the debounce delay elapses.
  debouncer.schedule();
  debouncer.schedule();
  debouncer.schedule();
  debouncer.schedule();

  assert.equal(scheduler.pendingCount, 1);
  scheduler.flush();
  assert.equal(calls, 1);
});

void test("Debouncer schedules a fresh delay after a previous invocation already fired", () => {
  const scheduler = new FakeScheduler();
  let calls = 0;
  const debouncer = new Debouncer(() => {
    calls += 1;
  }, scheduler);

  debouncer.schedule();
  scheduler.flush();
  assert.equal(calls, 1);

  debouncer.schedule();
  scheduler.flush();
  assert.equal(calls, 2);
});

void test("Debouncer.dispose() cancels a pending invocation so it never fires", () => {
  const scheduler = new FakeScheduler();
  let calls = 0;
  const debouncer = new Debouncer(() => {
    calls += 1;
  }, scheduler);

  debouncer.schedule();
  debouncer.dispose();

  assert.equal(scheduler.pendingCount, 0);
  scheduler.flush();
  assert.equal(calls, 0);
});

void test("Debouncer.dispose() is a no-op when nothing is pending", () => {
  const scheduler = new FakeScheduler();
  const debouncer = new Debouncer(() => {
    throw new Error("must not be called");
  }, scheduler);

  assert.doesNotThrow(() => {
    debouncer.dispose();
  });
});
