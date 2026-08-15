#include <hlsl_intellisense/json_rpc/message.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace json_rpc = hlsl_intellisense::json_rpc;

TEST_CASE("JSON-RPC requests and notifications are modeled strictly", "[json-rpc][message]") {
    SECTION("integer request id") {
        const auto parsed =
            json_rpc::parse_message(R"({"jsonrpc":"2.0","id":42,"method":"lookup","params":[1]})");

        REQUIRE(parsed.has_value());
        const auto& request = std::get<json_rpc::Request>(*parsed.message);
        CHECK(std::get<std::int64_t>(request.id) == 42);
        CHECK(request.method == "lookup");
        REQUIRE(request.params.has_value());
        CHECK(*request.params == json_rpc::Json::array({1}));
    }
    SECTION("string request id") {
        const auto parsed =
            json_rpc::parse_message(R"({"jsonrpc":"2.0","id":"abc","method":"lookup"})");

        REQUIRE(parsed.has_value());
        CHECK(std::get<std::string>(std::get<json_rpc::Request>(*parsed.message).id) == "abc");
    }
    SECTION("null request id") {
        const auto parsed =
            json_rpc::parse_message(R"({"jsonrpc":"2.0","id":null,"method":"lookup"})");

        REQUIRE(parsed.has_value());
        CHECK(std::holds_alternative<std::nullptr_t>(
            std::get<json_rpc::Request>(*parsed.message).id));
    }
    SECTION("notification") {
        const auto parsed =
            json_rpc::parse_message(R"({"jsonrpc":"2.0","method":"ready","params":{}})");

        REQUIRE(parsed.has_value());
        CHECK(std::get<json_rpc::Notification>(*parsed.message).method == "ready");
    }
}

TEST_CASE("Malformed JSON produces a parse error response", "[json-rpc][message]") {
    const auto parsed = json_rpc::parse_message(R"({"jsonrpc":)");

    REQUIRE(!parsed.has_value());
    const auto error = parsed.error.value_or(json_rpc::ErrorResponse{});
    CHECK(error.error.code == json_rpc::parse_error_code);
    CHECK(std::holds_alternative<std::nullptr_t>(error.id));
    CHECK(json_rpc::to_json(json_rpc::DispatchResponse{error})["error"]["code"] ==
          json_rpc::parse_error_code);
}

TEST_CASE("Invalid JSON-RPC messages produce invalid request errors", "[json-rpc][message]") {
    const auto check_invalid = [](std::string_view payload) {
        const auto parsed = json_rpc::parse_message(payload);
        REQUIRE(!parsed.has_value());
        CHECK(parsed.error.value_or(json_rpc::ErrorResponse{}).error.code ==
              json_rpc::invalid_request_code);
    };

    SECTION("wrong version") { check_invalid(R"({"jsonrpc":"1.0","id":1,"method":"x"})"); }
    SECTION("fractional id") { check_invalid(R"({"jsonrpc":"2.0","id":1.5,"method":"x"})"); }
    SECTION("scalar params") {
        check_invalid(R"({"jsonrpc":"2.0","id":1,"method":"x","params":true})");
    }
    SECTION("response has result and error") {
        check_invalid(
            R"({"jsonrpc":"2.0","id":1,"result":null,"error":{"code":-1,"message":"bad"}})");
    }
    SECTION("error code is out of range") {
        check_invalid(
            R"({"jsonrpc":"2.0","id":1,"error":{"code":9223372036854775807,"message":"bad"}})");
    }
}

TEST_CASE("Responses and errors serialize as JSON-RPC 2.0", "[json-rpc][message]") {
    const json_rpc::DispatchResponse response{
        json_rpc::Response{.id = std::string{"request"}, .result = {{"value", 7}}}};
    const json_rpc::DispatchResponse error{
        json_rpc::ErrorResponse{.id = std::int64_t{3},
                                .error = {.code = json_rpc::invalid_params_code,
                                          .message = "Invalid params",
                                          .data = json_rpc::Json{{"field", "name"}}}}};

    CHECK(json_rpc::to_json(response) == json_rpc::Json{{"jsonrpc", "2.0"},
                                                        {"id", "request"},
                                                        {"result", json_rpc::Json{{"value", 7}}}});
    CHECK(json_rpc::to_json(error)["error"]["data"]["field"] == "name");
}
