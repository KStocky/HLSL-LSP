#include "root_signature.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <d3d12.h>
#endif

namespace hlsl_intellisense::dxc::detail {

namespace {

#ifdef _WIN32

[[nodiscard]] RootSignatureVisibility to_visibility(D3D12_SHADER_VISIBILITY visibility) {
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_ALL:
        return RootSignatureVisibility::all;
    case D3D12_SHADER_VISIBILITY_VERTEX:
        return RootSignatureVisibility::vertex;
    case D3D12_SHADER_VISIBILITY_HULL:
        return RootSignatureVisibility::hull;
    case D3D12_SHADER_VISIBILITY_DOMAIN:
        return RootSignatureVisibility::domain;
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
        return RootSignatureVisibility::geometry;
    case D3D12_SHADER_VISIBILITY_PIXEL:
        return RootSignatureVisibility::pixel;
    case D3D12_SHADER_VISIBILITY_AMPLIFICATION:
        return RootSignatureVisibility::amplification;
    case D3D12_SHADER_VISIBILITY_MESH:
        return RootSignatureVisibility::mesh;
    default:
        return RootSignatureVisibility::unknown;
    }
}

[[nodiscard]] RootSignatureRangeType to_range_type(D3D12_DESCRIPTOR_RANGE_TYPE type) {
    switch (type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
        return RootSignatureRangeType::srv;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
        return RootSignatureRangeType::uav;
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
        return RootSignatureRangeType::cbv;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
        return RootSignatureRangeType::sampler;
    default:
        return RootSignatureRangeType::unknown;
    }
}

[[nodiscard]] RootSignatureRangeType to_root_descriptor_type(D3D12_ROOT_PARAMETER_TYPE type) {
    switch (type) {
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
        return RootSignatureRangeType::cbv;
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
        return RootSignatureRangeType::srv;
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
        return RootSignatureRangeType::uav;
    default:
        return RootSignatureRangeType::unknown;
    }
}

// Loads d3d12.dll for the lifetime of this object only; the DXC-authored
// root signature is deserialized once per compilation_info() call, so the
// module is loaded and released around that single use rather than kept
// resident, mirroring how InMemoryIncludeHandler and other single-use
// helpers in this codebase are scoped.
class ScopedD3D12Module final {
  public:
    ScopedD3D12Module() : handle_{::LoadLibraryW(L"d3d12.dll")} {}
    ScopedD3D12Module(const ScopedD3D12Module&) = delete;
    auto operator=(const ScopedD3D12Module&) -> ScopedD3D12Module& = delete;
    ScopedD3D12Module(ScopedD3D12Module&&) = delete;
    auto operator=(ScopedD3D12Module&&) -> ScopedD3D12Module& = delete;
    ~ScopedD3D12Module() {
        if (handle_ != nullptr) {
            ::FreeLibrary(handle_);
        }
    }

    [[nodiscard]] auto get() const noexcept -> HMODULE { return handle_; }

  private:
    HMODULE handle_{};
};

// Releases a raw ID3D12VersionedRootSignatureDeserializer* on scope exit;
// deliberately minimal (this file has exactly one COM pointer to manage) so
// it does not need to share LocalComPtr with compilation_info.cpp.
class ScopedDeserializer final {
  public:
    explicit ScopedDeserializer(ID3D12VersionedRootSignatureDeserializer* pointer)
        : pointer_{pointer} {}
    ScopedDeserializer(const ScopedDeserializer&) = delete;
    auto operator=(const ScopedDeserializer&) -> ScopedDeserializer& = delete;
    ScopedDeserializer(ScopedDeserializer&&) = delete;
    auto operator=(ScopedDeserializer&&) -> ScopedDeserializer& = delete;
    ~ScopedDeserializer() {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }

    [[nodiscard]] auto get() const noexcept -> ID3D12VersionedRootSignatureDeserializer* {
        return pointer_;
    }
    [[nodiscard]] auto operator->() const noexcept -> ID3D12VersionedRootSignatureDeserializer* {
        return pointer_;
    }

