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

class Server final {
  public:
    using NotificationSender = std::function<void(const json_rpc::Notification&)>;
    using Logger = std::function<void(std::string_view)>;

    explicit Server(NotificationSender sender, Logger logger = {}, ServerOptions options = {});
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
    [[nodiscard]] json_rpc::Json prepare_rename(const std::optional<json_rpc::Json>& params,
                                                const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json rename(const std::optional<json_rpc::Json>& params,
                                        const json_rpc::RequestContext& context);
    [[nodiscard]] ReferenceResult find_references(std::string_view uri,
                                                  const workspace::Position& position,
                                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json hover(const std::optional<json_rpc::Json>& params,
                                       const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json signature_help(const std::optional<json_rpc::Json>& params,
                                                const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json document_symbols(const std::optional<json_rpc::Json>& params,
                                                  const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json workspace_symbols(const std::optional<json_rpc::Json>& params,
                                                   const json_rpc::RequestContext& context);
    [[nodiscard]] json_rpc::Json semantic_tokens(const std::optional<json_rpc::Json>& params,
                                                 const json_rpc::RequestContext& context);
    void initialized(const std::optional<json_rpc::Json>& params);
    void did_open(const std::optional<json_rpc::Json>& params);
    void did_change(const std::optional<json_rpc::Json>& params);
    void did_save(const std::optional<json_rpc::Json>& params);
    void did_close(const std::optional<json_rpc::Json>& params);
    void did_change_configuration(const std::optional<json_rpc::Json>& params);
    void did_change_client_defaults(const std::optional<json_rpc::Json>& params);
    void did_change_workspace_folders(const std::optional<json_rpc::Json>& params);
    void did_change_watched_files(const std::optional<json_rpc::Json>& params);
    void exit(const std::optional<json_rpc::Json>& params);
    void analyze_affected(std::string_view uri);
    void analyze_and_publish(std::string_view uri);
    void reanalyze_all();
    void analysis_completed(const workspace::SourceSnapshot& snapshot,
                            const std::vector<dxc::Diagnostic>& diagnostics,
                            std::uint64_t generation);
    [[nodiscard]] workspace::WorkspaceConfiguration
    configuration_for(const workspace::SourceSnapshot& snapshot,
                      const workspace::ConfigurationOverrides& overrides) const;
    [[nodiscard]] std::filesystem::path
    configuration_base_directory(std::string_view shader_path) const;
    void publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                             const std::vector<dxc::Diagnostic>& diagnostics);
    void require_running() const;
    void log(std::string_view message) const;

    json_rpc::Dispatcher dispatcher_;
    workspace::DocumentStore documents_;
    std::unordered_map<std::string, std::filesystem::path> workspace_folders_;
    workspace::ConfigurationOverrides editor_settings_;
    std::optional<std::string> client_default_language_version_;
    NotificationSender sender_;
    Logger logger_;
    ServerOptions options_;
    analysis::Manager analysis_;
    mutable std::mutex state_mutex_;
    std::unordered_map<std::string, std::uint64_t> analysis_generations_;
    State state_{State::uninitialized};
    std::atomic_bool exit_requested_{};
    bool clean_shutdown_{};
};

[[nodiscard]] int run(std::istream& input, std::ostream& output, std::ostream& errors,
                      ServerOptions options = {});

} // namespace hlsl_intellisense::lsp
