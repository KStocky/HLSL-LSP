#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::workspace {

inline constexpr std::string_view configuration_file_name = "shadertoolsconfig.json";

enum class ConfigurationErrorCode {
    invalid_directory,
    invalid_json,
    invalid_type,
    invalid_virtual_directory,
    missing_path,
    path_not_directory,
    invalid_glob,
    conflicting_runtime,
    invalid_variant
};

// The only shadertoolsconfig.json named-variants schema version this build
// understands. A config that declares hlsl.variants must set
// hlsl.variantsVersion to this value; other values are reported rather than
// silently accepted so the schema can evolve compatibly.
inline constexpr int supported_variants_version = 1;

class ConfigurationError final : public std::runtime_error {
  public:
    ConfigurationError(ConfigurationErrorCode code, std::filesystem::path file, std::string key,
                       std::string_view message);

    [[nodiscard]] ConfigurationErrorCode code() const noexcept;
    [[nodiscard]] const std::filesystem::path& file() const noexcept;
    [[nodiscard]] const std::string& key() const noexcept;

  private:
    ConfigurationErrorCode code_;
    std::filesystem::path file_;
    std::string key_;
};

// Fully resolved settings contributed by a named variant, after any inherited
// variants have been merged in. Every field is optional so a variant only
// overrides the settings it explicitly declares; unset fields leave the
// file-derived configuration untouched. Paths are already resolved to absolute,
// existing directories relative to the declaring configuration file.
struct VariantSettings {
    std::map<std::string, std::string, std::less<>> preprocessor_definitions;
    std::vector<std::filesystem::path> additional_include_directories;
    std::map<std::string, std::filesystem::path, std::less<>> virtual_directory_mappings;
    std::optional<std::string> language_version;
    std::optional<std::string> target_profile;
    std::optional<std::string> entry_point;
    std::optional<std::vector<std::string>> additional_arguments;
    std::optional<std::filesystem::path> dxc_runtime_directory;
};

// A named compilation variant resolved for a specific shader. `applicable` is
// true when the variant's file patterns (if any) match the shader, meaning the
// variant may be selected for it.
struct ResolvedVariant {
    std::string name;
    std::string description;
    bool is_default{};
    bool applicable{true};
    VariantSettings settings;
};

// Outcome of selecting a named variant for a configuration.
enum class VariantSelection {
    applied,       // The variant existed, was applicable, and its settings were applied.
    undefined,     // No variant with that name is defined for the shader.
    not_applicable // A variant with that name exists but its file patterns exclude the shader.
};

struct WorkspaceConfiguration {
    std::map<std::string, std::string, std::less<>> preprocessor_definitions;
    std::vector<std::filesystem::path> additional_include_directories;
    std::map<std::string, std::filesystem::path, std::less<>> virtual_directory_mappings;
    std::optional<std::string> language_version;
    std::optional<std::string> target_profile;
    std::optional<std::string> entry_point;
    std::vector<std::string> additional_arguments;
    // Selects the process-wide DXC runtime for the workspace. Empty selects the
    // bundled default. Conflicting nested selections are reported rather than
    // silently switched.
    std::optional<std::filesystem::path> dxc_runtime_directory;
    // Named compilation variants resolved for the shader this configuration was
    // loaded for, in declaration order (outermost configuration first). Each
    // variant already has inheritance applied; `applicable` reflects whether the
    // variant's file patterns match the shader. Selecting a variant applies its
    // settings on top of the file-derived configuration.
    std::vector<ResolvedVariant> variants;

    [[nodiscard]] dxc::CompilerOptions compiler_options() const;
};

struct ConfigurationOverrides {
    std::optional<std::map<std::string, std::string, std::less<>>> preprocessor_definitions;
    std::optional<std::vector<std::filesystem::path>> additional_include_directories;
    std::optional<std::map<std::string, std::filesystem::path, std::less<>>>
        virtual_directory_mappings;
    std::optional<std::optional<std::string>> language_version;
    std::optional<std::optional<std::string>> target_profile;
    std::optional<std::optional<std::string>> entry_point;
    std::optional<std::vector<std::string>> additional_arguments;
    std::optional<std::optional<std::filesystem::path>> dxc_runtime_directory;
};

[[nodiscard]] std::vector<std::filesystem::path>
discover_configuration_files(const std::filesystem::path& shader_directory);

// Directory-only loading preserves the original API and intentionally cannot select file groups.
[[nodiscard]] WorkspaceConfiguration
load_workspace_configuration(const std::filesystem::path& shader_directory);

// File-aware loading applies matching file groups after the normal directory hierarchy.
[[nodiscard]] WorkspaceConfiguration
load_workspace_configuration_for_file(const std::filesystem::path& shader_file);

[[nodiscard]] WorkspaceConfiguration
apply_configuration_overrides(WorkspaceConfiguration configuration,
                              const ConfigurationOverrides& overrides,
                              const std::filesystem::path& base_directory);

// Applies the settings of the named variant on top of `configuration`. The
// variant is looked up in `configuration.variants`. Returns whether it was
// applied, is not defined, or is defined but not applicable to the shader this
// configuration was loaded for. Applying a variant overrides scalar settings,
// merges definition and virtual-mapping maps by key, searches variant include
// directories first, and replaces the DXC runtime selection when the variant
// declares one.
[[nodiscard]] VariantSelection apply_variant(WorkspaceConfiguration& configuration,
                                             std::string_view name);

} // namespace hlsl_intellisense::workspace
