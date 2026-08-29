#include "memory_layout.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__clang__)
// Partial designated initializers intentionally retain aggregate defaults.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#if defined(_MSC_VER)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif

namespace hlsl_intellisense::dxc::detail {
namespace {

struct Token {
    std::string text;
    std::size_t begin{};
    std::size_t end{};
};

struct Field {
    std::string name;
    std::string type;
    std::vector<std::uint32_t> dimensions;
    std::size_t begin{};
    std::size_t end{};
    bool row_major{};
    bool column_major{};
    std::string unsupported;
};

struct Record {
    std::string name;
    bool constant_buffer{};
    std::vector<Field> fields;
    std::size_t begin{};
    std::size_t end{};
    std::string unsupported;
};

struct MatrixMajorEvent {
    std::size_t offset{};
    bool row_major{};
    bool conditional{};
};

struct ConditionalRange {
    std::size_t begin{};
    std::size_t end{};
};

enum class TypeKind : std::uint8_t { scalar, vector, matrix, record, unsupported };

struct Type {
    TypeKind kind{TypeKind::unsupported};
    std::string spelling;
    std::string record;
    std::uint32_t scalar_size{};
    std::uint32_t rows{1};
    std::uint32_t columns{1};
    std::string explanation;
};

struct TypeLayout {
    MemoryLayoutElementKind kind{MemoryLayoutElementKind::scalar};
    std::uint32_t size{};
    std::uint32_t alignment{};
    std::uint32_t array_stride{};
    std::uint32_t matrix_stride{};
    std::vector<MemoryLayoutElement> members;
    bool supported{true};
    std::string explanation;
};

[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

[[nodiscard]] bool identifier_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
}

[[nodiscard]] bool identifier_continue(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

[[nodiscard]] bool directive_start(std::string_view source, std::size_t offset) {
    while (offset > 0 && source[offset - 1] != '\n' && source[offset - 1] != '\r') {
        if (std::isspace(static_cast<unsigned char>(source[offset - 1])) == 0) {
            return false;
        }
        --offset;
    }
    return true;
}

[[nodiscard]] std::vector<Token> lex(std::string_view source) {
    std::vector<Token> result;
    std::size_t index{};
    while (index < source.size()) {
        const auto character = source[index];
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            ++index;
            continue;
        }
        if (character == '#' && directive_start(source, index)) {
            while (index < source.size() && source[index] != '\n' && source[index] != '\r') {
                ++index;
            }
            continue;
        }
        if (character == '/' && index + 1 < source.size() && source[index + 1] == '/') {
            index += 2;
            while (index < source.size() && source[index] != '\n' && source[index] != '\r') {
                ++index;
            }
            continue;
        }
        if (character == '/' && index + 1 < source.size() && source[index + 1] == '*') {
            index += 2;
            while (index + 1 < source.size() &&
                   !(source[index] == '*' && source[index + 1] == '/')) {
                ++index;
            }
            index = (std::min)(index + 2, source.size());
            continue;
        }
        if (character == '"' || character == '\'') {
            const auto quote = character;
            const auto begin = index++;
            while (index < source.size()) {
                if (source[index] == '\\' && index + 1 < source.size()) {
                    index += 2;
                } else if (source[index++] == quote) {
                    break;
                }
            }
            result.push_back({std::string{source.substr(begin, index - begin)}, begin, index});
            continue;
        }
        if (identifier_start(character)) {
            const auto begin = index++;
            while (index < source.size() && identifier_continue(source[index])) {
                ++index;
            }
            result.push_back({std::string{source.substr(begin, index - begin)}, begin, index});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            const auto begin = index++;
            while (index < source.size() &&
                   std::isalnum(static_cast<unsigned char>(source[index])) != 0) {
                ++index;
            }
            result.push_back({std::string{source.substr(begin, index - begin)}, begin, index});
            continue;
        }
        result.push_back({std::string(1, character), index, index + 1});
        ++index;
    }
    return result;
}

[[nodiscard]] bool qualifier(std::string_view value) {
    return value == "const" || value == "volatile" || value == "static" || value == "uniform" ||
           value == "precise" || value == "groupshared" || value == "in" || value == "out" ||
           value == "inout" || value == "linear" || value == "centroid" ||
           value == "nointerpolation" || value == "noperspective" || value == "sample";
}

[[nodiscard]] std::optional<std::uint32_t> positive_integer(std::string_view value) {
    std::uint32_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool matrix_type_spelling(std::string_view value) {
    if (value.starts_with("matrix<")) {
        return true;
    }
    const auto x = value.rfind('x');
    return x != std::string_view::npos && x > 0 && x + 1 < value.size() && value[x - 1] >= '1' &&
           value[x - 1] <= '4' && value[x + 1] >= '1' && value[x + 1] <= '4' &&
           x + 2 == value.size();
}

class Parser final {
  public:
    Parser(std::string_view source, bool default_row_major)
        : source_{source}, tokens_{lex(source)}, default_row_major_{default_row_major} {
        parse_directives();
    }

    [[nodiscard]] std::vector<Record> parse() {
        std::vector<Record> records;
        for (std::size_t index = 0; index < tokens_.size();) {
            if (tokens_[index].text == "struct" || tokens_[index].text == "cbuffer") {
                auto [record, next] = parse_record(index);
                if (record.has_value()) {
                    if (std::ranges::any_of(conditional_ranges_, [&record](const auto& range) {
                            return range.begin < record->end && range.end > record->begin;
                        })) {
                        record->unsupported =
                            "Conditional preprocessing overlaps this record; layout is "
                            "unsupported without evaluating macros";
                    }
                    records.push_back(std::move(*record));
                }
                index = (std::max)(next, index + 1);
            } else {
                ++index;
            }
        }
        return records;
    }

    [[nodiscard]] bool has_include_before(std::size_t offset) const {
        return std::ranges::any_of(include_offsets_,
                                   [offset](const auto include) { return include < offset; });
    }

  private:
    void parse_directives() {
        std::vector<std::size_t> conditional_stack;
        std::size_t line_begin{};
        bool block_comment{};
        while (line_begin < source_.size()) {
            auto line_end = source_.find_first_of("\r\n", line_begin);
            if (line_end == std::string_view::npos) {
                line_end = source_.size();
            }
            const auto line = source_.substr(line_begin, line_end - line_begin);
            std::string uncommented;
            uncommented.reserve(line.size());
            for (std::size_t index = 0; index < line.size();) {
                if (block_comment) {
                    const auto end = line.find("*/", index);
                    if (end == std::string_view::npos) {
                        index = line.size();
                    } else {
                        block_comment = false;
                        index = end + 2;
                    }
                } else if (line.substr(index).starts_with("//")) {
                    break;
                } else if (line.substr(index).starts_with("/*")) {
                    block_comment = true;
                    index += 2;
                } else {
                    uncommented.push_back(line[index]);
                    ++index;
                }
            }
            std::string compact;
            compact.reserve(uncommented.size());
            for (const auto character : uncommented) {
                if (std::isspace(static_cast<unsigned char>(character)) == 0) {
                    compact.push_back(character);
                }
            }
            if (compact.starts_with("#ifdef") || compact.starts_with("#ifndef") ||
                compact.starts_with("#if")) {
                conditional_stack.push_back(line_begin);
            } else if (compact.starts_with("#endif")) {
                if (!conditional_stack.empty()) {
                    conditional_ranges_.push_back(
                        {.begin = conditional_stack.back(), .end = line_end});
                    conditional_stack.pop_back();
                }
            } else if (compact == "#pragmapack_matrix(row_major)") {
                matrix_major_events_.push_back({.offset = line_begin,
                                                .row_major = true,
                                                .conditional = !conditional_stack.empty()});
            } else if (compact == "#pragmapack_matrix(column_major)") {
                matrix_major_events_.push_back({.offset = line_begin,
                                                .row_major = false,
                                                .conditional = !conditional_stack.empty()});
            } else if (compact.starts_with("#include")) {
                include_offsets_.push_back(line_begin);
            }
            line_begin = line_end;
            while (line_begin < source_.size() &&
                   (source_[line_begin] == '\n' || source_[line_begin] == '\r')) {
                ++line_begin;
            }
        }
        for (const auto begin : conditional_stack) {
            conditional_ranges_.push_back({.begin = begin, .end = source_.size()});
        }
        std::ranges::sort(conditional_ranges_, {}, &ConditionalRange::begin);
    }

    [[nodiscard]] bool row_major_at(std::size_t offset) const {
        auto result = default_row_major_;
        for (const auto& event : matrix_major_events_) {
            if (event.offset >= offset) {
                break;
            }
            if (!event.conditional) {
                result = event.row_major;
            }
        }
        return result;
    }

    [[nodiscard]] bool conditional_matrix_major_at(std::size_t offset) const {
        const MatrixMajorEvent* latest{};
        for (const auto& event : matrix_major_events_) {
            if (event.offset >= offset) {
                break;
            }
            latest = &event;
        }
        return latest != nullptr && latest->conditional;
    }

    [[nodiscard]] std::pair<std::optional<Record>, std::size_t>
    parse_record(std::size_t start) const {
        const bool cbuffer = tokens_[start].text == "cbuffer";
        if (start + 2 >= tokens_.size() || !identifier_start(tokens_[start + 1].text.front())) {
            return {std::nullopt, start + 1};
        }
        const auto name = tokens_[start + 1].text;
        auto opening = start + 2;
        while (opening < tokens_.size() && tokens_[opening].text != "{" &&
               tokens_[opening].text != ";") {
            ++opening;
        }
        if (opening == tokens_.size() || tokens_[opening].text != "{") {
            return {std::nullopt, opening};
        }
        std::size_t depth{1};
        auto closing = opening + 1;
        for (; closing < tokens_.size() && depth != 0; ++closing) {
            if (tokens_[closing].text == "{") {
                ++depth;
            } else if (tokens_[closing].text == "}") {
                --depth;
            }
        }
        if (depth != 0) {
            return {Record{.name = name,
                           .constant_buffer = cbuffer,
                           .begin = tokens_[start].begin,
                           .end = source_.size(),
                           .unsupported = "Unterminated record declaration"},
                    tokens_.size()};
        }

        Record record{.name = name,
                      .constant_buffer = cbuffer,
                      .begin = tokens_[start].begin,
                      .end = tokens_[closing - 1].end};
        parse_fields(record, opening + 1, closing - 1);
        return {std::move(record), closing};
    }

    void parse_fields(Record& record, std::size_t begin, std::size_t end) const {
        auto declaration_begin = begin;
        std::size_t angle{};
        std::size_t square{};
        std::size_t parenthesis{};
        std::size_t brace{};
        for (auto index = begin; index < end; ++index) {
            const auto& text = tokens_[index].text;
            if (text == "<") {
                ++angle;
            } else if (text == ">" && angle != 0) {
                --angle;
            } else if (text == "[") {
                ++square;
            } else if (text == "]" && square != 0) {
                --square;
            } else if (text == "(") {
                ++parenthesis;
            } else if (text == ")" && parenthesis != 0) {
                --parenthesis;
            } else if (text == "{") {
                ++brace;
            } else if (text == "}" && brace != 0) {
                --brace;
            } else if (text == ";" && angle == 0 && square == 0 && parenthesis == 0 && brace == 0) {
                parse_declaration(record, declaration_begin, index);
                declaration_begin = index + 1;
            }
        }
        if (declaration_begin != end) {
            record.unsupported = "A field declaration is missing a semicolon";
        }
    }

    void parse_declaration(Record& record, std::size_t begin, std::size_t end) const {
        const auto declaration_begin = begin;
        bool explicit_row_major{};
        bool explicit_column_major{};
        while (begin < end &&
               (tokens_[begin].text == "row_major" || tokens_[begin].text == "column_major" ||
                qualifier(tokens_[begin].text))) {
            explicit_row_major = explicit_row_major || tokens_[begin].text == "row_major";
            explicit_column_major = explicit_column_major || tokens_[begin].text == "column_major";
            ++begin;
        }
        if (begin == end) {
            return;
        }
        if (tokens_[begin].text == "struct" || tokens_[begin].text == "class" ||
            tokens_[begin].text == "typedef" || tokens_[begin].text == "using" ||
            tokens_[begin].text == "template") {
            record.unsupported =
                "Inline, aliased, and templated field declarations are unsupported";
            return;
        }

        std::size_t type_end = begin + 1;
        if (type_end < end && tokens_[type_end].text == "<") {
            std::size_t depth{1};
            ++type_end;
            while (type_end < end && depth != 0) {
                if (tokens_[type_end].text == "<") {
                    ++depth;
                } else if (tokens_[type_end].text == ">") {
                    --depth;
                }
                ++type_end;
            }
        }
        if (type_end >= end) {
            record.unsupported = "A field declarator is missing";
            return;
        }
        std::string type;
        for (auto index = begin; index < type_end; ++index) {
            type += tokens_[index].text;
        }
        const auto row_major =
            explicit_row_major ||
            (!explicit_column_major && row_major_at(tokens_[declaration_begin].begin));
        const auto column_major = explicit_column_major;
        const auto conditional_matrix_major =
            !explicit_row_major && !explicit_column_major && matrix_type_spelling(type) &&
            conditional_matrix_major_at(tokens_[declaration_begin].begin);
        auto declarator_begin = type_end;
        bool first_declarator = true;
        std::size_t square{};
        std::size_t parenthesis{};
        for (auto index = type_end; index <= end; ++index) {
            const bool at_end = index == end;
            if (!at_end) {
                if (tokens_[index].text == "[") {
                    ++square;
                } else if (tokens_[index].text == "]" && square != 0) {
                    --square;
                } else if (tokens_[index].text == "(") {
                    ++parenthesis;
                } else if (tokens_[index].text == ")" && parenthesis != 0) {
                    --parenthesis;
                }
            }
            if (at_end || (tokens_[index].text == "," && square == 0 && parenthesis == 0)) {
                const auto selection_begin =
                    first_declarator ? tokens_[declaration_begin].begin
                                     : (declarator_begin < end ? tokens_[declarator_begin].begin
                                                               : tokens_[index - 1].end);
                parse_declarator(record, type, row_major, column_major, conditional_matrix_major,
                                 declarator_begin, index, selection_begin);
                first_declarator = false;
                declarator_begin = index + 1;
            }
        }
    }

    void parse_declarator(Record& record, const std::string& type, bool row_major,
                          bool column_major, bool conditional_matrix_major, std::size_t begin,
                          std::size_t end, std::size_t selection_begin) const {
        if (begin >= end) {
            return;
        }
        const auto name_index = begin;
        Field field{.name = tokens_[name_index].text,
                    .type = type,
                    .begin = selection_begin,
                    .end = tokens_[name_index].end,
                    .row_major = row_major,
                    .column_major = column_major};
        if (row_major && column_major) {
            field.unsupported = "A matrix cannot be both row_major and column_major";
        }
        if (conditional_matrix_major) {
            field.unsupported =
                "A conditional #pragma pack_matrix can affect this declaration; layout is "
                "unsupported without evaluating macros";
        }
        if (!identifier_start(field.name.front())) {
            field.unsupported = "Expected a named field declarator";
        }
        for (auto index = name_index + 1; index < end;) {
            if (tokens_[index].text == "[") {
                if (index + 2 >= end || tokens_[index + 2].text != "]") {
                    field.unsupported = "Only fixed-size array dimensions are supported";
                    break;
                }
                const auto dimension = positive_integer(tokens_[index + 1].text);
                if (!dimension.has_value()) {
                    field.unsupported = "Array dimensions must be positive integer literals";
                    break;
                }
                field.dimensions.push_back(*dimension);
                field.end = tokens_[index + 2].end;
                index += 3;
            } else if (tokens_[index].text == ":") {
                if (index + 1 < end && tokens_[index + 1].text == "packoffset") {
                    field.unsupported = "Explicit packoffset layout is not supported";
                } else {
                    field.unsupported = "Bit-field layout is not supported";
                }
                break;
            } else if (tokens_[index].text == "=" || tokens_[index].text == "(") {
                field.unsupported = "Initializers and function declarations are unsupported here";
                break;
            } else {
                field.unsupported = "Unsupported field declarator syntax";
                break;
            }
        }
        record.fields.push_back(std::move(field));
    }

    std::string_view source_;
    std::vector<Token> tokens_;
    bool default_row_major_{};
    std::vector<MatrixMajorEvent> matrix_major_events_;
    std::vector<ConditionalRange> conditional_ranges_;
    std::vector<std::size_t> include_offsets_;
};

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
vector_suffix(std::string_view value) {
    const auto x = value.find('x');
    if (x == std::string_view::npos) {
        if (value.size() == 1 && value.front() >= '1' && value.front() <= '4') {
            return std::pair{1U, static_cast<std::uint32_t>(value.front() - '0')};
        }
        return std::nullopt;
    }
    if (x == 0 || x + 1 >= value.size() || value[x - 1] < '1' || value[x - 1] > '4' ||
        value[x + 1] < '1' || value[x + 1] > '4' || x + 2 != value.size()) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::uint32_t>(value[x - 1] - '0'),
                     static_cast<std::uint32_t>(value[x + 1] - '0')};
}

[[nodiscard]] std::optional<std::uint32_t> scalar_size(std::string_view type,
                                                       bool native_16_bit_types) {
    if (type == "bool" || type == "int" || type == "uint" || type == "dword" || type == "float") {
        return 4;
    }
    if (type == "double" || type == "int64_t" || type == "uint64_t") {
        return 8;
    }
    if (type == "half") {
        return native_16_bit_types ? 2U : 4U;
    }
    if (type == "int16_t" || type == "uint16_t" || type == "float16_t") {
        return native_16_bit_types ? std::optional<std::uint32_t>{2} : std::nullopt;
    }
    if (type == "min16float" || type == "min10float" || type == "min16int" || type == "min12int" ||
        type == "min16uint") {
        return 4;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> generic_arguments(std::string_view spelling) {
    const auto opening = spelling.find('<');
    const auto closing = spelling.rfind('>');
    if (opening == std::string_view::npos || closing == std::string_view::npos ||
        closing <= opening) {
        return {};
    }
    std::vector<std::string> result;
    auto begin = opening + 1;
    for (auto index = begin; index <= closing; ++index) {
        if (index == closing || spelling[index] == ',') {
            result.emplace_back(spelling.substr(begin, index - begin));
            begin = index + 1;
        }
    }
    return result;
}

[[nodiscard]] Type parse_type(std::string spelling,
                              const std::unordered_map<std::string, const Record*>& records,
                              bool native_16_bit_types) {
    if (const auto size = scalar_size(spelling, native_16_bit_types)) {
        return {.kind = TypeKind::scalar, .spelling = std::move(spelling), .scalar_size = *size};
    }
    if ((spelling.starts_with("vector<") || spelling.starts_with("matrix<"))) {
        const auto arguments = generic_arguments(spelling);
        const bool matrix = spelling.starts_with("matrix<");
        if (arguments.size() == (matrix ? 3U : 2U)) {
            const auto size = scalar_size(arguments[0], native_16_bit_types);
            const auto first = positive_integer(arguments[1]);
            const auto second =
                matrix ? positive_integer(arguments[2]) : std::optional<std::uint32_t>{1};
            if (size && first && second && *first <= 4 && *second <= 4) {
                return {.kind = matrix ? TypeKind::matrix : TypeKind::vector,
                        .spelling = std::move(spelling),
                        .scalar_size = *size,
                        .rows = matrix ? *first : 1U,
                        .columns = matrix ? *second : *first};
            }
        }
        return {.spelling = std::move(spelling),
                .explanation = "Malformed or unsupported vector/matrix template type"};
    }

    static constexpr std::string_view scalar_names[] = {
        "min16float", "min10float", "min16uint", "min16int", "min12int", "float16_t",
        "uint16_t",   "int16_t",    "uint64_t",  "int64_t",  "double",   "dword",
        "float",      "uint",       "half",      "bool",     "int"};
    for (const auto scalar : scalar_names) {
        if (!spelling.starts_with(scalar) || spelling.size() == scalar.size()) {
            continue;
        }
        const auto suffix = spelling.substr(scalar.size());
        const auto dimensions = vector_suffix(suffix);
        const auto size = scalar_size(scalar, native_16_bit_types);
        if (dimensions && size) {
            return {.kind = dimensions->first == 1 ? TypeKind::vector : TypeKind::matrix,
                    .spelling = std::move(spelling),
                    .scalar_size = *size,
                    .rows = dimensions->first,
                    .columns = dimensions->second};
        }
    }
    if (records.contains(spelling)) {
        return {.kind = TypeKind::record, .spelling = spelling, .record = std::move(spelling)};
    }
    return {.spelling = std::move(spelling), .explanation = "Unsupported or unresolved HLSL type"};
}

class LayoutEngine final {
  public:
    LayoutEngine(const std::vector<Record>& records, bool native_16_bit_types)
        : native_16_bit_types_{native_16_bit_types} {
        for (const auto& record : records) {
            records_.insert_or_assign(record.name, &record);
        }
    }

    [[nodiscard]] MemoryLayout layout(const Record& record, const Field* selected) {
        MemoryLayout result{.name = record.name,
                            .type = record.constant_buffer ? "cbuffer" : record.name,
                            .kind = record.constant_buffer ? MemoryLayoutKind::constant_buffer
                                                           : MemoryLayoutKind::natural};
        if (!record.unsupported.empty()) {
            result.supported = false;
            result.explanation = record.unsupported;
            return result;
        }
        std::size_t remaining_nodes = max_expanded_nodes;
        const auto laid_out = layout_record(record, record.constant_buffer, {}, remaining_nodes,
                                            record.constant_buffer);
        result.size = laid_out.size;
        result.allocation_size = laid_out.size;
        result.alignment = laid_out.alignment;
        result.members = laid_out.members;
        if (record.constant_buffer && !result.members.empty()) {
            const auto& final_member = result.members.back();
            result.size = final_member.offset + final_member.size;
        }
        result.supported = laid_out.supported;
        result.explanation = laid_out.explanation;
        if (selected != nullptr) {
            const auto member =
                std::ranges::find(result.members, selected->name, &MemoryLayoutElement::name);
            if (member != result.members.end()) {
                result.selected_name = member->name;
                result.selected_type = member->type;
                result.selected_size = member->size;
                result.selected_alignment = member->alignment;
                if (record.constant_buffer) {
                    result.packed_offset = member->offset;
                }
            }
        } else {
            result.selected_name = result.name;
            result.selected_type = result.type;
            result.selected_size = result.size;
            result.selected_alignment = result.alignment;
            if (record.constant_buffer) {
                result.packed_offset = 0;
            }
        }
        return result;
    }

  private:
    [[nodiscard]] TypeLayout layout_record(const Record& record, bool constant_buffer,
                                           std::vector<std::string> stack,
                                           std::size_t& remaining_nodes, bool root = false) {
        if (!record.unsupported.empty()) {
            return {.supported = false, .explanation = record.unsupported};
        }
        if (std::ranges::find(stack, record.name) != stack.end()) {
            return {.supported = false, .explanation = "Recursive record layouts are unsupported"};
        }
        if (stack.size() >= max_record_depth) {
            return {.supported = false,
                    .explanation = "Record nesting exceeds the supported depth of 128"};
        }
        stack.push_back(record.name);
        TypeLayout result{.kind = MemoryLayoutElementKind::record,
                          .alignment = constant_buffer ? 16U : 1U};
        std::size_t cursor{};
        bool previous_was_record{};
        for (const auto& field : record.fields) {
            if (!field.unsupported.empty()) {
                return {.supported = false, .explanation = field.name + ": " + field.unsupported};
            }
            auto field_layout = layout_field(field, constant_buffer, stack, remaining_nodes);
            if (!field_layout.supported) {
                field_layout.explanation = field.name + ": " + field_layout.explanation;
                return field_layout;
            }
            if (constant_buffer) {
                if (previous_was_record) {
                    cursor = align_up(cursor, 16);
                }
                cursor = cbuffer_field_offset(cursor, field_layout);
            } else {
                cursor = align_up(cursor, field_layout.alignment);
            }
            if (cursor > UINT32_MAX || field_layout.size > UINT32_MAX - cursor) {
                return {.supported = false, .explanation = "Layout exceeds 32-bit byte offsets"};
            }
            if (remaining_nodes == 0) {
                return {.supported = false,
                        .explanation = "Layout expands to more than 4096 elements"};
            }
            --remaining_nodes;
            result.members.push_back({.name = field.name,
                                      .type = field.type,
                                      .kind = field_layout.kind,
                                      .offset = static_cast<std::uint32_t>(cursor),
                                      .size = field_layout.size,
                                      .alignment = field_layout.alignment,
                                      .array_stride = field_layout.array_stride,
                                      .matrix_stride = field_layout.matrix_stride,
                                      .row_major = field.row_major,
                                      .array_dimensions = field.dimensions,
                                      .members = std::move(field_layout.members)});
            cursor += field_layout.size;
            previous_was_record = field_layout.kind == MemoryLayoutElementKind::record;
            result.alignment = (std::max)(result.alignment, field_layout.alignment);
        }
        const auto final_size = constant_buffer ? (root ? align_up(cursor, 16) : cursor)
                                                : align_up(cursor, result.alignment);
        if (final_size > UINT32_MAX) {
            return {.supported = false, .explanation = "Layout exceeds 32-bit byte offsets"};
        }
        result.size = static_cast<std::uint32_t>(final_size);
        return result;
    }

    [[nodiscard]] TypeLayout layout_field(const Field& field, bool constant_buffer,
                                          const std::vector<std::string>& stack,
                                          std::size_t& remaining_nodes) {
        auto type = parse_type(field.type, records_, native_16_bit_types_);
        auto result = layout_type(type, constant_buffer, field.row_major, stack, remaining_nodes);
        if (!result.supported || field.dimensions.empty()) {
            return result;
        }
        std::size_t count{1};
        for (const auto dimension : field.dimensions) {
            if (count > UINT32_MAX / dimension) {
                return {.supported = false, .explanation = "Array element count is too large"};
            }
            count *= dimension;
        }
        const auto child_count = expanded_node_count(result.members);
        const auto additional_child_count = child_count * (count - 1);
        if (count > remaining_nodes || additional_child_count > remaining_nodes - count) {
            return {.supported = false,
                    .explanation = "Array layout expands to more than 4096 elements"};
        }
        remaining_nodes -= count + additional_child_count;
        const auto element = result;
        const auto stride =
            constant_buffer ? align_up(result.size, 16) : align_up(result.size, result.alignment);
        if (stride == 0) {
            return {.supported = false,
                    .explanation = "Arrays of zero-sized records are unsupported"};
        }
        if (stride > UINT32_MAX || count > UINT32_MAX / stride) {
            return {.supported = false, .explanation = "Array layout exceeds 32-bit byte offsets"};
        }
        result.kind = MemoryLayoutElementKind::array;
        result.array_stride = static_cast<std::uint32_t>(stride);
        result.size = static_cast<std::uint32_t>(stride * count);
        result.members.clear();
        result.members.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.members.push_back({.name = "[" + std::to_string(index) + "]",
                                      .type = field.type,
                                      .kind = element.kind,
                                      .offset = static_cast<std::uint32_t>(stride * index),
                                      .size = element.size,
                                      .alignment = element.alignment,
                                      .array_stride = element.array_stride,
                                      .matrix_stride = element.matrix_stride,
                                      .row_major = field.row_major,
                                      .array_index = static_cast<std::uint32_t>(index),
                                      .members = element.members});
        }
        if (constant_buffer) {
            result.alignment = 16;
        }
        return result;
    }

    [[nodiscard]] TypeLayout layout_type(const Type& type, bool constant_buffer, bool row_major,
                                         const std::vector<std::string>& stack,
                                         std::size_t& remaining_nodes) {
        switch (type.kind) {
        case TypeKind::scalar:
            return {.kind = MemoryLayoutElementKind::scalar,
                    .size = type.scalar_size,
                    .alignment = type.scalar_size};
        case TypeKind::vector:
            return {.kind = MemoryLayoutElementKind::vector,
                    .size = type.scalar_size * type.columns,
                    .alignment = type.scalar_size};
        case TypeKind::matrix: {
            const auto vectors = row_major ? type.rows : type.columns;
            const auto components = row_major ? type.columns : type.rows;
            const auto vector_size = type.scalar_size * components;
            const auto vector_stride = constant_buffer && vectors > 1 ? 16U : vector_size;
            if (vectors > remaining_nodes) {
                return {.supported = false,
                        .explanation = "Layout expands to more than 4096 elements"};
            }
            remaining_nodes -= vectors;
            std::vector<MemoryLayoutElement> vector_elements;
            vector_elements.reserve(vectors);
            for (std::uint32_t index = 0; index < vectors; ++index) {
                vector_elements.push_back({.name = "[" + std::to_string(index) + "]",
                                           .type = type.spelling,
                                           .kind = MemoryLayoutElementKind::vector,
                                           .offset = vector_stride * index,
                                           .size = vector_size,
                                           .alignment = type.scalar_size,
                                           .array_index = index});
            }
            if (constant_buffer && vectors > 1) {
                return {.kind = MemoryLayoutElementKind::matrix,
                        .size = static_cast<std::uint32_t>(16U * vectors),
                        .alignment = 16,
                        .matrix_stride = 16,
                        .members = std::move(vector_elements)};
            }
            return {.kind = MemoryLayoutElementKind::matrix,
                    .size = vector_size * vectors,
                    .alignment = type.scalar_size,
                    .matrix_stride = vector_size,
                    .members = std::move(vector_elements)};
        }
        case TypeKind::record: {
            const auto found = records_.find(type.record);
            if (found == records_.end()) {
                return {.supported = false, .explanation = "Unresolved record type"};
            }
            return layout_record(*found->second, constant_buffer, stack, remaining_nodes);
        }
        case TypeKind::unsupported:
            return {.supported = false, .explanation = type.explanation};
        }
        return {.supported = false, .explanation = "Unknown type category"};
    }

    [[nodiscard]] static std::size_t cbuffer_field_offset(std::size_t cursor,
                                                          const TypeLayout& layout) {
        if (layout.kind == MemoryLayoutElementKind::array ||
            layout.kind == MemoryLayoutElementKind::record ||
            (layout.kind == MemoryLayoutElementKind::matrix && layout.matrix_stride == 16)) {
            return align_up(cursor, 16);
        }
        cursor = align_up(cursor, layout.alignment);
        const auto row_offset = cursor % 16;
        if (row_offset + layout.size > 16) {
            cursor = align_up(cursor, 16);
        }
        return cursor;
    }

    [[nodiscard]] static std::size_t
    expanded_node_count(const std::vector<MemoryLayoutElement>& members) {
        std::size_t result{};
        for (const auto& member : members) {
            result += 1 + expanded_node_count(member.members);
        }
        return result;
    }

    static constexpr std::size_t max_expanded_nodes = 4096;
    static constexpr std::size_t max_record_depth = 128;
    bool native_16_bit_types_{};
    std::unordered_map<std::string, const Record*> records_;
};

[[nodiscard]] std::optional<std::size_t> source_offset(std::string_view text, std::uint32_t line,
                                                       std::uint32_t column) {
    if (line == 0 || column == 0) {
        return std::nullopt;
    }
    std::uint32_t current_line{1};
    std::size_t offset{};
    while (offset < text.size() && current_line < line) {
        if (text[offset] == '\r') {
            ++current_line;
            ++offset;
            if (offset < text.size() && text[offset] == '\n') {
                ++offset;
            }
            continue;
        }
        if (text[offset] == '\n') {
            ++current_line;
        }
        ++offset;
    }
    if (current_line != line) {
        return std::nullopt;
    }
    const auto line_start = offset;
    const auto requested = line_start + column - 1;
    if (requested > text.size()) {
        return std::nullopt;
    }
    return requested;
}

[[nodiscard]] bool contains_matrix(const std::vector<MemoryLayoutElement>& members) {
    return std::ranges::any_of(members, [](const auto& member) {
        return member.kind == MemoryLayoutElementKind::matrix || contains_matrix(member.members);
    });
}

} // namespace

std::optional<MemoryLayout> memory_layout_at(const std::vector<SourceFile>& sources,
                                             std::string_view path, std::uint32_t line,
                                             std::uint32_t column, bool native_16_bit_types,
                                             bool default_row_major) {
    const auto source = std::ranges::find(sources, path, &SourceFile::path);
    if (source == sources.end()) {
        return std::nullopt;
    }
    const auto offset = source_offset(source->text, line, column);
    if (!offset.has_value()) {
        return std::nullopt;
    }
    Parser parser{source->text, default_row_major};
    const auto records = parser.parse();
    const Record* selected_record{};
    const Field* selected_field{};
    for (const auto& record : records) {
        if (*offset < record.begin || *offset > record.end) {
            continue;
        }
        selected_record = &record;
        for (const auto& field : record.fields) {
            if (*offset >= field.begin && *offset <= field.end) {
                selected_field = &field;
                break;
            }
        }
        break;
    }
    if (selected_record == nullptr) {
        return std::nullopt;
    }
    auto result =
        LayoutEngine{records, native_16_bit_types}.layout(*selected_record, selected_field);
    if (result.supported && parser.has_include_before(selected_record->begin) &&
        contains_matrix(result.members)) {
        result.supported = false;
        result.explanation =
            "An included file can affect matrix packing before this declaration; layout is "
            "unsupported without include expansion";
    }
    return result;
}

} // namespace hlsl_intellisense::dxc::detail
