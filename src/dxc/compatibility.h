#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <string_view>
#include <vector>

namespace hlsl_intellisense::dxc::detail {

// Compares reflected shader resources against a deserialized root signature
// for `stage` (the compiled entry point's shader stage, e.g. "pixel"),
// reporting only conclusions provable from register class, space, range,
// active-stage shader visibility, and static samplers. Never guesses at
// bindless (ResourceDescriptorHeap/SamplerDescriptorHeap) coverage, since
// those accesses are invisible to reflection entirely. Reports
// ResourceCompatibilityStatus::unknown (with an explanation, not a fabricated
// verdict) whenever the root signature is absent, not applicable, or its
// details are unavailable on this platform.
[[nodiscard]] CompilationCompatibility
analyze_compatibility(const std::vector<CompilationResourceBinding>& resources,
                      const RootSignatureInfo& root_signature, std::string_view stage);

} // namespace hlsl_intellisense::dxc::detail
