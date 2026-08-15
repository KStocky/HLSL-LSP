#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace hlsl_intellisense::json_rpc {

using Json = nlohmann::json;
using RequestId = std::variant<std::nullptr_t, std::int64_t, std::string>;

inline constexpr int parse_error_code = -32700;
inline constexpr int invalid_request_code = -32600;
inline constexpr int method_not_found_code = -32601;
inline constexpr int invalid_params_code = -32602;
inline constexpr int internal_error_code = -32603;

struct Request {
    RequestId id;
    std::string method;
    std::optional<Json> params;

    bool operator==(const Request&) const = default;
};

struct Notification {
    std::string method;
    std::optional<Json> params;

    bool operator==(const Notification&) const = default;
};

struct Response {
    RequestId id;
    Json result;

    bool operator==(const Response&) const = default;
};

struct Error {
    int code{};
    std::string message;
    std::optional<Json> data;

    bool operator==(const Error&) const = default;
};

struct ErrorResponse {
    RequestId id;
    Error error;

    bool operator==(const ErrorResponse&) const = default;
};

using Message = std::variant<Request, Notification, Response, ErrorResponse>;
using DispatchResponse = std::variant<Response, ErrorResponse>;

struct ParseResult {
    std::optional<Message> message;
    std::optional<ErrorResponse> error;

    [[nodiscard]] bool has_value() const noexcept;
};

class HandlerError final : public std::runtime_error {
  public:
    HandlerError(int code, std::string_view message, std::optional<Json> data = std::nullopt);

    [[nodiscard]] int code() const noexcept;
    [[nodiscard]] const std::optional<Json>& data() const noexcept;

  private:
    int code_;
    std::optional<Json> data_;
};

[[nodiscard]] ParseResult parse_message(std::string_view payload);
[[nodiscard]] Json to_json(const Message& message);
[[nodiscard]] Json to_json(const DispatchResponse& response);
[[nodiscard]] std::string serialize(const Message& message);
[[nodiscard]] std::string serialize(const DispatchResponse& response);

} // namespace hlsl_intellisense::json_rpc
