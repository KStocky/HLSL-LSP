#include "compatibility.h"

#include "resource_binding_analysis.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace hlsl_intellisense::dxc::detail {

namespace {

// Maps the compiled entry point's stage to the D3D12 shader-visibility value
// a root-signature entry must match (or be D3D12_SHADER_VISIBILITY_ALL) to
// apply to it. Compute root signatures are required by Direct3D 12 to use
// ALL exclusively, so "compute" maps to `all` rather than a dedicated
// compute value (D3D12_SHADER_VISIBILITY has none). Stages this server
// cannot confidently map (e.g. "library", or any future/unknown stage) fall
// back to `std::nullopt`, which disables visibility filtering entirely
// rather than risk a wrong mapping.
[[nodiscard]] std::optional<RootSignatureVisibility> stage_to_visibility(std::string_view stage) {
    if (stage == "vertex")
        return RootSignatureVisibility::vertex;
    if (stage == "pixel")
        return RootSignatureVisibility::pixel;
    if (stage == "geometry")
        return RootSignatureVisibility::geometry;
    if (stage == "hull")
        return RootSignatureVisibility::hull;
    if (stage == "domain")
        return RootSignatureVisibility::domain;
    if (stage == "mesh")
        return RootSignatureVisibility::mesh;
    if (stage == "amplification")
        return RootSignatureVisibility::amplification;
    if (stage == "compute")
        return RootSignatureVisibility::all;
    return std::nullopt;
}

[[nodiscard]] bool visibility_active(RootSignatureVisibility entry_visibility,
                                     const std::optional<RootSignatureVisibility>& active_stage) {
    if (!active_stage.has_value()) {
        return true;
    }
    return entry_visibility == RootSignatureVisibility::all || entry_visibility == *active_stage;
}

[[nodiscard]] ResourceRegisterClass to_resource_register_class(RootSignatureRangeType type) {
    switch (type) {
    case RootSignatureRangeType::cbv:
        return ResourceRegisterClass::cbv;
    case RootSignatureRangeType::srv:
        return ResourceRegisterClass::srv;
    case RootSignatureRangeType::uav:
        return ResourceRegisterClass::uav;
    case RootSignatureRangeType::sampler:
        return ResourceRegisterClass::sampler;
    default:
        return ResourceRegisterClass::unknown;
    }
}

// A single register-range provider extracted from the root signature: a
// descriptor-table range, a root descriptor, root constants (which behave
// like a single-register CBV for coverage purposes), or a static sampler.
struct Provider {
    ResourceRegisterClass register_class{ResourceRegisterClass::unknown};
    std::uint32_t space{};
    std::uint32_t base_register{};
    bool unbounded{};
    std::uint32_t end_register{}; // valid only when !unbounded
    RootSignatureVisibility visibility{RootSignatureVisibility::all};
};

[[nodiscard]] std::vector<Provider> collect_providers(const RootSignatureDetails& details) {
    std::vector<Provider> providers;
    for (const auto& parameter : details.parameters) {
        switch (parameter.kind) {
        case RootSignatureParameterKind::descriptor_table:
            for (const auto& range : parameter.descriptor_table_ranges) {
                providers.push_back(Provider{
                    .register_class = to_resource_register_class(range.type),
                    .space = range.space,
                    .base_register = range.base_register,
                    .unbounded = range.unbounded,
                    .end_register = range.unbounded ? 0U
                                                    : safe_end_register(range.base_register,
                                                                        range.num_descriptors),
                    .visibility = parameter.visibility});
            }
            break;
        case RootSignatureParameterKind::constants:
            if (parameter.constants.has_value()) {
                providers.push_back(Provider{.register_class = ResourceRegisterClass::cbv,
                                             .space = parameter.constants->space,
                                             .base_register = parameter.constants->shader_register,
                                             .unbounded = false,
                                             .end_register = parameter.constants->shader_register,
                                             .visibility = parameter.visibility});
            }
            break;
        case RootSignatureParameterKind::root_descriptor:
            if (parameter.root_descriptor.has_value()) {
                providers.push_back(Provider{
                    .register_class = to_resource_register_class(parameter.root_descriptor->type),
                    .space = parameter.root_descriptor->space,
                    .base_register = parameter.root_descriptor->shader_register,
                    .unbounded = false,
                    .end_register = parameter.root_descriptor->shader_register,
                    .visibility = parameter.visibility});
            }
            break;
        }
    }
    for (const auto& sampler : details.static_samplers) {
        providers.push_back(Provider{.register_class = ResourceRegisterClass::sampler,
                                     .space = sampler.space,
                                     .base_register = sampler.shader_register,
                                     .unbounded = false,
                                     .end_register = sampler.shader_register,
                                     .visibility = sampler.visibility});
    }
    return providers;
}

} // namespace

