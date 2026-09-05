// White-box unit tests for the pure compatibility analysis in
// src/dxc/compatibility.cpp. All of the "incompatible" scenarios exercised
// below (a finite root-signature range narrower than a finite
// shader-declared array; a resource entirely missing from the root
// signature; a resource covered only by an entry whose visibility excludes
// the active stage) were empirically confirmed to already be rejected by
// DXC's own compile-time root-signature validation when a root signature is
// attached via [RootSignature(...)] (diagnostic: "... is not fully bound in
// root signature" -- pinned end-to-end in tests/dxc/intellisense_tests.cpp,
// "Compilation reports the compiler's own root-signature validation
// diagnostic..."). They can therefore never reach this analysis through a
// real successful compile. These tests exist as defense-in-depth / semantic
// unit coverage of the analysis logic itself, run directly against
// synthetic RootSignatureInfo/CompilationResourceBinding data, so the
// underlying rules (a finite range must fully and exactly cover a finite
// resource; visibility must include the active stage; a resource must have
// a matching entry at all) are independently verified without depending on
// which paths DXC's own validator happens to also catch.
//
// By contrast, an unbounded shader-declared array covered by a *bounded*
// root-signature range that actually contains its base register was
// empirically confirmed to compile AND validate successfully through pinned
// DXC 1.9.2607.13: this is authoritative confirmation that Direct3D 12 does
// not require the covering range to also be declared unbounded. This
// analysis therefore requires only that some provider's range genuinely
// contains the resource's base register (provider.base_register <=
// bind_point, and provider.end_register >= bind_point when the provider
// itself is bounded) for unbounded resources, and never reports
// incompatibility on that basis alone when containment holds. It is not
// enough for a bounded provider to merely start at or before the base
// register -- a range that also ends before it does not actually contain
// it, and root descriptors/root constants (which occupy exactly one
// register) only ever contain an unbounded resource at that single exact
// register -- see the "unbounded" test cases below, including the negative
// cases that pin both of these.
#include "dxc/compatibility.h"

#include <catch2/catch_test_macros.hpp>

using hlsl_intellisense::dxc::CompilationResourceBinding;
using hlsl_intellisense::dxc::ResourceCompatibilityStatus;
using hlsl_intellisense::dxc::ResourceRegisterClass;
using hlsl_intellisense::dxc::RootSignatureAvailability;
using hlsl_intellisense::dxc::RootSignatureDescriptorRange;
using hlsl_intellisense::dxc::RootSignatureDetails;
using hlsl_intellisense::dxc::RootSignatureInfo;
using hlsl_intellisense::dxc::RootSignatureParameter;
using hlsl_intellisense::dxc::RootSignatureParameterKind;
using hlsl_intellisense::dxc::RootSignatureRangeType;
using hlsl_intellisense::dxc::RootSignatureRootDescriptor;
using hlsl_intellisense::dxc::RootSignatureStaticSampler;
using hlsl_intellisense::dxc::RootSignatureVisibility;
using hlsl_intellisense::dxc::detail::analyze_compatibility;

namespace {

[[nodiscard]] CompilationResourceBinding
make_resource(std::string name, ResourceRegisterClass register_class, std::uint32_t bind_point,
              std::uint32_t bind_count, std::uint32_t space, bool unbounded = false) {
    CompilationResourceBinding resource;
    resource.name = std::move(name);
    resource.register_class = register_class;
    resource.bind_point = bind_point;
    resource.bind_count = bind_count;
    resource.space = space;
    resource.unbounded = unbounded;
    return resource;
}

[[nodiscard]] RootSignatureInfo
present_signature(std::vector<RootSignatureParameter> parameters,
                  std::vector<RootSignatureStaticSampler> static_samplers = {}) {
    RootSignatureDetails details;
    details.version = "1.1";
    details.parameters = std::move(parameters);
    details.static_samplers = std::move(static_samplers);
    return RootSignatureInfo{.availability = RootSignatureAvailability::present,
                             .unavailable_reason = {},
                             .details = std::move(details)};
}

[[nodiscard]] RootSignatureParameter
table_parameter(std::vector<RootSignatureDescriptorRange> ranges,
                RootSignatureVisibility visibility = RootSignatureVisibility::all) {
    RootSignatureParameter parameter;
    parameter.kind = RootSignatureParameterKind::descriptor_table;
    parameter.visibility = visibility;
    parameter.descriptor_table_ranges = std::move(ranges);
    return parameter;
}

[[nodiscard]] RootSignatureParameter
descriptor_parameter(RootSignatureRangeType type, std::uint32_t shader_register,
                     std::uint32_t space,
                     RootSignatureVisibility visibility = RootSignatureVisibility::all) {
    RootSignatureParameter parameter;
    parameter.kind = RootSignatureParameterKind::root_descriptor;
    parameter.visibility = visibility;
    parameter.root_descriptor = RootSignatureRootDescriptor{
        .type = type, .shader_register = shader_register, .space = space};
    return parameter;
}

} // namespace