  private:
    ID3D12VersionedRootSignatureDeserializer* pointer_;
};

// IDxcUtils::GetDxilContainerPart returns only the bare RTS0 payload (the
// bytes after the part's FourCC+size header), stripped of the outer DXBC
// container. D3D12CreateVersionedRootSignatureDeserializer requires the
// full container (empirically confirmed: it rejects the bare payload with
// E_INVALIDARG), so this rebuilds a minimal, single-part synthetic
// container around it. The 16-byte checksum is left zeroed; the deserializer
// was empirically confirmed not to validate it.
[[nodiscard]] std::vector<std::uint8_t> wrap_as_dxbc_container(const void* payload,
                                                               std::uint32_t payload_size) {
    constexpr std::uint32_t header_size =
        32; // "DXBC"(4) + checksum(16) + version(4) + totalSize(4) + partCount(4)
    constexpr std::uint32_t part_offset = header_size + 4; // + one part-offset table entry(4)
    const std::uint32_t total_size =
        part_offset + 4 + 4 + payload_size; // FourCC(4) + size(4) + payload

    std::vector<std::uint8_t> container(total_size, 0);
    std::memcpy(container.data(), "DXBC", 4);
    const std::uint32_t version = 1;
    std::memcpy(container.data() + 20, &version, sizeof(version));
    std::memcpy(container.data() + 24, &total_size, sizeof(total_size));
    const std::uint32_t part_count = 1;
    std::memcpy(container.data() + 28, &part_count, sizeof(part_count));
    std::memcpy(container.data() + 32, &part_offset, sizeof(part_offset));
    std::memcpy(container.data() + part_offset, "RTS0", 4);
    std::memcpy(container.data() + part_offset + 4, &payload_size, sizeof(payload_size));
    if (payload_size > 0) {
        std::memcpy(container.data() + part_offset + 8, payload, payload_size);
    }
    return container;
}

[[nodiscard]] RootSignatureDetails convert_desc_1_1(const D3D12_ROOT_SIGNATURE_DESC1& desc,
                                                    std::string version) {
    RootSignatureDetails details;
    details.version = std::move(version);
    details.raw_flags = static_cast<std::uint32_t>(desc.Flags);
    details.cbv_srv_uav_heap_directly_indexed =
        (desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) != 0;
    details.sampler_heap_directly_indexed =
        (desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED) != 0;

    details.parameters.reserve(desc.NumParameters);
    for (UINT index = 0; index < desc.NumParameters; ++index) {
        const auto& parameter = desc.pParameters[index];
        RootSignatureParameter out;
        out.visibility = to_visibility(parameter.ShaderVisibility);
        if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            out.kind = RootSignatureParameterKind::descriptor_table;
            out.descriptor_table_ranges.reserve(parameter.DescriptorTable.NumDescriptorRanges);
            for (UINT range_index = 0; range_index < parameter.DescriptorTable.NumDescriptorRanges;
                 ++range_index) {
                const auto& range = parameter.DescriptorTable.pDescriptorRanges[range_index];
                const bool unbounded = range.NumDescriptors == UINT_MAX;
                out.descriptor_table_ranges.push_back(RootSignatureDescriptorRange{
                    .type = to_range_type(range.RangeType),
                    .num_descriptors = unbounded ? 0U : range.NumDescriptors,
                    .unbounded = unbounded,
                    .base_register = range.BaseShaderRegister,
                    .space = range.RegisterSpace,
                    .raw_flags = static_cast<std::uint32_t>(range.Flags),
                    .offset_in_descriptors_from_table_start =
                        range.OffsetInDescriptorsFromTableStart});
            }
        } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            out.kind = RootSignatureParameterKind::constants;
            out.constants =
                RootSignatureRootConstants{.shader_register = parameter.Constants.ShaderRegister,
                                           .space = parameter.Constants.RegisterSpace,
                                           .num_32bit_values = parameter.Constants.Num32BitValues};
        } else {
            out.kind = RootSignatureParameterKind::root_descriptor;
            out.root_descriptor = RootSignatureRootDescriptor{
                .type = to_root_descriptor_type(parameter.ParameterType),
                .shader_register = parameter.Descriptor.ShaderRegister,
                .space = parameter.Descriptor.RegisterSpace,
                .raw_flags = static_cast<std::uint32_t>(parameter.Descriptor.Flags)};
        }
        details.parameters.push_back(std::move(out));
    }

    details.static_samplers.reserve(desc.NumStaticSamplers);
    for (UINT index = 0; index < desc.NumStaticSamplers; ++index) {
        const auto& sampler = desc.pStaticSamplers[index];
        details.static_samplers.push_back(RootSignatureStaticSampler{
            .shader_register = sampler.ShaderRegister,
            .space = sampler.RegisterSpace,
            .visibility = to_visibility(sampler.ShaderVisibility),
            .filter = static_cast<std::uint32_t>(sampler.Filter),
            .address_u = static_cast<std::uint32_t>(sampler.AddressU),
            .address_v = static_cast<std::uint32_t>(sampler.AddressV),
            .address_w = static_cast<std::uint32_t>(sampler.AddressW),
            .mip_lod_bias = sampler.MipLODBias,
            .max_anisotropy = sampler.MaxAnisotropy,
            .comparison_func = static_cast<std::uint32_t>(sampler.ComparisonFunc),
            .border_color = static_cast<std::uint32_t>(sampler.BorderColor),
            .min_lod = sampler.MinLOD,
            .max_lod = sampler.MaxLOD});
    }
    return details;
}

