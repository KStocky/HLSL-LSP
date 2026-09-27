#include <hlsl_intellisense/capture/capture.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: hlsl-capture-example <shader-source-path>\n";
        return 2;
    }

    hlsl_intellisense::capture::Client client;
    hlsl_intellisense::capture::Invocation invocation;
    invocation.source = argv[1];
    invocation.entry_point = "Main";
    invocation.target_profile = "ps_6_7";

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
    if (!client.report(invocation)) {
        std::cerr << "Capture report was not queued.\n";
        client.disconnect();
        return 1;
    }
    client.disconnect();
    std::cerr << "Report queued; capture client disconnected.\n";
}
