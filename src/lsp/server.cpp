#include <hlsl_intellisense/lsp/server.h>

#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/error.h>
#include <hlsl_intellisense/workspace/include_resolver.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace hlsl_intellisense::lsp {
namespace {

using json_rpc::HandlerError;
using json_rpc::Json;

[[noreturn]] void invalid_params(std::string_view message) {
    throw HandlerError{json_rpc::invalid_params_code, message};
}

[[nodiscard]] const Json& object_params(const std::optional<Json>& params) {
    if (!params.has_value() || !params->is_object()) {
        invalid_params("Expected object parameters");
    }
    return *params;
}

[[nodiscard]] const Json& member(const Json& object, std::string_view name) {
    const auto item = object.find(name);
    if (item == object.end()) {
        invalid_params(std::string{"Missing parameter: "} + std::string{name});
    }
    return *item;
}

[[nodiscard]] const Json& object_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_object()) {
        invalid_params(std::string{"Expected object: "} + std::string{name});
    }
    return value;
}

[[nodiscard]] std::string string_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_string()) {
        invalid_params(std::string{"Expected string: "} + std::string{name});
    }
    return value.get<std::string>();
}

[[nodiscard]] std::int64_t integer_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_number_integer()) {
        invalid_params(std::string{"Expected integer: "} + std::string{name});
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] std::uint32_t unsigned_member(const Json& object, std::string_view name) {
    const auto value = integer_member(object, name);
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        invalid_params(std::string{"Expected non-negative 32-bit integer: "} + std::string{name});
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] workspace::Position position(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected position object");
    }
    return {.line = unsigned_member(value, "line"),
            .character = unsigned_member(value, "character")};
}

[[nodiscard]] workspace::Range range(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected range object");
    }
    return {.start = position(object_member(value, "start")),
            .end = position(object_member(value, "end"))};
}

[[nodiscard]] Json lsp_position(workspace::Position value) {
    return {{"line", value.line}, {"character", value.character}};
}

[[nodiscard]] Json lsp_range(workspace::Range value) {
    return {{"start", lsp_position(value.start)}, {"end", lsp_position(value.end)}};
}

[[nodiscard]] int diagnostic_severity(dxc::DiagnosticSeverity severity) {
    switch (severity) {
    case dxc::DiagnosticSeverity::ignored:
    case dxc::DiagnosticSeverity::note:
        return 3;
    case dxc::DiagnosticSeverity::warning:
        return 2;
    case dxc::DiagnosticSeverity::error:
    case dxc::DiagnosticSeverity::fatal:
        return 1;
    }
    return 1;
}

[[nodiscard]] int completion_kind(std::uint32_t cursor_kind) {
    if (cursor_kind == 6) {
        return 5;
    }
    if (cursor_kind == 7) {
        return 20;
    }
    if (cursor_kind == 8 || cursor_kind == 30) {
        return 3;
    }
    if (cursor_kind == 9 || cursor_kind == 10 || cursor_kind == 50) {
        return 6;
    }
    if (cursor_kind == 21 || cursor_kind == 26) {
        return 2;
    }
    if (cursor_kind == 24) {
        return 4;
    }
    if (cursor_kind == 5) {
        return 13;
    }
    if (cursor_kind == 22) {
        return 9;
    }
    if (cursor_kind == 2 || cursor_kind == 3 || cursor_kind == 4 || cursor_kind == 20 ||
        cursor_kind == 27 || cursor_kind == 31 || cursor_kind == 36 || cursor_kind == 43 ||
        cursor_kind == 45) {
        return 7;
    }
    if (cursor_kind == 501 || cursor_kind == 502) {
        return 14;
    }
    return 1;
}

[[nodiscard]] int symbol_kind(std::uint32_t cursor_kind, std::string_view name) {
    if (name.starts_with("operator")) {
        return 25;
    }
    switch (cursor_kind) {
    case 2:
    case 3:
        return 23;
    case 4:
    case 31:
    case 32:
        return 5;
    case 5:
        return 10;
    case 6:
        return 8;
    case 7:
        return 22;
    case 8:
    case 30:
        return 12;
    case 9:
        return 13;
    case 20:
    case 36:
        return 5;
    case 21:
    case 25:
        return 6;
    case 22:
        return 3;
    case 24:
        return 9;
    case 26:
        return 25;
    case 27:
    case 28:
    case 29:
        return 26;
    case 501:
        return 14;
    default:
        return 13;
    }
}

[[nodiscard]] std::string_view symbol_detail(const dxc::Symbol& symbol) {
    switch (symbol_kind(symbol.cursor_kind, symbol.name)) {
    case 3:
        return "HLSL namespace";
    case 5:
        return "HLSL type";
    case 6:
        return "HLSL method";
    case 8:
        return "HLSL field";
    case 9:
        return "HLSL constructor";
    case 10:
        return "HLSL enum";
    case 12:
        return "HLSL function";
    case 14:
        return "HLSL macro";
    case 22:
        return "HLSL enum member";
    case 23:
        return "HLSL struct";
    case 25:
        return "HLSL operator";
    case 26:
        return "HLSL type parameter";
    default:
        return "HLSL variable";
    }
}

[[nodiscard]] bool symbol_is_in_document(const dxc::Symbol& symbol,
                                         const workspace::SourceSnapshot& snapshot) {
    try {
        return workspace::DocumentUri::from_path(symbol.location.path).identity() ==
               snapshot.document_uri().identity();
    } catch (const workspace::DocumentError&) {
        return false;
    }
}

[[nodiscard]] std::size_t symbol_offset(std::string_view text, std::size_t offset,
                                        bool end_offset) {
    offset = (std::min)(offset, text.size());
    if (offset > 0 && offset < text.size() && text[offset - 1] == '\r' && text[offset] == '\n') {
        return end_offset ? offset - 1 : offset + 1;
    }
    return offset;
}

[[nodiscard]] workspace::Range symbol_range(const dxc::Symbol& symbol,
                                            const workspace::SourceSnapshot& snapshot) {
    const auto start =
        symbol_offset(snapshot.text(), static_cast<std::size_t>(symbol.start_offset), false);
    const auto normalized_end = symbol_offset(
        snapshot.text(), (std::max)(static_cast<std::size_t>(symbol.end_offset), start), true);
    const auto end = (std::max)(normalized_end, start);
    return {.start = workspace::lsp_position_at(snapshot.text(), start),
            .end = workspace::lsp_position_at(snapshot.text(), end)};
}

[[nodiscard]] workspace::Range symbol_selection_range(const dxc::Symbol& symbol,
                                                      const workspace::SourceSnapshot& snapshot) {
    const auto text_size = snapshot.text().size();
    const auto start =
        symbol_offset(snapshot.text(), static_cast<std::size_t>(symbol.location.offset), false);
    auto source_offset = start;
    auto name_offset = std::size_t{};
    while (source_offset < text_size && name_offset < symbol.name.size()) {
        if (snapshot.text()[source_offset] == symbol.name[name_offset]) {
            ++source_offset;
            ++name_offset;
        } else if (snapshot.text()[source_offset] == ' ' ||
                   snapshot.text()[source_offset] == '\t') {
            ++source_offset;
        } else {
            break;
        }
    }
    const auto end = symbol_offset(snapshot.text(),
                                   name_offset == symbol.name.size()
                                       ? source_offset
                                       : (std::min)(start + symbol.name.size(), text_size),
                                   true);
    return {.start = workspace::lsp_position_at(snapshot.text(), start),
            .end = workspace::lsp_position_at(snapshot.text(), end)};
}