CompilationCompatibility
analyze_compatibility(const std::vector<CompilationResourceBinding>& resources,
                      const RootSignatureInfo& root_signature, std::string_view stage) {
    CompilationCompatibility result;

    switch (root_signature.availability) {
    case RootSignatureAvailability::absent:
        result.status = ResourceCompatibilityStatus::unknown;
        result.explanation =
            "No embedded root signature is present to compare against reflected resources.";
        return result;
    case RootSignatureAvailability::not_applicable:
        result.status = ResourceCompatibilityStatus::unknown;
        result.explanation = "Root signature compatibility does not apply to this compilation "
                             "target (for example, SPIR-V).";
        return result;
    case RootSignatureAvailability::present_details_unavailable:
        result.status = ResourceCompatibilityStatus::unknown;
        result.explanation = "An embedded root signature is present, but its details are "
                             "unavailable on this platform, so compatibility cannot be "
                             "determined.";
        return result;
    case RootSignatureAvailability::present:
        break;
    }
    if (!root_signature.details.has_value()) {
        result.status = ResourceCompatibilityStatus::unknown;
        result.explanation =
            "An embedded root signature is reported present, but no details were captured.";
        return result;
    }

    const auto providers = collect_providers(*root_signature.details);
    const auto active_stage = stage_to_visibility(stage);

    bool any_incompatible = false;
    for (const auto& resource : resources) {
        if (resource.system_reserved_space) {
            // Compiler/driver-internal; never user-addressable, so it is
            // never something a root signature is expected to cover.
            continue;
        }

        const std::uint64_t resource_end =
            resource.unbounded ? std::numeric_limits<std::uint32_t>::max()
                               : safe_end_register(resource.bind_point, resource.bind_count);

        bool fully_covered_active = false;
        bool fully_covered_wrong_visibility = false;
        bool partially_overlaps = false;

        for (const auto& provider : providers) {
            if (provider.register_class != resource.register_class ||
                provider.space != resource.space) {
                continue;
            }

            if (resource.unbounded) {
                // Empirically confirmed against DXC's own container
                // validator (see out/scratch/compat_probe.cpp history):
                // a shader-side unbounded array is accepted when covered by
                // a *bounded* root-signature range too, not only an
                // unbounded one. Direct3D 12 places no obligation on the
                // root signature to numerically bound how far an unbounded
                // array may be indexed at runtime -- that is the
                // application's responsibility -- so the only provable
                // requirement is that the resource's base register actually
                // falls within the provider's range: the provider must
                // start at or before it, and -- unless the provider is
                // itself unbounded -- must also end at or after it. A
                // bounded provider that starts before the base register but
                // ends before it too does not contain the base register at
                // all, and must not be treated as coverage.
                const bool contains_base_register =
                    provider.base_register <= resource.bind_point &&
                    (provider.unbounded || provider.end_register >= resource.bind_point);
                if (contains_base_register) {
                    if (visibility_active(provider.visibility, active_stage)) {
                        fully_covered_active = true;
                    } else {
                        fully_covered_wrong_visibility = true;
                    }
                }
                continue;
            }

            const std::uint64_t provider_end = provider.unbounded
                                                   ? std::numeric_limits<std::uint32_t>::max()
                                                   : provider.end_register;
            const bool covers_fully =
                provider.base_register <= resource.bind_point && provider_end >= resource_end;
            const bool overlaps =
                provider.base_register <= resource_end && resource.bind_point <= provider_end;

            if (covers_fully) {
                if (visibility_active(provider.visibility, active_stage)) {
                    fully_covered_active = true;
                } else {
                    fully_covered_wrong_visibility = true;
                }
            } else if (overlaps) {
                partially_overlaps = true;
            }
        }

        if (fully_covered_active) {
            continue;
        }

        any_incompatible = true;
        if (fully_covered_wrong_visibility) {
            result.issues.push_back(ResourceCompatibilityIssue{
                .resource_name = resource.name,
                .register_class = resource.register_class,
                .space = resource.space,
                .message = "'" + resource.name +
                           "' is covered by a root signature entry whose shader visibility "
                           "excludes the '" +
                           std::string{stage} + "' stage"});
        } else if (partially_overlaps) {
            result.issues.push_back(ResourceCompatibilityIssue{
                .resource_name = resource.name,
                .register_class = resource.register_class,
                .space = resource.space,
                .message = "'" + resource.name +
                           "' is only partially covered by an overlapping root signature entry"});
        } else {
            result.issues.push_back(ResourceCompatibilityIssue{
                .resource_name = resource.name,
                .register_class = resource.register_class,
                .space = resource.space,
                .message = "'" + resource.name + "' has no corresponding root signature entry"});
        }
    }

    result.status = any_incompatible ? ResourceCompatibilityStatus::incompatible
                                     : ResourceCompatibilityStatus::compatible;
    return result;
}

} // namespace hlsl_intellisense::dxc::detail
