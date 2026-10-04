#include <hlsl_intellisense/analysis/pipeline_validation.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <tuple>

namespace hlsl_intellisense::analysis {
namespace {

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return result;
}

[[nodiscard]] std::string semantic_label(const dxc::CompilationSignatureParameter& parameter) {
    return parameter.semantic_name + std::to_string(parameter.semantic_index);
}

[[nodiscard]] bool generated_system_value(const dxc::CompilationSignatureParameter& parameter,
                                          workspace::PipelineStageKind producer,
                                          workspace::PipelineStageKind consumer) {
    const auto semantic = lower(parameter.semantic_name);
    if (consumer == workspace::PipelineStageKind::geometry) {
        return semantic == "sv_primitiveid" || semantic == "sv_gsinstanceid";
    }
    if (consumer == workspace::PipelineStageKind::domain) {
        return semantic == "sv_domainlocation" || semantic == "sv_tessfactor" ||
               semantic == "sv_insidetessfactor" || semantic == "sv_primitiveid";
    }
    if (consumer != workspace::PipelineStageKind::pixel) {
        return false;
    }
    if (semantic == "sv_primitiveid") {
        return producer != workspace::PipelineStageKind::geometry;
    }
    return semantic == "sv_isfrontface" || semantic == "sv_sampleindex" ||
           semantic == "sv_coverage" || semantic == "sv_innercoverage" ||
           semantic == "sv_barycentrics" || semantic == "sv_shadingrate";
}

[[nodiscard]] const dxc::CompilationSignatureParameter*
find_output(const std::vector<dxc::CompilationSignatureParameter>& outputs,
            const dxc::CompilationSignatureParameter& input) {
    const auto input_name = lower(input.semantic_name);
    const auto match = std::ranges::find_if(outputs, [&](const auto& output) {
        return output.semantic_index == input.semantic_index &&
               lower(output.semantic_name) == input_name;
    });
    return match == outputs.end() ? nullptr : &*match;
}

[[nodiscard]] bool resource_binding_equal(const dxc::CompilationResourceBinding& left,
                                          const dxc::CompilationResourceBinding& right) {
    return left.register_class == right.register_class && left.bind_point == right.bind_point &&
           left.bind_count == right.bind_count && left.space == right.space &&
           left.unbounded == right.unbounded;
}

[[nodiscard]] bool resource_type_equal(const dxc::CompilationResourceBinding& left,
                                       const dxc::CompilationResourceBinding& right) {
    return left.type == right.type && left.dimension == right.dimension &&
           left.return_type == right.return_type && left.sample_count == right.sample_count;
}

void validate_interface(const workspace::ResolvedPipeline& pipeline,
                        std::span<const PipelineStageSnapshot> stages, std::size_t producer_index,
                        std::vector<PipelineIssue>& issues) {
    const auto consumer_index = producer_index + 1;
    const auto& producer = stages[producer_index];
    const auto& consumer = stages[consumer_index];
    if (!producer.compilation.reflection || !producer.compilation.reflection->available ||
        !consumer.compilation.reflection || !consumer.compilation.reflection->available) {
        return;
    }

    if (consumer.stage.kind == workspace::PipelineStageKind::pixel &&
        (std::ranges::any_of(producer.compilation.reflection->output_signature,
                             [](const auto& value) { return !value.interpolation_available; }) ||
         std::ranges::any_of(consumer.compilation.reflection->input_signature,
                             [](const auto& value) { return !value.interpolation_available; }))) {
        issues.push_back(PipelineIssue{
            .code = PipelineIssueCode::analysis_unavailable,
            .pipeline_name = pipeline.name,
            .message = "Pipeline '" + pipeline.name +
                       "' interpolation matching is unavailable for the selected compiler.",
            .producer_stage = producer_index,
            .consumer_stage = consumer_index,
            .producer_location = std::nullopt,
            .consumer_location = std::nullopt});
    }
    for (const auto& input : consumer.compilation.reflection->input_signature) {
        const auto* output = find_output(producer.compilation.reflection->output_signature, input);
        if (output == nullptr) {
            if (generated_system_value(input, producer.stage.kind, consumer.stage.kind)) {
                continue;
            }
            issues.push_back(PipelineIssue{
                .code = PipelineIssueCode::missing_producer_output,
                .pipeline_name = pipeline.name,
                .message = "Pipeline '" + pipeline.name + "' consumer semantic '" +
                           semantic_label(input) + "' has no matching producer output.",
                .producer_stage = producer_index,
                .consumer_stage = consumer_index,
                .producer_location = std::nullopt,
                .consumer_location = input.source_location});
            continue;
        }

        const auto add_mismatch = [&](PipelineIssueCode code, std::string detail) {
            issues.push_back(PipelineIssue{.code = code,
                                           .pipeline_name = pipeline.name,
                                           .message = "Pipeline '" + pipeline.name +
                                                      "' semantic '" + semantic_label(input) +
                                                      "' " + std::move(detail) + ".",
                                           .producer_stage = producer_index,
                                           .consumer_stage = consumer_index,
                                           .producer_location = output->source_location,
                                           .consumer_location = input.source_location});
        };
        if (output->component_type != input.component_type) {
            add_mismatch(PipelineIssueCode::component_type_mismatch,
                         "uses producer type " + output->component_type + " but consumer type " +
                             input.component_type);
        }
        if ((output->mask & input.mask) != input.mask) {
            add_mismatch(PipelineIssueCode::component_mask_mismatch,
                         "does not provide every component required by the consumer");
        }
        if (output->system_value != input.system_value) {
            add_mismatch(PipelineIssueCode::system_value_mismatch,
                         "uses incompatible system-value classifications");
        }
        if (consumer.stage.kind == workspace::PipelineStageKind::pixel &&
            output->interpolation_available && input.interpolation_available &&
            output->interpolation != dxc::InterpolationMode::undefined &&
            input.interpolation != dxc::InterpolationMode::undefined &&
            output->interpolation != input.interpolation) {
            add_mismatch(PipelineIssueCode::interpolation_mismatch,
                         "uses incompatible interpolation modifiers");
        }
    }
}

void validate_resources(const workspace::ResolvedPipeline& pipeline,
                        std::span<const PipelineStageSnapshot> stages,
                        std::vector<PipelineIssue>& issues) {
    struct FirstResource {
        std::size_t stage{};
        const dxc::CompilationResourceBinding* resource{};
    };
    std::map<std::string, FirstResource, std::less<>> resources;
    for (std::size_t stage_index = 0; stage_index < stages.size(); ++stage_index) {
        const auto& reflection = stages[stage_index].compilation.reflection;
        if (!reflection || !reflection->available) {
            continue;
        }
        for (const auto& resource : reflection->resources) {
            const auto [entry, inserted] =
                resources.emplace(resource.name, FirstResource{stage_index, &resource});
            if (inserted) {
                continue;
            }
            const auto& first = *entry->second.resource;
            const auto add_mismatch = [&](PipelineIssueCode code, std::string detail) {
                issues.push_back(PipelineIssue{.code = code,
                                               .pipeline_name = pipeline.name,
                                               .message = "Pipeline '" + pipeline.name +
                                                          "' resource '" + resource.name + "' " +
                                                          std::move(detail) + ".",
                                               .producer_stage = entry->second.stage,
                                               .consumer_stage = stage_index,
                                               .producer_location = first.source_location,
                                               .consumer_location = resource.source_location});
            };
            if (!resource_binding_equal(first, resource)) {
                add_mismatch(PipelineIssueCode::resource_binding_mismatch,
                             "uses incompatible register bindings across stages");
            }
            if (!resource_type_equal(first, resource)) {
                add_mismatch(PipelineIssueCode::resource_type_mismatch,
                             "uses incompatible reflected declarations across stages");
            }
        }
    }
}

} // namespace

