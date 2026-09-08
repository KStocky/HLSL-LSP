#include <hlsl_intellisense/analysis/worker_protocol.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char* argv[]) {
    hlsl_intellisense::dxc::RuntimeConfiguration runtime;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument != "--dxc-runtime" || index + 1 >= argc) {
            std::cerr << "HLSL-LSP analysis worker: invalid command line\n";
            return EXIT_FAILURE;
        }
        runtime.directory = argv[++index];
    }
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::cerr << "HLSL-LSP analysis worker: unable to configure binary stdio\n";
        return EXIT_FAILURE;
    }
#endif
    return hlsl_intellisense::analysis::run_analysis_worker(std::cin, std::cout, std::cerr,
                                                            runtime);
}
