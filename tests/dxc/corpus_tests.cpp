#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/document_store.h>
#include <hlsl_intellisense/workspace/include_resolver.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dxc = hlsl_intellisense::dxc;

namespace {

[[nodiscard]] std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{"Unable to read corpus fixture: " + path.string()};
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void require_clean_fixture(std::string_view relative_path, dxc::CompilerOptions options = {},
                           bool allow_warnings = false) {
    const auto path = std::filesystem::path{HLSL_TEST_CORPUS_DIR} / relative_path;
    std::vector sources{dxc::SourceFile{.path = path.generic_string(), .text = read_fixture(path)}};
    dxc::Intellisense intellisense;
    auto translation_unit = intellisense.parse(path.generic_string(), std::move(sources), options);
    const auto diagnostics = translation_unit.diagnostics();
    std::string messages;
    for (const auto& diagnostic : diagnostics) {
        messages += diagnostic.message;
        messages.push_back('\n');
    }
    INFO(messages);
    if (!allow_warnings) {
        CHECK(diagnostics.empty());
    }
    CHECK(std::ranges::none_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == dxc::DiagnosticSeverity::error ||
               diagnostic.severity == dxc::DiagnosticSeverity::fatal;
    }));
    CHECK_FALSE(translation_unit.symbols().empty());
}

} // namespace

TEST_CASE("Representative DXC HLSL 2021 corpus parses cleanly", "[dxc][corpus][modern]") {
    dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    require_clean_fixture("dxc/modern.hlsl", std::move(options));
}

TEST_CASE("Representative Unreal virtual include corpus parses cleanly", "[dxc][corpus][unreal]") {
    const auto directory = std::filesystem::path{HLSL_TEST_CORPUS_DIR} / "unreal";
    const auto root_path = directory / "Material.usf";
    const auto uri = hlsl_intellisense::workspace::DocumentUri::from_path(root_path.string());
    const hlsl_intellisense::workspace::SourceSnapshot root{uri, "hlsl", 1,
                                                            read_fixture(root_path)};
    hlsl_intellisense::workspace::WorkspaceConfiguration configuration;
    configuration.virtual_directory_mappings.emplace("/Project", directory);
    const std::array open_documents{root};
    const auto resolution =
        hlsl_intellisense::workspace::resolve_includes(root, open_documents, configuration);
    dxc::Intellisense intellisense;
    auto translation_unit = intellisense.parse(root_path.generic_string(), resolution.sources,
                                               configuration.compiler_options());
    INFO(resolution.sources.size());
    CHECK(translation_unit.diagnostics().empty());
    CHECK_FALSE(translation_unit.symbols().empty());
}

TEST_CASE("Representative extracted Unity HLSL corpus parses cleanly", "[dxc][corpus][unity]") {
    dxc::CompilerOptions options;
    options.defines = {"UNITY_REVERSED_Z=1"};
    require_clean_fixture("unity/Pass.hlsl", std::move(options));
}

TEST_CASE("Representative Vulkan HLSL corpus parses cleanly", "[dxc][corpus][vulkan]") {
    dxc::CompilerOptions options;
    options.target_profile = "cs_6_6";
    options.additional_arguments = {"-spirv"};
    require_clean_fixture("vulkan/Compute.hlsl", std::move(options), true);
}
