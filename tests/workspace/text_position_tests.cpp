#include <hlsl_intellisense/workspace/text_position.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace workspace = hlsl_intellisense::workspace;

namespace {

template <typename Function>
[[nodiscard]] std::optional<workspace::DocumentErrorCode> error_from(Function&& function) {
    try {
        function();
    } catch (const workspace::DocumentError& error) {
        return error.code();
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("ASCII positions use zero-based lines and characters", "[workspace][position]") {
    constexpr std::string_view text = "abc\ndef";

    CHECK(workspace::utf8_offset_at(text, {.line = 0, .character = 0}) == 0);
    CHECK(workspace::utf8_offset_at(text, {.line = 0, .character = 3}) == 3);
    CHECK(workspace::utf8_offset_at(text, {.line = 1, .character = 2}) == 6);
    CHECK(workspace::lsp_position_at(text, 4) == workspace::Position{.line = 1, .character = 0});
}

TEST_CASE("LF, CRLF, and CR line endings map consistently", "[workspace][position]") {
    constexpr std::string_view text = "a\r\nbb\nc\rd";

    CHECK(workspace::utf8_offset_at(text, {.line = 1, .character = 0}) == 3);
    CHECK(workspace::utf8_offset_at(text, {.line = 2, .character = 1}) == 7);
    CHECK(workspace::utf8_offset_at(text, {.line = 3, .character = 0}) == 8);
    CHECK(workspace::lsp_position_at(text, 1) == workspace::Position{.line = 0, .character = 1});
    CHECK(error_from([&] { static_cast<void>(workspace::lsp_position_at(text, 2)); }) ==
          workspace::DocumentErrorCode::invalid_position);
}

TEST_CASE("UTF-16 positions account for BMP and non-BMP characters",
          "[workspace][position][unicode]") {
    const std::string text = "a\xC3\xA9\xF0\x9F\x98\x80z";

    CHECK(workspace::utf8_offset_at(text, {.line = 0, .character = 1}) == 1);
    CHECK(workspace::utf8_offset_at(text, {.line = 0, .character = 2}) == 3);
    CHECK(workspace::utf8_offset_at(text, {.line = 0, .character = 4}) == 7);
    CHECK(workspace::lsp_position_at(text, 7) == workspace::Position{.line = 0, .character = 4});
    CHECK(workspace::utf16_length(text) == 5);
    CHECK(error_from([&] {
              static_cast<void>(workspace::utf8_offset_at(text, {.line = 0, .character = 3}));
          }) == workspace::DocumentErrorCode::invalid_position);
    CHECK(error_from([&] { static_cast<void>(workspace::lsp_position_at(text, 5)); }) ==
          workspace::DocumentErrorCode::invalid_position);
}

TEST_CASE("Out-of-range positions and malformed UTF-8 are explicit errors",
          "[workspace][position][unicode]") {
    CHECK(error_from([] {
              static_cast<void>(workspace::utf8_offset_at("abc", {.line = 1, .character = 0}));
          }) == workspace::DocumentErrorCode::invalid_position);
    CHECK(error_from([] {
              static_cast<void>(workspace::utf8_offset_at("abc", {.line = 0, .character = 4}));
          }) == workspace::DocumentErrorCode::invalid_position);

    const std::string truncated = "\xF0\x9F\x98";
    const std::string overlong = "\xC0\xAF";
    CHECK(error_from([&] { static_cast<void>(workspace::utf16_length(truncated)); }) ==
          workspace::DocumentErrorCode::malformed_utf8);
    CHECK(error_from([&] { static_cast<void>(workspace::utf16_length(overlong)); }) ==
          workspace::DocumentErrorCode::malformed_utf8);
}
