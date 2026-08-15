#pragma once

#include <hlsl_intellisense/json_rpc/message.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hlsl_intellisense::json_rpc {

class Dispatcher final {
  public:
    using RequestHandler = std::function<Json(const std::optional<Json>&)>;
    using NotificationHandler = std::function<void(const std::optional<Json>&)>;

    void register_request_handler(std::string method, RequestHandler handler);
    void register_notification_handler(std::string method, NotificationHandler handler);

    [[nodiscard]] DispatchResponse dispatch(const Request& request) const;
    void dispatch(const Notification& notification) const noexcept;
    [[nodiscard]] std::optional<DispatchResponse> dispatch(const Message& message) const;

  private:
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
};

} // namespace hlsl_intellisense::json_rpc
