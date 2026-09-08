#include <hlsl_intellisense/analysis/worker_protocol.h>
#include <hlsl_intellisense/json_rpc/framing.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[noreturn]] void hang() {
    for (;;) {
        std::this_thread::sleep_for(100ms);
    }
}

void close_stdout() {
    std::cout.flush();
#ifdef _WIN32
    static_cast<void>(::_close(::_fileno(stdout)));
#else
    static_cast<void>(::close(STDOUT_FILENO));
#endif
}

[[nodiscard]] Json reply(std::uint64_t id, Json result) {
    return Json{{"protocol", analysis::analysis_worker_protocol_version},
                {"id", id},
                {"result", std::move(result)}};
}

[[nodiscard]] Json error_reply(std::uint64_t id, std::string_view message) {
    return Json{{"protocol", analysis::analysis_worker_protocol_version},
                {"id", id},
                {"error", Json{{"message", message}}}};
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    if (::_setmode(::_fileno(stdin), _O_BINARY) == -1 ||
        ::_setmode(::_fileno(stdout), _O_BINARY) == -1) {
        return EXIT_FAILURE;
    }
#endif
    std::string runtime_directory;
    if (argc == 3 && std::string_view{argv[1]} == "--dxc-runtime") {
        runtime_directory = argv[2];
    } else if (argc != 1) {
        return EXIT_FAILURE;
    }

    json_rpc::FrameReader reader{std::cin, analysis::analysis_worker_max_payload_size};
    json_rpc::FrameWriter writer{std::cout};
    bool shutdown_hangs = false;
    while (const auto payload = reader.read()) {
        const auto request = Json::parse(*payload);
        const auto id = request.at("id").get<std::uint64_t>();
        const auto method = request.at("method").get<std::string>();
        if (method == "hang") {
            hang();
        }
        if (method == "crash") {
            std::_Exit(23);
        }
        if (method == "eof") {
            close_stdout();
            hang();
        }
        if (method == "malformed") {
            writer.write("{");
            continue;
        }
        if (method == "mismatch") {
            writer.write(reply(id + 1U, Json::object()).dump());
            continue;
        }
        if (method == "error") {
            writer.write(error_reply(id, "test worker error").dump());
            continue;
        }
        if (method == "setShutdownHang") {
            shutdown_hangs = true;
            writer.write(reply(id, Json::object()).dump());
            continue;
        }
        if (method == "shutdown") {
            if (shutdown_hangs) {
                hang();
            }
            writer.write(reply(id, Json::object()).dump());
            break;
        }
        if (method == "runtimeInfo") {
            writer.write(
                reply(id, Json{{"pid", process_id()}, {"dxcRuntime", runtime_directory}}).dump());
            continue;
        }
        writer.write(reply(id, request.value("params", Json::object())).dump());
    }
    return EXIT_SUCCESS;
}
