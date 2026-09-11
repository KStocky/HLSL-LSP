import assert from "node:assert/strict";
import test from "node:test";

import {
  ClientLifecycle,
  ConnectionRecoveryTracker,
  LifecycleClient,
  runGuardedRecovery,
} from "../../src/lifecycle";

void test("same-client stopped-to-running transition reports one automatic recovery", () => {
  const tracker = new ConnectionRecoveryTracker();

  assert.equal(tracker.observe(false), undefined);
  assert.equal(tracker.observe(true), undefined);
  assert.equal(tracker.observe(true), undefined);
  assert.equal(tracker.observe(false)?.transition, "disconnected");
  assert.equal(tracker.observe(false), undefined);
  assert.equal(tracker.observe(true)?.transition, "recovered");
  assert.equal(tracker.observe(true), undefined);
});

void test("a replacement client initial start is not mistaken for automatic recovery", () => {
  const replaced = new ConnectionRecoveryTracker();
  replaced.observe(true);
  assert.equal(replaced.observe(false)?.transition, "disconnected");

  const replacement = new ConnectionRecoveryTracker();
  assert.equal(replacement.observe(false), undefined);
  assert.equal(replacement.observe(true), undefined);
});

void test("a later stopped transition invalidates a queued recovery epoch", () => {
  const tracker = new ConnectionRecoveryTracker();
  tracker.observe(true);
  tracker.observe(false);
  const recovery = tracker.observe(true);
  assert(recovery);
  assert.equal(tracker.isCurrentRecovery(recovery.epoch), true);

  tracker.observe(false);

  assert.equal(tracker.isCurrentRecovery(recovery.epoch), false);
});

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
  reject(error: unknown): void;
}

function deferred<T>(): Deferred<T> {
  let resolveFn!: (value: T) => void;
  let rejectFn!: (error: unknown) => void;
  const promise = new Promise<T>((resolve, reject) => {
    resolveFn = resolve;
    rejectFn = reject;
  });
  return { promise, resolve: resolveFn, reject: rejectFn };
}

void test("recovery refresh waits for settings synchronization and the deferred turn", async () => {
  const synchronization = deferred<undefined>();
  const deferredTurn = deferred<undefined>();
  let refreshed = false;
  const recovery = runGuardedRecovery(
    synchronization.promise,
    () => deferredTurn.promise,
    () => true,
    () => {
      refreshed = true;
    },
  );

  await Promise.resolve();
  assert.equal(refreshed, false);
  synchronization.resolve(undefined);
  await Promise.resolve();
  assert.equal(refreshed, false);
  deferredTurn.resolve(undefined);
  assert.equal(await recovery, true);
  assert.equal(refreshed, true);
});

void test("recovery refresh does not run after synchronization failure", async () => {
  const synchronization = deferred<undefined>();
  let refreshed = false;
  const recovery = runGuardedRecovery(
    synchronization.promise,
    () => Promise.resolve(),
    () => true,
    () => {
      refreshed = true;
    },
  );

  synchronization.reject(new Error("settings failed"));

  await assert.rejects(recovery, /settings failed/);
  assert.equal(refreshed, false);
});

void test("later disconnect after synchronization prevents deferred recovery refresh", async () => {
  const tracker = new ConnectionRecoveryTracker();
  tracker.observe(true);
  tracker.observe(false);
  const observation = tracker.observe(true);
  assert(observation);
  const deferredTurn = deferred<undefined>();
  let refreshed = false;
  const recovery = runGuardedRecovery(
    Promise.resolve(),
    () => deferredTurn.promise,
    () => tracker.isCurrentRecovery(observation.epoch),
    () => {
      refreshed = true;
    },
  );
  await Promise.resolve();

  tracker.observe(false);
  deferredTurn.resolve(undefined);

  assert.equal(await recovery, false);
  assert.equal(refreshed, false);
});

class FakeClient implements LifecycleClient {
  public starts = 0;
  public stops = 0;

  public constructor(private readonly failStart = false) {}

  public start(): Promise<void> {
    ++this.starts;
    if (this.failStart) {
      return Promise.reject(new Error("start failed"));
    }
    return Promise.resolve();
  }

  public stop(): Promise<void> {
    ++this.stops;
    return Promise.resolve();
  }
}

void test("restart gracefully stops only the owned client before replacing it", async () => {
  const clients: FakeClient[] = [];
  const lifecycle = new ClientLifecycle(() => {
    const client = new FakeClient();
    clients.push(client);
    return Promise.resolve(client);
  });

  await lifecycle.start();
  await lifecycle.restart();

  const [first, second] = clients;
  assert(first);
  assert(second);
  assert.equal(first.stops, 1);
  assert.equal(second.stops, 0);
  assert.equal(lifecycle.state, "running");

  await lifecycle.stop();
  assert.equal(second.stops, 1);
  assert.equal(lifecycle.state, "stopped");
});

void test("a failed start cleans up its candidate and remains reusable", async () => {
  const failed = new FakeClient(true);
  const recovered = new FakeClient();
  let attempt = 0;
  const lifecycle = new ClientLifecycle(() =>
    Promise.resolve(attempt++ === 0 ? failed : recovered),
  );

  await assert.rejects(lifecycle.start(), /start failed/);
  assert.equal(failed.stops, 1);
  assert.equal(lifecycle.state, "stopped");

  await lifecycle.start();
  assert.equal(recovered.starts, 1);
  assert.equal(lifecycle.state, "running");
});

void test("concurrent starts are serialized and create one client", async () => {
  let creates = 0;
  const lifecycle = new ClientLifecycle(() => {
    ++creates;
    return Promise.resolve(new FakeClient());
  });

  await Promise.all([lifecycle.start(), lifecycle.start()]);
  assert.equal(creates, 1);
});

void test("an in-flight client action does not block shutdown", async () => {
  const client = new FakeClient();
  const lifecycle = new ClientLifecycle(() => Promise.resolve(client));
  await lifecycle.start();

  void lifecycle.withClient(() => new Promise<never>(() => undefined));
  await lifecycle.stop();

  assert.equal(client.stops, 1);
  assert.equal(lifecycle.state, "stopped");
});
