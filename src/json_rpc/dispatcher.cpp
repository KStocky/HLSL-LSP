#include <hlsl_intellisense/json_rpc/dispatcher.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace hlsl_intellisense::json_rpc {
namespace {

[[nodiscard]] ErrorResponse make_error(const RequestId& id, int code, std::string message,
                                       std::optional<Json> data = std::nullopt) {
    return ErrorResponse{
        .id = id,
        .error = Error{.code = code, .message = std::move(message), .data = std::move(data)}};
}

} // namespace

void Dispatcher::register_request_handler(std::string method, RequestHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument{"A request handler requires a method and callable"};
    }
    request_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

void Dispatcher::register_notification_handler(std::string method, NotificationHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument{"A notification handler requires a method and callable"};
    }
    notification_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

DispatchResponse Dispatcher::dispatch(const Request& request) const {
    const auto handler = request_handlers_.find(request.method);
    if (handler == request_handlers_.end()) {
        return make_error(request.id, method_not_found_code, "Method not found");
    }

    try {
        return Response{.id = request.id, .result = handler->second(request.params)};
    } catch (const HandlerError& error) {
        return make_error(request.id, error.code(), error.what(), error.data());
    } catch (const std::exception&) {
        return make_error(request.id, internal_error_code, "Internal error");
    } catch (...) {
        return make_error(request.id, internal_error_code, "Internal error");
    }
}

void Dispatcher::dispatch(const Notification& notification) const noexcept {
    const auto handler = notification_handlers_.find(notification.method);
    if (handler == notification_handlers_.end()) {
        return;
    }
    try {
        handler->second(notification.params);
    } catch (...) {
        return;
    }
}

std::optional<DispatchResponse> Dispatcher::dispatch(const Message& message) const {
    if (const auto* request = std::get_if<Request>(&message)) {
        return dispatch(*request);
    }
    if (const auto* notification = std::get_if<Notification>(&message)) {
        dispatch(*notification);
    }
    return std::nullopt;
}

} // namespace hlsl_intellisense::json_rpc
