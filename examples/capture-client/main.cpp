#include <hlsl_intellisense/capture/capture.h>

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: hlsl-capture-example <shader-source-path> [natural|warm|both]\n";
        return 2;
    }
    const std::string_view mode = argc == 3 ? argv[2] : "";
    if (!mode.empty() && mode != "natural" && mode != "warm" && mode != "both") {
        std::cerr << "Unknown capture example mode; use natural, warm, or both.\n";
        return 2;
    }

    hlsl_intellisense::capture::Client client;
    hlsl_intellisense::capture::Invocation invocation;
    invocation.source = argv[1];
    invocation.entry_point = mode.empty() ? "Main" : "MainPS";
    invocation.target_profile = mode.empty() ? "ps_6_7" : "ps_6_6";
    if (!mode.empty()) {
        invocation.language_version = "2021";
        invocation.defines = {{"WARM_GRADE", mode == "warm" ? "1" : "0"}};
    }

    // An engine calls report from its compile wrapper even before capture is enabled.
    if (client.report(invocation)) {
        std::cerr << "Unexpected report from a disconnected capture client.\n";
        return 1;
    }
    std::cerr << "Disconnected report skipped. Enter the endpoint and token from an active "
                 "editor capture session on separate input lines (or EOF to exit):\n";

    std::string endpoint;
    std::string token;
    if (!std::getline(std::cin, endpoint) || endpoint.empty()) {
        return 0;
    }
    if (!std::getline(std::cin, token) || !client.connect(endpoint, token)) {
        std::cerr << "Could not connect to the active capture session.\n";
        return 1;
    }
    const int report_count = mode == "both" ? 2 : 1;
    for (int index = 0; index < report_count; ++index) {
        if (mode == "both" && index == 1) {
            invocation.defines = {{"WARM_GRADE", "1"}};
        }
        if (!client.report(invocation)) {
            std::cerr << "Capture report was not queued.\n";
            client.disconnect();
            return 1;
        }
    }
    client.disconnect();
    std::cerr << report_count << " report(s) queued; capture client disconnected.\n";
}