#endif // _WIN32

} // namespace

RootSignatureInfo extract_root_signature(IDxcUtils* utils, const DxcBuffer& object_buffer,
                                         bool is_spirv) {
    if (is_spirv) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::not_applicable,
            .unavailable_reason =
                "SPIR-V output has no root signature concept; root signatures are a Direct3D 12 "
                "binding-model construct",
            .details = std::nullopt};
    }

    void* part_data = nullptr;
    UINT32 part_size = 0;
    const HRESULT part_hr = utils->GetDxilContainerPart(&object_buffer, DXC_PART_ROOT_SIGNATURE,
                                                        &part_data, &part_size);
    if (FAILED(part_hr) || part_data == nullptr || part_size == 0) {
        return RootSignatureInfo{.availability = RootSignatureAvailability::absent,
                                 .unavailable_reason = {},
                                 .details = std::nullopt};
    }

#ifdef _WIN32
    const auto container = wrap_as_dxbc_container(part_data, part_size);

    const ScopedD3D12Module module;
    if (module.get() == nullptr) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::present_details_unavailable,
            .unavailable_reason = "An embedded root signature is present, but the Windows D3D12 "
                                  "runtime (d3d12.dll) could not be loaded to deserialize it",
            .details = std::nullopt};
    }
    const auto create_deserializer =
        reinterpret_cast<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(
            ::GetProcAddress(module.get(), "D3D12CreateVersionedRootSignatureDeserializer"));
    if (create_deserializer == nullptr) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::present_details_unavailable,
            .unavailable_reason =
                "An embedded root signature is present, but this system's D3D12 runtime does not "
                "export D3D12CreateVersionedRootSignatureDeserializer",
            .details = std::nullopt};
    }

    ID3D12VersionedRootSignatureDeserializer* raw_deserializer = nullptr;
    const HRESULT deserialize_hr =
        create_deserializer(container.data(), static_cast<SIZE_T>(container.size()),
                            __uuidof(ID3D12VersionedRootSignatureDeserializer),
                            reinterpret_cast<void**>(&raw_deserializer));
    if (FAILED(deserialize_hr) || raw_deserializer == nullptr) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::present_details_unavailable,
            .unavailable_reason =
                "An embedded root signature is present, but the Windows D3D12 runtime rejected it "
                "with HRESULT " +
                std::to_string(static_cast<unsigned long>(deserialize_hr)),
            .details = std::nullopt};
    }
    const ScopedDeserializer deserializer{raw_deserializer};

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* unconverted =
        deserializer->GetUnconvertedRootSignatureDesc();
    if (unconverted == nullptr) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::present_details_unavailable,
            .unavailable_reason = "An embedded root signature is present, but its contents could "
                                  "not be retrieved from the Windows D3D12 deserializer",
            .details = std::nullopt};
    }

    std::string native_version;
    switch (unconverted->Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0:
        native_version = "1.0";
        break;
    case D3D_ROOT_SIGNATURE_VERSION_1_1:
        native_version = "1.1";
        break;
    default:
        native_version = "unknown";
        break;
    }

    // Always read through the version-1.1 shape for a single, uniform
    // schema: the versioned deserializer losslessly upgrades a version-1.0
    // signature to 1.1 (root descriptor/range flags default to NONE, which
    // is exactly what version 1.0 always meant); `version` above still
    // reports what was actually embedded, so clients are never misled.
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* converted = nullptr;
    const HRESULT convert_hr =
        deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &converted);
    if (FAILED(convert_hr) || converted == nullptr) {
        return RootSignatureInfo{
            .availability = RootSignatureAvailability::present_details_unavailable,
            .unavailable_reason = "An embedded root signature is present (version " +
                                  native_version +
                                  "), but this server could not normalize it to version 1.1 for "
                                  "inspection",
            .details = std::nullopt};
    }

    return RootSignatureInfo{.availability = RootSignatureAvailability::present,
                             .unavailable_reason = {},
                             .details = convert_desc_1_1(converted->Desc_1_1, native_version)};
#else
    return RootSignatureInfo{
        .availability = RootSignatureAvailability::present_details_unavailable,
        .unavailable_reason =
            "An embedded root signature is present, but detailed inspection requires the Windows "
            "D3D12 runtime's ID3D12VersionedRootSignatureDeserializer, which is unavailable on "
            "this platform; only presence could be determined",
        .details = std::nullopt};
#endif
}

} // namespace hlsl_intellisense::dxc::detail
