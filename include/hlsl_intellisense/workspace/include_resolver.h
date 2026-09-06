#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace hlsl_intellisense::workspace {

struct IncludeDirective {
    std::string path;
    std::size_t path_offset{};
    bool quoted{};
};

struct DynamicIncludeDirective {
    std::string expression;
    std::size_t expression_offset{};
};

struct IncludeMetadata {
    std::vector<IncludeDirective> directives;
    std::vector<DynamicIncludeDirective> dynamic_directives;
    bool has_dynamic{};
};

struct IncludeCacheLimits {
    std::size_t max_entries{512};
    std::size_t max_estimated_bytes{8U * 1024U * 1024U};
};

struct IncludeCacheMetrics {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
    std::size_t entries{};
    std::size_t estimated_bytes{};
};

class IncludeMetadataCache final {
  public:
    explicit IncludeMetadataCache(IncludeCacheLimits limits = {});
    IncludeMetadataCache(IncludeMetadataCache&&) noexcept;
    IncludeMetadataCache& operator=(IncludeMetadataCache&&) noexcept;
    IncludeMetadataCache(const IncludeMetadataCache&) = delete;
    IncludeMetadataCache& operator=(const IncludeMetadataCache&) = delete;
    ~IncludeMetadataCache();

    [[nodiscard]] IncludeMetadata get(std::string_view identity, std::string_view text);
    void invalidate(std::string_view identity);
    void clear() noexcept;
    [[nodiscard]] IncludeCacheMetrics metrics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

struct IncludeResolution {
    std::vector<dxc::SourceFile> sources;
    std::unordered_set<std::string> dependency_identities;
    bool has_dynamic_includes{};
    enum class Status : std::uint8_t { resolved, missing, cyclic, dynamic };

    struct Edge {
        std::string source_path;
        std::string requested_path;
        std::size_t path_offset{};
        bool quoted{};
        Status status{Status::missing};
        std::string resolved_path;
        std::string logical_path;
        std::string virtual_mapping;
    };

    struct File {
        std::string physical_path;
        std::string logical_path;
        bool open{};
        std::vector<Edge> includes;
        std::string source_text;
    };

    std::vector<File> files;
};

[[nodiscard]] IncludeResolution resolve_includes(const SourceSnapshot& root,
                                                 std::span<const SourceSnapshot> open_documents,
                                                 const WorkspaceConfiguration& configuration,
                                                 IncludeMetadataCache* cache = nullptr);

[[nodiscard]] std::optional<std::filesystem::path>
resolve_include_at(const SourceSnapshot& root, std::span<const SourceSnapshot> open_documents,
                   const WorkspaceConfiguration& configuration, std::size_t utf8_offset,
                   IncludeMetadataCache* cache = nullptr);

// Returns true when `utf8_offset` falls within an `#include` directive's path
// span in `text`, using only lightweight textual include-directive parsing:
// no `WorkspaceConfiguration` (and therefore no filesystem access) is
// required. Lets callers cheaply pre-filter candidates -- e.g. deciding
// whether a diagnostic is even structurally on an include path -- before
// paying for any configuration-dependent include resolution.
[[nodiscard]] bool is_include_directive_at(std::string_view text, std::size_t utf8_offset);

} // namespace hlsl_intellisense::workspace
