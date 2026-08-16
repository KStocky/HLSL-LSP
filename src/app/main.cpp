#include <hlsl_intellisense/lsp/server.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char* argv[]) {
    hlsl_intellisense::lsp::ServerOptions options;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == "--disable-semantic-tokens") {
            options.semantic_tokens = false;
        } else {
            std::cerr << "HLSL-LSP: unknown argument: " << argv[index] << '\n';
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