TEST_CASE("Compatibility analysis reports unknown with an explanation when no root signature is "
          "present",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 1, 0)};
    const auto result =
        analyze_compatibility(resources,
                              RootSignatureInfo{.availability = RootSignatureAvailability::absent,
                                                .unavailable_reason = {},
                                                .details = std::nullopt},
                              "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(result.explanation.empty());
    CHECK(result.issues.empty());
}

TEST_CASE("Compatibility analysis reports unknown for not-applicable root signatures (SPIR-V)",
          "[dxc][compatibility]") {
    const auto result = analyze_compatibility(
        {},
        RootSignatureInfo{.availability = RootSignatureAvailability::not_applicable,
                          .unavailable_reason = {},
                          .details = std::nullopt},
        "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(result.explanation.empty());
}

TEST_CASE("Compatibility analysis reports unknown when root signature details are unavailable "
          "(e.g. non-Windows)",
          "[dxc][compatibility]") {
    const auto result = analyze_compatibility(
        {},
        RootSignatureInfo{.availability = RootSignatureAvailability::present_details_unavailable,
                          .unavailable_reason = {},
                          .details = std::nullopt},
        "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(result.explanation.empty());
}

TEST_CASE("Compatibility analysis reports compatible when a descriptor table range fully covers "
          "a finite resource",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 4, 0)};
    const auto root_signature = present_signature(
        {table_parameter({RootSignatureDescriptorRange{.type = RootSignatureRangeType::srv,
                                                       .num_descriptors = 4,
                                                       .base_register = 0,
                                                       .space = 0}},
                         RootSignatureVisibility::pixel)});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
    CHECK(result.issues.empty());
}

TEST_CASE("Compatibility analysis reports compatible when an unbounded resource is covered by an "
          "unbounded range",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 0, 1, /*unbounded=*/true)};
    const auto root_signature = present_signature({table_parameter({RootSignatureDescriptorRange{
        .type = RootSignatureRangeType::srv, .unbounded = true, .base_register = 0, .space = 1}})});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
}

TEST_CASE("Compatibility analysis reports compatible when an unbounded resource is covered by a "
          "bounded range that covers its base register",
          "[dxc][compatibility]") {
    // Empirically confirmed against DXC's own container validator (a root
    // signature range narrower than an unbounded shader-declared array
    // compiles and validates successfully): Direct3D 12 does not require the
    // covering range to also be declared unbounded, nor to numerically
    // bound how far the array may be indexed at runtime. Only base-register
    // coverage is provable and required here.
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 0, 0, /*unbounded=*/true)};
    const auto root_signature = present_signature(
        {table_parameter({RootSignatureDescriptorRange{.type = RootSignatureRangeType::srv,
                                                       .num_descriptors = 4,
                                                       .base_register = 0,
                                                       .space = 0}})});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
    CHECK(result.issues.empty());
}

TEST_CASE("Compatibility analysis flags an unbounded resource whose base register is not covered "
          "by any root signature range",
          "[dxc][compatibility]") {
    // A root signature range entirely at a higher base register than the
    // unbounded array's own base register can never cover it (this is not
    // the "bounded vs unbounded" question above; it is a plain missing-entry
    // case, provable regardless of unbounded status).
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 0, 0, /*unbounded=*/true)};
    const auto root_signature = present_signature(
        {table_parameter({RootSignatureDescriptorRange{.type = RootSignatureRangeType::srv,
                                                       .num_descriptors = 4,
                                                       .base_register = 4,
                                                       .space = 0}})});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues.front().resource_name == "Tex");
}

TEST_CASE("Compatibility analysis flags an unbounded resource whose base register falls past the "
          "end of a bounded range that starts before it",
          "[dxc][compatibility]") {
    // A bounded provider starting at register 0 with 3 descriptors covers
    // only registers 0-2 (end_register == 2); it does not actually contain
    // an unbounded resource whose base register is 5, even though
    // provider.base_register (0) <= resource.bind_point (5). Requiring only
    // `provider.base_register <= bind_point` without also checking
    // `provider.end_register >= bind_point` (for bounded providers) would
    // wrongly treat this as coverage; this test pins that the base register
    // must actually fall within the provider's own range.
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 5, 0, 0, /*unbounded=*/true)};
    const auto root_signature = present_signature(
        {table_parameter({RootSignatureDescriptorRange{.type = RootSignatureRangeType::srv,
                                                       .num_descriptors = 3,
                                                       .base_register = 0,
                                                       .space = 0}})});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues.front().resource_name == "Tex");
}

