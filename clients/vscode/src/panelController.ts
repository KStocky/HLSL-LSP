// Generic, vscode-agnostic state machine for a "refresh-on-demand" analysis
// webview panel backed by exactly one LSP request. This is the orchestration
// extension.ts previously duplicated inline for each analysis panel
// (generation-guarded async refresh so a slower, superseded, or
// document-switched-away request can never overwrite a newer result;
// debounced edit-triggered refresh; document-switch and disposal
// handling) -- factored out into one dependency-injected, vscode-free
// class so unit tests can drive the exact orchestration behavior (request
// payload, out-of-order suppression, document switching, debounce timing,
// disposal) without an extension host.

import {
  AnalysisFreshness,
  AnalysisFreshnessCause,
  AnalysisFreshnessState,
} from "./analysisFreshness";

export interface PanelHost {
  setHtml(html: string): void;
  setTitle(title: string): void;
  setFreshness(state: AnalysisFreshnessState): void;
}

export interface RefreshOutcome {
  // undefined means "leave the panel's currently displayed HTML/title
  // alone".
  readonly html: string | undefined;
  readonly hasContent: boolean;
  readonly title: string | undefined;
}

export type ResolveRefresh<TResult> = (
  hasContent: boolean,
  uri: string,
  result: TResult | null | undefined,
  failureMessage: string | undefined,
) => RefreshOutcome;

// A minimal setTimeout/clearTimeout seam. Defaults to the real Node
// timers; unit tests inject a fake, manually-advanced scheduler instead so
// debounce behavior can be asserted deterministically, without depending
// on real wall-clock delays.
export interface Scheduler {
  setTimeout(callback: () => void, delayMs: number): number;
  clearTimeout(handle: number): void;
}

export const nodeScheduler: Scheduler = {
  setTimeout: (callback, delayMs) =>
    setTimeout(callback, delayMs) as unknown as number,
  clearTimeout: (handle) => {
    clearTimeout(handle);
  },
};

export interface PanelOpenResult {
  readonly switchingDocument: boolean;
}

export class PanelController<TResult> {
  private uri: string | undefined;
  private hasContent = false;
  private debounceHandle: number | undefined;
  public constructor(
    private readonly host: PanelHost,
    private readonly request: (
      uri: string,
    ) => Promise<TResult | null | undefined>,
    private readonly resolve: ResolveRefresh<TResult>,
    private readonly scheduler: Scheduler = nodeScheduler,
    private readonly debounceMs = 500,
    private readonly freshness: AnalysisFreshness = new AnalysisFreshness(),
  ) {}

  public get trackedUri(): string | undefined {
    return this.uri;
  }

  public get hasSuccessfulContent(): boolean {
    return this.hasContent;
  }

  // Tracks `uri` as this panel's document. Returns whether this is a
  // switch to a different document than previously tracked: the caller
  // The caller may use switchingDocument for target-specific bookkeeping,
  // but content is deliberately retained until an accepted replacement
  // arrives.
  public open(uri: string): PanelOpenResult {
    const switchingDocument = this.uri !== uri;
    this.uri = uri;
    return { switchingDocument };
  }

  // Issues one request for `uri` and applies its outcome, guarded by a
  // monotonic generation counter so a slower, superseded request can never
  // overwrite a result from a newer one, and by the panel's currently
  // tracked uri so a request for a document the panel has since switched
  // away from (or been disposed while in flight) is discarded rather than
  // misapplied.
  public async refresh(
    uri: string,
    cause: AnalysisFreshnessCause = "Manual refresh",
  ): Promise<void> {
    const generation = this.freshness.beginRefresh(cause);
    this.host.setFreshness(this.freshness.state);
    let result: TResult | null | undefined;
    let failureMessage: string | undefined;
    try {
      result = await this.request(uri);
    } catch (error) {
      failureMessage =
        error instanceof Error ? error.message : "The request failed.";
    }
    if (this.uri !== uri) {
      return;
    }
    const accepted =
      result !== null && result !== undefined
        ? this.freshness.succeed(generation)
        : this.freshness.fail(generation);
    if (!accepted) {
      return;
    }
    const outcome = this.resolve(this.hasContent, uri, result, failureMessage);
    this.hasContent = outcome.hasContent;
    if (outcome.title !== undefined) {
      this.host.setTitle(outcome.title);
    }
    if (outcome.html !== undefined) {
      this.host.setHtml(outcome.html);
    }
    this.host.setFreshness(this.freshness.state);
  }

  // Refreshes the panel's own currently tracked document, if any. Used by
  // save/configuration-change triggers, which are not high-frequency
  // enough to need debouncing. Returns undefined (rather than a
  // pre-resolved promise) when no document is tracked, so callers can tell
  // "nothing to do" apart from "refreshed".
  public refreshTracked(
    cause: AnalysisFreshnessCause = "Manual refresh",
  ): Promise<void> | undefined {
    return this.uri === undefined ? undefined : this.refresh(this.uri, cause);
  }

  // Schedules a debounced refresh of the panel's own tracked document,
  // canceling any previously pending one -- a burst of keystrokes triggers
  // one request, not a storm of them.
  public scheduleDebouncedRefresh(
    cause: AnalysisFreshnessCause = "Source edit",
  ): void {
    if (this.uri === undefined) {
      return;
    }
    this.freshness.invalidate(cause, true);
    this.host.setFreshness(this.freshness.state);
    if (this.debounceHandle !== undefined) {
      this.scheduler.clearTimeout(this.debounceHandle);
    }
    this.debounceHandle = this.scheduler.setTimeout(() => {
      this.debounceHandle = undefined;
      if (this.uri !== undefined) {
        void this.refresh(this.uri, cause);
      }
    }, this.debounceMs);
  }

  public markStale(cause: AnalysisFreshnessCause): void {
    this.freshness.invalidate(cause);
    this.host.setFreshness(this.freshness.state);
  }

  public cancelScheduledRefresh(): void {
    if (this.debounceHandle !== undefined) {
      this.scheduler.clearTimeout(this.debounceHandle);
      this.debounceHandle = undefined;
    }
  }

  // Cancels any pending debounce timer and clears tracked state so a
  // request already in flight before disposal is discarded (via the uri
  // check in `refresh`) when it resolves, and so a disposed panel can
  // never be resurrected by a stale debounce firing. Called when the
  // underlying webview panel itself is disposed (closed).
  public dispose(): void {
    this.cancelScheduledRefresh();
    this.uri = undefined;
    this.hasContent = false;
  }
}
