export type DefinitionValue = string | number | boolean;
export type TraceSetting = "off" | "messages" | "verbose";

export interface ConfigurationInspection<T> {
  globalValue?: T;
  workspaceValue?: T;
  workspaceFolderValue?: T;
  globalLanguageValue?: T;
  workspaceLanguageValue?: T;
  workspaceFolderLanguageValue?: T;
}

export interface ConfigurationReader {
  get(section: string): unknown;
  inspect(section: string): ConfigurationInspection<unknown> | undefined;
}

export interface HlslServerSettings {
  preprocessorDefinitions?: Readonly<Record<string, DefinitionValue>>;
  additionalIncludeDirectories?: readonly string[];
  virtualDirectoryMappings?: Readonly<Record<string, string>>;
  languageVersion?: string;
  targetProfile?: string;
  entryPoint?: string;
  additionalArguments?: readonly string[];
  dxcRuntimeDirectory?: string;
  inlayHints?: {
    types?: boolean;
    parameters?: boolean;
    matrixOrientation?: boolean;
    registers?: boolean;
    packedOffsets?: boolean;
    arrayStrides?: boolean;
    activeVariant?: boolean;
  };
}

const configuredValue = (
  inspection: ConfigurationInspection<unknown> | undefined,
): unknown =>
  inspection?.workspaceFolderLanguageValue ??
  inspection?.workspaceFolderValue ??
  inspection?.workspaceLanguageValue ??
  inspection?.workspaceValue ??
  inspection?.globalLanguageValue ??
  inspection?.globalValue;

export function readServerSettings(
  reader: ConfigurationReader,
): HlslServerSettings {
  const keys = [
    "preprocessorDefinitions",
    "additionalIncludeDirectories",
    "virtualDirectoryMappings",
    "languageVersion",
    "targetProfile",
    "entryPoint",
    "additionalArguments",
    "dxcRuntimeDirectory",
  ] as const;
  const entries = keys
    .map((key) => [key, configuredValue(reader.inspect(key))] as const)
    .filter((entry) => {
      return entry[1] !== undefined;
    });
  const result: HlslServerSettings = Object.fromEntries(entries);
  const inlayHintKeys = [
    "types",
    "parameters",
    "matrixOrientation",
    "registers",
    "packedOffsets",
    "arrayStrides",
    "activeVariant",
  ] as const;
  const inlayHints = Object.fromEntries(
    inlayHintKeys.map((key) => [key, reader.get(`inlayHints.${key}`) === true]),
  );
  return { ...result, inlayHints };
}

export function readDefaultLanguageVersion(
  reader: ConfigurationReader,
): string {
  const value = reader.get("languageVersion");
  return typeof value === "string" ? value : "2021";
}

export function readActiveVariant(reader: ConfigurationReader): string {
  const value = reader.get("activeVariant");
  return typeof value === "string" ? value.trim() : "";
}

export function readTraceSetting(reader: ConfigurationReader): TraceSetting {
  const value = reader.get("trace.server");
  return value === "messages" || value === "verbose" ? value : "off";
}
