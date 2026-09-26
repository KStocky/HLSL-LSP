#include <hlsl_intellisense/analysis/pipeline_validation.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace analysis = hlsl_intellisense::analysis;
namespace dxc = hlsl_intellisense::dxc;
namespace workspace = hlsl_intellisense::workspace;

namespace {

[[nodiscard]] dxc::CompilationSignatureParameter
signature(std::string name, std::uint32_t index = 0, std::string type = "float32",
          std::uint8_t mask = 0xf,
          dxc::InterpolationMode interpolation = dxc::InterpolationMode::linear,
          std::string system_value = "undefined") {
    return {.semantic_name = std::move(name),
            .semantic_index = index,
            .system_value = std::move(system_value),
            .component_type = std::move(type),
            .mask = mask,
            .interpolation = interpolation};
}

[[nodiscard]] dxc::CompilationResourceBinding resource(std::string name, std::uint32_t binding,
                                                       std::string type = "texture") {
    return {.name = std::move(name),
            .type = std::move(type),
            .bind_point = binding,
            .bind_count = 1,
            .space = 0,
            .dimension = "texture2d",
            .return_type = "float",
            .register_class = dxc::ResourceRegisterClass::srv,
            .sample_count = 0xffffffffU};
}

[[nodiscard]] analysis::PipelineStageSnapshot
stage(workspace::PipelineStageKind kind, std::vector<dxc::CompilationSignatureParameter> inputs,
      std::vector<dxc::CompilationSignatureParameter> outputs,
      std::vector<dxc::CompilationResourceBinding> resources = {}) {
    dxc::CompilationInfo compilation;
    compilation.success = true;
    compilation.reflection = dxc::CompilationReflection{.input_signature = std::move(inputs),
                                                        .output_signature = std::move(outputs),
                                                        .resources = std::move(resources)};
    return {.stage = workspace::PipelineStage{.kind = kind}, .compilation = std::move(compilation)};
}

[[nodiscard]] workspace::ResolvedPipeline pipeline(std::size_t stage_count = 2) {
    workspace::ResolvedPipeline result{.name = "Forward"};
    result.stages.resize(stage_count);
    return result;
}

} // namespace

TEST_CASE("Pipeline validation accepts compatible interfaces and shared resources",
          "[analysis][pipeline]") {
    const auto shared = resource("Scene", 0);
    const std::vector stages{stage(workspace::PipelineStageKind::vertex, {},
                                   {signature("SV_Position", 0, "float32", 0xf,
                                              dxc::InterpolationMode::linear, "position"),
                                    signature("TEXCOORD", 0, "float32", 0x3)},
                                   {shared}),
                             stage(workspace::PipelineStageKind::pixel,
                                   {signature("SV_Position", 0, "float32", 0xf,
                                              dxc::InterpolationMode::linear, "position"),
                                    signature("texcoord", 0, "float32", 0x3)},
                                   {}, {shared})};

    CHECK(analysis::validate_pipeline(pipeline(), stages).empty());
}

TEST_CASE("Pipeline validation reports every interface mismatch deterministically",
          "[analysis][pipeline]") {
    const std::vector stages{
        stage(
            workspace::PipelineStageKind::vertex, {},
            {signature("TEXCOORD", 0, "float32", 0x3, dxc::InterpolationMode::linear),
             signature("COLOR", 0, "uint32", 0xf, dxc::InterpolationMode::constant, "undefined")}),
        stage(workspace::PipelineStageKind::pixel,
              {signature("TEXCOORD", 0, "float32", 0xf, dxc::InterpolationMode::linear_centroid),
               signature("COLOR", 0, "float32", 0xf, dxc::InterpolationMode::linear, "position"),
               signature("MISSING", 1)},
              {})};

    const auto issues = analysis::validate_pipeline(pipeline(), stages);
    REQUIRE(issues.size() == 6);
    CHECK(issues[0].code == analysis::PipelineIssueCode::component_mask_mismatch);
    CHECK(issues[1].code == analysis::PipelineIssueCode::interpolation_mismatch);
    CHECK(issues[2].code == analysis::PipelineIssueCode::component_type_mismatch);
    CHECK(issues[3].code == analysis::PipelineIssueCode::system_value_mismatch);
    CHECK(issues[4].code == analysis::PipelineIssueCode::interpolation_mismatch);
    CHECK(issues[5].code == analysis::PipelineIssueCode::missing_producer_output);
}

TEST_CASE("Pipeline validation recognizes rasterizer-generated system values",
          "[analysis][pipeline]") {
    const std::vector stages{stage(workspace::PipelineStageKind::vertex, {}, {}),
                             stage(workspace::PipelineStageKind::pixel,
                                   {signature("SV_IsFrontFace", 0, "uint32", 0x1,
                                              dxc::InterpolationMode::constant, "is_front_face"),
                                    signature("SV_SampleIndex", 0, "uint32", 0x1,
                                              dxc::InterpolationMode::constant, "sample_index")},
                                   {})};

    CHECK(analysis::validate_pipeline(pipeline(), stages).empty());
}

TEST_CASE("Pipeline validation reports shared resource binding and type mismatches",
          "[analysis][pipeline]") {
    const std::vector stages{
        stage(workspace::PipelineStageKind::vertex, {}, {}, {resource("Scene", 0)}),
        stage(workspace::PipelineStageKind::pixel, {}, {},
              {resource("Scene", 1, "structured_buffer")})};

    const auto issues = analysis::validate_pipeline(pipeline(), stages);
    REQUIRE(issues.size() == 2);
    CHECK(issues[0].code == analysis::PipelineIssueCode::resource_binding_mismatch);
    CHECK(issues[1].code == analysis::PipelineIssueCode::resource_type_mismatch);
}

TEST_CASE("Pipeline validation reports unavailable stage analysis", "[analysis][pipeline]") {
    auto unavailable = stage(workspace::PipelineStageKind::pixel, {}, {});
    unavailable.compilation.success = false;
    const std::vector stages{stage(workspace::PipelineStageKind::vertex, {}, {}),
                             std::move(unavailable)};

    const auto issues = analysis::validate_pipeline(pipeline(), stages);
    REQUIRE(issues.size() == 1);
    CHECK(issues.front().code == analysis::PipelineIssueCode::analysis_unavailable);
}
