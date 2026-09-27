#include <hlsl_intellisense/workspace/configuration_authoring.h>

#include <hlsl_intellisense/workspace/configuration.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <vector>

namespace hlsl_intellisense::workspace {
namespace {

using Json = nlohmann::json;

constexpr std::string_view schema_uri =
    "https://raw.githubusercontent.com/KStocky/HLSL-LSP/main/schemas/v1/"
    "shadertoolsconfig.schema.json";

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool excluded_directory(const std::filesystem::path& path) {
    static const std::set<std::string, std::less<>> excluded{".git",
                                                             ".hg",
                                                             ".svn",
                                                             ".vs",
                                                             "binaries",
                                                             "bin",
                                                             "build",
                                                             "cmake-build-debug",
                                                             "cmake-build-release",
                                                             "deriveddatacache",
                                                             "dist",
                                                             "external",
                                                             "generated",
                                                             "intermediate",
                                                             "node_modules",
                                                             "obj",
                                                             "out",
                                                             "packages",
                                                             "saved",
                                                             "third_party",
                                                             "thirdparty",
                                                             "vendor"};
    return excluded.contains(lower_ascii(path.filename().string()));
}

[[nodiscard]] bool shader_extension(const std::filesystem::path& path) {
    const auto extension = lower_ascii(path.extension().string());
    return extension == ".hlsl" || extension == ".hlsli" || extension == ".fx" ||
           extension == ".fxh";
}

[[nodiscard]] std::string generic_relative(const std::filesystem::path& path,
                                           const std::filesystem::path& workspace) {
    auto relative = path.lexically_relative(workspace).generic_string();
    if (relative.empty()) {
        relative = path.filename().generic_string();
    }
    return relative;
}

[[nodiscard]] std::string error_code(ConfigurationErrorCode code) {
    switch (code) {
    case ConfigurationErrorCode::invalid_directory:
        return "invalid-directory";
    case ConfigurationErrorCode::invalid_json:
        return "invalid-json";
    case ConfigurationErrorCode::invalid_type:
        return "invalid-type";
    case ConfigurationErrorCode::invalid_virtual_directory:
        return "invalid-virtual-directory";
    case ConfigurationErrorCode::missing_path:
        return "missing-path";
    case ConfigurationErrorCode::path_not_directory:
        return "path-not-directory";
    case ConfigurationErrorCode::invalid_glob:
        return "invalid-glob";
    case ConfigurationErrorCode::conflicting_runtime:
        return "conflicting-runtime";
    case ConfigurationErrorCode::invalid_variant:
        return "invalid-variant";
    case ConfigurationErrorCode::invalid_pipeline:
        return "invalid-pipeline";
    }
    return "invalid-configuration";
}

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return std::rotr(value, static_cast<int>(count));
}

[[nodiscard]] std::array<std::uint32_t, 8> sha256(std::string_view content) {
    constexpr std::array constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};
    std::vector<std::uint8_t> bytes(content.begin(), content.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    bytes.push_back(0x80U);
    while (bytes.size() % 64U != 56U) {
        bytes.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    }

    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t block = 0; block < bytes.size(); block += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = block + index * 4U;
            words[index] = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
                           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
                           static_cast<std::uint32_t>(bytes[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto first = rotate_right(words[index - 15], 7) ^
                               rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const auto second = rotate_right(words[index - 2], 17) ^
                                rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + first + words[index - 7] + second;
        }
        auto [a, b, c, d, e, f, g, h] = hash;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto choose = (e & f) ^ (~e & g);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto first = h +
                               (rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25)) +
                               choose + constants[index] + words[index];
            const auto second =
                (rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22)) + majority;
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }
    return hash;
}

} // namespace

