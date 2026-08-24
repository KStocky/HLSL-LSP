#include <hlsl_intellisense/workspace/configuration.h>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace hlsl_intellisense::workspace {
namespace {

using Json = nlohmann::json;

struct ConfigurationSettings {
    std::map<std::string, std::string, std::less<>> definitions;
    std::vector<std::filesystem::path> include_directories;
    std::map<std::string, std::filesystem::path, std::less<>> virtual_mappings;
    std::optional<std::string> language_version;
    std::optional<std::string> target_profile;
    std::optional<std::string> entry_point;
    std::optional<std::vector<std::string>> additional_arguments;
};

struct GlobPattern {
    std::vector<std::string> segments;
    bool filename_only{};
};

struct FileGroup {
    std::vector<GlobPattern> patterns;
    ConfigurationSettings settings;
};

struct ConfigFile {
    std::filesystem::path path;
    bool root{};
    ConfigurationSettings settings;
    std::vector<FileGroup> file_groups;
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

[[nodiscard]] std::string contextual_key(std::string_view context, std::string_view key) {
    if (context.empty()) {
        return std::string{key};
    }
    return std::string{context} + "." + std::string{key};
}

[[nodiscard]] std::optional<std::string> optional_string(const Json& json,
                                                         const std::filesystem::path& path,
                                                         std::string_view key,
                                                         std::string_view context) {
    const auto iterator = json.find(key);
    if (iterator == json.end()) {
        return std::nullopt;
    }
    if (!iterator->is_string()) {
        throw_type_error(path, contextual_key(context, key), "a string");
    }
    return iterator->get<std::string>();
}

[[nodiscard]] ConfigurationSettings
parse_settings(const Json& json, const std::filesystem::path& path, std::string_view context) {
    ConfigurationSettings result;

    constexpr std::string_view definitions_key = "hlsl.preprocessorDefinitions";
    if (const auto definitions = json.find(definitions_key); definitions != json.end()) {
        const auto diagnostic_key = contextual_key(context, definitions_key);
        if (!definitions->is_object()) {
            throw_type_error(path, diagnostic_key, "an object");
        }
        for (const auto& [name, value] : definitions->items()) {
            if (name.empty()) {
                throw ConfigurationError{ConfigurationErrorCode::invalid_type, path, diagnostic_key,
                                         "Preprocessor definition names must not be empty"};
            }
            auto value_key = diagnostic_key;
            value_key.push_back('.');
            value_key += name;
            result.definitions.emplace(name, definition_value(path, value_key, value));
        }
    }

    constexpr std::string_view includes_key = "hlsl.additionalIncludeDirectories";
    if (const auto includes = json.find(includes_key); includes != json.end()) {
        const auto diagnostic_key = contextual_key(context, includes_key);
        if (!includes->is_array()) {
            throw_type_error(path, diagnostic_key, "an array of strings");
        }
        for (const auto& include : *includes) {
            if (!include.is_string()) {
                throw_type_error(path, diagnostic_key, "an array of strings");
            }
            result.include_directories.push_back(
                resolve_directory(path, diagnostic_key, include.get_ref<const std::string&>()));
        }
    }

    constexpr std::string_view mappings_key = "hlsl.virtualDirectoryMappings";
    if (const auto mappings = json.find(mappings_key); mappings != json.end()) {
        const auto diagnostic_key = contextual_key(context, mappings_key);
        if (!mappings->is_object()) {
            throw_type_error(path, diagnostic_key, "an object of string paths");
        }
        for (const auto& [virtual_directory, real_directory] : mappings->items()) {
            if (virtual_directory.empty() ||
                (virtual_directory.front() != '/' && virtual_directory.front() != '\\')) {
                throw ConfigurationError{ConfigurationErrorCode::invalid_virtual_directory, path,
                                         diagnostic_key,
                                         "Virtual directory '" + virtual_directory +
                                             "' must start with a forward slash or backslash"};
            }
            if (!real_directory.is_string()) {
                throw_type_error(path, diagnostic_key, "an object of string paths");
            }
            result.virtual_mappings.emplace(
                virtual_directory, resolve_directory(path, diagnostic_key,
                                                     real_directory.get_ref<const std::string&>()));
        }
    }

    result.language_version = optional_string(json, path, "hlsl.languageVersion", context);
    result.target_profile = optional_string(json, path, "hlsl.targetProfile", context);
    result.entry_point = optional_string(json, path, "hlsl.entryPoint", context);

    constexpr std::string_view arguments_key = "hlsl.additionalArguments";
    if (const auto arguments = json.find(arguments_key); arguments != json.end()) {
        const auto diagnostic_key = contextual_key(context, arguments_key);
        if (!arguments->is_array()) {
            throw_type_error(path, diagnostic_key, "an array of strings");
        }
        std::vector<std::string> values;
        values.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            if (!argument.is_string()) {
                throw_type_error(path, diagnostic_key, "an array of strings");
            }
            values.push_back(argument.get<std::string>());
        }
        result.additional_arguments = std::move(values);
    }