auto validate_pipeline(const workspace::ResolvedPipeline& pipeline,
                       std::span<const PipelineStageSnapshot> stages)
    -> std::vector<PipelineIssue> {
    std::vector<PipelineIssue> issues;
    if (stages.size() != pipeline.stages.size()) {
        issues.push_back(PipelineIssue{
            .code = PipelineIssueCode::analysis_unavailable,
            .pipeline_name = pipeline.name,
            .message = "Pipeline '" + pipeline.name +
                       "' could not be validated because one or more stage analyses are missing.",
            .producer_stage = 0,
            .consumer_stage = 0,
            .producer_location = std::nullopt,
            .consumer_location = std::nullopt});
        return issues;
    }
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto& compilation = stages[index].compilation;
        if (stages[index].unavailable_reason || !compilation.success || !compilation.reflection ||
            !compilation.reflection->available) {
            issues.push_back(PipelineIssue{
                .code = PipelineIssueCode::analysis_unavailable,
                .pipeline_name = pipeline.name,
                .message =
                    "Pipeline '" + pipeline.name + "' stage analysis is unavailable" +
                    (stages[index].unavailable_reason ? ": " + *stages[index].unavailable_reason
                                                      : std::string{}) +
                    ".",
                .producer_stage = index,
                .consumer_stage = index,
                .producer_location = std::nullopt,
                .consumer_location = std::nullopt});
        }
    }
    for (std::size_t index = 0; index + 1 < stages.size(); ++index) {
        if (stages[index].stage.kind == workspace::PipelineStageKind::hull &&
            stages[index + 1].stage.kind == workspace::PipelineStageKind::domain) {
            issues.push_back(PipelineIssue{
                .code = PipelineIssueCode::analysis_unavailable,
                .pipeline_name = pipeline.name,
                .message = "Pipeline '" + pipeline.name +
                           "' hull-to-domain validation is unavailable because DXC patch-constant "
                           "signature reflection is not exposed yet.",
                .producer_stage = index,
                .consumer_stage = index + 1,
                .producer_location = std::nullopt,
                .consumer_location = std::nullopt});
            continue;
        }
        validate_interface(pipeline, stages, index, issues);
    }
    validate_resources(pipeline, stages, issues);
    return issues;
}

} // namespace hlsl_intellisense::analysis
