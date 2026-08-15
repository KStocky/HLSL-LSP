#include <hlsl_intellisense/workspace/configuration.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace hlsl_intellisense::workspace {
namespace {

using Json = nlohmann::json;

struct ConfigFile {
    std::filesystem::path path;
    bool root{};
    std::map<std::string, std::string, std::less<>> definitions;
    std::vector<std::filesystem::path> include_directories;
    std::map<std::string, std::filesystem::path, std::less<>> virtual_mappings;
    std::optional<std::string> language_version;
    std::optional<std::string> target_profile;
    std::optional<std::string> entry_point;
    std::optional<std::vector<std::string>> additional_arguments;
};

[[noreturn]] void throw_type_error(const std::filesystem::path& file, std::string_view key,
                                   std::string_view expected) {
    throw ConfigurationError{ConfigurationErrorCode::invalid_type, file, std::string{key},
                             "Configuration key '" + std::string{key} + "' in '" + file.string() +
                                 "' must be " + std::string{expected}};
}

[[nodiscard]] Json read_json(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_json,
                                 path,
                                 {},
                                 "Unable to read configuration file '" + path.string() + "'"};
    }

    try {
        return Json::parse(stream, nullptr, true, true);
    } catch (const Json::parse_error& error) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_json,
                                 path,
                                 {},
                                 "Invalid JSON in configuration file '" + path.string() +
                                     "': " + error.what()};
    }
}

[[nodiscard]] std::filesystem::path resolve_directory(const std::filesystem::path& config_path,
                                                      std::string_view key,
                                                      const std::string& value) {
    auto path = std::filesystem::path{value};
    if (path.is_relative()) {
        path = config_path.parent_path() / path;
    }
    path = std::filesystem::absolute(path).lexically_normal();

    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        throw ConfigurationError{
            ConfigurationErrorCode::missing_path, config_path, std::string{key},
            "Path '" + path.string() + "' configured by '" + std::string{key} + "' does not exist"};
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        throw ConfigurationError{ConfigurationErrorCode::path_not_directory, config_path,
                                 std::string{key},
                                 "Path '" + path.string() + "' configured by '" + std::string{key} +
                                     "' is not a directory"};
    }
    return std::filesystem::weakly_canonical(path);
}

[[nodiscard]] std::string definition_value(const std::filesystem::path& path, std::string_view key,
                                           const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number() || value.is_boolean()) {
        return value.dump();
    }
    throw_type_error(path, key, "a string, number, or boolean");
}

[[nodiscard]] std::optional<std::string>
optional_string(const Json& json, const std::filesystem::path& path, std::string_view key) {
    const auto iterator = json.find(key);
    if (iterator == json.end()) {
        return std::nullopt;
    }
    if (!iterator->is_string()) {
        throw_type_error(path, key, "a string");
    }
    return iterator->get<std::string>();
}

[[nodiscard]] ConfigFile parse_config_file(const std::filesystem::path& path) {
    const auto json = read_json(path);
    if (!json.is_object()) {
        throw_type_error(path, "<root>", "an object");
    }

    ConfigFile result;
    result.path = path;

    if (const auto root = json.find("root"); root != json.end()) {
        if (!root->is_boolean()) {
            throw_type_error(path, "root", "a boolean");
        }
        result.root = root->get<bool>();
    }

    if (const auto definitions = json.find("hlsl.preprocessorDefinitions");
        definitions != json.end()) {
        if (!definitions->is_object()) {
            throw_type_error(path, "hlsl.preprocessorDefinitions", "an object");
        }
        for (const auto& [name, value] : definitions->items()) {
            if (name.empty()) {
                throw ConfigurationError{ConfigurationErrorCode::invalid_type, path,
                                         "hlsl.preprocessorDefinitions",
                                         "Preprocessor definition names must not be empty"};
            }
            result.definitions.emplace(name, definition_value(path, name, value));
        }
    }

    if (const auto includes = json.find("hlsl.additionalIncludeDirectories");
        includes != json.end()) {
        if (!includes->is_array()) {
            throw_type_error(path, "hlsl.additionalIncludeDirectories", "an array of strings");
        }
        for (const auto& include : *includes) {
            if (!include.is_string()) {
                throw_type_error(path, "hlsl.additionalIncludeDirectories", "an array of strings");
            }
            result.include_directories.push_back(resolve_directory(
                path, "hlsl.additionalIncludeDirectories", include.get_ref<const std::string&>()));
        }
    }

    if (const auto mappings = json.find("hlsl.virtualDirectoryMappings"); mappings != json.end()) {
        if (!mappings->is_object()) {
            throw_type_error(path, "hlsl.virtualDirectoryMappings", "an object of string paths");
        }
        for (const auto& [virtual_directory, real_directory] : mappings->items()) {
            if (virtual_directory.empty() ||
                (virtual_directory.front() != '/' && virtual_directory.front() != '\\')) {
                throw ConfigurationError{ConfigurationErrorCode::invalid_virtual_directory, path,
                                         "hlsl.virtualDirectoryMappings",
                                         "Virtual directory '" + virtual_directory +
                                             "' must start with a forward slash or backslash"};
            }
            if (!real_directory.is_string()) {
                throw_type_error(path, "hlsl.virtualDirectoryMappings",
                                 "an object of string paths");
            }
            result.virtual_mappings.emplace(
                virtual_directory, resolve_directory(path, "hlsl.virtualDirectoryMappings",
                                                     real_directory.get_ref<const std::string&>()));
        }
    }

    result.language_version = optional_string(json, path, "hlsl.languageVersion");
    result.target_profile = optional_string(json, path, "hlsl.targetProfile");
    result.entry_point = optional_string(json, path, "hlsl.entryPoint");

    if (const auto arguments = json.find("hlsl.additionalArguments"); arguments != json.end()) {
        if (!arguments->is_array()) {
            throw_type_error(path, "hlsl.additionalArguments", "an array of strings");
        }
        std::vector<std::string> values;
        values.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            if (!argument.is_string()) {
                throw_type_error(path, "hlsl.additionalArguments", "an array of strings");
            }
            values.push_back(argument.get<std::string>());
        }
        result.additional_arguments = std::move(values);
    }

    return result;
}