    return result;
}

[[noreturn]] void throw_glob_error(const std::filesystem::path& path, std::string_view key,
                                   std::string_view pattern, std::string_view reason) {
    throw ConfigurationError{ConfigurationErrorCode::invalid_glob, path, std::string{key},
                             "Invalid file glob '" + std::string{pattern} + "' configured by '" +
                                 std::string{key} + "': " + std::string{reason}};
}

[[nodiscard]] std::vector<std::string> split_path(std::string_view value) {
    std::vector<std::string> result;
    auto start = std::size_t{};
    while (start <= value.size()) {
        const auto separator = value.find('/', start);
        result.emplace_back(value.substr(
            start, separator == std::string_view::npos ? value.size() - start : separator - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return result;
}

[[nodiscard]] GlobPattern parse_glob(const std::filesystem::path& path, std::string_view key,
                                     std::string pattern) {
    if (pattern.empty()) {
        throw_glob_error(path, key, pattern, "patterns must not be empty");
    }
    if (pattern.find('\0') != std::string::npos) {
        throw_glob_error(path, key, pattern, "patterns must not contain null bytes");
    }
    if (pattern.find_first_of("[]") != std::string::npos) {
        throw_glob_error(path, key, pattern, "character classes are not supported");
    }

    std::ranges::replace(pattern, '\\', '/');
    if (pattern.front() == '/' ||
        (pattern.size() >= 2 && std::isalpha(static_cast<unsigned char>(pattern[0])) != 0 &&
         pattern[1] == ':')) {
        throw_glob_error(path, key, pattern, "patterns must be relative paths");
    }

    auto segments = split_path(pattern);
    for (const auto& segment : segments) {
        if (segment.empty()) {
            throw_glob_error(path, key, pattern, "path segments must not be empty");
        }
        if (segment == "." || segment == "..") {
            throw_glob_error(path, key, pattern,
                             "current-directory and parent-directory segments are not allowed");
        }
        if (segment != "**" && segment.find("**") != std::string::npos) {
            throw_glob_error(path, key, pattern, "'**' must occupy an entire path segment");
        }
    }

    return {.segments = std::move(segments),
            .filename_only = pattern.find('/') == std::string::npos};
}

[[nodiscard]] bool supported_group_key(std::string_view key) {
    constexpr std::array keys{
        std::string_view{"name"},
        std::string_view{"files"},
        std::string_view{"hlsl.preprocessorDefinitions"},
        std::string_view{"hlsl.additionalIncludeDirectories"},
        std::string_view{"hlsl.virtualDirectoryMappings"},
        std::string_view{"hlsl.languageVersion"},
        std::string_view{"hlsl.targetProfile"},
        std::string_view{"hlsl.entryPoint"},
        std::string_view{"hlsl.additionalArguments"},
    };
    return std::ranges::find(keys, key) != keys.end();
}

[[nodiscard]] std::vector<FileGroup> parse_file_groups(const Json& json,
                                                       const std::filesystem::path& path) {
    const auto groups = json.find("hlsl.fileGroups");
    if (groups == json.end()) {
        return {};
    }
    if (!groups->is_array()) {
        throw_type_error(path, "hlsl.fileGroups", "an array of objects");
    }

    std::vector<FileGroup> result;
    result.reserve(groups->size());
    for (std::size_t group_index = 0; group_index < groups->size(); ++group_index) {
        const auto& group = (*groups)[group_index];
        const auto group_key = "hlsl.fileGroups[" + std::to_string(group_index) + "]";
        if (!group.is_object()) {
            throw_type_error(path, group_key, "an object");
        }
        for (const auto& [key, value] : group.items()) {
            static_cast<void>(value);
            if (!supported_group_key(key)) {
                auto property_key = group_key;
                property_key.push_back('.');
                property_key += key;
                throw ConfigurationError{
                    ConfigurationErrorCode::invalid_type, path, std::move(property_key),
                    "Unsupported file-group property '" + key + "' in '" + path.string() + "'"};
            }
        }

        if (const auto name = group.find("name"); name != group.end() && !name->is_string()) {
            throw_type_error(path, group_key + ".name", "a string");
        }

        const auto files = group.find("files");
        if (files == group.end()) {
            throw ConfigurationError{
                ConfigurationErrorCode::invalid_type, path, group_key + ".files",
                "File group '" + group_key + "' must contain a non-empty files array"};
        }
        if (!files->is_array() || files->empty()) {
            throw_type_error(path, group_key + ".files", "a non-empty array of strings");
        }

        FileGroup parsed;
        parsed.patterns.reserve(files->size());
        for (std::size_t pattern_index = 0; pattern_index < files->size(); ++pattern_index) {
            const auto& pattern = (*files)[pattern_index];
            const auto pattern_key = group_key + ".files[" + std::to_string(pattern_index) + "]";
            if (!pattern.is_string()) {
                throw_type_error(path, pattern_key, "a string");
            }
            parsed.patterns.push_back(parse_glob(path, pattern_key, pattern.get<std::string>()));
        }
        parsed.settings = parse_settings(group, path, group_key);
        result.push_back(std::move(parsed));
    }
    return result;
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

    result.settings = parse_settings(json, path, {});
    result.file_groups = parse_file_groups(json, path);
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

struct GlobSegmentPattern {
    std::string_view value;
};

struct FilePathSegment {
    std::string_view value;
};

#ifdef _WIN32
[[nodiscard]] std::wstring utf8_to_utf16(std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error{"File-group path segment is too long"};
    }
    if (value.empty()) {
        return {};
    }

    const std::string input{value};
    const auto input_size = static_cast<int>(input.size());
    const auto output_size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), input_size, nullptr, 0);
    if (output_size == 0) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to decode a file-group path segment"};
    }

    std::wstring result(static_cast<std::size_t>(output_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), input_size, result.data(),
                            output_size) == 0) {
        throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to decode a file-group path segment"};
    }
    return result;
}

