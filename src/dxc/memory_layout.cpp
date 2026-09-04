#include "memory_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
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
// These mirror the binary-stable COM interfaces and constants from
// d3d12shader.h / d3dcommon.h, keeping the build independent of the Windows
// SDK on all platforms.
// ---------------------------------------------------------------------------
namespace {

// D3D_SHADER_VARIABLE_CLASS
constexpr unsigned SVC_SCALAR = 0;
constexpr unsigned SVC_VECTOR = 1;
constexpr unsigned SVC_MATRIX_ROWS = 2;
constexpr unsigned SVC_MATRIX_COLUMNS = 3;
constexpr unsigned SVC_STRUCT = 5;

// D3D_SHADER_VARIABLE_TYPE
constexpr unsigned SVT_BOOL = 1;
constexpr unsigned SVT_INT = 2;
constexpr unsigned SVT_FLOAT = 3;
constexpr unsigned SVT_UINT = 19;
constexpr unsigned SVT_DOUBLE = 39;
constexpr unsigned SVT_FLOAT16 = 58;
constexpr unsigned SVT_INT16 = 59;
constexpr unsigned SVT_UINT16 = 60;
constexpr unsigned SVT_INT64 = 61;
constexpr unsigned SVT_UINT64 = 62;

// D3D_CBUFFER_TYPE
constexpr unsigned CT_CBUFFER = 0;
constexpr unsigned CT_RESOURCE_BIND_INFO = 3;

// IID_ID3D12ShaderReflection {5A58797D-A72C-478D-8BA2-EFC6B0EFE88E}
static const GUID kIID_ShaderReflection = {
    0x5A58797DU, 0xA72C, 0x478D, {0x8B, 0xA2, 0xEF, 0xC6, 0xB0, 0xEF, 0xE8, 0x8E}};

// Binary-compatible struct descriptors (match D3D12_SHADER_*_DESC layout).
struct ShaderDesc {
    unsigned Version;
    LPCSTR Creator;
    unsigned Flags;
    unsigned ConstantBuffers;
    unsigned BoundResources;
    // We don't need the rest; only ConstantBuffers is used.
    // The struct layout must match the full D3D12_SHADER_DESC up to ConstantBuffers.
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
struct ShaderBufferDesc {
    LPCSTR Name;
    unsigned Type;
    unsigned Variables;
    unsigned Size;
    unsigned uFlags;
};

struct ShaderVariableDesc {
    LPCSTR Name;
    unsigned StartOffset;
    unsigned Size;
    unsigned uFlags;
    LPVOID DefaultValue;
    unsigned StartTexture;
    unsigned TextureSize;
    unsigned StartSampler;
    unsigned SamplerSize;
};

struct ShaderTypeDesc {
    unsigned Class;
    unsigned Type;
    unsigned Rows;
    unsigned Columns;
    unsigned Elements;
    unsigned Members;
    unsigned Offset;
    LPCSTR Name;
};

// Binary-compatible COM interfaces (vtable-order matches d3d12shader.h).
// These are NOT IUnknown-derived; DXC returns lightweight sub-objects.
struct IShaderReflectionType {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ShaderTypeDesc* pDesc) = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetMemberTypeByIndex(unsigned Index) = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetMemberTypeByName(LPCSTR Name) = 0;
    virtual LPCSTR STDMETHODCALLTYPE GetMemberTypeName(unsigned Index) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsEqual(IShaderReflectionType* pType) = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetSubType() = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetBaseClass() = 0;
    virtual unsigned STDMETHODCALLTYPE GetNumInterfaces() = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetInterfaceByIndex(unsigned uIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsOfType(IShaderReflectionType* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE ImplementsInterface(IShaderReflectionType* pBase) = 0;
};

struct IShaderReflectionVariable {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ShaderVariableDesc* pDesc) = 0;
    virtual IShaderReflectionType* STDMETHODCALLTYPE GetType() = 0;
    virtual void* STDMETHODCALLTYPE GetBuffer() = 0;
    virtual unsigned STDMETHODCALLTYPE GetInterfaceSlot(unsigned uArrayIndex) = 0;
};

struct IShaderReflectionConstantBuffer {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ShaderBufferDesc* pDesc) = 0;
    virtual IShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByIndex(unsigned Index) = 0;
    virtual IShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) = 0;
};

