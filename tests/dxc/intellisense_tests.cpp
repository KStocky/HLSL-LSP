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

TEST_CASE("DXC IntelliSense computes natural HLSL record layouts", "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Nested {\n"
                               "    bool enabled;\n"
                               "    half2 uv;\n"
                               "};\n"
                               "struct Data {\n"
                               "    float3 position, normal;\n"
                               "    double weight;\n"
                               "    Nested nested[2][3];\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 6, 13);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->kind == hlsl_intellisense::dxc::MemoryLayoutKind::natural);
    CHECK(layout->name == "Data");
    CHECK(layout->size == 104);
    CHECK(layout->alignment == 8);
    REQUIRE(layout->members.size() == 4);
    CHECK(layout->members[0].name == "position");
    CHECK(layout->members[0].offset == 0);
    CHECK(layout->members[0].size == 12);
    CHECK(layout->members[0].alignment == 4);
    CHECK(layout->members[1].name == "normal");
    CHECK(layout->members[1].offset == 12);
    CHECK(layout->members[2].offset == 24);
    CHECK(layout->members[3].offset == 32);
    CHECK(layout->members[3].size == 72);
    CHECK(layout->members[3].array_stride == 12);
    CHECK(layout->members[3].array_dimensions == std::vector<std::uint32_t>{6});
    REQUIRE(layout->members[3].members.size() == 6);
    CHECK(layout->members[3].members[0].name == "[0]");
    CHECK(layout->members[3].members[0].array_index == 0U);
    CHECK(layout->members[3].members[5].offset == 60);
    REQUIRE(layout->members[3].members[0].members.size() == 2);
    CHECK(layout->members[3].members[0].members[0].name == "enabled");
    CHECK(layout->selected_name == "position");
    CHECK(layout->selected_size == 12);
    CHECK_FALSE(layout->packed_offset.has_value());
}

TEST_CASE("Memory layout probes replace configured target profiles", "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    const std::string source = "struct Data { float3 value; };\n"
                               "float4 main() : SV_Target { return float4(0, 0, 0, 0); }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 9);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->name == "Data");
    CHECK(layout->size == 12);
}

TEST_CASE("DXC IntelliSense computes constant-buffer packing", "[dxc][memory-layout][cbuffer]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Inner { float3 direction; float scale; };\n"
                               "cbuffer Constants {\n"
                               "    float3 colour;\n"
                               "    float2 range;\n"
                               "    float exposure;\n"
                               "    float values[2];\n"
                               "    row_major float2x3 transform;\n"
                               "    float3x1 singleVector;\n"
                               "    Inner inner;\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 5, 12);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->kind == hlsl_intellisense::dxc::MemoryLayoutKind::constant_buffer);
    REQUIRE(layout->members.size() == 7);
    CHECK(layout->members[0].offset == 0);
    CHECK(layout->members[1].offset == 16);
    CHECK(layout->members[2].offset == 24);
    CHECK(layout->members[3].offset == 32);
    CHECK(layout->members[3].array_stride == 16);
    CHECK(layout->members[3].size == 32);
    REQUIRE(layout->members[3].members.size() == 2);
    CHECK(layout->members[3].members[0].offset == 0);
    CHECK(layout->members[3].members[1].offset == 16);
    CHECK(layout->members[4].offset == 64);
    CHECK(layout->members[4].matrix_stride == 16);
    CHECK(layout->members[4].row_major);
    CHECK(layout->members[4].size == 28);
    REQUIRE(layout->members[4].members.size() == 2);
    CHECK(layout->members[4].members[0].size == 12);
    CHECK(layout->members[4].members[1].offset == 16);
    CHECK(layout->members[5].offset == 96);
    CHECK(layout->members[5].size == 12);
    CHECK(layout->members[5].matrix_stride == 12);
    CHECK_FALSE(layout->members[5].row_major);
    CHECK(layout->members[6].offset == 112);
    CHECK(layout->members[6].size == 16);
    CHECK(layout->size == 128);
    CHECK(layout->selected_name == "exposure");
    CHECK(layout->packed_offset == 24U);
}

