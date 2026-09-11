#pragma once

#include <hlsl_intellisense/analysis/manager.h>
#include <hlsl_intellisense/json_rpc/dispatcher.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hlsl_intellisense::lsp {

struct ServerOptions {
    bool semantic_tokens{true};
    bool background_analysis{};
    bool protocol_trace{};
    bool trace_source{};
    std::size_t request_worker_count{4};
    std::size_t request_queue_capacity{64};
    analysis::AnalysisOptions analysis{};
    std::shared_ptr<analysis::AnalysisHooks> analysis_hooks;
};

struct InlayHintSettings {
    bool types{true};
    bool parameters{true};
    bool matrix_orientation{};
    bool registers{};
    bool packed_offsets{};
    bool array_strides{};
    bool active_variant{true};

    bool operator==(const InlayHintSettings&) const = default;
};

struct EffectiveContextOrigin {
    std::string label;
    std::string setting;
    std::optional<std::filesystem::path> file;
};

struct EffectiveShaderContext {
    std::string document_uri;
    std::string file;
    std::optional<std::string> active_variant;
    std::string entry_point;
    std::string target_profile;
    std::optional<EffectiveContextOrigin> variant_origin;
    std::optional<EffectiveContextOrigin> entry_point_origin;
    std::optional<EffectiveContextOrigin> target_profile_origin;
};

class Server final {
  public:
    using NotificationSender = std::function<void(const json_rpc::Notification&)>;
    using RequestSender = std::function<void(const json_rpc::Request&)>;
    using Logger = std::function<void(std::string_view)>;

    explicit Server(NotificationSender sender, Logger logger = {}, ServerOptions options = {},
                    RequestSender request_sender = {});
    ~Server();

    [[nodiscard]] std::optional<json_rpc::DispatchResponse>
    handle(const json_rpc::Message& message);
    [[nodiscard]] json_rpc::DispatchResponse
    handle(const json_rpc::Request& request, const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] json_rpc::CancellationToken begin_request(const json_rpc::RequestId& id) const;
    void finish_request(const json_rpc::RequestId& id,
                        const json_rpc::CancellationToken& cancellation) const noexcept;
    void cancel_all_requests() const noexcept;
    void wait_for_analysis();
    [[nodiscard]] analysis::AnalysisMetrics analysis_metrics() const noexcept;
    [[nodiscard]] bool exit_requested() const noexcept;
    [[nodiscard]] int exit_code() const noexcept;

  private:
    struct ReferenceResult;

    // The canonical identity/version envelope round-tripped through
    // CallHierarchyItem.data: `root_uri`/`root_identity`/`root_version`/
    // `generation` pin the exact compiled analysis this item's callable was
    // resolved from (the translation unit whose body/definitions the item
    // remains valid against -- `generation` additionally catches an
    // included-file edit or a configuration/active-variant change that
    // reparsed this root without changing `root_version`, see
    // `analysis::Manager::content_generation`), and
    // `path`/`line`/`column`/`cursor_kind`/`start_offset`/`name` identify
    // the callable itself within that translation unit -- the same (path,
    // start_offset, cursor_kind) triple `dxc::CallableSymbol` already uses
    // for identity, so an overloaded function's item can never be confused
    // with a sibling overload at the same name. A later
    // incomingCalls/outgoingCalls request is rejected with ContentModified
    // (rather than silently resolving whatever now happens to be at that
    // position) whenever the named root document is no longer open at
    // exactly `root_version`, whenever the root's current content
    // generation no longer matches `generation`, or whenever re-resolving
    // `path`/`line`/`column` in the current analysis no longer yields a
    // callable whose own identity (start offset, cursor kind, name)
    // matches what was captured here.
    struct CallHierarchyItemData {
        std::string root_uri;
        std::string root_identity;
        std::int64_t root_version{};
        std::uint64_t generation{};
        json_rpc::Json context;
        std::string path;
        std::uint32_t line{};
        std::uint32_t column{};
        std::uint32_t start_offset{};
        std::uint32_t cursor_kind{};
        std::string name;
    };
    [[nodiscard]] static CallHierarchyItemData
    parse_call_hierarchy_item_data(const json_rpc::Json& item);
    [[nodiscard]] json_rpc::Json
    call_hierarchy_item(const dxc::CallableSymbol& callable, const std::string& root_uri,
                        const std::string& root_identity, std::int64_t root_version,
                        std::uint64_t root_generation, const json_rpc::Json& context) const;
    // Re-validates a previously built CallHierarchyItemData against the
    // *current* analysis of its own root: throws
    // json_rpc::HandlerError{content_modified_code, ...} when the root is
    // no longer open at `data.root_version`, when the root's current
    // content generation no longer matches `data.generation`, or when
    // re-resolving `data.path`/`data.line`/`data.column` no longer yields a
    // callable matching `data.start_offset`/`data.cursor_kind`/`data.name`
    // -- otherwise returns the confirmed-current generation (identical to
    // `data.generation` when this passes, returned so callers building
    // further CallHierarchyItems for the *same* root do not need a second
    // round trip to fetch it again).
    [[nodiscard]] std::uint64_t
    validate_call_hierarchy_item(const CallHierarchyItemData& data,
                                 const json_rpc::CancellationToken& cancellation);
    // Fetches `path`'s current text: the open document's own in-memory
    // snapshot when it is open (reflecting unsaved edits), otherwise its
    // on-disk content, mirroring how `definition()` already resolves a
    // cross-file target's text.
    [[nodiscard]] std::string text_for_path(std::string_view path) const;
    // A navigation-ready {name, kind, uri, range, selectionRange} JSON
    // object shared by hlsl/entryPointDataFlow's function and
    // global/resource entries (which, unlike CallHierarchyItem, carry no
    // round-trippable `data`: entry-point data flow is a single, self
    // contained snapshot of the whole reachability graph, not a node the
    // client is expected to expand incrementally).
    [[nodiscard]] json_rpc::Json navigable_json(std::string_view name, std::uint32_t cursor_kind,
                                                const dxc::SourceLocation& location,
                                                std::uint32_t start_offset,
                                                std::uint32_t end_offset) const;

