// White-box unit tests for the pure resource-binding grouping/collision
// analysis in src/dxc/resource_binding_analysis.cpp. This is post-processing
// over already-reflected register data (bind_point/bind_count/space/
// unbounded); it never parses HLSL or invokes DXC, so testing it directly
// against synthetic CompilationResourceBinding values (rather than only
// through real compiles) is the correct way to exercise it, including
// scenarios DXC's own front-end never allows to reach reflection (see the
// "distinct resources with genuinely overlapping registers" test below).
#include "dxc/resource_binding_analysis.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

using hlsl_intellisense::dxc::CompilationResourceBinding;
using hlsl_intellisense::dxc::ResourceRegisterClass;
using hlsl_intellisense::dxc::detail::analyze_resource_bindings;
using hlsl_intellisense::dxc::detail::is_system_reserved_space;

namespace {

[[nodiscard]] CompilationResourceBinding
make_resource(std::string name, ResourceRegisterClass register_class, std::uint32_t bind_point,
              std::uint32_t bind_count, std::uint32_t space, bool unbounded = false,
              bool system_reserved_space = false) {
    CompilationResourceBinding resource;
    resource.name = std::move(name);
    resource.register_class = register_class;
    resource.bind_point = bind_point;
    resource.bind_count = bind_count;
    resource.space = space;
    resource.unbounded = unbounded;
    resource.system_reserved_space = system_reserved_space;
    return resource;
}

} // namespace

TEST_CASE("is_system_reserved_space classifies the D3D12 reserved range",
          "[dxc][resource-binding-analysis]") {
    CHECK_FALSE(is_system_reserved_space(0));
    CHECK_FALSE(is_system_reserved_space(0xffffffefU));
    CHECK(is_system_reserved_space(0xfffffff0U));
    CHECK(is_system_reserved_space(0xffffffffU));
}

TEST_CASE("Resource binding analysis groups resources by register class and space",
          "[dxc][resource-binding-analysis]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("TexA", ResourceRegisterClass::srv, 0, 1, 0),
        make_resource("TexB", ResourceRegisterClass::srv, 1, 1, 0),
        make_resource("CBufA", ResourceRegisterClass::cbv, 0, 1, 0),
        make_resource("TexC", ResourceRegisterClass::srv, 0, 1, 1),
    };

    const auto analysis = analyze_resource_bindings(resources);
    REQUIRE(analysis.groups.size() == 3);

    const auto srv_space0 = std::ranges::find_if(analysis.groups, [](const auto& group) {
        return group.register_class == ResourceRegisterClass::srv && group.space == 0;
    });
    REQUIRE(srv_space0 != analysis.groups.end());
    CHECK(srv_space0->ranges.size() == 2);
    CHECK_FALSE(srv_space0->system_reserved_space);

    const auto cbv_space0 = std::ranges::find_if(analysis.groups, [](const auto& group) {
        return group.register_class == ResourceRegisterClass::cbv && group.space == 0;
    });
    REQUIRE(cbv_space0 != analysis.groups.end());
    CHECK(cbv_space0->ranges.size() == 1);

    const auto srv_space1 = std::ranges::find_if(analysis.groups, [](const auto& group) {
        return group.register_class == ResourceRegisterClass::srv && group.space == 1;
    });
    REQUIRE(srv_space1 != analysis.groups.end());
    CHECK(srv_space1->ranges.size() == 1);

    CHECK(analysis.collisions.empty());
}

TEST_CASE("Resource binding analysis represents finite ranges without overflow",
          "[dxc][resource-binding-analysis]") {
    // A resource declaring an implausibly large base register/count must
    // never wrap the computed end register around through unsigned
    // overflow; it must clamp to UINT32_MAX instead. base + count - 1 here
    // is 4294967309, which overflows std::uint32_t (max 4294967295).
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Huge", ResourceRegisterClass::srv, 0xfffffffaU, 20U, 0),
    };
    const auto analysis = analyze_resource_bindings(resources);
    REQUIRE(analysis.groups.size() == 1);
    REQUIRE(analysis.groups.front().ranges.size() == 1);
    const auto& range = analysis.groups.front().ranges.front();
    CHECK_FALSE(range.unbounded);
    CHECK(range.end_register == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("Resource binding analysis represents unbounded ranges distinctly from finite ranges",
          "[dxc][resource-binding-analysis]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Bounded", ResourceRegisterClass::srv, 0, 4, 0),
        make_resource("Unbounded", ResourceRegisterClass::uav, 0, 0, 2, /*unbounded=*/true),
    };
    const auto analysis = analyze_resource_bindings(resources);

    const auto bounded_group = std::ranges::find_if(analysis.groups, [](const auto& group) {
        return group.register_class == ResourceRegisterClass::srv;
    });
    REQUIRE(bounded_group != analysis.groups.end());
    REQUIRE(bounded_group->ranges.size() == 1);
    CHECK_FALSE(bounded_group->ranges.front().unbounded);
    CHECK(bounded_group->ranges.front().end_register == 3);

    const auto unbounded_group = std::ranges::find_if(analysis.groups, [](const auto& group) {
        return group.register_class == ResourceRegisterClass::uav;
    });
    REQUIRE(unbounded_group != analysis.groups.end());
    REQUIRE(unbounded_group->ranges.size() == 1);
    CHECK(unbounded_group->ranges.front().unbounded);
    CHECK(unbounded_group->ranges.front().base_register == 0);
}