[[nodiscard]] std::vector<ConfigFile>
find_config_files(const std::filesystem::path& shader_directory) {
    std::error_code error;
    const auto absolute_directory = std::filesystem::absolute(shader_directory).lexically_normal();
    if (!std::filesystem::is_directory(absolute_directory, error) || error) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_directory,
                                 absolute_directory,
                                 {},
                                 "Shader directory '" + absolute_directory.string() +
                                     "' does not exist or is not a directory"};
    }

    std::vector<ConfigFile> result;
    auto directory = std::filesystem::weakly_canonical(absolute_directory);
    while (true) {
        const auto candidate = directory / configuration_file_name;
        if (std::filesystem::exists(candidate, error) && !error) {
            result.push_back(parse_config_file(candidate));
            if (result.back().root) {
                break;
            }
        } else if (error) {
            throw ConfigurationError{ConfigurationErrorCode::invalid_json,
                                     candidate,
                                     {},
                                     "Unable to inspect configuration file '" + candidate.string() +
                                         "'"};
        }

        const auto parent = directory.parent_path();
        if (parent == directory || parent.empty()) {
            break;
        }
        directory = parent;
    }
    return result;
}

} // namespace

ConfigurationError::ConfigurationError(ConfigurationErrorCode code, std::filesystem::path file,
                                       std::string key, std::string_view message)
    : std::runtime_error{std::string{message}}, code_{code}, file_{std::move(file)},
      key_{std::move(key)} {}

auto ConfigurationError::code() const noexcept -> ConfigurationErrorCode { return code_; }

auto ConfigurationError::file() const noexcept -> const std::filesystem::path& { return file_; }

auto ConfigurationError::key() const noexcept -> const std::string& { return key_; }

auto WorkspaceConfiguration::compiler_options() const -> dxc::CompilerOptions {
    dxc::CompilerOptions result;
    if (language_version) {
        result.language_version = *language_version;
    }
    if (target_profile) {
        result.target_profile = *target_profile;
    }
    if (entry_point) {
        result.entry_point = *entry_point;
    }

    result.defines.reserve(preprocessor_definitions.size());
    for (const auto& [name, value] : preprocessor_definitions) {
        auto define = name;
        if (!value.empty()) {
            define += '=';
            define += value;
        }
        result.defines.push_back(std::move(define));
    }
    result.include_directories.reserve(additional_include_directories.size());
    std::ranges::transform(additional_include_directories,
                           std::back_inserter(result.include_directories),
                           [](const auto& path) { return path.string(); });
    result.additional_arguments = additional_arguments;
    return result;
}

auto discover_configuration_files(const std::filesystem::path& shader_directory)
    -> std::vector<std::filesystem::path> {
    const auto configs = find_config_files(shader_directory);
    std::vector<std::filesystem::path> result;
    result.reserve(configs.size());
    std::ranges::transform(configs, std::back_inserter(result),
                           [](const auto& config) { return config.path; });
    return result;
}

auto load_workspace_configuration(const std::filesystem::path& shader_directory)
    -> WorkspaceConfiguration {
    const auto configs = find_config_files(shader_directory);
    WorkspaceConfiguration result;

    for (auto iterator = configs.rbegin(); iterator != configs.rend(); ++iterator) {
        for (const auto& [name, value] : iterator->definitions) {
            result.preprocessor_definitions[name] = value;
        }
        for (const auto& [virtual_directory, real_directory] : iterator->virtual_mappings) {
            result.virtual_directory_mappings[virtual_directory] = real_directory;
        }
        if (iterator->language_version) {
            result.language_version = iterator->language_version;
        }
        if (iterator->target_profile) {
            result.target_profile = iterator->target_profile;
        }
        if (iterator->entry_point) {
            result.entry_point = iterator->entry_point;
        }
        if (iterator->additional_arguments) {
            result.additional_arguments = *iterator->additional_arguments;
        }
    }

    std::set<std::filesystem::path> included;
    for (const auto& config : configs) {
        for (const auto& include_directory : config.include_directories) {
            if (included.insert(include_directory).second) {
                result.additional_include_directories.push_back(include_directory);
            }
        }
    }

    return result;
}

} // namespace hlsl_intellisense::workspace