// ID3D12ShaderReflection is IUnknown-derived. We only use a subset of methods.
struct IShaderReflection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(ShaderDesc* pDesc) = 0;
    virtual IShaderReflectionConstantBuffer* STDMETHODCALLTYPE
    GetConstantBufferByIndex(unsigned Index) = 0;
    virtual IShaderReflectionConstantBuffer* STDMETHODCALLTYPE
    GetConstantBufferByName(LPCSTR Name) = 0;
    // We do not need the remaining methods.
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
// Probe include handler – serves in-memory source files to the DXC compiler.
// ---------------------------------------------------------------------------
class ProbeIncludeHandler final : public IDxcIncludeHandler {
  public:
    ProbeIncludeHandler(IDxcUtils* utils,
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
        // Match if 'full' ends with the requested path after a separator.
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
// Type spelling reconstruction from reflection data.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string scalar_spelling(unsigned svt) {
    switch (svt) {
    case SVT_BOOL:
        return "bool";
    case SVT_INT:
        return "int";
    case SVT_FLOAT:
        return "float";
    case SVT_UINT:
        return "uint";
    case SVT_DOUBLE:
        return "double";
    case SVT_FLOAT16:
        return "half";
    case SVT_INT16:
        return "int16_t";
    case SVT_UINT16:
        return "uint16_t";
    case SVT_INT64:
        return "int64_t";
    case SVT_UINT64:
        return "uint64_t";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string type_spelling(const ShaderTypeDesc& desc) {
    switch (desc.Class) {
    case SVC_SCALAR:
        return scalar_spelling(desc.Type);
    case SVC_VECTOR: {
        auto base = scalar_spelling(desc.Type);
        return base + std::to_string(desc.Columns);
    }
    case SVC_MATRIX_ROWS:
    case SVC_MATRIX_COLUMNS: {
        auto base = scalar_spelling(desc.Type);
        return base + std::to_string(desc.Rows) + "x" + std::to_string(desc.Columns);
    }
    case SVC_STRUCT:
        return desc.Name != nullptr ? std::string{desc.Name} : "struct";
    default:
        return desc.Name != nullptr ? std::string{desc.Name} : "unknown";
    }
}

// ---------------------------------------------------------------------------
// Recursively convert reflection type data into MemoryLayoutElement.
// ---------------------------------------------------------------------------
using namespace hlsl_intellisense::dxc;

constexpr std::size_t kMaxExpandedNodes = 4096;

struct WalkContext {
    std::size_t remaining{kMaxExpandedNodes};
    bool is_cbuffer{false};
    std::string error;
};

[[nodiscard]] MemoryLayoutElementKind map_element_kind(unsigned svc) {
    switch (svc) {
    case SVC_SCALAR:
        return MemoryLayoutElementKind::scalar;
    case SVC_VECTOR:
        return MemoryLayoutElementKind::vector;
    case SVC_MATRIX_ROWS:
    case SVC_MATRIX_COLUMNS:
        return MemoryLayoutElementKind::matrix;
    case SVC_STRUCT:
        return MemoryLayoutElementKind::record;
    default:
        return MemoryLayoutElementKind::scalar;
    }
}

[[nodiscard]] std::uint32_t scalar_byte_size(unsigned svt);

[[nodiscard]] std::uint32_t infer_alignment_from_stride(std::uint32_t stride) {
    if (stride == 0)
        return 1;
    // Alignment is the largest power of 2 dividing the stride.
    std::uint32_t alignment = 1;
    while ((stride & 1) == 0 && alignment < stride) {
        stride >>= 1;
        alignment <<= 1;
    }
    return alignment;
}

void walk_struct_members(IShaderReflectionType* type, const ShaderTypeDesc& desc,
                         std::uint32_t base_offset, std::vector<MemoryLayoutElement>& out,
                         WalkContext& ctx) {
    for (unsigned i = 0; i < desc.Members; ++i) {
        if (ctx.remaining == 0) {
            ctx.error = "Layout expands to more than 4096 elements";
            return;
        }
        --ctx.remaining;

        auto* member_type = type->GetMemberTypeByIndex(i);
        if (member_type == nullptr)
            continue;
        const auto* member_name = type->GetMemberTypeName(i);

        ShaderTypeDesc member_desc{};
        if (FAILED(member_type->GetDesc(&member_desc)))
            continue;

        auto member_spelling = type_spelling(member_desc);
        std::string name_str = member_name != nullptr ? std::string{member_name} : "";

        MemoryLayoutElement element{
            .name = name_str, .type = member_spelling, .offset = base_offset + member_desc.Offset};

        // Compute the data size from the type descriptor.
        std::uint32_t data_size{};
        switch (member_desc.Class) {
        case SVC_SCALAR:
            data_size = scalar_byte_size(member_desc.Type);
            element.alignment = data_size;
            break;
        case SVC_VECTOR:
            data_size = scalar_byte_size(member_desc.Type) * member_desc.Columns;
            element.alignment = scalar_byte_size(member_desc.Type);
            break;
        default:
            break;
        }
        if (data_size > 0 && member_desc.Elements == 0) {
            element.size = data_size;
        }

        if (member_desc.Elements > 0) {
            // Array – the reflection type describes the element type.
            // Create a synthetic wrapper with expanded elements.
            ShaderTypeDesc elem_desc = member_desc;
            elem_desc.Elements = 0;
            elem_desc.Offset = 0;

            element.kind = MemoryLayoutElementKind::array;

            // For the element type, walk recursively.
            MemoryLayoutElement proto;
            proto.name = "[0]";
            proto.type = member_spelling;
            proto.kind = map_element_kind(elem_desc.Class);
            proto.offset = 0;
            proto.array_index = 0;

            // Walk element's own struct members if it's a struct.
            if (elem_desc.Class == SVC_STRUCT) {
                walk_struct_members(member_type, elem_desc, 0, proto.members, ctx);
                if (!ctx.error.empty())
                    return;
            }

            // For matrices, generate vector sub-elements.
            if (elem_desc.Class == SVC_MATRIX_ROWS || elem_desc.Class == SVC_MATRIX_COLUMNS) {
                bool row_major = (elem_desc.Class == SVC_MATRIX_ROWS);
                unsigned vectors = row_major ? elem_desc.Rows : elem_desc.Columns;
                unsigned components = row_major ? elem_desc.Columns : elem_desc.Rows;
                auto scalar = scalar_spelling(elem_desc.Type);
                // Cannot determine element matrix stride from the type alone;
                // use the overall variable/array information.
                // For now, estimate from cbuffer alignment rules.
                proto.row_major = row_major;
                for (unsigned v = 0; v < vectors; ++v) {
                    if (ctx.remaining == 0) {
                        ctx.error = "Layout expands to more than 4096 elements";
                        return;
                    }
                    --ctx.remaining;
                    // We'll fix strides after we know the total size.
                    proto.members.push_back(
                        MemoryLayoutElement{.name = "[" + std::to_string(v) + "]",
                                            .type = member_spelling,
                                            .kind = MemoryLayoutElementKind::vector,
                                            .array_index = v});
                    (void)components;
                    (void)scalar;
                }
            }

            // Determine sizes. For reflection, the variable-level Size covers
            // the entire array. We need to compute element stride.
            // At this point we don't have the variable size; the caller will
            // set element.size, array_stride, and element member offsets from
            // the variable desc.

            // Store element count for post-processing.
            element.array_dimensions.push_back(member_desc.Elements);

            // Store prototype so caller can replicate.
            element.members.push_back(std::move(proto));
        } else {
            element.kind = map_element_kind(member_desc.Class);

            if (member_desc.Class == SVC_STRUCT) {
                walk_struct_members(member_type, member_desc, 0, element.members, ctx);
                if (!ctx.error.empty())
                    return;
            }

            if (member_desc.Class == SVC_MATRIX_ROWS || member_desc.Class == SVC_MATRIX_COLUMNS) {
                element.row_major = (member_desc.Class == SVC_MATRIX_ROWS);
            }
        }

        out.push_back(std::move(element));
    }
}

/// Determine the scalar size for a D3D_SHADER_VARIABLE_TYPE.
[[nodiscard]] std::uint32_t scalar_byte_size(unsigned svt) {
    switch (svt) {
    case SVT_BOOL:
    case SVT_INT:
    case SVT_FLOAT:
    case SVT_UINT:
        return 4;
    case SVT_DOUBLE:
    case SVT_INT64:
    case SVT_UINT64:
        return 8;
    case SVT_FLOAT16:
    case SVT_INT16:
    case SVT_UINT16:
        return 2;
    default:
        return 4;
    }
}

/// Post-process members collected from reflection, fixing sizes and strides
/// using the variable-level size reported by DXC.
void fix_sizes(std::vector<MemoryLayoutElement>& members, std::uint32_t parent_size,
               bool is_cbuffer) {
    // For each member, compute size from the gap to the next member if not already set.
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto& m = members[i];
        if (m.size == 0) {
            const std::uint32_t next_offset =
                (i + 1 < members.size()) ? members[i + 1].offset : parent_size;
            if (next_offset >= m.offset) {
                m.size = next_offset - m.offset;
            }
        }
    }

    for (auto& m : members) {
        if (m.kind == MemoryLayoutElementKind::array && !m.array_dimensions.empty() &&
            m.array_dimensions[0] > 0) {
            const auto count = m.array_dimensions[0];
            const std::uint32_t array_stride = m.size / count;
            m.array_stride = array_stride;

            // Expand the array elements from the prototype.
            if (m.members.size() == 1) {
                auto proto = std::move(m.members[0]);
                proto.size = (count > 1 && array_stride > 0) ? array_stride : m.size;
                // Fix prototype's internal sizes recursively.
                if (!proto.members.empty() && proto.kind == MemoryLayoutElementKind::record) {
                    fix_sizes(proto.members, proto.size, is_cbuffer);
                }

                m.members.clear();
                m.members.reserve(count);
                for (std::uint32_t idx = 0; idx < count; ++idx) {
                    auto elem = proto;
                    elem.name = "[" + std::to_string(idx) + "]";
                    elem.offset = array_stride * idx;
                    elem.array_index = idx;
                    // For record arrays, adjust member offsets (they're already relative to 0).
                    m.members.push_back(std::move(elem));
                }
            }
        } else if (m.kind == MemoryLayoutElementKind::record && !m.members.empty()) {
            fix_sizes(m.members, m.size, is_cbuffer);
        } else if (m.kind == MemoryLayoutElementKind::matrix && !m.members.empty()) {
            // Fix matrix vector sub-elements.
            const auto vectors = static_cast<std::uint32_t>(m.members.size());
            if (vectors > 0) {
                const std::uint32_t matrix_stride = m.size / vectors;
                m.matrix_stride = matrix_stride;
                for (std::uint32_t v = 0; v < vectors; ++v) {
                    m.members[v].offset = matrix_stride * v;
                    m.members[v].size =
                        (v + 1 < vectors) ? matrix_stride : (m.size - matrix_stride * v);
                }
                // Last vector may be smaller (no padding after last).
                // We use scalar size * components for the actual data.
            }
        }

        // Infer alignment from offset when possible.
        if (m.offset > 0) {
            m.alignment = (std::max)(m.alignment, infer_alignment_from_stride(m.offset));
        }
        // For first member or zero-offset members, infer from size.
        if (m.alignment == 0) {
            if (m.kind == MemoryLayoutElementKind::scalar ||
                m.kind == MemoryLayoutElementKind::vector) {
                // Use the member size (for scalars) or a component-level heuristic.
                m.alignment = (m.size > 0 && m.size <= 8) ? m.size : 4;
            } else {
                m.alignment = 1;
            }
        }
    }
}

/// Compute alignment for the top-level type from member offsets and sizes.
[[nodiscard]] std::uint32_t infer_struct_alignment(const std::vector<MemoryLayoutElement>& members,
                                                   std::uint32_t size, bool is_cbuffer) {
    if (is_cbuffer)
        return 16;
    // For natural layout, alignment is the max alignment of all members.
    // Since we infer member alignment from their offsets, compute the GCD
    // of all non-zero offsets and the allocation size.
    std::uint32_t alignment = 1;
    for (const auto& m : members) {
        if (m.offset > 0) {
            alignment = (std::max)(alignment, infer_alignment_from_stride(m.offset));
        }
        if (m.alignment > 0) {
            alignment = (std::max)(alignment, m.alignment);
        }
    }
    // Allocation size should be a multiple of alignment.
    if (size > 0 && alignment > 1) {
        auto inferred = infer_alignment_from_stride(size);
        alignment = (std::max)(alignment, inferred);
    }
    return alignment;
}

} // namespace

namespace hlsl_intellisense::dxc::detail {

std::optional<MemoryLayout> memory_layout_from_probe(DxcCreateInstanceProc create_instance,
                                                     const std::vector<SourceFile>& sources,
                                                     const std::vector<std::string>& arguments,
                                                     std::string_view main_path,
                                                     const ProbeTarget& target) {
    // Step 1: Build the probe source.
    const auto source_it = std::ranges::find(sources, main_path, &SourceFile::path);
    if (source_it == sources.end())
        return std::nullopt;

    const bool is_cbuffer = !target.cbuffer_name.empty();
    const std::string probe_suffix = "__hlsl_lsp_probe_c7e3a1__";

    std::string probe_source = source_it->text;
    probe_source += "\n";

    if (!is_cbuffer) {
        // Struct probe: declare a RWStructuredBuffer<TypeName> to get natural layout.
        probe_source += "RWStructuredBuffer<" + target.type_name + "> " + probe_suffix +
                        "_sb : register(u0);\n";
        probe_source += "[numthreads(1,1,1)] void " + probe_suffix + "_entry() { " +
                        target.type_name + " " + probe_suffix + "_tmp = " + probe_suffix +
                        "_sb[0]; " + probe_suffix + "_sb[0] = " + probe_suffix + "_tmp; }\n";
    } else {
        // Cbuffer probe: the cbuffer is already in the user's source.
        // Write a scalar cbuffer variable to a UAV to force the cbuffer into
        // the compiled output. For struct variables, fall back to referencing
        // the first component.
        std::string ref_expr = target.reference_var;
        if (!ref_expr.empty()) {
            probe_source += "RWByteAddressBuffer " + probe_suffix + "_out : register(u0);\n";
            probe_source += "[numthreads(1,1,1)] void " + probe_suffix + "_entry() { " +
                            probe_suffix + "_out.Store(0, asuint((float)" + ref_expr + ")); }\n";
        } else {
            // No reference variable found; try empty entry point with -Vd.
            probe_source += "RWByteAddressBuffer " + probe_suffix + "_out : register(u0);\n";
            probe_source += "[numthreads(1,1,1)] void " + probe_suffix + "_entry() { " +
                            probe_suffix + "_out.Store(0, 0); }\n";
        }
    }

    // Step 2: Create DXC compiler and utils.
    LocalComPtr<IDxcCompiler3> compiler;
    check(create_instance(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), compiler.put_void()),
          "Create IDxcCompiler3");

