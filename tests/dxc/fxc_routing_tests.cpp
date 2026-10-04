#include <hlsl_intellisense/dxc/intellisense.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace dxc = hlsl_intellisense::dxc;

#ifdef _WIN32
TEST_CASE("FXC routing preserves shared legacy source symbols without DXC intrinsics",
          "[dxc][fxc][routing]") {
    const auto path = (std::filesystem::current_path() / "fxc-source.hlsl").string();
    const std::string source = "float twice(float value) { return value * 2.0; }\n"
                               "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
                               "  float localValue = twice(uv.x);\n"
                               "  return localValue.xxxx;\n"
                               "}\n";
    dxc::CompilerOptions options;
    options.language_version = "2016";
    options.entry_point = "main";
    options.target_profile = "ps_6_0";
    dxc::Intellisense intellisense;
    auto modern = intellisense.parse(path, {{path, source}}, options);
    options.backend = dxc::CompilerBackend::fxc;
    options.target_profile = "ps_5_0";
    auto legacy = intellisense.parse(path, {{path, source}}, options);
    CHECK(legacy.diagnostics().empty());
    CHECK(modern.compilation_info().success);
    CHECK(legacy.compilation_info().success);
    CHECK(legacy.compilation_info().compiler_backend == "fxc");
    REQUIRE(legacy.compilation_info().output);
    CHECK(legacy.compilation_info().output->type == "dxbc");

    const auto legacy_hover = legacy.hover_at(path, 3, 22);
    const auto modern_hover = modern.hover_at(path, 3, 22);
    REQUIRE(legacy_hover);
    REQUIRE(modern_hover);
    CHECK(legacy_hover->name == modern_hover->name);
    CHECK(legacy_hover->type == modern_hover->type);
    CHECK(legacy_hover->declaration == modern_hover->declaration);
    const auto legacy_definition = legacy.definition_at(path, 3, 22);
    const auto modern_definition = modern.definition_at(path, 3, 22);
    REQUIRE(legacy_definition);
    REQUIRE(modern_definition);
    CHECK(legacy_definition->name == modern_definition->name);
    CHECK(legacy_definition->location == modern_definition->location);
    const auto legacy_signatures = legacy.signatures_at(path, 3, 22);
    const auto modern_signatures = modern.signatures_at(path, 3, 22);
    REQUIRE_FALSE(legacy_signatures.empty());
    REQUIRE_FALSE(modern_signatures.empty());
    CHECK(legacy_signatures.front().label == modern_signatures.front().label);

    const auto completions = legacy.complete(path, 4, 3);
    CHECK(std::ranges::any_of(completions,
                              [](const auto& item) { return item.label == "localValue"; }));
    CHECK(std::ranges::any_of(completions, [](const auto& item) { return item.label == "twice"; }));
    CHECK(std::ranges::none_of(completions, [](const auto& item) {
        return item.label.starts_with("Wave") || item.label == "NonUniformResourceIndex";
    }));
    CHECK_THROWS_WITH(legacy.skipped_ranges(), Catch::Matchers::ContainsSubstring("FXC"));
    const auto flow = legacy.entry_point_data_flow();
    CHECK_FALSE(flow.found);
    CHECK(flow.explanation.find("FXC") != std::string::npos);
}

