#include <hlsl_intellisense/dxc/proof_of_concept.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <dxcisense.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace hlsl_intellisense::dxc {
namespace {

template <typename Interface> class ComPtr final {
  public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : pointer_{std::exchange(other.pointer_, nullptr)} {}

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    ~ComPtr() { reset(); }

    [[nodiscard]] Interface* get() const noexcept { return pointer_; }

    [[nodiscard]] Interface** put() noexcept {
        reset();
        return &pointer_;
    }

    [[nodiscard]] void** put_void() noexcept { return reinterpret_cast<void**>(put()); }

    [[nodiscard]] Interface* operator->() const noexcept { return pointer_; }

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
            throw std::runtime_error{"Unable to load the DXC runtime"};
        }
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    ~Module() {
#ifdef _WIN32
        ::FreeLibrary(handle_);
#else
        ::dlclose(handle_);
#endif
    }

    template <typename Function> [[nodiscard]] Function get(const char* name) const {
#ifdef _WIN32
        const auto address = ::GetProcAddress(handle_, name);
#else
        const auto address = ::dlsym(handle_, name);
#endif
        if (address == nullptr) {
            throw std::runtime_error{"Unable to find DxcCreateInstance"};
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
[[nodiscard]] ComPtr<Interface> create(DxcCreateInstanceProc create_instance, REFCLSID class_id) {
    ComPtr<Interface> result;
    check(create_instance(class_id, __uuidof(Interface), result.put_void()), "DxcCreateInstance");
    return result;
}

[[nodiscard]] ComPtr<IDxcUnsavedFile>
make_unsaved_file(IDxcIntelliSense& intellisense, const char* file_name, std::string_view source) {
    ComPtr<IDxcUnsavedFile> file;
    check(intellisense.CreateUnsavedFile(file_name, source.data(),
                                         static_cast<unsigned>(source.size()), file.put()),
          "CreateUnsavedFile");
    return file;
}

[[nodiscard]] bool completion_contains(IDxcTranslationUnit& translation_unit, const char* file_name,
                                       unsigned line, unsigned column,
                                       IDxcUnsavedFile* unsaved_file, std::string_view expected) {
    ComPtr<IDxcCodeCompleteResults> results;
    IDxcUnsavedFile* files[]{unsaved_file};
    check(translation_unit.CodeCompleteAt(file_name, line, column, files, 1,
                                          DxcCodeCompleteFlags_None, results.put()),
          "CodeCompleteAt");

    unsigned result_count{};
    check(results->GetNumResults(&result_count), "GetNumResults");

    for (unsigned result_index = 0; result_index < result_count; ++result_index) {
        ComPtr<IDxcCompletionResult> result;
        check(results->GetResultAt(result_index, result.put()), "GetResultAt");

        ComPtr<IDxcCompletionString> completion;
        check(result->GetCompletionString(completion.put()), "GetCompletionString");

        unsigned chunk_count{};
        check(completion->GetNumCompletionChunks(&chunk_count), "GetNumCompletionChunks");

        for (unsigned chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            DxcCompletionChunkKind kind{};
            check(completion->GetCompletionChunkKind(chunk_index, &kind), "GetCompletionChunkKind");
            if (kind != DxcCompletionChunk_TypedText) {
                continue;
            }

            char* text{};
            check(completion->GetCompletionChunkText(chunk_index, &text), "GetCompletionChunkText");
            const std::string_view completion_text{text != nullptr ? text : ""};
            const bool matches = completion_text == expected;
            ::CoTaskMemFree(text);
            if (matches) {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] bool definition_matches(IDxcTranslationUnit& translation_unit, const char* file_name,
                                      unsigned line, unsigned column, std::string_view expected) {
    ComPtr<IDxcFile> file;
    check(translation_unit.GetFile(file_name, file.put()), "GetFile");

    ComPtr<IDxcSourceLocation> location;
    check(translation_unit.GetLocation(file.get(), line, column, location.put()), "GetLocation");

    ComPtr<IDxcCursor> cursor;
    check(translation_unit.GetCursorForLocation(location.get(), cursor.put()),
          "GetCursorForLocation");

    ComPtr<IDxcCursor> definition;
    check(cursor->GetDefinitionCursor(definition.put()), "GetDefinitionCursor");

    char* spelling{};
    check(definition->GetSpelling(&spelling), "GetSpelling");
    const std::string_view actual{spelling != nullptr ? spelling : ""};
    const bool matches = actual == expected;
    ::CoTaskMemFree(spelling);
    return matches;
}

} // namespace

ProofOfConceptResult run_proof_of_concept() {
    constexpr char file_name[]{"prototype.hlsl"};
    constexpr std::string_view initial_source{"template<typename T>\n"
                                              "T combine(T left, T right) {\n"
                                              "    return left + right;\n"
                                              "}\n"
                                              "\n"
                                              "struct Number {\n"
                                              "    float value;\n"
                                              "    Number operator +(Number right) {\n"
                                              "        Number result = {value + right.value};\n"
                                              "        return result;\n"
                                              "    }\n"
                                              "};\n"
                                              "\n"
                                              "float4 main() : SV_Target {\n"
                                              "    Number left = {1.0};\n"
                                              "    Number right = {2.0};\n"
                                              "    Number sum = combine(left, right);\n"
                                              "    return sum.value.xxxx;\n"
                                              "}\n"
                                              "\n"};
    constexpr std::string_view updated_source{
        "template<typename T>\n"
        "T combineUpdated(T left, T right) {\n"
        "    return left + right;\n"
        "}\n"
        "\n"
        "struct UpdatedNumber {\n"
        "    float value;\n"
        "    UpdatedNumber operator +(UpdatedNumber right) {\n"
        "        UpdatedNumber result = {value + right.value};\n"
        "        return result;\n"
        "    }\n"
        "};\n"
        "\n"
        "float4 main() : SV_Target {\n"
        "    UpdatedNumber left = {1.0};\n"
        "    UpdatedNumber right = {2.0};\n"
        "    UpdatedNumber sum = combineUpdated(left, right);\n"
        "    return sum.value.xxxx;\n"
        "}\n"
        "\n"};
    constexpr const char* command_line[]{"-HV", "2021"};

    const Module dxcompiler;
    const auto create_instance = dxcompiler.get<DxcCreateInstanceProc>("DxcCreateInstance");
    auto intellisense = create<IDxcIntelliSense>(create_instance, CLSID_DxcIntelliSense);

    ComPtr<IDxcIndex> index;
    check(intellisense->CreateIndex(index.put()), "CreateIndex");

    auto initial_file = make_unsaved_file(*intellisense.get(), file_name, initial_source);
    IDxcUnsavedFile* initial_files[]{initial_file.get()};

    ComPtr<IDxcTranslationUnit> translation_unit;
    check(index->ParseTranslationUnit(file_name, command_line, 2, initial_files, 1,
                                      DxcTranslationUnitFlags_UseCallerThread,
                                      translation_unit.put()),
          "ParseTranslationUnit");

    ProofOfConceptResult result;
    result.parsed_hlsl_2021 = true;

    unsigned diagnostic_count{};
    check(translation_unit->GetNumDiagnostics(&diagnostic_count), "GetNumDiagnostics");
    result.produced_no_diagnostics = diagnostic_count == 0;
    result.completed_user_symbol = completion_contains(*translation_unit.get(), file_name, 20, 1,
                                                       initial_file.get(), "Number");
    result.resolved_template_definition =
        definition_matches(*translation_unit.get(), file_name, 17, 20, "combine");

    auto updated_file = make_unsaved_file(*intellisense.get(), file_name, updated_source);
    IDxcUnsavedFile* updated_files[]{updated_file.get()};
    check(translation_unit->Reparse(updated_files, 1), "Reparse");
    result.reparsed_updated_symbol = completion_contains(*translation_unit.get(), file_name, 20, 1,
                                                         updated_file.get(), "UpdatedNumber");

    return result;
}

} // namespace hlsl_intellisense::dxc