TEST_CASE("Compatibility analysis contains an unbounded resource through a root descriptor only "
          "at its own exact register",
          "[dxc][compatibility]") {
    // Root descriptors (and, identically, root constants -- collect_providers
    // gives both an end_register equal to their single base_register and
    // unbounded=false) occupy exactly one register. The base-register
    // containment rule must therefore treat them as covering an unbounded
    // resource only when the resource's base register equals that single
    // register, never merely because the register is numerically lower.
    const auto root_signature =
        present_signature({descriptor_parameter(RootSignatureRangeType::cbv, 0, 0)});

    const std::vector<CompilationResourceBinding> covered{
        make_resource("CB0", ResourceRegisterClass::cbv, 0, 0, 0, /*unbounded=*/true)};
    const auto covered_result = analyze_compatibility(covered, root_signature, "pixel");
    CHECK(covered_result.status == ResourceCompatibilityStatus::compatible);
    CHECK(covered_result.issues.empty());

    const std::vector<CompilationResourceBinding> uncovered{
        make_resource("CB1", ResourceRegisterClass::cbv, 1, 0, 0, /*unbounded=*/true)};
    const auto uncovered_result = analyze_compatibility(uncovered, root_signature, "pixel");
    CHECK(uncovered_result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(uncovered_result.issues.size() == 1);
    CHECK(uncovered_result.issues.front().resource_name == "CB1");
}

TEST_CASE("Compatibility analysis flags a resource only partially covered by a descriptor range",
          "[dxc][compatibility]") {
    // DXC's own compile-time root-signature validation already rejects this
    // exact shape when a root signature is attached via [RootSignature(...)]
    // (pinned "not fully bound in root signature" diagnostic, see
    // tests/dxc/intellisense_tests.cpp), so it is never reachable through a
    // real successful compile. This white-box test exercises the analysis
    // logic directly, as defense-in-depth semantic coverage of the "a finite
    // range must fully cover a finite resource" rule.
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 4, 0)};
    const auto root_signature = present_signature(
        {table_parameter({RootSignatureDescriptorRange{.type = RootSignatureRangeType::srv,
                                                       .num_descriptors = 1,
                                                       .base_register = 0,
                                                       .space = 0}})});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues.front().message.find("partially") != std::string::npos);
}

TEST_CASE("Compatibility analysis flags a resource with no corresponding root signature entry "
          "(synthetic: DXC's own validation already rejects this at compile time in practice)",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Tex", ResourceRegisterClass::srv, 0, 1, 0),
        make_resource("CB", ResourceRegisterClass::cbv, 0, 1, 0)};
    const auto root_signature =
        present_signature({descriptor_parameter(RootSignatureRangeType::cbv, 0, 0)});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues.front().resource_name == "Tex");
    CHECK(result.issues.front().message.find("no corresponding") != std::string::npos);
}

TEST_CASE("Compatibility analysis flags coverage whose visibility excludes the active stage "
          "(synthetic: DXC's own validation already rejects this at compile time in practice)",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("CB", ResourceRegisterClass::cbv, 0, 1, 0)};
    const auto root_signature = present_signature(
        {descriptor_parameter(RootSignatureRangeType::cbv, 0, 0, RootSignatureVisibility::vertex)});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::incompatible);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues.front().message.find("visibility") != std::string::npos);
}

TEST_CASE("Compatibility analysis treats ALL visibility as active for every stage",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("CB", ResourceRegisterClass::cbv, 0, 1, 0)};
    const auto root_signature = present_signature(
        {descriptor_parameter(RootSignatureRangeType::cbv, 0, 0, RootSignatureVisibility::all)});
    const auto result = analyze_compatibility(resources, root_signature, "vertex");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
}

TEST_CASE("Compatibility analysis matches a static sampler resource by register, space, and "
          "visibility",
          "[dxc][compatibility]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Samp", ResourceRegisterClass::sampler, 0, 1, 0)};
    RootSignatureStaticSampler sampler;
    sampler.shader_register = 0;
    sampler.space = 0;
    sampler.visibility = RootSignatureVisibility::pixel;
    const auto root_signature = present_signature({}, {sampler});
    const auto result = analyze_compatibility(resources, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
}

TEST_CASE("Compatibility analysis skips resources in D3D12 system-reserved register spaces",
          "[dxc][compatibility]") {
    CompilationResourceBinding resource =
        make_resource("Hidden", ResourceRegisterClass::uav, 0, 1, 0xfffffff0U);
    resource.system_reserved_space = true;
    const auto root_signature = present_signature({});
    const auto result = analyze_compatibility({resource}, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
    CHECK(result.issues.empty());
}

TEST_CASE("Compatibility analysis reports compatible with no resources and a present root "
          "signature",
          "[dxc][compatibility]") {
    const auto root_signature = present_signature({});
    const auto result = analyze_compatibility({}, root_signature, "pixel");
    CHECK(result.status == ResourceCompatibilityStatus::compatible);
    CHECK(result.issues.empty());
}