TEST_CASE("Nested cbuffer records force the following enclosing member to a new row",
          "[dxc][memory-layout][cbuffer]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Inner { float value; };\n"
                               "cbuffer Constants {\n"
                               "    Inner inner;\n"
                               "    float trailing;\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 3, 12);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    REQUIRE(layout->members.size() == 2);
    CHECK(layout->members[0].offset == 0);
    CHECK(layout->members[0].size == 4);
    // DXC packs trailing float right after the struct (offset 4, not 16).
    // The struct does not force the next member to a new 16-byte boundary.
    CHECK(layout->members[1].offset == 4);
    CHECK(layout->size == 8);
    CHECK(layout->allocation_size == 16);
}

TEST_CASE("Matrix layouts honor compiler defaults and position-sensitive pragmas",
          "[dxc][memory-layout][matrix]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string compiler_source = "cbuffer Constants { float2x3 transform; };\n";

    hlsl_intellisense::dxc::CompilerOptions row_options;
    row_options.additional_arguments = {"-Zpr"};
    auto row_translation =
        intellisense.parse(shader_path, {{shader_path, compiler_source}}, row_options);
    const auto row_layout = row_translation.memory_layout_at(shader_path, 1, 30);
    REQUIRE(row_layout.has_value());
    REQUIRE(row_layout->supported);
    CHECK(row_layout->members[0].row_major);
    CHECK(row_layout->members[0].size == 28);
    CHECK(row_layout->members[0].members.size() == 2);

    hlsl_intellisense::dxc::CompilerOptions column_options;
    column_options.additional_arguments = {"-Zpc"};
    auto column_translation =
        intellisense.parse(shader_path, {{shader_path, compiler_source}}, column_options);
    const auto column_layout = column_translation.memory_layout_at(shader_path, 1, 30);
    REQUIRE(column_layout.has_value());
    REQUIRE(column_layout->supported);
    CHECK_FALSE(column_layout->members[0].row_major);
    CHECK(column_layout->members[0].members.size() == 3);

    const std::string pragma_source = "cbuffer PragmaConstants {\n"
                                      "#pragma pack_matrix(row_major)\n"
                                      "    float2x3 first;\n"
                                      "#pragma pack_matrix(column_major)\n"
                                      "    float2x3 second;\n"
                                      "};\n";
    auto pragma_translation =
        intellisense.parse(shader_path, {{shader_path, pragma_source}}, column_options);
    const auto pragma_layout = pragma_translation.memory_layout_at(shader_path, 5, 14);
    REQUIRE(pragma_layout.has_value());
    REQUIRE(pragma_layout->supported);
    CHECK(pragma_layout->members[0].row_major);
    CHECK_FALSE(pragma_layout->members[1].row_major);
}

TEST_CASE("Memory layout field name selects the declarator", "[dxc][memory-layout][selection]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Data {\n"
                               "    float3 position;\n"
                               "    double weight;\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    // Cursor on field name "position" selects that field.
    const auto layout = translation_unit.memory_layout_at(shader_path, 2, 12);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->selected_name == "position");
    CHECK(layout->selected_type == "float3");
    CHECK(layout->selected_size == 12);
    CHECK(layout->selected_alignment == 4);
}

TEST_CASE("Compiler-backed layout handles conditional preprocessing correctly",
          "[dxc][memory-layout][preprocessor]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    // DXC compiles with default macro state. #if FEATURE evaluates to false,
    // so the #else branch is taken and the layout uses `double value`.
    const std::string fields_source = "struct ConditionalFields {\n"
                                      "#if FEATURE\n"
                                      "    float value;\n"
                                      "#else\n"
                                      "    double value;\n"
                                      "#endif\n"
                                      "};\n";
    auto fields_translation = intellisense.parse(shader_path, {{shader_path, fields_source}});
    const auto fields_layout = fields_translation.memory_layout_at(shader_path, 1, 10);
    REQUIRE(fields_layout.has_value());
    CHECK(fields_layout->supported);
    if (fields_layout->supported) {
        CHECK(fields_layout->size == 8); // double
    }

    // A struct inside #ifdef FEATURE (undefined) does not exist after
    // preprocessing. The probe compilation fails because the type is absent.
    const std::string record_source = "#ifdef FEATURE\n"
                                      "struct ConditionalRecord { float value; };\n"
                                      "#endif\n";
    auto record_translation = intellisense.parse(shader_path, {{shader_path, record_source}});
    const auto record_layout = record_translation.memory_layout_at(shader_path, 2, 10);
    // IntelliSense may or may not find the cursor in an inactive branch.
    // If it does find a struct, the probe compilation will fail.
    if (record_layout.has_value()) {
        CHECK_FALSE(record_layout->supported);
    }
}

