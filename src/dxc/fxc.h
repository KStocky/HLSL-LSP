#pragma once

#include "compilation_info.h"
#include "memory_layout.h"

namespace hlsl_intellisense::dxc::detail {

// Failures are returned as diagnostics, including invalid runtime/options and
// non-Windows capability errors. Reflection never falls back to DXC.
// Embedded root-signature presence uses D3DGetBlobPart; deserialization and
// compatibility remain unavailable.
// Diagnostic byte columns are converted to one-based UTF-16 using the supplied
// snapshots. Unresolvable locations have column zero (unavailable).
// Native compilation defaults to Flags1=0 and Flags2=0; /Ges is explicit.
[[nodiscard]] CompilationInfo compilation_info_from_fxc(const std::vector<SourceFile>& sources,
                                                        const CompilerOptions& options,
                                                        std::string_view main_path);

// D3DPreprocess accepts macros/includes, but no compilation flags. Additional
// arguments are validated; flags such as /WX apply only to compilation.
[[nodiscard]] PreprocessOutput preprocess_from_fxc(const std::vector<SourceFile>& sources,
                                                   const CompilerOptions& options,
                                                   std::string_view main_path);

// Reflects the original compiled cbuffer, never a synthetic probe. Natural
// layouts, alignment and strides not exposed by FXC remain unavailable.
[[nodiscard]] std::optional<MemoryLayout>
memory_layout_from_fxc(const std::vector<SourceFile>& sources, const CompilerOptions& options,
                       std::string_view main_path, const ProbeTarget& target);

} // namespace hlsl_intellisense::dxc::detail
