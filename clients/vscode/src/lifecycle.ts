export interface LifecycleClient {
  start(): Promise<void>;
  stop(): Promise<void>;
}

export type LifecycleState = "stopped" | "starting" | "running" | "stopping";

export type ConnectionTransition = "disconnected" | "recovered";

export interface ConnectionObservation {
  readonly transition: ConnectionTransition;
  readonly epoch: number;
}

export class ConnectionRecoveryTracker {
  private hasRun = false;
  private disconnected = false;
  private epoch = 0;

  public observe(running: boolean): ConnectionObservation | undefined {
    if (!running) {
      if (this.hasRun && !this.disconnected) {
        this.disconnected = true;
        return { transition: "disconnected", epoch: ++this.epoch };
      }
      return undefined;
    }
    if (!this.hasRun) {
      this.hasRun = true;
      return undefined;
    }
    if (!this.disconnected) {
      return undefined;
    }
    this.disconnected = false;
    return { transition: "recovered", epoch: ++this.epoch };
  }

  public isCurrentRecovery(epoch: number): boolean {
    return !this.disconnected && epoch === this.epoch;
  }
}

export async function runGuardedRecovery(
  synchronization: Promise<void>,
  defer: () => Promise<void>,
  isCurrentRecovery: () => boolean,
  refresh: () => void | Promise<void>,
): Promise<boolean> {
  await synchronization;
  await defer();
  if (!isCurrentRecovery()) {
    return false;
  }
  await refresh();
  return true;
}

export class ClientLifecycle<T extends LifecycleClient> {
  private client: T | undefined;
  private operation: Promise<void> = Promise.resolve();
  private currentState: LifecycleState = "stopped";

  public constructor(private readonly createClient: () => Promise<T>) {}

  public get state(): LifecycleState {
    return this.currentState;
  }

  public start(): Promise<void> {
    return this.enqueue(async () => {
      if (this.client !== undefined) {
        return;
      }
      this.currentState = "starting";
      let candidate: T | undefined;
      try {
        candidate = await this.createClient();
        await candidate.start();
        this.client = candidate;
        this.currentState = "running";
      } catch (error) {
        if (candidate !== undefined) {
          await candidate.stop().catch(() => undefined);
        }
        this.currentState = "stopped";
        throw error;
      }
    });
  }

  public stop(): Promise<void> {
    return this.enqueue(() => this.stopCurrent());
  }

  public restart(): Promise<void> {
    return this.enqueue(async () => {
      await this.stopCurrent();
      this.currentState = "starting";
      let candidate: T | undefined;
      try {
        candidate = await this.createClient();
        await candidate.start();
        this.client = candidate;
        this.currentState = "running";
      } catch (error) {
        if (candidate !== undefined) {
          await candidate.stop().catch(() => undefined);
        }
        this.currentState = "stopped";
        throw error;
      }
    });
  }

  public withClient<TResult>(
    action: (client: T) => Promise<TResult>,
  ): Promise<TResult | undefined> {
    const current = this.client;
    return current === undefined ? Promise.resolve(undefined) : action(current);
  }

  private enqueue(action: () => Promise<void>): Promise<void> {
    const result = this.operation.then(action, action);
    this.operation = result.catch(() => undefined);
    return result;
  }

  private async stopCurrent(): Promise<void> {
    const current = this.client;
    if (current === undefined) {
      this.currentState = "stopped";
      return;
    }
    this.client = undefined;
    this.currentState = "stopping";
    try {
      await current.stop();
    } finally {
      this.currentState = "stopped";
    }
  }
}
