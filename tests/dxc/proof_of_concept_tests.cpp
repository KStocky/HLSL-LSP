#include <hlsl_intellisense/dxc/proof_of_concept.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("DXC IntelliSense analyzes HLSL 2021", "[dxc][integration]") {
    const auto result = hlsl_intellisense::dxc::run_proof_of_concept();

    CHECK(result.parsed_hlsl_2021);
    CHECK(result.produced_no_diagnostics);
    CHECK(result.completed_user_symbol);
    CHECK(result.resolved_template_definition);
    CHECK(result.reparsed_updated_symbol);
}
