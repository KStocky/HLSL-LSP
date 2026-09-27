#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::workspace {

inline constexpr unsigned configuration_authoring_protocol_version = 1;

struct ConfigurationDiscoveryLimits {
    std::size_t max_directories{2048};
    std::size_t max_files{256};
    std::uintmax_t max_file_size{2U * 1024U * 1024U};
};

struct ConfigurationDiscovery {
    std::vector<std::filesystem::path> shader_files;
    std::vector<std::filesystem::path> nested_configurations;
    std::size_t directories_visited{};
    bool truncated{};
    std::string truncation_reason;
};

struct ConfigurationSelection {
    std::filesystem::path file;
    std::string entry_point;
    std::string target_profile;

    bool operator==(const ConfigurationSelection&) const = default;
};

struct ConfigurationValidationError {
    std::string code;
    std::string field;
    std::string message;
};

struct ConfigurationPreview {
    std::string content;
    bool valid{};
    bool changed{};
    std::vector<ConfigurationValidationError> errors;
};

struct CapturedConfigurationEntry {
    std::filesystem::path file;
    std::string entry_point;
    std::string target_profile;
    std::string settings_json;
    std::optional<std::string> variant_name;
};

struct CapturedPipelineStage {
    std::string stage;
    std::size_t entry_index{};
};

struct CapturedPipeline {
    std::string name;
    std::vector<CapturedPipelineStage> stages;
};

[[nodiscard]] ConfigurationPreview
generate_capture_configuration_preview(const std::filesystem::path& workspace,
                                       const std::optional<std::string>& existing_content,
                                       const std::vector<CapturedConfigurationEntry>& entries,
                                       const std::vector<CapturedPipeline>& pipelines);

[[nodiscard]] ConfigurationDiscovery
discover_shader_files(const std::filesystem::path& workspace,
                      const ConfigurationDiscoveryLimits& limits = {},
                      const std::function<void()>& cancellation_checkpoint = {});

[[nodiscard]] ConfigurationPreview
generate_configuration_preview(const std::filesystem::path& workspace,
                               const std::optional<std::string>& existing_content,
                               const std::vector<ConfigurationSelection>& selections,
                               const std::optional<std::string>& draft_content = std::nullopt);

[[nodiscard]] std::string configuration_content_hash(std::string_view content);

} // namespace hlsl_intellisense::workspace
