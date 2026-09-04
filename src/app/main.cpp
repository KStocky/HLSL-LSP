#include <hlsl_intellisense/lsp/server.h>

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

[[nodiscard]] bool parse_positive(std::string_view value, std::size_t& result) {
    std::size_t parsed{};
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        parsed == 0) {
        return false;
    }
    result = parsed;
    return true;
}

void write_crash_message(std::string_view message) noexcept {
#ifdef _WIN32
    const auto handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written{};
    static_cast<void>(
        WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr));
#else
    static_cast<void>(::write(STDERR_FILENO, message.data(), message.size()));
#endif
}

#ifdef _WIN32
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS*) {
    write_crash_message("HLSL-LSP: fatal native exception; consult Windows Error Reporting\n");
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void fatal_signal_handler(int signal) {
    write_crash_message("HLSL-LSP: fatal native signal; inspect the generated core dump\n");
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
#endif

void install_crash_diagnostics() {
    std::set_terminate([] {
        write_crash_message("HLSL-LSP: unhandled C++ exception; terminating\n");
        std::abort();
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#else
    for (const auto signal : {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV}) {
        std::signal(signal, fatal_signal_handler);
    }
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    install_crash_diagnostics();
    hlsl_intellisense::lsp::ServerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--disable-semantic-tokens") {
            options.semantic_tokens = false;
            continue;
        }
        if (argument == "--trace-protocol") {
            options.protocol_trace = true;
            continue;
        }
        if (argument == "--trace-source") {
            options.protocol_trace = true;
            options.trace_source = true;
            continue;
        }
        if (argument == "--dxc-runtime") {
            if (index + 1 >= argc) {
                std::cerr << "HLSL-LSP: missing value for argument: " << argument << '\n';
                return EXIT_FAILURE;
            }
            options.analysis.runtime.directory = argv[++index];
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "HLSL-LSP: missing value for argument: " << argument << '\n';
            return EXIT_FAILURE;
        }
        std::size_t value{};
        if (!parse_positive(argv[++index], value)) {
            std::cerr << "HLSL-LSP: argument requires a positive integer: " << argument << '\n';
            return EXIT_FAILURE;
        }
        if (argument == "--analysis-workers") {
            options.analysis.scheduler.worker_count = value;
        } else if (argument == "--analysis-queue-capacity") {
            options.analysis.scheduler.queue_capacity = value;
        } else if (argument == "--request-workers") {
            options.request_worker_count = value;
        } else if (argument == "--request-queue-capacity") {
            options.request_queue_capacity = value;
        } else if (argument == "--translation-unit-count") {
            options.analysis.limits.max_translation_units = value;
        } else if (argument == "--translation-unit-memory-mb" ||
                   argument == "--include-cache-memory-mb") {
            constexpr std::size_t mebibyte = std::size_t{1024} * 1024U;
            if (value > std::numeric_limits<std::size_t>::max() / mebibyte) {
                std::cerr << "HLSL-LSP: memory argument is too large: " << argument << '\n';
                return EXIT_FAILURE;
            }
            if (argument == "--translation-unit-memory-mb") {
                options.analysis.limits.max_translation_unit_estimated_bytes = value * mebibyte;
            } else {
                options.analysis.limits.include_cache.max_estimated_bytes = value * mebibyte;
            }
        } else if (argument == "--include-cache-count") {
            options.analysis.limits.include_cache.max_entries = value;
        } else {
            std::cerr << "HLSL-LSP: unknown argument: " << argument << '\n';
            return EXIT_FAILURE;
        }
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::cerr << "HLSL-LSP: unable to configure binary stdio\n";
        return EXIT_FAILURE;
    }
#endif
    return hlsl_intellisense::lsp::run(std::cin, std::cout, std::cerr, options);
}
