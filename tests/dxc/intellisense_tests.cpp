#include <hlsl_intellisense/dxc/intellisense.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr auto shader_path = "prototype.hlsl";

auto hlsl_2021_source(const std::string& type_name, const std::string& function_name)
    -> std::string {
    return "template<typename T>\n"
           "T " +
           function_name +
           "(T left, T right) {\n"
           "    return left + right;\n"
           "}\n"
           "\n"
           "struct " +
           type_name +
           " {\n"
           "    float value;\n"
           "    " +
           type_name + " operator +(" + type_name +
           " right) {\n"
           "        " +
           type_name +
           " result = {value + right.value};\n"
           "        return result;\n"
           "    }\n"
           "};\n"
           "\n"
           "float4 main() : SV_Target {\n"
           "    " +
           type_name +
           " left = {1.0};\n"
           "    " +
           type_name +
           " right = {2.0};\n"
           "    " +
           type_name + " sum = " + function_name +
           "(left, right);\n"
           "    return sum.value.xxxx;\n"
           "}\n"
           "\n";
}

} // namespace

TEST_CASE("Compiler options produce DXC arguments", "[dxc]") {
    hlsl_intellisense::dxc::CompilerOptions options{.language_version = "2021",
                                                    .target_profile = "ps_6_6",
                                                    .entry_point = "main",
                                                    .defines = {"FEATURE=1"},
                                                    .include_directories = {"include"},
                                                    .additional_arguments = {"-spirv"}};

    CHECK(options.arguments() == std::vector<std::string>{"-HV", "2021", "-T", "ps_6_6", "-E",
                                                          "main", "-D", "FEATURE=1", "-I",
                                                          "include", "-spirv"});
}

TEST_CASE("DXC IntelliSense analyzes HLSL 2021", "[dxc][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.language_version = "2021";
    auto translation_unit = intellisense.parse(
        shader_path, {{shader_path, hlsl_2021_source("Number", "combine")}}, options);

    CHECK(translation_unit.diagnostics().empty());

    const auto completions = translation_unit.complete(shader_path, 20, 1);
    CHECK(std::ranges::any_of(completions,
                              [](const auto& completion) { return completion.label == "Number"; }));

    const auto definition = translation_unit.definition_at(shader_path, 17, 20);
    if (!definition.has_value()) {
        FAIL("Expected a definition for combine");
    } else {
        CHECK(definition->name == "combine");
        CHECK(definition->location.line == 2);
    }

    const auto tokens = translation_unit.tokens(shader_path);
    CHECK(std::ranges::any_of(tokens, [](const auto& token) {
        return token.kind == hlsl_intellisense::dxc::TokenKind::keyword;
    }));
    CHECK(std::ranges::any_of(tokens, [](const auto& token) {
        return token.kind == hlsl_intellisense::dxc::TokenKind::built_in_type;
    }));
    CHECK(std::ranges::any_of(tokens, [](const auto& token) {
        return token.kind == hlsl_intellisense::dxc::TokenKind::identifier &&
               token.cursor_kind != 0;
    }));
}