void append_document_symbols(Json& output, const std::vector<dxc::Symbol>& symbols,
                             const workspace::SourceSnapshot& snapshot) {
    for (const auto& symbol : symbols) {
        Json children = Json::array();
        append_document_symbols(children, symbol.children, snapshot);
        if (!symbol_is_in_document(symbol, snapshot)) {
            output.insert(output.end(), children.begin(), children.end());
            continue;
        }

        Json item{{"name", symbol.name},
                  {"detail", symbol_detail(symbol)},
                  {"kind", symbol_kind(symbol.cursor_kind, symbol.name)},
                  {"range", lsp_range(symbol_range(symbol, snapshot))},
                  {"selectionRange", lsp_range(symbol_selection_range(symbol, snapshot))}};
        if (!children.empty()) {
            item["children"] = std::move(children);
        }
        output.push_back(std::move(item));
    }
}

[[nodiscard]] bool contains_case_insensitive(std::string_view text, std::string_view query) {
    return std::ranges::search(text, query, [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) ==
                      std::tolower(static_cast<unsigned char>(right));
           }).begin() != text.end();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void append_workspace_symbols(Json& output, const std::vector<dxc::Symbol>& symbols,
                              const workspace::SourceSnapshot& snapshot, std::string_view query,
                              std::string_view container) {
    for (const auto& symbol : symbols) {
        if (symbol_is_in_document(symbol, snapshot) &&
            contains_case_insensitive(symbol.name, query)) {
            auto container_name = std::string{"HLSL"};
            if (!container.empty()) {
                container_name += " \xC2\xB7 ";
                container_name += container;
            }
            output.push_back({{"name", symbol.name},
                              {"kind", symbol_kind(symbol.cursor_kind, symbol.name)},
                              {"location",
                               {{"uri", snapshot.uri()},
                                {"range", lsp_range(symbol_selection_range(symbol, snapshot))}}},
                              {"containerName", std::move(container_name)}});
        }

        auto nested_container = std::string{container};
        if (symbol_is_in_document(symbol, snapshot)) {
            if (!nested_container.empty()) {
                nested_container += "::";
            }
            nested_container += symbol.name;
        }
        append_workspace_symbols(output, symbol.children, snapshot, query, nested_container);
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

enum class SemanticTokenType : std::uint8_t {
    namespace_name,
    type,
    class_name,
    enum_name,
    parameter,
    variable,
    property,
    enum_member,
    function,
    method,
    macro,
    keyword,
    comment,
    string,
    number,
    type_parameter
};

struct SemanticToken {
    workspace::Position start;
    std::uint32_t length{};
    SemanticTokenType type{};
};

[[nodiscard]] SemanticTokenType identifier_token_type(std::uint32_t cursor_kind) {
    if (cursor_kind == 22 || cursor_kind == 33 || cursor_kind == 46) {
        return SemanticTokenType::namespace_name;
    }
    if (cursor_kind == 2 || cursor_kind == 3 || cursor_kind == 4 || cursor_kind == 31 ||
        cursor_kind == 32) {
        return SemanticTokenType::class_name;
    }
    if (cursor_kind == 5) {
        return SemanticTokenType::enum_name;
    }
    if (cursor_kind == 6 || cursor_kind == 47 || cursor_kind == 102) {
        return SemanticTokenType::property;
    }
    if (cursor_kind == 7) {
        return SemanticTokenType::enum_member;
    }
    if (cursor_kind == 8 || cursor_kind == 30 || cursor_kind == 103) {
        return SemanticTokenType::function;
    }
    if (cursor_kind == 21 || cursor_kind == 24 || cursor_kind == 25 || cursor_kind == 26) {
        return SemanticTokenType::method;
    }
    if (cursor_kind == 10 || cursor_kind == 28) {
        return SemanticTokenType::parameter;
    }
    if (cursor_kind == 20 || cursor_kind == 36 || cursor_kind == 43 || cursor_kind == 45) {
        return SemanticTokenType::type;
    }
    if (cursor_kind == 27 || cursor_kind == 29) {
        return SemanticTokenType::type_parameter;
    }
    if (cursor_kind == 501 || cursor_kind == 502) {
        return SemanticTokenType::macro;
    }
    return SemanticTokenType::variable;
}

[[nodiscard]] SemanticTokenType semantic_token_type(const dxc::Token& token, std::string_view text,
                                                    std::size_t offset) {
    switch (token.kind) {
    case dxc::TokenKind::keyword:
        return SemanticTokenType::keyword;
    case dxc::TokenKind::built_in_type:
        return SemanticTokenType::type;
    case dxc::TokenKind::comment:
        return SemanticTokenType::comment;
    case dxc::TokenKind::literal:
        return offset < text.size() && (text[offset] == '"' || text[offset] == '\'')
                   ? SemanticTokenType::string
                   : SemanticTokenType::number;
    case dxc::TokenKind::identifier:
        return identifier_token_type(token.cursor_kind);
    case dxc::TokenKind::punctuation:
    case dxc::TokenKind::unknown:
        return SemanticTokenType::variable;
    }
    return SemanticTokenType::variable;
}

[[nodiscard]] std::optional<std::size_t> dxc_offset_at(std::string_view text, std::uint32_t line,
                                                       std::uint32_t column) {
    if (line == 0 || column == 0) {
        return std::nullopt;
    }
    std::size_t line_start = 0;
    for (std::uint32_t current = 1; current < line; ++current) {
        const auto newline = text.find_first_of("\r\n", line_start);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        line_start = newline + 1;
        if (text[newline] == '\r' && line_start < text.size() && text[line_start] == '\n') {
            ++line_start;
        }
    }
    const auto offset = line_start + column - 1;
    const auto line_end = text.find_first_of("\r\n", line_start);
    if (offset > (line_end == std::string_view::npos ? text.size() : line_end)) {
        return std::nullopt;
    }
    return offset;
}

void append_semantic_token(std::vector<SemanticToken>& result, std::string_view text,
                           const dxc::Token& token) {
    const auto token_offset = dxc_offset_at(text, token.line, token.column);
    if (!token_offset.has_value() || *token_offset >= text.size() ||
        token.kind == dxc::TokenKind::punctuation || token.kind == dxc::TokenKind::unknown) {
        return;
    }

    auto token_end = std::min(text.size(), *token_offset + token.length);
    if (token.kind == dxc::TokenKind::literal &&
        (text[*token_offset] == '"' || text[*token_offset] == '\'')) {
        const auto quote = text[*token_offset];
        for (auto offset = *token_offset + 1; offset < text.size(); ++offset) {
            if (text[offset] == '\\') {
                ++offset;
            } else if (text[offset] == quote) {
                token_end = offset + 1;
                break;
            }
        }
    }
    const auto type = semantic_token_type(token, text, *token_offset);
    for (auto segment_start = *token_offset; segment_start < token_end;) {
        const auto newline = text.find_first_of("\r\n", segment_start);
        const auto segment_end =
            newline == std::string_view::npos ? token_end : std::min(token_end, newline);
        if (segment_end > segment_start) {
            const auto start = workspace::lsp_position_at(text, segment_start);
            const auto length =
                workspace::utf16_length(text.substr(segment_start, segment_end - segment_start));
            if (length <= std::numeric_limits<std::uint32_t>::max()) {
                result.push_back(
                    {.start = start, .length = static_cast<std::uint32_t>(length), .type = type});
            }
        }
        if (segment_end == token_end) {
            break;
        }
        segment_start = segment_end + 1;
        if (text[segment_end] == '\r' && segment_start < token_end && text[segment_start] == '\n') {
            ++segment_start;
        }
    }
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
dxc_position(std::string_view text, workspace::Position request_position) {
    const auto offset = workspace::utf8_offset_at(text, request_position);
    std::size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    const auto byte_column = offset - line_start + 1;
    if (byte_column > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Completion position is too large");
    }
    return {request_position.line + 1, static_cast<std::uint32_t>(byte_column)};
}

struct CallContext {
    std::size_t callee_offset{};
    std::size_t active_parameter{};
};

enum class LexicalState : std::uint8_t { code, line_comment, block_comment, string, character };

struct LexicalPrefix {
    std::vector<bool> code;
    LexicalState state{LexicalState::code};
};

[[nodiscard]] LexicalPrefix lexical_prefix(std::string_view text, std::size_t limit) {
    LexicalPrefix result{.code = std::vector<bool>(limit, false)};
    for (std::size_t offset = 0; offset < limit;) {
        const auto character = text[offset];
        switch (result.state) {
        case LexicalState::code:
            if (character == '/' && offset + 1 < limit && text[offset + 1] == '/') {
                result.state = LexicalState::line_comment;
                offset += 2;
            } else if (character == '/' && offset + 1 < limit && text[offset + 1] == '*') {
                result.state = LexicalState::block_comment;
                offset += 2;
            } else if (character == '"') {
                result.state = LexicalState::string;
                ++offset;
            } else if (character == '\'') {
                result.state = LexicalState::character;
                ++offset;
            } else {
                result.code[offset] = true;
                ++offset;
            }
            break;
        case LexicalState::line_comment:
            if (character == '\r' || character == '\n') {
                result.state = LexicalState::code;
                result.code[offset] = true;
            }
            ++offset;
            break;
        case LexicalState::block_comment:
            if (character == '*' && offset + 1 < limit && text[offset + 1] == '/') {
                result.state = LexicalState::code;
                offset += 2;
            } else {
                ++offset;
            }
            break;
        case LexicalState::string:
        case LexicalState::character: {
            const auto quote = result.state == LexicalState::string ? '"' : '\'';
            if (character == '\\' && offset + 1 < limit) {
                offset += 2;
            } else {
                ++offset;
                if (character == quote) {
                    result.state = LexicalState::code;
                }
            }
            break;
        }
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t>
previous_code_offset(std::string_view text, const std::vector<bool>& code, std::size_t offset) {
    while (offset > 0) {
        --offset;
        if (code[offset] && std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
            return offset;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool template_close_follows(std::string_view text, const std::vector<bool>& code,
                                          std::size_t open) {
    std::size_t depth = 1;
    for (auto offset = open + 1; offset < code.size(); ++offset) {
        if (!code[offset]) {
            continue;
        }
        if (text[offset] == '<') {
            ++depth;
        } else if (text[offset] == '>') {
            if (--depth == 0) {
                auto next = offset + 1;
                while (next < code.size() &&
                       (!code[next] || std::isspace(static_cast<unsigned char>(text[next])) != 0)) {
                    ++next;
                }
                return next < code.size() && text[next] == '(';
            }
        } else if ((text[offset] == ';' || text[offset] == ')' || text[offset] == ']' ||
                    text[offset] == '}') &&
                   depth == 1) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool template_open(std::string_view text, const std::vector<bool>& code,
                                 std::size_t offset) {
    const auto previous = previous_code_offset(text, code, offset);
    if (!previous.has_value()) {
        return false;
    }
    const auto character = text[*previous];
    const auto possible_name = std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                               character == '_' || character == '>' || character == ']';
    return possible_name && template_close_follows(text, code, offset);
}

[[nodiscard]] std::optional<std::size_t>
callee_at(std::string_view text, const std::vector<bool>& code, std::size_t open_parenthesis) {
    auto previous = previous_code_offset(text, code, open_parenthesis);
    if (!previous.has_value()) {
        return std::nullopt;
    }
    if (text[*previous] == '>') {
        std::size_t depth = 1;
        auto offset = *previous;
        while (offset > 0 && depth != 0) {
            --offset;
            if (!code[offset]) {
                continue;
            }
            if (text[offset] == '>') {
                ++depth;
            } else if (text[offset] == '<') {
                --depth;
            }
        }
        if (depth != 0) {
            return std::nullopt;
        }
        previous = previous_code_offset(text, code, offset);
        if (!previous.has_value()) {
            return std::nullopt;
        }
    }

    auto start = *previous;
    const auto identifier_character = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
    };
    if (!identifier_character(text[start])) {
        return std::nullopt;
    }
    while (start > 0 && code[start - 1] && identifier_character(text[start - 1])) {
        --start;
    }
    const auto name = text.substr(start, *previous - start + 1);
    static constexpr std::string_view non_call_keywords[] = {"if",     "for",     "while", "switch",
                                                             "sizeof", "alignof", "return"};
    if (std::ranges::find(non_call_keywords, name) != std::ranges::end(non_call_keywords)) {
        return std::nullopt;
    }
    return start;
}

[[nodiscard]] std::optional<CallContext> call_context(std::string_view text,
                                                      std::size_t cursor_offset) {
    const auto lexical = lexical_prefix(text, cursor_offset);
    if (lexical.state != LexicalState::code) {
        return std::nullopt;
    }

    struct Delimiter {
        char value{};
        std::optional<std::size_t> callee;
        std::size_t active_parameter{};
    };
    std::vector<Delimiter> delimiters;
    for (std::size_t offset = 0; offset < cursor_offset; ++offset) {
        if (!lexical.code[offset]) {
            continue;
        }
        const auto character = text[offset];
        if (character == '(') {
            delimiters.push_back({.value = character,
                                  .callee = callee_at(text, lexical.code, offset),
                                  .active_parameter = 0});
        } else if (character == '[' || character == '{' ||
                   (character == '<' && ((!delimiters.empty() && delimiters.back().value == '<') ||
                                         template_open(text, lexical.code, offset)))) {
            delimiters.push_back(
                {.value = character, .callee = std::nullopt, .active_parameter = 0});
        } else if (character == ')' || character == ']' || character == '}' || character == '>') {
            const auto expected =
                character == ')' ? '(' : (character == ']' ? '[' : (character == '}' ? '{' : '<'));
            const auto matching = std::ranges::find(delimiters.rbegin(), delimiters.rend(),
                                                    expected, &Delimiter::value);
            if (matching != delimiters.rend()) {
                delimiters.erase(matching.base() - 1, delimiters.end());
            }
        } else if (character == ',' && !delimiters.empty() && delimiters.back().value == '(' &&
                   delimiters.back().callee.has_value()) {
            ++delimiters.back().active_parameter;
        }
    }

    const auto call =
        std::ranges::find_if(delimiters.rbegin(), delimiters.rend(), [](const auto& delimiter) {
            return delimiter.value == '(' && delimiter.callee.has_value();
        });
    if (call == delimiters.rend()) {
        return std::nullopt;
    }
    return CallContext{.callee_offset = *call->callee, .active_parameter = call->active_parameter};
}

[[nodiscard]] workspace::Range diagnostic_range(const workspace::SourceSnapshot& snapshot,
                                                const dxc::Diagnostic& diagnostic) {
    if (diagnostic.location.line == 0 || diagnostic.location.column == 0) {
        return {};
    }

    std::size_t line_start = 0;
    for (std::uint32_t line = 1; line < diagnostic.location.line; ++line) {
        const auto newline = snapshot.text().find('\n', line_start);
        if (newline == std::string::npos) {
            return {};
        }
        line_start = newline + 1;
    }
    const auto line_end = snapshot.text().find_first_of("\r\n", line_start);
    const auto end_offset = line_end == std::string::npos ? snapshot.text().size() : line_end;
    const auto byte_column = static_cast<std::size_t>(diagnostic.location.column - 1);
    if (byte_column > end_offset - line_start) {
        return {};
    }

    const auto offset = line_start + byte_column;
    try {
        const auto start = workspace::lsp_position_at(snapshot.text(), offset);
        auto end = start;
        if (offset < snapshot.text().size() && snapshot.text()[offset] != '\r' &&
            snapshot.text()[offset] != '\n') {
            const auto first = static_cast<unsigned char>(snapshot.text()[offset]);
            const std::size_t bytes =
                first < 0x80 ? 1 : (first < 0xE0 ? 2 : (first < 0xF0 ? 3 : 4));
            end = workspace::lsp_position_at(snapshot.text(), offset + bytes);
        }
        return {.start = start, .end = end};
    } catch (const workspace::DocumentError&) {
        return {};
    }
}

[[nodiscard]] bool same_document_path(std::string_view left, std::string_view right) {
    try {
        return workspace::DocumentUri::from_path(left).identity() ==
               workspace::DocumentUri::from_path(right).identity();
    } catch (const workspace::DocumentError&) {
        return left == right;
    }
}

[[nodiscard]] std::string workspace_folder_identity(const Json& folder) {
    try {
        return workspace::DocumentUri::from_uri(string_member(folder, "uri")).identity();
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }
}

[[nodiscard]] std::pair<std::string, std::filesystem::path> workspace_folder(const Json& folder) {
    try {
        auto uri = workspace::DocumentUri::from_uri(string_member(folder, "uri"));
        return {uri.identity(), std::filesystem::path{uri.path()}};
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }
}

[[nodiscard]] const Json* setting(const Json& settings, const Json* hlsl, std::string_view name) {
    if (hlsl != nullptr) {
        const auto nested = hlsl->find(name);
        if (nested != hlsl->end()) {
            return &*nested;
        }
    }
    const auto dotted = settings.find("hlsl." + std::string{name});
    return dotted == settings.end() ? nullptr : &*dotted;
}

[[nodiscard]] std::string setting_value(std::string_view name, const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number() || value.is_boolean()) {
        return value.dump();
    }
    invalid_params("hlsl." + std::string{name} + " values must be strings, numbers, or booleans");
}

[[nodiscard]] std::optional<std::optional<std::string>>
optional_string_setting(const Json& settings, const Json* hlsl, std::string_view name) {
    const auto* value = setting(settings, hlsl, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_null()) {
        return std::optional<std::optional<std::string>>{std::in_place, std::nullopt};
    }
    if (!value->is_string()) {
        invalid_params("hlsl." + std::string{name} + " must be a string or null");
    }
    return std::optional<std::optional<std::string>>{std::in_place, value->get<std::string>()};
}

[[nodiscard]] workspace::ConfigurationOverrides configuration_overrides(const Json& settings) {
    if (!settings.is_object()) {
        invalid_params("settings must be an object");
    }
    const Json* hlsl = nullptr;
    if (const auto nested = settings.find("hlsl"); nested != settings.end()) {
        if (!nested->is_object()) {
            invalid_params("hlsl settings must be an object");
        }
        hlsl = &*nested;
    }

    workspace::ConfigurationOverrides result;
    if (const auto* definitions = setting(settings, hlsl, "preprocessorDefinitions")) {
        if (!definitions->is_object()) {
            invalid_params("hlsl.preprocessorDefinitions must be an object");
        }
        std::map<std::string, std::string, std::less<>> values;
        for (const auto& [name, value] : definitions->items()) {
            if (name.empty()) {
                invalid_params("Preprocessor definition names must not be empty");
            }
            values.emplace(name, setting_value("preprocessorDefinitions", value));
        }
        result.preprocessor_definitions = std::move(values);
    }

    const auto path_array =
        [&](std::string_view name) -> std::optional<std::vector<std::filesystem::path>> {
        const auto* value = setting(settings, hlsl, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_array()) {
            invalid_params("hlsl." + std::string{name} + " must be an array of strings");
        }
        std::vector<std::filesystem::path> paths;
        paths.reserve(value->size());
        for (const auto& path : *value) {
            if (!path.is_string()) {
                invalid_params("hlsl." + std::string{name} + " must be an array of strings");
            }
            paths.emplace_back(path.get_ref<const std::string&>());
        }
        return paths;
    };
    result.additional_include_directories = path_array("additionalIncludeDirectories");

    if (const auto* mappings = setting(settings, hlsl, "virtualDirectoryMappings")) {
        if (!mappings->is_object()) {
            invalid_params("hlsl.virtualDirectoryMappings must be an object of string paths");
        }
        std::map<std::string, std::filesystem::path, std::less<>> values;
        for (const auto& [virtual_directory, real_directory] : mappings->items()) {
            if (!real_directory.is_string()) {
                invalid_params("hlsl.virtualDirectoryMappings must be an object of string paths");
            }
            values.emplace(virtual_directory, real_directory.get_ref<const std::string&>());
        }
        result.virtual_directory_mappings = std::move(values);
    }

    result.language_version = optional_string_setting(settings, hlsl, "languageVersion");
    result.target_profile = optional_string_setting(settings, hlsl, "targetProfile");
    result.entry_point = optional_string_setting(settings, hlsl, "entryPoint");

    if (const auto* arguments = setting(settings, hlsl, "additionalArguments")) {
        if (!arguments->is_array()) {
            invalid_params("hlsl.additionalArguments must be an array of strings");
        }
        std::vector<std::string> values;
        values.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            if (!argument.is_string()) {
                invalid_params("hlsl.additionalArguments must be an array of strings");
            }
            values.push_back(argument.get<std::string>());
        }
        result.additional_arguments = std::move(values);
    }
    return result;
}

} // namespace

Server::Server(NotificationSender sender, Logger logger, ServerOptions options)
    : sender_{std::move(sender)}, logger_{std::move(logger)}, options_{std::move(options)},
      analysis_{[this](const auto& snapshot, const auto& diagnostics, std::uint64_t generation) {
                    analysis_completed(snapshot, diagnostics, generation);
                },
                options_.analysis, options_.analysis_hooks,
                [this](std::string_view message) { log(message); }} {
    if (!sender_) {
        throw std::invalid_argument{"The LSP server requires a notification sender"};
    }
    register_handlers();
}

Server::~Server() {
    cancel_all_requests();
    analysis_.shutdown();
}

void Server::register_handlers() {
    dispatcher_.register_request_handler("initialize",
                                         [this](const auto& params) { return initialize(params); });
    dispatcher_.register_request_handler("shutdown",
                                         [this](const auto& params) { return shutdown(params); });
    dispatcher_.register_request_handler(
        "textDocument/completion",
        [this](const auto& params, const auto& context) { return completion(params, context); });
    dispatcher_.register_request_handler(
        "textDocument/definition",
        [this](const auto& params, const auto& context) { return definition(params, context); });
    dispatcher_.register_request_handler(
        "textDocument/hover",
        [this](const auto& params, const auto& context) { return hover(params, context); });
    dispatcher_.register_request_handler("textDocument/signatureHelp",
                                         [this](const auto& params, const auto& context) {
                                             return signature_help(params, context);
                                         });
    dispatcher_.register_request_handler("textDocument/documentSymbol",
                                         [this](const auto& params, const auto& context) {
                                             return document_symbols(params, context);
                                         });
    dispatcher_.register_request_handler("workspace/symbol",
                                         [this](const auto& params, const auto& context) {
                                             return workspace_symbols(params, context);
                                         });
    if (options_.semantic_tokens) {
        dispatcher_.register_request_handler("textDocument/semanticTokens/full",
                                             [this](const auto& params, const auto& context) {
                                                 return semantic_tokens(params, context);
                                             });
    }
    dispatcher_.register_notification_handler("initialized",
                                              [this](const auto& params) { initialized(params); });
    dispatcher_.register_notification_handler("textDocument/didOpen",
                                              [this](const auto& params) { did_open(params); });
    dispatcher_.register_notification_handler("textDocument/didChange",
                                              [this](const auto& params) { did_change(params); });
    dispatcher_.register_notification_handler("textDocument/didSave",
                                              [this](const auto& params) { did_save(params); });
    dispatcher_.register_notification_handler("textDocument/didClose",
                                              [this](const auto& params) { did_close(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeConfiguration",
        [this](const auto& params) { did_change_configuration(params); });
    dispatcher_.register_notification_handler(
        "hlsl/didChangeClientDefaults",
        [this](const auto& params) { did_change_client_defaults(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeWorkspaceFolders",
        [this](const auto& params) { did_change_workspace_folders(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeWatchedFiles",
        [this](const auto& params) { did_change_watched_files(params); });
    dispatcher_.register_notification_handler("exit", [this](const auto& params) { exit(params); });
}

std::optional<json_rpc::DispatchResponse> Server::handle(const json_rpc::Message& message) {
    if (const auto* request = std::get_if<json_rpc::Request>(&message)) {
        return handle(*request, dispatcher_.begin_request(request->id));
    }
    return dispatcher_.dispatch(message);
}

json_rpc::DispatchResponse Server::handle(const json_rpc::Request& request,
                                          const json_rpc::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        dispatcher_.finish_request(request.id, cancellation);
        return json_rpc::ErrorResponse{.id = request.id,
                                       .error = {.code = json_rpc::request_cancelled_code,
                                                 .message = "Request cancelled",
                                                 .data = std::nullopt}};
    }
    {
        std::scoped_lock lock{state_mutex_};
        if (state_ == State::uninitialized && request.method != "initialize") {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::awaiting_initialized) {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::shutdown) {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = json_rpc::invalid_request_code,
                                                     .message = "Server has shut down",
                                                     .data = std::nullopt}};
        }
    }
    auto response = dispatcher_.dispatch(request, cancellation);
    dispatcher_.finish_request(request.id, cancellation);
    return response;
}

json_rpc::CancellationToken Server::begin_request(const json_rpc::RequestId& id) const {
    return dispatcher_.begin_request(id);
}

void Server::finish_request(const json_rpc::RequestId& id,
                            const json_rpc::CancellationToken& cancellation) const noexcept {
    dispatcher_.finish_request(id, cancellation);
}

void Server::cancel_all_requests() const noexcept { dispatcher_.cancel_all(); }

void Server::wait_for_analysis() { analysis_.wait_idle(); }

analysis::AnalysisMetrics Server::analysis_metrics() const noexcept { return analysis_.metrics(); }

bool Server::exit_requested() const noexcept {
    return exit_requested_.load(std::memory_order_acquire);
}

int Server::exit_code() const noexcept { return clean_shutdown_ ? 0 : 1; }

Json Server::initialize(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::uninitialized) {
        throw HandlerError{json_rpc::invalid_request_code, "Initialize may only be requested once"};
    }
    const auto& value = object_params(params);
    std::optional<std::string> client_default_language_version;
    if (const auto initialization_options = value.find("initializationOptions");
        initialization_options != value.end() && !initialization_options->is_null()) {
        const auto defaults = configuration_overrides(*initialization_options);
        if (defaults.language_version) {
            client_default_language_version = *defaults.language_version;
        }
    }
    std::unordered_map<std::string, std::filesystem::path> workspace_folders;
    if (const auto folders = value.find("workspaceFolders");
        folders != value.end() && !folders->is_null()) {
        if (!folders->is_array()) {
            invalid_params("workspaceFolders must be an array or null");
        }
        for (const auto& folder : *folders) {
            const auto [identity, path] = workspace_folder(folder);
            workspace_folders.insert_or_assign(identity, path);
        }
    } else if (const auto root_uri = value.find("rootUri");
               root_uri != value.end() && !root_uri->is_null()) {
        if (!root_uri->is_string()) {
            invalid_params("rootUri must be a string or null");
        }
        try {
            auto uri = workspace::DocumentUri::from_uri(root_uri->get_ref<const std::string&>());
            workspace_folders.insert_or_assign(uri.identity(), std::filesystem::path{uri.path()});
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }
    workspace_folders_ = std::move(workspace_folders);
    client_default_language_version_ = std::move(client_default_language_version);
    state_ = State::awaiting_initialized;
    Json capabilities = {
        {"positionEncoding", "utf-16"},
        {"textDocumentSync",
         {{"openClose", true}, {"change", 2}, {"save", {{"includeText", true}}}}},
        {"completionProvider", {{"resolveProvider", false}}},
        {"definitionProvider", true},
        {"hoverProvider", true},
        {"signatureHelpProvider",
         {{"triggerCharacters", Json::array({"(", ","})},
          {"retriggerCharacters", Json::array({")"})}}},
        {"documentSymbolProvider", true},
        {"workspaceSymbolProvider", true},
        {"workspace",
         {{"workspaceFolders", {{"supported", true}, {"changeNotifications", true}}}}}};
    if (options_.semantic_tokens) {
        capabilities["semanticTokensProvider"] = {
            {"legend",
             {{"tokenTypes",
               Json::array({"namespace", "type", "class", "enum", "parameter", "variable",
                            "property", "enumMember", "function", "method", "macro", "keyword",
                            "comment", "string", "number", "typeParameter"})},
              {"tokenModifiers", Json::array()}}},
            {"full", true},
            {"range", false}};
    }
    return {{"capabilities", std::move(capabilities)},
            {"serverInfo", {{"name", "HLSL-LSP"}, {"version", "0.4.0"}}}};
}

Json Server::shutdown(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (params.has_value() && !params->is_null()) {
        invalid_params("Shutdown does not accept parameters");
    }
    if (state_ != State::running) {
        throw HandlerError{json_rpc::invalid_request_code, "Shutdown is not valid now"};
    }
    state_ = State::shutdown;
    clean_shutdown_ = true;
    return nullptr;
}

Json Server::completion(const std::optional<Json>& params,
                        const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto& text_document = object_member(value, "textDocument");
    const auto uri = string_member(text_document, "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Completion document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto completions =
        analysis_.complete(snapshot.document_uri().identity(), snapshot.version(), snapshot.path(),
                           line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Completion was superseded"};
        }
    }

    Json items = Json::array();
    for (const auto& completion_item : completions) {
        items.push_back({{"label", completion_item.label},
                         {"detail", completion_item.detail},
                         {"kind", completion_kind(completion_item.cursor_kind)}});
    }
    return {{"isIncomplete", false}, {"items", std::move(items)}};
}

Json Server::definition(const std::optional<Json>& params,
                        const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Definition document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto utf8_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    workspace::WorkspaceConfiguration configuration;
    std::vector<workspace::SourceSnapshot> open_documents;
    {
        std::scoped_lock state_lock{state_mutex_};
        configuration = configuration_for(snapshot, editor_settings_);
        open_documents = documents_.open_snapshots();
    }
    context.cancellation.throw_if_cancellation_requested();
    const auto include_target =
        workspace::resolve_include_at(snapshot, open_documents, configuration, utf8_offset);
    if (include_target) {
        const auto target = workspace::DocumentUri::from_path(include_target->string());
        const workspace::Position start{};
        return {{"uri", target.uri()}, {"range", lsp_range({.start = start, .end = start})}};
    }

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto definition =
        analysis_.definition(snapshot.document_uri().identity(), snapshot.version(),
                             snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Definition was superseded"};
        }
    }
    if (!definition.has_value()) {
        return nullptr;
    }

    const auto target = workspace::DocumentUri::from_path(definition->location.path);
    std::string target_text;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (documents_.contains(target.uri())) {
            target_text = documents_.snapshot(target.uri()).text();
        }
    }
    if (target_text.empty()) {
        std::ifstream stream{target.path(), std::ios::binary};
        target_text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    workspace::Position start{
        .line = definition->location.line > 0 ? definition->location.line - 1 : 0,
        .character = definition->location.column > 0 ? definition->location.column - 1 : 0};
    if (!target_text.empty()) {
        if (const auto offset = dxc_offset_at(target_text, definition->location.line,
                                              definition->location.column)) {
            start = workspace::lsp_position_at(target_text, *offset);
        }
    }
    auto end = start;
    const auto name_length = workspace::utf16_length(definition->name);
    if (name_length <= std::numeric_limits<std::uint32_t>::max() - end.character) {
        end.character += static_cast<std::uint32_t>(name_length);
    }
    return {{"uri", target.uri()}, {"range", lsp_range({.start = start, .end = end})}};
}

Json Server::hover(const std::optional<Json>& params, const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Hover document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto request_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    if (request_offset >= snapshot.text().size()) {
        return nullptr;
    }
    const auto lexical = lexical_prefix(snapshot.text(), request_offset + 1);
    if (!lexical.code[request_offset]) {
        return nullptr;
    }

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto information = analysis_.hover(snapshot.document_uri().identity(), snapshot.version(),
                                             snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Hover was superseded"};
        }
    }
    if (!information.has_value()) {
        return nullptr;
    }

    std::string contents;
    if (!information->declaration.empty()) {
        contents += information->declaration;
    } else if (!information->display_name.empty()) {
        contents += information->display_name;
    } else {
        contents += information->name;
    }
    if (!information->qualified_name.empty() &&
        information->qualified_name != information->display_name &&
        information->qualified_name != information->declaration) {
        contents += "\nSymbol: ";
        contents += information->qualified_name;
    }
    if (!information->type.empty()) {
        contents += "\nType: ";
        contents += information->type;
    }
    if (!information->declaration_location.path.empty()) {
        contents += "\nDeclared at ";
        contents += information->declaration_location.path;
        if (information->declaration_location.line != 0) {
            contents += ':';
            contents += std::to_string(information->declaration_location.line);
            if (information->declaration_location.column != 0) {
                contents += ':';
                contents += std::to_string(information->declaration_location.column);
            }
        }
    }

    Json result{{"contents", {{"kind", "plaintext"}, {"value", std::move(contents)}}}};
    if (information->start_offset <= information->end_offset &&
        information->end_offset <= snapshot.text().size()) {
        result["range"] = lsp_range(
            {.start = workspace::lsp_position_at(snapshot.text(), information->start_offset),
             .end = workspace::lsp_position_at(snapshot.text(), information->end_offset)});
    }
    return result;
}

Json Server::signature_help(const std::optional<Json>& params,
                            const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Signature help document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto cursor_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto call = call_context(snapshot.text(), cursor_offset);
    if (!call.has_value()) {
        return nullptr;
    }
    const auto callee_position = workspace::lsp_position_at(snapshot.text(), call->callee_offset);
    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), callee_position);
    const auto signatures =
        analysis_.signatures(snapshot.document_uri().identity(), snapshot.version(),
                             snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Signature help was superseded"};
        }
    }
    if (signatures.empty()) {
        return nullptr;
    }

    Json items = Json::array();
    for (const auto& signature : signatures) {
        Json parameters = Json::array();
        for (const auto& parameter : signature.parameters) {
            parameters.push_back({{"label", parameter.label}});
        }
        Json item{{"label", signature.label}, {"parameters", std::move(parameters)}};
        if (!signature.parameters.empty()) {
            item["activeParameter"] =
                (std::min)(call->active_parameter, signature.parameters.size() - 1);
        }
        items.push_back(std::move(item));
    }

    Json result{{"signatures", std::move(items)}, {"activeSignature", 0}};
    if (!signatures.front().parameters.empty()) {
        result["activeParameter"] =
            (std::min)(call->active_parameter, signatures.front().parameters.size() - 1);
    }
    return result;
}

Json Server::document_symbols(const std::optional<Json>& params,
                              const json_rpc::RequestContext& context) {
    require_running();
    const auto uri = string_member(object_member(object_params(params), "textDocument"), "uri");
    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Document symbols require an open document");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    Json result = Json::array();
    analyze_and_publish(snapshot.uri());
    const auto symbols = analysis_.symbols(snapshot.document_uri().identity(), snapshot.version(),
                                           context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Document symbols were superseded"};
        }
    }
    append_document_symbols(result, symbols, snapshot);
    return result;
}

Json Server::workspace_symbols(const std::optional<Json>& params,
                               const json_rpc::RequestContext& context) {
    require_running();
    const auto query = string_member(object_params(params), "query");
    Json result = Json::array();
    for (const auto& root : analysis_.roots()) {
        workspace::SourceSnapshot snapshot = [&]() -> workspace::SourceSnapshot {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(root.root_uri)) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "Workspace symbols were superseded"};
            }
            return documents_.snapshot(root.root_uri);
        }();
        if (snapshot.document_uri().identity() != root.root_identity ||
            root.version != snapshot.version()) {
            continue;
        }
        analyze_and_publish(snapshot.uri());
        const auto symbols =
            analysis_.symbols(root.root_identity, root.version, context.cancellation);
        {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(snapshot.uri()) ||
                documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "Workspace symbols were superseded"};
            }
        }
        append_workspace_symbols(result, symbols, snapshot, query, {});
    }
    return result;
}

