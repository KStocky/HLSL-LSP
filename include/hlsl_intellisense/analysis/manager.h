#pragma once

#include <hlsl_intellisense/analysis/scheduler.h>
#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>
#include <hlsl_intellisense/workspace/include_resolver.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

struct AnalysisBudgets {
    // Bounds initial and background parse/reparse work, including diagnostic
    // extraction. A timed-out worker is terminated rather than left wedged.
    std::chrono::milliseconds background_timeout{30'000};
    // Bounds all editor-facing DXC queries.
    std::chrono::milliseconds interactive_timeout{15'000};
};

struct AnalysisOptions {
    SchedulerOptions scheduler{};
    AnalysisLimits limits{};
    AnalysisBudgets budgets{};
    // Selects the process-wide DXC runtime loaded by analysis workers. Empty
    // selects the bundled default.
    dxc::RuntimeConfiguration runtime{};
    // Test/deployment seam. Empty selects hlsl-analysis-worker beside the
    // current executable.
    std::filesystem::path worker_executable{};
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

enum class AnalysisUnavailableReason : std::uint8_t {
    timed_out,
    worker_crashed,
    protocol_error,
    launch_failed,
    worker_error,
};

struct AnalysisUnavailable {
    AnalysisUnavailableReason reason{AnalysisUnavailableReason::worker_error};
    std::string message;
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
    // Test seam only: lets tests rewrite the diagnostics a real DXC parse
    // produced immediately before they are cached and published, to exercise
    // malformed/overlapping fix-it rejection in textDocument/codeAction that
    // pinned DXC 1.9.2607.13 was not empirically observed to produce on its
    // own. Never set in production.
    std::function<void(std::vector<dxc::Diagnostic>&)> after_diagnostics;
    // Test seam only: invoked by the call-hierarchy handlers
    // (`Server::prepare_call_hierarchy`, `call_hierarchy_outgoing_calls`,
    // `call_hierarchy_incoming_calls`) after a full response has been
    // constructed (which independently re-fetches live document text via
    // `Server::text_for_path` -- a plain, non-`Manager`-mediated read of
    // mutable server state, so it cannot itself observe or wait on an
    // in-flight reanalysis) but strictly before the final generation
    // recheck that guards its return. `Manager::query`'s own
    // `before_interactive` hook cannot reach this window, because
    // `text_for_path` never goes through `Manager::query` at all. Lets
    // tests deterministically land a concurrent reanalysis (e.g. an
    // included-file edit) exactly inside that window, to prove the final
    // recheck actually rejects a response that was constructed against a
    // now-superseded analysis rather than merely one requested against it.
    // Never set in production.
    std::function<void()> before_call_hierarchy_revalidation;
    // Test seam only: invoked by `Server::call_hierarchy_incoming_calls`
    // once per candidate root, at the very start of that root's own
    // iteration of the exhaustive `analysis_.roots()` loop -- strictly
    // before that root's *live* document snapshot is taken (i.e. before
    // `analyze_and_publish` and before that root's own `incoming_calls`
    // query). `analysis_.roots()` returns a point-in-time *copy* of every
    // root's metadata, taken once before the loop begins; this hook lets
    // tests deterministically land a concurrent edit to a specific
    // candidate root strictly between that metadata snapshot being taken
    // and this root's own turn in the loop, to prove the subsequent query
    // resolves against the root's *current* document state (picking up a
    // newly added dependency on the target) rather than being skipped, or
    // resolved stale, on the basis of the metadata snapshot's own
    // point-in-time content. Never set in production.
    std::function<void(std::string_view)> before_call_hierarchy_candidate_root;
    // Test seam only: invoked after an analysis result and its server-side
    // submission context have been captured, but before handlers return
    // them together. Lets tests supersede only the server submission while
    // preserving the Manager content generation, proving these two
    // generation namespaces are validated independently.
    std::function<void()> before_server_submission_revalidation;
};

// Pairs a query result with the content generation (see
// `Manager::content_generation`) the analysis was actually computed
// against, both read within the *same* serialized manager operation. Every
// call-hierarchy-shaped query that later needs to stamp its result with a
// generation for round-tripping (`CallHierarchyItem.data.generation`)
// returns this instead of making a second, separate `content_generation`
// call: two separate calls could straddle a concurrent reanalysis (an
// included file edit or a configuration/active-variant change can reparse
// a root without bumping its own document version), so a generation
// fetched a moment apart from the query it is meant to describe could
// silently describe a *different* analysis than the one that actually
// produced `value`. Reading both from inside one `Impl::query<T>`
// invocation makes that impossible: the same worker thread computes
// `value` and reads `generation` from the exact same `Impl::Entry`
// reference, with no reparse able to run in between.
template <typename T> struct WithGeneration {
    T value;
    std::uint64_t generation{};
};

class Manager final {
  public:
    using DiagnosticsHandler = std::function<void(
        const workspace::SourceSnapshot&, const std::vector<dxc::Diagnostic>&, std::uint64_t)>;
    using ErrorHandler = std::function<void(std::string_view)>;
    using UnavailableHandler = std::function<void(const workspace::SourceSnapshot&,
                                                  const AnalysisUnavailable&, std::uint64_t)>;

    explicit Manager(DiagnosticsHandler diagnostics, AnalysisOptions options = {},
                     std::shared_ptr<AnalysisHooks> hooks = {}, ErrorHandler errors = {},
                     UnavailableHandler unavailable = {});
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    ~Manager();

    bool analyze(AnalysisInput input);
    void after_roots_idle(std::vector<std::string> roots, std::function<void()> callback);
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
    // Compiles the actual root source and resolved in-memory includes with
    // the effective compiler arguments and returns the compiler-authoritative
    // configuration, output, and reflection for an open document. Unlike the
    // cursor-based queries above, this operates on the whole translation
    // unit rather than a position.
    [[nodiscard]] dxc::CompilationInfo
    compilation_info(std::string root_identity, std::int64_t version, std::string path,
                     const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] WithGeneration<dxc::CompilationInfo>
    compilation_info_with_generation(std::string root_identity, std::int64_t version,
                                     std::string path,
                                     const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Signature>
    signatures(std::string root_identity, std::int64_t version, std::string path,
               std::uint32_t line, std::uint32_t column,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::InlayHint>
    inlay_hints(std::string root_identity, std::int64_t version, std::string path,
                std::vector<dxc::SourceOffsetRange> ranges, std::vector<dxc::InlayCall> calls,
                dxc::InlayHintOptions options, const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Token> tokens(std::string root_identity, std::int64_t version,
                                                 std::string path,
                                                 const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::SourceRange>
    skipped_ranges(std::string root_identity, std::int64_t version,
                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::MacroDefinition>
    macro_definitions(std::string root_identity, std::int64_t version,
                      const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Symbol> symbols(std::string root_identity, std::int64_t version,
                                                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Symbol>
    document_symbols(std::string root_identity, std::int64_t version,
                     const json_rpc::CancellationToken& cancellation, bool& truncated);

    // Call-hierarchy queries: `path`/`line`/`column` are 0-based
    // client-supplied positions, resolved against the current translation
    // unit snapshot for `root_identity`/`version` (throws
    // json_rpc::HandlerError{content_modified_code, ...} via `query<T>()`
    // when `version` no longer matches the cached analysis, exactly like
    // every other position-based query above). Each returns its result
    // paired atomically with the content generation it was computed
    // against (see `WithGeneration`), so callers building a
    // `CallHierarchyItem.data.generation` never need a second, separately
    // timed `content_generation` call that could describe a different
    // analysis than the one that actually produced the result.
    [[nodiscard]] WithGeneration<std::optional<dxc::CallableSymbol>>
    callable_at(std::string root_identity, std::int64_t version, std::string path,
                std::uint32_t line, std::uint32_t column,
                const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] WithGeneration<std::vector<dxc::OutgoingCall>>
    outgoing_calls(std::string root_identity, std::int64_t version, std::string path,
                   std::uint32_t line, std::uint32_t column,
                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] WithGeneration<std::vector<dxc::IncomingCall>>
    incoming_calls(std::string root_identity, std::int64_t version, std::string path,
                   std::uint32_t line, std::uint32_t column,
                   const json_rpc::CancellationToken& cancellation);
    // Traces reachability and global/resource access from this root's own
    // configured entry point using the isolated worker's retained analysis.
    [[nodiscard]] WithGeneration<dxc::EntryPointDataFlow>
    entry_point_data_flow(std::string root_identity, std::int64_t version,
                          dxc::EntryPointDataFlowLimits limits,
                          const json_rpc::CancellationToken& cancellation);

    // The content generation currently cached for `root_identity`/`version`
    // (see analysis/manager.cpp's `Impl::Entry::generation` for exactly what
    // this counts). `CallHierarchyItem.data` pins this alongside
    // `rootVersion` so a later incomingCalls/outgoingCalls request can
    // detect an included-file edit or a configuration/active-variant change
    // that reparsed this root without changing its own document version.
    // Prefer the `WithGeneration`-returning queries above when a generation
    // is needed alongside another query's result on the same root/version:
    // this standalone accessor exists for cases (staleness pre-checks,
    // cross-root lookups where no other query is being made for that root)
    // that need a generation on its own, with no other data to pair it with.
    [[nodiscard]] std::uint64_t content_generation(std::string root_identity, std::int64_t version,
                                                   const json_rpc::CancellationToken& cancellation);
    // Re-resolves the callable at (path, line, column) within the root's
    // *current* translation unit and returns its content generation only
    // when that callable's own identity (start offset, cursor kind, name)
    // still matches what was captured when a CallHierarchyItem was built --
    // std::nullopt otherwise (nothing resolves there anymore, or a
    // different callable now does), so callers can reject with
    // ContentModified instead of silently resolving a different symbol.
    [[nodiscard]] std::optional<std::uint64_t>
    verify_call_hierarchy_identity(std::string root_identity, std::int64_t version,
                                   std::string path, std::uint32_t line, std::uint32_t column,
                                   std::uint32_t expected_start_offset,
                                   std::uint32_t expected_cursor_kind, std::string expected_name,
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
