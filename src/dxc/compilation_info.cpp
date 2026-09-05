#include "compilation_info.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <WinAdapter.h>
#endif

#include <dxcapi.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#if defined(_MSC_VER)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif

// ---------------------------------------------------------------------------
// Minimal D3D12 shader-reflection definitions (portable, no d3dcommon.h).
// These mirror the binary-stable COM interfaces, structs, and enum values
// from d3d12shader.h / d3dcommon.h, keeping the build independent of the
// Windows SDK on all platforms. This mirrors the approach already used by
// memory_layout.cpp; the two files intentionally keep independent local
// copies rather than sharing a header, since each only declares the ABI
// prefix it actually calls through.
// ---------------------------------------------------------------------------
namespace {

// D3D_REGISTER_COMPONENT_TYPE
constexpr unsigned RCT_UINT32 = 1;
constexpr unsigned RCT_SINT32 = 2;
constexpr unsigned RCT_FLOAT32 = 3;

// D3D_NAME (system-value semantics); only the values DXC commonly emits are
// named, everything else falls back to a numeric spelling.
constexpr unsigned NAME_UNDEFINED = 0;
constexpr unsigned NAME_POSITION = 1;
constexpr unsigned NAME_CLIP_DISTANCE = 2;
constexpr unsigned NAME_CULL_DISTANCE = 3;
constexpr unsigned NAME_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr unsigned NAME_VIEWPORT_ARRAY_INDEX = 5;
constexpr unsigned NAME_VERTEX_ID = 6;
constexpr unsigned NAME_PRIMITIVE_ID = 7;
constexpr unsigned NAME_INSTANCE_ID = 8;
constexpr unsigned NAME_IS_FRONT_FACE = 9;
constexpr unsigned NAME_SAMPLE_INDEX = 10;
constexpr unsigned NAME_BARYCENTRICS = 23;
constexpr unsigned NAME_SHADINGRATE = 24;
constexpr unsigned NAME_CULLPRIMITIVE = 25;
constexpr unsigned NAME_TARGET = 64;
constexpr unsigned NAME_DEPTH = 65;
constexpr unsigned NAME_COVERAGE = 66;
constexpr unsigned NAME_DEPTH_GREATER_EQUAL = 67;
constexpr unsigned NAME_DEPTH_LESS_EQUAL = 68;
constexpr unsigned NAME_STENCIL_REF = 69;
constexpr unsigned NAME_INNER_COVERAGE = 70;

// D3D_SHADER_INPUT_TYPE
constexpr unsigned SIT_CBUFFER = 0;
constexpr unsigned SIT_TBUFFER = 1;
constexpr unsigned SIT_TEXTURE = 2;
constexpr unsigned SIT_SAMPLER = 3;
constexpr unsigned SIT_UAV_RWTYPED = 4;
constexpr unsigned SIT_STRUCTURED = 5;
constexpr unsigned SIT_UAV_RWSTRUCTURED = 6;
constexpr unsigned SIT_BYTEADDRESS = 7;
constexpr unsigned SIT_UAV_RWBYTEADDRESS = 8;
constexpr unsigned SIT_UAV_APPEND_STRUCTURED = 9;
constexpr unsigned SIT_UAV_CONSUME_STRUCTURED = 10;
constexpr unsigned SIT_UAV_RWSTRUCTURED_WITH_COUNTER = 11;
constexpr unsigned SIT_RTACCELERATIONSTRUCTURE = 12;
constexpr unsigned SIT_UAV_FEEDBACKTEXTURE = 13;

// D3D_SRV_DIMENSION
constexpr unsigned SRV_DIMENSION_BUFFER = 1;
constexpr unsigned SRV_DIMENSION_TEXTURE1D = 2;
constexpr unsigned SRV_DIMENSION_TEXTURE1DARRAY = 3;
constexpr unsigned SRV_DIMENSION_TEXTURE2D = 4;
constexpr unsigned SRV_DIMENSION_TEXTURE2DARRAY = 5;
constexpr unsigned SRV_DIMENSION_TEXTURE2DMS = 6;
constexpr unsigned SRV_DIMENSION_TEXTURE2DMSARRAY = 7;
constexpr unsigned SRV_DIMENSION_TEXTURE3D = 8;
constexpr unsigned SRV_DIMENSION_TEXTURECUBE = 9;
constexpr unsigned SRV_DIMENSION_TEXTURECUBEARRAY = 10;
constexpr unsigned SRV_DIMENSION_BUFFEREX = 11;

// D3D_RESOURCE_RETURN_TYPE
constexpr unsigned RETURN_TYPE_UNORM = 1;
constexpr unsigned RETURN_TYPE_SNORM = 2;
constexpr unsigned RETURN_TYPE_SINT = 3;
constexpr unsigned RETURN_TYPE_UINT = 4;
constexpr unsigned RETURN_TYPE_FLOAT = 5;
constexpr unsigned RETURN_TYPE_MIXED = 6;
constexpr unsigned RETURN_TYPE_DOUBLE = 7;
constexpr unsigned RETURN_TYPE_CONTINUED = 8;

// IID_ID3D12ShaderReflection {5A58797D-A72C-478D-8BA2-EFC6B0EFE88E}
constexpr GUID kIID_ShaderReflection = {
    0x5A58797DU, 0xA72C, 0x478D, {0x8B, 0xA2, 0xEF, 0xC6, 0xB0, 0xEF, 0xE8, 0x8E}};

// Binary-compatible struct descriptors (must match D3D12_SHADER_DESC layout
// in full, even though only a subset of fields is read, because
// ID3D12ShaderReflection::GetDesc writes the complete struct).
struct ShaderDesc {
    unsigned Version;
    LPCSTR Creator;
    unsigned Flags;
    unsigned ConstantBuffers;
    unsigned BoundResources;
    unsigned InputParameters;
    unsigned OutputParameters;
    unsigned InstructionCount;
    unsigned TempRegisterCount;
    unsigned TempArrayCount;
    unsigned DefCount;
    unsigned DclCount;
    unsigned TextureNormalInstructions;
    unsigned TextureLoadInstructions;
    unsigned TextureCompInstructions;
    unsigned TextureBiasInstructions;
    unsigned TextureGradientInstructions;
    unsigned FloatInstructionCount;
    unsigned IntInstructionCount;
    unsigned UintInstructionCount;
    unsigned StaticFlowControlCount;
    unsigned DynamicFlowControlCount;
    unsigned MacroInstructionCount;
    unsigned ArrayInstructionCount;
    unsigned CutInstructionCount;
    unsigned EmitInstructionCount;
    unsigned GSOutputTopology;
    unsigned GSMaxOutputVertexCount;
    unsigned InputPrimitive;
    unsigned PatchConstantParameters;
    unsigned cGSInstanceCount;
    unsigned cControlPoints;
    unsigned HSOutputPrimitive;
    unsigned HSPartitioning;
    unsigned TessellatorDomain;
    unsigned cBarrierInstructions;
    unsigned cInterlockedInstructions;
    unsigned cTextureStoreInstructions;
};

// Matches D3D12_SIGNATURE_PARAMETER_DESC exactly (complete struct).
struct SignatureParameterDesc {
    LPCSTR SemanticName;
    unsigned SemanticIndex;
    unsigned Register;
    unsigned SystemValueType;
    unsigned ComponentType;
    unsigned char Mask;
    unsigned char ReadWriteMask;
    unsigned Stream;
    unsigned MinPrecision;
};

// Matches D3D12_SHADER_INPUT_BIND_DESC exactly (complete struct).
struct ShaderInputBindDesc {
    LPCSTR Name;
    unsigned Type;
    unsigned BindPoint;
    unsigned BindCount;
    unsigned uFlags;
    unsigned ReturnType;
    unsigned Dimension;
    unsigned NumSamples;
    unsigned Space;
    unsigned uID;
};

// ID3D12ShaderReflection is IUnknown-derived. We declare the prefix of the
// real vtable up to and including the last method we call
// (GetThreadGroupSize); the trailing methods are never invoked through this
// local interface, so they do not need to be declared.
struct IShaderReflection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ShaderDesc* pDesc) = 0;
    virtual void* STDMETHODCALLTYPE GetConstantBufferByIndex(unsigned Index) = 0;
    virtual void* STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR Name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(unsigned ResourceIndex,
                                                             ShaderInputBindDesc* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputParameterDesc(unsigned ParameterIndex,
                                                            SignatureParameterDesc* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(unsigned ParameterIndex,
                                                             SignatureParameterDesc* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetPatchConstantParameterDesc(unsigned ParameterIndex, SignatureParameterDesc* pDesc) = 0;
    virtual void* STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR Name,
                                                                   ShaderInputBindDesc* pDesc) = 0;
    virtual unsigned STDMETHODCALLTYPE GetMovInstructionCount() = 0;
    virtual unsigned STDMETHODCALLTYPE GetMovcInstructionCount() = 0;
    virtual unsigned STDMETHODCALLTYPE GetConversionInstructionCount() = 0;
    virtual unsigned STDMETHODCALLTYPE GetBitwiseInstructionCount() = 0;
    virtual unsigned STDMETHODCALLTYPE GetGSInputPrimitive() = 0;
    virtual BOOL STDMETHODCALLTYPE IsSampleFrequencyShader() = 0;
    virtual unsigned STDMETHODCALLTYPE GetNumInterfaceSlots() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(void* pLevel) = 0;
    virtual unsigned STDMETHODCALLTYPE GetThreadGroupSize(unsigned* pSizeX, unsigned* pSizeY,
                                                          unsigned* pSizeZ) = 0;
};

// ---------------------------------------------------------------------------
// COM helpers (local to this file).
// ---------------------------------------------------------------------------
template <typename Interface> class LocalComPtr final {
  public:
    LocalComPtr() = default;
    LocalComPtr(const LocalComPtr&) = delete;
    auto operator=(const LocalComPtr&) -> LocalComPtr& = delete;
    LocalComPtr(LocalComPtr&& o) noexcept : p_{std::exchange(o.p_, nullptr)} {}
    auto operator=(LocalComPtr&& o) noexcept -> LocalComPtr& {
        if (this != &o) {
            reset();
            p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
    }
    ~LocalComPtr() { reset(); }

    [[nodiscard]] auto get() const noexcept -> Interface* { return p_; }
    [[nodiscard]] auto put() noexcept -> Interface** {
        reset();
        return &p_;
    }
    [[nodiscard]] auto put_void() noexcept -> void** { return reinterpret_cast<void**>(put()); }
    [[nodiscard]] auto operator->() const noexcept -> Interface* { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

  private:
    void reset() noexcept {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    Interface* p_{};
};

void check(HRESULT hr, std::string_view op) {
    if (FAILED(hr)) {
        throw std::runtime_error{std::string{op} + " failed with HRESULT " +
                                 std::to_string(static_cast<unsigned long>(hr))};
    }
}

// ---------------------------------------------------------------------------
// Wide-string helpers for IDxcIncludeHandler / IDxcCompiler3.
// ---------------------------------------------------------------------------
#ifdef _WIN32
[[nodiscard]] std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int len =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(),
                          len);
    return result;
}

[[nodiscard]] std::string wide_to_utf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0')
        return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string result(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    result.pop_back();
    return result;
}
#else
[[nodiscard]] std::wstring utf8_to_wide(std::string_view utf8) {
    std::wstring result;
    result.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        auto c = static_cast<unsigned char>(utf8[i]);
        wchar_t code{};
        if (c < 0x80) {
            code = static_cast<wchar_t>(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            code = static_cast<wchar_t>(((c & 0x1F) << 6) |
                                        (static_cast<unsigned char>(utf8[i + 1]) & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            code = static_cast<wchar_t>(((c & 0x0F) << 12) |
                                        ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                                        (static_cast<unsigned char>(utf8[i + 2]) & 0x3F));
            i += 3;
        } else {
            code = L'?';
            ++i;
        }
        result.push_back(code);
    }
    return result;
}

[[nodiscard]] std::string wide_to_utf8(const wchar_t* wide) {
    std::string result;
    if (wide == nullptr)
        return result;
    for (; *wide != L'\0'; ++wide) {
        auto code = static_cast<unsigned long>(*wide);
        if (code <= 0x7F) {
            result.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Include handler - serves in-memory source files (the root document and all
// resolved includes) to the DXC compiler, bypassing filesystem search paths
// entirely so unsaved edits are honored.
// ---------------------------------------------------------------------------
class InMemoryIncludeHandler final : public IDxcIncludeHandler {
  public:
    InMemoryIncludeHandler(IDxcUtils* utils,
                           const std::vector<hlsl_intellisense::dxc::SourceFile>& sources)
        : utils_{utils}, sources_{sources} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IDxcIncludeHandler))) {
            *ppv = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++ref_count_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --ref_count_;
        if (count == 0)
            delete this;
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override {
        if (ppIncludeSource == nullptr)
            return E_POINTER;
        *ppIncludeSource = nullptr;

        const auto path = wide_to_utf8(pFilename);
        for (const auto& source : sources_) {
            if (path_matches(source, path)) {
                LocalComPtr<IDxcBlobEncoding> blob;
                HRESULT hr = utils_->CreateBlobFromPinned(source.text.data(),
                                                          static_cast<UINT32>(source.text.size()),
                                                          DXC_CP_UTF8, blob.put());
                if (FAILED(hr))
                    return hr;
                *ppIncludeSource = blob.get();
                blob.get()->AddRef();
                return S_OK;
            }
        }
        return E_FAIL;
    }

  private:
    [[nodiscard]] static bool path_matches(const hlsl_intellisense::dxc::SourceFile& source,
                                           std::string_view requested) {
        if (requested.empty())
            return false;

        auto normalize = [](std::string_view path) {
            std::string result{path};
            std::ranges::replace(result, '\\', '/');
#ifdef _WIN32
            std::ranges::transform(result, result.begin(), [](char value) {
                if (value >= 'A' && value <= 'Z')
                    return static_cast<char>(value - 'A' + 'a');
                return value;
            });
#endif
            return result;
        };

        const auto normalized_full = normalize(source.path);
        const auto normalized_requested = normalize(requested);
        if (normalized_full == normalized_requested)
            return true;
        if (normalized_full.size() > normalized_requested.size()) {
            const auto separator =
                normalized_full[normalized_full.size() - normalized_requested.size() - 1];
            if (separator == '/') {
                return normalized_full.substr(normalized_full.size() -
                                              normalized_requested.size()) == normalized_requested;
            }
        }
        return false;
    }

    unsigned long ref_count_{1};
    IDxcUtils* utils_;
    const std::vector<hlsl_intellisense::dxc::SourceFile>& sources_;
};

// ---------------------------------------------------------------------------
// Enum-to-string mapping helpers. Every mapping falls back to "unknown"
// rather than guessing, consistent with the rest of the reflection bridge.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string component_type_name(unsigned value) {
    switch (value) {
    case RCT_UINT32:
        return "uint32";
    case RCT_SINT32:
        return "sint32";
    case RCT_FLOAT32:
        return "float32";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string system_value_name(unsigned value) {
    switch (value) {
    case NAME_UNDEFINED:
        return "undefined";
    case NAME_POSITION:
        return "position";
    case NAME_CLIP_DISTANCE:
        return "clip_distance";
    case NAME_CULL_DISTANCE:
        return "cull_distance";
    case NAME_RENDER_TARGET_ARRAY_INDEX:
        return "render_target_array_index";
    case NAME_VIEWPORT_ARRAY_INDEX:
        return "viewport_array_index";
    case NAME_VERTEX_ID:
        return "vertex_id";
    case NAME_PRIMITIVE_ID:
        return "primitive_id";
    case NAME_INSTANCE_ID:
        return "instance_id";
    case NAME_IS_FRONT_FACE:
        return "is_front_face";
    case NAME_SAMPLE_INDEX:
        return "sample_index";
    case NAME_BARYCENTRICS:
        return "barycentrics";
    case NAME_SHADINGRATE:
        return "shading_rate";
    case NAME_CULLPRIMITIVE:
        return "cull_primitive";
    case NAME_TARGET:
        return "target";
    case NAME_DEPTH:
        return "depth";
    case NAME_COVERAGE:
        return "coverage";
    case NAME_DEPTH_GREATER_EQUAL:
        return "depth_greater_equal";
    case NAME_DEPTH_LESS_EQUAL:
        return "depth_less_equal";
    case NAME_STENCIL_REF:
        return "stencil_ref";
    case NAME_INNER_COVERAGE:
        return "inner_coverage";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string resource_type_name(unsigned value) {
    switch (value) {
    case SIT_CBUFFER:
        return "cbuffer";
    case SIT_TBUFFER:
        return "tbuffer";
    case SIT_TEXTURE:
        return "texture";
    case SIT_SAMPLER:
        return "sampler";
    case SIT_UAV_RWTYPED:
        return "uav_rwtyped";
    case SIT_STRUCTURED:
        return "structured_buffer";
    case SIT_UAV_RWSTRUCTURED:
        return "uav_rwstructured";
    case SIT_BYTEADDRESS:
        return "byteaddress_buffer";
    case SIT_UAV_RWBYTEADDRESS:
        return "uav_rwbyteaddress";
    case SIT_UAV_APPEND_STRUCTURED:
        return "uav_append_structured";
    case SIT_UAV_CONSUME_STRUCTURED:
        return "uav_consume_structured";
    case SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        return "uav_rwstructured_with_counter";
    case SIT_RTACCELERATIONSTRUCTURE:
        return "raytracing_acceleration_structure";
    case SIT_UAV_FEEDBACKTEXTURE:
        return "uav_feedback_texture";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string srv_dimension_name(unsigned value) {
    switch (value) {
    case SRV_DIMENSION_BUFFER:
        return "buffer";
    case SRV_DIMENSION_TEXTURE1D:
        return "texture1d";
    case SRV_DIMENSION_TEXTURE1DARRAY:
        return "texture1darray";
    case SRV_DIMENSION_TEXTURE2D:
        return "texture2d";
    case SRV_DIMENSION_TEXTURE2DARRAY:
        return "texture2darray";
    case SRV_DIMENSION_TEXTURE2DMS:
        return "texture2dms";
    case SRV_DIMENSION_TEXTURE2DMSARRAY:
        return "texture2dmsarray";
    case SRV_DIMENSION_TEXTURE3D:
        return "texture3d";
    case SRV_DIMENSION_TEXTURECUBE:
        return "texturecube";
    case SRV_DIMENSION_TEXTURECUBEARRAY:
        return "texturecubearray";
    case SRV_DIMENSION_BUFFEREX:
        return "bufferex";
    default:
        return "";
    }
}

[[nodiscard]] std::string resource_return_type_name(unsigned value) {
    switch (value) {
    case RETURN_TYPE_UNORM:
        return "unorm";
    case RETURN_TYPE_SNORM:
        return "snorm";
    case RETURN_TYPE_SINT:
        return "sint";
    case RETURN_TYPE_UINT:
        return "uint";
    case RETURN_TYPE_FLOAT:
        return "float";
    case RETURN_TYPE_MIXED:
        return "mixed";
    case RETURN_TYPE_DOUBLE:
        return "double";
    case RETURN_TYPE_CONTINUED:
        return "continued";
    default:
        return "";
    }
}

// ---------------------------------------------------------------------------
// Effective configuration extraction from the already-resolved compiler
// argument list. This does not parse HLSL; it reads DXC's own flag syntax.
// ---------------------------------------------------------------------------
struct EffectiveConfig {
    std::string entry_point;
    std::string target_profile;
    std::string language_version;
    std::vector<std::string> defines;
    std::vector<std::string> include_directories;
};

[[nodiscard]] EffectiveConfig parse_effective_config(const std::vector<std::string>& arguments) {
    // DXC's argument parser accepts both separated ("-T ps_6_0") and joined
    // ("-Tps_6_0") spellings for -T/-E/-D/-I (mirroring the handling already
    // used for -T/-E in memory_layout.cpp), but not for -HV (confirmed
    // against the compiler: "-HV2021" is rejected as an unknown argument),
    // so -HV is only recognized in its separated form. Later occurrences win
    // for single-valued fields (entry point/target profile/language
    // version); defines/include directories accumulate in argument order,
    // matching DXC's own last-wins/accumulate semantics.
    EffectiveConfig config;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "-E" && index + 1 < arguments.size()) {
            config.entry_point = arguments[++index];
        } else if (argument.starts_with("-E") && argument.size() > 2) {
            config.entry_point = argument.substr(2);
        } else if (argument == "-T" && index + 1 < arguments.size()) {
            config.target_profile = arguments[++index];
        } else if (argument.starts_with("-T") && argument.size() > 2) {
            config.target_profile = argument.substr(2);
        } else if (argument == "-HV" && index + 1 < arguments.size()) {
            config.language_version = arguments[++index];
        } else if (argument == "-D" && index + 1 < arguments.size()) {
            config.defines.push_back(arguments[++index]);
        } else if (argument.starts_with("-D") && argument.size() > 2) {
            config.defines.push_back(argument.substr(2));
        } else if (argument == "-I" && index + 1 < arguments.size()) {
            config.include_directories.push_back(arguments[++index]);
        } else if (argument.starts_with("-I") && argument.size() > 2) {
            config.include_directories.push_back(argument.substr(2));
        }
    }
    return config;
}

[[nodiscard]] std::string stage_from_target_profile(std::string_view target_profile) {
    const auto separator = target_profile.find('_');
    const auto prefix =
        separator == std::string_view::npos ? target_profile : target_profile.substr(0, separator);
    if (prefix == "cs")
        return "compute";
    if (prefix == "ps")
        return "pixel";
    if (prefix == "vs")
        return "vertex";
    if (prefix == "gs")
        return "geometry";
    if (prefix == "hs")
        return "hull";
    if (prefix == "ds")
        return "domain";
    if (prefix == "ms")
        return "mesh";
    if (prefix == "as")
        return "amplification";
    if (prefix == "lib")
        return "library";
    return std::string{prefix};
}

[[nodiscard]] hlsl_intellisense::dxc::DiagnosticSeverity
severity_from_label(std::string_view label) {
    using hlsl_intellisense::dxc::DiagnosticSeverity;
    if (label == "fatal error") {
        return DiagnosticSeverity::fatal;
    }
    if (label == "error") {
        return DiagnosticSeverity::error;
    }
    if (label == "warning") {
        return DiagnosticSeverity::warning;
    }
    return DiagnosticSeverity::note;
}

// Splits DXC's own clang-style diagnostic text ("path:line:col: severity:
// message") into structured diagnostics. This parses the compiler's uniform
// diagnostic output format, not HLSL source. Lines that do not match the
// pattern (source snippets, caret markers) are ignored. A secondary pattern
// covers argument/global diagnostics DXC reports without a source location
// (e.g. "error: invalid profile"), which still deserve a structured
// severity/message rather than falling back to a generic message.
[[nodiscard]] std::vector<hlsl_intellisense::dxc::Diagnostic>
parse_compiler_diagnostics(std::string_view text) {
    using hlsl_intellisense::dxc::Diagnostic;

    static const std::regex located_pattern{
        R"(^(.+?):(\d+):(\d+): (fatal error|error|warning|note): (.*)$)"};
    static const std::regex unlocated_pattern{R"(^(fatal error|error|warning|note): (.*)$)"};

    std::vector<Diagnostic> result;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto newline = text.find('\n', line_start);
        auto line = newline == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, newline - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const std::string owned_line{line};
        std::smatch match;
        if (std::regex_match(owned_line, match, located_pattern)) {
            Diagnostic diagnostic;
            diagnostic.severity = severity_from_label(match[4].str());
            diagnostic.location.path = match[1].str();
            diagnostic.location.line = static_cast<std::uint32_t>(std::stoul(match[2].str()));
            diagnostic.location.column = static_cast<std::uint32_t>(std::stoul(match[3].str()));
            diagnostic.message = match[5].str();
            result.push_back(std::move(diagnostic));
        } else if (std::regex_match(owned_line, match, unlocated_pattern)) {
            Diagnostic diagnostic;
            diagnostic.severity = severity_from_label(match[1].str());
            diagnostic.message = match[2].str();
            result.push_back(std::move(diagnostic));
        }

        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }
    return result;
}

} // namespace

namespace hlsl_intellisense::dxc::detail {

CompilationInfo compilation_info_from_compile(DxcCreateInstanceProc create_instance,
                                              const std::vector<SourceFile>& sources,
                                              const std::vector<std::string>& arguments,
                                              std::string_view main_path) {
    CompilationInfo info;
    const auto effective = parse_effective_config(arguments);
    info.entry_point = effective.entry_point;
    info.target_profile = effective.target_profile;
    info.stage = stage_from_target_profile(effective.target_profile);
    info.language_version = effective.language_version;
    info.defines = effective.defines;
    info.include_directories = effective.include_directories;
    info.compiler_arguments = arguments;
    info.resolved_include_paths.reserve(sources.size());
    for (const auto& source : sources) {
        if (source.path != main_path) {
            info.resolved_include_paths.push_back(source.path);
        }
    }

    const auto source_it = std::ranges::find(sources, main_path, &SourceFile::path);
    if (source_it == sources.end()) {
        info.success = false;
        info.diagnostics.push_back({.severity = DiagnosticSeverity::fatal,
                                    .message = "The requested document has no resolved source"});
        return info;
    }

    LocalComPtr<IDxcCompiler3> compiler;
    check(create_instance(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), compiler.put_void()),
          "Create IDxcCompiler3");
    LocalComPtr<IDxcUtils> utils;
    check(create_instance(CLSID_DxcUtils, __uuidof(IDxcUtils), utils.put_void()),
          "Create IDxcUtils");

    std::vector<std::wstring> wide_args;
    const auto source_directory = std::filesystem::path{main_path}.parent_path();
    if (!source_directory.empty()) {
        wide_args.push_back(L"-I");
        wide_args.push_back(utf8_to_wide(source_directory.generic_string()));
    }
    for (const auto& argument : arguments) {
        wide_args.push_back(utf8_to_wide(argument));
    }
    const bool wants_spirv = std::ranges::any_of(
        arguments, [](const std::string& argument) { return argument == "-spirv"; });

    std::vector<LPCWSTR> arg_ptrs;
    arg_ptrs.reserve(wide_args.size());
    for (const auto& wide_argument : wide_args) {
        arg_ptrs.push_back(wide_argument.c_str());
    }

    LocalComPtr<IDxcBlobEncoding> source_blob;
    check(utils->CreateBlobFromPinned(source_it->text.data(),
                                      static_cast<UINT32>(source_it->text.size()), DXC_CP_UTF8,
                                      source_blob.put()),
          "CreateBlobFromPinned");
    DxcBuffer source_buffer{.Ptr = source_blob->GetBufferPointer(),
                            .Size = source_blob->GetBufferSize(),
                            .Encoding = DXC_CP_UTF8};

    auto* include_handler = new InMemoryIncludeHandler(utils.get(), sources);

    LocalComPtr<IDxcResult> result;
    const HRESULT compile_hr =
        compiler->Compile(&source_buffer, arg_ptrs.data(), static_cast<UINT32>(arg_ptrs.size()),
                          include_handler, __uuidof(IDxcResult), result.put_void());
    include_handler->Release();

    if (FAILED(compile_hr) || !result) {
        info.success = false;
        info.diagnostics.push_back({.severity = DiagnosticSeverity::fatal,
                                    .message = "DXC compilation failed to produce a result"});
        return info;
    }

    LocalComPtr<IDxcBlobEncoding> errors;
    result->GetErrorBuffer(errors.put());
    if (errors && errors->GetBufferSize() > 0) {
        const std::string_view error_text{static_cast<const char*>(errors->GetBufferPointer()),
                                          errors->GetBufferSize()};
        info.diagnostics = parse_compiler_diagnostics(error_text);
    }

    HRESULT status{};
    check(result->GetStatus(&status), "GetStatus");
    if (FAILED(status)) {
        info.success = false;
        if (info.diagnostics.empty()) {
            info.diagnostics.push_back(
                {.severity = DiagnosticSeverity::fatal, .message = "DXC compilation failed"});
        }
        return info;
    }
    info.success = true;

    LocalComPtr<IDxcBlob> object_blob;
    check(result->GetResult(object_blob.put()), "GetResult");
    if (!object_blob || object_blob->GetBufferSize() == 0) {
        info.output = CompilationOutput{.size = 0, .type = "none"};
        return info;
    }
    info.output = CompilationOutput{.size = object_blob->GetBufferSize(),
                                    .type = wants_spirv ? "spirv" : "dxil"};

    if (wants_spirv) {
        info.reflection = CompilationReflection{
            .available = false,
            .unavailable_reason = "Reflection is not available for SPIR-V output through the "
                                  "ID3D12ShaderReflection DXC reflection path used by this server"};
        return info;
    }

    DxcBuffer object_buffer{.Ptr = object_blob->GetBufferPointer(),
                            .Size = object_blob->GetBufferSize(),
                            .Encoding = 0};
    LocalComPtr<IShaderReflection> reflection;
    const HRESULT create_reflection_hr =
        utils->CreateReflection(&object_buffer, kIID_ShaderReflection, reflection.put_void());
    if (FAILED(create_reflection_hr) || !reflection) {
        // A successful compile can still produce DXIL without reflection
        // metadata (for example, when compiled with -Qstrip_reflect). DXC
        // remains the sole authority: report the compile as successful while
        // clearly explaining that reflection is unavailable, rather than
        // treating a missing-reflection blob as a hard failure.
        info.reflection = CompilationReflection{
            .available = false,
            .unavailable_reason =
                "DXIL reflection metadata is unavailable for this compiled output (for example, "
                "when compiled with -Qstrip_reflect); IDxcUtils::CreateReflection failed with "
                "HRESULT " +
                std::to_string(static_cast<unsigned long>(create_reflection_hr))};
        return info;
    }

    ShaderDesc shader_desc{};
    const HRESULT shader_desc_hr = reflection->GetDesc(&shader_desc);
    if (FAILED(shader_desc_hr)) {
        info.reflection = CompilationReflection{
            .available = false,
            .unavailable_reason =
                "DXIL reflection metadata is unavailable for this compiled output; "
                "ID3D12ShaderReflection::GetDesc failed with HRESULT " +
                std::to_string(static_cast<unsigned long>(shader_desc_hr))};
        return info;
    }

    CompilationReflection reflection_result;

    reflection_result.input_signature.reserve(shader_desc.InputParameters);
    for (unsigned index = 0; index < shader_desc.InputParameters; ++index) {
        SignatureParameterDesc desc{};
        if (FAILED(reflection->GetInputParameterDesc(index, &desc))) {
            continue;
        }
        reflection_result.input_signature.push_back(
            {.semantic_name = desc.SemanticName != nullptr ? std::string{desc.SemanticName} : "",
             .semantic_index = desc.SemanticIndex,
             .register_index = desc.Register,
             .system_value = system_value_name(desc.SystemValueType),
             .component_type = component_type_name(desc.ComponentType),
             .mask = desc.Mask,
             .read_write_mask = desc.ReadWriteMask,
             .stream = desc.Stream});
    }

    reflection_result.output_signature.reserve(shader_desc.OutputParameters);
    for (unsigned index = 0; index < shader_desc.OutputParameters; ++index) {
        SignatureParameterDesc desc{};
        if (FAILED(reflection->GetOutputParameterDesc(index, &desc))) {
            continue;
        }
        reflection_result.output_signature.push_back(
            {.semantic_name = desc.SemanticName != nullptr ? std::string{desc.SemanticName} : "",
             .semantic_index = desc.SemanticIndex,
             .register_index = desc.Register,
             .system_value = system_value_name(desc.SystemValueType),
             .component_type = component_type_name(desc.ComponentType),
             .mask = desc.Mask,
             .read_write_mask = desc.ReadWriteMask,
             .stream = desc.Stream});
    }

    reflection_result.resources.reserve(shader_desc.BoundResources);
    for (unsigned index = 0; index < shader_desc.BoundResources; ++index) {
        ShaderInputBindDesc desc{};
        if (FAILED(reflection->GetResourceBindingDesc(index, &desc))) {
            continue;
        }
        reflection_result.resources.push_back(
            {.name = desc.Name != nullptr ? std::string{desc.Name} : "",
             .type = resource_type_name(desc.Type),
             .bind_point = desc.BindPoint,
             .bind_count = desc.BindCount,
             .space = desc.Space,
             .dimension = srv_dimension_name(desc.Dimension),
             .return_type = resource_return_type_name(desc.ReturnType)});
    }

    if (info.stage == "compute") {
        unsigned x{};
        unsigned y{};
        unsigned z{};
        static_cast<void>(reflection->GetThreadGroupSize(&x, &y, &z));
        reflection_result.thread_group_size = CompilationThreadGroupSize{.x = x, .y = y, .z = z};
    }

    info.reflection = std::move(reflection_result);
    return info;
}

} // namespace hlsl_intellisense::dxc::detail
