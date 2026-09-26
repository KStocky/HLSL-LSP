export const firstRunGuidanceStateKey = "hlsl.firstRunGuidanceShown";
export const hlslToolsConflictStateKey = "hlsl.hlslToolsConflictWarningShown";
export const hlslToolsExtensionId = "TimGJones.hlsltools";

export function shouldShowFirstRunGuidance(
  languageId: string | undefined,
  alreadyShown: boolean,
): boolean {
  return languageId === "hlsl" && !alreadyShown;
}

export function shouldShowHlslToolsConflict(
  languageId: string | undefined,
  alreadyShown: boolean,
  hlslToolsAvailable: boolean,
): boolean {
  return languageId === "hlsl" && !alreadyShown && hlslToolsAvailable;
}
