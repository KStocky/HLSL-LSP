#include <hlsl_intellisense/dxc/intellisense.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <dxcisense.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hlsl_intellisense::dxc {
namespace {

template <typename Interface> class ComPtr final {
  public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    auto operator=(const ComPtr&) -> ComPtr& = delete;

    ComPtr(ComPtr&& other) noexcept : pointer_{std::exchange(other.pointer_, nullptr)} {}

    auto operator=(ComPtr&& other) noexcept -> ComPtr& {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    ~ComPtr() { reset(); }

    [[nodiscard]] auto get() const noexcept -> Interface* { return pointer_; }

    [[nodiscard]] auto put() noexcept -> Interface** {
        reset();
        return &pointer_;
    }

    [[nodiscard]] auto put_void() noexcept -> void** { return reinterpret_cast<void**>(put()); }

    [[nodiscard]] auto operator->() const noexcept -> Interface* { return pointer_; }

  private:
    void reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    Interface* pointer_{};
};

class Module final {
  public:
    Module() {
#ifdef _WIN32
        handle_ = ::LoadLibraryW(L"dxcompiler.dll");
#else
        handle_ = ::dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (handle_ == nullptr) {
#ifdef _WIN32
            throw std::runtime_error{"Unable to load the DXC runtime"};
#else
            const auto* error = ::dlerror();
            throw std::runtime_error{std::string{"Unable to load the DXC runtime: "} +
                                     (error != nullptr ? error : "unknown error")};
#endif
        }
    }

    Module(const Module&) = delete;
    auto operator=(const Module&) -> Module& = delete;
    Module(Module&&) = delete;
    auto operator=(Module&&) -> Module& = delete;

    ~Module() {
#ifdef _WIN32
        ::FreeLibrary(handle_);
#else
        ::dlclose(handle_);
#endif
    }

    template <typename Function> [[nodiscard]] auto get(const char* name) const -> Function {
#ifdef _WIN32
        const auto address = ::GetProcAddress(handle_, name);
#else
        const auto address = ::dlsym(handle_, name);
#endif
        if (address == nullptr) {
            throw std::runtime_error{std::string{"Unable to find DXC entry point: "} + name};
        }

        return reinterpret_cast<Function>(address);
    }

  private:
#ifdef _WIN32
    HMODULE handle_{};
#else
    void* handle_{};
#endif
};

void check(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error{std::string{operation} + " failed with HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result))};
    }
}

template <typename Interface>
[[nodiscard]] auto create(DxcCreateInstanceProc create_instance, REFCLSID class_id)
    -> ComPtr<Interface> {
    ComPtr<Interface> result;
    check(create_instance(class_id, __uuidof(Interface), result.put_void()), "DxcCreateInstance");
    return result;
}

class TaskString final {
  public:
    TaskString() = default;
    explicit TaskString(char* value) : value_{value} {}

    TaskString(const TaskString&) = delete;
    auto operator=(const TaskString&) -> TaskString& = delete;

    TaskString(TaskString&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}

    auto operator=(TaskString&& other) noexcept -> TaskString& {
        if (this != &other) {
            ::CoTaskMemFree(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    ~TaskString() { ::CoTaskMemFree(value_); }

    [[nodiscard]] auto view() const noexcept -> std::string_view {
        return value_ != nullptr ? std::string_view{value_} : std::string_view{};
    }

  private:
    char* value_{};
};

[[nodiscard]] std::size_t bstr_length(BSTR value) noexcept {
    if (value == nullptr) {
        return 0;
    }
#ifdef _WIN32
    return static_cast<std::size_t>(::SysStringLen(value));
#else
    const auto address = reinterpret_cast<std::uintptr_t>(value) - sizeof(std::uint32_t);
    const auto* byte_length = reinterpret_cast<const std::uint32_t*>(address);
    return *byte_length / sizeof(OLECHAR);
#endif
}

void free_bstr(BSTR value) noexcept {
    if (value == nullptr) {
        return;
    }
#ifdef _WIN32
    ::SysFreeString(value);
#else
    const auto address = reinterpret_cast<std::uintptr_t>(value) - sizeof(std::uint32_t);
    std::free(reinterpret_cast<void*>(address));
#endif
}

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

[[nodiscard]] std::string bstr_to_utf8(BSTR value) {
    const auto length = bstr_length(value);
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        auto code_point = static_cast<std::uint32_t>(value[index]);
        if constexpr (sizeof(OLECHAR) == 2) {
            if (code_point >= 0xD800 && code_point <= 0xDBFF && index + 1 < length) {
                const auto low = static_cast<std::uint32_t>(value[index + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                    ++index;
                } else {
                    code_point = 0xFFFD;
                }
            } else if (code_point >= 0xD800 && code_point <= 0xDFFF) {
                code_point = 0xFFFD;
            }
        }
        if (code_point > 0x10FFFF) {
            code_point = 0xFFFD;
        }
        append_utf8(result, code_point);
    }
    return result;
}

class TaskBstr final {
  public:
    TaskBstr() = default;
    explicit TaskBstr(BSTR value) : value_{value} {}

    TaskBstr(const TaskBstr&) = delete;
    auto operator=(const TaskBstr&) -> TaskBstr& = delete;

    TaskBstr(TaskBstr&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}

    auto operator=(TaskBstr&& other) noexcept -> TaskBstr& {
        if (this != &other) {
            free_bstr(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    ~TaskBstr() { free_bstr(value_); }

    [[nodiscard]] std::string utf8() const { return bstr_to_utf8(value_); }

  private:
    BSTR value_{};
};

class TaskTokens final {
  public:
    TaskTokens(IDxcToken** tokens, unsigned count) : tokens_{tokens}, count_{count} {}

    TaskTokens(const TaskTokens&) = delete;
    auto operator=(const TaskTokens&) -> TaskTokens& = delete;

    ~TaskTokens() {
        for (unsigned index = 0; index < count_; ++index) {
            if (tokens_[index] != nullptr) {
                tokens_[index]->Release();
            }
        }
        ::CoTaskMemFree(reinterpret_cast<void*>(tokens_));
    }

    [[nodiscard]] IDxcToken* operator[](unsigned index) const noexcept { return tokens_[index]; }

  private:
    IDxcToken** tokens_{};
    unsigned count_{};
};

class TaskCursors final {
  public:
    TaskCursors(IDxcCursor** cursors, unsigned count) : cursors_{cursors}, count_{count} {}

    TaskCursors(const TaskCursors&) = delete;
    auto operator=(const TaskCursors&) -> TaskCursors& = delete;

    ~TaskCursors() {
        for (unsigned index = 0; index < count_; ++index) {
            if (cursors_[index] != nullptr) {
                cursors_[index]->Release();
            }
        }
        ::CoTaskMemFree(reinterpret_cast<void*>(cursors_));
    }

    [[nodiscard]] IDxcCursor* operator[](unsigned index) const noexcept { return cursors_[index]; }

  private:
    IDxcCursor** cursors_{};
    unsigned count_{};
};

[[nodiscard]] auto make_source_location(IDxcSourceLocation& location) -> SourceLocation {
    ComPtr<IDxcFile> file;
    unsigned line{};
    unsigned column{};
    unsigned offset{};
    check(location.GetSpellingLocation(file.put(), &line, &column, &offset), "GetSpellingLocation");

    char* file_name{};
    if (file.get() != nullptr) {
        check(file->GetName(&file_name), "GetName");
    }
    TaskString owned_file_name{file_name};

    return SourceLocation{.path = std::string{owned_file_name.view()},
                          .line = line,
                          .column = column,
                          .offset = offset};
}

[[nodiscard]] auto map_severity(DxcDiagnosticSeverity severity) -> DiagnosticSeverity {
    switch (severity) {
    case DxcDiagnostic_Ignored:
        return DiagnosticSeverity::ignored;
    case DxcDiagnostic_Note:
        return DiagnosticSeverity::note;
    case DxcDiagnostic_Warning:
        return DiagnosticSeverity::warning;
    case DxcDiagnostic_Error:
        return DiagnosticSeverity::error;
    case DxcDiagnostic_Fatal:
        return DiagnosticSeverity::fatal;
    }
    throw std::runtime_error{"DXC returned an unknown diagnostic severity"};
}

[[nodiscard]] auto map_token_kind(DxcTokenKind kind) -> TokenKind {
    switch (kind) {
    case DxcTokenKind_Punctuation:
        return TokenKind::punctuation;
    case DxcTokenKind_Keyword:
        return TokenKind::keyword;
    case DxcTokenKind_Identifier:
        return TokenKind::identifier;
    case DxcTokenKind_Literal:
        return TokenKind::literal;
    case DxcTokenKind_Comment:
        return TokenKind::comment;
    case DxcTokenKind_BuiltInType:
        return TokenKind::built_in_type;
    case DxcTokenKind_Unknown:
        return TokenKind::unknown;
    }
    return TokenKind::unknown;
}

[[nodiscard]] bool supports_descriptor_heaps(std::string_view target_profile) {
    static const std::regex profile_pattern{R"(^[A-Za-z]+_([0-9]+)_([0-9]+|x)$)"};
    std::cmatch match;
    const std::string profile{target_profile};
    if (!std::regex_match(profile.c_str(), match, profile_pattern)) {
        return false;
    }

    try {
        const auto major = std::stoul(match[1].str());
        if (major > 6 || match[2].str() == "x") {
            return true;
        }
        return major == 6 && std::stoul(match[2].str()) >= 6;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] bool is_missing_descriptor_heap_diagnostic(std::string_view message) {
    return message == "use of undeclared identifier 'ResourceDescriptorHeap'" ||
           message == "use of undeclared identifier 'SamplerDescriptorHeap'";
}

[[nodiscard]] bool is_null_cursor(IDxcCursor* cursor) {
    if (cursor == nullptr) {
        return true;
    }
    BOOL is_null{};
    check(cursor->IsNull(&is_null), "IsNull");
    return is_null != FALSE;
}

[[nodiscard]] auto cursor_kind_at(IDxcTranslationUnit& translation_unit,
                                  IDxcSourceLocation& location) -> std::uint32_t {
    ComPtr<IDxcCursor> cursor;
    check(translation_unit.GetCursorForLocation(&location, cursor.put()), "GetCursorForLocation");

    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    if (referenced.get() != nullptr) {
        BOOL is_null{};
        check(referenced->IsNull(&is_null), "IsNull");
        if (is_null == FALSE) {
            cursor = std::move(referenced);
        }
    }

    DxcCursorKind kind{DxcCursor_UnexposedDecl};
    check(cursor->GetKind(&kind), "GetKind");
    return static_cast<std::uint32_t>(kind);
}

[[nodiscard]] auto safe_diagnostic_location(IDxcDiagnostic& diagnostic,
                                            std::string_view fallback_path) -> SourceLocation {
    char* formatted{};
    const auto options = static_cast<DxcDiagnosticDisplayOptions>( // NOLINT
        DxcDiagnostic_DisplaySourceLocation | DxcDiagnostic_DisplayColumn);
    if (FAILED(diagnostic.FormatDiagnostic(options, &formatted))) {
        return {.path = std::string{fallback_path}};
    }
    TaskString owned_formatted{formatted};

    static const std::regex location_pattern{R"(^(.+):([0-9]+):([0-9]+):)"};
    std::cmatch match;
    const std::string text{owned_formatted.view()};
    if (!std::regex_search(text.c_str(), match, location_pattern)) {
        return {.path = std::string{fallback_path}};
    }

    try {
        return {.path = match[1].str(),
                .line = static_cast<std::uint32_t>(std::stoul(match[2].str())),
                .column = static_cast<std::uint32_t>(std::stoul(match[3].str()))};
    } catch (const std::exception&) {
        return {.path = std::string{fallback_path}};
    }
}

[[nodiscard]] bool symbol_container(DxcCursorKind kind) {
    return kind == DxcCursor_StructDecl || kind == DxcCursor_UnionDecl ||
           kind == DxcCursor_ClassDecl || kind == DxcCursor_EnumDecl ||
           kind == DxcCursor_Namespace || kind == DxcCursor_ClassTemplate ||
           kind == DxcCursor_ClassTemplatePartialSpecialization;
}

[[nodiscard]] auto cursor_symbols(IDxcCursor& cursor, std::uint32_t depth) -> std::vector<Symbol> {
    if (depth >= 64) {
        return {};
    }

    constexpr unsigned page_size = 256;
    std::vector<Symbol> result;
    for (unsigned skip = 0;; skip += page_size) {
        unsigned child_count{};
        IDxcCursor** raw_children{};
        check(cursor.GetChildren(skip, page_size, &child_count, &raw_children), "GetChildren");
        TaskCursors children{raw_children, child_count};
        for (unsigned index = 0; index < child_count; ++index) {
            auto* child = children[index];
            if (child == nullptr) {
                continue;
            }

            DxcCursorKind kind{DxcCursor_UnexposedDecl};
            check(child->GetKind(&kind), "GetKind");
            DxcCursorKindFlags flags{DxcCursorKind_None};
            check(child->GetKindFlags(&flags), "GetKindFlags");
            const auto is_symbol =
                (flags & DxcCursorKind_Declaration) != 0 || kind == DxcCursor_MacroDefinition;
            const auto is_container = symbol_container(kind);
            if (!is_symbol) {
                continue;
            }

            char* spelling{};
            check(child->GetSpelling(&spelling), "GetSpelling");
            TaskString owned_spelling{spelling};
            auto nested = is_container ? cursor_symbols(*child, depth + 1) : std::vector<Symbol>{};
            if (owned_spelling.view().empty()) {
                result.insert(result.end(), std::make_move_iterator(nested.begin()),
                              std::make_move_iterator(nested.end()));
                continue;
            }

            ComPtr<IDxcSourceLocation> location;
            check(child->GetLocation(location.put()), "GetLocation");
            BOOL location_is_null{};
            check(location->IsNull(&location_is_null), "IsNull");
            if (location_is_null != FALSE) {
                continue;
            }

            ComPtr<IDxcSourceRange> extent;
            check(child->GetExtent(extent.put()), "GetExtent");
            unsigned start_offset{};
            unsigned end_offset{};
            check(extent->GetOffsets(&start_offset, &end_offset), "GetOffsets");
            result.push_back(Symbol{
                .name = std::string{owned_spelling.view()},
                .cursor_kind = static_cast<std::uint32_t>(kind),
                .location = make_source_location(*location.get()),
                .start_offset = start_offset,
                .end_offset = end_offset,
                .children = std::move(nested),
            });
        }
        if (child_count < page_size) {
            break;
        }
    }
    return result;
}

[[nodiscard]] bool is_identifier_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

struct IdentifierExtent {
    std::string_view name;
    std::size_t start{};
    std::size_t end{};
};

[[nodiscard]] auto identifier_at(const std::vector<SourceFile>& sources, std::string_view path,
                                 std::uint32_t line, std::uint32_t column)
    -> std::optional<IdentifierExtent> {
    const auto source = std::ranges::find(sources, path, &SourceFile::path);
    if (source == sources.end() || line == 0 || column == 0) {
        return std::nullopt;
    }

    std::size_t line_start{};
    for (std::uint32_t current_line = 1; current_line < line; ++current_line) {
        const auto line_end = source->text.find_first_of("\r\n", line_start);
        if (line_end == std::string::npos) {
            return std::nullopt;
        }
        line_start = line_end + 1;
        if (source->text[line_end] == '\r' && line_start < source->text.size() &&
            source->text[line_start] == '\n') {
            ++line_start;
        }
    }

    auto line_end = source->text.find_first_of("\r\n", line_start);
    if (line_end == std::string::npos) {
        line_end = source->text.size();
    }
    const auto position = line_start + column - 1;
    if (position >= line_end || !is_identifier_character(source->text[position])) {
        return std::nullopt;
    }

    auto start = position;
    while (start > line_start && is_identifier_character(source->text[start - 1])) {
        --start;
    }
    auto end = position + 1;
    while (end < line_end && is_identifier_character(source->text[end])) {
        ++end;
    }
    return IdentifierExtent{.name = std::string_view{source->text}.substr(start, end - start),
                            .start = start,
                            .end = end};
}

[[nodiscard]] auto find_symbol_definition(const std::vector<Symbol>& symbols, std::string_view name)
    -> std::optional<Definition> {
    for (const auto& symbol : symbols) {
        if (symbol.name == name) {
            return Definition{.name = symbol.name, .location = symbol.location};
        }
        if (const auto nested = find_symbol_definition(symbol.children, name)) {
            return nested;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string cursor_spelling(IDxcCursor& cursor) {
    char* spelling{};
    check(cursor.GetSpelling(&spelling), "GetSpelling");
    return std::string{TaskString{spelling}.view()};
}

[[nodiscard]] std::string cursor_display_name(IDxcCursor& cursor) {
    BSTR display_name{};
    check(cursor.GetDisplayName(&display_name), "GetDisplayName");
    return TaskBstr{display_name}.utf8();
}

[[nodiscard]] DxcCursorKind cursor_kind(IDxcCursor& cursor);
[[nodiscard]] bool callable_cursor(DxcCursorKind kind);
[[nodiscard]] std::string trim(std::string_view value);

[[nodiscard]] std::string cursor_qualified_symbol_name(IDxcCursor& cursor) {
    std::vector<std::string> components;
    if (auto spelling = cursor_spelling(cursor); !spelling.empty()) {
        components.push_back(std::move(spelling));
    }

    ComPtr<IDxcCursor> current;
    check(cursor.GetSemanticParent(current.put()), "GetSemanticParent");
    for (std::uint32_t depth = 0; !is_null_cursor(current.get()) && depth < 64; ++depth) {
        const auto kind = cursor_kind(*current.get());
        if (symbol_container(kind) || callable_cursor(kind)) {
            if (auto spelling = cursor_spelling(*current.get()); !spelling.empty()) {
                components.push_back(std::move(spelling));
            }
        }
        ComPtr<IDxcCursor> parent;
        check(current->GetSemanticParent(parent.put()), "GetSemanticParent");
        current = std::move(parent);
    }

    std::string result;
    for (auto component = components.rbegin(); component != components.rend(); ++component) {
        if (!result.empty()) {
            result += "::";
        }
        result += *component;
    }
    return result;
}

[[nodiscard]] std::string cursor_formatted_name(IDxcCursor& cursor) {
    BSTR formatted_name{};
    check(cursor.GetFormattedName(DxcCursorFormatting_UseLanguageOptions, &formatted_name),
          "GetFormattedName");
    return TaskBstr{formatted_name}.utf8();
}

[[nodiscard]] std::string declaration_header(IDxcCursor& cursor) {
    auto result = cursor_formatted_name(cursor);
    const auto kind = cursor_kind(cursor);
    if (callable_cursor(kind) || symbol_container(kind)) {
        if (const auto body = result.find('{'); body != std::string::npos) {
            result.erase(body);
        }
    }
    return trim(result);
}

[[nodiscard]] std::string cursor_type(IDxcCursor& cursor) {
    ComPtr<IDxcType> type;
    check(cursor.GetCursorType(type.put()), "GetCursorType");
    if (type.get() == nullptr) {
        return {};
    }
    char* spelling{};
    check(type->GetSpelling(&spelling), "GetSpelling");
    return std::string{TaskString{spelling}.view()};
}

[[nodiscard]] DxcCursorKind cursor_kind(IDxcCursor& cursor) {
    DxcCursorKind kind{DxcCursor_UnexposedDecl};
    check(cursor.GetKind(&kind), "GetKind");
    return kind;
}

[[nodiscard]] bool callable_cursor(DxcCursorKind kind) {
    return kind == DxcCursor_FunctionDecl || kind == DxcCursor_CXXMethod ||
           kind == DxcCursor_Constructor || kind == DxcCursor_ConversionFunction ||
           kind == DxcCursor_FunctionTemplate;
}

[[nodiscard]] bool type_cursor(DxcCursorKind kind) {
    return kind == DxcCursor_StructDecl || kind == DxcCursor_UnionDecl ||
           kind == DxcCursor_ClassDecl || kind == DxcCursor_ClassTemplate ||
           kind == DxcCursor_ClassTemplatePartialSpecialization;
}

[[nodiscard]] std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

[[nodiscard]] SignatureParameter signature_parameter(IDxcCursor& cursor) {
    auto name = cursor_spelling(cursor);
    auto type = cursor_type(cursor);
    auto label = cursor_formatted_name(cursor);
    if (label.empty()) {
        label = type;
        if (!name.empty()) {
            if (!label.empty()) {
                label += ' ';
            }
            label += name;
        }
    }
    return {.label = std::move(label), .name = std::move(name), .type = std::move(type)};
}

[[nodiscard]] bool append_template_parameters(IDxcCursor& cursor,
                                              std::vector<SignatureParameter>& parameters) {
    constexpr unsigned page_size = 256;
    for (unsigned skip = 0;; skip += page_size) {
        unsigned child_count{};
        IDxcCursor** raw_children{};
        check(cursor.GetChildren(skip, page_size, &child_count, &raw_children), "GetChildren");
        TaskCursors children{raw_children, child_count};
        for (unsigned index = 0; index < child_count; ++index) {
            auto* child = children[index];
            if (child != nullptr && cursor_kind(*child) == DxcCursor_ParmDecl) {
                parameters.push_back(signature_parameter(*child));
            }
        }
        if (child_count < page_size) {
            return true;
        }
        // NOLINTNEXTLINE(readability-redundant-parentheses)
        if (skip > (std::numeric_limits<unsigned>::max)() - page_size) {
            return false;
        }
    }
}

[[nodiscard]] std::optional<Signature> signature_from_cursor(IDxcCursor& cursor) {
    Signature result;
    const auto kind = cursor_kind(cursor);
    result.cursor_kind = static_cast<std::uint32_t>(kind);
    result.qualified_name = cursor_qualified_symbol_name(cursor);
    if (result.qualified_name.empty()) {
        result.qualified_name = cursor_spelling(cursor);
    }

    int argument_count{};
    check(cursor.GetNumArguments(&argument_count), "GetNumArguments");
    if (argument_count < 0) {
        if (kind != DxcCursor_FunctionTemplate ||
            !append_template_parameters(cursor, result.parameters)) {
            return std::nullopt;
        }
    } else {
        result.parameters.reserve(static_cast<std::size_t>(argument_count));
        for (int index = 0; index < argument_count; ++index) {
            ComPtr<IDxcCursor> argument;
            check(cursor.GetArgumentAt(index, argument.put()), "GetArgumentAt");
            if (is_null_cursor(argument.get())) {
                return std::nullopt;
            }
            result.parameters.push_back(signature_parameter(*argument.get()));
        }
    }

    std::string return_type;
    if (kind != DxcCursor_Constructor) {
        const auto type = cursor_type(cursor);
        if (const auto open = type.find('('); open != std::string::npos) {
            return_type = trim(std::string_view{type}.substr(0, open));
        }
    }
    if (!return_type.empty()) {
        result.label = std::move(return_type);
        result.label += ' ';
    }
    result.label += result.qualified_name;
    result.label += '(';
    for (std::size_t index = 0; index < result.parameters.size(); ++index) {
        if (index != 0) {
            result.label += ", ";
        }
        result.label += result.parameters[index].label;
    }
    result.label += ')';
    return result;
}

void append_signature(std::vector<Signature>& result, IDxcCursor& cursor) {
    auto signature = signature_from_cursor(cursor);
    if (!signature.has_value()) {
        return;
    }
    const auto duplicate = std::ranges::any_of(
        result, [&signature](const auto& existing) { return existing.label == signature->label; });
    if (!duplicate) {
        result.push_back(std::move(*signature));
    }
}

void append_named_callables(IDxcCursor& parent, std::string_view name,
                            std::vector<Signature>& result, std::uint32_t depth, bool recursive) {
    if (depth >= 64) {
        return;
    }
    constexpr unsigned page_size = 256;
    for (unsigned skip = 0;; skip += page_size) {
        unsigned child_count{};
        IDxcCursor** raw_children{};
        check(parent.GetChildren(skip, page_size, &child_count, &raw_children), "GetChildren");
        TaskCursors children{raw_children, child_count};
        for (unsigned index = 0; index < child_count; ++index) {
            auto* child = children[index];
            if (child == nullptr) {
                continue;
            }
            const auto kind = cursor_kind(*child);
            if (callable_cursor(kind) && cursor_spelling(*child) == name) {
                append_signature(result, *child);
            }
            if (recursive && !callable_cursor(kind)) {
                append_named_callables(*child, name, result, depth + 1, true);
            }
        }
        if (child_count < page_size) {
            break;
        }
    }
}

} // namespace

struct Intellisense::Impl final {
    Impl()
        : create_instance{module.get<DxcCreateInstanceProc>("DxcCreateInstance")},
          intellisense{create<IDxcIntelliSense>(create_instance, CLSID_DxcIntelliSense)} {
        check(intellisense->CreateIndex(index.put()), "CreateIndex");
    }

    Module module;
    DxcCreateInstanceProc create_instance;
    ComPtr<IDxcIntelliSense> intellisense;
    ComPtr<IDxcIndex> index;
};

struct TranslationUnit::Impl final {
    std::shared_ptr<Intellisense::Impl> owner;
    std::string root_path;
    std::vector<SourceFile> sources;
    std::vector<std::string> arguments;
    bool descriptor_heaps_supported{};
    std::vector<ComPtr<IDxcUnsavedFile>> unsaved_files;
    ComPtr<IDxcTranslationUnit> translation_unit;

    void rebuild_unsaved_files() {
        unsaved_files.clear();
        unsaved_files.reserve(sources.size());

        for (const auto& source : sources) {
            ComPtr<IDxcUnsavedFile> unsaved_file;
            check(owner->intellisense->CreateUnsavedFile(source.path.c_str(), source.text.data(),
                                                         static_cast<unsigned>(source.text.size()),
                                                         unsaved_file.put()),
                  "CreateUnsavedFile");
            unsaved_files.push_back(std::move(unsaved_file));
        }
    }

    [[nodiscard]] auto unsaved_file_pointers() const -> std::vector<IDxcUnsavedFile*> {
        std::vector<IDxcUnsavedFile*> pointers;
        pointers.reserve(unsaved_files.size());
        std::ranges::transform(unsaved_files, std::back_inserter(pointers),
                               [](const auto& file) { return file.get(); });
        return pointers;
    }

    void parse_translation_unit() {
        std::vector<const char*> argument_pointers;
        argument_pointers.reserve(arguments.size());
        std::ranges::transform(arguments, std::back_inserter(argument_pointers),
                               [](const auto& argument) { return argument.c_str(); });
        auto file_pointers = unsaved_file_pointers();

        check(owner->index->ParseTranslationUnit(
                  root_path.c_str(), argument_pointers.data(),
                  static_cast<int>(argument_pointers.size()), file_pointers.data(),
                  static_cast<unsigned>(file_pointers.size()),
                  DxcTranslationUnitFlags_UseCallerThread, translation_unit.put()),
              "ParseTranslationUnit");
    }
};

auto CompilerOptions::arguments() const -> std::vector<std::string> {
    std::vector<std::string> result;
    if (!language_version.empty()) {
        result.emplace_back("-HV");
        result.push_back(language_version);
    }
    if (!target_profile.empty()) {
        result.emplace_back("-T");
        result.push_back(target_profile);
    }
    if (!entry_point.empty()) {
        result.emplace_back("-E");
        result.push_back(entry_point);
    }
    for (const auto& define : defines) {
        result.emplace_back("-D");
        result.push_back(define);
    }
    for (const auto& include_directory : include_directories) {
        result.emplace_back("-I");
        result.push_back(include_directory);
    }
    result.insert(result.end(), additional_arguments.begin(), additional_arguments.end());
    return result;
}

TranslationUnit::TranslationUnit(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

TranslationUnit::TranslationUnit(TranslationUnit&&) noexcept = default;
auto TranslationUnit::operator=(TranslationUnit&&) noexcept -> TranslationUnit& = default;
TranslationUnit::~TranslationUnit() = default;

auto TranslationUnit::diagnostics() const -> std::vector<Diagnostic> {
    unsigned count{};
    check(implementation_->translation_unit->GetNumDiagnostics(&count), "GetNumDiagnostics");

    std::vector<Diagnostic> result;
    result.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        ComPtr<IDxcDiagnostic> diagnostic;
        check(implementation_->translation_unit->GetDiagnostic(index, diagnostic.put()),
              "GetDiagnostic");

        DxcDiagnosticSeverity severity{};
        check(diagnostic->GetSeverity(&severity), "GetSeverity");

        char* spelling{};
        check(diagnostic->GetSpelling(&spelling), "GetSpelling");
        TaskString owned_spelling{spelling};
        if (implementation_->descriptor_heaps_supported &&
            is_missing_descriptor_heap_diagnostic(owned_spelling.view())) {
            continue;
        }

        result.push_back(Diagnostic{
            .severity = map_severity(severity),
            .message = std::string{owned_spelling.view()},
            .location = safe_diagnostic_location(*diagnostic.get(), implementation_->root_path)});
    }
    return result;
}

auto TranslationUnit::complete(std::string_view path, std::uint32_t line,
                               std::uint32_t column) const -> std::vector<Completion> {
    auto unsaved_files = implementation_->unsaved_file_pointers();
    ComPtr<IDxcCodeCompleteResults> results;
    const std::string owned_path{path};
    check(implementation_->translation_unit->CodeCompleteAt(
              owned_path.c_str(), line, column, unsaved_files.data(),
              static_cast<unsigned>(unsaved_files.size()), DxcCodeCompleteFlags_None,
              results.put()),
          "CodeCompleteAt");

    unsigned result_count{};
    check(results->GetNumResults(&result_count), "GetNumResults");

    std::vector<Completion> completions;
    completions.reserve(result_count);
    for (unsigned result_index = 0; result_index < result_count; ++result_index) {
        ComPtr<IDxcCompletionResult> result;
        check(results->GetResultAt(result_index, result.put()), "GetResultAt");

        DxcCursorKind cursor_kind{DxcCursor_UnexposedDecl};
        check(result->GetCursorKind(&cursor_kind), "GetCursorKind");

        ComPtr<IDxcCompletionString> completion_string;
        check(result->GetCompletionString(completion_string.put()), "GetCompletionString");

        unsigned chunk_count{};
        check(completion_string->GetNumCompletionChunks(&chunk_count), "GetNumCompletionChunks");

        Completion completion{};
        completion.cursor_kind = static_cast<std::uint32_t>(cursor_kind);
        for (unsigned chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            DxcCompletionChunkKind chunk_kind{};
            check(completion_string->GetCompletionChunkKind(chunk_index, &chunk_kind),
                  "GetCompletionChunkKind");

            char* chunk_text{};
            check(completion_string->GetCompletionChunkText(chunk_index, &chunk_text),
                  "GetCompletionChunkText");
            TaskString owned_chunk_text{chunk_text};
            completion.detail.append(owned_chunk_text.view());
            if (chunk_kind == DxcCompletionChunk_TypedText) {
                completion.label = owned_chunk_text.view();
            }
        }
        if (!completion.label.empty()) {
            completions.push_back(std::move(completion));
        }
    }
    return completions;
}

auto TranslationUnit::definition_at(std::string_view path, std::uint32_t line,
                                    std::uint32_t column) const -> std::optional<Definition> {
    const std::string owned_path{path};
    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(owned_path.c_str(), file.put()), "GetFile");

    ComPtr<IDxcSourceLocation> location;
    check(implementation_->translation_unit->GetLocation(file.get(), line, column, location.put()),
          "GetLocation");

    ComPtr<IDxcCursor> cursor;
    check(implementation_->translation_unit->GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");

    ComPtr<IDxcCursor> definition;
    check(cursor->GetDefinitionCursor(definition.put()), "GetDefinitionCursor");
    if (is_null_cursor(definition.get())) {
        ComPtr<IDxcCursor> referenced;
        check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
        if (is_null_cursor(referenced.get())) {
            const auto identifier = identifier_at(implementation_->sources, path, line, column);
            if (!identifier) {
                return std::nullopt;
            }
            return find_symbol_definition(symbols(), identifier->name);
        }

        check(referenced->GetDefinitionCursor(definition.put()), "GetDefinitionCursor");
        if (is_null_cursor(definition.get())) {
            definition = std::move(referenced);
        }
    }

    char* spelling{};
    check(definition->GetSpelling(&spelling), "GetSpelling");
    TaskString owned_spelling{spelling};

    ComPtr<IDxcSourceLocation> definition_location;
    check(definition->GetLocation(definition_location.put()), "GetLocation");

    return Definition{.name = std::string{owned_spelling.view()},
                      .location = make_source_location(*definition_location.get())};
}

auto TranslationUnit::references_at(std::string_view path, std::uint32_t line,
                                    std::uint32_t column) const -> std::vector<Reference> {
    const auto source = std::ranges::find(implementation_->sources, path, &SourceFile::path);
    if (source == implementation_->sources.end()) {
        return {};
    }

    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(source->path.c_str(), file.put()), "GetFile");
    ComPtr<IDxcSourceLocation> location;
    check(implementation_->translation_unit->GetLocation(file.get(), line, column, location.put()),
          "GetLocation");
    ComPtr<IDxcCursor> cursor;
    check(implementation_->translation_unit->GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");
    if (is_null_cursor(cursor.get())) {
        return {};
    }

    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    auto* target = is_null_cursor(referenced.get()) ? cursor.get() : referenced.get();

    constexpr unsigned page_size = 256;
    std::vector<Reference> result;
    for (const auto& candidate_source : implementation_->sources) {
        ComPtr<IDxcFile> candidate_file;
        check(implementation_->translation_unit->GetFile(candidate_source.path.c_str(),
                                                         candidate_file.put()),
              "GetFile");
        for (unsigned skip = 0;; skip += page_size) {
            unsigned count{};
            IDxcCursor** raw_references{};
            check(target->FindReferencesInFile(candidate_file.get(), skip, page_size, &count,
                                               &raw_references),
                  "FindReferencesInFile");
            TaskCursors references{raw_references, count};
            for (unsigned index = 0; index < count; ++index) {
                auto* reference = references[index];
                if (reference == nullptr) {
                    continue;
                }
                ComPtr<IDxcSourceLocation> reference_location;
                check(reference->GetLocation(reference_location.put()), "GetLocation");
                const auto resolved = make_source_location(*reference_location.get());
                const auto identifier = identifier_at(implementation_->sources, resolved.path,
                                                      resolved.line, resolved.column);
                if (!identifier.has_value() || identifier->start > UINT32_MAX ||
                    identifier->end > UINT32_MAX) {
                    continue;
                }
                result.push_back({.location = resolved,
                                  .start_offset = static_cast<std::uint32_t>(identifier->start),
                                  .end_offset = static_cast<std::uint32_t>(identifier->end)});
            }
            if (count < page_size) {
                break;
            }
        }
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return std::tie(left.location.path, left.start_offset) <
               std::tie(right.location.path, right.start_offset);
    });
    result.erase(std::ranges::unique(result, {},
                                     [](const auto& reference) {
                                         return std::tie(reference.location.path,
                                                         reference.start_offset);
                                     })
                     .begin(),
                 result.end());
    return result;
}

auto TranslationUnit::hover_at(std::string_view path, std::uint32_t line,
                               std::uint32_t column) const -> std::optional<Hover> {
    const auto identifier = identifier_at(implementation_->sources, path, line, column);
    if (!identifier.has_value()) {
        return std::nullopt;
    }
    if (identifier->start > UINT32_MAX || identifier->end > UINT32_MAX) {
        throw std::invalid_argument{"The hover source file is too large"};
    }

    const std::string owned_path{path};
    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(owned_path.c_str(), file.put()), "GetFile");
    ComPtr<IDxcSourceLocation> location;
    check(implementation_->translation_unit->GetLocation(file.get(), line, column, location.put()),
          "GetLocation");
    ComPtr<IDxcCursor> cursor;
    check(implementation_->translation_unit->GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");
    if (is_null_cursor(cursor.get())) {
        return std::nullopt;
    }

    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    auto* target = is_null_cursor(referenced.get()) ? cursor.get() : referenced.get();
    auto name = cursor_spelling(*target);
    if (name.empty()) {
        return std::nullopt;
    }

    SourceLocation declaration_location;
    ComPtr<IDxcSourceLocation> target_location;
    check(target->GetLocation(target_location.put()), "GetLocation");
    if (target_location.get() != nullptr) {
        BOOL location_is_null{};
        check(target_location->IsNull(&location_is_null), "IsNull");
        if (location_is_null == FALSE) {
            declaration_location = make_source_location(*target_location.get());
        }
    }

    return Hover{
        .name = std::move(name),
        .qualified_name = cursor_qualified_symbol_name(*target),
        .display_name = cursor_display_name(*target),
        .type = cursor_type(*target),
        .declaration = declaration_header(*target),
        .cursor_kind = static_cast<std::uint32_t>(cursor_kind(*target)),
        .declaration_location = std::move(declaration_location),
        .start_offset = static_cast<std::uint32_t>(identifier->start),
        .end_offset = static_cast<std::uint32_t>(identifier->end),
    };
}

auto TranslationUnit::signatures_at(std::string_view path, std::uint32_t line,
                                    std::uint32_t column) const -> std::vector<Signature> {
    const auto identifier = identifier_at(implementation_->sources, path, line, column);
    if (!identifier.has_value()) {
        return {};
    }

    const std::string owned_path{path};
    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(owned_path.c_str(), file.put()), "GetFile");
    ComPtr<IDxcSourceLocation> location;
    check(implementation_->translation_unit->GetLocation(file.get(), line, column, location.put()),
          "GetLocation");
    ComPtr<IDxcCursor> cursor;
    check(implementation_->translation_unit->GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");
    if (is_null_cursor(cursor.get())) {
        return {};
    }

    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    auto* target = is_null_cursor(referenced.get()) ? cursor.get() : referenced.get();
    const auto target_kind = cursor_kind(*target);
    std::vector<Signature> result;
    if (callable_cursor(target_kind)) {
        append_signature(result, *target);
        ComPtr<IDxcCursor> parent;
        check(target->GetSemanticParent(parent.put()), "GetSemanticParent");
        if (!is_null_cursor(parent.get())) {
            append_named_callables(*parent.get(), identifier->name, result, 0, false);
        }
    } else if (type_cursor(target_kind)) {
        append_named_callables(*target, identifier->name, result, 0, false);
    }

    if (result.empty()) {
        ComPtr<IDxcCursor> root;
        check(implementation_->translation_unit->GetCursor(root.put()), "GetCursor");
        append_named_callables(*root.get(), identifier->name, result, 0, true);
    }
    return result;
}

auto TranslationUnit::tokens(std::string_view path) const -> std::vector<Token> {
    const std::string owned_path{path};
    const auto source = std::ranges::find(implementation_->sources, owned_path, &SourceFile::path);
    if (source == implementation_->sources.end()) {
        throw std::invalid_argument{"The token source file is missing"};
    }
    // NOLINTNEXTLINE(readability-redundant-parentheses)
    if (source->text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::invalid_argument{"The token source file is too large"};
    }
    const auto text_length = static_cast<std::uint32_t>(source->text.size());
    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(owned_path.c_str(), file.put()), "GetFile");

    ComPtr<IDxcSourceLocation> start;
    check(implementation_->translation_unit->GetLocationForOffset(file.get(), 0, start.put()),
          "GetLocationForOffset");
    ComPtr<IDxcSourceLocation> end;
    check(
        implementation_->translation_unit->GetLocationForOffset(file.get(), text_length, end.put()),
        "GetLocationForOffset");
    ComPtr<IDxcSourceRange> range;
    check(implementation_->owner->intellisense->GetRange(start.get(), end.get(), range.put()),
          "GetRange");

    IDxcToken** raw_tokens{};
    unsigned token_count{};
    check(implementation_->translation_unit->Tokenize(range.get(), &raw_tokens, &token_count),
          "Tokenize");
    TaskTokens owned_tokens{raw_tokens, token_count};

    std::vector<Token> result;
    result.reserve(token_count);
    for (unsigned index = 0; index < token_count; ++index) {
        auto* token = owned_tokens[index];
        DxcTokenKind kind{DxcTokenKind_Unknown};
        check(token->GetKind(&kind), "GetKind");

        ComPtr<IDxcSourceRange> extent;
        check(token->GetExtent(extent.put()), "GetExtent");
        unsigned token_start{};
        unsigned token_end{};
        check(extent->GetOffsets(&token_start, &token_end), "GetOffsets");
        if (token_end <= token_start) {
            continue;
        }

        ComPtr<IDxcSourceLocation> location;
        check(token->GetLocation(location.put()), "GetLocation");
        const auto source_location = make_source_location(*location.get());
        std::uint32_t cursor_kind{};
        if (kind == DxcTokenKind_Identifier) {
            cursor_kind = cursor_kind_at(*implementation_->translation_unit.get(), *location.get());
        }
        result.push_back({.line = source_location.line,
                          .column = source_location.column,
                          .length = token_end - token_start,
                          .kind = map_token_kind(kind),
                          .cursor_kind = cursor_kind});
    }
    return result;
}

auto TranslationUnit::symbols() const -> std::vector<Symbol> {
    ComPtr<IDxcCursor> root;
    check(implementation_->translation_unit->GetCursor(root.put()), "GetCursor");
    return cursor_symbols(*root.get(), 0);
}

void TranslationUnit::reparse(std::vector<SourceFile> files) {
    implementation_->sources = std::move(files);
    implementation_->rebuild_unsaved_files();
#ifdef _WIN32
    auto pointers = implementation_->unsaved_file_pointers();
    check(implementation_->translation_unit->Reparse(pointers.data(),
                                                     static_cast<unsigned>(pointers.size())),
          "Reparse");
#else
    // DXC 1.9.2607's native Reparse has been observed to crash on Linux. An
    // in-process crash cannot be recovered safely, so rebuild with the same
    // index, arguments, and unsaved buffers instead.
    implementation_->parse_translation_unit();
#endif
}

Intellisense::Intellisense() : implementation_{std::make_shared<Impl>()} {}

Intellisense::Intellisense(Intellisense&&) noexcept = default;
auto Intellisense::operator=(Intellisense&&) noexcept -> Intellisense& = default;
Intellisense::~Intellisense() = default;

auto Intellisense::parse(std::string root_path, std::vector<SourceFile> files,
                         const CompilerOptions& options) const -> TranslationUnit {
    if (files.empty()) {
        throw std::invalid_argument{"At least one source file is required"};
    }
    if (std::ranges::none_of(
            files, [&root_path](const SourceFile& file) { return file.path == root_path; })) {
        throw std::invalid_argument{"The root source file is missing"};
    }

    auto implementation = std::make_unique<TranslationUnit::Impl>();
    implementation->owner = implementation_;
    implementation->root_path = std::move(root_path);
    implementation->sources = std::move(files);
    implementation->descriptor_heaps_supported = supports_descriptor_heaps(options.target_profile);
    implementation->rebuild_unsaved_files();

    implementation->arguments = options.arguments();
    implementation->parse_translation_unit();

    return TranslationUnit{std::move(implementation)};
}

} // namespace hlsl_intellisense::dxc