Json Server::semantic_tokens(const std::optional<Json>& params,
                             const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Semantic token document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    if (snapshot.text().size() > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Semantic token document is too large");
    }

    analyze_and_publish(snapshot.uri());
    const auto dxc_tokens = analysis_.tokens(snapshot.document_uri().identity(), snapshot.version(),
                                             snapshot.path(), context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Semantic tokens were superseded"};
        }
    }
    std::vector<SemanticToken> tokens;
    tokens.reserve(dxc_tokens.size());
    for (const auto& token : dxc_tokens) {
        append_semantic_token(tokens, snapshot.text(), token);
    }
    std::ranges::sort(tokens, [](const auto& left, const auto& right) {
        return std::pair{left.start.line, left.start.character} <
               std::pair{right.start.line, right.start.character};
    });

    Json data = Json::array();
    std::uint32_t previous_line{};
    std::uint32_t previous_character{};
    for (const auto& token : tokens) {
        const auto delta_line = token.start.line - previous_line;
        const auto delta_character =
            delta_line == 0 ? token.start.character - previous_character : token.start.character;
        data.push_back(delta_line);
        data.push_back(delta_character);
        data.push_back(token.length);
        data.push_back(static_cast<std::uint32_t>(token.type));
        data.push_back(0);
        previous_line = token.start.line;
        previous_character = token.start.character;
    }
    return {{"data", std::move(data)}};
}

