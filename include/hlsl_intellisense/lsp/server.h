#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/json_rpc/dispatcher.h>
#include <hlsl_intellisense/workspace/document_store.h>

#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hlsl_intellisense::lsp {

class Server final {
  public:
    using NotificationSender = std::function<void(const json_rpc::Notification&)>;
    using Logger = std::function<void(std::string_view)>;

    explicit Server(NotificationSender sender, Logger logger = {});

    [[nodiscard]] std::optional<json_rpc::DispatchResponse>
    handle(const json_rpc::Message& message);
    [[nodiscard]] bool exit_requested() const noexcept;
    [[nodiscard]] int exit_code() const noexcept;

  private:
    enum class State { uninitialized, awaiting_initialized, running, shutdown };

    struct Analysis {
        std::int64_t version;
        dxc::TranslationUnit translation_unit;
    };

    void register_handlers();
    [[nodiscard]] json_rpc::Json initialize(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json shutdown(const std::optional<json_rpc::Json>& params);
    [[nodiscard]] json_rpc::Json completion(const std::optional<json_rpc::Json>& params);
    void initialized(const std::optional<json_rpc::Json>& params);
    void did_open(const std::optional<json_rpc::Json>& params);
    void did_change(const std::optional<json_rpc::Json>& params);
    void did_save(const std::optional<json_rpc::Json>& params);
    void did_close(const std::optional<json_rpc::Json>& params);
    void exit(const std::optional<json_rpc::Json>& params);
    void analyze_and_publish(std::string_view uri);
    void publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                             const std::vector<dxc::Diagnostic>& diagnostics);
    void require_running() const;
    void log(std::string_view message) const;

    json_rpc::Dispatcher dispatcher_;
    workspace::DocumentStore documents_;
    dxc::Intellisense intellisense_;
    std::unordered_map<std::string, Analysis> analyses_;
    NotificationSender sender_;
    Logger logger_;
    State state_{State::uninitialized};
    bool exit_requested_{};
    bool clean_shutdown_{};
};

[[nodiscard]] int run(std::istream& input, std::ostream& output, std::ostream& errors);

} // namespace hlsl_intellisense::lsp