[[nodiscard]] std::size_t next_utf16_character(std::wstring_view value, std::size_t index) {
    const auto character = static_cast<std::uint16_t>(value[index]);
    if (character >= 0xD800 && character <= 0xDBFF && index + 1 < value.size()) {
        const auto next = static_cast<std::uint16_t>(value[index + 1]);
        if (next >= 0xDC00 && next <= 0xDFFF) {
            return index + 2;
        }
    }
    return index + 1;
}

[[nodiscard]] bool match_segment(GlobSegmentPattern pattern, FilePathSegment value) {
    const auto wide_pattern = utf8_to_utf16(pattern.value);
    const auto wide_value = utf8_to_utf16(value.value);
    std::size_t pattern_index{};
    std::size_t value_index{};
    auto star_index = std::string_view::npos;
    auto star_value_index = std::size_t{};

    while (value_index < wide_value.size()) {
        const auto pattern_character =
            pattern_index < wide_pattern.size() ? wide_pattern[pattern_index] : L'\0';
        bool equal{};
        auto next_pattern_index = pattern_index;
        if (pattern_character != L'\0' && pattern_character != L'?' && pattern_character != L'*') {
            next_pattern_index = next_utf16_character(wide_pattern, pattern_index);
            const auto next_value_index = next_utf16_character(wide_value, value_index);
            const auto comparison =
                CompareStringOrdinal(wide_pattern.data() + pattern_index,
                                     static_cast<int>(next_pattern_index - pattern_index),
                                     wide_value.data() + value_index,
                                     static_cast<int>(next_value_index - value_index), TRUE);
            if (comparison == 0) {
                throw std::system_error{static_cast<int>(GetLastError()), std::system_category(),
                                        "Unable to compare file-group path characters"};
            }
            equal = comparison == CSTR_EQUAL;
        }

        if (pattern_character == L'?' || equal) {
            pattern_index = pattern_character == L'?' ? pattern_index + 1 : next_pattern_index;
            value_index = next_utf16_character(wide_value, value_index);
        } else if (pattern_character == L'*') {
            star_index = pattern_index++;
            star_value_index = value_index;
        } else if (star_index != std::string_view::npos) {
            pattern_index = star_index + 1;
            star_value_index = next_utf16_character(wide_value, star_value_index);
            value_index = star_value_index;
        } else {
            return false;
        }
    }
    while (pattern_index < wide_pattern.size() && wide_pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == wide_pattern.size();
}
#else
[[nodiscard]] bool match_segment(GlobSegmentPattern pattern_value, FilePathSegment path_value) {
    const auto pattern = pattern_value.value;
    const auto value = path_value.value;
    const auto next_character = [](std::string_view text, std::size_t index) {
        ++index;
        while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
            ++index;
        }
        return index;
    };
    std::size_t pattern_index{};
    std::size_t value_index{};
    auto star_index = std::string_view::npos;
    auto star_value_index = std::size_t{};

    while (value_index < value.size()) {
        const auto pattern_character =
            pattern_index < pattern.size() ? pattern[pattern_index] : '\0';
        bool equal{};
        auto next_pattern_index = pattern_index;
        if (pattern_character != '\0' && pattern_character != '?' && pattern_character != '*') {
            next_pattern_index = next_character(pattern, pattern_index);
            const auto next_value_index = next_character(value, value_index);
            equal = pattern.substr(pattern_index, next_pattern_index - pattern_index) ==
                    value.substr(value_index, next_value_index - value_index);
        }

        if (pattern_character == '?' || equal) {
            pattern_index = pattern_character == '?' ? pattern_index + 1 : next_pattern_index;
            value_index = next_character(value, value_index);
        } else if (pattern_character == '*') {
            star_index = pattern_index++;
            star_value_index = value_index;
        } else if (star_index != std::string_view::npos) {
            pattern_index = star_index + 1;
            star_value_index = next_character(value, star_value_index);
            value_index = star_value_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}
#endif

[[nodiscard]] bool matches(const GlobPattern& pattern,
                           const std::vector<std::string>& path_segments) {
    if (pattern.filename_only) {
        return !path_segments.empty() &&
               match_segment({pattern.segments.front()}, {path_segments.back()});
    }

    std::vector<bool> matched(path_segments.size() + 1);
    matched.front() = true;
    for (const auto& pattern_segment : pattern.segments) {
        std::vector<bool> next(path_segments.size() + 1);
        if (pattern_segment == "**") {
            next.front() = matched.front();
            for (std::size_t index = 1; index < next.size(); ++index) {
                next[index] = matched[index] || next[index - 1];
            }
        } else {
            for (std::size_t index = 1; index < next.size(); ++index) {
                next[index] = matched[index - 1] &&
                              match_segment({pattern_segment}, {path_segments[index - 1]});
            }
        }
        matched = std::move(next);
    }
    return matched.back();
}

[[nodiscard]] bool matches(const FileGroup& group,
                           const std::filesystem::path& relative_shader_path) {
    const auto utf8_path = relative_shader_path.generic_u8string();
    auto normalized =
        std::string{reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size()};
    std::ranges::replace(normalized, '\\', '/');
    const auto path_segments = split_path(normalized);
    return std::ranges::any_of(group.patterns, [&path_segments](const auto& pattern) {
        return matches(pattern, path_segments);
    });
}

void apply_settings(WorkspaceConfiguration& result, const ConfigurationSettings& settings) {
    for (const auto& [name, value] : settings.definitions) {
        result.preprocessor_definitions[name] = value;
    }
    for (const auto& [virtual_directory, real_directory] : settings.virtual_mappings) {
        result.virtual_directory_mappings[virtual_directory] = real_directory;
    }
    if (settings.language_version) {
        result.language_version = settings.language_version;
    }
    if (settings.target_profile) {
        result.target_profile = settings.target_profile;
    }
    if (settings.entry_point) {
        result.entry_point = settings.entry_point;
    }
    if (settings.additional_arguments) {
        result.additional_arguments = *settings.additional_arguments;
    }
}

[[nodiscard]] WorkspaceConfiguration
merge_configurations(const std::vector<ConfigFile>& configs,
                     const std::optional<std::filesystem::path>& canonical_shader) {
    WorkspaceConfiguration result;
    std::vector<const ConfigurationSettings*> applied_settings;

    for (auto iterator = configs.rbegin(); iterator != configs.rend(); ++iterator) {
        apply_settings(result, iterator->settings);
        applied_settings.push_back(&iterator->settings);
    }

    if (canonical_shader) {
        for (auto iterator = configs.rbegin(); iterator != configs.rend(); ++iterator) {
            const auto relative_shader =
                canonical_shader->lexically_relative(iterator->path.parent_path());
            if (relative_shader.empty() || relative_shader.is_absolute() ||
                *relative_shader.begin() == "..") {
                continue;
            }
            for (const auto& group : iterator->file_groups) {
                if (matches(group, relative_shader)) {
                    apply_settings(result, group.settings);
                    applied_settings.push_back(&group.settings);
                }
            }
        }
    }

    std::set<std::filesystem::path> included;
    for (auto settings = applied_settings.rbegin(); settings != applied_settings.rend();
         ++settings) {
        for (const auto& include_directory : (*settings)->include_directories) {
            if (included.insert(include_directory).second) {
                result.additional_include_directories.push_back(include_directory);
            }
        }
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
    return merge_configurations(find_config_files(shader_directory), std::nullopt);
}

auto load_workspace_configuration_for_file(const std::filesystem::path& shader_file)
    -> WorkspaceConfiguration {
    const auto absolute_shader = std::filesystem::absolute(shader_file).lexically_normal();
    std::error_code error;
    if (std::filesystem::is_directory(absolute_shader, error)) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_directory,
                                 absolute_shader,
                                 {},
                                 "Shader path '" + absolute_shader.string() +
                                     "' identifies a directory rather than a file"};
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw ConfigurationError{ConfigurationErrorCode::invalid_directory,
                                 absolute_shader,
                                 {},
                                 "Unable to inspect shader path '" + absolute_shader.string() +
                                     "'"};
    }

    const auto shader_directory = absolute_shader.parent_path();
    const auto configs = find_config_files(shader_directory);
    const auto canonical_shader_directory = std::filesystem::weakly_canonical(shader_directory);
    const auto canonical_shader = canonical_shader_directory / absolute_shader.filename();
    return merge_configurations(configs, canonical_shader);
}

auto apply_configuration_overrides(WorkspaceConfiguration configuration,
                                   const ConfigurationOverrides& overrides,
                                   const std::filesystem::path& base_directory)
    -> WorkspaceConfiguration {
    const auto settings_path = base_directory / "<editor-settings>";
    if (overrides.preprocessor_definitions) {
        configuration.preprocessor_definitions = *overrides.preprocessor_definitions;
    }
    if (overrides.additional_include_directories) {
        configuration.additional_include_directories.clear();
        configuration.additional_include_directories.reserve(
            overrides.additional_include_directories->size());
        for (const auto& directory : *overrides.additional_include_directories) {
            configuration.additional_include_directories.push_back(resolve_directory(
                settings_path, "hlsl.additionalIncludeDirectories", directory.string()));
        }
    }
    if (overrides.virtual_directory_mappings) {
        configuration.virtual_directory_mappings.clear();
        for (const auto& [virtual_directory, real_directory] :
             *overrides.virtual_directory_mappings) {
            if (virtual_directory.empty() ||
                (virtual_directory.front() != '/' && virtual_directory.front() != '\\')) {
                throw ConfigurationError{ConfigurationErrorCode::invalid_virtual_directory,
                                         settings_path, "hlsl.virtualDirectoryMappings",
                                         "Virtual directory '" + virtual_directory +
                                             "' must start with a forward slash or backslash"};
            }
            configuration.virtual_directory_mappings.emplace(
                virtual_directory, resolve_directory(settings_path, "hlsl.virtualDirectoryMappings",
                                                     real_directory.string()));
        }
    }
    if (overrides.language_version) {
        configuration.language_version = *overrides.language_version;
    }
    if (overrides.target_profile) {
        configuration.target_profile = *overrides.target_profile;
    }
    if (overrides.entry_point) {
        configuration.entry_point = *overrides.entry_point;
    }
    if (overrides.additional_arguments) {
        configuration.additional_arguments = *overrides.additional_arguments;
    }
    return configuration;
}

} // namespace hlsl_intellisense::workspace
