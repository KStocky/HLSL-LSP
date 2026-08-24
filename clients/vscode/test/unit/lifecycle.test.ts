import assert from "node:assert/strict";
import test from "node:test";

import { ClientLifecycle, LifecycleClient } from "../../src/lifecycle";

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
