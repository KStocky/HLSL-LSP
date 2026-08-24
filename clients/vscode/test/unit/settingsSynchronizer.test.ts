import assert from "node:assert/strict";
import test from "node:test";

import { RunningSettingsSynchronizer } from "../../src/settingsSynchronizer";

void test("settings are applied once per configuration and running connection", async () => {
  let settings = 1;
  const applied: number[] = [];
  const synchronizer = new RunningSettingsSynchronizer(
    () => settings,
    (value, isCurrentConnection) => {
      assert(isCurrentConnection());
      applied.push(value);
      return Promise.resolve(true);
    },
  );

  await Promise.all([
    synchronizer.stateChanged(true),
    synchronizer.ensureSynchronized(),
  ]);
  assert.deepEqual(applied, [1]);

  settings = 2;
  await synchronizer.configurationChanged();
  assert.deepEqual(applied, [1, 2]);

  await synchronizer.stateChanged(false);
  await synchronizer.stateChanged(true);
  assert.deepEqual(applied, [1, 2, 2]);
});

void test("a stale apply cannot mark a replacement connection synchronized", async () => {
  let settings = 1;
  let releaseFirst: (() => void) | undefined;
  let applyingFirst: (() => void) | undefined;
  const firstStarted = new Promise<void>((resolve) => {
    applyingFirst = resolve;
  });
  const firstGate = new Promise<void>((resolve) => {
    releaseFirst = resolve;
  });
  const attempts: number[] = [];
  const completed: number[] = [];
  const synchronizer = new RunningSettingsSynchronizer(
    () => settings,
    async (value, isCurrentConnection) => {
      attempts.push(value);
      if (value === 1) {
        applyingFirst?.();
        await firstGate;
      }
      if (!isCurrentConnection()) {
        return false;
      }
      completed.push(value);
      return true;
    },
  );

  const initial = synchronizer.stateChanged(true);
  await firstStarted;
  const stopped = synchronizer.stateChanged(false);
  settings = 2;
  const replacement = synchronizer.stateChanged(true);
  releaseFirst?.();
  await Promise.all([initial, stopped, replacement]);

  assert.deepEqual(attempts, [1, 2]);
  assert.deepEqual(completed, [2]);
});