TEST_CASE("Resource binding analysis detects a provable collision between distinct resources",
          "[dxc][resource-binding-analysis]") {
    // DXC's own front end was empirically confirmed to reject two distinct
    // resources sharing overlapping registers in the same space at compile
    // time ("resource B at register 0 overlaps with resource A at register
    // 0, space 0"), so this scenario can never appear in real reflection
    // output for a single successfully compiled entry point. It is still
    // exercised here directly against the pure analysis function, both as a
    // defense-in-depth check and in case a future compiler surfaces
    // overlaps DXC 1.9.2607.13 does not (for example, across variants or
    // library entry points this server does not currently reflect).
    const std::vector<CompilationResourceBinding> resources{
        make_resource("A", ResourceRegisterClass::srv, 0, 4, 0),
        make_resource("B", ResourceRegisterClass::srv, 2, 1, 0),
    };
    const auto analysis = analyze_resource_bindings(resources);
    REQUIRE(analysis.collisions.size() == 1);
    const auto& collision = analysis.collisions.front();
    CHECK(collision.register_class == ResourceRegisterClass::srv);
    CHECK(collision.space == 0);
    CHECK(((collision.first_resource == "A" && collision.second_resource == "B") ||
           (collision.first_resource == "B" && collision.second_resource == "A")));
    CHECK_FALSE(collision.message.empty());
}

TEST_CASE("Resource binding analysis never reports a resource colliding with itself",
          "[dxc][resource-binding-analysis]") {
    // Reflection should never report the same resource name twice, but the
    // collision detector must not fabricate a self-collision even if it
    // did.
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Self", ResourceRegisterClass::srv, 0, 1, 0),
        make_resource("Self", ResourceRegisterClass::srv, 0, 1, 0),
    };
    const auto analysis = analyze_resource_bindings(resources);
    CHECK(analysis.collisions.empty());
}

TEST_CASE("Resource binding analysis distinctly classifies but excludes system-reserved spaces "
          "from collision detection",
          "[dxc][resource-binding-analysis]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("SystemA", ResourceRegisterClass::uav, 0, 1, 0xfffffff0U, false,
                      /*system_reserved_space=*/true),
        make_resource("SystemB", ResourceRegisterClass::uav, 0, 1, 0xfffffff0U, false,
                      /*system_reserved_space=*/true),
    };
    const auto analysis = analyze_resource_bindings(resources);
    REQUIRE(analysis.groups.size() == 1);
    CHECK(analysis.groups.front().system_reserved_space);
    CHECK(analysis.groups.front().ranges.size() == 2);
    // The two resources overlap exactly, but the group is system-reserved,
    // so no collision is reported for it.
    CHECK(analysis.collisions.empty());
}

TEST_CASE("Resource binding analysis detects an unbounded range colliding with a finite range",
          "[dxc][resource-binding-analysis]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("Finite", ResourceRegisterClass::srv, 4, 4, 1),
        make_resource("Unbounded", ResourceRegisterClass::srv, 0, 0, 1, /*unbounded=*/true),
    };
    const auto analysis = analyze_resource_bindings(resources);
    REQUIRE(analysis.collisions.size() == 1);
}

TEST_CASE("Resource binding analysis does not report adjacent, non-overlapping ranges as colliding",
          "[dxc][resource-binding-analysis]") {
    const std::vector<CompilationResourceBinding> resources{
        make_resource("First", ResourceRegisterClass::srv, 0, 4, 0),
        make_resource("Second", ResourceRegisterClass::srv, 4, 4, 0),
    };
    const auto analysis = analyze_resource_bindings(resources);
    CHECK(analysis.collisions.empty());
}
