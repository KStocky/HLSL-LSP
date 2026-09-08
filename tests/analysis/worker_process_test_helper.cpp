#include <hlsl_intellisense/analysis/worker_protocol.h>
#include <hlsl_intellisense/json_rpc/framing.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

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

void signal_entered(const std::string& path) {
    std::ofstream marker{path + ".worker-entered", std::ios::trunc};
    marker << "entered";
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
    std::unordered_map<std::string, std::string> cache_keys;
    std::unordered_map<std::string, std::string> sources;
    while (const auto payload = reader.read()) {
        const auto request = Json::parse(*payload);
        const auto id = request.at("id").get<std::uint64_t>();
        const auto method = request.at("method").get<std::string>();
        const auto& params = request.at("params");
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
        if (method == "analyze") {
            const auto root = params.at("rootIdentity").get<std::string>();
            const auto cache_key = params.at("cacheKey").get<std::string>();
            const auto path = params.at("path").get<std::string>();
            const auto& source_values = params.at("sources");
            const auto existing_key = cache_keys.find(root);
            const auto cache_hit =
                existing_key != cache_keys.end() && existing_key->second == cache_key;
            if (cache_hit && !source_values.empty()) {
                writer.write(error_reply(id, "cache hit redundantly included sources").dump());
                continue;
            }
            if (!cache_hit && source_values.empty()) {
                writer.write(error_reply(id, "cache miss omitted sources").dump());
                continue;
            }
            const auto source =
                cache_hit ? sources.at(root) : source_values.at(0).at("text").get<std::string>();
            if (source.find("HLSL_TEST_BACKGROUND_HANG") != std::string::npos) {
                signal_entered(path);
                hang();
            }
            if (source.find("HLSL_TEST_BACKGROUND_CRASH_ONCE") != std::string::npos) {
                const auto marker = std::filesystem::path{path + ".worker-crashed-once"};
                if (!std::filesystem::exists(marker)) {
                    std::ofstream created{marker, std::ios::trunc};
                    created << "crashed";
                    created.close();
                    std::_Exit(23);
                }
            } else if (source.find("HLSL_TEST_BACKGROUND_CRASH") != std::string::npos) {
                std::_Exit(23);
            }
            if (source.find("HLSL_TEST_BACKGROUND_MALFORMED_RESULT") != std::string::npos) {
                writer.write(reply(id, Json{{"diagnostics", "not-an-array"}, {"kind", 1}}).dump());
                continue;
            }

            auto kind = std::uint8_t{1};
            if (existing_key != cache_keys.end()) {
                kind = cache_hit ? std::uint8_t{0} : std::uint8_t{2};
            }
            cache_keys.insert_or_assign(root, cache_key);
            if (!cache_hit) {
                sources.insert_or_assign(root, source);
            }
            writer.write(reply(id, Json{{"diagnostics", Json::array()}, {"kind", kind}}).dump());
            continue;
        }
        if (method == "erase") {
            const auto root = params.at("rootIdentity").get<std::string>();
            cache_keys.erase(root);
            sources.erase(root);
            writer.write(reply(id, Json::object()).dump());
            continue;
        }
        if (const auto root_value = params.find("rootIdentity");
            root_value != params.end() && root_value->is_string()) {
            const auto root = root_value->get<std::string>();
            if (const auto source = sources.find(root); source != sources.end()) {
                if (source->second.find("HLSL_TEST_HANG_INTERACTIVE") != std::string::npos) {
                    hang();
                }
                if (source->second.find("HLSL_TEST_CRASH_INTERACTIVE") != std::string::npos) {
                    std::_Exit(23);
                }
                if (source->second.find("HLSL_TEST_MALFORMED_INTERACTIVE") != std::string::npos) {
                    writer.write(reply(id, Json::object()).dump());
                    continue;
                }
                if (source->second.find("HLSL_TEST_OVERSIZED_INTERACTIVE") != std::string::npos &&
                    method == "tokens") {
                    writer.write(reply(id, Json::array({Json{{"line", std::uint64_t{1} << 40U},
                                                             {"column", 1U},
                                                             {"length", 1U},
                                                             {"kind", 0U},
                                                             {"cursor_kind", 0U}}}))
                                     .dump());
                    continue;
                }
            }
        }
        if (method == "complete" || method == "references" || method == "signatures" ||
            method == "inlayHints" || method == "tokens" || method == "skippedRanges" ||
            method == "macroDefinitions" || method == "symbols" || method == "outgoingCalls" ||
            method == "incomingCalls") {
            writer.write(reply(id, Json::array()).dump());
            continue;
        }
        if (method == "definition" || method == "hover" || method == "memoryLayout" ||
            method == "callableAt") {
            writer.write(reply(id, Json(nullptr)).dump());
            continue;
        }
        if (method == "documentSymbols") {
            writer.write(reply(id, Json{{"symbols", Json::array()}, {"truncated", false}}).dump());
            continue;
        }
        if (method == "verifyCallHierarchyIdentity") {
            writer.write(reply(id, Json(false)).dump());
            continue;
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
