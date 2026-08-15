#pragma once

namespace hlsl_intellisense::dxc {

struct ProofOfConceptResult {
    bool parsed_hlsl_2021{};
    bool produced_no_diagnostics{};
    bool completed_user_symbol{};
    bool resolved_template_definition{};
    bool reparsed_updated_symbol{};
};

[[nodiscard]] ProofOfConceptResult run_proof_of_concept();

} // namespace hlsl_intellisense::dxc
