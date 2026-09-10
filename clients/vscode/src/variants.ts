export interface VariantInfo {
  readonly name: string;
  readonly description: string;
  readonly default: boolean;
  readonly applicable: boolean;
}

export interface VariantList {
  readonly activeVariant: string | null;
  readonly variants: readonly VariantInfo[];
}

export function applicableVariants(
  variants: readonly VariantInfo[],
): readonly VariantInfo[] {
  return variants.filter((variant) => variant.applicable);
}
