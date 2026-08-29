#pragma once

#include <hlsl_intellisense/analysis/scheduler.h>
#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>
#include <hlsl_intellisense/workspace/include_resolver.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hlsl_intellisense::analysis {

struct AnalysisLimits {
    std::size_t max_translation_units{16};
    std::size_t max_translation_unit_estimated_bytes{256U * 1024U * 1024U};
    std::size_t opaque_translation_unit_estimate{4U * 1024U * 1024U};
    workspace::IncludeCacheLimits include_cache{};
};

struct AnalysisOptions {
    SchedulerOptions scheduler{};
    AnalysisLimits limits{};
    // Selects the process-wide DXC runtime loaded by analysis workers. Empty
    // selects the bundled default.
    dxc::RuntimeConfiguration runtime{};
};

struct AnalysisMetrics {
    SchedulerMetrics scheduler;
    std::uint64_t parse_count{};
    std::uint64_t reparse_count{};
    std::uint64_t cache_hits{};
    std::uint64_t cache_misses{};
    std::uint64_t cache_evictions{};
    std::uint64_t completion_count{};
    std::uint64_t parse_microseconds{};
    std::uint64_t reparse_microseconds{};
    std::uint64_t completion_microseconds{};
    std::size_t translation_units{};
    std::size_t translation_unit_estimated_bytes{};
    workspace::IncludeCacheMetrics include_cache;
};

struct AnalysisInput {
    workspace::SourceSnapshot root;
    std::vector<workspace::SourceSnapshot> open_documents;
    workspace::WorkspaceConfiguration configuration;
    std::uint64_t generation{};
};

struct RootMetadata {
    std::string root_uri;
    std::string root_identity;
    std::int64_t version{};
    std::string configuration_fingerprint;
    std::unordered_set<std::string> dependency_identities;
    bool has_dynamic_includes{};
};

struct AnalysisHooks {
    std::function<void(std::string_view, std::int64_t)> before_analysis;
    std::function<void(std::string_view)> before_interactive;
};

class Manager final {
  public:
    using DiagnosticsHandler = std::function<void(
        const workspace::SourceSnapshot&, const std::vector<dxc::Diagnostic>&, std::uint64_t)>;
    using ErrorHandler = std::function<void(std::string_view)>;

    explicit Manager(DiagnosticsHandler diagnostics, AnalysisOptions options = {},
                     std::shared_ptr<AnalysisHooks> hooks = {}, ErrorHandler errors = {});
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    ~Manager();

    void analyze(AnalysisInput input);
    void erase(std::string_view root_identity);
    void invalidate_include_metadata(const std::unordered_set<std::string>& identities);
    void wait_idle();
    void shutdown();

    [[nodiscard]] std::vector<dxc::Completion>
    complete(std::string root_identity, std::int64_t version, std::string path, std::uint32_t line,
             std::uint32_t column, const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::Definition>
    definition(std::string root_identity, std::int64_t version, std::string path,
               std::uint32_t line, std::uint32_t column,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Reference>
    references(std::string root_identity, std::int64_t version, std::string path,
               std::uint32_t line, std::uint32_t column,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::Hover> hover(std::string root_identity, std::int64_t version,
                                                  std::string path, std::uint32_t line,
                                                  std::uint32_t column,
                                                  const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::MemoryLayout>
    memory_layout(std::string root_identity, std::int64_t version, std::string path,
                  std::uint32_t line, std::uint32_t column,
                  const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Signature>
    signatures(std::string root_identity, std::int64_t version, std::string path,
               std::uint32_t line, std::uint32_t column,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Token> tokens(std::string root_identity, std::int64_t version,
                                                 std::string path,
                                                 const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Symbol> symbols(std::string root_identity, std::int64_t version,
                                                   const json_rpc::CancellationToken& cancellation);

    [[nodiscard]] std::vector<RootMetadata> roots() const;
    [[nodiscard]] std::vector<std::string>
    dependent_root_uris(const std::unordered_set<std::string>& changed_identities,
                        std::string_view except_root = {}) const;
    [[nodiscard]] AnalysisMetrics metrics() const noexcept;
    // Reports the DXC runtime the workers load, for client and server
    // diagnostics. Loading and validating the runtime can throw dxc::RuntimeError.
    [[nodiscard]] dxc::RuntimeInfo dxc_runtime_info() const;
    [[nodiscard]] static std::string
    configuration_fingerprint(const workspace::WorkspaceConfiguration& configuration);

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace hlsl_intellisense::analysis
