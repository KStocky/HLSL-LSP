#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hlsl_intellisense::analysis {

enum class PipelineIssueCode {
    analysis_unavailable,
    missing_producer_output,
    component_type_mismatch,
    component_mask_mismatch,
    interpolation_mismatch,
    system_value_mismatch,
    resource_binding_mismatch,
    resource_type_mismatch
};

struct PipelineStageSnapshot {
    workspace::PipelineStage stage;
    dxc::CompilationInfo compilation;
};

struct PipelineIssue {
    PipelineIssueCode code{};
    std::string pipeline_name;
    std::string message;
    std::size_t producer_stage{};
    std::size_t consumer_stage{};
    std::optional<dxc::SourceLocation> producer_location;
    std::optional<dxc::SourceLocation> consumer_location;
};

[[nodiscard]] std::vector<PipelineIssue>
validate_pipeline(const workspace::ResolvedPipeline& pipeline,
                  std::span<const PipelineStageSnapshot> stages);

} // namespace hlsl_intellisense::analysis
