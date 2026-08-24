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

struct IncludeMetadata {
    std::vector<IncludeDirective> directives;
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
};

[[nodiscard]] IncludeResolution resolve_includes(const SourceSnapshot& root,
                                                 std::span<const SourceSnapshot> open_documents,
                                                 const WorkspaceConfiguration& configuration,
                                                 IncludeMetadataCache* cache = nullptr);

[[nodiscard]] std::optional<std::filesystem::path>
resolve_include_at(const SourceSnapshot& root, std::span<const SourceSnapshot> open_documents,
                   const WorkspaceConfiguration& configuration, std::size_t utf8_offset,
                   IncludeMetadataCache* cache = nullptr);

} // namespace hlsl_intellisense::workspace
