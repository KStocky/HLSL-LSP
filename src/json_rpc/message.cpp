#include <hlsl_intellisense/json_rpc/message.h>

#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hlsl_intellisense::json_rpc {
namespace {

[[nodiscard]] ErrorResponse make_error(RequestId id, int code, std::string message,
                                       std::optional<Json> data = std::nullopt) {
    return ErrorResponse{
        .id = std::move(id),
        .error = Error{.code = code, .message = std::move(message), .data = std::move(data)}};
}

[[nodiscard]] std::optional<RequestId> parse_id(const Json& value) {
    if (value.is_null()) {
        return RequestId{nullptr};
    }
    if (value.is_string()) {
        return RequestId{value.get<std::string>()};
    }
    if (value.is_number_integer()) {
        return RequestId{value.get<std::int64_t>()};
    }
    if (value.is_number_unsigned()) {
        const auto unsigned_id = value.get<std::uint64_t>();
        if (unsigned_id <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return RequestId{static_cast<std::int64_t>(unsigned_id)};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> parse_error_code_value(const Json& value) {
    if (value.is_number_integer()) {
        const auto code = value.get<std::int64_t>();
        if (code >= std::numeric_limits<int>::min() && code <= std::numeric_limits<int>::max()) {
            return static_cast<int>(code);
        }
    } else if (value.is_number_unsigned()) {
        const auto code = value.get<std::uint64_t>();
        if (code <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return static_cast<int>(code);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool has_valid_version(const Json& value) {
    const auto version = value.find("jsonrpc");
    return version != value.end() && version->is_string() &&
           version->get_ref<const std::string&>() == "2.0";
}

[[nodiscard]] bool has_valid_params(const Json& value) {
    const auto params = value.find("params");
    return params == value.end() || params->is_array() || params->is_object();
}

[[nodiscard]] std::optional<Json> get_optional(const Json& value, std::string_view name) {
    const auto item = value.find(name);
    if (item == value.end()) {
        return std::nullopt;
    }
    return *item;
}

[[nodiscard]] Json id_to_json(const RequestId& id) {
    return std::visit([](const auto& value) -> Json { return value; }, id);
}

[[nodiscard]] Json error_to_json(const ErrorResponse& response) {
    Json error{{"code", response.error.code}, {"message", response.error.message}};
    if (response.error.data.has_value()) {
        error["data"] = *response.error.data;
    }
    return Json{{"jsonrpc", "2.0"}, {"id", id_to_json(response.id)}, {"error", std::move(error)}};
}

[[nodiscard]] ParseResult success(Message message) {
    return {.message = std::move(message), .error = std::nullopt};
}

[[nodiscard]] ParseResult failure(ErrorResponse error) {
    return {.message = std::nullopt, .error = std::move(error)};
}

} // namespace

bool ParseResult::has_value() const noexcept { return message.has_value(); }

HandlerError::HandlerError(int code, std::string_view message, std::optional<Json> data)
    : std::runtime_error{std::string{message}}, code_{code}, data_{std::move(data)} {}

int HandlerError::code() const noexcept { return code_; }

const std::optional<Json>& HandlerError::data() const noexcept { return data_; }

ParseResult parse_message(std::string_view payload) {
    Json value;
    try {
        value = Json::parse(payload);
    } catch (const Json::parse_error&) {
        return failure(make_error(nullptr, parse_error_code, "Parse error"));
    }

    if (!value.is_object() || !has_valid_version(value)) {
        return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
    }

    const auto method = value.find("method");
    if (method != value.end()) {
        if (!method->is_string() || !has_valid_params(value) || value.contains("result") ||
            value.contains("error")) {
            const auto id = value.find("id");
            const auto parsed_id =
                id == value.end() ? std::optional<RequestId>{RequestId{nullptr}} : parse_id(*id);
            return failure(make_error(parsed_id.value_or(RequestId{nullptr}), invalid_request_code,
                                      "Invalid Request"));
        }

        const auto params = get_optional(value, "params");
        const auto id = value.find("id");
        if (id == value.end()) {
            return success(Notification{.method = method->get<std::string>(), .params = params});
        }
        const auto parsed_id = parse_id(*id);
        if (!parsed_id.has_value()) {
            return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
        }
        return success(
            Request{.id = *parsed_id, .method = method->get<std::string>(), .params = params});
    }

    const auto id = value.find("id");
    if (id == value.end()) {
        return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
    }
    const auto parsed_id = parse_id(*id);
    if (!parsed_id.has_value()) {
        return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
    }

    const bool has_result = value.contains("result");
    const bool has_error = value.contains("error");
    if (has_result == has_error) {
        return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
    }
    if (has_result) {
        return success(Response{.id = *parsed_id, .result = value["result"]});
    }

    const auto& error = value["error"];
    const auto code = error.is_object() && error.contains("code")
                          ? parse_error_code_value(error["code"])
                          : std::nullopt;
    if (!code.has_value() || !error.contains("message") || !error["message"].is_string()) {
        return failure(make_error(nullptr, invalid_request_code, "Invalid Request"));
    }
    return success(ErrorResponse{.id = *parsed_id,
                                 .error = Error{.code = *code,
                                                .message = error["message"].get<std::string>(),
                                                .data = get_optional(error, "data")}});
}

Json to_json(const Message& message) {
    return std::visit(
        [](const auto& value) -> Json {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Request>) {
                Json result{
                    {"jsonrpc", "2.0"}, {"id", id_to_json(value.id)}, {"method", value.method}};
                if (value.params.has_value()) {
                    result["params"] = *value.params;
                }
                return result;
            } else if constexpr (std::is_same_v<Value, Notification>) {
                Json result{{"jsonrpc", "2.0"}, {"method", value.method}};
                if (value.params.has_value()) {
                    result["params"] = *value.params;
                }
                return result;
            } else if constexpr (std::is_same_v<Value, Response>) {
                return Json{
                    {"jsonrpc", "2.0"}, {"id", id_to_json(value.id)}, {"result", value.result}};
            } else {
                return error_to_json(value);
            }
        },
        message);
}

Json to_json(const DispatchResponse& response) {
    return std::visit(
        [](const auto& value) -> Json {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Response>) {
                return Json{
                    {"jsonrpc", "2.0"}, {"id", id_to_json(value.id)}, {"result", value.result}};
            } else {
                return error_to_json(value);
            }
        },
        response);
}

std::string serialize(const Message& message) { return to_json(message).dump(); }

std::string serialize(const DispatchResponse& response) { return to_json(response).dump(); }

} // namespace hlsl_intellisense::json_rpc