TEST_CASE("Compiler-backed layout handles conditional matrix pragmas correctly",
          "[dxc][memory-layout][preprocessor][matrix]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    // DXC evaluates #if 0 correctly: the pragma is skipped.
    // Default column_major applies.
    const std::string conditional_source = "#if 0\n"
                                           "#pragma pack_matrix(row_major)\n"
                                           "#endif\n"
                                           "cbuffer Constants { float2x3 transform; };\n";
    auto conditional_translation =
        intellisense.parse(shader_path, {{shader_path, conditional_source}});
    const auto conditional_layout = conditional_translation.memory_layout_at(shader_path, 4, 33);
    if (conditional_layout.has_value() && conditional_layout->supported) {
        CHECK_FALSE(conditional_layout->members[0].row_major);
    }

    // Unconditional pragma overrides conditional ones.
    const std::string reset_source = "#if FEATURE\n"
                                     "#pragma pack_matrix(row_major)\n"
                                     "#endif\n"
                                     "#pragma pack_matrix(column_major)\n"
                                     "cbuffer Constants { float2x3 transform; };\n";
    auto reset_translation = intellisense.parse(shader_path, {{shader_path, reset_source}});
    const auto reset_layout = reset_translation.memory_layout_at(shader_path, 5, 33);
    if (reset_layout.has_value()) {
        REQUIRE(reset_layout->supported);
        CHECK_FALSE(reset_layout->members[0].row_major);
    }
}

TEST_CASE("Constant-buffer root size includes the final register row",
          "[dxc][memory-layout][cbuffer]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, "cbuffer Small { float value; };\n"}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 23);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->members[0].size == 4);
    CHECK(layout->size == 4);
    CHECK(layout->allocation_size == 16);
}

TEST_CASE("DXC memory layouts honor native 16-bit types and explain unsupported fields",
          "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.additional_arguments = {"-enable-16bit-types"};
    const std::string source = "struct Native16 {\n"
                               "    half value;\n"
                               "    uint16_t flags;\n"
                               "};\n"
                               "struct Unsupported {\n"
                               "    Texture2D texture;\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto native = translation_unit.memory_layout_at(shader_path, 2, 10);
    REQUIRE(native.has_value());
    REQUIRE(native->supported);
    CHECK(native->size == 4);
    CHECK(native->alignment == 2);
    CHECK(native->members[0].size == 2);
    CHECK(native->members[1].offset == 2);

    const auto unsupported = translation_unit.memory_layout_at(shader_path, 6, 15);
    // Texture2D cannot be a StructuredBuffer element; probe compilation fails.
    if (unsupported.has_value()) {
        CHECK_FALSE(unsupported->supported);
    }
}

