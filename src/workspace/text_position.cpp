#include <hlsl_intellisense/workspace/text_position.h>

#include <limits>
#include <string>
#include <utility>

namespace hlsl_intellisense::workspace {
namespace {

struct CodePoint {
    char32_t value;
    std::size_t bytes;
};

[[noreturn]] void malformed() {
    throw DocumentError{DocumentErrorCode::malformed_utf8, "Text contains malformed UTF-8"};
}

[[nodiscard]] CodePoint decode(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        return {.value = first, .bytes = 1};
    }

    std::size_t bytes = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        bytes = 2;
        value = first & 0x1F;
        minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
        bytes = 3;
        value = first & 0x0F;
        minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
        bytes = 4;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        malformed();
    }

    if (offset + bytes > text.size()) {
        malformed();
    }
    for (std::size_t index = 1; index < bytes; ++index) {
        const auto continuation = static_cast<unsigned char>(text[offset + index]);
        if ((continuation & 0xC0) != 0x80) {
            malformed();
        }
        value = static_cast<char32_t>((value << 6) | (continuation & 0x3F));
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        malformed();
    }
    return {.value = value, .bytes = bytes};
}

void validate(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        offset += decode(text, offset).bytes;
    }
}

[[nodiscard]] std::pair<std::size_t, std::size_t> line_bounds(std::string_view text,
                                                              std::uint32_t target_line) {
    std::size_t start = 0;
    std::uint32_t line = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (text[offset] != '\r' && text[offset] != '\n') {
            continue;
        }
        if (line == target_line) {
            return {start, offset};
        }
        if (text[offset] == '\r' && offset + 1 < text.size() && text[offset + 1] == '\n') {
            ++offset;
        }
        ++line;
        start = offset + 1;
    }
    if (line == target_line) {
        return {start, text.size()};
    }
    throw DocumentError{DocumentErrorCode::invalid_position, "Line is outside the document"};
}

[[nodiscard]] std::uint32_t checked_character(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw DocumentError{DocumentErrorCode::invalid_position, "Character offset is too large"};
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

std::size_t utf8_offset_at(std::string_view text, Position position) {
    validate(text);
    const auto [start, end] = line_bounds(text, position.line);
    std::size_t utf16_offset = 0;
    for (auto offset = start; offset < end;) {
        if (utf16_offset == position.character) {
            return offset;
        }
        const auto code_point = decode(text, offset);
        const std::size_t units = code_point.value > 0xFFFF ? 2 : 1;
        if (utf16_offset + units > position.character) {
            throw DocumentError{DocumentErrorCode::invalid_position,
                                "Position splits a UTF-16 surrogate pair"};
        }
        utf16_offset += units;
        offset += code_point.bytes;
    }
    if (utf16_offset == position.character) {
        return end;
    }
    throw DocumentError{DocumentErrorCode::invalid_position,
                        "Character is outside the requested line"};
}

Position lsp_position_at(std::string_view text, std::size_t utf8_offset) {
    validate(text);
    if (utf8_offset > text.size()) {
        throw DocumentError{DocumentErrorCode::invalid_position,
                            "UTF-8 offset is outside the document"};
    }

    Position position{};
    for (std::size_t offset = 0; offset < text.size();) {
        if (offset == utf8_offset) {
            return position;
        }
        if (text[offset] == '\r' || text[offset] == '\n') {
            const auto newline_bytes =
                text[offset] == '\r' && offset + 1 < text.size() && text[offset + 1] == '\n' ? 2U
                                                                                             : 1U;
            if (utf8_offset > offset && utf8_offset < offset + newline_bytes) {
                throw DocumentError{DocumentErrorCode::invalid_position,
                                    "Offset splits a CRLF sequence"};
            }
            offset += newline_bytes;
            ++position.line;
            position.character = 0;
            continue;
        }

        const auto code_point = decode(text, offset);
        if (utf8_offset > offset && utf8_offset < offset + code_point.bytes) {
            throw DocumentError{DocumentErrorCode::invalid_position,
                                "Offset splits a UTF-8 sequence"};
        }
        position.character = checked_character(static_cast<std::size_t>(position.character) +
                                               (code_point.value > 0xFFFF ? 2 : 1));
        offset += code_point.bytes;
    }
    if (utf8_offset == text.size()) {
        return position;
    }
    throw DocumentError{DocumentErrorCode::invalid_position,
                        "UTF-8 offset is outside the document"};
}

std::size_t utf16_length(std::string_view text) {
    std::size_t length = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto code_point = decode(text, offset);
        length += code_point.value > 0xFFFF ? 2 : 1;
        offset += code_point.bytes;
    }
    return length;
}

} // namespace hlsl_intellisense::workspace