TEST_CASE("FXC routing invalidates compilation on unsaved root and include edits",
          "[dxc][fxc][routing]") {
    const std::string path = "fxc-reparse.hlsl";
    const std::string root = "#include \"fxc-values.hlsli\"\n"
                             "float4 main() : SV_Target { return color(); }\n";
    dxc::CompilerOptions options;
    options.backend = dxc::CompilerBackend::fxc;
    options.target_profile = "ps_5_0";
    options.entry_point = "main";
    dxc::Intellisense intellisense;
    auto unit = intellisense.parse(
        path, {{path, root}, {"fxc-values.hlsli", "float4 color() { return 1.0.xxxx; }\n"}},
        options);
    CHECK(unit.compilation_info().success);
    unit.reparse({{path, root}, {"fxc-values.hlsli", "float4 color() { return missing; }\n"}});
    const auto diagnostics = unit.diagnostics();
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics.front().source == "fxc");
    CHECK(diagnostics.front().location.path.ends_with("fxc-values.hlsli"));
    CHECK_FALSE(unit.compilation_info().success);
    unit.reparse({{path, "float4 main() : SV_Target { return 0; }\n"}});
    CHECK(unit.diagnostics().empty());
    CHECK(unit.compilation_info().success);
    CHECK_THROWS_WITH(unit.macro_definitions(), Catch::Matchers::ContainsSubstring("FXC"));
}

TEST_CASE("FXC routing preserves native legacy syntax and explicit strictness policy",
          "[dxc][fxc][routing]") {
    const std::string path = "fxc-legacy.hlsl";
    const std::string source = "texture tex;\n"
                               "sampler s = sampler_state { Texture = <tex>; };\n"
                               "float4 main() : SV_Target { return 1; }\n";
    dxc::CompilerOptions options;
    options.backend = dxc::CompilerBackend::fxc;
    options.target_profile = "ps_5_0";
    options.entry_point = "main";
    dxc::Intellisense intellisense;
    auto legacy = intellisense.parse(path, {{path, source}}, options);
    CHECK(legacy.compilation_info().success);
    CHECK(std::ranges::none_of(legacy.diagnostics(), [](const auto& diagnostic) {
        return diagnostic.severity == dxc::DiagnosticSeverity::error ||
               diagnostic.severity == dxc::DiagnosticSeverity::fatal;
    }));
    options.additional_arguments = {"/Ges"};
    auto strict = intellisense.parse(path, {{path, source}}, options);
    CHECK_FALSE(strict.compilation_info().success);
    CHECK(std::ranges::any_of(strict.diagnostics(), [](const auto& diagnostic) {
        return diagnostic.source == "fxc" && diagnostic.message.find("X3086") != std::string::npos;
    }));
}

TEST_CASE("FXC routing reports unsupported profiles through the selected compiler",
          "[dxc][fxc][routing]") {
    dxc::Intellisense intellisense;
    for (const auto* profile : {"ps_6_6", "lib_6_6", "ms_6_5"}) {
        CAPTURE(profile);
        dxc::CompilerOptions options;
        options.backend = dxc::CompilerBackend::fxc;
        options.target_profile = profile;
        options.entry_point = "main";
        auto unit = intellisense.parse(
            "fxc-profile.hlsl", {{"fxc-profile.hlsl", "float4 main() : SV_Target { return 1; }\n"}},
            options);
        const auto info = unit.compilation_info();
        CHECK_FALSE(info.success);
        CHECK(info.compiler_backend == "fxc");
        CHECK_FALSE(info.output);
        REQUIRE_FALSE(info.diagnostics.empty());
        CHECK(info.diagnostics.front().source == "fxc");
    }
}
#else
TEST_CASE("FXC routing never silently falls back to DXC outside Windows", "[dxc][fxc][routing]") {
    dxc::CompilerOptions options;
    options.backend = dxc::CompilerBackend::fxc;
    options.target_profile = "ps_5_0";
    options.entry_point = "main";
    dxc::Intellisense intellisense;
    auto unit = intellisense.parse(
        "fxc.hlsl", {{"fxc.hlsl", "float4 main() : SV_Target { return 1; }\n"}}, options);
    const auto diagnostics = unit.diagnostics();
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(diagnostics.front().source == "fxc");
    CHECK(diagnostics.front().message.find("Windows") != std::string::npos);
    const auto info = unit.compilation_info();
    CHECK_FALSE(info.success);
    CHECK(info.compiler_backend == "fxc");
    CHECK_FALSE(info.output);
    CHECK_FALSE(info.reflection);
}
#endif