void Server::initialized(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::awaiting_initialized) {
        log("Ignoring initialized notification in an invalid lifecycle state");
        return;
    }
    if (params.has_value() && !params->is_object()) {
        log("Ignoring initialized notification with invalid parameters");
        return;
    }
    state_ = State::running;
}

void Server::did_open(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& document = object_member(object_params(params), "textDocument");
        const auto uri = string_member(document, "uri");
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_open(uri, string_member(document, "languageId"),
                                integer_member(document, "version"),
                                string_member(document, "text"));
        }
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto& document = object_member(value, "textDocument");
        const auto uri = string_member(document, "uri");
        const auto& raw_changes = member(value, "contentChanges");
        if (!raw_changes.is_array() || raw_changes.empty()) {
            invalid_params("contentChanges must be a non-empty array");
        }

        std::vector<workspace::ContentChange> changes;
        changes.reserve(raw_changes.size());
        for (const auto& raw_change : raw_changes) {
            if (!raw_change.is_object()) {
                invalid_params("Each content change must be an object");
            }
            workspace::ContentChange change{.range = std::nullopt,
                                            .range_length = std::nullopt,
                                            .text = string_member(raw_change, "text")};
            if (const auto item = raw_change.find("range"); item != raw_change.end()) {
                change.range = range(*item);
            }
            if (const auto item = raw_change.find("rangeLength"); item != raw_change.end()) {
                if (!item->is_number_unsigned() && !item->is_number_integer()) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                const auto length = item->get<std::int64_t>();
                if (length < 0) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                change.range_length = static_cast<std::size_t>(length);
            }
            changes.push_back(std::move(change));
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_change(uri, integer_member(document, "version"), changes);
        }
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_save(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto uri = string_member(object_member(value, "textDocument"), "uri");
        std::optional<std::string> text;
        if (const auto item = value.find("text"); item != value.end()) {
            if (!item->is_string()) {
                invalid_params("Save text must be a string");
            }
            text = item->get<std::string>();
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_save(uri, std::move(text));
        }
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_close(const std::optional<Json>& params) {
    try {
        require_running();
        const auto uri = string_member(object_member(object_params(params), "textDocument"), "uri");
        workspace::SourceSnapshot snapshot = [&] {
            std::scoped_lock state_lock{state_mutex_};
            return documents_.snapshot(uri);
        }();
        const std::unordered_set changed{snapshot.document_uri().identity()};
        auto affected_roots =
            analysis_.dependent_root_uris(changed, snapshot.document_uri().identity());
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_close(uri);
            ++analysis_generations_[snapshot.document_uri().identity()];
        }
        analysis_.erase(snapshot.document_uri().identity());
        for (const auto& root_uri : affected_roots) {
            analyze_and_publish(root_uri);
        }
        sender_(json_rpc::Notification{
            .method = "textDocument/publishDiagnostics",
            .params = Json{{"uri", snapshot.uri()}, {"diagnostics", Json::array()}}});
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_configuration(const std::optional<Json>& params) {
    try {
        require_running();
        const auto candidate =
            configuration_overrides(object_member(object_params(params), "settings"));
        {
            std::scoped_lock state_lock{state_mutex_};
            for (const auto& document : documents_.open_snapshots()) {
                static_cast<void>(configuration_for(document, candidate));
            }
            editor_settings_ = candidate;
        }
        reanalyze_all();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_client_defaults(const std::optional<Json>& params) {
    try {
        require_running();
        const auto defaults = configuration_overrides(object_params(params));
        if (!defaults.language_version) {
            invalid_params("hlsl.languageVersion must be provided");
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            client_default_language_version_ = *defaults.language_version;
        }
        reanalyze_all();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_workspace_folders(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& event = object_member(object_params(params), "event");
        const auto& removed = member(event, "removed");
        const auto& added = member(event, "added");
        if (!removed.is_array() || !added.is_array()) {
            invalid_params("Workspace folder changes must contain added and removed arrays");
        }
        std::unordered_map<std::string, std::filesystem::path> workspace_folders;
        {
            std::scoped_lock state_lock{state_mutex_};
            workspace_folders = workspace_folders_;
        }
        for (const auto& folder : removed) {
            workspace_folders.erase(workspace_folder_identity(folder));
        }
        for (const auto& folder : added) {
            const auto [identity, path] = workspace_folder(folder);
            workspace_folders.insert_or_assign(identity, path);
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            workspace_folders_ = std::move(workspace_folders);
        }
        reanalyze_all();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_watched_files(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& changes = member(object_params(params), "changes");
        if (!changes.is_array()) {
            invalid_params("Watched file changes must be an array");
        }

        std::unordered_set<std::string> changed_identities;
        std::vector<std::string> changed_configuration_directories;
        for (const auto& change : changes) {
            try {
                const auto changed = workspace::DocumentUri::from_uri(string_member(change, "uri"));
                changed_identities.insert(changed.identity());
                auto filename = std::filesystem::path{changed.path()}.filename().string();
#ifdef _WIN32
                std::ranges::transform(filename, filename.begin(), [](char value) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                });
#endif
                if (filename == workspace::configuration_file_name) {
                    changed_configuration_directories.push_back(
                        workspace::DocumentUri::from_path(
                            std::filesystem::path{changed.path()}.parent_path().string())
                            .identity());
                }
            } catch (const workspace::DocumentError& error) {
                invalid_params(error.what());
            }
        }

        analysis_.invalidate_include_metadata(changed_identities);
        std::vector<std::string> affected_roots = analysis_.dependent_root_uris(changed_identities);
        std::unordered_set<std::string> affected_root_identities;
        for (const auto& root : analysis_.roots()) {
            if (std::ranges::find(affected_roots, root.root_uri) != affected_roots.end()) {
                affected_root_identities.insert(root.root_identity);
            }
        }

        const auto in_changed_configuration_scope =
            [&changed_configuration_directories](std::string_view identity) {
                return std::ranges::any_of(
                    changed_configuration_directories, [identity](const auto& directory) {
#ifdef _WIN32
                        constexpr char separator = '\\';
#else
                        constexpr char separator = '/';
#endif
                        if (!identity.starts_with(directory)) {
                            return false;
                        }
                        if (!directory.empty() && directory.back() == separator) {
                            return identity.size() > directory.size();
                        }
                        return identity.size() > directory.size() &&
                               identity[directory.size()] == separator;
                    });
            };
        std::vector<workspace::SourceSnapshot> open_documents;
        {
            std::scoped_lock state_lock{state_mutex_};
            open_documents = documents_.open_snapshots();
        }
        for (const auto& document : open_documents) {
            const auto& identity = document.document_uri().identity();
            if (in_changed_configuration_scope(identity)) {
                if (affected_root_identities.emplace(identity).second) {
                    affected_roots.push_back(document.uri());
                }
            }
        }
        for (const auto& root_uri : affected_roots) {
            analyze_and_publish(root_uri);
        }
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::exit(const std::optional<Json>& params) {
    if (params.has_value() && !params->is_null()) {
        log("Exit notification does not accept parameters");
    }
    exit_requested_.store(true, std::memory_order_release);
}

void Server::analyze_affected(std::string_view uri) {
    const auto changed = [&] {
        std::scoped_lock state_lock{state_mutex_};
        return documents_.snapshot(uri);
    }();
    const std::unordered_set changed_identities{changed.document_uri().identity()};
    analysis_.invalidate_include_metadata(changed_identities);
    auto affected_roots =
        analysis_.dependent_root_uris(changed_identities, changed.document_uri().identity());

    analyze_and_publish(uri);
    for (const auto& root_uri : affected_roots) {
        analyze_and_publish(root_uri);
    }
}

void Server::analyze_and_publish(std::string_view uri) {
    analysis::AnalysisInput input = [&] {
        std::scoped_lock state_lock{state_mutex_};
        const auto& state = documents_.document(uri);
        if (!state.open) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Document was closed before analysis"};
        }
        auto snapshot = documents_.snapshot(uri);
        const auto generation = ++analysis_generations_[snapshot.document_uri().identity()];
        return analysis::AnalysisInput{.root = snapshot,
                                       .open_documents = documents_.open_snapshots(),
                                       .configuration =
                                           configuration_for(snapshot, editor_settings_),
                                       .generation = generation};
    }();
    analysis_.analyze(std::move(input));
    if (!options_.background_analysis) {
        analysis_.wait_idle();
    }
}

workspace::WorkspaceConfiguration
Server::configuration_for(const workspace::SourceSnapshot& snapshot,
                          const workspace::ConfigurationOverrides& overrides) const {
    workspace::WorkspaceConfiguration configuration;
    const auto shader_directory = std::filesystem::path{snapshot.path()}.parent_path();
    std::error_code error;
    if (std::filesystem::is_directory(shader_directory, error)) {
        configuration = workspace::load_workspace_configuration_for_file(snapshot.path());
    } else if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error{"Unable to inspect shader directory",
                                                shader_directory, error};
    }
    if (!configuration.language_version && client_default_language_version_) {
        configuration.language_version = client_default_language_version_;
    }
    return workspace::apply_configuration_overrides(std::move(configuration), overrides,
                                                    configuration_base_directory(snapshot.path()));
}

std::filesystem::path Server::configuration_base_directory(std::string_view shader_path) const {
    auto directory = std::filesystem::absolute(std::filesystem::path{shader_path}.parent_path())
                         .lexically_normal();
    auto candidate = directory;
    while (!candidate.empty()) {
        try {
            const auto identity = workspace::DocumentUri::from_path(candidate.string()).identity();
            if (const auto folder = workspace_folders_.find(identity);
                folder != workspace_folders_.end()) {
                return folder->second;
            }
        } catch (const workspace::DocumentError&) {
            break;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return directory;
}

void Server::reanalyze_all() {
    const auto open_documents = [&] {
        std::scoped_lock state_lock{state_mutex_};
        return documents_.open_snapshots();
    }();
    for (const auto& document : open_documents) {
        analyze_and_publish(document.uri());
    }
}

void Server::analysis_completed(const workspace::SourceSnapshot& snapshot,
                                const std::vector<dxc::Diagnostic>& diagnostics,
                                std::uint64_t generation) {
    std::scoped_lock state_lock{state_mutex_};
    if (!documents_.contains(snapshot.uri())) {
        return;
    }
    const auto& state = documents_.document(snapshot.uri());
    const auto expected = analysis_generations_.find(snapshot.document_uri().identity());
    const auto latest = documents_.snapshot(snapshot.uri());
    if (state.open && expected != analysis_generations_.end() && expected->second == generation &&
        latest.version() == snapshot.version()) {
        publish_diagnostics(latest, diagnostics);
    }
}

void Server::publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                                 const std::vector<dxc::Diagnostic>& diagnostics) {
    Json items = Json::array();
    for (const auto& diagnostic : diagnostics) {
        if (!diagnostic.location.path.empty() &&
            !same_document_path(diagnostic.location.path, snapshot.path())) {
            continue;
        }
        items.push_back({{"range", lsp_range(diagnostic_range(snapshot, diagnostic))},
                         {"severity", diagnostic_severity(diagnostic.severity)},
                         {"source", "dxc"},
                         {"message", diagnostic.message}});
    }
    sender_(json_rpc::Notification{.method = "textDocument/publishDiagnostics",
                                   .params = Json{{"uri", snapshot.uri()},
                                                  {"version", snapshot.version()},
                                                  {"diagnostics", std::move(items)}}});
}

void Server::require_running() const {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::running) {
        throw HandlerError{-32002, "Server not initialized"};
    }
}

void Server::log(std::string_view message) const {
    if (logger_) {
        logger_(message);
    }
}

namespace {

class RequestExecutor final {
  public:
    RequestExecutor(std::size_t worker_count, std::size_t capacity) : capacity_{capacity} {
        if (worker_count == 0 || capacity == 0) {
            throw std::invalid_argument{"Request executor limits must be positive"};
        }
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            static_cast<void>(index);
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    RequestExecutor(const RequestExecutor&) = delete;
    RequestExecutor& operator=(const RequestExecutor&) = delete;
    ~RequestExecutor() { shutdown(); }

    [[nodiscard]] bool submit(std::function<void()> task) {
        if (!task) {
            throw std::invalid_argument{"Request task must be callable"};
        }
        {
            std::scoped_lock lock{mutex_};
            if (stopping_ || queue_.size() >= capacity_) {
                return false;
            }
            queue_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::scoped_lock lock{mutex_};
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        ready_.notify_all();
        workers_.clear();
    }

    void rethrow_if_failed() {
        std::scoped_lock lock{failure_mutex_};
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

  private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task();
            } catch (...) {
                std::scoped_lock lock{failure_mutex_};
                if (!failure_) {
                    failure_ = std::current_exception();
                }
            }
        }
    }

    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool stopping_{};
};

} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run(std::istream& input, std::ostream& output, std::ostream& errors, ServerOptions options) {
    try {
        options.background_analysis = true;
        std::mutex output_mutex;
        std::mutex error_mutex;
        json_rpc::FrameWriter writer{output};
        Server server{[&writer, &output_mutex](const json_rpc::Notification& notification) {
                          std::scoped_lock lock{output_mutex};
                          writer.write(json_rpc::serialize(json_rpc::Message{notification}));
                      },
                      [&errors, &error_mutex](std::string_view message) {
                          std::scoped_lock lock{error_mutex};
                          errors << "HLSL-LSP: " << message << '\n';
                      },
                      options};
        RequestExecutor requests{options.request_worker_count, options.request_queue_capacity};
        json_rpc::FrameReader reader{input};

        while (!server.exit_requested()) {
            const auto payload = reader.read();
            if (!payload.has_value()) {
                break;
            }
            const auto parsed = json_rpc::parse_message(*payload);
            if (parsed.error.has_value()) {
                std::scoped_lock lock{output_mutex};
                writer.write(json_rpc::serialize(json_rpc::DispatchResponse{*parsed.error}));
                continue;
            }
            if (const auto* request = std::get_if<json_rpc::Request>(&*parsed.message)) {
                const auto cancellation = server.begin_request(request->id);
                if (request->method == "initialize" || request->method == "shutdown") {
                    const auto response = server.handle(*request, cancellation);
                    std::scoped_lock lock{output_mutex};
                    writer.write(json_rpc::serialize(response));
                    continue;
                }
                const auto accepted = requests.submit(
                    [&server, &writer, &output_mutex, request = *request, cancellation] {
                        const auto response = server.handle(request, cancellation);
                        std::scoped_lock lock{output_mutex};
                        writer.write(json_rpc::serialize(response));
                    });
                if (!accepted) {
                    cancellation.cancel();
                    server.finish_request(request->id, cancellation);
                    const json_rpc::ErrorResponse response{
                        .id = request->id,
                        .error = {
                            .code = -32000, .message = "Request queue full", .data = std::nullopt}};
                    std::scoped_lock lock{output_mutex};
                    writer.write(json_rpc::serialize(json_rpc::DispatchResponse{response}));
                }
            } else {
                static_cast<void>(server.handle(*parsed.message));
            }
        }
        server.cancel_all_requests();
        requests.shutdown();
        requests.rethrow_if_failed();
        server.wait_for_analysis();
        return server.exit_requested() ? server.exit_code() : 0;
    } catch (const std::exception& error) {
        errors << "HLSL-LSP: " << error.what() << '\n';
        return 1;
    }
}

} // namespace hlsl_intellisense::lsp