    // The diagnostics last published for a document, alongside the exact
    // snapshot version and analysis generation they were computed from.
    // textDocument/codeAction requires an exact match against the requesting
    // document's current version before deriving any action from this cache,
    // so a document that has changed since its last analysis (or has none yet)
    // never offers fixes for stale content.
    struct DiagnosticsRecord {
        std::int64_t version{};
        std::uint64_t generation{};
        std::vector<dxc::Diagnostic> diagnostics;
    };

    enum class State { uninitialized, awaiting_initialized, running, shutdown };

    void register_handlers();
    [[nodiscard]] json_rpc::Json initialize(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json shutdown(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json completion(const std::optional<json_rpc::Json>& params,
                                            const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json definition(const std::optional<json_rpc::Json>& params,
                                            const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json references(const std::optional<json_rpc::Json>& params,
                                            const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json prepare_call_hierarchy(const std::optional<json_rpc::Json>& params,
                                                        const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json
    call_hierarchy_incoming_calls(const std::optional<json_rpc::Json>& params,
                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json
    call_hierarchy_outgoing_calls(const std::optional<json_rpc::Json>& params,
                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json entry_point_data_flow(const std::optional<json_rpc::Json>& params,
                                                       const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json prepare_rename(const std::optional<json_rpc::Json>& params,
                                                const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json rename(const std::optional<json_rpc::Json>& params,
                                        const json_rpc::RequestContext& context);
    [[nodiscard]] ReferenceResult find_references(std::string_view uri,
                                                  const workspace::Position& position,
                                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json hover(const std::optional<json_rpc::Json>& params,
                                       const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json memory_layout(const std::optional<json_rpc::Json>& params,
                                               const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json command_context(const std::optional<json_rpc::Json>& params,
                                                 const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json compilation_info(const std::optional<json_rpc::Json>& params,
                                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json effective_context(const std::optional<json_rpc::Json>& params,
                                                   const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json compute_visualization(const std::optional<json_rpc::Json>& params,
                                                       const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json preprocessor_explorer(const std::optional<json_rpc::Json>& params,
                                                       const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json signature_help(const std::optional<json_rpc::Json>& params,
                                                const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json inlay_hints(const std::optional<json_rpc::Json>& params,
                                             const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json document_symbols(const std::optional<json_rpc::Json>& params,
                                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json workspace_symbols(const std::optional<json_rpc::Json>& params,
                                                   const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json semantic_tokens(const std::optional<json_rpc::Json>& params,
                                                 const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json dxc_runtime(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json variants(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json code_action(const std::optional<json_rpc::Json>& params,
                                             const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json execute_command(const std::optional<json_rpc::Json>& params);
    void initialized(const std::optional<json_rpc::Json>& params);
    void did_open(const std::optional<json_rpc::Json>& params);
    void did_change(const std::optional<json_rpc::Json>& params);
    void did_save(const std::optional<json_rpc::Json>& params);
    void did_close(const std::optional<json_rpc::Json>& params);
    void did_change_configuration(const std::optional<json_rpc::Json>& params);
    void did_change_client_defaults(const std::optional<json_rpc::Json>& params);
    void did_change_active_variant(const std::optional<json_rpc::Json>& params);
    void did_change_workspace_folders(const std::optional<json_rpc::Json>& params);
    void did_change_watched_files(const std::optional<json_rpc::Json>& params);
    void exit(const std::optional<json_rpc::Json>& params);
    void analyze_affected(std::string_view uri);
    struct AnalysisSubmission {
        std::uint64_t generation{};
        std::optional<std::string> active_variant;
        std::string entry_point;
        std::string target_profile;
        EffectiveShaderContext context;
    };

    AnalysisSubmission analyze_and_publish(std::string_view uri);
    void require_current_submission(std::string_view root_identity, std::uint64_t generation,
                                    std::string_view message) const;
    void require_current_submission(std::string_view root_identity, std::uint64_t generation,
                                    const json_rpc::Json& context, std::string_view message) const;
    void reanalyze_all();
    // Compares the DXC runtime selected by editor settings and shadertoolsconfig
    // against the runtime this process loaded. A valid, different selection
    // triggers a single controlled-restart request; invalid or conflicting
    // selections are reported without a restart so no restart loop can form.
    void reevaluate_runtime_selection();
    // Checks whether the active variant is defined, applicable, and free of
    // schema errors for the open documents. Problems surface as a single
    // deduplicated window/showMessage; a variant change reanalyzes rather than
    // restarts, so this never triggers a restart on its own.
    void reevaluate_variant_selection();
    void invalidate_inlay_hints(bool refresh);
    void request_inlay_hint_refresh();
    void finalize_inlay_hint_refresh() noexcept;
    [[nodiscard]] std::string loaded_runtime_directory() const;
    void analysis_completed(const workspace::SourceSnapshot& snapshot,
                            const std::vector<dxc::Diagnostic>& diagnostics,
                            std::uint64_t generation);
    void analysis_unavailable(const workspace::SourceSnapshot& snapshot,
                              const analysis::AnalysisUnavailable& unavailable,
                              std::uint64_t generation);
    // A narrow, point-in-time copy of exactly the server state that
    // configuration resolution depends on, snapshotted under a single brief
    // state_mutex_ lock. The static configuration_for/variant_configuration_for
    // overloads below take this by value so that (possibly disk-I/O-bound)
    // configuration computation for a whole request can happen without
    // holding state_mutex_ for its duration.
    struct ConfigurationState {
        workspace::ConfigurationOverrides editor_settings;
        std::optional<std::string> client_default_language_version;
        std::optional<std::string> active_variant;
        std::unordered_map<std::string, std::filesystem::path> workspace_folders;
    };
    [[nodiscard]] ConfigurationState snapshot_configuration_state() const;
    [[nodiscard]] workspace::WorkspaceConfiguration
    base_configuration_for(const workspace::SourceSnapshot& snapshot) const;
    // Same computation, but taking the client default language version
    // explicitly instead of reading client_default_language_version_, so it
    // can be called against a state already snapshotted outside the lock.
    [[nodiscard]] static workspace::WorkspaceConfiguration
    base_configuration_for(const workspace::SourceSnapshot& snapshot,
                           const std::optional<std::string>& client_default_language_version);
    [[nodiscard]] workspace::WorkspaceConfiguration
    configuration_for(const workspace::SourceSnapshot& snapshot,
                      const workspace::ConfigurationOverrides& overrides) const;
    // Explicit-state overload of configuration_for; see ConfigurationState.
    [[nodiscard]] static workspace::WorkspaceConfiguration
    configuration_for(const workspace::SourceSnapshot& snapshot, const ConfigurationState& state,
                      workspace::VariantSelection* active_variant_selection = nullptr);
    [[nodiscard]] static EffectiveShaderContext
    effective_context_for(const workspace::SourceSnapshot& snapshot,
                          const ConfigurationState& state,
                          const workspace::WorkspaceConfiguration& configuration,
                          workspace::VariantSelection active_variant_selection);
    // Resolves the configuration that would be active if `variant_name` were
    // selected instead of (or in addition to, if none is currently active) the
    // active variant, applied on top of the same file-derived base and editor
    // overrides `configuration_for` uses. Lets code actions probe "would
    // switching variants fix this?" using the exact same structured
    // include/configuration machinery as real analysis, without mutating
    // server state.
    [[nodiscard]] workspace::WorkspaceConfiguration
    variant_configuration_for(const workspace::SourceSnapshot& snapshot,
                              std::string_view variant_name,
                              const workspace::ConfigurationOverrides& overrides) const;
    // Explicit-state overload of variant_configuration_for; see ConfigurationState.
    [[nodiscard]] static workspace::WorkspaceConfiguration
    variant_configuration_for(const workspace::SourceSnapshot& snapshot,
                              std::string_view variant_name, const ConfigurationState& state);
    [[nodiscard]] std::filesystem::path
    configuration_base_directory(std::string_view shader_path) const;
    [[nodiscard]] static std::filesystem::path configuration_base_directory(
        std::string_view shader_path,
        const std::unordered_map<std::string, std::filesystem::path>& workspace_folders);
    void publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                             const std::vector<dxc::Diagnostic>& diagnostics,
                             std::uint64_t generation);
    // Builds a quickfix CodeAction from a diagnostic's DXC fix-its, revalidating
    // every fix-it's byte range and replacement text against `snapshot`'s
    // current content. Returns nullopt if any fix-it in the diagnostic is
    // malformed, stale, cross-file, or overlaps another fix-it in the same
    // diagnostic: a multi-edit fix is never partially offered.
    [[nodiscard]] std::optional<json_rpc::Json>
    fix_it_code_action(const workspace::SourceSnapshot& snapshot, const dxc::Diagnostic& diagnostic,
                       const json_rpc::Json& diagnostic_item) const;
    // Offers a deterministic "select shader variant" command action when a
    // diagnostic's location falls exactly on an #include directive's path that
    // fails to resolve under the active configuration but is proven (via the
    // same structured include-resolution machinery real analysis uses) to
    // resolve under an inactive, applicable variant already declared for this
    // shader. Never fabricates include paths and never parses diagnostic text.
    // `active_configuration` and `variant_configurations` (a cache keyed by
    // variant name, populated lazily as variants are probed) are computed once
    // per code_action request by the caller, not reloaded per diagnostic.
    [[nodiscard]] static std::optional<json_rpc::Json> include_recovery_action(
        const workspace::SourceSnapshot& snapshot,
        const std::vector<workspace::SourceSnapshot>& open_documents,
        const dxc::Diagnostic& diagnostic, const json_rpc::Json& diagnostic_item,
        const workspace::WorkspaceConfiguration& active_configuration,
        const std::optional<std::string>& active_variant,
        const std::function<const workspace::WorkspaceConfiguration&(std::string_view)>&
            variant_configuration_for_name);
    void require_running() const;
    void log(std::string_view message) const;

    json_rpc::Dispatcher dispatcher_;
    workspace::DocumentStore documents_;
    std::unordered_map<std::string, std::filesystem::path> workspace_folders_;
    workspace::ConfigurationOverrides editor_settings_;
    std::optional<std::string> client_default_language_version_;
    // The compilation variant the editor has selected as active, applied to each
    // open document for which it is defined and applicable. Empty selects the
    // file-derived configuration with no variant.
    std::optional<std::string> active_variant_;
    InlayHintSettings inlay_hint_settings_;
    std::uint64_t inlay_hint_generation_{};
    std::atomic<std::int64_t> next_outbound_request_id_{1};
    bool client_inlay_hint_refresh_{};
    NotificationSender sender_;
    RequestSender request_sender_;
    Logger logger_;
    ServerOptions options_;
    analysis::Manager analysis_;
    std::mutex analysis_submission_mutex_;
    mutable std::mutex state_mutex_;
    std::unordered_map<std::string, std::string> configuration_watch_states_;
    std::uint64_t effective_context_revision_{};
    std::unordered_map<std::string, std::uint64_t> analysis_generations_;
    std::unordered_map<std::string, AnalysisSubmission> analysis_submissions_;
    // The diagnostics last published for each document (keyed by document
    // identity), used exclusively to derive textDocument/codeAction results.
    // Never populated from, or trusted against, a client-supplied payload.
    std::unordered_map<std::string, DiagnosticsRecord> diagnostics_by_identity_;
    // The generation for which a dedicated analysis-unavailable diagnostic is
    // currently displayed. A successful analysis always replaces it, even
    // when the compiler diagnostics themselves are still empty.
    std::unordered_map<std::string, std::uint64_t> unavailable_by_identity_;
    State state_{State::uninitialized};
    bool command_links_{};
    // Loop prevention: the runtime target already requested and the runtime issue
    // already reported, both stored as normalized comparison keys.
    std::optional<std::string> requested_runtime_key_;
    std::optional<std::string> reported_runtime_issue_key_;
    // The variant problem already reported, so an unchanged issue is not shown
    // repeatedly on every reanalysis or configuration event.
    std::optional<std::string> reported_variant_issue_key_;
    std::atomic_bool exit_requested_{};
    bool clean_shutdown_{};
};

[[nodiscard]] int run(std::istream& input, std::ostream& output, std::ostream& errors,
                      ServerOptions options = {});

} // namespace hlsl_intellisense::lsp
