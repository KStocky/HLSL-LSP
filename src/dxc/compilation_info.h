#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

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

// Compiles the actual root source and every resolved in-memory include source
// using the exact effective compiler arguments (unmodified: no synthetic
// wrapper source and no argument substitution), then extracts the
// compiler-authoritative result. DXIL output is reflected through
// IDxcUtils::CreateReflection and ID3D12ShaderReflection. SPIR-V output is
// still compiled and its diagnostics/size reported, but reflection is marked
// unavailable rather than fabricated. DXC is the sole authority; there is no
// fallback parser or manual layout engine. Compiler failures are reported
// through the returned `success` flag and `diagnostics`, never thrown.
[[nodiscard]] CompilationInfo compilation_info_from_compile(
    DxcCreateInstanceProc create_instance, const std::vector<SourceFile>& sources,
    const std::vector<std::string>& arguments, std::string_view main_path);

} // namespace hlsl_intellisense::dxc::detail
