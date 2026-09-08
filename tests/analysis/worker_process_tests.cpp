#include <hlsl_intellisense/analysis/worker_process.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;
using Json = json_rpc::Json;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] analysis::WorkerProcess make_worker(std::chrono::milliseconds shutdown = 100ms,
                                                  std::string runtime = {}) {
    return analysis::WorkerProcess{
        analysis::WorkerProcessOptions{.executable = HLSL_TEST_WORKER_HELPER,
                                       .dxc_runtime_directory = std::move(runtime),
                                       .shutdown_timeout = shutdown}};
}

void check_error(const analysis::WorkerProcessErrorCode expected, const auto& operation) {
    try {
        operation();
        FAIL("Expected WorkerProcessError");
    } catch (const analysis::WorkerProcessError& error) {
        CHECK(error.code() == expected);
        CHECK_FALSE(std::string{error.what()}.empty());
    }
}

[[nodiscard]] Json request(analysis::WorkerProcess& worker, std::string_view method,
                           Json params = Json::object(), std::chrono::milliseconds timeout = 2s) {
    json_rpc::CancellationToken cancellation;
    return worker.request(method, std::move(params), timeout, cancellation);
}

} // namespace

TEST_CASE("Worker process launches, handshakes, and forwards runtime configuration",
          "[analysis][worker][process]") {
    auto worker = make_worker(100ms, "runtime-test-directory");

    const auto runtime = request(worker, "runtimeInfo");
    CHECK(runtime.at("pid").get<std::uint64_t>() != 0);
    CHECK(runtime.at("dxcRuntime") == "runtime-test-directory");

    const auto result = request(worker, "echo", Json{{"value", 42}});
    CHECK(result == Json{{"value", 42}});
}

TEST_CASE("Worker process finds the sibling production worker by default",
          "[analysis][worker][process][default]") {
    analysis::WorkerProcess worker{analysis::WorkerProcessOptions{
        .executable = {}, .dxc_runtime_directory = {}, .shutdown_timeout = 500ms}};

    const auto runtime = request(worker, "runtimeInfo");
    CHECK(runtime.at("libraryPath").is_string());
    CHECK_FALSE(runtime.at("libraryPath").get<std::string>().empty());
    CHECK(runtime.at("version").is_string());
}

TEST_CASE("Worker process timeout kills only the child and permits replacement",
          "[analysis][worker][process][timeout]") {
    auto worker = make_worker();
    const auto first_pid = request(worker, "runtimeInfo").at("pid").get<std::uint64_t>();

    const auto started = std::chrono::steady_clock::now();
    check_error(analysis::WorkerProcessErrorCode::timed_out,
                [&] { static_cast<void>(request(worker, "hang", Json::object(), 40ms)); });
    CHECK(std::chrono::steady_clock::now() - started < 1s);

    const auto second_pid = request(worker, "runtimeInfo").at("pid").get<std::uint64_t>();
    CHECK(second_pid != first_pid);
}

TEST_CASE("Worker process cancellation terminates a wedged child promptly",
          "[analysis][worker][process][cancel]") {
    auto worker = make_worker();
    json_rpc::CancellationToken cancellation;
    std::jthread canceller{[cancellation] {
        std::this_thread::sleep_for(20ms);
        cancellation.cancel();
    }};

    const auto started = std::chrono::steady_clock::now();
    check_error(analysis::WorkerProcessErrorCode::cancelled, [&] {
        static_cast<void>(worker.request("hang", Json::object(), 2s, cancellation));
    });
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    CHECK(request(worker, "echo", Json{{"recovered", true}}).at("recovered") == true);
}

TEST_CASE("Worker process reports crash and protocol EOF", "[analysis][worker][process][eof]") {
    SECTION("crash") {
        auto worker = make_worker();
        check_error(analysis::WorkerProcessErrorCode::worker_exited,
                    [&] { static_cast<void>(request(worker, "crash")); });
        CHECK(request(worker, "echo", Json{{"replacement", true}}).at("replacement") == true);
    }
    SECTION("EOF from a live child") {
        auto worker = make_worker();
        check_error(analysis::WorkerProcessErrorCode::unexpected_eof,
                    [&] { static_cast<void>(request(worker, "eof")); });
        CHECK(request(worker, "echo", Json{{"replacement", true}}).at("replacement") == true);
    }
}

TEST_CASE("Worker process rejects malformed and mismatched replies and recovers",
          "[analysis][worker][process][protocol]") {
    auto worker = make_worker();
    check_error(analysis::WorkerProcessErrorCode::malformed_reply,
                [&] { static_cast<void>(request(worker, "malformed")); });
    CHECK(request(worker, "echo", Json{{"afterMalformed", true}}).at("afterMalformed") == true);

    check_error(analysis::WorkerProcessErrorCode::mismatched_reply,
                [&] { static_cast<void>(request(worker, "mismatch")); });
    CHECK(request(worker, "echo", Json{{"afterMismatch", true}}).at("afterMismatch") == true);
}

TEST_CASE("Worker process surfaces worker errors without discarding a healthy child",
          "[analysis][worker][process][errors]") {
    auto worker = make_worker();
    const auto first_pid = request(worker, "runtimeInfo").at("pid").get<std::uint64_t>();
    check_error(analysis::WorkerProcessErrorCode::worker_error,
                [&] { static_cast<void>(request(worker, "error")); });
    const auto second_pid = request(worker, "runtimeInfo").at("pid").get<std::uint64_t>();
    CHECK(second_pid == first_pid);
}

TEST_CASE("Worker process shutdown and destructor are bounded",
          "[analysis][worker][process][shutdown]") {
    const auto started = std::chrono::steady_clock::now();
    {
        auto worker = make_worker(40ms);
        static_cast<void>(request(worker, "setShutdownHang"));
    }
    CHECK(std::chrono::steady_clock::now() - started < 1s);
}

TEST_CASE("Worker process shutdown interrupts an active wedged request",
          "[analysis][worker][process][shutdown]") {
    auto worker = make_worker(40ms);
    auto request_result = std::async(std::launch::async, [&worker] {
        try {
            static_cast<void>(request(worker, "hang", Json::object(), 10s));
            return false;
        } catch (const analysis::WorkerProcessError&) {
            return true;
        }
    });
    std::this_thread::sleep_for(20ms);

    const auto started = std::chrono::steady_clock::now();
    worker.shutdown();
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    REQUIRE(request_result.wait_for(1s) == std::future_status::ready);
    CHECK(request_result.get());
}