ConfigurationDiscovery discover_shader_files(const std::filesystem::path& workspace,
                                             const ConfigurationDiscoveryLimits& limits,
                                             const std::function<void()>& cancellation_checkpoint) {
    ConfigurationDiscovery result;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(workspace).lexically_normal();
    if (!std::filesystem::is_directory(absolute, error) || error) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_directory,
                                 absolute,
                                 {},
                                 "Workspace folder '" + absolute.string() +
                                     "' does not exist or is not a directory"};
    }

    std::filesystem::recursive_directory_iterator iterator{
        absolute, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        const auto entry = *iterator;
        if (entry.is_directory(error)) {
            ++result.directories_visited;
            if (result.directories_visited > limits.max_directories) {
                result.truncated = true;
                result.truncation_reason = "directoryLimit";
                break;
            }
            if (excluded_directory(entry.path())) {
                iterator.disable_recursion_pending();
            } else {
                const auto nested = entry.path() / configuration_file_name;
                if (entry.path() != absolute && std::filesystem::is_regular_file(nested, error) &&
                    !error) {
                    result.nested_configurations.push_back(nested);
                    iterator.disable_recursion_pending();
                }
            }
        } else if (entry.is_regular_file(error) && shader_extension(entry.path())) {
            if (entry.file_size(error) <= limits.max_file_size && !error) {
                if (result.shader_files.size() == limits.max_files) {
                    result.truncated = true;
                    result.truncation_reason = "fileLimit";
                    break;
                }
                result.shader_files.push_back(entry.path());
            }
        }
        error.clear();
        iterator.increment(error);
    }
    std::ranges::sort(result.shader_files);
    std::ranges::sort(result.nested_configurations);
    return result;
}

ConfigurationPreview
generate_configuration_preview(const std::filesystem::path& workspace,
                               const std::optional<std::string>& existing_content,
                               const std::vector<ConfigurationSelection>& selections,
                               const std::optional<std::string>& draft_content) {
    ConfigurationPreview result;
    if (draft_content) {
        result.content = *draft_content;
        result.changed = !existing_content || result.content != *existing_content;
        try {
            validate_workspace_configuration_content(result.content,
                                                     workspace / configuration_file_name);
            result.valid = true;
        } catch (const ConfigurationError& error) {
            result.errors.push_back({.code = error_code(error.code()),
                                     .field = error.key().empty() ? "$" : error.key(),
                                     .message = error.what()});
        }
        return result;
    }
    Json document;
    if (existing_content.has_value()) {
        try {
            document = Json::parse(*existing_content, nullptr, true, true);
        } catch (const Json::parse_error& error) {
            result.content = *existing_content;
            result.errors.push_back(
                {.code = "invalid-json", .field = "$", .message = error.what()});
            return result;
        }
        if (!document.is_object()) {
            result.content = *existing_content;
            result.errors.push_back({.code = "invalid-type",
                                     .field = "<root>",
                                     .message = "Configuration root must be an object"});
            return result;
        }
        try {
            validate_workspace_configuration_content(*existing_content,
                                                     workspace / configuration_file_name);
        } catch (const ConfigurationError& error) {
            result.content = *existing_content;
            result.errors.push_back({.code = error_code(error.code()),
                                     .field = error.key().empty() ? "$" : error.key(),
                                     .message = error.what()});
            return result;
        }
    } else {
        document = Json::object();
    }

    const auto before = document;
    if (!document.contains("$schema")) {
        document["$schema"] = schema_uri;
    }
    if (!document.contains("root")) {
        document["root"] = true;
    }
    if (!document.contains("hlsl.languageVersion")) {
        document["hlsl.languageVersion"] = "2021";
    }

    std::vector<ConfigurationSelection> ordered = selections;
    std::ranges::sort(ordered, [&](const auto& left, const auto& right) {
        return std::tuple{generic_relative(left.file, workspace), left.entry_point,
                          left.target_profile} < std::tuple{generic_relative(right.file, workspace),
                                                            right.entry_point,
                                                            right.target_profile};
    });
    const auto unique = std::ranges::unique(ordered);
    ordered.erase(unique.begin(), ordered.end());

    if (!ordered.empty()) {
        if (!document.contains("hlsl.fileGroups")) {
            document["hlsl.fileGroups"] = Json::array();
        }
        if (!document["hlsl.fileGroups"].is_array()) {
            result.content = existing_content.value_or(document.dump(2) + '\n');
            result.errors.push_back({.code = "invalid-type",
                                     .field = "hlsl.fileGroups",
                                     .message = "hlsl.fileGroups must be an array"});
            return result;
        }
        for (const auto& selection : ordered) {
            const auto file = generic_relative(selection.file, workspace);
            const auto duplicate =
                std::ranges::any_of(document["hlsl.fileGroups"], [&](const auto& group) {
                    return group.is_object() &&
                           group.value("hlsl.entryPoint", "") == selection.entry_point &&
                           group.value("hlsl.targetProfile", "") == selection.target_profile &&
                           group.contains("files") && group["files"].is_array() &&
                           std::ranges::find(group["files"], Json(file)) != group["files"].end();
                });
            if (!duplicate) {
                document["hlsl.fileGroups"].push_back(
                    Json{{"name", file + " - " + selection.entry_point},
                         {"files", Json::array({file})},
                         {"hlsl.targetProfile", selection.target_profile},
                         {"hlsl.entryPoint", selection.entry_point}});
            }
        }
    }

    result.content = document.dump(2) + '\n';
    result.changed = existing_content ? result.content != *existing_content : document != before;
    try {
        validate_workspace_configuration_content(result.content,
                                                 workspace / configuration_file_name);
        result.valid = true;
    } catch (const ConfigurationError& error) {
        result.errors.push_back({.code = error_code(error.code()),
                                 .field = error.key().empty() ? "$" : error.key(),
                                 .message = error.what()});
    }
    return result;
}