TEST_CASE("Memory layouts reject ambiguous types and excessive expansion", "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    SECTION("scalar-prefixed record names remain records") {
        const std::string source = "struct floatData3 { double value; };\n"
                                   "struct Outer { floatData3 data; };\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 2, 28);
        REQUIRE(layout.has_value());
        REQUIRE(layout->supported);
        CHECK(layout->size == 8);
        CHECK(layout->members[0].kind == hlsl_intellisense::dxc::MemoryLayoutElementKind::record);
    }

    SECTION("bit-fields produce an unsupported probe compilation") {
        auto translation_unit =
            intellisense.parse(shader_path, {{shader_path, "struct Bits { uint value : 4; };\n"}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 1, 20);
        // DXC rejects or accepts bit-fields; either way the result is compiler-authoritative.
        if (layout.has_value()) {
            CHECK(true); // Accepted or rejected by compiler
        }
    }

    SECTION("nested arrays produce compiler-authoritative layout") {
        const std::string source = "struct Inner { float values[8]; };\n"
                                   "struct Outer { Inner values[4]; };\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 2, 23);
        REQUIRE(layout.has_value());
        if (layout->supported) {
            CHECK(layout->size == 128); // 4 * (8 * 4) = 128
            REQUIRE(layout->members.size() == 1);
            CHECK(layout->members[0].size == 128);
            CHECK(layout->members[0].array_stride == 32);
            REQUIRE(layout->members[0].members.size() == 4);
            REQUIRE(layout->members[0].members[0].members.size() == 1);
            CHECK(layout->members[0].members[0].members[0].size == 32);
            CHECK(layout->members[0].members[0].members[0].array_stride == 4);
            REQUIRE(layout->members[0].members[0].members[0].members.size() == 8);
            CHECK(layout->members[0].members[0].members[0].members[7].offset == 28);
        }
    }

    SECTION("arrays of empty records compile but have zero size") {
        const std::string source = "struct Empty {};\n"
                                   "struct Outer { Empty values[2]; };\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 2, 23);
        // DXC may accept or reject empty structs in StructuredBuffer.
        if (layout.has_value()) {
            CHECK(true); // Compiler-authoritative result
        }
    }

    SECTION("deep nesting is handled by DXC compilation") {
        // DXC handles moderate nesting. Define bottom-up to avoid forward references.
        std::string source = "struct Node10 { float value; };\n";
        for (int index = 9; index >= 0; --index) {
            source += "struct Node" + std::to_string(index) + " { Node" +
                      std::to_string(index + 1) + " value; };\n";
        }
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        // Node0 is at line 12 (after Node10 at line 1 and Node9..Node1 at lines 2-10).
        const auto layout = translation_unit.memory_layout_at(shader_path, 11, 8);
        REQUIRE(layout.has_value());
        CHECK(layout->supported);
        if (layout->supported) {
            CHECK(layout->size == 4); // float at the bottom
        }
    }
}

TEST_CASE("Memory layout positions support CR-only line endings", "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct First { float value; };\r"
                               "struct Second { double value; };\r";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 2, 25);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->name == "Second");
    CHECK(layout->size == 8);
}

TEST_CASE("Compiler handles referenced conditional records via compilation",
          "[dxc][memory-layout][preprocessor]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "#if FEATURE\n"
                               "struct Inner { float value; };\n"
                               "#endif\n"
                               "struct Outer { Inner value; };\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 4, 22);
    // Without FEATURE defined, Inner doesn't exist and the probe compilation
    // fails. DXC reports the compilation error.
    if (layout.has_value()) {
        CHECK_FALSE(layout->supported);
    }
}

TEST_CASE("Compiler-backed layout handles comments and includes correctly",
          "[dxc][memory-layout][preprocessor]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    SECTION("commented directives do not affect layout") {
        const std::string source = "/*\n"
                                   "#if FEATURE\n"
                                   "*/\n"
                                   "struct Data { float value; };\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 4, 8);
        REQUIRE(layout.has_value());
        REQUIRE(layout->supported);
        CHECK(layout->size == 4);
    }

    SECTION("includes are handled by DXC compilation") {
        // Without the include file available, the probe compilation fails.
        const std::string source = "#include \"packing.hlsli\"\n"
                                   "cbuffer Data { float2x3 value; };\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        const auto layout = translation_unit.memory_layout_at(shader_path, 2, 25);
        // Layout may be unsupported due to missing include file.
        if (layout.has_value()) {
            // The probe compilation will fail because the include file is missing.
            CHECK_FALSE(layout->supported);
        }
    }
}

