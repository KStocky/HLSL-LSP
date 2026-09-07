#include <hlsl_intellisense/dxc/intellisense.h>

#include "compilation_info.h"
#include "memory_layout.h"

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
#include <filesystem>
#include <iterator>
#include <limits>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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

[[nodiscard]] std::string loader_error() {
#ifdef _WIN32
    return {};
#else
    const auto* error = ::dlerror();
    return error != nullptr ? std::string{error} : std::string{};
#endif
}

#ifdef _WIN32
[[nodiscard]] std::string module_file_name(HMODULE handle) {
    if (handle == nullptr) {
        return {};
    }
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length =
            ::GetModuleFileNameW(handle, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= std::size_t{1} << 16U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
    try {
        return std::filesystem::path{buffer}.string();
    } catch (const std::exception&) {
        return {};
    }
}
#endif

class Module final {
  public:
    explicit Module(std::string_view directory) {
        if (directory.empty()) {
            load_bundled();
        } else {
            load_from_directory(directory);
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

    [[nodiscard]] auto library_path() const noexcept -> const std::string& { return library_path_; }

    [[nodiscard]] auto bundled() const noexcept -> bool { return bundled_; }

  private:
    void load_bundled() {
        bundled_ = true;
#ifdef _WIN32
        handle_ = ::LoadLibraryW(L"dxcompiler.dll");
#else
        handle_ = ::dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (handle_ == nullptr) {
            const auto detail = loader_error();
            throw RuntimeError{std::string{"Unable to load the bundled DXC runtime"} +
                               (detail.empty() ? "" : ": " + detail)};
        }
#ifdef _WIN32
        library_path_ = module_file_name(handle_);
#endif
        if (library_path_.empty()) {
            library_path_ = std::string{runtime_library_name()};
        }
    }

    void load_from_directory(std::string_view directory) {
        bundled_ = false;
        library_path_ = validate_runtime_directory(directory);
        const std::filesystem::path library{library_path_};
#ifdef _WIN32
        handle_ = ::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
        handle_ = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (handle_ == nullptr) {
            const auto detail = loader_error();
            throw RuntimeError{"Unable to load the selected DXC runtime '" + library_path_ + "'" +
                               (detail.empty() ? "" : ": " + detail)};
        }
    }

#ifdef _WIN32
    HMODULE handle_{};
#else
    void* handle_{};
#endif
    std::string library_path_;
    bool bundled_{true};
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

class TaskRanges final {
  public:
    TaskRanges(IDxcSourceRange** ranges, unsigned count) : ranges_{ranges}, count_{count} {}

    TaskRanges(const TaskRanges&) = delete;
    auto operator=(const TaskRanges&) -> TaskRanges& = delete;

    ~TaskRanges() {
        for (unsigned index = 0; index < count_; ++index) {
            if (ranges_[index] != nullptr) {
                ranges_[index]->Release();
            }
        }
        ::CoTaskMemFree(reinterpret_cast<void*>(ranges_));
    }

    [[nodiscard]] IDxcSourceRange* operator[](unsigned index) const noexcept {
        return ranges_[index];
    }

  private:
    IDxcSourceRange** ranges_{};
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

// Converts a DXC source range into a compiler-owned SourceRange, or nullopt
// when the range is absent, null, spans more than one file, or is malformed
// (end before start). DXC never intentionally emits cross-file or
// out-of-order ranges for a single diagnostic; a range failing these checks
// is treated as unusable rather than guessed at.
//
// This is only ever called on ranges returned from IDxcDiagnostic::GetFixItAt.
// IDxcDiagnostic::GetRangeAt (the general per-diagnostic "referenced ranges"
// API paired with GetNumRanges) is never called: empirically, pinned DXC
// 1.9.2607.13 crashes (access violation) inside GetRangeAt whenever
// GetNumRanges reports a non-zero count, reproduced for multiple unrelated
// diagnostic categories (undeclared-member and assignment-in-condition
// diagnostics). GetNumRanges itself does not crash; only dereferencing the
// range it announces does. GetFixItAt's replacement range does not share this
// defect and was verified safe across dozens of diagnostics, including ones
// whose GetNumRanges was non-zero, so only fix-it ranges are surfaced here.
[[nodiscard]] auto safe_source_range(IDxcSourceRange* range) -> std::optional<SourceRange> {
    if (range == nullptr) {
        return std::nullopt;
    }
    BOOL is_null{};
    check(range->IsNull(&is_null), "IsNull");
    if (is_null != FALSE) {
        return std::nullopt;
    }

    ComPtr<IDxcSourceLocation> start_location;
    ComPtr<IDxcSourceLocation> end_location;
    check(range->GetStart(start_location.put()), "GetStart");
    check(range->GetEnd(end_location.put()), "GetEnd");
    if (start_location.get() == nullptr || end_location.get() == nullptr) {
        return std::nullopt;
    }

    auto start = make_source_location(*start_location.get());
    auto end = make_source_location(*end_location.get());
    if (start.path.empty() || end.path.empty() || start.path != end.path ||
        end.offset < start.offset) {
        return std::nullopt;
    }
    return SourceRange{.start = std::move(start), .end = std::move(end)};
}

// Extracts DXC's compiler-owned fix-its for a diagnostic. Empirically (pinned
// DXC 1.9.2607.13), fix-its are only ever populated for a narrow set of
// diagnostics: an inserted ";" for some (not all) "expected ';'" parse errors,
// and a replacement with the suggested spelling for some "did you mean '...'?"
// identifier/type/function corrections. Every other diagnostic category probed
// (undeclared identifiers without a close match, struct member typos,
// assignment-used-as-condition, missing includes, missing return, implicit
// narrowing, redefinitions, argument-count mismatches, type mismatches,
// unknown attributes, and "expected ')'": ) reported zero fix-its. Callers must
// not assume a fix-it exists for any diagnostic message or category; this
// function is the only source of truth.
[[nodiscard]] auto safe_diagnostic_fix_its(IDxcDiagnostic& diagnostic) -> std::vector<FixIt> {
    unsigned count{};
    check(diagnostic.GetNumFixIts(&count), "GetNumFixIts");

    std::vector<FixIt> result;
    result.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        ComPtr<IDxcSourceRange> range;
        char* text{};
        check(diagnostic.GetFixItAt(index, range.put(), &text), "GetFixItAt");
        TaskString owned_text{text};
        auto safe_range = safe_source_range(range.get());
        if (!safe_range) {
            continue;
        }
        result.push_back(FixIt{.range = std::move(*safe_range),
                               .replacement_text = std::string{owned_text.view()}});
    }
    return result;
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

// Flattens the document-symbol tree into every named declaration cursor it
// contains, recording all matches per name (not just the first) so callers
// can detect genuine ambiguity. `Symbol.children` already reflects exactly
// what DXC's cursor tree exposes: function bodies are never walked (locals
// cannot collide with resource names), while struct/union/class/enum/
// namespace members are, so a same-named field on an unrelated struct is
// correctly treated as a collision rather than silently ignored.
void collect_symbols_by_name(const std::vector<Symbol>& symbols,
                             std::unordered_map<std::string, std::vector<const Symbol*>>& by_name) {
    for (const auto& symbol : symbols) {
        if (!symbol.name.empty()) {
            by_name[symbol.name].push_back(&symbol);
        }
        collect_symbols_by_name(symbol.children, by_name);
    }
}

// Attaches a `source_location` to each reflected resource whose name matches
// exactly one declaration cursor in `document_symbols` -- the same DXC
// IntelliSense parse index already used for hover/go-to-definition/document
// symbols over the current unsaved snapshot. Resources are never guessed:
// a name with zero matches (e.g. reflected but not found by the parse
// index, which can happen if the two DXC entry points ever disagree) or
// more than one match (a genuine ambiguity the cursor tree itself cannot
// resolve) is left without a location rather than reporting a possibly
// wrong one.
void attach_resource_source_locations(CompilationInfo& info,
                                      const std::vector<Symbol>& document_symbols) {
    if (!info.reflection.has_value() || info.reflection->resources.empty()) {
        return;
    }
    std::unordered_map<std::string, std::vector<const Symbol*>> by_name;
    collect_symbols_by_name(document_symbols, by_name);
    for (auto& resource : info.reflection->resources) {
        if (resource.name.empty()) {
            continue;
        }
        const auto it = by_name.find(resource.name);
        if (it == by_name.end() || it->second.size() != 1) {
            continue;
        }
        resource.source_location = it->second.front()->location;
    }
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
[[nodiscard]] bool type_cursor(DxcCursorKind kind);
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

[[nodiscard]] std::string inferred_cursor_type(IDxcCursor& declaration) {
    auto declared = cursor_type(declaration);
    if (!declared.empty() && declared.find("auto") == std::string::npos &&
        declared.find("dependent") == std::string::npos &&
        declared.find("<error") == std::string::npos) {
        return declared;
    }

    constexpr unsigned max_children = 64;
    unsigned child_count{};
    IDxcCursor** raw_children{};
    check(declaration.GetChildren(0, max_children, &child_count, &raw_children), "GetChildren");
    TaskCursors children{raw_children, child_count};
    for (unsigned index = 0; index < child_count; ++index) {
        if (children[index] == nullptr) {
            continue;
        }
        auto type = cursor_type(*children[index]);
        if (!type.empty() && type != "auto" && type.find("dependent") == std::string::npos &&
            type.find("<error") == std::string::npos) {
            return type;
        }
    }
    return {};
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

[[nodiscard]] bool expression_cursor(DxcCursorKind kind) {
    return kind >= DxcCursor_FirstExpr && kind <= DxcCursor_LastExpr;
}

[[nodiscard]] std::optional<std::string> layout_container_key(IDxcCursor& cursor) {
    const auto parent_key = [](IDxcCursor& parent) -> std::optional<std::string> {
        const auto kind = cursor_kind(parent);
        if (kind == DxcCursor_StructDecl) {
            auto name = cursor_qualified_symbol_name(parent);
            return name.empty() ? std::nullopt
                                : std::optional<std::string>{"record:" + std::move(name)};
        }
        if (kind == DxcCursor_UnexposedDecl) {
            const auto formatted = cursor_formatted_name(parent);
            if (formatted.starts_with("cbuffer ")) {
                ComPtr<IDxcSourceLocation> location;
                check(parent.GetLocation(location.put()), "GetLocation");
                if (location.get() == nullptr) {
                    return std::nullopt;
                }
                const auto source = make_source_location(*location.get());
                return "cbuffer:" + formatted + ':' + source.path + ':' +
                       std::to_string(source.offset);
            }
        }
        return std::nullopt;
    };

    ComPtr<IDxcCursor> semantic;
    check(cursor.GetSemanticParent(semantic.put()), "GetSemanticParent");
    if (!is_null_cursor(semantic.get())) {
        if (auto key = parent_key(*semantic.get())) {
            return key;
        }
    }
    ComPtr<IDxcCursor> lexical;
    check(cursor.GetLexicalParent(lexical.put()), "GetLexicalParent");
    if (!is_null_cursor(lexical.get())) {
        return parent_key(*lexical.get());
    }
    return std::nullopt;
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

[[nodiscard]] std::string query_runtime_version(DxcCreateInstanceProc create_instance) {
    try {
        auto compiler = create<IDxcCompiler>(create_instance, CLSID_DxcCompiler);
        ComPtr<IDxcVersionInfo> version;
        if (FAILED(compiler->QueryInterface(__uuidof(IDxcVersionInfo), version.put_void())) ||
            version.get() == nullptr) {
            return "unknown";
        }
        std::uint32_t major{};
        std::uint32_t minor{};
        if (FAILED(version->GetVersion(&major, &minor))) {
            return "unknown";
        }
        std::string result = std::to_string(major) + "." + std::to_string(minor);
        ComPtr<IDxcVersionInfo2> version2;
        if (SUCCEEDED(version->QueryInterface(__uuidof(IDxcVersionInfo2), version2.put_void())) &&
            version2.get() != nullptr) {
            std::uint32_t commit_count{};
            char* commit_hash{};
            if (SUCCEEDED(version2->GetCommitInfo(&commit_count, &commit_hash)) &&
                commit_hash != nullptr) {
                result += "." + std::to_string(commit_count) + " (" + commit_hash + ")";
            }
            ::CoTaskMemFree(commit_hash);
        }
        return result;
    } catch (const std::exception&) {
        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Call hierarchy and entry-point data-flow support.
//
// Everything below is derived exclusively from DXC's IDxcCursor cursor tree
// (GetChildren/GetReferencedCursor/GetArgumentAt/GetSemanticParent); there is
// no separate HLSL parser or textual heuristic anywhere in this section.
// ---------------------------------------------------------------------------

[[nodiscard]] ComPtr<IDxcCursor> adopt_addref(IDxcCursor* cursor) {
    ComPtr<IDxcCursor> result;
    if (cursor != nullptr) {
        cursor->AddRef();
        *result.put() = cursor;
    }
    return result;
}

// Pages through `cursor`'s children (DXC's GetChildren is paginated) and
// invokes `callback` once per non-null child, in order.
template <typename Callback> void for_each_child(IDxcCursor& cursor, Callback&& callback) {
    constexpr unsigned page_size = 256;
    for (unsigned skip = 0;; skip += page_size) {
        unsigned child_count{};
        IDxcCursor** raw_children{};
        check(cursor.GetChildren(skip, page_size, &child_count, &raw_children), "GetChildren");
        TaskCursors children{raw_children, child_count};
        for (unsigned index = 0; index < child_count; ++index) {
            if (children[index] != nullptr) {
                callback(*children[index]);
            }
        }
        if (child_count < page_size) {
            break;
        }
    }
}

// Recurses into container declarations the same way `cursor_symbols` does
// (structs/classes/namespaces/templates), collecting every callable cursor
// that is itself a *definition* (`IsDefinition() == true`); a declaration
// without a visible body cannot be traced, so those are skipped rather than
// reported as reachable/unreachable with no body to scan. `node_count` is
// threaded through recursive calls (rather than reset per-call) so the
// checkpoint below covers the whole tree walk, not just one container's
// direct children.
//
// Bounded by `max_definitions` independently of any reachability budget:
// once `out` reaches that size, `truncated` is set and no further
// definitions are collected or recursed into -- a translation unit with an
// enormous number of (mostly unreachable/dead) function definitions would
// otherwise make this collection pass itself, and the `unreachable_functions`
// output it feeds, unbounded (see `EntryPointDataFlowLimits::
// max_definitions_collected`'s comment). `truncated` is checked at the top
// of the recursive call and the callback both, so no further work of any
// kind (not even walking sibling nodes at the *current* level, which are
// still cheap paginated `GetChildren` calls but skip all callable/kind
// checks) proceeds once the budget is hit.
void collect_callable_definitions(IDxcCursor& cursor, std::vector<ComPtr<IDxcCursor>>& out,
                                  std::uint32_t depth,
                                  const std::function<void()>& cancellation_checkpoint,
                                  std::uint64_t& node_count, std::size_t max_definitions,
                                  bool& truncated) {
    if (depth >= 64 || truncated) {
        return;
    }
    for_each_child(cursor, [&](IDxcCursor& child) {
        if (truncated) {
            return;
        }
        if ((++node_count % 512) == 0 && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const auto kind = cursor_kind(child);
        if (callable_cursor(kind)) {
            BOOL is_definition{};
            check(child.IsDefinition(&is_definition), "IsDefinition");
            if (is_definition != FALSE) {
                if (out.size() >= max_definitions) {
                    truncated = true;
                    return;
                }
                out.push_back(adopt_addref(&child));
            }
            return;
        }
        if (symbol_container(kind)) {
            collect_callable_definitions(child, out, depth + 1, cancellation_checkpoint, node_count,
                                         max_definitions, truncated);
        }
    });
}

// True when `candidate` could plausibly be an HLSL shader entry point:
// a plain free-function definition (`DxcCursor_FunctionDecl`), never a
// method, constructor, conversion function, or (uninstantiated) function
// template, and declared at translation-unit or namespace scope rather
// than as a struct/class/union member. HLSL entry points are ordinary
// top-level functions; a struct method or a constructor that merely
// shares its spelling with the configured entry point name must never be
// silently selected instead. This is a conservative filter over compiler
// cursor kind/scope, not a textual guess.
[[nodiscard]] bool is_valid_entry_point_candidate(IDxcCursor& candidate) {
    if (cursor_kind(candidate) != DxcCursor_FunctionDecl) {
        return false;
    }
    ComPtr<IDxcCursor> parent;
    check(candidate.GetSemanticParent(parent.put()), "GetSemanticParent");
    if (is_null_cursor(parent.get())) {
        return true;
    }
    const auto parent_kind = cursor_kind(*parent.get());
    return !type_cursor(parent_kind);
}

// True when `referenced` is a variable declared at file/namespace scope
// (a "global" for data-flow purposes), as opposed to a local variable or
// parameter declared inside a function/method body. DXC exposes no direct
// "is this a local" flag, so this walks the semantic-parent chain: a
// variable declared inside any callable (function/method/constructor) is
// local, and everything else (including `static` file-scope variables and
// namespace-scope variables) is global.
[[nodiscard]] bool is_global_variable_cursor(IDxcCursor& referenced) {
    if (cursor_kind(referenced) != DxcCursor_VarDecl) {
        return false;
    }
    ComPtr<IDxcCursor> current;
    check(referenced.GetSemanticParent(current.put()), "GetSemanticParent");
    for (std::uint32_t depth = 0; !is_null_cursor(current.get()) && depth < 64; ++depth) {
        if (callable_cursor(cursor_kind(*current.get()))) {
            return false;
        }
        ComPtr<IDxcCursor> parent;
        check(current->GetSemanticParent(parent.put()), "GetSemanticParent");
        current = std::move(parent);
    }
    return true;
}

// True when `referenced` is a field of a cbuffer/tbuffer (HLSL's global
// constant-buffer containers, which the cursor tree represents as an
// anonymous/named UnexposedDecl whose formatted name starts with "cbuffer "
// or "tbuffer ", the same shape `layout_container_key` already recognizes
// for memory-layout reporting). A field of an ordinary struct is not itself
// treated as a global: if the struct *instance* is a global variable, the
// instance itself is reported as accessed via `is_global_variable_cursor`
// when its own DeclRefExpr is visited.
[[nodiscard]] bool is_cbuffer_or_tbuffer_field_cursor(IDxcCursor& referenced) {
    if (cursor_kind(referenced) != DxcCursor_FieldDecl) {
        return false;
    }
    ComPtr<IDxcCursor> parent;
    check(referenced.GetSemanticParent(parent.put()), "GetSemanticParent");
    if (is_null_cursor(parent.get())) {
        check(referenced.GetLexicalParent(parent.put()), "GetLexicalParent");
    }
    if (is_null_cursor(parent.get()) || cursor_kind(*parent.get()) != DxcCursor_UnexposedDecl) {
        return false;
    }
    const auto formatted = cursor_formatted_name(*parent.get());
    return formatted.starts_with("cbuffer ") || formatted.starts_with("tbuffer ");
}

// A small, deliberately conservative allow-list of HLSL resource-object
// methods DXC's cursor tree cannot otherwise prove are read-only (a method
// call's implicit object argument is always exposed "bare", the same shape
// as a mutating access -- see the comment on `BodyScanner::walk`'s CallExpr
// handling). Any method not on this list (Store, InterlockedAdd, Append,
// Consume, IncrementCounter/DecrementCounter, or any unrecognized/
// user-defined method) is classified read_write rather than guessed.
//
// This allowlist encodes semantics specific to the compiler *builtin*
// resource/sampler types; it must never be applied to a method call whose
// receiver has not first been proven (via `is_builtin_resource_type_spelling`
// against the receiver's own declared type) to actually be one of those
// builtin types, since a user-defined struct/class is free to declare its
// own method reusing one of these names with entirely different (and
// possibly mutating) semantics.
[[nodiscard]] bool is_read_only_resource_method(std::string_view name) {
    static const std::unordered_set<std::string_view> read_only_methods{
        "Sample",
        "SampleBias",
        "SampleGrad",
        "SampleLevel",
        "SampleCmp",
        "SampleCmpBias",
        "SampleCmpGrad",
        "SampleCmpLevelZero",
        "Load",
        "Load2DMS",
        "Gather",
        "GatherRed",
        "GatherGreen",
        "GatherBlue",
        "GatherAlpha",
        "GatherCmp",
        "GatherCmpRed",
        "GatherCmpGreen",
        "GatherCmpBlue",
        "GatherCmpAlpha",
        "CalculateLevelOfDetail",
        "CalculateLevelOfDetailUnclamped",
        "GetDimensions",
        "GetSamplePosition",
    };
    return read_only_methods.contains(name);
}

// Returns whether `spelling` (a cursor's `IDxcType::GetSpelling` result, as
// produced by `cursor_type`) names one of HLSL's compiler builtin
// resource/sampler object types -- exactly, never merely as a prefix, so a
// user-defined type that happens to start with one of these names (e.g.
// `Texture2DArrayOfThings`) is never mistaken for the real thing. Template
// arguments (e.g. the `<float4>` in `Texture2D<float4>`) and a leading
// qualifier DXC may include are stripped before comparison.
[[nodiscard]] bool is_builtin_resource_type_spelling(std::string_view spelling) {
    auto name = spelling;
    if (const auto angle = name.find('<'); angle != std::string_view::npos) {
        name = name.substr(0, angle);
    }
    while (!name.empty() && name.back() == ' ') {
        name.remove_suffix(1);
    }
    if (const auto space = name.rfind(' '); space != std::string_view::npos) {
        name = name.substr(space + 1);
    }
    static const std::unordered_set<std::string_view> builtin_resource_types{
        "Texture1D",
        "Texture1DArray",
        "Texture2D",
        "Texture2DArray",
        "Texture2DMS",
        "Texture2DMSArray",
        "Texture3D",
        "TextureCube",
        "TextureCubeArray",
        "RWTexture1D",
        "RWTexture1DArray",
        "RWTexture2D",
        "RWTexture2DArray",
        "RWTexture3D",
        "Buffer",
        "RWBuffer",
        "StructuredBuffer",
        "RWStructuredBuffer",
        "AppendStructuredBuffer",
        "ConsumeStructuredBuffer",
        "ByteAddressBuffer",
        "RWByteAddressBuffer",
        "ConstantBuffer",
        "SamplerState",
        "SamplerComparisonState",
        "RaytracingAccelerationStructure",
        "FeedbackTexture2D",
        "FeedbackTexture2DArray",
        "RasterizerOrderedTexture1D",
        "RasterizerOrderedTexture1DArray",
        "RasterizerOrderedTexture2D",
        "RasterizerOrderedTexture2DArray",
        "RasterizerOrderedTexture3D",
        "RasterizerOrderedBuffer",
        "RasterizerOrderedByteAddressBuffer",
        "RasterizerOrderedStructuredBuffer",
    };
    return builtin_resource_types.contains(name);
}

// Peels at most a few layers of implicit-load (UnexposedExpr) and
// member-access (MemberRefExpr) wrapping from `cursor` to reach an
// underlying DeclRefExpr, then resolves it. Used to locate a method or
// operator call's implicit object argument (e.g. the resource `buf` in
// `buf.Sample(...)` or `buf[i]`), which empirically appears at an
// unpredictable position among a CallExpr's children rather than a fixed
// index, instead of guessing that position.
[[nodiscard]] ComPtr<IDxcCursor> resolve_global_operand(ComPtr<IDxcCursor> cursor) {
    for (std::uint32_t guard = 0; !is_null_cursor(cursor.get()) && guard < 8; ++guard) {
        const auto kind = cursor_kind(*cursor.get());
        if (kind != DxcCursor_UnexposedExpr && kind != DxcCursor_MemberRefExpr) {
            break;
        }
        unsigned child_count{};
        IDxcCursor** raw_children{};
        check(cursor->GetChildren(0, 1, &child_count, &raw_children), "GetChildren");
        TaskCursors children{raw_children, child_count};
        if (child_count == 0 || children[0] == nullptr) {
            break;
        }
        cursor = adopt_addref(children[0]);
    }
    if (is_null_cursor(cursor.get()) || cursor_kind(*cursor.get()) != DxcCursor_DeclRefExpr) {
        return {};
    }
    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    return referenced;
}

// Searches `call_expr`'s immediate children (not recursing into nested call
// expressions, so a resource reference belonging to a *different* nested
// call, e.g. `a.Store(idx, b.Load(idx2))`, is never misattributed to `a`)
// for the implicit receiver -- what a method/operator call's implicit
// object argument always is in HLSL, since every resource is declared at
// global scope. `explicit_arguments` (the call's genuine, caller-supplied
// arguments -- for an overloaded operator call on a member this is
// `GetArgumentAt`'s results *excluding* index 0, which DXC/Clang models as
// the implicit receiver itself, not a real argument; for an ordinary
// (non-operator) method call it is all of `GetArgumentAt`'s results, none
// of which include the receiver) are explicitly excluded from this search:
// without that exclusion, a global passed *as an argument* to a method
// called on a non-global receiver (e.g. a resource function parameter)
// could otherwise be mistaken for the receiver itself, silently
// suppressing that argument's own independent access.
[[nodiscard]] ComPtr<IDxcCursor>
find_resource_base(IDxcCursor& call_expr, std::span<const ComPtr<IDxcCursor>> explicit_arguments) {
    ComPtr<IDxcCursor> found;
    for_each_child(call_expr, [&](IDxcCursor& child) {
        if (found.get() != nullptr) {
            return;
        }
        const auto is_argument = std::ranges::any_of(explicit_arguments, [&](const auto& argument) {
            if (argument.get() == nullptr) {
                return false;
            }
            BOOL equal{};
            check(argument->IsEqualTo(&child, &equal), "IsEqualTo");
            return equal != FALSE;
        });
        if (is_argument) {
            return;
        }
        auto referenced = resolve_global_operand(adopt_addref(&child));
        if (!is_null_cursor(referenced.get()) && is_global_variable_cursor(*referenced.get())) {
            found = std::move(referenced);
        }
    });
    return found;
}

[[nodiscard]] GlobalAccess make_global_access(IDxcCursor& referenced, GlobalAccessKind kind) {
    ComPtr<IDxcSourceLocation> location;
    check(referenced.GetLocation(location.put()), "GetLocation");
    ComPtr<IDxcSourceRange> extent;
    check(referenced.GetExtent(extent.put()), "GetExtent");
    unsigned start{};
    unsigned end{};
    check(extent->GetOffsets(&start, &end), "GetOffsets");
    return GlobalAccess{
        .name = cursor_spelling(referenced),
        .qualified_name = cursor_qualified_symbol_name(referenced),
        .cursor_kind = static_cast<std::uint32_t>(cursor_kind(referenced)),
        .location = make_source_location(*location.get()),
        .start_offset = start,
        .end_offset = end,
        .access = kind,
    };
}

[[nodiscard]] CallableSymbol make_callable_symbol(IDxcCursor& cursor) {
    ComPtr<IDxcCursor> definition;
    check(cursor.GetDefinitionCursor(definition.put()), "GetDefinitionCursor");
    auto* target = is_null_cursor(definition.get()) ? &cursor : definition.get();
    BOOL is_definition{};
    check(target->IsDefinition(&is_definition), "IsDefinition");

    ComPtr<IDxcSourceLocation> location;
    check(target->GetLocation(location.put()), "GetLocation");
    ComPtr<IDxcSourceRange> extent;
    check(target->GetExtent(extent.put()), "GetExtent");
    unsigned start{};
    unsigned end{};
    check(extent->GetOffsets(&start, &end), "GetOffsets");

    return CallableSymbol{
        .name = cursor_spelling(*target),
        .qualified_name = cursor_qualified_symbol_name(*target),
        .signature = declaration_header(*target),
        .cursor_kind = static_cast<std::uint32_t>(cursor_kind(*target)),
        .location = make_source_location(*location.get()),
        .start_offset = start,
        .end_offset = end,
        .is_definition = is_definition != FALSE,
    };
}

[[nodiscard]] std::string callable_identity_key(const CallableSymbol& symbol) {
    return symbol.location.path + ':' + std::to_string(symbol.start_offset);
}

// Distinguishes, for one level of recursion into a CallExpr's children,
// whether the child being visited is one of the call's genuine arguments
// (found via DXC's own GetArgumentAt, which is compared against by cursor
// identity rather than child position -- see `BodyScanner::walk`) versus
// everything else (the callee designator and, for method/operator calls,
// the implicit object argument).
enum class BareContext : std::uint8_t { normal, call_argument };

// Classifies a global/resource reference that DXC's cursor tree left "bare"
// (i.e. NOT wrapped in an implicit-load UnexposedExpr -- see the file
// comment on the empirical basis for this rule). `parent_kind` is the
// cursor kind of the reference's immediate structural parent.
[[nodiscard]] GlobalAccessKind classify_bare_context(DxcCursorKind parent_kind,
                                                     BareContext context) {
    if (context == BareContext::call_argument) {
        // A bare (unwrapped) call argument binds to an `out`/`inout`
        // parameter (DXC only wraps a call argument in an implicit load
        // when it binds to a plain `in` parameter). Whether it is
        // write-only (`out`) or both read and written (`inout`) cannot be
        // told apart from the cursor tree alone, so this is conservatively
        // read_write rather than guessed.
        return GlobalAccessKind::read_write;
    }
    switch (parent_kind) {
    case DxcCursor_CompoundAssignOperator:
    case DxcCursor_UnaryOperator:
        // `x += ...` / `++x` / `--x` read the previous value and write the
        // new one.
        return GlobalAccessKind::read_write;
    case DxcCursor_BinaryOperator:
        // Every operand of a value-producing binary operator (arithmetic or
        // comparison) is wrapped in an implicit load by DXC; a bare operand
        // of a BinaryOperator can therefore only be the left-hand side of a
        // plain `=` assignment.
        return GlobalAccessKind::write;
    default:
        // No bare-producing context has been observed other than the ones
        // above and a plain `=` assignment's left-hand side; anything else
        // reaching here without a load wrapper is conservatively treated as
        // both read and written rather than assumed read-only.
        return GlobalAccessKind::read_write;
    }
}

// Walks a callable definition's own cursor subtree (parameters and body)
// once, recording:
//  - every direct call to another callable definition or declaration
//    (`outgoing_calls()`), keyed by the callee's own identity so repeated
//    calls to the same overload accumulate call sites rather than
//    duplicating entries; and
//  - every global variable, cbuffer/tbuffer field, and resource object
//    read or written, merged across every `scan()` call made on the same
//    BodyScanner instance (callers that want per-function isolation should
//    use a fresh instance; `TranslationUnit::entry_point_data_flow` shares
//    one instance across every reachable function precisely so the merged
//    accesses aggregate the way its documented contract promises).
//
// Read/write classification rests on one empirically verified DXC/Clang
// behavior: an expression node is wrapped in an implicit-load UnexposedExpr
// exactly when its value is loaded (read); a reference used purely as an
// lvalue (assignment left-hand side, compound-assign/increment/decrement
// operand, or an `out`/`inout` call argument) is left "bare" with no
// wrapper. This was confirmed against pinned DXC 1.9.2607.13 across plain
// assignment, compound assignment, comparison, unary increment, `in`/
// `inout` call arguments, resource subscript operators, and resource
// method calls (see intellisense_tests.cpp's call-hierarchy/data-flow test
// cases). Wherever the cursor tree does not disambiguate (an unresolvable
// bare context, or a named resource method not on the read-only
// allow-list), classification defaults to read_write rather than guessing
// narrower -- this is a hard requirement, not merely a fallback of
// convenience.
class BodyScanner final {
  public:
    BodyScanner(EntryPointDataFlowLimits limits, std::function<void()> cancellation_checkpoint)
        : limits_{limits}, cancellation_checkpoint_{std::move(cancellation_checkpoint)} {}

    void scan(IDxcCursor& callable_definition) {
        outgoing_calls_.clear();
        outgoing_index_.clear();
        walk(callable_definition, DxcCursor_UnexposedDecl, BareContext::normal, 0);
    }

    [[nodiscard]] std::vector<OutgoingCall> outgoing_calls() const {
        auto result = outgoing_calls_;
        for (auto& call : result) {
            std::ranges::sort(call.call_sites, {}, &Reference::start_offset);
        }
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            return std::tie(left.callee.location.path, left.callee.start_offset) <
                   std::tie(right.callee.location.path, right.callee.start_offset);
        });
        return result;
    }

    [[nodiscard]] std::vector<GlobalAccess> global_accesses() const {
        std::vector<GlobalAccess> result;
        result.reserve(accesses_by_key_.size());
        for (const auto& [key, access] : accesses_by_key_) {
            result.push_back(access);
        }
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            return std::tie(left.location.path, left.start_offset) <
                   std::tie(right.location.path, right.start_offset);
        });
        return result;
    }

    [[nodiscard]] bool access_truncated() const noexcept { return access_truncated_; }

  private:
    void checkpoint() {
        if ((++node_count_ % 512) == 0 && cancellation_checkpoint_) {
            cancellation_checkpoint_();
        }
    }

    // Records a read/write/read_write access to a global/resource
    // declaration. Once the retention budget (`max_global_accesses`) is
    // hit, *newly encountered* (unseen) declarations are rejected -- their
    // key is not present in `accesses_by_key_`, so admitting them would
    // grow the retained set past its budget. Declarations already retained
    // before the budget was hit, however, must keep being conservatively
    // merged for every later access: skipping that merge (as an earlier
    // version of this function did, via an unconditional early return once
    // truncated) could leave an already-retained global under-reported as
    // `read` even though a later write to it was actually observed, which
    // is exactly the kind of under-reporting this API's "never
    // under-report a real read/write" contract forbids.
    void record_access(IDxcCursor& referenced, GlobalAccessKind kind) {
        auto access = make_global_access(referenced, kind);
        const auto key = access.location.path + ':' + std::to_string(access.start_offset);
        auto it = accesses_by_key_.find(key);
        if (it == accesses_by_key_.end()) {
            if (access_truncated_ || accesses_by_key_.size() >= limits_.max_global_accesses) {
                access_truncated_ = true;
                return;
            }
            accesses_by_key_.emplace(key, std::move(access));
            return;
        }
        if (it->second.access != access.access) {
            it->second.access = GlobalAccessKind::read_write;
        }
    }

    // The same `path:start_offset` identity key `record_access` keys
    // `accesses_by_key_` with, computed directly from a referenced
    // declaration cursor -- used to recognize when a CallExpr's own
    // specialized resource-access classification (from its method-name
    // allow-list or its own wrap/bare state for an operator call) and the
    // generic DeclRefExpr/MemberRefExpr walk are about to (re-)classify the
    // exact same declaration, so the generic walk can defer to the already
    // more-precise specialized classification instead of overwriting it.
    [[nodiscard]] static std::string declaration_key(IDxcCursor& referenced) {
        ComPtr<IDxcSourceLocation> location;
        check(referenced.GetLocation(location.put()), "GetLocation");
        ComPtr<IDxcSourceRange> extent;
        check(referenced.GetExtent(extent.put()), "GetExtent");
        unsigned start{};
        unsigned end{};
        check(extent->GetOffsets(&start, &end), "GetOffsets");
        (void)end;
        return make_source_location(*location.get()).path + ':' + std::to_string(start);
    }

    void record_call(IDxcCursor& call_expr, IDxcCursor& callee) {
        if (!callable_cursor(cursor_kind(callee))) {
            return;
        }
        auto symbol = make_callable_symbol(callee);
        if (symbol.location.path.empty()) {
            // No navigable location (a compiler intrinsic or other
            // synthetic cursor) -- not reportable as an outgoing call
            // target.
            return;
        }
        ComPtr<IDxcSourceLocation> location;
        check(call_expr.GetLocation(location.put()), "GetLocation");
        ComPtr<IDxcSourceRange> extent;
        check(call_expr.GetExtent(extent.put()), "GetExtent");
        unsigned start{};
        unsigned end{};
        check(extent->GetOffsets(&start, &end), "GetOffsets");
        Reference call_site{.location = make_source_location(*location.get()),
                            .start_offset = start,
                            .end_offset = end};

        const auto key = callable_identity_key(symbol);
        const auto existing = outgoing_index_.find(key);
        if (existing == outgoing_index_.end()) {
            outgoing_index_.emplace(key, outgoing_calls_.size());
            outgoing_calls_.push_back(
                OutgoingCall{.callee = std::move(symbol), .call_sites = {std::move(call_site)}});
            return;
        }
        outgoing_calls_[existing->second].call_sites.push_back(std::move(call_site));
    }

    void walk(IDxcCursor& cursor, DxcCursorKind parent_kind, BareContext context,
              std::uint32_t depth, std::string_view suppress_key = {}) {
        if (depth > 4096) {
            return;
        }
        checkpoint();
        const auto kind = cursor_kind(cursor);

        if (kind == DxcCursor_DeclRefExpr || kind == DxcCursor_MemberRefExpr) {
            ComPtr<IDxcCursor> referenced;
            check(cursor.GetReferencedCursor(referenced.put()), "GetReferencedCursor");
            if (!is_null_cursor(referenced.get()) &&
                (is_global_variable_cursor(*referenced.get()) ||
                 is_cbuffer_or_tbuffer_field_cursor(*referenced.get()))) {
                // Skip a declaration whose access was already classified,
                // more precisely, by the enclosing CallExpr's own
                // specialized resource-access logic just below (a resource
                // reference reached through this generic recursion, e.g.
                // `tex` inside `tex.Sample(...)`'s MemberRefExpr subtree,
                // would otherwise be reclassified here using a strictly
                // less informative rule -- see the CallExpr branch).
                if (declaration_key(*referenced.get()) != suppress_key) {
                    const auto access_kind = parent_kind == DxcCursor_UnexposedExpr
                                                 ? GlobalAccessKind::read
                                                 : classify_bare_context(parent_kind, context);
                    record_access(*referenced.get(), access_kind);
                }
            }
        } else if (kind == DxcCursor_CallExpr) {
            ComPtr<IDxcCursor> callee;
            check(cursor.GetReferencedCursor(callee.put()), "GetReferencedCursor");
            int num_arguments{-1};
            check(cursor.GetNumArguments(&num_arguments), "GetNumArguments");
            std::vector<ComPtr<IDxcCursor>> arguments;
            arguments.reserve(static_cast<std::size_t>((std::max)(num_arguments, 0)));
            for (int index = 0; index < num_arguments; ++index) {
                ComPtr<IDxcCursor> argument;
                check(cursor.GetArgumentAt(index, argument.put()), "GetArgumentAt");
                arguments.push_back(std::move(argument));
            }

            std::string resource_suppress_key;
            if (!is_null_cursor(callee.get())) {
                record_call(cursor, *callee.get());
                if (cursor_kind(*callee.get()) == DxcCursor_CXXMethod) {
                    const auto callee_name = cursor_spelling(*callee.get());
                    const auto is_operator = callee_name.starts_with("operator");
                    // For an overloaded operator call on a member (e.g.
                    // `buf[i]`, modeled by DXC/Clang as a
                    // CXXOperatorCallExpr), `GetArgumentAt(0)` is the
                    // implicit receiver itself, not a genuine call
                    // argument -- unlike an ordinary (non-operator)
                    // CXXMethod call, whose `GetArgumentAt` results never
                    // include the receiver. Excluding that receiver
                    // "argument" from the implicit-receiver search below
                    // would make the search find nothing (the receiver is
                    // the *only* global among an operator call's
                    // arguments in the common case, e.g. `buf[id.x]` has
                    // no other global operand), silently falling through
                    // to a far less precise generic classification. Only
                    // arguments genuinely supplied by the caller --
                    // `arguments[1:]` for an operator call on a member,
                    // all of `arguments` for a non-operator method call --
                    // are excluded from the receiver search.
                    auto resource = find_resource_base(
                        cursor, std::span<const ComPtr<IDxcCursor>>{arguments}.subspan(
                                    is_operator && !arguments.empty() ? 1 : 0));
                    if (!is_null_cursor(resource.get())) {
                        // The read-only method-name allowlist encodes
                        // knowledge specific to the *compiler builtin*
                        // resource/sampler types (Texture2D::Sample,
                        // RWStructuredBuffer::Load, ...); a user-defined
                        // struct or class can declare its own method with
                        // the same name that mutates state, so the
                        // allowlist may only be applied once the
                        // receiver's own declared type is proven to be one
                        // of those builtin types. An unproven (i.e.
                        // user-defined) receiver type is always
                        // conservatively read_write, regardless of name.
                        const auto receiver_is_builtin_resource =
                            is_builtin_resource_type_spelling(cursor_type(*resource.get()));
                        const auto access_kind =
                            is_operator ? (parent_kind == DxcCursor_UnexposedExpr
                                               ? GlobalAccessKind::read
                                               : classify_bare_context(parent_kind, context))
                                        : (receiver_is_builtin_resource &&
                                                   is_read_only_resource_method(callee_name)
                                               ? GlobalAccessKind::read
                                               : GlobalAccessKind::read_write);
                        record_access(*resource.get(), access_kind);
                        resource_suppress_key = declaration_key(*resource.get());
                    }
                }
            }

            for_each_child(cursor, [&](IDxcCursor& child) {
                const auto is_argument = std::ranges::any_of(arguments, [&](const auto& argument) {
                    if (argument.get() == nullptr) {
                        return false;
                    }
                    BOOL equal{};
                    check(argument->IsEqualTo(&child, &equal), "IsEqualTo");
                    return equal != FALSE;
                });
                walk(child, kind, is_argument ? BareContext::call_argument : BareContext::normal,
                     depth + 1, resource_suppress_key);
            });
            return;
        }

        for_each_child(cursor, [&](IDxcCursor& child) {
            walk(child, kind, BareContext::normal, depth + 1, suppress_key);
        });
    }

    EntryPointDataFlowLimits limits_;
    std::function<void()> cancellation_checkpoint_;
    std::unordered_map<std::string, GlobalAccess> accesses_by_key_;
    bool access_truncated_{};
    std::unordered_map<std::string, std::size_t> outgoing_index_;
    std::vector<OutgoingCall> outgoing_calls_;
    std::uint64_t node_count_{};
};

// Resolves the callable declaration/definition cursor at `path`/`line`/
// `column` (a call site or the callable's own name), the same way
// `hover_at`/`definition_at` resolve a target cursor: the cursor found at
// that position if it is not itself a reference, otherwise whatever it
// refers to. Returns a null ComPtr when the position does not resolve to a
// function, method, constructor, or conversion function.
[[nodiscard]] ComPtr<IDxcCursor> resolve_callable_cursor(IDxcTranslationUnit& translation_unit,
                                                         std::string_view path, std::uint32_t line,
                                                         std::uint32_t column) {
    const std::string owned_path{path};
    ComPtr<IDxcFile> file;
    check(translation_unit.GetFile(owned_path.c_str(), file.put()), "GetFile");
    ComPtr<IDxcSourceLocation> location;
    check(translation_unit.GetLocation(file.get(), line, column, location.put()), "GetLocation");
    ComPtr<IDxcCursor> cursor;
    check(translation_unit.GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");
    if (is_null_cursor(cursor.get())) {
        return {};
    }
    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    auto* target = is_null_cursor(referenced.get()) ? cursor.get() : referenced.get();
    if (!callable_cursor(cursor_kind(*target))) {
        return {};
    }
    return adopt_addref(target);
}

// True when `declaration` has at least one reference anywhere in `sources`
// other than its own declaration/definition occurrence -- used to identify
// unused top-level declarations. FindReferencesInFile reports the
// declaration's own name as one of its "references", so that occurrence
// (matched by exact source location) is excluded rather than counted.
[[nodiscard]] bool has_any_reference(IDxcTranslationUnit& translation_unit, IDxcCursor& declaration,
                                     const std::vector<SourceFile>& sources,
                                     const SourceLocation& declaration_location,
                                     const std::function<void()>& cancellation_checkpoint) {
    constexpr unsigned page_size = 64;
    std::uint64_t page_count = 0;
    for (const auto& source : sources) {
        ComPtr<IDxcFile> file;
        check(translation_unit.GetFile(source.path.c_str(), file.put()), "GetFile");
        for (unsigned skip = 0;; skip += page_size) {
            if ((++page_count % 8) == 0 && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            unsigned count{};
            IDxcCursor** raw_references{};
            check(declaration.FindReferencesInFile(file.get(), skip, page_size, &count,
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
                if (make_source_location(*reference_location.get()) != declaration_location) {
                    return true;
                }
            }
            if (count < page_size) {
                break;
            }
        }
    }
    return false;
}

// Top-level (file/namespace-scope) function and global-variable
// declarations with zero references anywhere in the current unsaved
// snapshot -- a compiler-verifiable, entry-point-independent dead code
// signal. The configured entry point itself is always excluded (shader
// entry points are the analysis root, not something anything else in the
// shader is expected to call). Deliberately scoped to top-level
// declarations only, not struct/class members: a struct field's "use" is
// bound up with its containing type's use, which this conservative,
// well-defined signal does not attempt to model.
//
// Each candidate's reference scan is at least O(sources.size()); `limits`
// bounds the total number of declarations actually scanned so a
// translation unit with an enormous number of top-level declarations
// cannot make a single request perform unbounded work. Declarations beyond
// that budget are simply omitted from the result (a conservative
// under-report, never a false "unused" claim), and `truncated` is set so
// the caller can fold it into the overall response's truncation signal.
[[nodiscard]] std::vector<Symbol> unused_top_level_declarations(
    IDxcTranslationUnit& translation_unit, IDxcCursor& root, const std::vector<SourceFile>& sources,
    std::string_view entry_point_name, const EntryPointDataFlowLimits& limits,
    const std::function<void()>& cancellation_checkpoint, bool& truncated) {
    std::vector<Symbol> result;
    std::uint64_t node_count = 0;
    std::uint64_t reference_scans_performed = 0;
    for_each_child(root, [&](IDxcCursor& child) {
        if ((++node_count % 512) == 0 && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const auto kind = cursor_kind(child);
        const auto is_function = callable_cursor(kind);
        const auto is_global = kind == DxcCursor_VarDecl;
        if (!is_function && !is_global) {
            return;
        }
        if (is_function) {
            BOOL is_definition{};
            check(child.IsDefinition(&is_definition), "IsDefinition");
            if (is_definition == FALSE) {
                return;
            }
        }
        const auto name = cursor_spelling(child);
        if (name.empty() || name == entry_point_name) {
            return;
        }
        ComPtr<IDxcSourceLocation> location;
        check(child.GetLocation(location.put()), "GetLocation");
        if (location.get() == nullptr) {
            return;
        }
        if (reference_scans_performed >= limits.max_unused_declaration_candidates) {
            truncated = true;
            return;
        }
        ++reference_scans_performed;
        const auto declaration_location = make_source_location(*location.get());
        if (has_any_reference(translation_unit, child, sources, declaration_location,
                              cancellation_checkpoint)) {
            return;
        }
        ComPtr<IDxcSourceRange> extent;
        check(child.GetExtent(extent.put()), "GetExtent");
        unsigned start{};
        unsigned end{};
        check(extent->GetOffsets(&start, &end), "GetOffsets");
        result.push_back(Symbol{
            .name = name,
            .cursor_kind = static_cast<std::uint32_t>(kind),
            .location = declaration_location,
            .start_offset = start,
            .end_offset = end,
            .children = {},
        });
    });
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return std::tie(left.location.path, left.start_offset) <
               std::tie(right.location.path, right.start_offset);
    });
    return result;
}

} // namespace

RuntimeError::RuntimeError(const std::string& message) : std::runtime_error{message} {}

std::string_view runtime_library_name() noexcept {
#ifdef _WIN32
    return "dxcompiler.dll";
#else
    return "libdxcompiler.so";
#endif
}

std::string validate_runtime_directory(std::string_view directory) {
    if (directory.empty()) {
        throw RuntimeError{"A DXC runtime directory was not provided"};
    }

    std::error_code error;
    const auto resolved =
        std::filesystem::absolute(std::filesystem::path{std::string{directory}}, error)
            .lexically_normal();
    if (error) {
        throw RuntimeError{"Unable to resolve the DXC runtime directory '" +
                           std::string{directory} + "'"};
    }
    if (!std::filesystem::exists(resolved, error) || error) {
        throw RuntimeError{"The DXC runtime directory '" + resolved.string() + "' does not exist"};
    }
    if (!std::filesystem::is_directory(resolved, error) || error) {
        throw RuntimeError{"The DXC runtime path '" + resolved.string() + "' is not a directory"};
    }

    const auto library = resolved / std::filesystem::path{std::string{runtime_library_name()}};
    if (!std::filesystem::is_regular_file(library, error) || error) {
        throw RuntimeError{"The DXC runtime directory '" + resolved.string() +
                           "' does not contain the required '" +
                           std::string{runtime_library_name()} +
                           "' compiler library for this platform"};
    }
#ifdef _WIN32
    // The Windows DXC runtime depends on dxil.dll for validation support; the
    // bundled default and both editor clients require it, so validate it here to
    // keep the server and clients consistent about which runtimes are usable.
    const auto validator = resolved / std::filesystem::path{"dxil.dll"};
    if (!std::filesystem::is_regular_file(validator, error) || error) {
        throw RuntimeError{"The DXC runtime directory '" + resolved.string() +
                           "' does not contain the required 'dxil.dll' platform dependency"};
    }
#endif
    return library.string();
}

struct Intellisense::Impl final {
    explicit Impl(const RuntimeConfiguration& runtime)
        : configured_directory{runtime.directory}, module{runtime.directory},
          create_instance{module.get<DxcCreateInstanceProc>("DxcCreateInstance")},
          intellisense{create<IDxcIntelliSense>(create_instance, CLSID_DxcIntelliSense)},
          version{query_runtime_version(create_instance)} {
        check(intellisense->CreateIndex(index.put()), "CreateIndex");
    }

    [[nodiscard]] RuntimeInfo runtime_info() const {
        return {.directory = module.bundled() ? std::string{} : configured_directory,
                .library_path = module.library_path(),
                .version = version,
                .bundled = module.bundled()};
    }

    std::string configured_directory;
    Module module;
    DxcCreateInstanceProc create_instance;
    ComPtr<IDxcIntelliSense> intellisense;
    std::string version;
    ComPtr<IDxcIndex> index;
};

struct TranslationUnit::Impl final {
    std::shared_ptr<Intellisense::Impl> owner;
    std::string root_path;
    std::vector<SourceFile> sources;
    std::vector<std::string> arguments;
    // The unmodified effective compiler arguments (unlike `arguments`, this
    // keeps -spirv/-fspv-* since it is used for actual compilation, not the
    // IntelliSense index parse which cannot consume them).
    std::vector<std::string> full_arguments;
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
                  static_cast<DxcTranslationUnitFlags>(
                      DxcTranslationUnitFlags_UseCallerThread |
                      DxcTranslationUnitFlags_DetailedPreprocessingRecord),
                  translation_unit.put()),
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
            .location = safe_diagnostic_location(*diagnostic.get(), implementation_->root_path),
            .fix_its = safe_diagnostic_fix_its(*diagnostic.get())});
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
    DxcCursorKind target_kind{DxcCursor_UnexposedDecl};
    check(target->GetKind(&target_kind), "GetKind");
    if (target_kind == DxcCursor_MacroDefinition || target_kind == DxcCursor_MacroExpansion) {
        return {};
    }

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

auto TranslationUnit::memory_layout_at(std::string_view path, std::uint32_t line,
                                       std::uint32_t column) const -> std::optional<MemoryLayout> {
    // Step 1: Identify the type at cursor position using DXC IntelliSense cursor APIs.
    const auto ident = identifier_at(implementation_->sources, path, line, column);
    if (!ident.has_value()) {
        return std::nullopt;
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

    // Resolve to the referenced declaration.
    ComPtr<IDxcCursor> referenced;
    check(cursor->GetReferencedCursor(referenced.put()), "GetReferencedCursor");
    auto* target = is_null_cursor(referenced.get()) ? cursor.get() : referenced.get();
    const auto kind = cursor_kind(*target);

    detail::ProbeTarget probe_target;

    auto resolve_struct = [&](IDxcCursor* struct_cursor) -> bool {
        probe_target.type_name = cursor_qualified_symbol_name(*struct_cursor);
        return !probe_target.type_name.empty();
    };

    auto resolve_cbuffer = [&](IDxcCursor* cb_cursor, const std::string& field) -> bool {
        probe_target.cbuffer_name = cursor_spelling(*cb_cursor);
        probe_target.selected_field = field;
        // Walk children to find a scalar variable to reference in the probe.
        // Prefer scalar/vector types over structs since asuint() requires scalar.
        unsigned child_count{};
        IDxcCursor** raw_children{};
        if (SUCCEEDED(cb_cursor->GetChildren(0, 64, &child_count, &raw_children))) {
            TaskCursors children{raw_children, child_count};
            std::string first_var;
            for (unsigned i = 0; i < child_count; ++i) {
                if (children[i] == nullptr)
                    continue;
                const auto ck = cursor_kind(*children[i]);
                if (ck != DxcCursor_VarDecl && ck != DxcCursor_FieldDecl)
                    continue;
                auto var_name = cursor_spelling(*children[i]);
                if (var_name.empty())
                    continue;
                if (first_var.empty())
                    first_var = var_name;
                // Check if the type is scalar/vector (not a struct).
                auto var_type_str = cursor_type(*children[i]);
                if (!var_type_str.empty() && (var_type_str.find("float") != std::string::npos ||
                                              var_type_str.find("int") != std::string::npos ||
                                              var_type_str.find("uint") != std::string::npos ||
                                              var_type_str.find("half") != std::string::npos ||
                                              var_type_str.find("bool") != std::string::npos ||
                                              var_type_str.find("double") != std::string::npos)) {
                    probe_target.reference_var = var_name;
                    break;
                }
            }
            if (probe_target.reference_var.empty()) {
                probe_target.reference_var = first_var;
            }
        }
        return !probe_target.cbuffer_name.empty();
    };

    if (kind == DxcCursor_StructDecl) {
        // Cursor is on a struct declaration.
        if (!resolve_struct(target))
            return std::nullopt;
    } else if (kind == DxcCursor_FieldDecl) {
        // Cursor is on a field within a struct.
        probe_target.selected_field = cursor_spelling(*target);
        ComPtr<IDxcCursor> parent;
        check(target->GetSemanticParent(parent.put()), "GetSemanticParent");
        if (!is_null_cursor(parent.get())) {
            const auto parent_kind = cursor_kind(*parent.get());
            if (parent_kind == DxcCursor_StructDecl) {
                if (!resolve_struct(parent.get()))
                    return std::nullopt;
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    } else if (kind == DxcCursor_VarDecl) {
        // Could be a cbuffer variable or something else.
        probe_target.selected_field = cursor_spelling(*target);
        probe_target.reference_var = probe_target.selected_field;

        // Check lexical parent for cbuffer detection.
        ComPtr<IDxcCursor> lexical;
        check(target->GetLexicalParent(lexical.put()), "GetLexicalParent");

        // Also check semantic parent as fallback.
        ComPtr<IDxcCursor> semantic;
        check(target->GetSemanticParent(semantic.put()), "GetSemanticParent");

        // Try lexical parent first, then semantic parent.
        IDxcCursor* parent_candidates[] = {lexical.get(), semantic.get()};
        bool resolved = false;
        for (auto* parent : parent_candidates) {
            if (is_null_cursor(parent))
                continue;
            const auto pk = cursor_kind(*parent);
            if (pk == DxcCursor_UnexposedDecl) {
                auto formatted = cursor_formatted_name(*parent);
                if (formatted.starts_with("cbuffer ")) {
                    resolve_cbuffer(parent, probe_target.selected_field);
                    resolved = true;
                    break;
                }
            } else if (pk == DxcCursor_StructDecl) {
                probe_target.selected_field = cursor_spelling(*target);
                if (resolve_struct(parent)) {
                    resolved = true;
                    break;
                }
            }
        }

        if (!resolved) {
            return std::nullopt;
        }
    } else if (kind == DxcCursor_UnexposedDecl) {
        // Might be a cbuffer declaration itself.
        auto formatted = cursor_formatted_name(*target);
        if (formatted.starts_with("cbuffer ")) {
            if (!resolve_cbuffer(target, ""))
                return std::nullopt;
        } else {
            return std::nullopt;
        }
    } else {
        // Try walking up the cursor hierarchy to find a containing struct, field,
        // or cbuffer variable. Walk up to 8 levels of lexical parents.
        bool resolved = false;
        IDxcCursor* current = cursor.get();
        ComPtr<IDxcCursor> current_owner;
        for (unsigned depth = 0; depth < 8 && !resolved; ++depth) {
            ComPtr<IDxcCursor> parent;
            check(current->GetLexicalParent(parent.put()), "GetLexicalParent");
            if (is_null_cursor(parent.get()))
                break;
            const auto pk = cursor_kind(*parent.get());

            if (pk == DxcCursor_FieldDecl) {
                probe_target.selected_field = cursor_spelling(*parent.get());
                ComPtr<IDxcCursor> gp;
                check(parent->GetSemanticParent(gp.put()), "GetSemanticParent");
                if (!is_null_cursor(gp.get()) && cursor_kind(*gp.get()) == DxcCursor_StructDecl) {
                    resolved = resolve_struct(gp.get());
                }
                break;
            }
            if (pk == DxcCursor_StructDecl) {
                resolved = resolve_struct(parent.get());
                break;
            }
            if (pk == DxcCursor_VarDecl) {
                probe_target.selected_field = cursor_spelling(*parent.get());
                probe_target.reference_var = probe_target.selected_field;
                ComPtr<IDxcCursor> gp;
                check(parent->GetLexicalParent(gp.put()), "GetLexicalParent");
                if (!is_null_cursor(gp.get()) &&
                    cursor_kind(*gp.get()) == DxcCursor_UnexposedDecl) {
                    auto fmt = cursor_formatted_name(*gp.get());
                    if (fmt.starts_with("cbuffer ")) {
                        resolved = resolve_cbuffer(gp.get(), probe_target.selected_field);
                    }
                }
                break;
            }
            if (pk == DxcCursor_UnexposedDecl) {
                auto fmt = cursor_formatted_name(*parent.get());
                if (fmt.starts_with("cbuffer ")) {
                    resolved = resolve_cbuffer(parent.get(), "");
                    break;
                }
            }
            // Keep parent alive and continue walking up.
            current_owner = std::move(parent);
            current = current_owner.get();
        }

        if (!resolved) {
            return std::nullopt;
        }
    }

    // Step 2: Call the compiler-backed probe.
    return detail::memory_layout_from_probe(implementation_->owner->create_instance,
                                            implementation_->sources, implementation_->arguments,
                                            implementation_->root_path, probe_target);
}

auto TranslationUnit::compilation_info() const -> CompilationInfo {
    auto info = detail::compilation_info_from_compile(
        implementation_->owner->create_instance, implementation_->sources,
        implementation_->full_arguments, implementation_->root_path);
    // Correlates reflected resources to their declaration site using the
    // same IntelliSense parse index (and therefore the same current unsaved
    // snapshot) already used for hover/go-to-definition/document symbols,
    // rather than re-parsing or inferring anything from raw text.
    attach_resource_source_locations(info, symbols());
    return info;
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

auto TranslationUnit::inlay_hints(std::string_view path,
                                  const std::vector<SourceOffsetRange>& ranges,
                                  const std::vector<InlayCall>& calls,
                                  const InlayHintOptions& options, InlayHintWork* work,
                                  const std::function<void()>& cancellation_checkpoint) const
    -> std::vector<InlayHint> {
    constexpr std::uint32_t token_overlap = 1024;
    const std::string owned_path{path};
    const auto source = std::ranges::find(implementation_->sources, owned_path, &SourceFile::path);
    if (source == implementation_->sources.end() || ranges.empty()) {
        return {};
    }
    const auto checkpoint = [&] {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
    };
    const auto requested = [&](std::uint32_t offset) {
        return std::ranges::any_of(ranges, [offset](const auto& range) {
            return offset >= range.start && offset < range.end;
        });
    };

    std::vector<InlayHint> result;
    const auto add_hint = [&](std::uint32_t offset, std::string label, InlayHintCategory category) {
        if (requested(offset) && !label.empty()) {
            result.push_back({.offset = offset, .label = std::move(label), .category = category});
        }
    };

    ComPtr<IDxcFile> file;
    check(implementation_->translation_unit->GetFile(owned_path.c_str(), file.put()), "GetFile");

    if (options.parameters) {
        for (std::size_t call_index = 0; call_index < calls.size(); ++call_index) {
            if (call_index % 32 == 0) {
                checkpoint();
            }
            const auto& call = calls[call_index];
            ComPtr<IDxcSourceLocation> location;
            check(implementation_->translation_unit->GetLocation(file.get(), call.line, call.column,
                                                                 location.put()),
                  "GetLocation");
            ComPtr<IDxcCursor> cursor;
            check(implementation_->translation_unit->GetCursorForLocation(location.get(),
                                                                          cursor.put()),
                  "GetCursorForLocation");
            if (is_null_cursor(cursor.get()) || !expression_cursor(cursor_kind(*cursor.get()))) {
                continue;
            }

            const auto signatures = signatures_at(path, call.line, call.column);
            std::vector<const Signature*> viable;
            for (const auto& signature : signatures) {
                if (signature.parameters.size() >= call.argument_offsets.size()) {
                    viable.push_back(&signature);
                }
            }
            if (viable.size() != 1) {
                continue;
            }
            const auto& parameters = viable.front()->parameters;
            for (std::size_t index = 0;
                 index < call.argument_offsets.size() && index < parameters.size(); ++index) {
                const auto offset = call.argument_offsets[index];
                const auto& name = parameters[index].name;
                if (offset >= source->text.size() || name.empty()) {
                    continue;
                }
                auto argument_end = offset;
                while (argument_end < source->text.size() &&
                       is_identifier_character(source->text[argument_end])) {
                    ++argument_end;
                }
                if (std::string_view{source->text}.substr(offset, argument_end - offset) == name) {
                    continue;
                }
                add_hint(offset, name + ":", InlayHintCategory::parameter);
            }
        }
    }

    std::unordered_map<std::string, std::optional<MemoryLayout>> layout_cache;
    if (options.types || options.matrix_orientation || options.packed_offsets ||
        options.array_strides) {
        for (const auto& requested_range : ranges) {
            checkpoint();
            if (requested_range.start >= requested_range.end ||
                requested_range.start >= source->text.size()) {
                continue;
            }
            const auto query_start =
                requested_range.start > token_overlap ? requested_range.start - token_overlap : 0;
            const auto query_end = static_cast<std::uint32_t>(
                (std::min)(source->text.size(),
                           static_cast<std::size_t>(requested_range.end) + token_overlap));
            ComPtr<IDxcSourceLocation> start;
            ComPtr<IDxcSourceLocation> end;
            check(implementation_->translation_unit->GetLocationForOffset(file.get(), query_start,
                                                                          start.put()),
                  "GetLocationForOffset");
            check(implementation_->translation_unit->GetLocationForOffset(file.get(), query_end,
                                                                          end.put()),
                  "GetLocationForOffset");
            ComPtr<IDxcSourceRange> range;
            check(
                implementation_->owner->intellisense->GetRange(start.get(), end.get(), range.put()),
                "GetRange");

            IDxcToken** raw_tokens{};
            unsigned token_count{};
            check(
                implementation_->translation_unit->Tokenize(range.get(), &raw_tokens, &token_count),
                "Tokenize");
            TaskTokens tokens{raw_tokens, token_count};
            for (unsigned index = 0; index < token_count; ++index) {
                if (index % 256 == 0) {
                    checkpoint();
                }
                auto* token = tokens[index];
                DxcTokenKind token_kind{DxcTokenKind_Unknown};
                check(token->GetKind(&token_kind), "GetKind");
                if (token_kind != DxcTokenKind_Identifier) {
                    continue;
                }
                ComPtr<IDxcSourceRange> extent;
                check(token->GetExtent(extent.put()), "GetExtent");
                unsigned token_start{};
                unsigned token_end{};
                check(extent->GetOffsets(&token_start, &token_end), "GetOffsets");
                ComPtr<IDxcSourceLocation> location;
                check(token->GetLocation(location.put()), "GetLocation");
                ComPtr<IDxcCursor> cursor;
                check(implementation_->translation_unit->GetCursorForLocation(location.get(),
                                                                              cursor.put()),
                      "GetCursorForLocation");
                if (is_null_cursor(cursor.get())) {
                    continue;
                }
                const auto kind = cursor_kind(*cursor.get());

                if (options.types && kind == DxcCursor_VarDecl) {
                    auto before = static_cast<std::size_t>(token_start);
                    while (before > 0 && std::isspace(static_cast<unsigned char>(
                                             source->text[before - 1])) != 0) {
                        --before;
                    }
                    auto word_start = before;
                    while (word_start > 0 &&
                           is_identifier_character(source->text[word_start - 1])) {
                        --word_start;
                    }
                    if (std::string_view{source->text}.substr(word_start, before - word_start) ==
                        "auto") {
                        const auto type = inferred_cursor_type(*cursor.get());
                        if (!type.empty()) {
                            add_hint(token_end, ": " + type, InlayHintCategory::type);
                        }
                    }
                }

                if ((options.matrix_orientation || options.packed_offsets ||
                     options.array_strides) &&
                    (kind == DxcCursor_FieldDecl || kind == DxcCursor_VarDecl)) {
                    const auto key = layout_container_key(*cursor.get());
                    if (!key) {
                        continue;
                    }
                    auto [cached, inserted] = layout_cache.try_emplace(*key);
                    if (inserted) {
                        checkpoint();
                        if (work != nullptr) {
                            ++work->layout_probes;
                        }
                        const auto source_location = make_source_location(*location.get());
                        cached->second =
                            memory_layout_at(path, source_location.line, source_location.column);
                    }
                    const auto& layout = cached->second;
                    if (!layout || !layout->supported) {
                        continue;
                    }
                    const auto name = cursor_spelling(*cursor.get());
                    const auto member =
                        std::ranges::find(layout->members, name, &MemoryLayoutElement::name);
                    if (member == layout->members.end()) {
                        continue;
                    }
                    auto* matrix = &*member;
                    while (matrix->kind == MemoryLayoutElementKind::array &&
                           !matrix->members.empty()) {
                        matrix = &matrix->members.front();
                    }
                    if (options.matrix_orientation &&
                        matrix->kind == MemoryLayoutElementKind::matrix) {
                        add_hint(token_end, matrix->row_major ? " row-major" : " column-major",
                                 InlayHintCategory::matrix_orientation);
                    }
                    if (options.packed_offsets &&
                        layout->kind == MemoryLayoutKind::constant_buffer) {
                        add_hint(token_end, " offset " + std::to_string(member->offset),
                                 InlayHintCategory::packed_offset);
                    }
                    if (options.array_strides && member->array_stride != 0) {
                        add_hint(token_end, " stride " + std::to_string(member->array_stride),
                                 InlayHintCategory::array_stride);
                    }
                }
            }
        }
    }

    if (options.registers) {
        checkpoint();
        if (work != nullptr) {
            ++work->reflection_compilations;
        }
        const auto info = compilation_info();
        if (info.reflection && info.reflection->available) {
            const auto register_prefix = [](ResourceRegisterClass value) -> std::string_view {
                switch (value) {
                case ResourceRegisterClass::cbv:
                    return "b";
                case ResourceRegisterClass::srv:
                    return "t";
                case ResourceRegisterClass::uav:
                    return "u";
                case ResourceRegisterClass::sampler:
                    return "s";
                case ResourceRegisterClass::unknown:
                    return "";
                }
                return "";
            };
            for (const auto& resource : info.reflection->resources) {
                if (!resource.source_location ||
                    std::filesystem::path{resource.source_location->path}.lexically_normal() !=
                        std::filesystem::path{path}.lexically_normal()) {
                    continue;
                }
                const auto prefix = register_prefix(resource.register_class);
                if (prefix.empty()) {
                    continue;
                }
                auto label =
                    " register(" + std::string{prefix} + std::to_string(resource.bind_point);
                if (resource.space != 0) {
                    label += ", space" + std::to_string(resource.space);
                }
                label += ')';
                add_hint(resource.source_location->offset +
                             static_cast<std::uint32_t>(resource.name.size()),
                         std::move(label), InlayHintCategory::register_binding);
            }
        }
    }

    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return left.offset != right.offset ? left.offset < right.offset
                                           : static_cast<std::uint8_t>(left.category) <
                                                 static_cast<std::uint8_t>(right.category);
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const auto& left, const auto& right) {
                                 return left.offset == right.offset &&
                                        left.category == right.category &&
                                        left.label == right.label;
                             }),
                 result.end());
    checkpoint();
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

auto TranslationUnit::skipped_ranges() const -> std::vector<SourceRange> {
    if (!implementation_) {
        throw std::logic_error{"Translation unit is not initialized"};
    }

    std::vector<SourceRange> result;
    for (const auto& source : implementation_->sources) {
        ComPtr<IDxcFile> file;
        check(implementation_->translation_unit->GetFile(source.path.c_str(), file.put()),
              "GetFile");
        unsigned count{};
        IDxcSourceRange** ranges{};
        check(implementation_->translation_unit->GetSkippedRanges(file.get(), &count, &ranges),
              "GetSkippedRanges");
        TaskRanges owned_ranges{ranges, count};
        for (unsigned index = 0; index < count; ++index) {
            if (auto range = safe_source_range(owned_ranges[index])) {
                result.push_back(std::move(*range));
            }
        }
    }
    return result;
}

auto TranslationUnit::macro_definitions() const -> std::vector<MacroDefinition> {
    if (!implementation_) {
        throw std::logic_error{"Translation unit is not initialized"};
    }

    std::vector<MacroDefinition> result;
    for (const auto& symbol : symbols()) {
        if (symbol.cursor_kind != static_cast<std::uint32_t>(DxcCursor_MacroDefinition)) {
            continue;
        }
        const auto source =
            std::ranges::find(implementation_->sources, symbol.location.path, &SourceFile::path);
        if (source == implementation_->sources.end() || symbol.end_offset > source->text.size() ||
            symbol.start_offset > symbol.end_offset) {
            continue;
        }
        auto declaration = std::string_view{source->text}.substr(
            symbol.start_offset, symbol.end_offset - symbol.start_offset);
        const auto name_offset = declaration.find(symbol.name);
        if (name_offset == std::string_view::npos) {
            continue;
        }
        auto value = declaration.substr(name_offset + symbol.name.size());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.remove_suffix(1);
        }
        result.push_back(
            {.name = symbol.name, .value = std::string{value}, .location = symbol.location});
    }
    return result;
}

auto TranslationUnit::symbols() const -> std::vector<Symbol> {
    ComPtr<IDxcCursor> root;
    check(implementation_->translation_unit->GetCursor(root.put()), "GetCursor");
    return cursor_symbols(*root.get(), 0);
}

auto TranslationUnit::callable_at(std::string_view path, std::uint32_t line,
                                  std::uint32_t column) const -> std::optional<CallableSymbol> {
    auto cursor =
        resolve_callable_cursor(*implementation_->translation_unit.get(), path, line, column);
    if (is_null_cursor(cursor.get())) {
        return std::nullopt;
    }
    return make_callable_symbol(*cursor.get());
}

auto TranslationUnit::outgoing_calls(std::string_view path, std::uint32_t line,
                                     std::uint32_t column,
                                     const std::function<void()>& cancellation_checkpoint) const
    -> std::vector<OutgoingCall> {
    auto cursor =
        resolve_callable_cursor(*implementation_->translation_unit.get(), path, line, column);
    if (is_null_cursor(cursor.get())) {
        return {};
    }
    ComPtr<IDxcCursor> definition;
    check(cursor->GetDefinitionCursor(definition.put()), "GetDefinitionCursor");
    auto* body = is_null_cursor(definition.get()) ? cursor.get() : definition.get();
    BOOL is_definition{};
    check(body->IsDefinition(&is_definition), "IsDefinition");
    if (is_definition == FALSE) {
        // A declaration with no visible body in the current unsaved
        // snapshot: there is nothing to scan for calls.
        return {};
    }
    BodyScanner scanner{EntryPointDataFlowLimits{}, cancellation_checkpoint};
    scanner.scan(*body);
    return scanner.outgoing_calls();
}

auto TranslationUnit::incoming_calls(std::string_view path, std::uint32_t line,
                                     std::uint32_t column,
                                     const std::function<void()>& cancellation_checkpoint) const
    -> std::vector<IncomingCall> {
    auto target =
        resolve_callable_cursor(*implementation_->translation_unit.get(), path, line, column);
    if (is_null_cursor(target.get())) {
        return {};
    }

    std::vector<IncomingCall> result;
    std::unordered_map<std::string, std::size_t> caller_index;

    constexpr unsigned page_size = 256;
    for (const auto& candidate_source : implementation_->sources) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        ComPtr<IDxcFile> candidate_file;
        check(implementation_->translation_unit->GetFile(candidate_source.path.c_str(),
                                                         candidate_file.put()),
              "GetFile");
        for (unsigned skip = 0;; skip += page_size) {
            if (cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            unsigned count{};
            IDxcCursor** raw_references{};
            check(target->FindReferencesInFile(candidate_file.get(), skip, page_size, &count,
                                               &raw_references),
                  "FindReferencesInFile");
            TaskCursors references{raw_references, count};
            for (unsigned index = 0; index < count; ++index) {
                if (cancellation_checkpoint && (index % 64) == 0) {
                    cancellation_checkpoint();
                }
                auto* reference = references[index];
                if (reference == nullptr) {
                    continue;
                }
                // A call site is always the DeclRefExpr/MemberRefExpr
                // designator of a call expression; this also excludes the
                // callable's own declaration/definition occurrence, whose
                // cursor kind is the callable kind itself, not a reference
                // expression.
                const auto reference_kind = cursor_kind(*reference);
                if (reference_kind != DxcCursor_DeclRefExpr &&
                    reference_kind != DxcCursor_MemberRefExpr) {
                    continue;
                }

                ComPtr<IDxcCursor> parent;
                check(reference->GetSemanticParent(parent.put()), "GetSemanticParent");
                ComPtr<IDxcCursor> caller;
                for (std::uint32_t depth = 0; !is_null_cursor(parent.get()) && depth < 64;
                     ++depth) {
                    if (callable_cursor(cursor_kind(*parent.get()))) {
                        caller = std::move(parent);
                        break;
                    }
                    ComPtr<IDxcCursor> next;
                    check(parent->GetSemanticParent(next.put()), "GetSemanticParent");
                    parent = std::move(next);
                }
                if (is_null_cursor(caller.get())) {
                    // Not enclosed by any callable (e.g. a global
                    // initializer) -- not representable as a call.
                    continue;
                }

                auto caller_symbol = make_callable_symbol(*caller.get());
                ComPtr<IDxcSourceLocation> reference_location;
                check(reference->GetLocation(reference_location.put()), "GetLocation");
                ComPtr<IDxcSourceRange> reference_extent;
                check(reference->GetExtent(reference_extent.put()), "GetExtent");
                unsigned start{};
                unsigned end{};
                check(reference_extent->GetOffsets(&start, &end), "GetOffsets");
                Reference call_site{.location = make_source_location(*reference_location.get()),
                                    .start_offset = start,
                                    .end_offset = end};

                const auto key = callable_identity_key(caller_symbol);
                const auto existing = caller_index.find(key);
                if (existing == caller_index.end()) {
                    caller_index.emplace(key, result.size());
                    result.push_back(IncomingCall{.caller = std::move(caller_symbol),
                                                  .call_sites = {std::move(call_site)}});
                } else {
                    result[existing->second].call_sites.push_back(std::move(call_site));
                }
            }
            if (count < page_size) {
                break;
            }
        }
    }

    for (auto& call : result) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        std::ranges::sort(call.call_sites, {}, &Reference::start_offset);
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return std::tie(left.caller.location.path, left.caller.start_offset) <
               std::tie(right.caller.location.path, right.caller.start_offset);
    });
    return result;
}

auto TranslationUnit::entry_point_data_flow(
    const EntryPointDataFlowLimits& limits,
    const std::function<void()>& cancellation_checkpoint) const -> EntryPointDataFlow {
    // Derives the entry point exactly as compilation_info() does: parsed
    // from this translation unit's own effective compiler arguments' `-E`,
    // never accepted as a request parameter, so there is no second,
    // possibly inconsistent compiler configuration for the same document.
    std::string entry_point_name;
    const auto& full_arguments = implementation_->full_arguments;
    for (std::size_t index = 0; index < full_arguments.size(); ++index) {
        const auto& argument = full_arguments[index];
        if (argument == "-E" && index + 1 < full_arguments.size()) {
            entry_point_name = full_arguments[++index];
        } else if (argument.starts_with("-E") && argument.size() > 2) {
            entry_point_name = argument.substr(2);
        }
    }

    EntryPointDataFlow result;
    if (entry_point_name.empty()) {
        result.explanation = "No entry point is configured for this document (set "
                             "hlsl.entryPoint or a variant entry point)";
        return result;
    }

    ComPtr<IDxcCursor> root;
    check(implementation_->translation_unit->GetCursor(root.put()), "GetCursor");
    std::vector<ComPtr<IDxcCursor>> all_definitions;
    std::uint64_t definition_collection_node_count = 0;
    bool definitions_truncated = false;
    collect_callable_definitions(*root.get(), all_definitions, 0, cancellation_checkpoint,
                                 definition_collection_node_count, limits.max_definitions_collected,
                                 definitions_truncated);
    // Assigned immediately after collection -- before the entry point is
    // resolved -- so that every subsequent return path (including the
    // "not found"/"ambiguous" early returns below, which never reach the
    // final `result.truncated` recomputation near the end of this
    // function) still reports an accurate truncation state. Without this,
    // a truncated definition set that happens to omit the configured
    // entry point (or omit a second, colliding definition that would have
    // made it ambiguous) would present as a definitive, complete "not
    // found"/"ambiguous" result instead of an incomplete one.
    result.definitions_truncated = definitions_truncated;
    result.truncated = definitions_truncated;

    // Restricted to plain top-level function definitions (see
    // `is_valid_entry_point_candidate`): a struct method, constructor, or
    // conversion function that merely shares its spelling with the
    // configured entry point name is never a valid HLSL entry point and
    // must not be selected by traversal order. If more than one top-level
    // function definition shares the name (illegal overloading of an entry
    // point name), the ambiguity is reported rather than silently resolved
    // to whichever definition happened to be visited first.
    std::vector<ComPtr<IDxcCursor>> entry_point_candidates;
    for (auto& candidate : all_definitions) {
        if (cursor_spelling(*candidate.get()) != entry_point_name) {
            continue;
        }
        if (!is_valid_entry_point_candidate(*candidate.get())) {
            continue;
        }
        // Adopt an additional reference rather than moving `candidate` out:
        // `all_definitions` is walked again below to compute
        // `unreachable_functions`, and must still contain every definition
        // (including the entry point itself, which is always trivially
        // reachable) with a valid cursor.
        entry_point_candidates.push_back(adopt_addref(candidate.get()));
    }
    if (entry_point_candidates.empty()) {
        result.explanation = "The configured entry point '" + entry_point_name +
                             "' does not resolve to a top-level function definition in this "
                             "document (methods, constructors, and conversion functions are not "
                             "valid HLSL entry points)";
        if (definitions_truncated) {
            result.explanation +=
                "; definition collection was truncated at the configured limit before "
                "every top-level definition in this document was examined, so this result "
                "may be incomplete rather than a definitive not-found";
        }
        return result;
    }
    if (entry_point_candidates.size() > 1) {
        result.explanation = "The configured entry point '" + entry_point_name +
                             "' is ambiguous: " + std::to_string(entry_point_candidates.size()) +
                             " top-level function definitions share this name in this document";
        if (definitions_truncated) {
            result.explanation +=
                "; definition collection was truncated at the configured limit before "
                "every top-level definition in this document was examined, so additional "
                "colliding definitions beyond the ones counted here may exist";
        }
        return result;
    }
    ComPtr<IDxcCursor> entry_cursor = std::move(entry_point_candidates.front());

    result.found = true;
    auto entry_symbol = make_callable_symbol(*entry_cursor.get());
    const auto entry_key = callable_identity_key(entry_symbol);
    result.entry_point = entry_symbol;

    BodyScanner scanner{limits, cancellation_checkpoint};

    std::vector<std::string> bfs_order;
    std::unordered_map<std::string, ReachableFunction> reachable_by_key;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;

    struct QueueItem final {
        ComPtr<IDxcCursor> cursor;
        std::string key;
    };
    std::vector<QueueItem> queue;

    reachable_by_key.emplace(entry_key, ReachableFunction{.function = entry_symbol, .depth = 0});
    bfs_order.push_back(entry_key);
    queue.push_back(QueueItem{.cursor = std::move(entry_cursor), .key = entry_key});

    bool functions_visited_truncated = false;
    for (std::size_t head = 0; head < queue.size(); ++head) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        auto current_cursor = std::move(queue[head].cursor);
        auto current_key = queue[head].key;
        const auto current_depth = reachable_by_key.at(current_key).depth;

        scanner.scan(*current_cursor.get());
        auto& successors = adjacency[current_key];
        for (const auto& call : scanner.outgoing_calls()) {
            const auto callee_key = callable_identity_key(call.callee);
            successors.push_back(callee_key);
            if (reachable_by_key.contains(callee_key)) {
                continue;
            }
            if (reachable_by_key.size() >= limits.max_functions_visited) {
                functions_visited_truncated = true;
                continue;
            }
            reachable_by_key.emplace(
                callee_key, ReachableFunction{.function = call.callee, .depth = current_depth + 1});
            bfs_order.push_back(callee_key);
            if (call.callee.is_definition) {
                auto callee_cursor = resolve_callable_cursor(
                    *implementation_->translation_unit.get(), call.callee.location.path,
                    call.callee.location.line, call.callee.location.column);
                if (!is_null_cursor(callee_cursor.get())) {
                    queue.push_back(
                        QueueItem{.cursor = std::move(callee_cursor), .key = callee_key});
                }
            }
        }
    }

    // Marks every callable that participates in a nontrivial call-graph
    // cycle reachable from the entry point (mutual recursion of any size,
    // or direct self-recursion) using Kosaraju's strongly-connected-
    // components algorithm over the already-bounded
    // `reachable_by_key`/`adjacency` graph built above.
    //
    // A naive "is this successor currently on the DFS path when visited"
    // back-edge check (the previous implementation) only detects cycles
    // that close back to a node still on the *current* DFS path; it misses
    // SCC membership reached through a node that was already fully
    // finalized on a different branch of the same DFS tree. For example,
    // with edges A->B, B->A, A->C, C->B: a DFS from A visits and fully
    // finishes B's subtree first (correctly marking the A<->B cycle)
    // before visiting C, so by the time C->B is examined, B is already
    // finalized and the edge is silently ignored -- even though C can
    // reach back to A (via B) and is therefore in the very same SCC as A
    // and B. Kosaraju's algorithm (a forward DFS pass computing a
    // finishing order, then a DFS pass over the transposed graph in
    // reverse-finishing order) recovers exact SCC membership regardless of
    // traversal/discovery order. Every node whose SCC has more than one
    // member, or is a single node with a direct self-loop, is marked
    // `recursive`. Every phase below is an explicit-stack iteration (never
    // native recursion) to stay stack-safe on arbitrarily long chains, and
    // each phase has its own cancellation checkpoint so total work stays
    // bounded and cancellable, not only the traversal above.
    std::unordered_set<std::string> recursive_keys;
    {
        struct Frame final {
            std::string key;
            std::size_t next_child_index{};
        };
        std::uint64_t work_units = 0;
        const auto checkpoint = [&] {
            if ((++work_units % 512) == 0 && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
        };

        // Pass 1: iterative DFS over the forward graph, recording a
        // finishing (post) order. `Frame::next_child_index` resumes each
        // frame exactly where it left off, playing the role a compiler-
        // generated stack frame's loop counter and return address would.
        std::vector<std::string> finishing_order;
        finishing_order.reserve(reachable_by_key.size());
        {
            std::unordered_set<std::string> visited;
            std::vector<Frame> stack;
            for (const auto& start_key : bfs_order) {
                if (visited.contains(start_key)) {
                    continue;
                }
                visited.insert(start_key);
                stack.push_back(Frame{.key = start_key});
                while (!stack.empty()) {
                    const auto it = adjacency.find(stack.back().key);
                    const auto* successors = it != adjacency.end() ? &it->second : nullptr;
                    bool descended = false;
                    while (successors != nullptr &&
                           stack.back().next_child_index < successors->size()) {
                        checkpoint();
                        const auto& next = (*successors)[stack.back().next_child_index];
                        ++stack.back().next_child_index;
                        if (!reachable_by_key.contains(next) || visited.contains(next)) {
                            continue;
                        }
                        visited.insert(next);
                        stack.push_back(Frame{.key = next});
                        descended = true;
                        break;
                    }
                    if (descended) {
                        continue;
                    }
                    finishing_order.push_back(stack.back().key);
                    stack.pop_back();
                }
            }
        }

        // Transpose graph: only over edges between two reachable nodes,
        // matching exactly what pass 1 above walked.
        std::unordered_map<std::string, std::vector<std::string>> transpose;
        for (const auto& [key, successors] : adjacency) {
            if (!reachable_by_key.contains(key)) {
                continue;
            }
            for (const auto& next : successors) {
                checkpoint();
                if (reachable_by_key.contains(next)) {
                    transpose[next].push_back(key);
                }
            }
        }

        // Pass 2: iterative DFS over the transposed graph in reverse
        // finishing order; each tree recovered this way is exactly one
        // strongly connected component (Kosaraju's key property).
        std::unordered_set<std::string> assigned;
        std::vector<std::string> component;
        for (auto order_it = finishing_order.rbegin(); order_it != finishing_order.rend();
             ++order_it) {
            if (assigned.contains(*order_it)) {
                continue;
            }
            component.clear();
            std::vector<Frame> stack;
            assigned.insert(*order_it);
            stack.push_back(Frame{.key = *order_it});
            while (!stack.empty()) {
                const auto transpose_it = transpose.find(stack.back().key);
                const auto* successors =
                    transpose_it != transpose.end() ? &transpose_it->second : nullptr;
                bool descended = false;
                while (successors != nullptr &&
                       stack.back().next_child_index < successors->size()) {
                    checkpoint();
                    const auto& next = (*successors)[stack.back().next_child_index];
                    ++stack.back().next_child_index;
                    if (assigned.contains(next)) {
                        continue;
                    }
                    assigned.insert(next);
                    stack.push_back(Frame{.key = next});
                    descended = true;
                    break;
                }
                if (descended) {
                    continue;
                }
                component.push_back(stack.back().key);
                stack.pop_back();
            }

            checkpoint();
            const bool has_self_loop = [&] {
                if (component.size() != 1) {
                    return false;
                }
                const auto adjacency_it = adjacency.find(component.front());
                if (adjacency_it == adjacency.end()) {
                    return false;
                }
                return std::ranges::find(adjacency_it->second, component.front()) !=
                       adjacency_it->second.end();
            }();
            if (component.size() > 1 || has_self_loop) {
                for (const auto& member : component) {
                    recursive_keys.insert(member);
                }
            }
        }
    }

    result.reachable_functions.reserve(reachable_by_key.size());
    {
        std::uint64_t conversion_count = 0;
        for (auto& [key, node] : reachable_by_key) {
            if ((++conversion_count % 512) == 0 && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            node.recursive = recursive_keys.contains(key);
            result.reachable_functions.push_back(std::move(node));
        }
    }
    std::ranges::sort(result.reachable_functions, [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        return std::tie(left.function.location.path, left.function.start_offset) <
               std::tie(right.function.location.path, right.function.start_offset);
    });

    // When the BFS above was truncated (hit `max_functions_visited` before
    // exhausting the call graph), `reachable_by_key` is only a partial,
    // possibly-incomplete view of the true reachable set: some definitions
    // outside it were never explored because traversal stopped, not
    // because they are provably unreachable from the entry point (they may
    // be reachable via edges from queued-but-unvisited callees). Reporting
    // "all_definitions minus reachable_by_key" as unreachable in that case
    // would falsely classify those downstream-but-unvisited functions as
    // dead code. Likewise, when `all_definitions` itself was truncated
    // (hit `max_definitions_collected` before every top-level definition
    // was collected), it is not the complete universe to subtract
    // `reachable_by_key` from -- a definition that was never collected
    // cannot be told apart from one that was collected and found
    // reachable. `unreachable_functions` is therefore left empty whenever
    // *either* of these two specific budgets was hit -- an explicit,
    // conservative "unknown" rather than an unproven claim -- and only
    // populated once both the reachability BFS and the definition
    // collection have run to completion. This is deliberately independent
    // of the other, unrelated truncation causes folded into
    // `result.truncated` below (the global-access retention limit and the
    // unused-declaration scan budget): neither of those affects whether
    // `reachable_by_key`/`all_definitions` are complete, so neither should
    // suppress a classification that is otherwise sound.
    if (!functions_visited_truncated && !definitions_truncated) {
        std::uint64_t unreachable_scan_count = 0;
        for (auto& definition : all_definitions) {
            if ((++unreachable_scan_count % 512) == 0 && cancellation_checkpoint) {
                cancellation_checkpoint();
            }
            auto symbol = make_callable_symbol(*definition.get());
            if (!reachable_by_key.contains(callable_identity_key(symbol))) {
                result.unreachable_functions.push_back(std::move(symbol));
            }
        }
        std::ranges::sort(result.unreachable_functions, [](const auto& left, const auto& right) {
            return std::tie(left.location.path, left.start_offset) <
                   std::tie(right.location.path, right.start_offset);
        });
    }

    result.global_accesses = scanner.global_accesses();
    bool unused_declarations_truncated = false;
    result.unused_declarations = unused_top_level_declarations(
        *implementation_->translation_unit.get(), *root.get(), implementation_->sources,
        entry_point_name, limits, cancellation_checkpoint, unused_declarations_truncated);
    result.functions_visited_truncated = functions_visited_truncated;
    result.definitions_truncated = definitions_truncated;
    result.global_accesses_truncated = scanner.access_truncated();
    result.unused_declarations_truncated = unused_declarations_truncated;
    result.truncated = result.functions_visited_truncated || result.definitions_truncated ||
                       result.global_accesses_truncated || result.unused_declarations_truncated;
    result.functions_visited = reachable_by_key.size();
    return result;
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

Intellisense::Intellisense() : implementation_{std::make_shared<Impl>(RuntimeConfiguration{})} {}

Intellisense::Intellisense(const RuntimeConfiguration& runtime)
    : implementation_{std::make_shared<Impl>(runtime)} {}

Intellisense::Intellisense(Intellisense&&) noexcept = default;
auto Intellisense::operator=(Intellisense&&) noexcept -> Intellisense& = default;
Intellisense::~Intellisense() = default;

auto Intellisense::runtime_info() const -> RuntimeInfo { return implementation_->runtime_info(); }

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

    implementation->full_arguments = options.arguments();
    implementation->arguments = implementation->full_arguments;
    // The IntelliSense index (IDxcIndex::ParseTranslationUnit) is observed to
    // fail outright when an explicit entry point argument is present --
    // whether separated ("-E" "entry") or joined ("-Eentry") -- regardless of
    // target profile or entry validity, so both spellings are stripped here
    // alongside the SPIR-V flags, "-Qstrip_reflect", and "-force-rootsig-ver"
    // (which the legacy parsing index does not recognize either).
    // `full_arguments` (used for the real compile in
    // TranslationUnit::compilation_info) keeps the original, unstripped
    // arguments.
    for (auto it = implementation->arguments.begin(); it != implementation->arguments.end();) {
        if ((*it == "-E" || *it == "-force-rootsig-ver") &&
            std::next(it) != implementation->arguments.end()) {
            it = implementation->arguments.erase(it, std::next(it, 2));
        } else {
            ++it;
        }
    }
    std::erase_if(implementation->arguments, [](std::string_view argument) {
        return argument == "-spirv" || argument == "-Qstrip_reflect" ||
               argument.starts_with("-fspv-") ||
               (argument.starts_with("-E") && argument.size() > 2);
    });
    implementation->parse_translation_unit();

    return TranslationUnit{std::move(implementation)};
}

} // namespace hlsl_intellisense::dxc
