#pragma once

#include <hlsl_intellisense/json_rpc/message.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hlsl_intellisense::json_rpc {

inline constexpr int request_cancelled_code = -32800;
inline constexpr int content_modified_code = -32801;
inline constexpr int server_cancelled_code = -32802;

class CancellationToken final {
  public:
    CancellationToken();

    void cancel() const noexcept;
    [[nodiscard]] bool is_cancellation_requested() const noexcept;
    void throw_if_cancellation_requested() const;
    void on_cancel(std::function<void()> callback) const;

  private:
    struct State;

    explicit CancellationToken(std::shared_ptr<State> state);
    [[nodiscard]] bool shares_state(const CancellationToken& other) const noexcept;

    std::shared_ptr<State> state_;

    friend class Dispatcher;
};

struct RequestContext {
    RequestId id;
    CancellationToken cancellation;
};

class Dispatcher final {
  public:
    using RequestHandler = std::function<Json(const std::optional<Json>&)>;
    using ContextRequestHandler =
        std::function<Json(const std::optional<Json>&, const RequestContext&)>;
    using NotificationHandler = std::function<void(const std::optional<Json>&)>;

    void register_request_handler(std::string method, RequestHandler handler);
    void register_request_handler(std::string method, ContextRequestHandler handler);
    void register_notification_handler(std::string method, NotificationHandler handler);

    [[nodiscard]] DispatchResponse dispatch(const Request& request) const;
    [[nodiscard]] DispatchResponse dispatch(const Request& request,
                                            const CancellationToken& cancellation) const;
    void dispatch(const Notification& notification) const noexcept;
    [[nodiscard]] std::optional<DispatchResponse> dispatch(const Message& message) const;

    [[nodiscard]] CancellationToken begin_request(const RequestId& id) const;
    void finish_request(const RequestId& id, const CancellationToken& cancellation) const noexcept;
    void cancel_all() const noexcept;

  private:
    [[nodiscard]] static std::string request_key(const RequestId& id);

    std::unordered_map<std::string, ContextRequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
    mutable std::mutex requests_mutex_;
    mutable std::unordered_map<std::string, CancellationToken> active_requests_;
};

} // namespace hlsl_intellisense::json_rpc
