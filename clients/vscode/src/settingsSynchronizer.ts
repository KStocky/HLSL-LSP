export type ApplySettings<T> = (
  settings: T,
  isCurrentConnection: () => boolean,
) => Promise<boolean>;

export class RunningSettingsSynchronizer<T> {
  private operation: Promise<void> = Promise.resolve();
  private running = false;
  private connectionGeneration = 0;
  private settingsRevision = 0;
  private appliedConnectionGeneration = -1;
  private appliedSettingsRevision = -1;

  public constructor(
    private readonly readSettings: () => T,
    private readonly applySettings: ApplySettings<T>,
  ) {}

  public stateChanged(running: boolean): Promise<void> {
    if (running !== this.running) {
      this.running = running;
      if (running) {
        ++this.connectionGeneration;
      }
    }
    return this.enqueue();
  }

  public configurationChanged(): Promise<void> {
    ++this.settingsRevision;
    return this.enqueue();
  }

  public ensureSynchronized(): Promise<void> {
    return this.enqueue();
  }

  private enqueue(): Promise<void> {
    const result = this.operation.then(
      () => this.synchronize(),
      () => this.synchronize(),
    );
    this.operation = result.catch(() => undefined);
    return result;
  }

  private async synchronize(): Promise<void> {
    if (!this.running) {
      return;
    }
    const connectionGeneration = this.connectionGeneration;
    const settingsRevision = this.settingsRevision;
    if (
      this.appliedConnectionGeneration === connectionGeneration &&
      this.appliedSettingsRevision === settingsRevision
    ) {
      return;
    }

    const isCurrentConnection = (): boolean =>
      this.running && this.connectionGeneration === connectionGeneration;
    const applied = await this.applySettings(
      this.readSettings(),
      isCurrentConnection,
    );
    if (applied && isCurrentConnection()) {
      this.appliedConnectionGeneration = connectionGeneration;
      this.appliedSettingsRevision = settingsRevision;
    }
  }
}
