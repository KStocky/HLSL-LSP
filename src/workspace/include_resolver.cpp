#include <hlsl_intellisense/workspace/include_resolver.h>

#include <hlsl_intellisense/workspace/document_uri.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace hlsl_intellisense::workspace {
namespace {

struct IncludeDirective {
    std::string path;
    bool quoted{};
};

struct ParsedIncludes {
    std::vector<IncludeDirective> directives;
    bool has_dynamic{};
};

struct SourceNode {
    std::filesystem::path physical_path;
    std::string logical_path;
    std::string text;
    bool virtual_path{};
};

[[nodiscard]] std::string_view trim_left(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] ParsedIncludes parse_includes(std::string_view text) {
    ParsedIncludes result;
    while (!text.empty()) {
        const auto line_end = text.find_first_of("\r\n");
        auto line = trim_left(text.substr(0, line_end));
        if (line.starts_with('#')) {
            line = trim_left(line.substr(1));
            constexpr std::string_view keyword = "include";
            if (line.starts_with(keyword)) {
                line = trim_left(line.substr(keyword.size()));
                if (!line.empty() && (line.front() == '"' || line.front() == '<')) {
                    const auto terminator = line.front() == '"' ? '"' : '>';
                    const auto end = line.find(terminator, 1);
                    if (end != std::string_view::npos) {
                        result.directives.push_back(
                            {std::string{line.substr(1, end - 1)}, line.front() == '"'});
                    }
                } else if (!line.empty()) {
                    result.has_dynamic = true;
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        const auto next = line_end + (text[line_end] == '\r' && line_end + 1 < text.size() &&
                                              text[line_end + 1] == '\n'
                                          ? 2
                                          : 1);
        text.remove_prefix(next);
    }
    return result;
}

[[nodiscard]] std::filesystem::path normalized_physical_path(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

[[nodiscard]] std::string physical_identity(const std::filesystem::path& path) {
    return DocumentUri::from_path(normalized_physical_path(path).string()).identity();
}

[[nodiscard]] std::string normalized_virtual_path(std::string_view path) {
    std::string portable_path{path};
    std::ranges::replace(portable_path, '\\', '/');
    auto result = std::filesystem::path{portable_path}.lexically_normal().generic_string();
    if (!result.starts_with('/')) {
        result.insert(result.begin(), '/');
    }
    return result;
}

[[nodiscard]] bool virtual_prefix_matches(std::string_view path, std::string_view prefix) {
    return path == prefix ||
           (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

class Resolver final {
  public:
    Resolver(std::span<const SourceSnapshot> open_documents,
             const WorkspaceConfiguration& configuration)
        : configuration_{configuration} {
        for (const auto& document : open_documents) {
            open_documents_.emplace(
                document.document_uri().identity(),
                SourceNode{.physical_path = normalized_physical_path(document.path()),
                           .logical_path =
                               normalized_physical_path(document.path()).generic_string(),
                           .text = document.text(),
                           .virtual_path = false});
        }
    }

    [[nodiscard]] IncludeResolution resolve(const SourceSnapshot& root) {
        IncludeResolution result;
        SourceNode root_node{.physical_path = normalized_physical_path(root.path()),
                             .logical_path = normalized_physical_path(root.path()).generic_string(),
                             .text = root.text(),
                             .virtual_path = false};
        visit(root_node, result, true);
        return result;
    }

  private:
    [[nodiscard]] std::optional<std::string>
    source_text(const std::filesystem::path& physical_path) const {
        const auto open = open_documents_.find(physical_identity(physical_path));
        if (open != open_documents_.end()) {
            return open->second.text;
        }

        std::ifstream stream{physical_path, std::ios::binary};
        if (!stream) {
            return std::nullopt;
        }
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] std::optional<SourceNode> virtual_source(std::string_view include_path,
                                                           IncludeResolution& result) const {
        const auto normalized_include = normalized_virtual_path(include_path);
        const std::pair<std::string, std::filesystem::path>* best = nullptr;
        std::pair<std::string, std::filesystem::path> candidate;
        for (const auto& [configured_prefix, physical_directory] :
             configuration_.virtual_directory_mappings) {
            auto prefix = normalized_virtual_path(configured_prefix);
            if (virtual_prefix_matches(normalized_include, prefix) &&
                (best == nullptr || prefix.size() > best->first.size())) {
                candidate = {std::move(prefix), physical_directory};
                best = &candidate;
            }
        }
        if (best == nullptr) {
            return std::nullopt;
        }

        auto suffix = normalized_include.substr(best->first.size());
        while (!suffix.empty() && suffix.front() == '/') {
            suffix.erase(suffix.begin());
        }
        const auto physical_path =
            normalized_physical_path(best->second / std::filesystem::path{suffix});
        result.dependency_identities.insert(physical_identity(physical_path));
        auto text = source_text(physical_path);
        if (!text) {
            return std::nullopt;
        }
        return SourceNode{.physical_path = physical_path,
                          .logical_path = normalized_include,
                          .text = std::move(*text),
                          .virtual_path = true};
    }

    [[nodiscard]] std::optional<SourceNode>
    physical_source(const std::filesystem::path& physical_path, IncludeResolution& result,
                    const std::optional<std::string>& logical_path = std::nullopt) const {
        const auto normalized = normalized_physical_path(physical_path);
        result.dependency_identities.insert(physical_identity(normalized));
        auto text = source_text(normalized);
        if (!text) {
            return std::nullopt;
        }
        return SourceNode{.physical_path = normalized,
                          .logical_path = logical_path.value_or(normalized.generic_string()),
                          .text = std::move(*text),
                          .virtual_path = logical_path.has_value()};
    }

    [[nodiscard]] std::optional<SourceNode> resolve_directive(const SourceNode& source,
                                                              const IncludeDirective& directive,
                                                              IncludeResolution& result) const {
        if (directive.path.starts_with('/') || directive.path.starts_with('\\')) {
            if (auto resolved = virtual_source(directive.path, result)) {
                return resolved;
            }
        }

        if (directive.quoted) {
            const auto physical_path = source.physical_path.parent_path() / directive.path;
            if (source.virtual_path) {
                const auto logical_path = normalized_virtual_path(
                    std::filesystem::path{source.logical_path}.parent_path().generic_string() +
                    "/" + directive.path);
                if (auto resolved = physical_source(physical_path, result, logical_path)) {
                    return resolved;
                }
            } else if (auto resolved = physical_source(physical_path, result)) {
                return resolved;
            }
        }

        for (const auto& include_directory : configuration_.additional_include_directories) {
            if (auto resolved = physical_source(include_directory / directive.path, result)) {
                return resolved;
            }
        }
        return std::nullopt;
    }

    void visit(const SourceNode& source, IncludeResolution& result, bool root) {
        const auto identity = physical_identity(source.physical_path);
        if (!emitted_logical_paths_.insert(source.logical_path).second) {
            return;
        }
        result.sources.push_back({source.logical_path, source.text});
        if (!root) {
            result.dependency_identities.insert(identity);
        }
        if (!visited_physical_paths_.insert(identity).second) {
            return;
        }

        const auto parsed = parse_includes(source.text);
        if (parsed.has_dynamic) {
            result.has_dynamic_includes = true;
            for (const auto& [open_identity, open_document] : open_documents_) {
                if (open_identity != identity) {
                    result.dependency_identities.insert(open_identity);
                    if (emitted_logical_paths_.insert(open_document.logical_path).second) {
                        result.sources.push_back({open_document.logical_path, open_document.text});
                    }
                }
            }
        }
        for (const auto& directive : parsed.directives) {
            if (auto included = resolve_directive(source, directive, result)) {
                visit(*included, result, false);
            }
        }
    }

    const WorkspaceConfiguration& configuration_;
    std::unordered_map<std::string, SourceNode> open_documents_;
    std::unordered_set<std::string> emitted_logical_paths_;
    std::unordered_set<std::string> visited_physical_paths_;
};

} // namespace

IncludeResolution resolve_includes(const SourceSnapshot& root,
                                   std::span<const SourceSnapshot> open_documents,
                                   const WorkspaceConfiguration& configuration) {
    return Resolver{open_documents, configuration}.resolve(root);
}

} // namespace hlsl_intellisense::workspace
