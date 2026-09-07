#include <hlsl_intellisense/workspace/include_resolver.h>

#include <hlsl_intellisense/workspace/document_uri.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hlsl_intellisense::workspace {
namespace {

struct SourceNode {
    std::filesystem::path physical_path;
    std::string logical_path;
    std::string text;
    bool virtual_path{};
    bool open{};
    std::string virtual_mapping;
};

[[nodiscard]] std::string_view trim_left(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view value) {
    value = trim_left(value);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool identifier_start(char value) {
    const auto character = static_cast<unsigned char>(value);
    return value == '_' || std::isalpha(character) != 0;
}

[[nodiscard]] bool identifier_continue(char value) {
    const auto character = static_cast<unsigned char>(value);
    return value == '_' || std::isalnum(character) != 0;
}

[[nodiscard]] std::optional<std::string_view> identifier_token(std::string_view value) {
    if (value.empty() || !identifier_start(value.front())) {
        return std::nullopt;
    }
    std::size_t length = 1;
    while (length < value.size() && identifier_continue(value[length])) {
        ++length;
    }
    return value.substr(0, length);
}

inline constexpr std::size_t max_scanned_logical_line_bytes = 256U * 1024U;
inline constexpr std::size_t max_phase2_chunk_bytes = 64U * 1024U;
inline constexpr std::size_t initial_scanned_logical_line_capacity = 4U * 1024U;

struct ScannedLogicalLine {
    std::string text;
    std::vector<std::size_t> source_offsets;
    std::size_t source_offset{};
    bool ambiguous{};
};

struct Phase2Chunk {
    std::string text;
    std::vector<std::size_t> source_offsets;
};

class Phase2LogicalByteStream final {
  public:
    explicit Phase2LogicalByteStream(std::string_view source) : source_{source} {}

    [[nodiscard]] std::optional<Phase2Chunk> next() {
        if (cursor_ == source_.size()) {
            return std::nullopt;
        }

        Phase2Chunk chunk;
        chunk.text.reserve(std::min(source_.size() - cursor_, max_phase2_chunk_bytes));
        chunk.source_offsets.reserve(chunk.text.capacity());
        while (cursor_ < source_.size() && chunk.text.size() < max_phase2_chunk_bytes) {
            if (source_[cursor_] == '\\') {
                const auto newline_length = newline_size(cursor_ + 1);
                if (newline_length != 0) {
                    cursor_ += 1 + newline_length;
                    continue;
                }
            }

            const auto newline_length = newline_size(cursor_);
            if (newline_length != 0) {
                chunk.text.push_back('\n');
                chunk.source_offsets.push_back(cursor_);
                cursor_ += newline_length;
                continue;
            }
            chunk.text.push_back(source_[cursor_]);
            chunk.source_offsets.push_back(cursor_);
            ++cursor_;
        }

        if (chunk.text.empty()) {
            return std::nullopt;
        }
        return chunk;
    }

    [[nodiscard]] std::size_t source_offset() const noexcept { return cursor_; }

  private:
    [[nodiscard]] std::size_t newline_size(std::size_t offset) const noexcept {
        if (offset >= source_.size()) {
            return 0;
        }
        if (source_[offset] == '\n') {
            return 1;
        }
        if (source_[offset] == '\r') {
            return offset + 1 < source_.size() && source_[offset + 1] == '\n' ? 2 : 1;
        }
        return 0;
    }

    std::string_view source_;
    std::size_t cursor_{};
};

class PreprocessingLogicalLineScanner final {
  public:
    explicit PreprocessingLogicalLineScanner(std::string_view source) : stream_{source} {
        skip_initial_utf8_bom();
    }

    [[nodiscard]] std::optional<ScannedLogicalLine> next() {
        if (finished_) {
            return std::nullopt;
        }

        ScannedLogicalLine line{.text = {},
                                .source_offsets = {},
                                .source_offset = stream_.source_offset(),
                                .ambiguous = false};
        line.text.reserve(initial_scanned_logical_line_capacity);
        line.source_offsets.reserve(initial_scanned_logical_line_capacity);
        std::optional<MappedByte> pending_slash;

        const auto append = [&](char character, std::size_t source_offset) {
            if (line.text.size() == max_scanned_logical_line_bytes) {
                line.ambiguous = true;
                return;
            }
            line.text.push_back(character);
            line.source_offsets.push_back(source_offset);
        };

        while (const auto byte = next_byte()) {
            if (line.text.empty() && !pending_slash) {
                line.source_offset = byte->source_offset;
            }
            if (byte->character == '\n') {
                if (pending_slash) {
                    append('/', pending_slash->source_offset);
                }
                pending_slash.reset();
                block_comment_maybe_closing_ = false;
                if (state_ == State::string_literal || state_ == State::character_literal) {
                    line.ambiguous = true;
                    state_ = State::normal;
                    escaped_ = false;
                } else if (state_ == State::line_comment) {
                    state_ = State::normal;
                }
                return line;
            }

            if (state_ == State::line_comment) {
                continue;
            }
            if (state_ == State::block_comment) {
                if (block_comment_maybe_closing_ && byte->character == '/') {
                    state_ = State::normal;
                    block_comment_maybe_closing_ = false;
                } else {
                    block_comment_maybe_closing_ = byte->character == '*';
                }
                continue;
            }
            if (state_ == State::string_literal || state_ == State::character_literal) {
                append(byte->character, byte->source_offset);
                if (escaped_) {
                    escaped_ = false;
                } else if (byte->character == '\\') {
                    escaped_ = true;
                } else if ((state_ == State::string_literal && byte->character == '"') ||
                           (state_ == State::character_literal && byte->character == '\'')) {
                    state_ = State::normal;
                }
                continue;
            }

            if (pending_slash) {
                if (byte->character == '/') {
                    append(' ', pending_slash->source_offset);
                    pending_slash.reset();
                    state_ = State::line_comment;
                    continue;
                }
                if (byte->character == '*') {
                    append(' ', pending_slash->source_offset);
                    block_comment_line_offset_ = line.source_offset;
                    pending_slash.reset();
                    state_ = State::block_comment;
                    block_comment_maybe_closing_ = false;
                    continue;
                }
                append('/', pending_slash->source_offset);
                pending_slash.reset();
            }

            if (byte->character == '/') {
                pending_slash = byte;
                continue;
            }
            if (byte->character == '"') {
                state_ = State::string_literal;
            } else if (byte->character == '\'') {
                state_ = State::character_literal;
            }
            append(byte->character, byte->source_offset);
        }

        if (pending_slash) {
            append('/', pending_slash->source_offset);
        }
        finished_ = true;
        if (state_ == State::block_comment) {
            line.ambiguous = true;
            line.source_offset = std::min(line.source_offset, block_comment_line_offset_);
        } else if (state_ == State::string_literal || state_ == State::character_literal) {
            line.ambiguous = true;
        }
        state_ = State::normal;
        if (line.text.empty() && !line.ambiguous) {
            return std::nullopt;
        }
        return line;
    }

  private:
    struct MappedByte {
        char character{};
        std::size_t source_offset{};
    };

    enum class State : std::uint8_t {
        normal,
        line_comment,
        block_comment,
        string_literal,
        character_literal
    };

    [[nodiscard]] std::optional<MappedByte> next_stream_byte() {
        while (!chunk_ || chunk_cursor_ == chunk_->text.size()) {
            chunk_ = stream_.next();
            chunk_cursor_ = 0;
            if (!chunk_) {
                return std::nullopt;
            }
        }
        const auto index = chunk_cursor_++;
        return MappedByte{.character = chunk_->text[index],
                          .source_offset = chunk_->source_offsets[index]};
    }

    [[nodiscard]] std::optional<MappedByte> next_byte() {
        if (replay_cursor_ < replay_size_) {
            return replay_[replay_cursor_++];
        }
        return next_stream_byte();
    }

    void skip_initial_utf8_bom() {
        constexpr std::array<char, 3> utf8_bom{static_cast<char>(0xEF), static_cast<char>(0xBB),
                                               static_cast<char>(0xBF)};
        while (replay_size_ < utf8_bom.size()) {
            const auto byte = next_stream_byte();
            if (!byte) {
                break;
            }
            replay_[replay_size_++] = *byte;
        }
        const auto is_initial_bom =
            replay_size_ == utf8_bom.size() && replay_[0].character == utf8_bom[0] &&
            replay_[1].character == utf8_bom[1] && replay_[2].character == utf8_bom[2] &&
            replay_[0].source_offset == 0 && replay_[1].source_offset == 1 &&
            replay_[2].source_offset == 2;
        if (is_initial_bom) {
            replay_size_ = 0;
        }
    }

    Phase2LogicalByteStream stream_;
    std::optional<Phase2Chunk> chunk_;
    std::array<MappedByte, 3> replay_{};
    std::size_t chunk_cursor_{};
    std::size_t replay_cursor_{};
    std::size_t replay_size_{};
    std::size_t block_comment_line_offset_{};
    State state_{State::normal};
    bool escaped_{};
    bool block_comment_maybe_closing_{};
    bool finished_{};
};

[[nodiscard]] std::optional<std::string> remove_preprocessing_comments(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    char quote{};
    bool escaped{};
    for (std::size_t index = 0; index < value.size();) {
        const auto character = value[index];
        if (quote != '\0') {
            result.push_back(character);
            ++index;
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = '\0';
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
            result.push_back(character);
            ++index;
            continue;
        }
        if (character != '/' || index + 1 >= value.size()) {
            result.push_back(character);
            ++index;
            continue;
        }
        if (value[index + 1] == '/') {
            result.push_back(' ');
            break;
        }
        if (value[index + 1] != '*') {
            result.push_back(character);
            ++index;
            continue;
        }
        const auto comment_end = value.find("*/", index + 2);
        if (comment_end == std::string_view::npos) {
            return std::nullopt;
        }
        result.push_back(' ');
        index = comment_end + 2;
    }
    return result;
}

[[nodiscard]] bool keyword_matches(std::string_view line, std::string_view keyword) {
    return line.starts_with(keyword) &&
           (line.size() == keyword.size() || !identifier_continue(line[keyword.size()]));
}

[[nodiscard]] std::size_t trim_left_index(std::string_view value, std::size_t index = 0) {
    while (index < value.size() && std::isspace(static_cast<unsigned char>(value[index])) != 0) {
        ++index;
    }
    return index;
}

[[nodiscard]] std::size_t trim_right_index(std::string_view value, std::size_t end) {
    while (end != 0 && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return end;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> source_span(const ScannedLogicalLine& line,
                                                              std::size_t begin, std::size_t end) {
    if (begin >= end || begin >= line.source_offsets.size() || end > line.source_offsets.size()) {
        return {line.source_offset, 0};
    }
    const auto source_begin = line.source_offsets[begin];
    return {source_begin, line.source_offsets[end - 1] + 1 - source_begin};
}

[[nodiscard]] IncludeMetadata parse_includes(std::string_view text) {
    IncludeMetadata result;
    PreprocessingLogicalLineScanner scanner{text};
    while (auto scanned = scanner.next()) {
        if (scanned->ambiguous) {
            result.has_dynamic = true;
            result.source_macro_directives.push_back(
                {.name = {}, .directive_offset = scanned->source_offset, .state_unknown = true});
        }

        const auto line = std::string_view{scanned->text};
        auto directive_start = trim_left_index(line);
        if (directive_start == line.size() || line[directive_start] != '#') {
            continue;
        }
        directive_start = trim_left_index(line, directive_start + 1);
        if (directive_start == line.size()) {
            continue;
        }

        const auto directive_offset = scanned->source_offsets[directive_start];
        const auto directive_text = line.substr(directive_start);
        constexpr std::string_view include_keyword = "include";
        if (keyword_matches(directive_text, include_keyword)) {
            const auto operand_start =
                trim_left_index(line, directive_start + include_keyword.size());
            if (operand_start < line.size() &&
                (line[operand_start] == '"' || line[operand_start] == '<')) {
                const auto terminator = line[operand_start] == '"' ? '"' : '>';
                const auto end = line.find(terminator, operand_start + 1);
                if (end != std::string_view::npos) {
                    const auto [source_offset, source_length] =
                        source_span(*scanned, operand_start, end + 1);
                    const auto path_begin = operand_start + 1;
                    const auto path_offset = path_begin < end
                                                 ? scanned->source_offsets[path_begin]
                                                 : scanned->source_offsets[operand_start] + 1;
                    result.directives.push_back(
                        {.path = std::string{line.substr(path_begin, end - path_begin)},
                         .path_offset = path_offset,
                         .source_offset = source_offset,
                         .source_length = source_length,
                         .quoted = line[operand_start] == '"'});
                }
            } else if (operand_start < line.size()) {
                const auto operand_end = trim_right_index(line, line.size());
                if (operand_start < operand_end) {
                    const auto [source_offset, source_length] =
                        source_span(*scanned, operand_start, operand_end);
                    result.has_dynamic = true;
                    result.dynamic_directives.push_back(
                        {.expression =
                             std::string{line.substr(operand_start, operand_end - operand_start)},
                         .expression_offset = scanned->source_offsets[operand_start],
                         .source_offset = source_offset,
                         .source_length = source_length});
                }
            }
        }

        constexpr std::string_view define_keyword = "define";
        constexpr std::string_view undef_keyword = "undef";
        const auto macro_keyword = keyword_matches(directive_text, define_keyword) ? define_keyword
                                   : keyword_matches(directive_text, undef_keyword)
                                       ? undef_keyword
                                       : std::string_view{};
        if (!macro_keyword.empty()) {
            const auto macro_text = trim_left(directive_text.substr(macro_keyword.size()));
            if (const auto name = identifier_token(macro_text)) {
                result.source_macro_directives.push_back({.name = std::string{*name},
                                                          .directive_offset = directive_offset,
                                                          .state_unknown = false});
            } else {
                result.source_macro_directives.push_back(
                    {.name = {}, .directive_offset = directive_offset, .state_unknown = true});
            }
        }
    }
    return result;
}

struct ConfiguredInclude {
    IncludeDirective directive;
    std::string macro;
    std::string origin;
    std::string origin_file;
    std::vector<std::string> expansion_macros;
};

inline constexpr std::size_t max_configured_include_expansion_depth = 32;
inline constexpr std::size_t max_configured_include_expansion_bytes = 4096;
inline constexpr std::size_t max_additional_argument_count = 4096;
inline constexpr std::size_t max_additional_argument_bytes = 1024U * 1024U;
inline constexpr std::size_t max_command_line_macro_name_bytes = 4096;

struct AdditionalArgumentMacroAnalysis {
    std::unordered_set<std::string> modified_macros;
    bool opaque{};
};

[[nodiscard]] AdditionalArgumentMacroAnalysis
analyze_additional_arguments(const std::vector<std::string>& arguments) {
    AdditionalArgumentMacroAnalysis result;
    if (arguments.size() > max_additional_argument_count) {
        result.opaque = true;
        return result;
    }

    std::size_t scanned_bytes{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument.size() > max_additional_argument_bytes - scanned_bytes) {
            result.opaque = true;
            return result;
        }
        scanned_bytes += argument.size();

        if (argument.starts_with('@')) {
            result.opaque = true;
            return result;
        }

        const auto is_macro_switch = argument.size() >= 2 &&
                                     (argument[0] == '-' || argument[0] == '/') &&
                                     (argument[1] == 'D' || argument[1] == 'U');
        if (!is_macro_switch) {
            continue;
        }

        std::string_view operand;
        if (argument.size() == 2) {
            if (++index == arguments.size()) {
                result.opaque = true;
                return result;
            }
            const auto& split_operand = arguments[index];
            if (split_operand.size() > max_additional_argument_bytes - scanned_bytes) {
                result.opaque = true;
                return result;
            }
            scanned_bytes += split_operand.size();
            operand = split_operand;
        } else {
            operand = std::string_view{argument}.substr(2);
        }

        const auto name = identifier_token(operand);
        if (!name || name->size() > max_command_line_macro_name_bytes) {
            result.opaque = true;
            return result;
        }
        result.modified_macros.emplace(*name);
    }
    return result;
}

[[nodiscard]] std::optional<IncludeDirective> header_name_directive(std::string_view value,
                                                                    std::size_t expression_offset,
                                                                    std::size_t source_offset,
                                                                    std::size_t source_length) {
    value = trim(value);
    if (value.size() < 3 || (value.front() != '"' && value.front() != '<')) {
        return std::nullopt;
    }
    const auto terminator = value.front() == '"' ? '"' : '>';
    const auto end = value.find(terminator, 1);
    if (end != value.size() - 1) {
        return std::nullopt;
    }
    return IncludeDirective{.path = std::string{value.substr(1, value.size() - 2)},
                            .path_offset = expression_offset,
                            .source_offset = source_offset,
                            .source_length = source_length,
                            .quoted = value.front() == '"'};
}

[[nodiscard]] std::optional<ConfiguredInclude>
configured_include(const DynamicIncludeDirective& include,
                   const WorkspaceConfiguration& configuration,
                   const AdditionalArgumentMacroAnalysis& additional_arguments,
                   const std::unordered_set<std::string>* source_macros = nullptr) {
    if (additional_arguments.opaque) {
        return std::nullopt;
    }
    const auto uncommented_expression = remove_preprocessing_comments(include.expression);
    if (!uncommented_expression) {
        return std::nullopt;
    }
    const auto expression = trim(*uncommented_expression);
    const auto first_name = identifier_token(expression);
    if (!first_name || first_name->size() != expression.size()) {
        return std::nullopt;
    }

    std::string macro{*first_name};
    std::string expansion{*first_name};
    std::unordered_set<std::string> visited;
    std::vector<std::string> expansion_macros;
    for (std::size_t depth = 0; depth < max_configured_include_expansion_depth; ++depth) {
        if (!visited.insert(expansion).second ||
            additional_arguments.modified_macros.contains(expansion) ||
            (source_macros != nullptr && source_macros->contains(expansion))) {
            return std::nullopt;
        }
        expansion_macros.push_back(expansion);
        const auto definition = configuration.preprocessor_definitions.find(expansion);
        if (definition == configuration.preprocessor_definitions.end()) {
            return std::nullopt;
        }

        if (definition->second.size() > max_configured_include_expansion_bytes) {
            return std::nullopt;
        }
        const auto uncommented_value = remove_preprocessing_comments(definition->second);
        if (!uncommented_value) {
            return std::nullopt;
        }
        const auto value = trim(*uncommented_value);
        if (auto directive = header_name_directive(value, include.expression_offset,
                                                   include.source_offset, include.source_length)) {
            ConfiguredInclude result{.directive = std::move(*directive),
                                     .macro = std::move(macro),
                                     .origin = {},
                                     .origin_file = {},
                                     .expansion_macros = std::move(expansion_macros)};
            if (const auto origin = configuration.definition_origins.find(result.macro);
                origin != configuration.definition_origins.end()) {
                result.origin = origin->second;
            }
            if (const auto origin_file = configuration.definition_origin_files.find(result.macro);
                origin_file != configuration.definition_origin_files.end()) {
                result.origin_file = origin_file->second.generic_string();
            }
            return result;
        }

        const auto alias = identifier_token(value);
        if (!alias || alias->size() != value.size()) {
            return std::nullopt;
        }
        expansion = std::string{*alias};
    }
    return std::nullopt;
}

[[nodiscard]] std::filesystem::path normalized_physical_path(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

[[nodiscard]] std::string physical_identity(const std::filesystem::path& path) {
    return DocumentUri::from_path(normalized_physical_path(path).string()).identity();
}

[[nodiscard]] std::string normalized_virtual_path(std::string_view path) {
    std::string portable_path{path};
    std::ranges::replace(portable_path, '\\', '/');
    auto result = std::filesystem::path{portable_path}.lexically_normal().generic_string();
    if (!result.starts_with('/')) {
        result.insert(result.begin(), '/');
    }
    return result;
}

[[nodiscard]] bool virtual_prefix_matches(std::string_view path, std::string_view prefix) {
    return path == prefix ||
           (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

class Resolver final {
  public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Resolver(std::span<const SourceSnapshot> open_documents,
             const WorkspaceConfiguration& configuration, IncludeMetadataCache* cache)
        : configuration_{configuration}, additional_argument_macros_{analyze_additional_arguments(
                                             configuration.additional_arguments)},
          cache_{cache} {
        for (const auto& document : open_documents) {
            open_documents_.emplace(
                document.document_uri().identity(),
                SourceNode{.physical_path = normalized_physical_path(document.path()),
                           .logical_path =
                               normalized_physical_path(document.path()).generic_string(),
                           .text = document.text(),
                           .virtual_path = false,
                           .open = true,
                           .virtual_mapping = {}});
        }
    }

    [[nodiscard]] IncludeResolution resolve(const SourceSnapshot& root) {
        IncludeResolution result;
        SourceNode root_node{.physical_path = normalized_physical_path(root.path()),
                             .logical_path = normalized_physical_path(root.path()).generic_string(),
                             .text = root.text(),
                             .virtual_path = false,
                             .open = true,
                             .virtual_mapping = {}};
        visit(root_node, result, true);
        return result;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<std::filesystem::path> resolve_at(const SourceSnapshot& root,
                                                                  std::size_t utf8_offset) {
        const auto resolution = resolve(root);
        const auto root_path = normalized_physical_path(root.path()).generic_string();
        const auto file =
            std::ranges::find(resolution.files, root_path, &IncludeResolution::File::physical_path);
        if (file == resolution.files.end()) {
            return std::nullopt;
        }
        const auto edge = std::ranges::find_if(file->includes, [utf8_offset](const auto& include) {
            const auto start = include.source_offset;
            const auto end = include.source_offset + include.source_length;
            return utf8_offset >= start && utf8_offset < end;
        });
        if (edge == file->includes.end() || edge->resolved_path.empty()) {
            return std::nullopt;
        }
        return normalized_physical_path(edge->resolved_path);
    }

  private:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] IncludeMetadata metadata(std::string_view identity, std::string_view text) const {
        return cache_ == nullptr ? parse_includes(text) : cache_->get(identity, text);
    }

    [[nodiscard]] std::optional<std::string>
    source_text(const std::filesystem::path& physical_path) const {
        const auto open = open_documents_.find(physical_identity(physical_path));
        if (open != open_documents_.end()) {
            return open->second.text;
        }

        std::ifstream stream{physical_path, std::ios::binary};
        if (!stream) {
            return std::nullopt;
        }
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] std::optional<SourceNode> virtual_source(std::string_view include_path,
                                                           IncludeResolution& result) const {
        const auto normalized_include = normalized_virtual_path(include_path);
        const std::pair<std::string, std::filesystem::path>* best = nullptr;
        std::pair<std::string, std::filesystem::path> candidate;
        for (const auto& [configured_prefix, physical_directory] :
             configuration_.virtual_directory_mappings) {
            auto prefix = normalized_virtual_path(configured_prefix);
            if (virtual_prefix_matches(normalized_include, prefix) &&
                (best == nullptr || prefix.size() > best->first.size())) {
                candidate = {std::move(prefix), physical_directory};
                best = &candidate;
            }
        }
        if (best == nullptr) {
            return std::nullopt;
        }

        auto suffix = normalized_include.substr(best->first.size());
        while (!suffix.empty() && suffix.front() == '/') {
            suffix.erase(suffix.begin());
        }
        const auto physical_path =
            normalized_physical_path(best->second / std::filesystem::path{suffix});
        result.dependency_identities.insert(physical_identity(physical_path));
        auto text = source_text(physical_path);
        if (!text) {
            return std::nullopt;
        }
        return SourceNode{.physical_path = physical_path,
                          .logical_path = normalized_include,
                          .text = std::move(*text),
                          .virtual_path = true,
                          .open = open_documents_.contains(physical_identity(physical_path)),
                          .virtual_mapping = best->first};
    }

    [[nodiscard]] std::optional<SourceNode>
    physical_source(const std::filesystem::path& physical_path, IncludeResolution& result,
                    const std::optional<std::string>& logical_path = std::nullopt) const {
        const auto normalized = normalized_physical_path(physical_path);
        result.dependency_identities.insert(physical_identity(normalized));
        auto text = source_text(normalized);
        if (!text) {
            return std::nullopt;
        }
        return SourceNode{.physical_path = normalized,
                          .logical_path = logical_path.value_or(normalized.generic_string()),
                          .text = std::move(*text),
                          .virtual_path = logical_path.has_value(),
                          .open = open_documents_.contains(physical_identity(normalized)),
                          .virtual_mapping = {}};
    }

    [[nodiscard]] std::optional<SourceNode> resolve_directive(const SourceNode& source,
                                                              const IncludeDirective& directive,
                                                              IncludeResolution& result) const {
        if (directive.path.starts_with('/') || directive.path.starts_with('\\')) {
            if (auto resolved = virtual_source(directive.path, result)) {
                return resolved;
            }
        }

        if (directive.quoted) {
            const auto physical_path = source.physical_path.parent_path() / directive.path;
            if (source.virtual_path) {
                const auto logical_path = normalized_virtual_path(
                    std::filesystem::path{source.logical_path}.parent_path().generic_string() +
                    "/" + directive.path);
                if (auto resolved = physical_source(physical_path, result, logical_path)) {
                    return resolved;
                }
            } else if (auto resolved = physical_source(physical_path, result)) {
                return resolved;
            }
        }

        for (const auto& include_directory : configuration_.additional_include_directories) {
            if (auto resolved = physical_source(include_directory / directive.path, result)) {
                return resolved;
            }
        }
        return std::nullopt;
    }

    void visit(const SourceNode& source, IncludeResolution& result, bool root) {
        const auto identity = physical_identity(source.physical_path);
        if (active_physical_paths_.contains(identity)) {
            return;
        }
        if (const auto existing = emitted_files_.find(source.logical_path);
            existing != emitted_files_.end()) {
            if (source_macro_state_unknown_ ||
                std::ranges::any_of(existing->second.configuration_macros,
                                    [this](const auto& macro) {
                                        return encountered_source_macros_.contains(macro);
                                    })) {
                make_context_dependent(identity, result);
            }
            return;
        }
        if (!root) {
            result.dependency_identities.insert(identity);
        }
        const auto first_physical_visit = visited_physical_paths_.insert(identity).second;
        if (!first_physical_visit) {
            const auto configured_macros = physical_configuration_macros_.find(identity);
            if (source_macro_state_unknown_ ||
                (configured_macros != physical_configuration_macros_.end() &&
                 std::ranges::any_of(configured_macros->second, [this](const auto& macro) {
                     return encountered_source_macros_.contains(macro);
                 }))) {
                make_context_dependent(identity, result);
            }
        }
        active_physical_paths_.insert(identity);

        const auto parsed = metadata(identity, source.text);
        IncludeResolution::File file{.physical_path = source.physical_path.generic_string(),
                                     .logical_path = source.logical_path,
                                     .open = source.open,
                                     .includes = {},
                                     .source_text = source.text};
        auto dxc_text = source.text;
        struct Rewrite {
            std::size_t offset{};
            std::size_t length{};
            std::string text;
        };
        struct DirectiveEvent {
            std::size_t offset{};
            const IncludeDirective* literal{};
            const DynamicIncludeDirective* dynamic{};
            const SourceMacroDirective* source_macro{};
        };
        std::vector<Rewrite> rewrites;
        std::vector<DirectiveEvent> events;
        events.reserve(parsed.directives.size() + parsed.dynamic_directives.size() +
                       parsed.source_macro_directives.size());
        for (const auto& directive : parsed.directives) {
            events.push_back({.offset = directive.path_offset, .literal = &directive});
        }
        for (const auto& dynamic : parsed.dynamic_directives) {
            events.push_back({.offset = dynamic.expression_offset, .dynamic = &dynamic});
        }
        for (const auto& source_macro : parsed.source_macro_directives) {
            events.push_back(
                {.offset = source_macro.directive_offset, .source_macro = &source_macro});
        }
        std::ranges::sort(events, {}, &DirectiveEvent::offset);

        const auto file_index = result.files.size();
        result.files.emplace_back();
        auto [emitted, inserted] = emitted_files_.try_emplace(
            source.logical_path, EmittedFile{.file_index = file_index,
                                             .original_text = source.text,
                                             .configuration_macros = {}});
        static_cast<void>(inserted);
        physical_logical_paths_[identity].push_back(source.logical_path);
        const auto logical_source_index =
            add_source(source.logical_path, source.text, identity, result);
        std::optional<std::size_t> physical_source_index;
        if (source.virtual_path && first_physical_visit) {
            physical_source_index =
                add_source(normalized_physical_path(source.physical_path).generic_string(),
                           source.text, identity, result);
        }

        bool has_unresolved_dynamic = false;
        for (const auto& event : events) {
            if (event.source_macro != nullptr) {
                if (event.source_macro->state_unknown) {
                    source_macro_state_unknown_ = true;
                    has_unresolved_dynamic = true;
                } else {
                    encountered_source_macros_.insert(event.source_macro->name);
                }
                continue;
            }
            if (event.literal != nullptr) {
                const auto& directive = *event.literal;
                if (auto included = resolve_directive(source, directive, result)) {
                    const auto included_identity = physical_identity(included->physical_path);
                    file.includes.push_back(
                        {.source_path = source.physical_path.generic_string(),
                         .requested_path = directive.path,
                         .path_offset = directive.path_offset,
                         .source_offset = directive.source_offset,
                         .source_length = directive.source_length,
                         .quoted = directive.quoted,
                         .status = active_physical_paths_.contains(included_identity)
                                       ? IncludeResolution::Status::cyclic
                                       : IncludeResolution::Status::resolved,
                         .resolved_path = included->physical_path.generic_string(),
                         .logical_path = included->logical_path,
                         .virtual_mapping = included->virtual_mapping,
                         .macro_expanded = false,
                         .expanded_path = {},
                         .configuration_macro = {},
                         .configuration_origin = {},
                         .configuration_origin_file = {}});
                    if (directive.path.starts_with('/') || directive.path.starts_with('\\')) {
                        const auto opening = directive.quoted ? '"' : '<';
                        const auto closing = directive.quoted ? '"' : '>';
                        rewrites.push_back({.offset = directive.source_offset,
                                            .length = directive.source_length,
                                            .text = std::string{opening} +
                                                    included->physical_path.generic_string() +
                                                    closing});
                    }
                    visit(*included, result, false);
                } else {
                    file.includes.push_back({.source_path = source.physical_path.generic_string(),
                                             .requested_path = directive.path,
                                             .path_offset = directive.path_offset,
                                             .source_offset = directive.source_offset,
                                             .source_length = directive.source_length,
                                             .quoted = directive.quoted,
                                             .status = IncludeResolution::Status::missing,
                                             .resolved_path = {},
                                             .logical_path = {},
                                             .virtual_mapping = {},
                                             .macro_expanded = false,
                                             .expanded_path = {},
                                             .configuration_macro = {},
                                             .configuration_origin = {},
                                             .configuration_origin_file = {}});
                }
                continue;
            }

            const auto& dynamic = *event.dynamic;
            const auto configured =
                source_macro_state_unknown_
                    ? std::nullopt
                    : configured_include(dynamic, configuration_, additional_argument_macros_,
                                         &encountered_source_macros_);
            if (configured) {
                emitted->second.configuration_macros.insert(configured->expansion_macros.begin(),
                                                            configured->expansion_macros.end());
                physical_configuration_macros_[identity].insert(
                    configured->expansion_macros.begin(), configured->expansion_macros.end());
                if (auto included = resolve_directive(source, configured->directive, result)) {
                    const auto included_identity = physical_identity(included->physical_path);
                    file.includes.push_back(
                        {.source_path = source.physical_path.generic_string(),
                         .requested_path = dynamic.expression,
                         .path_offset = dynamic.expression_offset,
                         .source_offset = dynamic.source_offset,
                         .source_length = dynamic.source_length,
                         .quoted = configured->directive.quoted,
                         .status = active_physical_paths_.contains(included_identity)
                                       ? IncludeResolution::Status::cyclic
                                       : IncludeResolution::Status::resolved,
                         .resolved_path = included->physical_path.generic_string(),
                         .logical_path = included->logical_path,
                         .virtual_mapping = included->virtual_mapping,
                         .macro_expanded = true,
                         .expanded_path = configured->directive.path,
                         .configuration_macro = configured->macro,
                         .configuration_origin = configured->origin,
                         .configuration_origin_file = configured->origin_file});
                    if (!included->virtual_mapping.empty()) {
                        rewrites.push_back(
                            {.offset = dynamic.source_offset,
                             .length = dynamic.source_length,
                             .text = "\"" + included->physical_path.generic_string() + "\""});
                    }
                    visit(*included, result, false);
                    continue;
                }
                file.includes.push_back({.source_path = source.physical_path.generic_string(),
                                         .requested_path = dynamic.expression,
                                         .path_offset = dynamic.expression_offset,
                                         .source_offset = dynamic.source_offset,
                                         .source_length = dynamic.source_length,
                                         .quoted = configured->directive.quoted,
                                         .status = IncludeResolution::Status::missing,
                                         .resolved_path = {},
                                         .logical_path = {},
                                         .virtual_mapping = {},
                                         .macro_expanded = true,
                                         .expanded_path = configured->directive.path,
                                         .configuration_macro = configured->macro,
                                         .configuration_origin = configured->origin,
                                         .configuration_origin_file = configured->origin_file});
                continue;
            }

            has_unresolved_dynamic = true;
            source_macro_state_unknown_ = true;
            file.includes.push_back({.source_path = source.physical_path.generic_string(),
                                     .requested_path = dynamic.expression,
                                     .path_offset = dynamic.expression_offset,
                                     .source_offset = dynamic.source_offset,
                                     .source_length = dynamic.source_length,
                                     .quoted = false,
                                     .status = IncludeResolution::Status::dynamic,
                                     .resolved_path = {},
                                     .logical_path = {},
                                     .virtual_mapping = {},
                                     .macro_expanded = true,
                                     .expanded_path = {},
                                     .configuration_macro = {},
                                     .configuration_origin = {},
                                     .configuration_origin_file = {}});
        }
        std::ranges::sort(rewrites, {}, [](const auto& rewrite) { return rewrite.offset; });
        for (auto rewrite = rewrites.rbegin(); rewrite != rewrites.rend(); ++rewrite) {
            dxc_text.replace(rewrite->offset, rewrite->length, rewrite->text);
        }
        std::ranges::sort(file.includes, {}, &IncludeResolution::Edge::path_offset);
        result.files[file_index] = std::move(file);
        if (context_dependent_physical_paths_.contains(identity)) {
            restore_original_sources(identity, result);
        } else {
            result.sources[logical_source_index].text = dxc_text;
            if (physical_source_index) {
                result.sources[*physical_source_index].text = dxc_text;
            }
        }
        if (has_unresolved_dynamic) {
            if (!first_physical_visit) {
                make_context_dependent(identity, result);
            } else {
                add_dynamic_dependencies(identity, result);
            }
        }
        active_physical_paths_.erase(identity);
    }

    struct EmittedFile {
        std::size_t file_index{};
        std::string original_text;
        std::unordered_set<std::string> configuration_macros;
    };

    [[nodiscard]] std::size_t add_source(std::string path, std::string_view text,
                                         std::string_view identity, IncludeResolution& result) {
        if (const auto existing = source_indices_.find(path); existing != source_indices_.end()) {
            return existing->second;
        }
        const auto index = result.sources.size();
        source_indices_.emplace(path, index);
        physical_source_indices_[std::string{identity}].push_back(index);
        result.sources.push_back({std::move(path), std::string{text}});
        return index;
    }

    void add_dynamic_dependencies(std::string_view identity, IncludeResolution& result) {
        result.has_dynamic_includes = true;
        for (const auto& [open_identity, open_document] : open_documents_) {
            if (open_identity != identity) {
                result.dependency_identities.insert(open_identity);
                static_cast<void>(add_source(open_document.logical_path, open_document.text,
                                             open_identity, result));
            }
        }
    }

    void restore_original_sources(std::string_view identity, IncludeResolution& result) {
        const auto logical_paths = physical_logical_paths_.find(std::string{identity});
        if (logical_paths == physical_logical_paths_.end() || logical_paths->second.empty()) {
            return;
        }
        const auto& original = emitted_files_.at(logical_paths->second.front()).original_text;
        for (const auto index : physical_source_indices_.at(std::string{identity})) {
            result.sources[index].text = original;
        }
    }

    void make_context_dependent(std::string_view identity, IncludeResolution& result) {
        const auto identity_string = std::string{identity};
        if (!context_dependent_physical_paths_.insert(identity_string).second) {
            add_dynamic_dependencies(identity, result);
            return;
        }

        restore_original_sources(identity, result);
        if (const auto logical_paths = physical_logical_paths_.find(identity_string);
            logical_paths != physical_logical_paths_.end()) {
            for (const auto& logical_path : logical_paths->second) {
                auto& file = result.files[emitted_files_.at(logical_path).file_index];
                for (auto& include : file.includes) {
                    if (include.configuration_macro.empty()) {
                        continue;
                    }
                    include.status = IncludeResolution::Status::dynamic;
                    include.resolved_path.clear();
                    include.logical_path.clear();
                    include.virtual_mapping.clear();
                    include.expanded_path.clear();
                    include.configuration_macro.clear();
                    include.configuration_origin.clear();
                    include.configuration_origin_file.clear();
                }
            }
        }
        add_dynamic_dependencies(identity, result);
    }

    const WorkspaceConfiguration& configuration_;
    AdditionalArgumentMacroAnalysis additional_argument_macros_;
    IncludeMetadataCache* cache_;
    std::unordered_map<std::string, SourceNode> open_documents_;
    std::unordered_map<std::string, EmittedFile> emitted_files_;
    std::unordered_map<std::string, std::size_t> source_indices_;
    std::unordered_map<std::string, std::vector<std::size_t>> physical_source_indices_;
    std::unordered_map<std::string, std::vector<std::string>> physical_logical_paths_;
    std::unordered_map<std::string, std::unordered_set<std::string>> physical_configuration_macros_;
    std::unordered_set<std::string> context_dependent_physical_paths_;
    std::unordered_set<std::string> visited_physical_paths_;
    std::unordered_set<std::string> active_physical_paths_;
    std::unordered_set<std::string> encountered_source_macros_;
    bool source_macro_state_unknown_{};
};

} // namespace

struct IncludeMetadataCache::Impl final {
    struct Entry final {
        std::string identity;
        std::string content;
        IncludeMetadata metadata;
        std::size_t estimated_bytes{};
    };

    explicit Impl(IncludeCacheLimits value) : limits{value} {
        if (limits.max_entries == 0 || limits.max_estimated_bytes == 0) {
            throw std::invalid_argument{"Include cache limits must be positive"};
        }
    }

    [[nodiscard]] static std::size_t estimate(const Entry& entry) noexcept {
        std::size_t bytes = sizeof(Entry) + 3U * sizeof(void*) + entry.identity.capacity() +
                            entry.content.capacity();
        bytes += entry.metadata.directives.capacity() * sizeof(IncludeDirective);
        for (const auto& directive : entry.metadata.directives) {
            bytes += directive.path.capacity();
        }
        bytes += entry.metadata.dynamic_directives.capacity() * sizeof(DynamicIncludeDirective);
        for (const auto& directive : entry.metadata.dynamic_directives) {
            bytes += directive.expression.capacity();
        }
        bytes += entry.metadata.source_macro_directives.capacity() * sizeof(SourceMacroDirective);
        for (const auto& directive : entry.metadata.source_macro_directives) {
            bytes += directive.name.capacity();
        }
        return bytes;
    }

    IncludeCacheLimits limits;
    std::list<Entry> entries;
    IncludeCacheMetrics metrics;
};

IncludeMetadataCache::IncludeMetadataCache(IncludeCacheLimits limits)
    : implementation_{std::make_unique<Impl>(limits)} {}

IncludeMetadataCache::IncludeMetadataCache(IncludeMetadataCache&&) noexcept = default;
auto IncludeMetadataCache::operator=(IncludeMetadataCache&&) noexcept
    -> IncludeMetadataCache& = default;
IncludeMetadataCache::~IncludeMetadataCache() = default;

IncludeMetadata IncludeMetadataCache::get(std::string_view identity, std::string_view text) {
    auto& implementation = *implementation_;
    const auto found = std::ranges::find_if(implementation.entries, [&](const auto& entry) {
        return entry.identity == identity && entry.content == text;
    });
    if (found != implementation.entries.end()) {
        ++implementation.metrics.hits;
        auto metadata = found->metadata;
        implementation.entries.splice(implementation.entries.begin(), implementation.entries,
                                      found);
        return metadata;
    }

    ++implementation.metrics.misses;
    Impl::Entry candidate{.identity = std::string{identity},
                          .content = std::string{text},
                          .metadata = parse_includes(text)};
    candidate.estimated_bytes = Impl::estimate(candidate);
    auto result = candidate.metadata;
    if (candidate.estimated_bytes > implementation.limits.max_estimated_bytes) {
        return result;
    }

    implementation.metrics.estimated_bytes += candidate.estimated_bytes;
    implementation.entries.push_front(std::move(candidate));
    while (implementation.entries.size() > implementation.limits.max_entries ||
           implementation.metrics.estimated_bytes > implementation.limits.max_estimated_bytes) {
        implementation.metrics.estimated_bytes -= implementation.entries.back().estimated_bytes;
        implementation.entries.pop_back();
        ++implementation.metrics.evictions;
    }
    implementation.metrics.entries = implementation.entries.size();
    return result;
}

void IncludeMetadataCache::invalidate(std::string_view identity) {
    auto& implementation = *implementation_;
    for (auto entry = implementation.entries.begin(); entry != implementation.entries.end();) {
        if (entry->identity == identity) {
            implementation.metrics.estimated_bytes -= entry->estimated_bytes;
            entry = implementation.entries.erase(entry);
            ++implementation.metrics.evictions;
        } else {
            ++entry;
        }
    }
    implementation.metrics.entries = implementation.entries.size();
}

void IncludeMetadataCache::clear() noexcept {
    implementation_->entries.clear();
    implementation_->metrics.entries = 0;
    implementation_->metrics.estimated_bytes = 0;
}

IncludeCacheMetrics IncludeMetadataCache::metrics() const noexcept {
    return implementation_->metrics;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
IncludeResolution resolve_includes(const SourceSnapshot& root,
                                   std::span<const SourceSnapshot> open_documents,
                                   const WorkspaceConfiguration& configuration,
                                   IncludeMetadataCache* cache) {
    return Resolver{open_documents, configuration, cache}.resolve(root);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<std::filesystem::path>
resolve_include_at(const SourceSnapshot& root, std::span<const SourceSnapshot> open_documents,
                   const WorkspaceConfiguration& configuration, std::size_t utf8_offset,
                   IncludeMetadataCache* cache) {
    return Resolver{open_documents, configuration, cache}.resolve_at(root, utf8_offset);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool is_include_directive_at(std::string_view text, std::size_t utf8_offset) {
    const auto parsed = parse_includes(text);
    return std::ranges::any_of(parsed.directives,
                               [utf8_offset](const auto& item) {
                                   return utf8_offset >= item.source_offset &&
                                          utf8_offset < item.source_offset + item.source_length;
                               }) ||
           std::ranges::any_of(parsed.dynamic_directives, [utf8_offset](const auto& item) {
               return utf8_offset >= item.source_offset &&
                      utf8_offset < item.source_offset + item.source_length;
           });
}

} // namespace hlsl_intellisense::workspace
