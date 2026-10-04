#include "fxc.h"
#include "resource_binding_analysis.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <d3d11shader.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace hlsl_intellisense::dxc::detail {
namespace {

void error(std::vector<Diagnostic>& diagnostics, std::string message, std::string_view path) {
    Diagnostic diagnostic;
    diagnostic.source = "fxc";
    diagnostic.severity = DiagnosticSeverity::error;
    diagnostic.message = std::move(message);
    diagnostic.location.path = path;
    diagnostics.push_back(std::move(diagnostic));
}

CompilationInfo configuration(const CompilerOptions& options) {
    CompilationInfo result;
    result.compiler_backend = "fxc";
    result.language_version = "FXC legacy HLSL";
    result.entry_point = options.entry_point.empty() ? "main" : options.entry_point;
    result.target_profile = options.target_profile.empty() ? "ps_5_0" : options.target_profile;
    result.stage = result.target_profile.substr(0, result.target_profile.find('_'));
    result.defines = options.defines;
    result.include_directories = options.include_directories;
    result.compiler_arguments = options.additional_arguments;
    result.compiler_arguments.insert(result.compiler_arguments.begin(),
                                     {"/T", result.target_profile, "/E", result.entry_point});
    for (const auto& define : options.defines)
        result.compiler_arguments.insert(result.compiler_arguments.end(), {"/D", define});
    for (const auto& directory : options.include_directories)
        result.compiler_arguments.insert(result.compiler_arguments.end(), {"/I", directory});
    result.psv_wave_size.unavailable_reason = "FXC DXBC has no DXIL PSV/wave-size metadata.";
    return result;
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

std::wstring wide(std::string_view text) {
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        throw std::runtime_error("FXC path exceeds the Windows UTF-8 length limit.");
    if (text.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (!length)
        throw std::runtime_error("FXC path is not valid UTF-8.");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string utf8(std::wstring_view text) {
    const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

std::wstring canonical(std::string_view path) {
    auto input = wide(path);
    if (input.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("FXC paths must not contain NUL.");
    const auto length = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!length)
        throw std::runtime_error("FXC could not normalize a source path.");
    std::wstring result(length, L'\0');
    const auto written = GetFullPathNameW(input.c_str(), length, result.data(), nullptr);
    if (!written || written >= length)
        throw std::runtime_error("FXC could not normalize a source path.");
    result.resize(written);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, result.data(),
                       static_cast<int>(result.size()), result.data(),
                       static_cast<int>(result.size()), nullptr, nullptr, 0))
        throw std::runtime_error("FXC could not canonicalize source path casing.");
    return result;
}

std::string joined(std::string_view directory, std::string_view name) {
    return utf8((std::filesystem::path(wide(directory)) / wide(name)).native());
}

std::string parent(std::string_view path) {
    return utf8(std::filesystem::path(wide(path)).parent_path().native());
}

struct Runtime {
    HMODULE module{};
    decltype(&D3DCompile) compile{};
    decltype(&D3DCompile2) compile2{};
    decltype(&D3DPreprocess) preprocess{};
    decltype(&D3DReflect) reflect{};
    decltype(&D3DDisassemble) disassemble{};
    decltype(&D3DGetBlobPart) get_blob_part{};
    std::string path;

    explicit Runtime(const std::string& selected) {
        std::wstring library;
        if (selected.empty()) {
            std::wstring directory(32768, L'\0');
            const auto count =
                GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (!count || count >= directory.size())
                throw std::runtime_error("FXC cannot locate the Windows system directory.");
            directory.resize(count);
            library = directory + L"\\D3DCompiler_47.dll";
        } else {
            library = wide(selected);
            const std::filesystem::path file(library);
            const auto extension = file.extension().native();
            if (library.find(L'\0') != std::wstring::npos || !file.is_absolute() ||
                CompareStringOrdinal(extension.c_str(), -1, L".dll", -1, TRUE) != CSTR_EQUAL ||
                !std::filesystem::is_regular_file(file))
                throw std::runtime_error(
                    "FXC runtime must be an existing absolute .dll path; configure "
                    "fxcRuntimePath with a D3DCompiler_47.dll runtime.");
        }
        module = LoadLibraryExW(library.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            throw std::runtime_error("FXC cannot load " + utf8(library) + "; Windows error " +
                                     std::to_string(GetLastError()) +
                                     ". Check fxcRuntimePath and runtime architecture.");
        try {
            compile = load<decltype(compile)>("D3DCompile");
            // Some older runtimes expose D3DCompile but not D3DCompile2.
            compile2 = reinterpret_cast<decltype(compile2)>(GetProcAddress(module, "D3DCompile2"));
            preprocess = load<decltype(preprocess)>("D3DPreprocess");
            reflect = load<decltype(reflect)>("D3DReflect");
            disassemble = load<decltype(disassemble)>("D3DDisassemble");
            get_blob_part = load<decltype(get_blob_part)>("D3DGetBlobPart");
            std::wstring actual(32768, L'\0');
            const auto count =
                GetModuleFileNameW(module, actual.data(), static_cast<DWORD>(actual.size()));
            if (!count || count >= actual.size())
                throw std::runtime_error("FXC cannot obtain the loaded runtime path.");
            actual.resize(count);
            path = utf8(actual);
        } catch (...) {
            FreeLibrary(module);
            module = nullptr;
            throw;
        }
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() {
        if (module)
            FreeLibrary(module);
    }
    template <class T> T load(const char* name) {
        const auto address = GetProcAddress(module, name);
        if (!address)
            throw std::runtime_error(std::string("FXC runtime lacks export ") + name +
                                     "; select a compatible D3DCompiler_47.dll.");
        return reinterpret_cast<T>(address);
    }
};

class Includes final : public ID3DInclude {
    struct OpenSource {
        std::string path;
        std::vector<char> bytes;
    };
    std::map<std::wstring, const SourceFile*> sources_;
    std::map<const void*, std::unique_ptr<OpenSource>> open_;
    const CompilerOptions& options_;
    std::string main_;
    const void* main_data_;

  public:
    std::string failure;
    std::vector<std::string> resolved;
    Includes(const std::vector<SourceFile>& sources, const CompilerOptions& options,
             std::string_view main_path, const void* main_data)
        : options_(options), main_(main_path), main_data_(main_data) {
        for (const auto& source : sources) {
            const auto [it, inserted] = sources_.emplace(canonical(source.path), &source);
            if (!inserted && it->second->text != source.text)
                throw std::runtime_error("FXC has conflicting snapshots for " + source.path);
        }
    }

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent_data,
                                   LPCVOID* data, UINT* bytes) noexcept override {
        try {
            if (!name || !data || !bytes)
                return E_INVALIDARG;
            *data = nullptr;
            *bytes = 0;
            std::string parent_path = main_;
            if (parent_data && parent_data != main_data_) {
                const auto found = open_.find(parent_data);
                if (found == open_.end())
                    throw std::runtime_error("FXC include has unknown parent snapshot.");
                parent_path = found->second->path;
            }
            std::vector<std::string> candidates;
            if (std::filesystem::path(wide(name)).is_absolute())
                candidates.emplace_back(name);
            else {
                if (type == D3D_INCLUDE_LOCAL)
                    candidates.push_back(joined(parent(parent_path), name));
                for (const auto& directory : options_.include_directories)
                    candidates.push_back(joined(directory, name));
                candidates.push_back(joined(parent(main_), name));
                candidates.emplace_back(name);
            }
            for (const auto& candidate : candidates) {
                const auto found = sources_.find(canonical(candidate));
                if (found == sources_.end())
                    continue;
                const auto& source = *found->second;
                if (source.text.size() > (std::numeric_limits<UINT>::max)())
                    throw std::runtime_error("FXC include snapshot exceeds UINT byte limit.");
                auto allocation = std::make_unique<OpenSource>();
                allocation->path = source.path;
                allocation->bytes.assign(source.text.begin(), source.text.end());
                if (allocation->bytes.empty())
                    allocation->bytes.push_back('\0');
                const auto pointer = allocation->bytes.data();
                open_.emplace(pointer, std::move(allocation));
                *data = pointer;
                *bytes = static_cast<UINT>(source.text.size());
                if (std::ranges::find(resolved, source.path) == resolved.end())
                    resolved.push_back(source.path);
                return S_OK;
            }
            throw std::runtime_error(std::string("FXC include snapshot not supplied: ") + name +
                                     " (parent " + parent_path +
                                     "). Resolve the include into the source graph.");
        } catch (const std::exception& exception) {
            try {
                failure = exception.what();
            } catch (...) {
            }
            return E_FAIL;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) noexcept override {
        return open_.erase(data) ? S_OK : E_FAIL;
    }
};

const SourceFile& root(const std::vector<SourceFile>& sources, std::string_view path) {
    const auto key = canonical(path);
    for (const auto& source : sources)
        if (canonical(source.path) == key) {
            if (source.text.size() > (std::numeric_limits<UINT>::max)())
                throw std::runtime_error("FXC source snapshot exceeds UINT byte limit.");
            return source;
        }
    throw std::runtime_error("FXC main source snapshot not supplied: " + std::string(path));
}

struct Arguments {
    UINT flags = 0;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<D3D_SHADER_MACRO> macros;

    explicit Arguments(const CompilerOptions& options) {
        std::optional<std::string> matrix;
        std::optional<std::string> optimization;
        std::optional<std::string> flow;
        std::optional<std::string> strictness;
        for (auto argument : options.additional_arguments) {
            if (argument.empty() || (argument[0] != '/' && argument[0] != '-'))
                throw std::runtime_error("FXC unknown argument: " + argument);
            argument.erase(0, 1);
            auto exclusive = [&](std::optional<std::string>& group) {
                if (group && *group != argument)
                    throw std::runtime_error("FXC conflicting flags: " + *group + " / " + argument);
                group = argument;
            };
            if (argument == "WX")
                flags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
            else if (argument == "Zi")
                flags |= D3DCOMPILE_DEBUG;
            else if (argument == "Od" || argument == "O0" || argument == "O1" || argument == "O2" ||
                     argument == "O3") {
                exclusive(optimization);
                flags &= ~(D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_OPTIMIZATION_LEVEL2);
                if (argument == "Od")
                    flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
                else if (argument == "O0")
                    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
                else if (argument == "O2")
                    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
                else if (argument == "O3")
                    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
            } else if (argument == "Zpr" || argument == "Zpc") {
                exclusive(matrix);
                flags |= argument == "Zpr" ? D3DCOMPILE_PACK_MATRIX_ROW_MAJOR
                                           : D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
            } else if (argument == "Ges") {
                exclusive(strictness);
                flags |= D3DCOMPILE_ENABLE_STRICTNESS;
            } else if (argument == "Gec") {
                exclusive(strictness);
                flags &= ~D3DCOMPILE_ENABLE_STRICTNESS;
                flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
            } else if (argument == "Gis")
                flags |= D3DCOMPILE_IEEE_STRICTNESS;
            else if (argument == "Gfa" || argument == "Gfp") {
                exclusive(flow);
                flags |= argument == "Gfa" ? D3DCOMPILE_AVOID_FLOW_CONTROL
                                           : D3DCOMPILE_PREFER_FLOW_CONTROL;
            } else if (argument == "Vd")
                flags |= D3DCOMPILE_SKIP_VALIDATION;
            else if (argument == "enable_unbounded_descriptor_tables")
                flags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
            else if (argument == "res_may_alias")
                flags |= D3DCOMPILE_RESOURCES_MAY_ALIAS;
            else if (argument == "all_resources_bound")
                flags |= D3DCOMPILE_ALL_RESOURCES_BOUND;
            else if (argument.starts_with("T") || argument.starts_with("E") ||
                     argument.starts_with("D") || argument.starts_with("I"))
                throw std::runtime_error("FXC -T/-E/-D/-I overrides are not supported; "
                                         "use targetProfile, entryPoint, defines and "
                                         "includeDirectories structured fields.");
            else
                throw std::runtime_error("FXC unsupported/DXC-only argument: " + argument);
        }
        for (const auto& define : options.defines) {
            const auto equal = define.find('=');
            auto name = define.substr(0, equal);
            auto value = equal == std::string::npos ? "1" : define.substr(equal + 1);
            if (name.empty() || define.find('\0') != std::string::npos)
                throw std::runtime_error("FXC invalid macro definition.");
            values.emplace_back(std::move(name), std::move(value));
        }
        for (const auto& [name, value] : values)
            macros.push_back({name.c_str(), value.c_str()});
        macros.push_back({nullptr, nullptr});
    }
};

void validate_profile(std::string_view profile) {
    static const std::regex supported(
        R"(^(vs|ps|gs|hs|ds|cs)_(4_[01]|5_[01])$|^(vs|ps)_4_0_level_9_[13]$)");
    if (!std::regex_match(profile.begin(), profile.end(), supported))
        throw std::runtime_error("FXC backend supports only SM4/SM5 DXBC shader profiles "
                                 "(including SM4 level9), not SM2/SM3 DX9 bytecode, SM6 "
                                 "or library profiles: " +
                                 std::string(profile));
}

std::string blob_text(ID3DBlob* blob) {
    if (!blob)
        return {};
    std::string text(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!text.empty() && text.back() == '\0')
        text.pop_back();
    return text;
}

void normalize_location(Diagnostic& item, const std::vector<SourceFile>& sources,
                        const CompilerOptions& options, std::string_view main) {
    if (!item.location.line || !item.location.column)
        return;
    const auto byte_column = item.location.column;
    // Located FXC diagnostics count source bytes, not UTF-16 code units.
    // Unknown/#line-remapped locations cannot be converted from a snapshot.
    item.location.column = 0;
    std::vector<std::string> candidates{item.location.path};
    if (!std::filesystem::path(wide(item.location.path)).is_absolute()) {
        candidates.push_back(joined(parent(main), item.location.path));
        for (const auto& directory : options.include_directories)
            candidates.push_back(joined(directory, item.location.path));
    }
    for (const auto& candidate : candidates) {
        const auto key = canonical(candidate);
        for (const auto& source : sources) {
            if (canonical(source.path) != key)
                continue;
            const std::string_view text(source.text);
            std::size_t start = 0;
            for (std::uint32_t line = 1; line < item.location.line; ++line) {
                const auto newline = text.find('\n', start);
                if (newline == std::string_view::npos)
                    return;
                start = newline + 1;
            }
            const auto end = text.find_first_of("\r\n", start);
            const auto length = (end == std::string_view::npos ? text.size() : end) - start;
            const auto prefix_length = static_cast<std::size_t>(byte_column - 1);
            if (prefix_length > length)
                return;
            item.location.path = source.path;
            item.location.column =
                static_cast<std::uint32_t>(wide(text.substr(start, prefix_length)).size()) + 1;
            item.location.offset = static_cast<std::uint32_t>(start + prefix_length);
            return;
        }
    }
}

void diagnostics(ID3DBlob* blob, std::vector<Diagnostic>& output, std::string_view main,
                 const std::vector<SourceFile>& sources, const CompilerOptions& options) {
    static const std::regex located(
        R"(^(.*)\(([0-9]+),([0-9]+)(?:-[0-9]+)?\):\s*(warning|error|note)\s*(.*)$)");
    std::istringstream lines(blob_text(blob));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        Diagnostic item;
        item.source = "fxc";
        item.location.path = main;
        std::smatch match;
        if (std::regex_match(line, match, located)) {
            item.location.path = match[1].str();
            const auto line_number = match[2].str();
            const auto column = match[3].str();
            std::from_chars(line_number.data(), line_number.data() + line_number.size(),
                            item.location.line);
            std::from_chars(column.data(), column.data() + column.size(), item.location.column);
            item.severity = match[4] == "warning" ? DiagnosticSeverity::warning
                            : match[4] == "note"  ? DiagnosticSeverity::note
                                                  : DiagnosticSeverity::error;
            item.message = match[5].str();
            try {
                normalize_location(item, sources, options, main);
            } catch (const std::exception&) {
                // Invalid UTF-8 or an unresolvable path leaves column unavailable,
                // rather than exposing a byte count as a UTF-16 position.
                item.location.column = 0;
            }
        } else {
            item.severity = line.find("warning") != std::string::npos ? DiagnosticSeverity::warning
                                                                      : DiagnosticSeverity::error;
            item.message = line;
        }
        output.push_back(std::move(item));
    }
}

ComPtr<ID3DBlob> compile(Runtime& runtime, const std::vector<SourceFile>& sources,
                         const CompilerOptions& options, std::string_view main,
                         CompilationInfo& result) {
    validate_profile(result.target_profile);
    if (result.entry_point.find('\0') != std::string::npos)
        throw std::runtime_error("FXC entryPoint must not contain NUL.");
    Arguments arguments(options);
    const auto& source = root(sources, main);
    Includes includes(sources, options, main, source.text.data());
    ComPtr<ID3DBlob> object;
    ComPtr<ID3DBlob> messages;
    const auto status =
        runtime.compile2
            ? runtime.compile2(source.text.data(), source.text.size(), source.path.c_str(),
                               arguments.macros.data(), &includes, result.entry_point.c_str(),
                               result.target_profile.c_str(), arguments.flags, 0, 0, nullptr, 0,
                               object.GetAddressOf(), messages.GetAddressOf())
            : runtime.compile(source.text.data(), source.text.size(), source.path.c_str(),
                              arguments.macros.data(), &includes, result.entry_point.c_str(),
                              result.target_profile.c_str(), arguments.flags, 0,
                              object.GetAddressOf(), messages.GetAddressOf());
    diagnostics(messages.Get(), result.diagnostics, main, sources, options);
    if (!includes.failure.empty())
        error(result.diagnostics, includes.failure, main);
    result.resolved_include_paths = std::move(includes.resolved);
    result.success = SUCCEEDED(status) && object && object->GetBufferSize() != 0;
    if (!result.success && result.diagnostics.empty())
        error(result.diagnostics, "FXC compilation failed; HRESULT " + std::to_string(status),
              main);
    return object;
}

std::string system_value(D3D_NAME value) {
    switch (value) {
    case D3D_NAME_UNDEFINED:
        return "undefined";
    case D3D_NAME_POSITION:
        return "position";
    case D3D_NAME_CLIP_DISTANCE:
        return "clip_distance";
    case D3D_NAME_CULL_DISTANCE:
        return "cull_distance";
    case D3D_NAME_RENDER_TARGET_ARRAY_INDEX:
        return "render_target_array_index";
    case D3D_NAME_VIEWPORT_ARRAY_INDEX:
        return "viewport_array_index";
    case D3D_NAME_VERTEX_ID:
        return "vertex_id";
    case D3D_NAME_PRIMITIVE_ID:
        return "primitive_id";
    case D3D_NAME_INSTANCE_ID:
        return "instance_id";
    case D3D_NAME_IS_FRONT_FACE:
        return "is_front_face";
    case D3D_NAME_SAMPLE_INDEX:
        return "sample_index";
    case D3D_NAME_TARGET:
        return "target";
    case D3D_NAME_DEPTH:
        return "depth";
    case D3D_NAME_COVERAGE:
        return "coverage";
    case D3D_NAME_DEPTH_GREATER_EQUAL:
        return "depth_greater_equal";
    case D3D_NAME_DEPTH_LESS_EQUAL:
        return "depth_less_equal";
    case D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR:
        return "final_quad_edge_tessfactor";
    case D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR:
        return "final_quad_inside_tessfactor";
    case D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR:
        return "final_tri_edge_tessfactor";
    case D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR:
        return "final_tri_inside_tessfactor";
    case D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR:
        return "final_line_detail_tessfactor";
    case D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
        return "final_line_density_tessfactor";
    default:
        return "unknown";
    }
}

std::string resource_type(D3D_SHADER_INPUT_TYPE type) {
    switch (type) {
    case D3D_SIT_CBUFFER:
        return "cbuffer";
    case D3D_SIT_TBUFFER:
        return "tbuffer";
    case D3D_SIT_TEXTURE:
        return "texture";
    case D3D_SIT_SAMPLER:
        return "sampler";
    case D3D_SIT_UAV_RWTYPED:
        return "uav_rwtyped";
    case D3D_SIT_STRUCTURED:
        return "structured";
    case D3D_SIT_UAV_RWSTRUCTURED:
        return "uav_rwstructured";
    case D3D_SIT_BYTEADDRESS:
        return "byteaddress";
    case D3D_SIT_UAV_RWBYTEADDRESS:
        return "uav_rwbyteaddress";
    case D3D_SIT_UAV_APPEND_STRUCTURED:
        return "uav_append_structured";
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
        return "uav_consume_structured";
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        return "uav_rwstructured_with_counter";
    default:
        return "unknown";
    }
}

ResourceRegisterClass register_class(D3D_SHADER_INPUT_TYPE type) {
    switch (type) {
    case D3D_SIT_CBUFFER:
        return ResourceRegisterClass::cbv;
    case D3D_SIT_SAMPLER:
        return ResourceRegisterClass::sampler;
    case D3D_SIT_TBUFFER:
    case D3D_SIT_TEXTURE:
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
        return ResourceRegisterClass::srv;
    case D3D_SIT_UAV_RWTYPED:
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
    case D3D_SIT_UAV_APPEND_STRUCTURED:
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        return ResourceRegisterClass::uav;
    default:
        return ResourceRegisterClass::unknown;
    }
}

std::string dimension(D3D_SRV_DIMENSION value) {
    switch (value) {
    case D3D_SRV_DIMENSION_UNKNOWN:
        return "";
    case D3D_SRV_DIMENSION_BUFFER:
        return "buffer";
    case D3D_SRV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case D3D_SRV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case D3D_SRV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    case D3D_SRV_DIMENSION_TEXTURE3D:
        return "texture3d";
    case D3D_SRV_DIMENSION_TEXTURECUBE:
        return "texturecube";
    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
        return "texturecubearray";
    case D3D_SRV_DIMENSION_BUFFEREX:
        return "bufferex";
    default:
        return "unknown";
    }
}

std::string return_type(D3D_RESOURCE_RETURN_TYPE value) {
    switch (value) {
    case D3D_RETURN_TYPE_UNORM:
        return "unorm";
    case D3D_RETURN_TYPE_SNORM:
        return "snorm";
    case D3D_RETURN_TYPE_SINT:
        return "sint";
    case D3D_RETURN_TYPE_UINT:
        return "uint";
    case D3D_RETURN_TYPE_FLOAT:
        return "float";
    case D3D_RETURN_TYPE_MIXED:
        return "mixed";
    case D3D_RETURN_TYPE_DOUBLE:
        return "double";
    case D3D_RETURN_TYPE_CONTINUED:
        return "continued";
    default:
        return "unknown";
    }
}

// The two SDK reflection interfaces expose equivalent descriptor fields.
// Only ID3D12 exposes register spaces/range IDs for SM5.1.
template <class Interface, class Desc, class Binding, class Signature>
CompilationReflection reflected(Interface* shader, bool sm51, bool compute) {
    CompilationReflection result;
    result.unavailable_reason =
        "FXC does not expose interpolation modes or DXIL PSV/wave metadata. "
        "D3D11 profiles have implicit space 0 and no resource range IDs.";
    Desc desc{};
    if (FAILED(shader->GetDesc(&desc)))
        throw std::runtime_error("FXC shader reflection GetDesc failed.");
    CompilationStatistics stats;
    stats.instruction_count = desc.InstructionCount;
    stats.temp_register_count = desc.TempRegisterCount;
    stats.temp_array_count = desc.TempArrayCount;
    stats.define_count = desc.DefCount;
    stats.declaration_count = desc.DclCount;
    stats.texture_normal_instruction_count = desc.TextureNormalInstructions;
    stats.texture_load_instruction_count = desc.TextureLoadInstructions;
    stats.texture_comparison_instruction_count = desc.TextureCompInstructions;
    stats.texture_bias_instruction_count = desc.TextureBiasInstructions;
    stats.texture_gradient_instruction_count = desc.TextureGradientInstructions;
    stats.float_instruction_count = desc.FloatInstructionCount;
    stats.int_instruction_count = desc.IntInstructionCount;
    stats.uint_instruction_count = desc.UintInstructionCount;
    stats.static_flow_control_count = desc.StaticFlowControlCount;
    stats.dynamic_flow_control_count = desc.DynamicFlowControlCount;
    stats.macro_instruction_count = desc.MacroInstructionCount;
    stats.array_instruction_count = desc.ArrayInstructionCount;
    stats.cut_instruction_count = desc.CutInstructionCount;
    stats.emit_instruction_count = desc.EmitInstructionCount;
    stats.geometry_shader_max_output_vertex_count = desc.GSMaxOutputVertexCount;
    stats.geometry_shader_instance_count = desc.cGSInstanceCount;
    stats.control_point_count = desc.cControlPoints;
    stats.patch_constant_parameter_count = desc.PatchConstantParameters;
    stats.barrier_instruction_count = desc.cBarrierInstructions;
    stats.interlocked_instruction_count = desc.cInterlockedInstructions;
    stats.texture_store_instruction_count = desc.cTextureStoreInstructions;
    result.statistics = stats;
    result.barrier_instruction_count = desc.cBarrierInstructions;
    auto signatures = [&](UINT count, bool input) {
        for (UINT index = 0; index < count; ++index) {
            Signature signature{};
            const auto status = input ? shader->GetInputParameterDesc(index, &signature)
                                      : shader->GetOutputParameterDesc(index, &signature);
            if (FAILED(status))
                throw std::runtime_error("FXC signature reflection failed.");
            CompilationSignatureParameter parameter;
            parameter.semantic_name = signature.SemanticName ? signature.SemanticName : "";
            parameter.semantic_index = signature.SemanticIndex;
            parameter.register_index = signature.Register;
            parameter.system_value = system_value(signature.SystemValueType);
            parameter.component_type =
                signature.ComponentType == D3D_REGISTER_COMPONENT_UINT32    ? "uint32"
                : signature.ComponentType == D3D_REGISTER_COMPONENT_SINT32  ? "sint32"
                : signature.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? "float32"
                                                                            : "unknown";
            parameter.mask = signature.Mask;
            parameter.read_write_mask = signature.ReadWriteMask;
            parameter.stream = signature.Stream;
            parameter.interpolation_available = false;
            (input ? result.input_signature : result.output_signature)
                .push_back(std::move(parameter));
        }
    };
    signatures(desc.InputParameters, true);
    signatures(desc.OutputParameters, false);
    for (UINT index = 0; index < desc.BoundResources; ++index) {
        Binding binding{};
        if (FAILED(shader->GetResourceBindingDesc(index, &binding)))
            throw std::runtime_error("FXC resource reflection failed.");
        CompilationResourceBinding resource;
        resource.name = binding.Name ? binding.Name : "";
        resource.type = resource_type(binding.Type);
        resource.bind_point = binding.BindPoint;
        resource.bind_count = binding.BindCount;
        resource.range_id_available = false;
        if constexpr (requires {
                          binding.Space;
                          binding.uID;
                      }) {
            resource.space = binding.Space;
            resource.range_id = binding.uID;
            resource.range_id_available = true;
        } else if (sm51) {
            throw std::runtime_error("FXC SM5.1 reflection requires ID3D12ShaderReflection "
                                     "to expose register spaces.");
        }
        resource.dimension = dimension(binding.Dimension);
        resource.return_type = return_type(binding.ReturnType);
        resource.register_class = register_class(binding.Type);
        resource.raw_flags = binding.uFlags;
        resource.sample_count = binding.NumSamples;
        resource.unbounded = sm51 && binding.BindCount == 0;
        resource.system_reserved_space = is_system_reserved_space(resource.space);
        resource.usage = (binding.uFlags & D3D_SIF_UNUSED) ? ResourceUsageStatus::unused
                                                           : ResourceUsageStatus::used;
        result.resources.push_back(std::move(resource));
    }
    result.binding_analysis = analyze_resource_bindings(result.resources);
    if (compute) {
        CompilationThreadGroupSize group;
        if (shader->GetThreadGroupSize(&group.x, &group.y, &group.z))
            result.thread_group_size = group;
    }
    return result;
}

CompilationReflection reflection(Runtime& runtime, ID3DBlob* object, bool sm51, bool compute) {
    if (sm51) {
        ComPtr<ID3D12ShaderReflection> shader;
        if (FAILED(runtime.reflect(object->GetBufferPointer(), object->GetBufferSize(),
                                   __uuidof(ID3D12ShaderReflection),
                                   reinterpret_cast<void**>(shader.GetAddressOf()))))
            throw std::runtime_error("Selected FXC runtime cannot reflect SM5.1 register "
                                     "spaces through ID3D12ShaderReflection.");
        return reflected<ID3D12ShaderReflection, D3D12_SHADER_DESC, D3D12_SHADER_INPUT_BIND_DESC,
                         D3D12_SIGNATURE_PARAMETER_DESC>(shader.Get(), true, compute);
    }
    ComPtr<ID3D11ShaderReflection> shader;
    if (FAILED(runtime.reflect(object->GetBufferPointer(), object->GetBufferSize(),
                               __uuidof(ID3D11ShaderReflection),
                               reinterpret_cast<void**>(shader.GetAddressOf()))))
        throw std::runtime_error("Selected FXC runtime cannot reflect this DXBC object.");
    return reflected<ID3D11ShaderReflection, D3D11_SHADER_DESC, D3D11_SHADER_INPUT_BIND_DESC,
                     D3D11_SIGNATURE_PARAMETER_DESC>(shader.Get(), false, compute);
}

std::string scalar_name(D3D_SHADER_VARIABLE_TYPE type) {
    switch (type) {
    case D3D_SVT_BOOL:
        return "bool";
    case D3D_SVT_INT:
        return "int";
    case D3D_SVT_UINT:
        return "uint";
    case D3D_SVT_FLOAT:
        return "float";
    case D3D_SVT_DOUBLE:
        return "double";
    default:
        return "unknown";
    }
}

// Offsets and allocation spans come exclusively from the reflected descriptors.
// Scalar widths below are explicit legacy shader ABI fields, not a packing model.
template <class Type, class TypeDesc>
MemoryLayoutElement element(Type* type, std::string name, UINT offset, UINT span,
                            unsigned depth = 0) {
    if (!type || depth > 64)
        throw std::runtime_error("FXC reflected type is unavailable or too deeply nested.");
    TypeDesc desc{};
    if (FAILED(type->GetDesc(&desc)))
        throw std::runtime_error("FXC reflected type descriptor is unavailable.");
    MemoryLayoutElement result;
    result.name = std::move(name);
    result.offset = offset;
    result.allocation_size = span;
    result.type = desc.Name ? desc.Name : scalar_name(desc.Type);
    if (desc.Elements) {
        result.kind = MemoryLayoutElementKind::array;
        result.array_dimensions.push_back(desc.Elements);
        result.size = span;
    } else if (desc.Class == D3D_SVC_STRUCT) {
        result.kind = MemoryLayoutElementKind::record;
        result.size = span;
        for (UINT index = 0; index < desc.Members; ++index) {
            auto* member = type->GetMemberTypeByIndex(index);
            TypeDesc child{};
            if (!member || FAILED(member->GetDesc(&child)) || child.Offset > span)
                throw std::runtime_error("FXC invalid reflected member offset.");
            UINT end = span;
            if (index + 1 < desc.Members) {
                auto* next = type->GetMemberTypeByIndex(index + 1);
                TypeDesc next_desc{};
                if (!next || FAILED(next->GetDesc(&next_desc)))
                    throw std::runtime_error("FXC member reflection unavailable.");
                end = next_desc.Offset;
            }
            if (end < child.Offset || end > span ||
                offset > (std::numeric_limits<UINT>::max)() - child.Offset)
                throw std::runtime_error("FXC invalid reflected member span.");
            const auto* member_name = type->GetMemberTypeName(index);
            result.members.push_back(element<Type, TypeDesc>(member, member_name ? member_name : "",
                                                             offset + child.Offset,
                                                             end - child.Offset, depth + 1));
        }
    } else if (desc.Class == D3D_SVC_MATRIX_ROWS || desc.Class == D3D_SVC_MATRIX_COLUMNS) {
        result.kind = MemoryLayoutElementKind::matrix;
        result.row_major = desc.Class == D3D_SVC_MATRIX_ROWS;
        result.size = span;
    } else if (desc.Class == D3D_SVC_SCALAR || desc.Class == D3D_SVC_VECTOR) {
        result.kind = desc.Class == D3D_SVC_VECTOR ? MemoryLayoutElementKind::vector
                                                   : MemoryLayoutElementKind::scalar;
        const auto width = desc.Type == D3D_SVT_DOUBLE           ? 8U
                           : scalar_name(desc.Type) != "unknown" ? 4U
                                                                 : 0U;
        if (!width || desc.Columns > span / width)
            throw std::runtime_error("FXC scalar/vector ABI size is unavailable.");
        result.size = width * desc.Columns;
        if (desc.Class == D3D_SVC_VECTOR)
            result.type = scalar_name(desc.Type) + std::to_string(desc.Columns);
    } else {
        throw std::runtime_error("FXC does not expose a supported cbuffer type.");
    }
    return result;
}

template <class Interface, class BufferDesc, class VariableDesc, class TypeDesc>
MemoryLayout layout(Interface* shader, const ProbeTarget& target) {
    MemoryLayout result;
    result.name = target.cbuffer_name;
    result.type = target.cbuffer_name;
    result.kind = MemoryLayoutKind::constant_buffer;
    // Alignment and array/matrix strides are not reported by these APIs.
    // The partial result is intentionally not a complete supported layout.
    result.supported = false;
    result.explanation =
        "FXC exposes original cbuffer sizes, variable sizes, nested member offsets "
        "and reflected allocation spans, but not alignment or array/matrix strides. "
        "Zero alignment/stride fields are unavailable, not measured zero values; "
        "record/array/matrix sizes below are reflected spans, not payload sizes.";
    auto* buffer = shader->GetConstantBufferByName(target.cbuffer_name.c_str());
    BufferDesc desc{};
    if (!buffer || FAILED(buffer->GetDesc(&desc)) || desc.Type != D3D_CT_CBUFFER)
        throw std::runtime_error("FXC cbuffer not present in compiled shader reflection; "
                                 "it may be unused or optimized out.");
    result.size = desc.Size;
    result.allocation_size = desc.Size;
    for (UINT index = 0; index < desc.Variables; ++index) {
        auto* variable = buffer->GetVariableByIndex(index);
        VariableDesc variable_desc{};
        if (!variable || FAILED(variable->GetDesc(&variable_desc)) ||
            variable_desc.StartOffset > desc.Size ||
            variable_desc.Size > desc.Size - variable_desc.StartOffset)
            throw std::runtime_error("FXC invalid cbuffer variable descriptor.");
        auto member = element<std::remove_pointer_t<decltype(variable->GetType())>, TypeDesc>(
            variable->GetType(), variable_desc.Name ? variable_desc.Name : "",
            variable_desc.StartOffset, variable_desc.Size);
        // The top-level variable payload size is provided directly by FXC.
        member.size = variable_desc.Size;
        if (member.name == target.selected_field) {
            result.selected_name = member.name;
            result.selected_type = member.type;
            result.selected_size = member.size;
            result.packed_offset = member.offset;
        }
        result.members.push_back(std::move(member));
    }
    return result;
}
#endif
} // namespace

CompilationInfo compilation_info_from_fxc(const std::vector<SourceFile>& sources,
                                          const CompilerOptions& options,
                                          std::string_view main_path) {
    auto result = configuration(options);
#ifdef _WIN32
    try {
        Runtime runtime(options.fxc_runtime_path);
        result.compiler_runtime_path = runtime.path;
        auto object = compile(runtime, sources, options, main_path, result);
        if (!result.success)
            return result;
        result.output = CompilationOutput{object->GetBufferSize(), "dxbc"};
        try {
            result.reflection =
                reflection(runtime, object.Get(), result.target_profile.ends_with("_5_1"),
                           result.stage == "cs");
        } catch (const std::exception& exception) {
            CompilationReflection unavailable;
            unavailable.available = false;
            unavailable.unavailable_reason = exception.what();
            result.reflection = std::move(unavailable);
        }
        CompilationDisassembly disassembly;
        disassembly.format = "dxbc";
        ComPtr<ID3DBlob> text;
        if (SUCCEEDED(runtime.disassemble(object->GetBufferPointer(), object->GetBufferSize(), 0,
                                          nullptr, text.GetAddressOf())) &&
            text) {
            disassembly.available = true;
            disassembly.text = blob_text(text.Get());
            disassembly.original_size = disassembly.text.size();
            constexpr std::size_t limit = 256 * 1024;
            disassembly.truncated = disassembly.text.size() > limit;
            disassembly.text.resize((std::min)(limit, disassembly.text.size()));
            disassembly.displayed_size = disassembly.text.size();
        } else {
            disassembly.unavailable_reason = "FXC D3DDisassemble failed.";
        }
        result.disassembly = std::move(disassembly);
        CompilationCompatibility compatibility;
        ComPtr<ID3DBlob> root_signature;
        const auto part_status =
            runtime.get_blob_part(object->GetBufferPointer(), object->GetBufferSize(),
                                  D3D_BLOB_ROOT_SIGNATURE, 0, root_signature.GetAddressOf());
        if (SUCCEEDED(part_status) && root_signature && root_signature->GetBufferSize()) {
            RootSignatureInfo signature;
            signature.availability = RootSignatureAvailability::present_details_unavailable;
            signature.unavailable_reason =
                "D3DGetBlobPart reports an embedded root signature; native root-signature "
                "deserialization is unavailable in the FXC backend.";
            result.root_signature = std::move(signature);
            compatibility.explanation =
                "Embedded FXC root signature is present, but compatibility is unknown "
                "without native deserialization.";
        } else if (part_status == E_FAIL) {
            // D3DGetBlobPart returns E_FAIL when the requested part is absent.
            result.root_signature = RootSignatureInfo{};
            compatibility.explanation = "Compiled FXC bytecode has no embedded root signature.";
        } else {
            compatibility.explanation =
                "FXC root-signature presence is unavailable: D3DGetBlobPart failed or "
                "returned an empty part; HRESULT " +
                std::to_string(part_status) + ".";
        }
        result.compatibility = std::move(compatibility);
    } catch (const std::exception& exception) {
        result.success = false;
        error(result.diagnostics, exception.what(), main_path);
    }
#else
    (void)sources;
    error(result.diagnostics, "FXC backend is available only on Windows (D3DCompiler).", main_path);
#endif
    return result;
}

PreprocessOutput preprocess_from_fxc(const std::vector<SourceFile>& sources,
                                     const CompilerOptions& options, std::string_view main_path) {
    PreprocessOutput result;
#ifdef _WIN32
    try {
        Runtime runtime(options.fxc_runtime_path);
        Arguments arguments(options);
        const auto& source = root(sources, main_path);
        Includes includes(sources, options, main_path, source.text.data());
        ComPtr<ID3DBlob> text;
        ComPtr<ID3DBlob> messages;
        const auto status = runtime.preprocess(
            source.text.data(), source.text.size(), source.path.c_str(), arguments.macros.data(),
            &includes, text.GetAddressOf(), messages.GetAddressOf());
        diagnostics(messages.Get(), result.diagnostics, main_path, sources, options);
        if (!includes.failure.empty())
            error(result.diagnostics, includes.failure, main_path);
        result.available = SUCCEEDED(status) && text;
        if (result.available)
            result.text = blob_text(text.Get());
        else if (result.diagnostics.empty())
            error(result.diagnostics, "FXC preprocessing failed.", main_path);
    } catch (const std::exception& exception) {
        error(result.diagnostics, exception.what(), main_path);
    }
#else
    (void)sources;
    (void)options;
    error(result.diagnostics, "FXC preprocessing is available only on Windows.", main_path);
#endif
    return result;
}

std::optional<MemoryLayout> memory_layout_from_fxc(const std::vector<SourceFile>& sources,
                                                   const CompilerOptions& options,
                                                   std::string_view main_path,
                                                   const ProbeTarget& target) {
    MemoryLayout unavailable;
    unavailable.name = target.cbuffer_name.empty() ? target.type_name : target.cbuffer_name;
    unavailable.kind =
        target.cbuffer_name.empty() ? MemoryLayoutKind::natural : MemoryLayoutKind::constant_buffer;
    unavailable.supported = false;
    unavailable.explanation = "FXC cannot expose natural struct layouts without source rewriting; "
                              "only original compiled cbuffer reflection is available.";
#ifdef _WIN32
    if (target.cbuffer_name.empty())
        return unavailable;
    try {
        Runtime runtime(options.fxc_runtime_path);
        auto info = configuration(options);
        auto object = compile(runtime, sources, options, main_path, info);
        if (!info.success)
            throw std::runtime_error(info.diagnostics.empty() ? "FXC compilation failed."
                                                              : info.diagnostics.front().message);
        if (info.target_profile.ends_with("_5_1")) {
            ComPtr<ID3D12ShaderReflection> shader;
            if (FAILED(runtime.reflect(object->GetBufferPointer(), object->GetBufferSize(),
                                       __uuidof(ID3D12ShaderReflection),
                                       reinterpret_cast<void**>(shader.GetAddressOf()))))
                throw std::runtime_error("FXC SM5.1 reflection is unavailable.");
            return layout<ID3D12ShaderReflection, D3D12_SHADER_BUFFER_DESC,
                          D3D12_SHADER_VARIABLE_DESC, D3D12_SHADER_TYPE_DESC>(shader.Get(), target);
        }
        ComPtr<ID3D11ShaderReflection> shader;
        if (FAILED(runtime.reflect(object->GetBufferPointer(), object->GetBufferSize(),
                                   __uuidof(ID3D11ShaderReflection),
                                   reinterpret_cast<void**>(shader.GetAddressOf()))))
            throw std::runtime_error("FXC cbuffer reflection is unavailable.");
        return layout<ID3D11ShaderReflection, D3D11_SHADER_BUFFER_DESC, D3D11_SHADER_VARIABLE_DESC,
                      D3D11_SHADER_TYPE_DESC>(shader.Get(), target);
    } catch (const std::exception& exception) {
        unavailable.explanation = exception.what();
    }
#else
    (void)sources;
    (void)options;
    (void)main_path;
    unavailable.explanation = "FXC layout reflection is available only on Windows.";
#endif
    return unavailable;
}
} // namespace hlsl_intellisense::dxc::detail
