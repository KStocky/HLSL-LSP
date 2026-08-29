#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include "WinAdapter.h"
#endif

#include <dxcapi.h>

namespace hlsl_intellisense::dxc::detail {

/// Target identified by DXC cursor APIs for the memory layout probe.
struct ProbeTarget {
    std::string type_name;       ///< Struct type name (e.g. "Data").
    std::string cbuffer_name;    ///< Non-empty when probing a cbuffer.
    std::string selected_field;  ///< Field name if cursor was on a field.
    std::string reference_var;   ///< Cbuffer variable to reference in probe entry point.
};

/// Compile a synthetic DXC probe and extract compiler-authoritative layout via
/// ID3D12ShaderReflection. This is the sole layout path; there is no fallback
/// parser or manual layout engine.
[[nodiscard]] std::optional<MemoryLayout>
memory_layout_from_probe(DxcCreateInstanceProc create_instance,
                         const std::vector<SourceFile>& sources,
                         const std::vector<std::string>& arguments,
                         std::string_view main_path,
                         const ProbeTarget& target);

} // namespace hlsl_intellisense::dxc::detail