ConfigurationPreview
generate_capture_configuration_preview(const std::filesystem::path& workspace,
                                       const std::optional<std::string>& existing_content,
                                       const std::vector<CapturedConfigurationEntry>& entries,
                                       const std::vector<CapturedPipeline>& pipelines) {
    if (existing_content) {
        if (existing_content->size() > 2U * 1024U * 1024U) {
            return {.content = *existing_content,
                    .valid = false,
                    .changed = false,
                    .errors = {{.code = "capture-limit",
                                .field = "$",
                                .message = "Configuration exceeds the 2 MiB limit"}}};
        }
        enum class LexicalState { text, string, line_comment, block_comment };
        LexicalState state = LexicalState::text;
        bool escaped = false;
        unsigned depth = 0;
        for (std::size_t index = 0; index < existing_content->size(); ++index) {
            const char character = (*existing_content)[index];
            const char next =
                index + 1 < existing_content->size() ? (*existing_content)[index + 1] : '\0';
            if (state == LexicalState::line_comment) {
                if (character == '\n' || character == '\r') {
                    state = LexicalState::text;
                }
            } else if (state == LexicalState::block_comment) {
                if (character == '*' && next == '/') {
                    state = LexicalState::text;
                    ++index;
                }
            } else if (state == LexicalState::string) {
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    state = LexicalState::text;
                }
            } else if (character == '"') {
                state = LexicalState::string;
            } else if (character == '/' && next == '/') {
                state = LexicalState::line_comment;
                ++index;
            } else if (character == '/' && next == '*') {
                state = LexicalState::block_comment;
                ++index;
            } else if (character == '{' || character == '[') {
                if (++depth > 64) {
                    return {.content = *existing_content,
                            .valid = false,
                            .changed = false,
                            .errors = {{.code = "capture-limit",
                                        .field = "$",
                                        .message = "Configuration nesting exceeds 64 levels"}}};
                }
            } else if (character == '}' || character == ']') {
                if (depth != 0) {
                    --depth;
                }
            }
        }
    }
    auto base = generate_configuration_preview(workspace, existing_content, {});
    if (!base.valid) {
        return base;
    }
    auto document = Json::parse(base.content);
    const auto conflict = [&](std::string field, std::string message) {
        ConfigurationPreview result;
        result.content = existing_content.value_or(base.content);
        result.errors.push_back(
            {.code = "capture-conflict", .field = std::move(field), .message = std::move(message)});
        return result;
    };
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto path = generic_relative(entry.file, workspace);
        Json settings = Json::parse(entry.settings_json, nullptr, false);
        if (!settings.is_object()) {
            return conflict("selectedEntryIds", "Captured settings are not an object");
        }
        settings["files"] = Json::array({path});
        if (entry.variant_name) {
            settings["name"] = *entry.variant_name;
            if (!document.contains("hlsl.variants")) {
                document["hlsl.variants"] = Json::array();
                document["hlsl.variantsVersion"] = 1;
            }
            auto& variants = document["hlsl.variants"];
            const auto matching = std::ranges::find_if(variants, [&](const Json& value) {
                return value.is_object() && value.value("name", "") == *entry.variant_name;
            });
            if (matching == variants.end()) {
                variants.push_back(std::move(settings));
            } else if (*matching != settings) {
                return conflict("hlsl.variants",
                                "Captured variant name already has different settings");
            }
        } else {
            if (!document.contains("hlsl.fileGroups")) {
                document["hlsl.fileGroups"] = Json::array();
            }
            auto& groups = document["hlsl.fileGroups"];
            bool duplicate = false;
            for (const Json& existing : groups) {
                if (!existing.is_object() || !existing.contains("files") ||
                    !existing["files"].is_array() ||
                    std::ranges::find(existing["files"], Json(path)) == existing["files"].end()) {
                    continue;
                }
                auto comparable = existing;
                comparable.erase("name");
                if (comparable != settings) {
                    return conflict("hlsl.fileGroups",
                                    "Captured file overlaps an existing file group");
                }
                duplicate = true;
            }
            if (!duplicate) {
                groups.push_back(std::move(settings));
            }
        }
    }
    for (const auto& pipeline : pipelines) {
        Json stages = Json::object();
        for (const auto& stage : pipeline.stages) {
            const auto& entry = entries.at(stage.entry_index);
            Json value = {{"file", generic_relative(entry.file, workspace)},
                          {"entryPoint", entry.entry_point},
                          {"targetProfile", entry.target_profile}};
            if (entry.variant_name) {
                value["variant"] = *entry.variant_name;
            }
            stages[stage.stage] = std::move(value);
        }
        Json declaration = {{"name", pipeline.name}, {"stages", std::move(stages)}};
        if (!document.contains("hlsl.pipelines")) {
            document["hlsl.pipelines"] = Json::array();
            document["hlsl.pipelinesVersion"] = 1;
        }
        auto& existing = document["hlsl.pipelines"];
        const auto matching = std::ranges::find_if(existing, [&](const Json& value) {
            return value.is_object() && value.value("name", "") == pipeline.name;
        });
        if (matching == existing.end()) {
            existing.push_back(std::move(declaration));
        } else if (*matching != declaration) {
            return conflict("hlsl.pipelines",
                            "Captured pipeline name already has different stages");
        }
    }
    const auto content = document.dump(2) + '\n';
    if (content.size() > 2U * 1024U * 1024U) {
        return conflict("$", "Captured preview exceeds the 2 MiB configuration limit");
    }
    return generate_configuration_preview(workspace, existing_content, {}, content);
}

std::string configuration_content_hash(std::string_view content) {
    std::ostringstream stream;
    stream << "sha256:";
    for (const auto word : sha256(content)) {
        stream << std::hex << std::setfill('0') << std::setw(8) << word;
    }
    return stream.str();
}

} // namespace hlsl_intellisense::workspace
