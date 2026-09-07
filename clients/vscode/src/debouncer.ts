import { nodeScheduler, Scheduler } from "./panelController";

// A minimal debounced-callback helper: calling `schedule()` repeatedly
// cancels any previously pending invocation and starts a new delay, so a
// burst of events (a batch of file-system watcher events, a flurry of
// keystrokes, etc.) collapses into exactly one callback invocation rather
// than one per event. Shares `panelController.ts`'s injectable `Scheduler`
// seam so tests can drive it deterministically with a fake, manually
// advanced scheduler instead of real timers.
export class Debouncer {
  private handle: number | undefined;

  public constructor(
    private readonly callback: () => void,
    private readonly scheduler: Scheduler = nodeScheduler,
    private readonly delayMs = 500,
  ) {}

  public schedule(): void {
    if (this.handle !== undefined) {
      this.scheduler.clearTimeout(this.handle);
    }
    this.handle = this.scheduler.setTimeout(() => {
      this.handle = undefined;
      this.callback();
    }, this.delayMs);
  }

  // Cancels any pending invocation without running it. Safe to call
  // whether or not a schedule() is currently pending.
  public dispose(): void {
    if (this.handle !== undefined) {
      this.scheduler.clearTimeout(this.handle);
      this.handle = undefined;
    }
  }
}
