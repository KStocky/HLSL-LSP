#include <hlsl_intellisense/dxc/proof_of_concept.h>

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        const auto result = hlsl_intellisense::dxc::run_proof_of_concept();
        if (!result.parsed_hlsl_2021 || !result.produced_no_diagnostics ||
            !result.completed_user_symbol || !result.resolved_template_definition ||
            !result.reparsed_updated_symbol) {
            std::cerr << "error: DXC IntelliSense proof of concept was incomplete\n";
            return EXIT_FAILURE;
        }

        std::cout << "DXC IntelliSense proof of concept succeeded\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
