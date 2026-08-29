#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::dxc::detail {

[[nodiscard]] std::optional<MemoryLayout>
memory_layout_at(const std::vector<SourceFile>& sources, std::string_view path, std::uint32_t line,
                 std::uint32_t column, bool native_16_bit_types, bool default_row_major);

} // namespace hlsl_intellisense::dxc::detail
