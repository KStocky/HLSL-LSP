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
#include <iterator>
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

        result.push_back(
            Diagnostic{.severity = map_severity(severity),
                       .message = std::string{owned_spelling.view()},
                       .location = SourceLocation{.path = implementation_->root_path}});
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
    BOOL is_null{};
    check(definition->IsNull(&is_null), "IsNull");
    if (is_null != FALSE) {
        return std::nullopt;
    }

    char* spelling{};
    check(definition->GetSpelling(&spelling), "GetSpelling");
    TaskString owned_spelling{spelling};

    ComPtr<IDxcSourceLocation> definition_location;
    check(definition->GetLocation(definition_location.put()), "GetLocation");

    return Definition{.name = std::string{owned_spelling.view()},
                      .location = make_source_location(*definition_location.get())};
}

void TranslationUnit::reparse(std::vector<SourceFile> files) {
    implementation_->sources = std::move(files);
    implementation_->rebuild_unsaved_files();
    auto pointers = implementation_->unsaved_file_pointers();
    check(implementation_->translation_unit->Reparse(pointers.data(),
                                                     static_cast<unsigned>(pointers.size())),
          "Reparse");
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
    implementation->rebuild_unsaved_files();

    const auto arguments = options.arguments();
    std::vector<const char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    std::ranges::transform(arguments, std::back_inserter(argument_pointers),
                           [](const auto& argument) { return argument.c_str(); });
    auto unsaved_file_pointers = implementation->unsaved_file_pointers();

    check(implementation->owner->index->ParseTranslationUnit(
              implementation->root_path.c_str(), argument_pointers.data(),
              static_cast<int>(argument_pointers.size()), unsaved_file_pointers.data(),
              static_cast<unsigned>(unsaved_file_pointers.size()),
              DxcTranslationUnitFlags_UseCallerThread, implementation->translation_unit.put()),
          "ParseTranslationUnit");

    return TranslationUnit{std::move(implementation)};
}

} // namespace hlsl_intellisense::dxc