TEST_CASE("DXC IntelliSense recognizes Shader Model 6.6 descriptor heaps", "[dxc][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "lib_6_6";
    auto translation_unit =
        intellisense.parse(shader_path,
                           {{shader_path, "RWByteAddressBuffer GetBuffer(uint index) {\n"
                                          "    return ResourceDescriptorHeap[index];\n"
                                          "}\n"
                                          "SamplerState GetSampler(uint index) {\n"
                                          "    return SamplerDescriptorHeap[index];\n"
                                          "}\n"}},
                           options);

    const auto diagnostics = translation_unit.diagnostics();
    std::string messages;
    for (const auto& diagnostic : diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    CHECK(diagnostics.empty());
}

TEST_CASE("DXC IntelliSense extracts hierarchical declaration symbols",
          "[dxc][symbols][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "enum Mode { ModeA, ModeB };\n"
                               "struct Material {\n"
                               "    float roughness;\n"
                               "    float Shade(float value) { return value * roughness; }\n"
                               "};\n"
                               "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto symbols = translation_unit.symbols();
    const auto find_symbol = [](const auto& self, const auto& candidates,
                                std::string_view name) -> const hlsl_intellisense::dxc::Symbol* {
        for (const auto& symbol : candidates) {
            if (symbol.name == name) {
                return &symbol;
            }
            if (const auto* nested = self(self, symbol.children, name)) {
                return nested;
            }
        }
        return nullptr;
    };

    const auto* material = find_symbol(find_symbol, symbols, "Material");
    REQUIRE(material != nullptr);
    CHECK(material->cursor_kind == 2);
    CHECK(find_symbol(find_symbol, material->children, "roughness") != nullptr);
    CHECK(find_symbol(find_symbol, material->children, "Shade") != nullptr);
    CHECK(find_symbol(find_symbol, symbols, "Mode") != nullptr);
    CHECK(find_symbol(find_symbol, symbols, "ModeA") != nullptr);
    CHECK(find_symbol(find_symbol, symbols, "main") != nullptr);
    CHECK(material->end_offset > material->start_offset);
    CHECK(material->location.path == shader_path);
}

TEST_CASE("DXC IntelliSense reports descriptor heaps below Shader Model 6.6",
          "[dxc][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "lib_6_5";
    auto translation_unit =
        intellisense.parse(shader_path,
                           {{shader_path, "RWByteAddressBuffer GetBuffer(uint index) {\n"
                                          "    return ResourceDescriptorHeap[index];\n"
                                          "}\n"}},
                           options);

    CHECK_FALSE(translation_unit.diagnostics().empty());
}

TEST_CASE("DXC IntelliSense reports diagnostics", "[dxc][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit = intellisense.parse(
        shader_path, {{shader_path, "float4 main() : SV_Target { return missing_symbol; }\n"}});

    const auto diagnostics = translation_unit.diagnostics();

    REQUIRE(!diagnostics.empty());
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == hlsl_intellisense::dxc::DiagnosticSeverity::error;
    }));
    CHECK(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.location.line > 0 && diagnostic.location.column > 0;
    }));
}

TEST_CASE("DXC IntelliSense reparses edited sources", "[dxc][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, hlsl_2021_source("Number", "combine")}});

    translation_unit.reparse({{shader_path, hlsl_2021_source("UpdatedNumber", "combineUpdated")}});

    const auto completions = translation_unit.complete(shader_path, 20, 1);
    CHECK(std::ranges::any_of(
        completions, [](const auto& completion) { return completion.label == "UpdatedNumber"; }));
}

TEST_CASE("DXC IntelliSense navigates to incomplete template declarations",
          "[dxc][navigation][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source =
        "template<typename T> struct container_wrapper;\n"
        "template<typename T> void use_wrapper(inout container_wrapper<T> value) {}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto definition = translation_unit.definition_at(shader_path, 2, 49);
    REQUIRE(definition.has_value());
    CHECK(definition->name == "container_wrapper");
    CHECK(definition->location.line == 1);
}

TEST_CASE("DXC IntelliSense requires the root source", "[dxc]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    CHECK_THROWS_AS(intellisense.parse(shader_path, {{"other.hlsl", ""}}), std::invalid_argument);
}

TEST_CASE("DXC IntelliSense consumes unsaved include buffers", "[dxc][includes][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto directory = std::filesystem::current_path() / "unsaved-includes";
    std::filesystem::create_directories(directory);
    const auto root = (directory / "root.hlsl").generic_string();
    const auto include = std::filesystem::path{root}.parent_path() / "dependency.hlsli";
    const std::string root_text =
        "#include \"dependency.hlsli\"\nfloat4 main() : SV_Target { return includeValue; }\n";
    const std::string include_text = "static const float4 includeValue = 1.0.xxxx;\n";

    auto translation_unit =
        intellisense.parse(root, {{root, root_text}, {include.generic_string(), include_text}});

    const auto diagnostics = translation_unit.diagnostics();
    INFO(include.string());
    INFO(include.generic_string());
    std::string messages;
    for (const auto& diagnostic : diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    CHECK(diagnostics.empty());
    std::filesystem::remove_all(directory);
}