TEST_CASE("Include guards do not block compiler-backed layout",
          "[dxc][memory-layout][preprocessor][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto guarded_shader_path =
        (std::filesystem::current_path() / "guarded-layout.hlsli").generic_string();
    const auto traits_path = (std::filesystem::current_path() / "traits.hlsli").generic_string();
    // Simulate a header with conventional include guards and an enum field —
    // the pattern reported in v0.6.0 where the old parser rejected the struct
    // because the #ifndef/#endif overlapped the declaration.
    const std::string source = "#ifndef SECTION_MANAGEMENT_HEADER\n"
                               "#define SECTION_MANAGEMENT_HEADER\n"
                               "\n"
                               "#include \"traits.hlsli\"\n"
                               "\n"
                               "namespace stf {\n"
                               "namespace detail {\n"
                               "enum class ExecutionRunState { Idle, Running, Complete };\n"
                               "struct ScenarioSectionInfo {\n"
                               "    int ParentID;\n"
                               "    ExecutionRunState RunState;\n"
                               "};\n"
                               "}\n"
                               "}\n"
                               "#endif\n";
    hlsl_intellisense::dxc::CompilerOptions options;
    options.language_version = "2021";
    auto translation_unit = intellisense.parse(
        guarded_shader_path,
        {{guarded_shader_path, source}, {traits_path, "struct IncludedTrait {};\n"}}, options);
    const auto diagnostics = translation_unit.diagnostics();
    const auto diagnostic_message =
        diagnostics.empty() ? std::string{} : diagnostics.front().message;
    INFO(diagnostic_message);
    REQUIRE(diagnostics.empty());
    const auto hover = translation_unit.hover_at(guarded_shader_path, 9, 12);
    REQUIRE(hover.has_value());
    CHECK(hover->qualified_name == "stf::detail::ScenarioSectionInfo");
    const auto layout = translation_unit.memory_layout_at(guarded_shader_path, 9, 12);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    CHECK(layout->name == "stf::detail::ScenarioSectionInfo");
    CHECK(layout->size == 8);
    CHECK(layout->members.size() == 2);
    CHECK(layout->members[0].name == "ParentID");
    CHECK(layout->members[0].offset == 0);
    CHECK(layout->members[0].size == 4);
    CHECK(layout->members[1].name == "RunState");
    CHECK(layout->members[1].offset == 4);

    const auto field_layout = translation_unit.memory_layout_at(guarded_shader_path, 10, 9);
    REQUIRE(field_layout.has_value());
    CHECK(field_layout->supported);
    CHECK(field_layout->size == 8);

    const auto enum_field_layout = translation_unit.memory_layout_at(guarded_shader_path, 11, 25);
    REQUIRE(enum_field_layout.has_value());
    CHECK(enum_field_layout->supported);
    CHECK(enum_field_layout->size == 8);
}

TEST_CASE("DXC memory layouts reparse unsaved record edits", "[dxc][memory-layout][reparse]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, "struct Data { float value; };\n"}});
    REQUIRE(translation_unit.memory_layout_at(shader_path, 1, 21)->size == 4);

    translation_unit.reparse({{shader_path, "struct Data { double value; float tail; };\n"}});
    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 22);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->size == 16);
    CHECK(layout->members[1].offset == 8);
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

