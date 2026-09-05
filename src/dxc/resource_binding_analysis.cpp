#include "resource_binding_analysis.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace hlsl_intellisense::dxc::detail {

namespace {

// Two ranges overlap when they share at least one register index. Unbounded
// ranges are treated as extending to UINT32_MAX for this purpose (register
// indices are 32-bit, so this is the largest register an unbounded range
// could ever actually reach), keeping the comparison within safe 64-bit
// arithmetic without modelling true mathematical infinity.
[[nodiscard]] bool ranges_overlap(const ResourceBindingRange& first,
                                  const ResourceBindingRange& second) {
    const std::uint64_t first_end =
        first.unbounded ? std::numeric_limits<std::uint32_t>::max() : first.end_register;
    const std::uint64_t second_end =
        second.unbounded ? std::numeric_limits<std::uint32_t>::max() : second.end_register;
    return static_cast<std::uint64_t>(first.base_register) <= second_end &&
           static_cast<std::uint64_t>(second.base_register) <= first_end;
}

[[nodiscard]] std::string describe_range(const ResourceBindingRange& range) {
    if (range.unbounded) {
        return "register " + std::to_string(range.base_register) + " and above";
    }
    return "registers " + std::to_string(range.base_register) + "-" +
           std::to_string(range.end_register);
}

struct GroupKey {
    ResourceRegisterClass register_class;
    std::uint32_t space;

    [[nodiscard]] bool operator<(const GroupKey& other) const {
        if (register_class != other.register_class) {
            return register_class < other.register_class;
        }
        return space < other.space;
    }
};

} // namespace

ResourceBindingAnalysis
analyze_resource_bindings(const std::vector<CompilationResourceBinding>& resources) {
    ResourceBindingAnalysis analysis;

    std::map<GroupKey, std::size_t> group_index;
    for (const auto& resource : resources) {
        const GroupKey key{.register_class = resource.register_class, .space = resource.space};
        const auto [it, inserted] = group_index.try_emplace(key, analysis.groups.size());
        if (inserted) {
            analysis.groups.push_back(
                ResourceBindingGroup{.register_class = resource.register_class,
                                     .space = resource.space,
                                     .system_reserved_space = resource.system_reserved_space,
                                     .ranges = {}});
        }
        auto& group = analysis.groups[it->second];
        ResourceBindingRange range{.resource_name = resource.name,
                                   .base_register = resource.bind_point,
                                   .unbounded = resource.unbounded,
                                   .end_register = {}};
        if (!range.unbounded) {
            range.end_register = safe_end_register(resource.bind_point, resource.bind_count);
        }
        group.ranges.push_back(std::move(range));
    }

    // Detect provable collisions within each group, skipping groups whose
    // space is D3D12's reserved system range: those are compiler/driver
    // internal, so collisions there are neither actionable nor meaningful,
    // but the group itself is still surfaced above with
    // system_reserved_space set so it is distinctly classified rather than
    // silently dropped.
    for (const auto& group : analysis.groups) {
        if (group.system_reserved_space) {
            continue;
        }
        for (std::size_t i = 0; i < group.ranges.size(); ++i) {
            for (std::size_t j = i + 1; j < group.ranges.size(); ++j) {
                const auto& first = group.ranges[i];
                const auto& second = group.ranges[j];
                if (first.resource_name == second.resource_name) {
                    // Never report a resource colliding with itself, even if
                    // reflection somehow reported the same name twice.
                    continue;
                }
                if (!ranges_overlap(first, second)) {
                    continue;
                }
                analysis.collisions.push_back(ResourceBindingCollision{
                    .first_resource = first.resource_name,
                    .second_resource = second.resource_name,
                    .register_class = group.register_class,
                    .space = group.space,
                    .message = "'" + first.resource_name + "' (" + describe_range(first) +
                               ") and '" + second.resource_name + "' (" + describe_range(second) +
                               ") both occupy space " + std::to_string(group.space) +
                               " in the same register class"});
            }
        }
    }

    return analysis;
}

} // namespace hlsl_intellisense::dxc::detail
