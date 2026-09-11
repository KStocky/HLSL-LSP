export interface EffectiveContextOrigin {
  readonly label: string;
  readonly setting: string;
  readonly uri?: string;
}

export function effectiveConfigurationOriginUri(
  context: EffectiveShaderContext,
): string | undefined {
  if (
    context.configurationUri !== null &&
    context.configurationUri !== undefined
  ) {
    return context.configurationUri;
  }
  return [
    context.origins.variant,
    context.origins.entryPoint,
    context.origins.targetProfile,
  ]
    .map((origin) => origin?.uri)
    .find((uri) => uri !== undefined && uri !== "");
}

export interface EffectiveShaderContext {
  readonly documentUri: string;
  readonly file: string;
  readonly configurationUri?: string | null;
  readonly activeVariant: string | null;
  readonly entryPoint: string;
  readonly targetProfile: string;
  readonly origins: {
    readonly variant?: EffectiveContextOrigin;
    readonly entryPoint?: EffectiveContextOrigin;
    readonly targetProfile?: EffectiveContextOrigin;
  };
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

export function contextValue(value: string | null | undefined): string {
  return value === null || value === undefined || value === ""
    ? "Not configured"
    : value;
}

export function variantLabel(value: string | null | undefined): string {
  return value === null || value === undefined || value === ""
    ? "Default"
    : value;
}

function originSummary(context: EffectiveShaderContext): string {
  const entries: string[] = [];
  if (context.origins.variant !== undefined) {
    entries.push(`Variant — ${context.origins.variant.label}`);
  }
  if (context.origins.entryPoint !== undefined) {
    entries.push(`Entry point — ${context.origins.entryPoint.label}`);
  }
  if (context.origins.targetProfile !== undefined) {
    entries.push(`Target profile — ${context.origins.targetProfile.label}`);
  }
  return entries.join("; ");
}

export function effectiveContextHeaderHtml(
  context: EffectiveShaderContext | undefined,
): string {
  const file = context?.file ?? "Not configured";
  const variant = variantLabel(context?.activeVariant);
  const entryPoint = contextValue(context?.entryPoint);
  const targetProfile = contextValue(context?.targetProfile);
  const origins = context === undefined ? "" : originSummary(context);
  return `<section style="margin:.25rem 0 1rem;padding:.55rem .7rem;border:1px solid var(--vscode-panel-border);border-radius:4px;">
<div><strong>File:</strong> ${escapeHtml(file)} · <strong>Variant:</strong> ${escapeHtml(variant)} · <strong>Entry point:</strong> ${escapeHtml(entryPoint)} · <strong>Target profile:</strong> ${escapeHtml(targetProfile)}</div>
${origins === "" ? "" : `<div style="color:var(--vscode-descriptionForeground);margin-top:.25rem;"><strong>Configuration origins:</strong> ${escapeHtml(origins)}</div>`}
</section>`;
}

export function effectiveContextTooltip(
  context: EffectiveShaderContext,
): string {
  const lines = [
    "**HLSL shader context**",
    "",
    `File: ${context.file}`,
    `Variant: ${variantLabel(context.activeVariant)}`,
    `Entry point: ${contextValue(context.entryPoint)}`,
    `Target profile: ${contextValue(context.targetProfile)}`,
  ];
  const origins = originSummary(context);
  if (origins !== "") {
    lines.push(`Configuration origins: ${origins}`);
  }
  lines.push("", "Click to select a shader variant.");
  return lines.join("\n\n");
}