    LocalComPtr<IDxcUtils> utils;
    check(create_instance(CLSID_DxcUtils, __uuidof(IDxcUtils), utils.put_void()),
          "Create IDxcUtils");

    // Step 3: Build compiler arguments.
    bool has_16bit = false;
    std::string shader_model;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string_view profile;
        if (arguments[index] == "-T" && index + 1 < arguments.size()) {
            profile = arguments[index + 1];
        } else if (arguments[index].starts_with("-T") && arguments[index].size() > 2) {
            profile = std::string_view{arguments[index]}.substr(2);
        }
        const auto separator = profile.find('_');
        if (separator != std::string_view::npos) {
            shader_model = profile.substr(separator);
        }
    }
    std::vector<std::wstring> wide_args;
    const auto source_directory = std::filesystem::path{main_path}.parent_path();
    if (!source_directory.empty()) {
        wide_args.push_back(L"-I");
        wide_args.push_back(utf8_to_wide(source_directory.generic_string()));
    }
    bool skip_next = false;
    for (const auto& arg : arguments) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (arg == "-E" || arg == "-T") {
            skip_next = true;
            continue;
        }
        if ((arg.starts_with("-E") || arg.starts_with("-T")) && arg.size() > 2) {
            continue;
        }
        if (arg == "-enable-16bit-types")
            has_16bit = true;
        wide_args.push_back(utf8_to_wide(arg));
    }

    // Add probe entry point and target.
    auto wide_entry = utf8_to_wide(probe_suffix + "_entry");
    wide_args.push_back(L"-E");
    wide_args.push_back(wide_entry);
    wide_args.push_back(L"-T");
    wide_args.push_back(utf8_to_wide(
        "cs" + (shader_model.empty() ? (has_16bit ? std::string{"_6_2"} : std::string{"_6_0"})
                                     : shader_model)));
    wide_args.push_back(L"-Od");
    wide_args.push_back(L"-Vd");
    // Remove SPIRV flags.
    std::erase_if(wide_args, [](const std::wstring& w) {
        auto s = wide_to_utf8(w.c_str());
        return s == "-spirv" || s.starts_with("-fspv-");
    });

    std::vector<LPCWSTR> arg_ptrs;
    arg_ptrs.reserve(wide_args.size());
    for (const auto& w : wide_args) {
        arg_ptrs.push_back(w.c_str());
    }

    // Step 4: Create source blob and include handler.
    LocalComPtr<IDxcBlobEncoding> source_blob;
    check(utils->CreateBlobFromPinned(probe_source.data(), static_cast<UINT32>(probe_source.size()),
                                      DXC_CP_UTF8, source_blob.put()),
          "CreateBlobFromPinned");

    DxcBuffer source_buffer{.Ptr = source_blob->GetBufferPointer(),
                            .Size = source_blob->GetBufferSize(),
                            .Encoding = DXC_CP_UTF8};

    auto* include_handler = new ProbeIncludeHandler(utils.get(), sources);

    // Step 5: Compile.
    LocalComPtr<IDxcResult> result;
    HRESULT compile_hr =
        compiler->Compile(&source_buffer, arg_ptrs.data(), static_cast<UINT32>(arg_ptrs.size()),
                          include_handler, __uuidof(IDxcResult), result.put_void());
    include_handler->Release();

    if (FAILED(compile_hr) || !result) {
        return MemoryLayout{.name = is_cbuffer ? target.cbuffer_name : target.type_name,
                            .type = is_cbuffer ? "cbuffer" : target.type_name,
                            .kind = is_cbuffer ? MemoryLayoutKind::constant_buffer
                                               : MemoryLayoutKind::natural,
                            .supported = false,
                            .explanation = "DXC probe compilation failed"};
    }

    HRESULT status{};
    check(result->GetStatus(&status), "GetStatus");
    if (FAILED(status)) {
        // Extract error text for a useful diagnostic.
        LocalComPtr<IDxcBlobEncoding> errors;
        result->GetErrorBuffer(errors.put());
        std::string error_text = "DXC probe compilation error";
        if (errors && errors->GetBufferSize() > 0) {
            error_text = std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize());
            // Trim trailing whitespace.
            while (!error_text.empty() && (error_text.back() == '\n' || error_text.back() == '\r' ||
                                           error_text.back() == '\0')) {
                error_text.pop_back();
            }
        }
        return MemoryLayout{.name = is_cbuffer ? target.cbuffer_name : target.type_name,
                            .type = is_cbuffer ? "cbuffer" : target.type_name,
                            .kind = is_cbuffer ? MemoryLayoutKind::constant_buffer
                                               : MemoryLayoutKind::natural,
                            .supported = false,
                            .explanation = std::move(error_text)};
    }

    // Step 6: Extract reflection data from the DXIL container.
    LocalComPtr<IDxcBlob> object_blob;
    check(result->GetResult(object_blob.put()), "GetResult");
    if (!object_blob || object_blob->GetBufferSize() == 0) {
        return MemoryLayout{.name = is_cbuffer ? target.cbuffer_name : target.type_name,
                            .type = is_cbuffer ? "cbuffer" : target.type_name,
                            .kind = is_cbuffer ? MemoryLayoutKind::constant_buffer
                                               : MemoryLayoutKind::natural,
                            .supported = false,
                            .explanation = "DXC did not produce compiled output"};
    }

    DxcBuffer object_buffer{.Ptr = object_blob->GetBufferPointer(),
                            .Size = object_blob->GetBufferSize(),
                            .Encoding = 0};

    LocalComPtr<IShaderReflection> reflection;
    check(utils->CreateReflection(&object_buffer, kIID_ShaderReflection, reflection.put_void()),
          "CreateReflection");

    // Step 7: Walk reflection to build MemoryLayout.
    MemoryLayout layout;
    WalkContext walk_ctx;
    walk_ctx.is_cbuffer = is_cbuffer;

    if (is_cbuffer) {
        // Find the cbuffer by iterating (avoids sentinel object issues).
        ShaderDesc shader_desc{};
        check(reflection->GetDesc(&shader_desc), "GetDesc(shader)");

        IShaderReflectionConstantBuffer* cb = nullptr;
        ShaderBufferDesc buf_desc{};
        for (unsigned i = 0; i < shader_desc.ConstantBuffers; ++i) {
            auto* candidate = reflection->GetConstantBufferByIndex(i);
            if (candidate == nullptr)
                continue;
            ShaderBufferDesc candidate_desc{};
            if (FAILED(candidate->GetDesc(&candidate_desc)))
                continue;
            if (candidate_desc.Type == CT_CBUFFER && candidate_desc.Name != nullptr &&
                target.cbuffer_name == candidate_desc.Name) {
                cb = candidate;
                buf_desc = candidate_desc;
                break;
            }
        }

        if (cb == nullptr) {
            return MemoryLayout{.name = target.cbuffer_name,
                                .type = "cbuffer",
                                .kind = MemoryLayoutKind::constant_buffer,
                                .supported = false,
                                .explanation = "Cbuffer '" + target.cbuffer_name +
                                               "' not found in reflection"};
        }

        layout.name = target.cbuffer_name;
        layout.type = "cbuffer";
        layout.kind = MemoryLayoutKind::constant_buffer;

        // Walk each variable in the cbuffer.
        for (unsigned i = 0; i < buf_desc.Variables; ++i) {
            auto* var = cb->GetVariableByIndex(i);
            if (var == nullptr)
                continue;

            ShaderVariableDesc var_desc{};
            if (FAILED(var->GetDesc(&var_desc)))
                continue;

            auto* var_type = var->GetType();
            if (var_type == nullptr)
                continue;

            ShaderTypeDesc type_desc{};
            if (FAILED(var_type->GetDesc(&type_desc)))
                continue;

            if (walk_ctx.remaining == 0) {
                layout.supported = false;
                layout.explanation = "Layout expands to more than 4096 elements";
                return layout;
            }
            --walk_ctx.remaining;

            MemoryLayoutElement element;
            element.name = var_desc.Name != nullptr ? std::string{var_desc.Name} : "";
            element.type = type_spelling(type_desc);
            element.offset = var_desc.StartOffset;
            element.size = var_desc.Size;

            if (type_desc.Elements > 0) {
                // Array variable.
                element.kind = MemoryLayoutElementKind::array;
                const auto count = type_desc.Elements;
                // For cbuffer arrays, DXC reports the tight size (without
                // trailing padding on the last element). The stride between
                // elements is always a multiple of 16 in cbuffer layout.
                // Compute: stride = ceil(size/count) rounded up to 16.
                const std::uint32_t raw_elem_size =
                    (count > 0) ? (var_desc.Size + count - 1) / count : var_desc.Size;
                const std::uint32_t array_stride = ((raw_elem_size + 15) / 16) * 16;
                element.array_stride = array_stride;
                element.size = array_stride * count;
                element.array_dimensions.push_back(count);

                // Get the element type description (same type without array).
                ShaderTypeDesc elem_desc = type_desc;
                elem_desc.Elements = 0;
                elem_desc.Offset = 0;

                // Build element prototype.
                MemoryLayoutElement proto;
                proto.type = element.type;
                proto.kind = map_element_kind(elem_desc.Class);
                proto.size = array_stride;
                proto.row_major = (elem_desc.Class == SVC_MATRIX_ROWS);

                if (elem_desc.Class == SVC_STRUCT) {
                    walk_struct_members(var_type, elem_desc, 0, proto.members, walk_ctx);
                    if (!walk_ctx.error.empty()) {
                        layout.supported = false;
                        layout.explanation = walk_ctx.error;
                        return layout;
                    }
                    fix_sizes(proto.members, proto.size, is_cbuffer);
                }

                if (elem_desc.Class == SVC_MATRIX_ROWS || elem_desc.Class == SVC_MATRIX_COLUMNS) {
                    bool rm = (elem_desc.Class == SVC_MATRIX_ROWS);
                    unsigned vectors = rm ? elem_desc.Rows : elem_desc.Columns;
                    unsigned components = rm ? elem_desc.Columns : elem_desc.Rows;
                    std::uint32_t vec_size = scalar_byte_size(elem_desc.Type) * components;
                    std::uint32_t matrix_stride = (vectors > 1 && is_cbuffer) ? 16 : vec_size;
                    proto.matrix_stride = matrix_stride;
                    proto.members.clear();
                    for (unsigned v = 0; v < vectors; ++v) {
                        if (walk_ctx.remaining == 0) {
                            layout.supported = false;
                            layout.explanation = "Layout expands to more than 4096 elements";
                            return layout;
                        }
                        --walk_ctx.remaining;
                        proto.members.push_back(
                            MemoryLayoutElement{.name = "[" + std::to_string(v) + "]",
                                                .type = element.type,
                                                .kind = MemoryLayoutElementKind::vector,
                                                .offset = matrix_stride * v,
                                                .size = vec_size,
                                                .alignment = scalar_byte_size(elem_desc.Type),
                                                .array_index = v});
                    }
                }

                element.members.reserve(count);
                for (unsigned idx = 0; idx < count; ++idx) {
                    if (walk_ctx.remaining == 0) {
                        layout.supported = false;
                        layout.explanation = "Layout expands to more than 4096 elements";
                        return layout;
                    }
                    --walk_ctx.remaining;
                    auto elem = proto;
                    elem.name = "[" + std::to_string(idx) + "]";
                    elem.offset = array_stride * idx;
                    elem.array_index = idx;
                    element.members.push_back(std::move(elem));
                }
            } else {
                element.kind = map_element_kind(type_desc.Class);
                element.row_major = (type_desc.Class == SVC_MATRIX_ROWS);

                if (type_desc.Class == SVC_STRUCT) {
                    walk_struct_members(var_type, type_desc, 0, element.members, walk_ctx);
                    if (!walk_ctx.error.empty()) {
                        layout.supported = false;
                        layout.explanation = walk_ctx.error;
                        return layout;
                    }
                    fix_sizes(element.members, element.size, is_cbuffer);
                }

                if (type_desc.Class == SVC_MATRIX_ROWS || type_desc.Class == SVC_MATRIX_COLUMNS) {
                    bool rm = (type_desc.Class == SVC_MATRIX_ROWS);
                    unsigned vectors = rm ? type_desc.Rows : type_desc.Columns;
                    unsigned components = rm ? type_desc.Columns : type_desc.Rows;
                    std::uint32_t vec_size = scalar_byte_size(type_desc.Type) * components;
                    std::uint32_t matrix_stride = (vectors > 1 && is_cbuffer) ? 16 : vec_size;
                    element.matrix_stride = matrix_stride;
                    for (unsigned v = 0; v < vectors; ++v) {
                        if (walk_ctx.remaining == 0) {
                            layout.supported = false;
                            layout.explanation = "Layout expands to more than 4096 elements";
                            return layout;
                        }
                        --walk_ctx.remaining;
                        element.members.push_back(
                            MemoryLayoutElement{.name = "[" + std::to_string(v) + "]",
                                                .type = element.type,
                                                .kind = MemoryLayoutElementKind::vector,
                                                .offset = matrix_stride * v,
                                                .size = vec_size,
                                                .alignment = scalar_byte_size(type_desc.Type),
                                                .array_index = v});
                    }
                }
            }

            // Infer alignment from the variable offset within the cbuffer.
            if (element.offset > 0) {
                element.alignment = infer_alignment_from_stride(element.offset);
            } else if (element.kind == MemoryLayoutElementKind::scalar ||
                       element.kind == MemoryLayoutElementKind::vector) {
                element.alignment = scalar_byte_size(type_desc.Type);
            } else {
                element.alignment = 16;
            }

            layout.members.push_back(std::move(element));
        }

        layout.allocation_size = buf_desc.Size;
        // Cbuffer 'size' is the end of the last member (data size without final padding).
        if (!layout.members.empty()) {
            const auto& last = layout.members.back();
            layout.size = last.offset + last.size;
        } else {
            layout.size = buf_desc.Size;
        }
        layout.alignment = 16;
    } else {
        // Struct natural layout via StructuredBuffer probe.
        // The StructuredBuffer appears in reflection as a constant buffer
        // with type D3D_CT_RESOURCE_BIND_INFO.
        ShaderDesc shader_desc{};
        check(reflection->GetDesc(&shader_desc), "GetDesc(shader)");

        IShaderReflectionConstantBuffer* cb = nullptr;
        ShaderBufferDesc buf_desc{};
        for (unsigned i = 0; i < shader_desc.ConstantBuffers; ++i) {
            auto* candidate = reflection->GetConstantBufferByIndex(i);
            if (candidate == nullptr)
                continue;
            ShaderBufferDesc candidate_desc{};
            if (FAILED(candidate->GetDesc(&candidate_desc)))
                continue;
            if (candidate_desc.Type == CT_RESOURCE_BIND_INFO) {
                cb = candidate;
                buf_desc = candidate_desc;
                break;
            }
        }

        if (cb == nullptr) {
            return MemoryLayout{.name = target.type_name,
                                .type = target.type_name,
                                .kind = MemoryLayoutKind::natural,
                                .supported = false,
                                .explanation = "StructuredBuffer probe not found in reflection"};
        }

        if (buf_desc.Variables < 1) {
            return MemoryLayout{.name = target.type_name,
                                .type = target.type_name,
                                .kind = MemoryLayoutKind::natural,
                                .supported = false,
                                .explanation = "StructuredBuffer element type has no variables"};
        }

        auto* element_var = cb->GetVariableByIndex(0);
        if (element_var == nullptr) {
            return MemoryLayout{.name = target.type_name,
                                .type = target.type_name,
                                .kind = MemoryLayoutKind::natural,
                                .supported = false,
                                .explanation =
                                    "Unable to retrieve StructuredBuffer element variable"};
        }

        ShaderVariableDesc element_var_desc{};
        check(element_var->GetDesc(&element_var_desc), "GetDesc(element var)");

        auto* element_type = element_var->GetType();
        if (element_type == nullptr) {
            return MemoryLayout{.name = target.type_name,
                                .type = target.type_name,
                                .kind = MemoryLayoutKind::natural,
                                .supported = false,
                                .explanation = "Unable to retrieve StructuredBuffer element type"};
        }

        ShaderTypeDesc element_type_desc{};
        check(element_type->GetDesc(&element_type_desc), "GetDesc(element type)");

        layout.name = target.type_name;
        layout.type = target.type_name;
        layout.kind = MemoryLayoutKind::natural;
        layout.size = element_var_desc.Size;
        layout.allocation_size = buf_desc.Size;

        if (element_type_desc.Class == SVC_STRUCT) {
            walk_struct_members(element_type, element_type_desc, 0, layout.members, walk_ctx);
            if (!walk_ctx.error.empty()) {
                layout.supported = false;
                layout.explanation = walk_ctx.error;
                return layout;
            }
            fix_sizes(layout.members, layout.size, false);
        }

        layout.alignment = infer_struct_alignment(layout.members, layout.allocation_size, false);
    }

    // Step 8: Set selected member info.
    if (!target.selected_field.empty()) {
        const auto member =
            std::ranges::find(layout.members, target.selected_field, &MemoryLayoutElement::name);
        if (member != layout.members.end()) {
            layout.selected_name = member->name;
            layout.selected_type = member->type;
            layout.selected_size = member->size;
            layout.selected_alignment = member->alignment;
            if (is_cbuffer) {
                layout.packed_offset = member->offset;
            }
        }
    }
    if (layout.selected_name.empty()) {
        layout.selected_name = layout.name;
        layout.selected_type = layout.type;
        layout.selected_size = layout.size;
        layout.selected_alignment = layout.alignment;
        if (is_cbuffer) {
            layout.packed_offset = 0;
        }
    }

    layout.supported = true;
    return layout;
}

} // namespace hlsl_intellisense::dxc::detail
