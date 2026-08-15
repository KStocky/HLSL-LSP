#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>

#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace hlsl_intellisense::workspace {

struct IncludeResolution {
    std::vector<dxc::SourceFile> sources;
    std::unordered_set<std::string> dependency_identities;
    bool has_dynamic_includes{};
};

[[nodiscard]] IncludeResolution resolve_includes(const SourceSnapshot& root,
                                                 std::span<const SourceSnapshot> open_documents,
                                                 const WorkspaceConfiguration& configuration);

} // namespace hlsl_intellisense::workspace
