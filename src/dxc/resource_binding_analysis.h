#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace hlsl_intellisense::dxc::detail {

// D3D12 reserves register spaces [0xfffffff0, 0xffffffff] for system/driver
// use; resources or root-signature entries in this range are never
// user-addressable.
inline constexpr std::uint32_t kSystemReservedSpaceBegin = 0xfffffff0U;

[[nodiscard]] inline bool is_system_reserved_space(std::uint32_t space) noexcept {
    return space >= kSystemReservedSpaceBegin;
}

// Safe end-register computation for a finite range: base + count - 1,
// computed in 64-bit arithmetic and clamped to UINT32_MAX so a resource
// declaring an implausibly large base register/count can never wrap around.
// Shared by resource_binding_analysis.cpp and compatibility.cpp so both
// perform the same overflow-safe register-range arithmetic.
[[nodiscard]] inline std::uint32_t safe_end_register(std::uint32_t base,
                                                     std::uint32_t count) noexcept {
    const auto end = static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(count) - 1U;
    return end > (std::numeric_limits<std::uint32_t>::max)()
               ? (std::numeric_limits<std::uint32_t>::max)()
               : static_cast<std::uint32_t>(end);
}

// Groups already-reflected resources by register class and register space,
// and detects provable overlapping register ranges between distinct
// resources within the same group. This is pure post-processing over
// compiler-reported register data (bind_point/bind_count/space/unbounded);
// it never parses HLSL or infers bindings the compiler did not report.
[[nodiscard]] ResourceBindingAnalysis
analyze_resource_bindings(const std::vector<CompilationResourceBinding>& resources);

} // namespace hlsl_intellisense::dxc::detail
