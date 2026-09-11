import { AnalysisTrackingMode } from "./analysisFreshness";

export const defaultAnalysisTrackingMode: AnalysisTrackingMode = "pinned";

export type AnalysisTargetTransition = "unchanged" | "retarget" | "unavailable";

export function analysisTargetTransition(
  mode: AnalysisTrackingMode,
  trackedUri: string,
  activeUri: string | undefined,
  targetAvailable: boolean,
): AnalysisTargetTransition {
  if (mode === "pinned") {
    return "unchanged";
  }
  if (activeUri === undefined) {
    return "unavailable";
  }
  return !targetAvailable || trackedUri !== activeUri
    ? "retarget"
    : "unchanged";
}

export function isReopenedAnalysisTarget(
  mode: AnalysisTrackingMode,
  trackedUri: string,
  openedUri: string,
  targetAvailable: boolean,
): boolean {
  return mode === "pinned" && !targetAvailable && trackedUri === openedUri;
}
