#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include "WinAdapter.h"
#endif

#include <dxcapi.h>

namespace hlsl_intellisense::dxc::detail {

// Extracts the embedded root-signature result for a single compiled DXIL (or
// SPIR-V) object. Detects presence/absence purely through
// IDxcUtils::GetDxilContainerPart(DXC_PART_ROOT_SIGNATURE), which works on
// every platform DXC supports. On Windows, a present root signature is
// deserialized using the official
// D3D12CreateVersionedRootSignatureDeserializer /
// ID3D12VersionedRootSignatureDeserializer API - never a custom RTS0 binary
// parser. On other platforms, or if the Windows D3D12 runtime cannot be
// loaded, a present root signature is reported with
// RootSignatureAvailability::present_details_unavailable and an explicit
// reason, never guessed at. `is_spirv` short-circuits to `not_applicable`
// without ever calling GetDxilContainerPart, since SPIR-V has no root
// signature concept.
[[nodiscard]] RootSignatureInfo
extract_root_signature(IDxcUtils* utils, const DxcBuffer& object_buffer, bool is_spirv);

} // namespace hlsl_intellisense::dxc::detail
