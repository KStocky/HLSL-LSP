#include <hlsl_intellisense/json_rpc/dispatcher.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace json_rpc = hlsl_intellisense::json_rpc;

TEST_CASE("Dispatcher invokes registered request handlers", "[json-rpc][dispatcher]") {
    json_rpc::Dispatcher dispatcher;
    dispatcher.register_request_handler("sum", [](const std::optional<json_rpc::Json>& params) {
        return params->at(0).get<int>() + params->at(1).get<int>();
    });
    const json_rpc::Request request{
        .id = std::int64_t{7}, .method = "sum", .params = json_rpc::Json::array({2, 3})};

    const auto dispatched = dispatcher.dispatch(request);

    const auto& response = std::get<json_rpc::Response>(dispatched);
    CHECK(std::get<std::int64_t>(response.id) == 7);
    CHECK(response.result == 5);
}

TEST_CASE("Dispatcher invokes notifications without producing responses",
          "[json-rpc][dispatcher]") {
    json_rpc::Dispatcher dispatcher;
    std::string received;
    dispatcher.register_notification_handler(
        "status", [&received](const std::optional<json_rpc::Json>& params) {
            received = params->at("value").get<std::string>();
        });
    const json_rpc::Message notification{
        json_rpc::Notification{.method = "status", .params = json_rpc::Json{{"value", "ready"}}}};

    const auto response = dispatcher.dispatch(notification);

    CHECK(received == "ready");
    CHECK(!response.has_value());
}

TEST_CASE("Dispatcher produces standard method and handler errors", "[json-rpc][dispatcher]") {
    json_rpc::Dispatcher dispatcher;
    const json_rpc::Request request{
        .id = std::string{"id"}, .method = "missing", .params = std::nullopt};

    SECTION("unknown request") {
        const auto response = dispatcher.dispatch(request);
        CHECK(std::get<json_rpc::ErrorResponse>(response).error.code ==
              json_rpc::method_not_found_code);
    }
    SECTION("invalid params") {
        dispatcher.register_request_handler(
            "missing", [](const std::optional<json_rpc::Json>&) -> json_rpc::Json {
                throw json_rpc::HandlerError{json_rpc::invalid_params_code, "Invalid params"};
            });
        const auto response = dispatcher.dispatch(request);
        CHECK(std::get<json_rpc::ErrorResponse>(response).error.code ==
              json_rpc::invalid_params_code);
    }
    SECTION("internal error") {
        dispatcher.register_request_handler(
            "missing", [](const std::optional<json_rpc::Json>&) -> json_rpc::Json {
                throw std::runtime_error{"implementation detail"};
            });
        const auto response = dispatcher.dispatch(request);
        const auto& error = std::get<json_rpc::ErrorResponse>(response).error;
        CHECK(error.code == json_rpc::internal_error_code);
        CHECK(error.message == "Internal error");
    }
}

TEST_CASE("Unknown notifications are ignored", "[json-rpc][dispatcher]") {
    json_rpc::Dispatcher dispatcher;
    const json_rpc::Message notification{
        json_rpc::Notification{.method = "unknown", .params = std::nullopt}};

    CHECK(!dispatcher.dispatch(notification).has_value());
}
