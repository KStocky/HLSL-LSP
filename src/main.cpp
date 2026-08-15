#include <Windows.h>

#include <dxc/dxcisense.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;

class Module final {
public:
    explicit Module(const wchar_t* path)
        : handle_{::LoadLibraryW(path)}
    {
        if (handle_ == nullptr) {
            throw std::runtime_error{"Unable to load dxcompiler.dll"};
        }
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    ~Module()
    {
        ::FreeLibrary(handle_);
    }

    template <typename Function>
    [[nodiscard]] Function get(const char* name) const
    {
        const auto address = ::GetProcAddress(handle_, name);
        if (address == nullptr) {
            throw std::runtime_error{"Unable to find DxcCreateInstance"};
        }

        return reinterpret_cast<Function>(address);
    }

private:
    HMODULE handle_;
};

void check(HRESULT result, std::string_view operation)
{
    if (FAILED(result)) {
        throw std::runtime_error{
            std::string{operation} + " failed with HRESULT " +
            std::to_string(static_cast<unsigned long>(result))};
    }
}

template <typename Interface>
[[nodiscard]] ComPtr<Interface> create(
    DxcCreateInstanceProc create_instance,
    REFCLSID class_id)
{
    ComPtr<Interface> result;
    check(
        create_instance(class_id, __uuidof(Interface),
                        reinterpret_cast<void**>(result.GetAddressOf())),
        "DxcCreateInstance");
    return result;
}

[[nodiscard]] ComPtr<IDxcUnsavedFile> make_unsaved_file(
    IDxcIntelliSense& intellisense,
    const char* file_name,
    std::string_view source)
{
    ComPtr<IDxcUnsavedFile> file;
    check(
        intellisense.CreateUnsavedFile(
            file_name, source.data(), static_cast<unsigned>(source.size()),
            file.GetAddressOf()),
        "CreateUnsavedFile");
    return file;
}

[[nodiscard]] bool completion_contains(
    IDxcTranslationUnit& translation_unit,
    const char* file_name,
    unsigned line,
    unsigned column,
    IDxcUnsavedFile* unsaved_file,
    std::string_view expected)
{
    ComPtr<IDxcCodeCompleteResults> results;
    IDxcUnsavedFile* files[]{unsaved_file};
    check(
        translation_unit.CodeCompleteAt(
            file_name, line, column, files, 1, DxcCodeCompleteFlags_None,
            results.GetAddressOf()),
        "CodeCompleteAt");

    unsigned result_count{};
    check(results->GetNumResults(&result_count), "GetNumResults");

    for (unsigned result_index = 0; result_index < result_count;
         ++result_index) {
        ComPtr<IDxcCompletionResult> result;
        check(
            results->GetResultAt(result_index, result.GetAddressOf()),
            "GetResultAt");

        ComPtr<IDxcCompletionString> completion;
        check(
            result->GetCompletionString(completion.GetAddressOf()),
            "GetCompletionString");

        unsigned chunk_count{};
        check(
            completion->GetNumCompletionChunks(&chunk_count),
            "GetNumCompletionChunks");

        for (unsigned chunk_index = 0; chunk_index < chunk_count;
             ++chunk_index) {
            DxcCompletionChunkKind kind{};
            check(
                completion->GetCompletionChunkKind(chunk_index, &kind),
                "GetCompletionChunkKind");
            if (kind != DxcCompletionChunk_TypedText) {
                continue;
            }

            char* text{};
            check(
                completion->GetCompletionChunkText(chunk_index, &text),
                "GetCompletionChunkText");
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

void verify_definition(
    IDxcTranslationUnit& translation_unit,
    const char* file_name,
    unsigned line,
    unsigned column,
    std::string_view expected)
{
    ComPtr<IDxcFile> file;
    check(translation_unit.GetFile(file_name, file.GetAddressOf()), "GetFile");

    ComPtr<IDxcSourceLocation> location;
    check(
        translation_unit.GetLocation(
            file.Get(), line, column, location.GetAddressOf()),
        "GetLocation");

    ComPtr<IDxcCursor> cursor;
    check(
        translation_unit.GetCursorForLocation(
            location.Get(), cursor.GetAddressOf()),
        "GetCursorForLocation");

    ComPtr<IDxcCursor> definition;
    check(
        cursor->GetDefinitionCursor(definition.GetAddressOf()),
        "GetDefinitionCursor");

    char* spelling{};
    check(definition->GetSpelling(&spelling), "GetSpelling");
    const std::string_view actual{spelling != nullptr ? spelling : ""};
    const bool matches = actual == expected;
    ::CoTaskMemFree(spelling);

    if (!matches) {
        throw std::runtime_error{"Definition lookup returned an unexpected symbol"};
    }
}

} // namespace

int main()
{
    try {
        constexpr char file_name[]{"prototype.hlsl"};
        constexpr std::string_view initial_source{
            "template<typename T>\n"
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

        const Module dxcompiler{L"dxcompiler.dll"};
        const auto create_instance =
            dxcompiler.get<DxcCreateInstanceProc>("DxcCreateInstance");
        auto intellisense =
            create<IDxcIntelliSense>(create_instance, CLSID_DxcIntelliSense);

        ComPtr<IDxcIndex> index;
        check(intellisense->CreateIndex(index.GetAddressOf()), "CreateIndex");

        auto initial_file =
            make_unsaved_file(*intellisense.Get(), file_name, initial_source);
        IDxcUnsavedFile* initial_files[]{initial_file.Get()};

        ComPtr<IDxcTranslationUnit> translation_unit;
        check(
            index->ParseTranslationUnit(
                file_name, command_line, 2, initial_files, 1,
                DxcTranslationUnitFlags_UseCallerThread,
                translation_unit.GetAddressOf()),
            "ParseTranslationUnit");

        unsigned diagnostic_count{};
        check(
            translation_unit->GetNumDiagnostics(&diagnostic_count),
            "GetNumDiagnostics");
        if (diagnostic_count != 0) {
            throw std::runtime_error{"The valid shader produced diagnostics"};
        }

        if (!completion_contains(
                *translation_unit.Get(), file_name, 20, 1, initial_file.Get(),
                "Number")) {
            throw std::runtime_error{
                "Completion did not contain the HLSL 2021 user-defined type"};
        }

        verify_definition(
            *translation_unit.Get(), file_name, 17, 20, "combine");

        auto updated_file =
            make_unsaved_file(*intellisense.Get(), file_name, updated_source);
        IDxcUnsavedFile* updated_files[]{updated_file.Get()};
        check(
            translation_unit->Reparse(updated_files, 1),
            "Reparse");

        if (!completion_contains(
                *translation_unit.Get(), file_name, 20, 1, updated_file.Get(),
                "UpdatedNumber")) {
            throw std::runtime_error{"Reparsed completion was not updated"};
        }

        std::cout << "DXC IntelliSense proof of concept succeeded:\n"
                  << "  parsed an unsaved HLSL 2021 translation unit\n"
                  << "  accepted a function template and overloaded operator\n"
                  << "  produced zero diagnostics\n"
                  << "  completed a user-defined symbol\n"
                  << "  resolved a template function definition\n"
                  << "  reparsed and returned updated completion results\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
