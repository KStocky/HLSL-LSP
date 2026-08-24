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
    invalid_glob
};

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

struct WorkspaceConfiguration {
    std::map<std::string, std::string, std::less<>> preprocessor_definitions;
    std::vector<std::filesystem::path> additional_include_directories;
    std::map<std::string, std::filesystem::path, std::less<>> virtual_directory_mappings;
    std::optional<std::string> language_version;
    std::optional<std::string> target_profile;
    std::optional<std::string> entry_point;
    std::vector<std::string> additional_arguments;

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

} // namespace hlsl_intellisense::workspace
