#include <hlsl_intellisense/json_rpc/dispatcher.h>

#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace hlsl_intellisense::json_rpc {
namespace {

[[nodiscard]] ErrorResponse make_error(const RequestId& id, int code, std::string message,
                                       std::optional<Json> data = std::nullopt) {
    return ErrorResponse{
        .id = id,
        .error = Error{.code = code, .message = std::move(message), .data = std::move(data)}};
}

} // namespace

struct CancellationToken::State final {
    std::mutex mutex;
    bool cancelled{};
    std::vector<std::function<void()>> callbacks;
};

CancellationToken::CancellationToken() : state_{std::make_shared<State>()} {}

CancellationToken::CancellationToken(std::shared_ptr<State> state) : state_{std::move(state)} {}

void CancellationToken::cancel() const noexcept {
    std::vector<std::function<void()>> callbacks;
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->cancelled) {
            return;
        }
        state_->cancelled = true;
        callbacks = std::move(state_->callbacks);
    }
    for (auto& callback : callbacks) {
        callback();
    }
}

bool CancellationToken::is_cancellation_requested() const noexcept {
    std::scoped_lock lock{state_->mutex};
    return state_->cancelled;
}

void CancellationToken::throw_if_cancellation_requested() const {
    if (is_cancellation_requested()) {
        throw HandlerError{request_cancelled_code, "Request cancelled"};
    }
}

void CancellationToken::on_cancel(std::function<void()> callback) const {
    if (!callback) {
        throw std::invalid_argument{"Cancellation callback must be callable"};
    }
    {
        std::scoped_lock lock{state_->mutex};
        if (!state_->cancelled) {
            state_->callbacks.push_back(std::move(callback));
            return;
        }
    }
    callback();
}

bool CancellationToken::shares_state(const CancellationToken& other) const noexcept {
    return state_ == other.state_;
}

void Dispatcher::register_request_handler(std::string method, RequestHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument{"A request handler requires a method and callable"};
    }
    register_request_handler(
        std::move(method),
        [handler = std::move(handler)](const std::optional<Json>& params, const RequestContext&) {
            return handler(params);
        });
}

void Dispatcher::register_request_handler(std::string method, ContextRequestHandler handler) {
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
    const auto cancellation = begin_request(request.id);
    auto response = dispatch(request, cancellation);
    finish_request(request.id, cancellation);
    return response;
}

DispatchResponse Dispatcher::dispatch(const Request& request,
                                      const CancellationToken& cancellation) const {
    const auto handler = request_handlers_.find(request.method);
    if (handler == request_handlers_.end()) {
        return make_error(request.id, method_not_found_code, "Method not found");
    }

    try {
        const RequestContext context{.id = request.id, .cancellation = cancellation};
        cancellation.throw_if_cancellation_requested();
        auto result = handler->second(request.params, context);
        cancellation.throw_if_cancellation_requested();
        return Response{.id = request.id, .result = std::move(result)};
    } catch (const HandlerError& error) {
        return make_error(request.id, error.code(), error.what(), error.data());
    } catch (const std::exception&) {
        if (cancellation.is_cancellation_requested()) {
            return make_error(request.id, request_cancelled_code, "Request cancelled");
        }
        return make_error(request.id, internal_error_code, "Internal error");
    } catch (...) {
        if (cancellation.is_cancellation_requested()) {
            return make_error(request.id, request_cancelled_code, "Request cancelled");
        }
        return make_error(request.id, internal_error_code, "Internal error");
    }
}

void Dispatcher::dispatch(const Notification& notification) const noexcept {
    if (notification.method == "$/cancelRequest") {
        try {
            if (!notification.params || !notification.params->is_object()) {
                return;
            }
            const auto id = notification.params->find("id");
            if (id == notification.params->end()) {
                return;
            }
            RequestId request_id;
            if (id->is_number_integer()) {
                request_id = id->get<std::int64_t>();
            } else if (id->is_string()) {
                request_id = id->get<std::string>();
            } else {
                return;
            }
            std::scoped_lock lock{requests_mutex_};
            if (const auto active = active_requests_.find(request_key(request_id));
                active != active_requests_.end()) {
                active->second.cancel();
            }
        } catch (const std::exception&) {
            return;
        }
        return;
    }
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

CancellationToken Dispatcher::begin_request(const RequestId& id) const {
    CancellationToken cancellation;
    std::scoped_lock lock{requests_mutex_};
    const auto key = request_key(id);
    if (const auto active = active_requests_.find(key); active != active_requests_.end()) {
        active->second.cancel();
        active->second = cancellation;
    } else {
        active_requests_.emplace(key, cancellation);
    }
    return cancellation;
}

void Dispatcher::finish_request(const RequestId& id,
                                const CancellationToken& cancellation) const noexcept {
    std::scoped_lock lock{requests_mutex_};
    const auto active = active_requests_.find(request_key(id));
    if (active != active_requests_.end() && active->second.shares_state(cancellation)) {
        active_requests_.erase(active);
    }
}

void Dispatcher::cancel_all() const noexcept {
    std::scoped_lock lock{requests_mutex_};
    for (const auto& [key, cancellation] : active_requests_) {
        static_cast<void>(key);
        cancellation.cancel();
    }
}

std::string Dispatcher::request_key(const RequestId& id) {
    return std::visit(
        [](const auto& value) -> std::string {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::nullptr_t>) {
                return "n:";
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                return "i:" + std::to_string(value);
            } else {
                return "s:" + value;
            }
        },
        id);
}

} // namespace hlsl_intellisense::json_rpc
