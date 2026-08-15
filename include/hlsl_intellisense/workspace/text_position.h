#pragma once

#include <hlsl_intellisense/workspace/error.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hlsl_intellisense::workspace {

struct Position {
    std::uint32_t line{};
    std::uint32_t character{};

    bool operator==(const Position&) const = default;
};

struct Range {
    Position start;
    Position end;

    bool operator==(const Range&) const = default;
};

[[nodiscard]] std::size_t utf8_offset_at(std::string_view text, Position position);
[[nodiscard]] Position lsp_position_at(std::string_view text, std::size_t utf8_offset);
[[nodiscard]] std::size_t utf16_length(std::string_view text);

} // namespace hlsl_intellisense::workspace
