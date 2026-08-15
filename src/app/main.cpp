#include <hlsl_intellisense/dxc/intellisense.h>

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        constexpr auto file_name = "prototype.hlsl";
        constexpr auto source =
            "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

        hlsl_intellisense::dxc::Intellisense intellisense;
        const auto translation_unit = intellisense.parse(file_name, {{file_name, source}});
        if (!translation_unit.diagnostics().empty()) {
            std::cerr << "error: valid HLSL produced diagnostics\n";
            return EXIT_FAILURE;
        }

        std::cout << "HLSL-LSP DXC analysis succeeded\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