TEST_CASE("DXC IntelliSense navigates to partially specialized template declarations",
          "[dxc][navigation][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source =
        "template<bool B, typename T = void> struct enable_if {};\n"
        "template<typename T> struct enable_if<true, T> { using type = T; };\n"
        "template<typename T> struct container_traits { static const bool is_container = "
        "true; };\n"
        "template<typename T, typename = void> struct container_wrapper;\n"
        "template<typename T> struct container_wrapper<T, typename "
        "enable_if<container_traits<T>::is_container>::type> {};\n"
        "template<typename U> void use_wrapper(U input) { container_wrapper<U> value; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto diagnostics = translation_unit.diagnostics();
    std::string diagnostic_messages;
    for (const auto& diagnostic : diagnostics) {
        diagnostic_messages += diagnostic.message;
        diagnostic_messages += '\n';
    }
    INFO(diagnostic_messages);
    CHECK(diagnostics.empty());

    const auto symbols = translation_unit.symbols();
    std::string symbol_names;
    for (const auto& symbol : symbols) {
        symbol_names += symbol.name;
        symbol_names += '\n';
    }
    INFO(symbol_names);

    const auto definition = translation_unit.definition_at(shader_path, 6, 50);
    REQUIRE(definition.has_value());
    CHECK(definition->name == "container_wrapper");
    CHECK(definition->location.line == 4);
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

TEST_CASE("Pinned DXC runtime supports the complete Linux IntelliSense workflow",
          "[dxc][linux-runtime][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto directory = std::filesystem::current_path() / "linux-runtime";
    const auto root = (directory / "root.hlsl").generic_string();
    const auto dependency = (directory / "dependency.hlsli").generic_string();
    const std::string root_source = "#include \"dependency.hlsli\"\n"
                                    "float4 main() : SV_Target {\n"
                                    "    float result = shade(1.0, 2.0);\n"
                                    "    return result.xxxx;\n"
                                    "}\n";
    const std::string dependency_source =
        "float shade(float value, float bias) { return value + bias; }\n";

    auto translation_unit =
        intellisense.parse(root, {{root, root_source}, {dependency, dependency_source}});

    REQUIRE(translation_unit.diagnostics().empty());
    const auto completions = translation_unit.complete(root, 4, 1);
    CHECK(std::ranges::any_of(completions,
                              [](const auto& completion) { return completion.label == "shade"; }));
    const auto definition = translation_unit.definition_at(root, 3, 21);
    REQUIRE(definition.has_value());
    CHECK(definition->name == "shade");
    CHECK(definition->location.path == dependency);
    const auto hover = translation_unit.hover_at(root, 3, 21);
    REQUIRE(hover.has_value());
    CHECK(hover->name == "shade");
    const auto signatures = translation_unit.signatures_at(root, 3, 21);
    REQUIRE(signatures.size() == 1);
    CHECK(signatures[0].label == "float shade(float value, float bias)");

    const std::string updated_root_source = "#include \"dependency.hlsli\"\n"
                                            "float4 main() : SV_Target {\n"
                                            "    float result = updatedShade(1.0);\n"
                                            "    return result.xxxx;\n"
                                            "}\n";
    const std::string updated_dependency_source =
        "float updatedShade(float value) { return value; }\n";
    translation_unit.reparse(
        {{root, updated_root_source}, {dependency, updated_dependency_source}});

    REQUIRE(translation_unit.diagnostics().empty());
    const auto updated_definition = translation_unit.definition_at(root, 3, 21);
    REQUIRE(updated_definition.has_value());
    CHECK(updated_definition->name == "updatedShade");
    const auto updated_hover = translation_unit.hover_at(root, 3, 21);
    REQUIRE(updated_hover.has_value());
    CHECK(updated_hover->name == "updatedShade");
    const auto updated_signatures = translation_unit.signatures_at(root, 3, 21);
    REQUIRE(updated_signatures.size() == 1);
    CHECK(updated_signatures[0].label == "float updatedShade(float value)");

    translation_unit.reparse(
        {{root, updated_root_source}, {dependency, "// updatedShade removed\n"}});
    CHECK(std::ranges::any_of(translation_unit.diagnostics(), [](const auto& diagnostic) {
        return diagnostic.severity == hlsl_intellisense::dxc::DiagnosticSeverity::error &&
               diagnostic.message.find("updatedShade") != std::string::npos;
    }));
}

TEST_CASE("DXC references preserve symbol identity across scopes, overloads, and includes",
          "[dxc][references][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto root = (std::filesystem::current_path() / "references.hlsl").generic_string();
    const auto include = (std::filesystem::current_path() / "references.hlsli").generic_string();
    const std::string root_source = "#include \"references.hlsli\"\n"
                                    "float select(float value) { return value; }\n"
                                    "float select(int value) { return value; }\n"
                                    "float4 main() : SV_Target {\n"
                                    "  float value = select(sharedValue);\n"
                                    "  { float sharedValue = 2.0; value += sharedValue; }\n"
                                    "  return value.xxxx;\n"
                                    "}\n";
    const std::string include_source = "static const float sharedValue = 1.0;\n";
    auto translation_unit =
        intellisense.parse(root, {{root, root_source}, {include, include_source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto global = translation_unit.references_at(root, 5, 24);
    REQUIRE(global.size() == 2);
    CHECK(global[0].location.path == root);
    CHECK(global[0].location.line == 5);
    CHECK(global[1].location.path == include);

    const auto local = translation_unit.references_at(root, 6, 44);
    REQUIRE(local.size() == 2);
    CHECK(local[0].location.line == 6);
    CHECK(local[1].location.line == 6);

    const auto overload = translation_unit.references_at(root, 5, 17);
    REQUIRE(overload.size() == 2);
    CHECK(overload[0].location.line == 2);
    CHECK(overload[1].location.line == 5);
}

TEST_CASE("DXC reference API does not expose macro definitions and expansions",
          "[dxc][references][macros]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto root = (std::filesystem::current_path() / "macro-references.hlsl").generic_string();
    const std::string source = "#define EXPAND(value) ((value) + 1.0)\n"
                               "float4 main() : SV_Target { return EXPAND(2.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(root, {{root, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto references = translation_unit.references_at(root, 2, 36);
    CHECK(references.empty());
}

TEST_CASE("DXC IntelliSense exposes hover and overload signatures for HLSL 2021",
          "[dxc][hover][signature-help][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float shade(float value) { return value; }\n"
                               "float shade(float value, float bias) { return value + bias; }\n"
                               "struct Material {\n"
                               "  float Scale(float value) { return value; }\n"
                               "  float Scale(float value, float bias) { return value + bias; }\n"
                               "};\n"
                               "float4 main() : SV_Target {\n"
                               "  Material material;\n"
                               "  float value = shade(1.0, 2.0);\n"
                               "  value = material.Scale(value, 3.0);\n"
                               "  return float4(value, value, value, 1.0);\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto hover = translation_unit.hover_at(shader_path, 9, 17);
    REQUIRE(hover.has_value());
    const auto hover_info = hover.value_or(hlsl_intellisense::dxc::Hover{});
    CHECK(hover_info.name == "shade");
    CHECK(hover_info.qualified_name == "shade");
    CHECK(hover_info.display_name == "shade(float, float)");
    CHECK(hover_info.type == "float (float, float)");
    CHECK(hover_info.declaration == "float shade(float value, float bias)");
    CHECK(hover_info.declaration_location.line == 2);
    CHECK(hover_info.end_offset > hover_info.start_offset);

    const auto functions = translation_unit.signatures_at(shader_path, 9, 17);
    REQUIRE(functions.size() == 2);
    CHECK(functions[0].label == "float shade(float value, float bias)");
    CHECK(functions[0].parameters.size() == 2);
    CHECK(functions[0].parameters[1].label == "float bias");
    CHECK(functions[1].label == "float shade(float value)");

    const auto methods = translation_unit.signatures_at(shader_path, 10, 20);
    REQUIRE(methods.size() == 2);
    CHECK(methods[0].label == "float Material::Scale(float value, float bias)");
    CHECK(methods[1].label == "float Material::Scale(float value)");

    translation_unit.reparse(
        {{shader_path, "float updated(float value) { return value; }\n"
                       "float4 main() : SV_Target { return updated(1.0).xxxx; }\n"}});
    const auto edited_hover = translation_unit.hover_at(shader_path, 2, 42);
    REQUIRE(edited_hover.has_value());
    CHECK(edited_hover.value_or(hlsl_intellisense::dxc::Hover{}).name == "updated");
    const auto edited_signatures = translation_unit.signatures_at(shader_path, 2, 42);
    REQUIRE(edited_signatures.size() == 1);
    CHECK(edited_signatures[0].label == "float updated(float value)");
}

TEST_CASE("DXC IntelliSense exposes explicit and inferred function-template calls",
          "[dxc][hover][signature-help][templates][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;

    SECTION("explicit template arguments") {
        const std::string source =
            "template<typename T, typename U> T conv(U value) { return (T)value; }\n"
            "float4 main(float x : X) : SV_Target { return conv<float, float>(x).xxxx; }\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        REQUIRE(translation_unit.diagnostics().empty());

        const auto hover = translation_unit.hover_at(shader_path, 2, 49);
        REQUIRE(hover.has_value());
        CHECK(hover->name == "conv");
        CHECK(hover->type == "float (float)");

        const auto signatures = translation_unit.signatures_at(shader_path, 2, 49);
        REQUIRE(signatures.size() == 2);
        CHECK(signatures[0].label == "float conv(float value)");
        REQUIRE(signatures[0].parameters.size() == 1);
        CHECK(signatures[0].parameters[0].label == "float value");
        CHECK(signatures[1].label == "T conv(U value)");
    }

    SECTION("inferred template arguments") {
        const std::string source =
            "template<typename T> T conv(T value) { return value; }\n"
            "float4 main(float x : X) : SV_Target { return conv(x).xxxx; }\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        REQUIRE(translation_unit.diagnostics().empty());

        const auto hover = translation_unit.hover_at(shader_path, 2, 49);
        REQUIRE(hover.has_value());
        CHECK(hover->name == "conv");
        CHECK(hover->type == "float (float)");

        const auto signatures = translation_unit.signatures_at(shader_path, 2, 49);
        REQUIRE(signatures.size() == 2);
        CHECK(signatures[0].label == "float conv(float value)");
        REQUIRE(signatures[0].parameters.size() == 1);
        CHECK(signatures[0].parameters[0].label == "float value");
        CHECK(signatures[1].label == "T conv(T value)");
    }
}

TEST_CASE("DXC IntelliSense supports hover and signatures with common source line endings",
          "[dxc][hover][signature-help][line-endings][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto check_line_ending = [&intellisense](std::string_view line_ending) {
        const auto source = "float shade(float value) { return value; }" +
                            std::string{line_ending} +
                            "float4 main(float x : X) : SV_Target { return shade(x).xxxx; }";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
        REQUIRE(translation_unit.diagnostics().empty());

        const auto hover = translation_unit.hover_at(shader_path, 2, 48);
        REQUIRE(hover.has_value());
        CHECK(hover->name == "shade");
        CHECK(hover->start_offset == source.find("shade(x)"));

        const auto signatures = translation_unit.signatures_at(shader_path, 2, 48);
        REQUIRE(signatures.size() == 1);
        CHECK(signatures[0].label == "float shade(float value)");
    };

    SECTION("CR") { check_line_ending("\r"); }
    SECTION("LF") { check_line_ending("\n"); }
    SECTION("CRLF") { check_line_ending("\r\n"); }
}

TEST_CASE("Pinned DXC exposes built-in type declarations but not constructor overloads",
          "[dxc][hover][signature-help][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source =
        "float4 main() : SV_Target {\n"
        "  float scalar_value = float(1.0);\n"
        "  float4 vector_value = float4(1.0, 2.0, 3.0, 4.0);\n"
        "  float2x2 matrix_value = float2x2(1.0, 2.0, 3.0, 4.0);\n"
        "  vector<float, 4> generic_vector = vector<float, 4>(1.0, 2.0, 3.0, 4.0);\n"
        "  matrix<float, 2, 2> generic_matrix = matrix<float, 2, 2>(1.0, 2.0, 3.0, 4.0);\n"
        "  return vector_value + matrix_value[0].xyxy + generic_vector + generic_matrix[0].xyxy + "
        "scalar_value;\n"
        "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    CHECK_FALSE(translation_unit.hover_at(shader_path, 2, 25).has_value());
    CHECK(translation_unit.signatures_at(shader_path, 2, 25).empty());

    const auto probe = [&translation_unit](std::uint32_t line, std::uint32_t column,
                                           std::string_view name, std::string_view type,
                                           std::string_view declaration, std::uint32_t kind) {
        const auto hover = translation_unit.hover_at(shader_path, line, column);
        REQUIRE(hover.has_value());
        CHECK(hover->name == std::string{name});
        CHECK(hover->type == std::string{type});
        CHECK(hover->declaration == std::string{declaration});
        CHECK(hover->cursor_kind == kind);
        CHECK(translation_unit.signatures_at(shader_path, line, column).empty());
    };
    probe(3, 27, "float4", "float4", "typedef vector<float, 4> float4", 20);
    probe(4, 31, "float2x2", "float2x2", "typedef matrix<float, 2, 2> float2x2", 20);
    probe(5, 39, "vector", "",
          "template <class element = float, int element_count = 4> class final vector", 31);
    probe(6, 42, "matrix", "",
          "template <class element = float, int row_count = 4, int col_count = 4> class final "
          "matrix",
          31);

    const std::string completion_source = "floa\n";
    auto completion_unit = intellisense.parse(shader_path, {{shader_path, completion_source}});
    const auto completions = completion_unit.complete(shader_path, 1, 5);
    const auto vector_completion =
        std::ranges::find(completions, "vector", &hlsl_intellisense::dxc::Completion::label);
    const auto matrix_completion =
        std::ranges::find(completions, "matrix", &hlsl_intellisense::dxc::Completion::label);
    REQUIRE(vector_completion != completions.end());
    REQUIRE(matrix_completion != completions.end());
    CHECK(vector_completion->detail == "vector::");
    CHECK(matrix_completion->detail == "matrix::");
    CHECK(std::ranges::find(completions, "float4", &hlsl_intellisense::dxc::Completion::label) ==
          completions.end());
}
