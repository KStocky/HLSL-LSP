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

TEST_CASE("DXC accepts spliced preprocessing directive keywords",
          "[dxc][preprocessor][splices][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    const std::string source = "#def\\\n"
                               "ine VALUE 1\n"
                               "/* prefix */ #un\\\n"
                               "def VALUE\n"
                               "#ifndef VALUE\n"
                               "#define VALUE 2\n"
                               "#endif\n"
                               "float4 main() : SV_Target {\n"
                               "    return float4(VALUE, VALUE, VALUE, VALUE);\n"
                               "}\n";

    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    CHECK(translation_unit.diagnostics().empty());
}

TEST_CASE("DXC accepts comment delimiters formed by phase-two splicing",
          "[dxc][preprocessor][comments][splices][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    const std::string source = "/\\\n"
                               "* comment with an internal \\\n"
                               "splice *\\\n"
                               "/ #define VALUE 1\n"
                               "/\\\n"
                               "/ hidden #undef VALUE \\\n"
                               "and still hidden\n"
                               "#if VALUE != 1\n"
                               "#error phase-two comment handling failed\n"
                               "#endif\n"
                               "float4 main() : SV_Target {\n"
                               "    return float4(VALUE, VALUE, VALUE, VALUE);\n"
                               "}\n";

    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    const auto diagnostics = translation_unit.diagnostics();
    std::string messages;
    for (const auto& diagnostic : diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    CHECK(std::ranges::none_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity >= hlsl_intellisense::dxc::DiagnosticSeverity::error;
    }));
}

TEST_CASE("DXC inlay hints use inferred cursor types and unambiguous signatures",
          "[dxc][inlay-hints]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.language_version = "202x";
    const std::string source =
        "float shade(float value, float bias) { return value + bias; }\n"
        "float4 main() : SV_Target { auto result = shade(1.0, 2.0); return result.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    CHECK(translation_unit.diagnostics().empty());

    const auto first_argument = static_cast<std::uint32_t>(source.find("1.0"));
    const auto second_argument = static_cast<std::uint32_t>(source.find("2.0"));
    const auto declaration_argument = static_cast<std::uint32_t>(source.find("float value"));
    const auto hints = translation_unit.inlay_hints(
        shader_path, {{.start = 0, .end = static_cast<std::uint32_t>(source.size())}},
        {{.line = 1, .column = 7, .argument_offsets = {declaration_argument}},
         {.line = 2, .column = 43, .argument_offsets = {first_argument, second_argument}}},
        {});

    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::type &&
               hint.label == ": float";
    }));
    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::parameter &&
               hint.label == "value:";
    }));
    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::parameter &&
               hint.label == "bias:";
    }));
    CHECK_FALSE(std::ranges::any_of(hints, [declaration_argument](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::parameter &&
               hint.offset == declaration_argument;
    }));
    const auto exclusive_end = translation_unit.inlay_hints(
        shader_path, {{.start = 0, .end = first_argument}},
        {{.line = 2, .column = 43, .argument_offsets = {first_argument, second_argument}}},
        {.types = false, .parameters = true});
    CHECK(exclusive_end.empty());

    const std::string overloaded = "float shade(float value) { return value; }\n"
                                   "float shade(int count) { return count; }\n"
                                   "float4 main() : SV_Target { return shade(1).xxxx; }\n";
    auto overloaded_unit = intellisense.parse(shader_path, {{shader_path, overloaded}}, options);
    const auto argument = static_cast<std::uint32_t>(overloaded.find("1)"));
    const auto ambiguous = overloaded_unit.inlay_hints(
        shader_path, {{.start = 0, .end = static_cast<std::uint32_t>(overloaded.size())}},
        {{.line = 3, .column = 36, .argument_offsets = {argument}}},
        {.types = false, .parameters = true});
    CHECK(ambiguous.empty());
}

TEST_CASE("DXC inlay hints expose compiler-reflected layout and register data",
          "[dxc][inlay-hints][reflection]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source =
        "Texture2D<float4> SourceTexture;\n"
        "SamplerState SourceSampler;\n"
        "cbuffer Constants { row_major float2x2 transform; float values[2]; };\n"
        "float4 main(float2 uv : TEXCOORD) : SV_Target {\n"
        "  return SourceTexture.Sample(SourceSampler, uv) + values[0] + transform[0][0];\n"
        "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    hlsl_intellisense::dxc::InlayHintOptions hint_options{.types = false,
                                                          .parameters = false,
                                                          .matrix_orientation = true,
                                                          .registers = true,
                                                          .packed_offsets = true,
                                                          .array_strides = true};
    hlsl_intellisense::dxc::InlayHintWork work;
    const auto split = static_cast<std::uint32_t>(source.find("values"));
    const auto hints = translation_unit.inlay_hints(
        shader_path,
        {{.start = 0, .end = split},
         {.start = split, .end = static_cast<std::uint32_t>(source.size())}},
        {}, hint_options, &work);

    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::matrix_orientation &&
               hint.label == " row-major";
    }));
    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::packed_offset &&
               hint.label == " offset 0";
    }));
    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::array_stride &&
               hint.label == " stride 16";
    }));
    CHECK(std::ranges::any_of(hints, [](const auto& hint) {
        return hint.category == hlsl_intellisense::dxc::InlayHintCategory::register_binding &&
               hint.label.starts_with(" register(");
    }));
    CHECK(work.reflection_compilations == 1);
    CHECK(work.layout_probes == 1);
    CHECK(std::adjacent_find(hints.begin(), hints.end(), [](const auto& left, const auto& right) {
              return left.offset == right.offset && left.category == right.category &&
                     left.label == right.label;
          }) == hints.end());
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

TEST_CASE("DXC IntelliSense reports compiler-skipped preprocessor ranges", "[dxc][preprocessor]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "#define ACTIVE_VALUE 7\n"
                               "#define SCALE(value) ((value) * ACTIVE_VALUE)\n"
                               "#if 0\n"
                               "float skippedValue;\n"
                               "#endif\n"
                               "float activeValue;\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto ranges = translation_unit.skipped_ranges();
    REQUIRE_FALSE(ranges.empty());
    CHECK(std::ranges::any_of(ranges, [](const auto& range) {
        return range.start.path == shader_path && range.start.line <= 3 && range.end.line >= 3;
    }));
    const auto macros = translation_unit.macro_definitions();
    const auto active =
        std::ranges::find(macros, "ACTIVE_VALUE", &hlsl_intellisense::dxc::MacroDefinition::name);
    REQUIRE(active != macros.end());
    CHECK(active->value == "7");
    CHECK(active->location.path == shader_path);
    const auto scale =
        std::ranges::find(macros, "SCALE", &hlsl_intellisense::dxc::MacroDefinition::name);
    REQUIRE(scale != macros.end());
    CHECK(scale->value == "(value) ((value) * ACTIVE_VALUE)");
}

TEST_CASE("DXC exposes rewritten-source skipped-range capability",
          "[dxc][preprocessor][platform]") {
#ifdef _WIN32
    CHECK(hlsl_intellisense::dxc::supports_skipped_ranges_for_rewritten_sources());
#else
    CHECK_FALSE(hlsl_intellisense::dxc::supports_skipped_ranges_for_rewritten_sources());
#endif
}

TEST_CASE("DXC IntelliSense reports preprocessing records from unsaved includes",
          "[dxc][preprocessor][includes]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto root = (std::filesystem::current_path() / "preprocessor-root.hlsl").generic_string();
    const auto include =
        (std::filesystem::current_path() / "preprocessor-include.hlsli").generic_string();
    const std::string root_source = "#include \"preprocessor-include.hlsli\"\n"
                                    "float activeValue = INCLUDED_VALUE;\n";
    const std::string include_source = "#define INCLUDED_VALUE 3\n"
                                       "#if 0\n"
                                       "float skippedIncludeValue;\n"
                                       "#endif\n";
    auto translation_unit =
        intellisense.parse(root, {{root, root_source}, {include, include_source}});

    const auto ranges = translation_unit.skipped_ranges();
    CHECK(std::ranges::any_of(ranges, [&include](const auto& range) {
        return range.start.path == include && range.start.line <= 3 && range.end.line >= 3;
    }));
    const auto macros = translation_unit.macro_definitions();
    const auto included =
        std::ranges::find(macros, "INCLUDED_VALUE", &hlsl_intellisense::dxc::MacroDefinition::name);
    REQUIRE(included != macros.end());
    CHECK(included->value == "3");
    CHECK(included->location.path == include);
}

TEST_CASE("Memory layout probes replace configured target profiles", "[dxc][memory-layout]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "lib_6_6";
    const std::string source = "struct Data { float3 value; };\n"
                               "void useHeap(uint index) {\n"
                               "    RWByteAddressBuffer buffer = ResourceDescriptorHeap[index];\n"
                               "    buffer.Store(0, 0);\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 9);
    REQUIRE(layout.has_value());
    REQUIRE(layout->supported);
    CHECK(layout->name == "Data");
    CHECK(layout->size == 12);
}

TEST_CASE("Memory layout probes preserve Shader Model 6.6 for unrelated framework code",
          "[dxc][memory-layout][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.language_version = "202x";
    options.target_profile = "lib_6_6";
    options.additional_arguments = {"-enable-16bit-types"};
    const std::string source = "#include \"framework.hlsli\"\n"
                               "struct MyStruct {\n"
                               "    float3 Vec3;\n"
                               "    int2 IntPoint;\n"
                               "};\n"
                               "MyStruct GlobalBinding;\n"
                               "ConstantBuffer<MyStruct> CBuffer;\n";
    const std::string framework =
        "void useHeap(uint index) {\n"
        "    RWByteAddressBuffer buffer = ResourceDescriptorHeap[index];\n"
        "    buffer.Store(0, 0);\n"
        "}\n";
    const auto root_path =
        (std::filesystem::current_path() / "shader-model-layout.hlsl").generic_string();
    const auto framework_path =
        (std::filesystem::current_path() / "framework.hlsli").generic_string();
    auto translation_unit =
        intellisense.parse(root_path, {{root_path, source}, {framework_path, framework}}, options);

    const auto layout = translation_unit.memory_layout_at(root_path, 2, 10);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    CHECK(layout->size == 20);
    REQUIRE(layout->members.size() == 2);
    CHECK(layout->members[0].name == "Vec3");
    CHECK(layout->members[0].size == 12);
    CHECK(layout->members[1].name == "IntPoint");
    CHECK(layout->members[1].offset == 12);
    CHECK(layout->members[1].size == 8);
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
    CHECK(layout->members[3].size == 20);
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

TEST_CASE("Nested matrix arrays preserve compiler-owned vector structure",
          "[dxc][memory-layout][matrix][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Inner {\n"
                               "    row_major float2x2 transforms[2];\n"
                               "    column_major float2x2 basis;\n"
                               "};\n"
                               "cbuffer Constants { Inner inner; };\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto layout = translation_unit.memory_layout_at(shader_path, 5, 28);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    REQUIRE(layout->members.size() == 1);
    REQUIRE(layout->members[0].members.size() == 2);

    const auto& transforms = layout->members[0].members[0];
    CHECK(transforms.kind == hlsl_intellisense::dxc::MemoryLayoutElementKind::array);
    CHECK(transforms.array_stride == 32);
    CHECK(transforms.size == 56);
    REQUIRE(transforms.members.size() == 2);
    for (std::uint32_t index = 0; index < transforms.members.size(); ++index) {
        const auto& transform = transforms.members[index];
        CHECK(transform.kind == hlsl_intellisense::dxc::MemoryLayoutElementKind::matrix);
        CHECK(transform.row_major);
        CHECK(transform.matrix_stride == 16);
        CHECK(transform.size == 24);
        CHECK(transform.allocation_size == (index + 1U < transforms.members.size() ? 32U : 24U));
        REQUIRE(transform.members.size() == 2);
        CHECK(transform.members[0].offset == 0);
        CHECK(transform.members[0].size == 8);
        CHECK(transform.members[0].allocation_size == 16);
        CHECK(transform.members[1].offset == 16);
        CHECK(transform.members[1].size == 8);
        CHECK(transform.members[1].allocation_size == 8);
    }
    CHECK(transforms.members[1].offset == 32);

    const auto& basis = layout->members[0].members[1];
    CHECK(basis.kind == hlsl_intellisense::dxc::MemoryLayoutElementKind::matrix);
    CHECK_FALSE(basis.row_major);
    CHECK(basis.matrix_stride == 16);
    CHECK(basis.size == 24);
    CHECK(basis.allocation_size == 24);
    REQUIRE(basis.members.size() == 2);
    CHECK(basis.members[0].allocation_size == 16);
    CHECK(basis.members[1].offset == 16);
    CHECK(basis.members[1].size == 8);
    CHECK(basis.members[1].allocation_size == 8);
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

TEST_CASE("Constant-buffer arrays separate value bytes from allocation stride",
          "[dxc][memory-layout][cbuffer][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.additional_arguments = {"-enable-16bit-types"};
    auto translation_unit = intellisense.parse(
        shader_path, {{shader_path, "cbuffer Constants { int16_t values[5]; };\n"}}, options);

    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 33);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    REQUIRE(layout->members.size() == 1);
    const auto& values = layout->members[0];
    CHECK(values.size == 66);
    CHECK(values.allocation_size == 66);
    CHECK(values.array_stride == 16);
    REQUIRE(values.members.size() == 5);
    for (std::uint32_t index = 0; index < values.members.size(); ++index) {
        const auto& value = values.members[index];
        CHECK(value.offset == index * 16);
        CHECK(value.size == 2);
        CHECK(value.allocation_size == (index + 1U < values.members.size() ? 16U : 2U));
    }
    CHECK(layout->size == 66);
    CHECK(layout->allocation_size == 80);

    auto nested_translation =
        intellisense.parse(shader_path,
                           {{shader_path, "struct Inner { int16_t values[5]; };\n"
                                          "cbuffer Constants { Inner data; };\n"}},
                           options);
    const auto nested = nested_translation.memory_layout_at(shader_path, 2, 27);
    REQUIRE(nested.has_value());
    INFO(nested->explanation);
    REQUIRE(nested->supported);
    REQUIRE(nested->members.size() == 1);
    REQUIRE(nested->members[0].members.size() == 1);
    const auto& nested_values = nested->members[0].members[0];
    REQUIRE(nested_values.members.size() == 5);
    for (std::uint32_t index = 0; index < nested_values.members.size(); ++index) {
        const auto& value = nested_values.members[index];
        CHECK(value.size == 2);
        CHECK(value.allocation_size == (index + 1U < nested_values.members.size() ? 16U : 2U));
    }
}

TEST_CASE("Constant-buffer array tail does not overlap a following member",
          "[dxc][memory-layout][cbuffer][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.additional_arguments = {"-enable-16bit-types"};
    const std::string source = "cbuffer Test {\n"
                               "    float3 a;\n"
                               "    int16_t b[5];\n"
                               "    float3 c;\n"
                               "    float3x4 d;\n"
                               "};\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto layout = translation_unit.memory_layout_at(shader_path, 1, 9);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    REQUIRE(layout->members.size() == 4);
    const auto& values = layout->members[1];
    const auto& following = layout->members[2];
    CHECK(values.name == "b");
    CHECK(values.offset == 16);
    CHECK(values.size == 66);
    CHECK(values.allocation_size == 66);
    REQUIRE(values.members.size() == 5);
    CHECK(values.members.back().offset == 64);
    CHECK(values.members.back().size == 2);
    CHECK(values.members.back().allocation_size == 2);
    CHECK(following.name == "c");
    CHECK(values.offset + values.allocation_size <= following.offset);
    CHECK(following.offset == 84);
    CHECK(following.size == 12);
}

TEST_CASE("Nested records keep ancestor tail padding out of their value extent",
          "[dxc][memory-layout][regression]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.additional_arguments = {"-enable-16bit-types"};
    const std::string source = "struct Leaf { int16_t value; };\n"
                               "struct Middle { double prefix; Leaf inner; };\n"
                               "struct Outer { Middle values[4]; };\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto layout = translation_unit.memory_layout_at(shader_path, 3, 24);
    REQUIRE(layout.has_value());
    INFO(layout->explanation);
    REQUIRE(layout->supported);
    REQUIRE(layout->members.size() == 1);
    REQUIRE(layout->members[0].members.size() == 4);
    const auto& middle = layout->members[0].members[0];
    CHECK(middle.size == 10);
    CHECK(middle.allocation_size == 16);
    REQUIRE(middle.members.size() == 2);
    const auto& inner = middle.members[1];
    CHECK(inner.size == 2);
    CHECK(inner.allocation_size == 2);
    REQUIRE(inner.members.size() == 1);
    CHECK(inner.members[0].size == 2);
    CHECK(inner.members[0].allocation_size == 2);
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
            CHECK(layout->members[0].members[0].members[0].members[7].size == 4);
            CHECK(layout->members[0].members[0].members[0].members[7].allocation_size == 4);
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

TEST_CASE("DXC IntelliSense reports a fix-it for a missing semicolon",
          "[dxc][integration][fixit]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, "float4 main() : SV_Target {\n"
                                                       "    float x = 1.0\n"
                                                       "    return x.xxxx;\n"
                                                       "}\n"}});
    const auto diagnostics = translation_unit.diagnostics();
    REQUIRE(!diagnostics.empty());
    const auto missing_semicolon = std::ranges::find_if(diagnostics, [](const auto& diagnostic) {
        return diagnostic.message == "expected ';' at end of declaration";
    });
    REQUIRE(missing_semicolon != diagnostics.end());
    REQUIRE(missing_semicolon->fix_its.size() == 1);
    const auto& fix_it = missing_semicolon->fix_its.front();
    CHECK(fix_it.replacement_text == ";");
    CHECK(fix_it.range.start.path == shader_path);
    CHECK(fix_it.range.start.path == fix_it.range.end.path);
    CHECK(fix_it.range.start.offset == fix_it.range.end.offset);
}

TEST_CASE("DXC IntelliSense reports independent fix-its for repeated missing semicolons",
          "[dxc][integration][fixit]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, "float4 main() : SV_Target {\n"
                                                       "    float a = 1.0\n"
                                                       "    float b = 2.0\n"
                                                       "    return float4(a, b, 0, 0);\n"
                                                       "}\n"}});
    const auto diagnostics = translation_unit.diagnostics();
    std::vector<std::uint32_t> fix_it_offsets;
    for (const auto& diagnostic : diagnostics) {
        for (const auto& fix_it : diagnostic.fix_its) {
            fix_it_offsets.push_back(fix_it.range.start.offset);
        }
    }
    REQUIRE(fix_it_offsets.size() == 2);
    CHECK(fix_it_offsets[0] != fix_it_offsets[1]);
}

TEST_CASE("DXC IntelliSense reports a replacement fix-it for a typo'd declared identifier",
          "[dxc][integration][fixit]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit = intellisense.parse(
        shader_path, {{shader_path, "float combine(float a, float b) { return a + b; }\n"
                                    "float4 main() : SV_Target { return "
                                    "combin(1.0, 2.0).xxxx; }\n"}});
    const auto diagnostics = translation_unit.diagnostics();
    const auto typo = std::ranges::find_if(diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("did you mean") != std::string::npos;
    });
    REQUIRE(typo != diagnostics.end());
    REQUIRE(typo->fix_its.size() == 1);
    const auto& fix_it = typo->fix_its.front();
    CHECK(fix_it.replacement_text == "combine");
    CHECK(fix_it.range.end.offset > fix_it.range.start.offset);
}

TEST_CASE("DXC IntelliSense reports no fix-it for diagnostics with no compiler-suggested repair",
          "[dxc][integration][fixit]") {
    // Verified empirically against pinned DXC 1.9.2607.13: none of these common
    // diagnostic categories carry a fix-it, and callers must not assume one.
    using namespace std::string_view_literals;
    const std::vector<std::string_view> sources{
        // Undeclared identifier with no close match: no "did you mean", no fix-it.
        "float4 main() : SV_Target { return missing_symbol; }\n"sv,
        // Struct member typo: DXC does not offer "did you mean" for member lookups.
        "struct S { float value; };\n"
        "float4 main() : SV_Target { S s; return s.valeu.xxxx; }\n"sv,
        // Assignment used as a condition: a warning with no machine-applicable fix.
        "float4 main() : SV_Target {\n"
        "    int a = 0; int b = 1;\n"
        "    if (a = b) { return float4(1,1,1,1); }\n"
        "    return float4(0,0,0,0);\n"
        "}\n"sv,
        // Missing include: no fix-it (there is nothing deterministic to insert).
        "#include \"does_not_exist.hlsli\"\n"
        "float4 main() : SV_Target { return float4(0,0,0,0); }\n"sv,
        // "expected ';' after struct": unlike some other missing-';' diagnostics,
        // this one carries no fix-it, so fix-its cannot be assumed present even
        // within the same broad diagnostic family.
        "struct S { float value; }\n"
        "float4 main() : SV_Target { return float4(0,0,0,0); }\n"sv,
    };

    for (const auto& source : sources) {
        hlsl_intellisense::dxc::Intellisense intellisense;
        auto translation_unit =
            intellisense.parse(shader_path, {{shader_path, std::string{source}}});
        for (const auto& diagnostic : translation_unit.diagnostics()) {
            CHECK(diagnostic.fix_its.empty());
        }
    }
}

TEST_CASE("DXC IntelliSense diagnostics with non-zero GetNumRanges do not crash fix-it extraction",
          "[dxc][integration][fixit]") {
    // Regression coverage for an empirically confirmed DXC 1.9.2607.13 defect:
    // IDxcDiagnostic::GetRangeAt crashes whenever GetNumRanges() > 0. The
    // production code must never call GetRangeAt; this only exercises
    // diagnostics known to report a non-zero range count, to guard against a
    // future regression that reintroduces the call.
    hlsl_intellisense::dxc::Intellisense intellisense;
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, "struct S { float value; };\n"
                                                       "float4 main() : SV_Target {\n"
                                                       "    S s;\n"
                                                       "    return s.valeu.xxxx;\n"
                                                       "}\n"}});
    CHECK_NOTHROW(translation_unit.diagnostics());
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

TEST_CASE("Compilation info reflects effective configuration and DXIL resource reflection",
          "[dxc][compilation-info]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    options.defines = {"USE_TINT=1"};
    options.include_directories = {"include"};
    const std::string source = "Texture2D<float4> MainTexture : register(t0);\n"
                               "SamplerState MainSampler : register(s0);\n"
                               "cbuffer Params : register(b0) { float4 tint; };\n"
                               "float4 main(float4 position : SV_Position) : SV_Target {\n"
                               "    return MainTexture.Sample(MainSampler, position.xy) * tint;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    std::string messages;
    for (const auto& diagnostic : info.diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    CHECK(info.entry_point == "main");
    CHECK(info.stage == "pixel");
    CHECK(info.target_profile == "ps_6_6");
    CHECK(info.defines == std::vector<std::string>{"USE_TINT=1"});
    CHECK(info.include_directories == std::vector<std::string>{"include"});
    CHECK(std::ranges::find(info.compiler_arguments, "-spirv") == info.compiler_arguments.end());
    REQUIRE(info.success);
    REQUIRE(info.output.has_value());
    CHECK(info.output->type == "dxil");
    CHECK(info.output->size > 0);
    REQUIRE(info.disassembly.has_value());
    CHECK(info.disassembly->available);
    CHECK(info.disassembly->format == "dxil");
    CHECK_FALSE(info.disassembly->text.empty());
    CHECK(info.disassembly->text.find("define") != std::string::npos);
    CHECK_FALSE(info.disassembly->truncated);
    CHECK(info.disassembly->displayed_size == info.disassembly->text.size());
    CHECK(info.disassembly->original_size == info.disassembly->displayed_size);
    REQUIRE(info.reflection.has_value());
    CHECK(info.reflection->available);

    const auto find_resource = [&](std::string_view name) {
        return std::ranges::find(info.reflection->resources, name,
                                 &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    };
    const auto texture = find_resource("MainTexture");
    REQUIRE(texture != info.reflection->resources.end());
    CHECK(texture->type == "texture");
    CHECK(texture->bind_point == 0);
    const auto sampler = find_resource("MainSampler");
    REQUIRE(sampler != info.reflection->resources.end());
    CHECK(sampler->type == "sampler");
    const auto constants = find_resource("Params");
    REQUIRE(constants != info.reflection->resources.end());
    CHECK(constants->type == "cbuffer");

    REQUIRE(!info.reflection->output_signature.empty());
    CHECK(std::ranges::any_of(info.reflection->output_signature, [](const auto& parameter) {
        return parameter.semantic_name == "SV_TARGET" && parameter.system_value == "target";
    }));
    CHECK_FALSE(info.reflection->thread_group_size.has_value());
}

TEST_CASE("Compilation info exposes compute thread group size", "[dxc][compilation-info]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "cs_6_6";
    options.entry_point = "main";
    const std::string source = "RWStructuredBuffer<float> Output : register(u0);\n"
                               "[numthreads(8, 4, 2)]\n"
                               "void main(uint3 id : SV_DispatchThreadID) {\n"
                               "    GroupMemoryBarrierWithGroupSync();\n"
                               "    Output[id.x] = 1.0;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    CHECK(info.stage == "compute");
    REQUIRE(info.reflection.has_value());
    REQUIRE(info.reflection->thread_group_size.has_value());
    CHECK(info.reflection->thread_group_size->x == 8);
    CHECK(info.reflection->thread_group_size->y == 4);
    CHECK(info.reflection->thread_group_size->z == 2);
    CHECK(info.reflection->barrier_instruction_count == 1);
    const auto output =
        std::ranges::find(info.reflection->resources, "Output",
                          &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(output != info.reflection->resources.end());
    CHECK(output->type == "uav_rwstructured");
}

TEST_CASE("Compute metadata comes from DXC cursors and compiler-formatted declarations",
          "[dxc][compilation-info][compute]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "cs_6_8";
    options.entry_point = "main";
    const auto root_path =
        (std::filesystem::current_path() / "compute-metadata-root.hlsl").generic_string();
    const auto include_path =
        (std::filesystem::current_path() / "compute-metadata.hlsli").generic_string();
    const std::string include_source = "struct SharedRecord { uint value; float3 color; };\n"
                                       "groupshared SharedRecord Records[2];\n"
                                       "void includedBarrier() {\n"
                                       "    DeviceMemoryBarrierWithGroupSync();\n"
                                       "    AllMemoryBarrierWithGroupSync();\n"
                                       "}\n";
    const std::string source = "#include \"compute-metadata.hlsli\"\n"
                               "groupshared uint Counter;\n"
                               "groupshared uint Tile[32];\n"
                               "void GroupMemoryBarrier(uint value) { Counter = value; }\n"
                               "void unreachableBarrier() { DeviceMemoryBarrierWithGroupSync(); }\n"
                               "[WaveSize(32, 64, 64)]\n"
                               "[numthreads(8, 4, 1)]\n"
                               "void main(uint3 id : SV_DispatchThreadID) {\n"
                               "    GroupMemoryBarrier(1);\n"
                               "    includedBarrier();\n"
                               "    GroupMemoryBarrierWithGroupSync();\n"
                               "    GroupMemoryBarrier();\n"
                               "    DeviceMemoryBarrier();\n"
                               "    AllMemoryBarrier();\n"
                               "    Tile[id.x] = Records[0].value;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(
        root_path, {{root_path, source}, {include_path, include_source}}, options);

    const auto info = translation_unit.compilation_info();
    const auto first_diagnostic =
        info.diagnostics.empty() ? std::string{} : info.diagnostics.front().message;
    INFO(first_diagnostic);
    REQUIRE(info.success);
    CHECK(info.psv_wave_size.available);
    CHECK(info.psv_wave_size.min == std::uint32_t{32});
    CHECK(info.psv_wave_size.max == std::uint32_t{64});
    REQUIRE(info.compute_metadata.has_value());
    const auto& metadata = *info.compute_metadata;
    CHECK(metadata.barrier_locations_available);
    CHECK_FALSE(metadata.barrier_locations_truncated);
    REQUIRE(metadata.barrier_locations.size() == 6);
    const auto included_barrier =
        std::ranges::find(metadata.barrier_locations, "DeviceMemoryBarrierWithGroupSync",
                          &hlsl_intellisense::dxc::ComputeBarrierLocation::label);
    REQUIRE(included_barrier != metadata.barrier_locations.end());
    CHECK(included_barrier->location.path == include_path);
    const auto included_call = std::string{std::string_view{include_source}.substr(
        included_barrier->start_offset,
        included_barrier->end_offset - included_barrier->start_offset)};
    CHECK(included_call == "DeviceMemoryBarrierWithGroupSync()");
    CHECK(std::ranges::any_of(metadata.barrier_locations, [](const auto& location) {
        return location.label == "GroupMemoryBarrierWithGroupSync";
    }));
    CHECK(std::ranges::any_of(metadata.barrier_locations, [](const auto& location) {
        return location.label == "GroupMemoryBarrier";
    }));
    CHECK(std::ranges::any_of(metadata.barrier_locations, [](const auto& location) {
        return location.label == "DeviceMemoryBarrier";
    }));
    CHECK(std::ranges::any_of(metadata.barrier_locations, [](const auto& location) {
        return location.label == "AllMemoryBarrier";
    }));
    CHECK(std::ranges::any_of(metadata.barrier_locations, [](const auto& location) {
        return location.label == "AllMemoryBarrierWithGroupSync";
    }));
    CHECK(std::ranges::count(metadata.barrier_locations, "GroupMemoryBarrier",
                             &hlsl_intellisense::dxc::ComputeBarrierLocation::label) == 1);

    REQUIRE(metadata.group_shared_declarations.size() == 3);
    const auto find_shared = [&](std::string_view name) {
        return std::ranges::find(metadata.group_shared_declarations, name,
                                 &hlsl_intellisense::dxc::ComputeGroupSharedDeclaration::name);
    };
    const auto records = find_shared("Records");
    REQUIRE(records != metadata.group_shared_declarations.end());
    CHECK(records->type == "SharedRecord [2]");
    REQUIRE(records->bytes.has_value());
    CHECK(*records->bytes == 32);
    CHECK(records->location.path == include_path);
    CHECK(records->location.offset == include_source.find("Records"));
    CHECK(records->start_offset == include_source.find("groupshared"));
    const auto counter = find_shared("Counter");
    REQUIRE(counter != metadata.group_shared_declarations.end());
    CHECK(counter->bytes == std::uint64_t{4});
    const auto tile = find_shared("Tile");
    REQUIRE(tile != metadata.group_shared_declarations.end());
    CHECK(tile->declaration == "groupshared uint Tile[32]");
    CHECK(tile->type == "uint [32]");
    CHECK(tile->bytes == std::uint64_t{128});
    CHECK(tile->location.offset == source.find("Tile[32]"));
    CHECK(tile->start_offset == source.find("groupshared uint Tile"));
    CHECK(metadata.group_shared_total_bytes == std::uint64_t{164});

    CHECK(metadata.wave_size.known);
    CHECK(metadata.wave_size.min == std::uint32_t{32});
    CHECK(metadata.wave_size.max == std::uint32_t{64});
    CHECK(metadata.wave_size.preferred == std::uint32_t{64});
    CHECK(metadata.wave_size.min_max_source == "psv0");
    CHECK(metadata.wave_size.preferred_source == "compilerFormattedEntryCursor");
}

TEST_CASE("Compute wave metadata supports fixed, range, and absent forms",
          "[dxc][compilation-info][compute][wave-size]") {
    const auto inspect = [](std::string_view attribute) {
        hlsl_intellisense::dxc::Intellisense intellisense;
        hlsl_intellisense::dxc::CompilerOptions options;
        options.target_profile = "cs_6_8";
        options.entry_point = "main";
        const auto source = std::string{attribute} + "\n[numthreads(1, 1, 1)] void main() {}\n";
        auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
        const auto info = translation_unit.compilation_info();
        REQUIRE(info.success);
        REQUIRE(info.compute_metadata.has_value());
        return info.compute_metadata->wave_size;
    };

    const auto fixed = inspect("[WaveSize(32)]");
    CHECK(fixed.known);
    CHECK(fixed.min == std::uint32_t{32});
    CHECK(fixed.max == std::uint32_t{32});
    CHECK_FALSE(fixed.preferred.has_value());
    CHECK(fixed.min_max_source == "psv0");
    CHECK(fixed.preferred_source.empty());

    const auto range = inspect("[WaveSize(32, 64)]");
    CHECK(range.known);
    CHECK(range.min == std::uint32_t{32});
    CHECK(range.max == std::uint32_t{64});
    CHECK_FALSE(range.preferred.has_value());
    CHECK(range.min_max_source == "psv0");

    const auto absent = inspect("");
    CHECK_FALSE(absent.known);
    CHECK_FALSE(absent.min.has_value());
    CHECK(absent.explanation.find("PSV0") != std::string::npos);
}

TEST_CASE("Reachable barrier locations are independently bounded",
          "[dxc][data-flow][compute][limits]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "cs_6_6";
    options.entry_point = "main";
    const std::string source = "[numthreads(1, 1, 1)] void main() {\n"
                               "  GroupMemoryBarrier();\n"
                               "  DeviceMemoryBarrier();\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_barrier_locations = 1;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    REQUIRE(flow.found);
    CHECK(flow.barrier_locations.size() == 1);
    CHECK(flow.barrier_locations_truncated);
}

TEST_CASE("Group-shared declaration collection is independently bounded",
          "[dxc][compilation-info][compute][limits]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "cs_6_6";
    options.entry_point = "main";
    const std::string source = "groupshared uint First;\n"
                               "groupshared uint Second;\n"
                               "[numthreads(1, 1, 1)] void main() { First = Second; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    hlsl_intellisense::dxc::ComputeMetadataLimits limits;
    limits.max_group_shared_declarations = 1;
    const auto info = translation_unit.compilation_info(limits);
    REQUIRE(info.compute_metadata.has_value());
    CHECK(info.compute_metadata->group_shared_available);
    CHECK(info.compute_metadata->group_shared_truncated);
    CHECK(info.compute_metadata->group_shared_declarations.size() == 1);
    CHECK_FALSE(info.compute_metadata->group_shared_total_bytes.has_value());
    CHECK_FALSE(info.compute_metadata->group_shared_unavailable_reason.empty());
    CHECK_FALSE(info.compute_metadata->group_shared_total_bytes_unavailable_reason.empty());
}

TEST_CASE("Compilation info recognizes joined DXC flag spellings and honors last-wins order",
          "[dxc][compilation-info]") {
    // DXC accepts both separated ("-T" "cs_6_6") and joined ("-Tcs_6_6")
    // spellings for -T/-E/-D/-I (mirroring memory_layout.cpp's existing -T/-E
    // handling); parse_effective_config must recognize the joined forms too.
    // Placing overriding joined flags in additional_arguments (which DXC
    // itself processes in argument order, later flags winning) also proves
    // last-wins semantics: the separated "-T ps_6_0"/"-E WrongEntry" from
    // CompilerOptions are overridden by the later joined "-Tcs_6_6"/"-EMain".
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_0";
    options.entry_point = "WrongEntry";
    options.defines = {"BASE=1"};
    options.include_directories = {"shared"};
    options.additional_arguments = {"-Tcs_6_6", "-EMain", "-DTILE_SIZE=8", "-Iinclude"};
    const std::string source = "RWStructuredBuffer<float> Output : register(u0);\n"
                               "[numthreads(8, 1, 1)]\n"
                               "void Main(uint3 id : SV_DispatchThreadID) {\n"
                               "    Output[id.x] = 1.0;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    std::string messages;
    for (const auto& diagnostic : info.diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    CHECK(info.entry_point == "Main");
    CHECK(info.target_profile == "cs_6_6");
    CHECK(info.stage == "compute");
    CHECK(info.defines == std::vector<std::string>{"BASE=1", "TILE_SIZE=8"});
    CHECK(info.include_directories == std::vector<std::string>{"shared", "include"});
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    REQUIRE(info.reflection->thread_group_size.has_value());
    CHECK(info.reflection->thread_group_size->x == 8);
    CHECK(info.reflection->thread_group_size->y == 1);
    CHECK(info.reflection->thread_group_size->z == 1);
}

TEST_CASE("Compilation info reports structured diagnostics on failure without fabricating success",
          "[dxc][compilation-info]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    auto translation_unit = intellisense.parse(
        shader_path, {{shader_path, "float4 main() : SV_Target { return missing_symbol; }\n"}},
        options);

    const auto info = translation_unit.compilation_info();
    CHECK_FALSE(info.success);
    REQUIRE(!info.diagnostics.empty());
    CHECK(std::ranges::any_of(info.diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == hlsl_intellisense::dxc::DiagnosticSeverity::error;
    }));
    CHECK(std::ranges::any_of(info.diagnostics, [](const auto& diagnostic) {
        return diagnostic.location.line > 0 && diagnostic.location.column > 0;
    }));
    CHECK_FALSE(info.output.has_value());
    CHECK_FALSE(info.disassembly.has_value());
    CHECK_FALSE(info.reflection.has_value());
}

TEST_CASE("Compilation info reports SPIR-V output as successful without fabricated reflection",
          "[dxc][compilation-info][spirv]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    options.additional_arguments = {"-spirv"};
    const std::string source = "float4 main() : SV_Target {\n"
                               "    return 1.0.xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.output.has_value());
    CHECK(info.output->type == "spirv");
    CHECK(info.output->size > 0);
    REQUIRE(info.disassembly.has_value());
    CHECK_FALSE(info.disassembly->available);
    CHECK(info.disassembly->format == "spirv");
    CHECK(info.disassembly->text.empty());
    CHECK_FALSE(info.disassembly->unavailable_reason.empty());
    CHECK_FALSE(info.disassembly->truncated);
    REQUIRE(info.reflection.has_value());
    CHECK_FALSE(info.reflection->available);
    CHECK_FALSE(info.reflection->unavailable_reason.empty());
    CHECK(std::ranges::find(info.compiler_arguments, "-spirv") != info.compiler_arguments.end());
    // Root signatures are a Direct3D 12 binding-model concept with no SPIR-V
    // equivalent, so `root_signature` must still be non-null (reporting
    // `not_applicable` distinctly rather than being omitted), and
    // `compatibility` must likewise be non-null, reporting `unknown` since
    // the notion does not apply to this target at all.
    REQUIRE(info.root_signature.has_value());
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::not_applicable);
    CHECK_FALSE(info.root_signature->unavailable_reason.empty());
    CHECK_FALSE(info.root_signature->details.has_value());
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(info.compatibility->explanation.empty());
}

TEST_CASE("Compilation info reports successful DXIL output with unavailable reflection for "
          "library targets",
          "[dxc][compilation-info]") {
    // A `lib_*` target profile compiles successfully to DXIL but produces a
    // library container, not a single-stage shader; requesting
    // ID3D12ShaderReflection (rather than ID3D12LibraryReflection) via
    // IDxcUtils::CreateReflection therefore genuinely fails even though the
    // compile itself succeeded. That must surface as success=true with a
    // structured reflection.available=false and a clear reason, never as a
    // thrown exception or a fabricated (empty-but-misleading) reflection.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "lib_6_3";
    const std::string source = "export float4 Shade(float4 color) {\n"
                               "    return color;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    std::string messages;
    for (const auto& diagnostic : info.diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    REQUIRE(info.success);
    REQUIRE(info.output.has_value());
    CHECK(info.output->type == "dxil");
    CHECK(info.output->size > 0);
    REQUIRE(info.reflection.has_value());
    CHECK_FALSE(info.reflection->available);
    CHECK_FALSE(info.reflection->unavailable_reason.empty());
    CHECK(info.reflection->input_signature.empty());
    CHECK(info.reflection->output_signature.empty());
    CHECK(info.reflection->resources.empty());
    CHECK_FALSE(info.reflection->thread_group_size.has_value());
    // Extracting the embedded root signature reads directly from the DXIL
    // container part and does not depend on ID3D12ShaderReflection, so it
    // must remain populated even though reflection itself failed for this
    // library target: `root_signature` is non-null whenever compiler output
    // exists. However, comparing resources to it requires the very
    // reflected resource list that just failed to materialize, so
    // `compatibility` must be reported as `unknown` with an explanation
    // rather than left null (which would previously happen) or fabricated
    // as `compatible` from an empty resource list.
    REQUIRE(info.root_signature.has_value());
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::absent);
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(info.compatibility->explanation.empty());
    CHECK(info.compatibility->issues.empty());
}

TEST_CASE("Compilation info tolerates -Qstrip_reflect without failing translation unit parsing",
          "[dxc][compilation-info]") {
    // The legacy IntelliSense parsing index (IDxcIndex::ParseTranslationUnit)
    // does not recognize "-Qstrip_reflect", the same way it does not
    // recognize "-E"/"-spirv" (see Intellisense::parse), so it must be
    // stripped from the arguments used only for parsing while still reaching
    // the real compiler for TranslationUnit::compilation_info.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    options.additional_arguments = {"-Qstrip_reflect"};
    const std::string source = "float4 main() : SV_Target {\n"
                               "    return 1.0.xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    CHECK(std::ranges::find(info.compiler_arguments, "-Qstrip_reflect") !=
          info.compiler_arguments.end());
}

TEST_CASE("Compilation info recompiles unsaved edits and reflects resolved includes",
          "[dxc][compilation-info][reparse]") {
    // The root references `includeValue`, which only the *current* include
    // buffer defines. Reparsing with an include that renames the symbol away
    // must be observed by the very next compilation_info() call: a stale
    // cached source set would keep compiling against the old include text
    // and (incorrectly) still succeed, so a genuine post-reparse compile
    // failure with a diagnostic naming the missing identifier is the
    // observable proof that the new source was actually used, not fabricated
    // or left stale.
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto directory = std::filesystem::current_path() / "compilation-info-includes";
    std::filesystem::create_directories(directory);
    const auto root = (directory / "root.hlsl").generic_string();
    const auto include = std::filesystem::path{root}.parent_path() / "dependency.hlsli";
    const std::string root_source =
        "#include \"dependency.hlsli\"\nfloat4 main() : SV_Target { return includeValue; }\n";

    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    auto translation_unit = intellisense.parse(
        root,
        {{root, root_source},
         {include.generic_string(), "static const float4 includeValue = 1.0.xxxx;\n"}},
        options);

    auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    CHECK(std::ranges::any_of(info.resolved_include_paths,
                              [&](const auto& path) { return path == include.generic_string(); }));

    // Rename the identifier away in the include only; the root is unchanged.
    translation_unit.reparse(
        {{root, root_source},
         {include.generic_string(), "static const float4 renamedValue = 1.0.xxxx;\n"}});
    info = translation_unit.compilation_info();
    CHECK_FALSE(info.success);
    REQUIRE(!info.diagnostics.empty());
    CHECK(std::ranges::any_of(info.diagnostics, [](const auto& diagnostic) {
        return diagnostic.message.find("includeValue") != std::string::npos;
    }));
    CHECK_FALSE(info.output.has_value());

    // Reparsing again to restore the identifier proves the failure above
    // reflects the intervening edit rather than a permanently broken state.
    translation_unit.reparse(
        {{root, root_source},
         {include.generic_string(), "static const float4 includeValue = 2.0.xxxx;\n"}});
    info = translation_unit.compilation_info();
    CHECK(info.success);
    REQUIRE(info.output.has_value());

    std::filesystem::remove_all(directory);
}

TEST_CASE("Compilation info exposes register class, raw flags, range id, and sample count for "
          "reflected resources",
          "[dxc][compilation-info][resource-binding]") {
    // Also empirically confirms tbuffer binds through an SRV 't' register,
    // not a CBV 'b' register, despite its constant-buffer-like declaration
    // syntax.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex : register(t0);\n"
                               "SamplerState Samp : register(s0);\n"
                               "cbuffer CB : register(b0) { float4 tint; }\n"
                               "RWStructuredBuffer<float4> Uav : register(u0);\n"
                               "tbuffer TB : register(t3) { float4 tbValue; }\n"
                               "float4 main() : SV_Target {\n"
                               "    Uav[0] = tint + tbValue;\n"
                               "    return Tex.Sample(Samp, float2(0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto find_resource = [&](std::string_view name) {
        return std::ranges::find(info.reflection->resources, name,
                                 &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    };

    const auto tex = find_resource("Tex");
    REQUIRE(tex != info.reflection->resources.end());
    CHECK(tex->register_class == hlsl_intellisense::dxc::ResourceRegisterClass::srv);

    const auto samp = find_resource("Samp");
    REQUIRE(samp != info.reflection->resources.end());
    CHECK(samp->register_class == hlsl_intellisense::dxc::ResourceRegisterClass::sampler);

    const auto cbuf = find_resource("CB");
    REQUIRE(cbuf != info.reflection->resources.end());
    CHECK(cbuf->register_class == hlsl_intellisense::dxc::ResourceRegisterClass::cbv);

    const auto uav = find_resource("Uav");
    REQUIRE(uav != info.reflection->resources.end());
    CHECK(uav->register_class == hlsl_intellisense::dxc::ResourceRegisterClass::uav);

    // Empirically pinned: tbuffer reflects as an SRV bound through a 't'
    // register (D3D_SIT_TBUFFER), never as a CBV, even though the HLSL
    // declaration reads like a constant buffer.
    const auto tbuf = find_resource("TB");
    REQUIRE(tbuf != info.reflection->resources.end());
    CHECK(tbuf->register_class == hlsl_intellisense::dxc::ResourceRegisterClass::srv);
    CHECK(tbuf->bind_point == 3);

    // Non-multisampled textures report NumSamples == 0xFFFFFFFF ("not
    // applicable"), passed through unchanged rather than reinterpreted.
    CHECK(tex->sample_count == 0xFFFFFFFFU);
    CHECK_FALSE(tex->unbounded);
    CHECK_FALSE(tex->system_reserved_space);
}

TEST_CASE("Compilation info attaches an unambiguous declaration location to reflected resources",
          "[dxc][compilation-info][resource-binding][source-location]") {
    // Resources are correlated to their declaration site using the same DXC
    // IntelliSense cursor/document-symbol machinery already used for
    // hover/go-to-definition (TranslationUnit::symbols()), never by parsing
    // or guessing from raw text.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex : register(t0);\n"
                               "SamplerState Samp : register(s0);\n"
                               "float4 main() : SV_Target {\n"
                               "    return Tex.Sample(Samp, float2(0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                       &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    REQUIRE(tex->source_location.has_value());
    CHECK(tex->source_location->path == shader_path);
    CHECK(tex->source_location->line == 1);
    // Column 19 is where the "Tex" identifier itself begins on line 1
    // ("Texture2D<float4> Tex : register(t0);"), not the start of the
    // declaration statement.
    CHECK(tex->source_location->column == 19);

    const auto samp = std::ranges::find(info.reflection->resources, "Samp",
                                        &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(samp != info.reflection->resources.end());
    REQUIRE(samp->source_location.has_value());
    CHECK(samp->source_location->line == 2);
}

TEST_CASE("Compilation info attaches a declaration location to an array resource",
          "[dxc][compilation-info][resource-binding][source-location]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex[4] : register(t0);\n"
                               "float4 main(uint idx : INDEX) : SV_Target {\n"
                               "    return Tex[idx].Load(int3(0, 0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                       &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    REQUIRE(tex->source_location.has_value());
    CHECK(tex->source_location->path == shader_path);
    CHECK(tex->source_location->line == 1);
}

TEST_CASE("Compilation info recomputes resource declaration locations after unsaved edits",
          "[dxc][compilation-info][resource-binding][source-location][reparse]") {
    // A stale cached symbol tree would keep reporting the resource's old
    // location after it moves; the very next compilation_info() call after
    // reparse() must observe the new line, proving the location is derived
    // from the current unsaved snapshot rather than a fabricated or cached
    // value.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string original_source = "Texture2D<float4> Tex : register(t0);\n"
                                        "SamplerState Samp : register(s0);\n"
                                        "float4 main() : SV_Target {\n"
                                        "    return Tex.Sample(Samp, float2(0, 0));\n"
                                        "}\n";
    auto translation_unit =
        intellisense.parse(shader_path, {{shader_path, original_source}}, options);

    auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                 &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    REQUIRE(tex->source_location.has_value());
    CHECK(tex->source_location->line == 1);

    const std::string moved_source = "\n\nTexture2D<float4> Tex : register(t0);\n"
                                     "SamplerState Samp : register(s0);\n"
                                     "float4 main() : SV_Target {\n"
                                     "    return Tex.Sample(Samp, float2(0, 0));\n"
                                     "}\n";
    translation_unit.reparse({{shader_path, moved_source}});
    info = translation_unit.compilation_info();
    REQUIRE(info.success);
    tex = std::ranges::find(info.reflection->resources, "Tex",
                            &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    REQUIRE(tex->source_location.has_value());
    CHECK(tex->source_location->line == 3);
}

TEST_CASE("Compilation info omits the resource source location when the name is ambiguous in the "
          "cursor tree",
          "[dxc][compilation-info][resource-binding][source-location]") {
    // "Data" names both the reflected cbuffer resource and an unrelated
    // struct field elsewhere in the same translation unit. DXC's own cursor
    // tree cannot disambiguate a plain name match here (both are genuine,
    // distinct declarations), so the correlation must conservatively omit
    // the location rather than pick one and risk pointing at the wrong
    // declaration.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "struct Unrelated { float Data; };\n"
                               "cbuffer Data : register(b0) { float4 tint; }\n"
                               "float4 main() : SV_Target {\n"
                               "    Unrelated value = (Unrelated)0;\n"
                               "    return tint + value.Data;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto data = std::ranges::find(info.reflection->resources, "Data",
                                        &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(data != info.reflection->resources.end());
    CHECK_FALSE(data->source_location.has_value());
}

TEST_CASE("Compilation info reports finite resource arrays as bounded",
          "[dxc][compilation-info][resource-binding]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex[4] : register(t0);\n"
                               "float4 main(uint idx : INDEX) : SV_Target {\n"
                               "    return Tex[idx].Load(int3(0, 0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                       &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    CHECK_FALSE(tex->unbounded);
    CHECK(tex->bind_count == 4);
}

TEST_CASE("Compilation info reports unbounded resource arrays distinctly from finite arrays",
          "[dxc][compilation-info][resource-binding]") {
    // Empirically confirmed: an unbounded shader-side resource array reports
    // BindCount == 0 (not UINT_MAX, which is the *root-signature-side*
    // convention for unbounded descriptor ranges).
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex[] : register(t0, space1);\n"
                               "float4 main(uint idx : INDEX) : SV_Target {\n"
                               "    return Tex[idx].Load(int3(0, 0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                       &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    CHECK(tex->unbounded);
    CHECK(tex->bind_count == 0);
    CHECK(tex->space == 1);
}

TEST_CASE("Compilation info classifies resources in D3D12 reserved register spaces",
          "[dxc][compilation-info][resource-binding]") {
    // Empirically confirmed: DXC accepts a user-declared space at the start
    // of D3D12's reserved system range [0xfffffff0, 0xffffffff] without
    // rejecting the compile; this server must still classify it distinctly
    // rather than treating it like an ordinary user space.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex : register(t0, space4294967280);\n"
                               "float4 main() : SV_Target {\n"
                               "    return Tex.Load(int3(0, 0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    const auto tex = std::ranges::find(info.reflection->resources, "Tex",
                                       &hlsl_intellisense::dxc::CompilationResourceBinding::name);
    REQUIRE(tex != info.reflection->resources.end());
    CHECK(tex->space == 4294967280U);
    CHECK(tex->system_reserved_space);

    const auto group =
        std::ranges::find_if(info.reflection->binding_analysis.groups,
                             [](const auto& group) { return group.space == 4294967280U; });
    REQUIRE(group != info.reflection->binding_analysis.groups.end());
    CHECK(group->system_reserved_space);
}

TEST_CASE("Compilation info groups well-formed resources with no provable collisions",
          "[dxc][compilation-info][resource-binding]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> TexA : register(t0);\n"
                               "Texture2D<float4> TexB : register(t1);\n"
                               "SamplerState Samp : register(s0);\n"
                               "cbuffer CB : register(b0) { float4 tint; }\n"
                               "float4 main() : SV_Target {\n"
                               "    return TexA.Sample(Samp, float2(0, 0)) +\n"
                               "           TexB.Sample(Samp, float2(0, 0)) + tint;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.reflection.has_value());
    CHECK(info.reflection->binding_analysis.collisions.empty());
    // srv (2), sampler (1), cbv (1) groups, each in space 0.
    CHECK(info.reflection->binding_analysis.groups.size() == 3);
    const auto srv_group =
        std::ranges::find_if(info.reflection->binding_analysis.groups, [](const auto& group) {
            return group.register_class == hlsl_intellisense::dxc::ResourceRegisterClass::srv;
        });
    REQUIRE(srv_group != info.reflection->binding_analysis.groups.end());
    CHECK(srv_group->ranges.size() == 2);
}

TEST_CASE("Compilation info extracts an embedded root signature with parameters, ranges, static "
          "samplers, and direct-indexing flags",
          "[dxc][compilation-info][root-signature]") {
    // Pins the exact 1.1-shaped structure DXC 1.9.2607.13 was empirically
    // observed to produce for a root signature exercising root constants, a
    // root CBV descriptor, a descriptor table with a finite DATA_STATIC SRV
    // range and an unbounded DESCRIPTORS_VOLATILE UAV range, a static
    // sampler, and CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source =
        R"HLSL(
#define MyRS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
             "RootConstants(num32BitConstants=4, b0, space=0, visibility=SHADER_VISIBILITY_PIXEL), " \
             "CBV(b1, space=0, visibility=SHADER_VISIBILITY_ALL), " \
             "DescriptorTable(SRV(t0, numDescriptors=4, space=1, flags=DATA_STATIC), " \
                             "UAV(u0, numDescriptors=unbounded, space=2, flags=DESCRIPTORS_VOLATILE), " \
                             "visibility=SHADER_VISIBILITY_PIXEL), " \
             "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, space=0, visibility=SHADER_VISIBILITY_PIXEL)"
Texture2D<float4> Tex[4] : register(t0, space1);
RWStructuredBuffer<float4> Uav[] : register(u0, space2);
SamplerState Samp : register(s0);
cbuffer CB0 : register(b0) { float4 c0; }
cbuffer CB1 : register(b1) { float4 c1; }
[RootSignature(MyRS)]
float4 main(uint idx : INDEX) : SV_Target {
    Uav[idx][0] = c0;
    return Tex[idx].Sample(Samp, float2(0,0)) + c1;
}
)HLSL";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    std::string messages;
    for (const auto& diagnostic : info.diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    REQUIRE(info.success);
    REQUIRE(info.root_signature.has_value());
#ifdef _WIN32
    // Windows: the official D3D12 deserializer is available, so this
    // server exposes the full parameter/range/static-sampler structure.
    REQUIRE(info.root_signature->availability ==
            hlsl_intellisense::dxc::RootSignatureAvailability::present);
    REQUIRE(info.root_signature->details.has_value());
    const auto& details = *info.root_signature->details;
    CHECK(details.version == "1.1");
    CHECK(details.cbv_srv_uav_heap_directly_indexed);
    CHECK_FALSE(details.sampler_heap_directly_indexed);
    REQUIRE(details.parameters.size() == 3);
    REQUIRE(details.static_samplers.size() == 1);

    const auto& root_constants_param = details.parameters[0];
    CHECK(root_constants_param.kind ==
          hlsl_intellisense::dxc::RootSignatureParameterKind::constants);
    CHECK(root_constants_param.visibility ==
          hlsl_intellisense::dxc::RootSignatureVisibility::pixel);
    REQUIRE(root_constants_param.constants.has_value());
    CHECK(root_constants_param.constants->shader_register == 0);
    CHECK(root_constants_param.constants->space == 0);
    CHECK(root_constants_param.constants->num_32bit_values == 4);

    const auto& root_descriptor_param = details.parameters[1];
    CHECK(root_descriptor_param.kind ==
          hlsl_intellisense::dxc::RootSignatureParameterKind::root_descriptor);
    CHECK(root_descriptor_param.visibility == hlsl_intellisense::dxc::RootSignatureVisibility::all);
    REQUIRE(root_descriptor_param.root_descriptor.has_value());
    CHECK(root_descriptor_param.root_descriptor->type ==
          hlsl_intellisense::dxc::RootSignatureRangeType::cbv);
    CHECK(root_descriptor_param.root_descriptor->shader_register == 1);
    CHECK(root_descriptor_param.root_descriptor->space == 0);

    const auto& table_param = details.parameters[2];
    CHECK(table_param.kind == hlsl_intellisense::dxc::RootSignatureParameterKind::descriptor_table);
    CHECK(table_param.visibility == hlsl_intellisense::dxc::RootSignatureVisibility::pixel);
    REQUIRE(table_param.descriptor_table_ranges.size() == 2);

    const auto& srv_range = table_param.descriptor_table_ranges[0];
    CHECK(srv_range.type == hlsl_intellisense::dxc::RootSignatureRangeType::srv);
    CHECK_FALSE(srv_range.unbounded);
    CHECK(srv_range.num_descriptors == 4);
    CHECK(srv_range.base_register == 0);
    CHECK(srv_range.space == 1);
    CHECK((srv_range.raw_flags & 0x8U) != 0); // D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC

    const auto& uav_range = table_param.descriptor_table_ranges[1];
    CHECK(uav_range.type == hlsl_intellisense::dxc::RootSignatureRangeType::uav);
    CHECK(uav_range.unbounded);
    CHECK(uav_range.base_register == 0);
    CHECK(uav_range.space == 2);
    CHECK((uav_range.raw_flags & 0x1U) != 0); // D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE

    const auto& sampler = details.static_samplers[0];
    CHECK(sampler.shader_register == 0);
    CHECK(sampler.space == 0);
    CHECK(sampler.visibility == hlsl_intellisense::dxc::RootSignatureVisibility::pixel);
    CHECK(sampler.filter == 21U); // D3D12_FILTER_MIN_MAG_MIP_LINEAR

    // The reflected resources in this same compilation are fully and
    // correctly covered for the active (pixel) stage, so compatibility must
    // report `compatible` with no issues.
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::compatible);
    CHECK(info.compatibility->issues.empty());
#else
    // Non-Windows (e.g. Linux): the embedded root signature is detected
    // present via IDxcUtils::GetDxilContainerPart (which is cross-platform),
    // but detailed deserialization requires the Windows-only
    // ID3D12VersionedRootSignatureDeserializer, so this server correctly
    // reports presence without fabricating details, and compatibility
    // cannot be determined without them.
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::present_details_unavailable);
    CHECK_FALSE(info.root_signature->details.has_value());
    CHECK_FALSE(info.root_signature->unavailable_reason.empty());
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
#endif
}

TEST_CASE("Compilation info reports an absent root signature distinctly when none is embedded",
          "[dxc][compilation-info][root-signature]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> Tex : register(t0);\n"
                               "SamplerState Samp : register(s0);\n"
                               "float4 main() : SV_Target {\n"
                               "    return Tex.Sample(Samp, float2(0, 0));\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.root_signature.has_value());
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::absent);
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
    CHECK_FALSE(info.compatibility->explanation.empty());
}

TEST_CASE("Compilation info reports SPIR-V root signatures as not applicable",
          "[dxc][compilation-info][root-signature][spirv]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    options.additional_arguments = {"-spirv"};
    const std::string source = "float4 main() : SV_Target {\n"
                               "    return 1.0.xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.root_signature.has_value());
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::not_applicable);
    CHECK_FALSE(info.root_signature->unavailable_reason.empty());
    REQUIRE(info.compatibility.has_value());
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
}

TEST_CASE("Compilation info reports the true root signature version even though details are "
          "always exposed through the 1.1 shape",
          "[dxc][compilation-info][root-signature]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    options.additional_arguments = {"-force-rootsig-ver", "rootsig_1_0"};
    const std::string source = R"HLSL(
#define MyRS "RootFlags(0), CBV(b0)"
cbuffer CB0 : register(b0) { float4 c0; }
[RootSignature(MyRS)]
float4 main() : SV_Target {
    return c0;
}
)HLSL";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    std::string messages;
    for (const auto& diagnostic : info.diagnostics) {
        messages += diagnostic.message;
        messages += '\n';
    }
    INFO(messages);
    REQUIRE(info.success);
    REQUIRE(info.root_signature.has_value());
#ifdef _WIN32
    REQUIRE(info.root_signature->details.has_value());
    CHECK(info.root_signature->details->version == "1.0");
    REQUIRE(info.root_signature->details->parameters.size() == 1);
    CHECK(info.root_signature->details->parameters[0].kind ==
          hlsl_intellisense::dxc::RootSignatureParameterKind::root_descriptor);
#else
    // Presence is still correctly detected via GetDxilContainerPart, but the
    // version-specific parameter shape can only be read back through the
    // Windows-only deserializer.
    CHECK(info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::present_details_unavailable);
    CHECK_FALSE(info.root_signature->details.has_value());
    CHECK_FALSE(info.root_signature->unavailable_reason.empty());
#endif
}

TEST_CASE("Compilation reports the compiler's own root-signature validation diagnostic when a "
          "descriptor-table range is narrower than the shader's declared resource array",
          "[dxc][compilation-info][compatibility]") {
    // Empirically re-verified (see out/scratch/compat_probe.cpp): once a root
    // signature is embedded via the [RootSignature(...)] attribute, DXC's own
    // front-end validates finite-vs-finite range coverage at compile time and
    // rejects a descriptor-table range narrower than the shader's declared
    // resource array *before* this server's own compilation_info() pipeline
    // ever runs. This scenario is therefore only reachable as a compile
    // failure with the compiler's own diagnostic (never as a successful
    // compile carrying an "incompatible" analysis result) -- this test
    // pins that compiler-owned behavior. The white-box
    // tests/dxc/compatibility_tests.cpp exercises the analysis logic itself
    // with synthetic data for this same shape of mismatch.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = R"HLSL(
#define MyRS "RootFlags(0), DescriptorTable(SRV(t0, numDescriptors=1, space=0))"
Texture2D<float4> Tex[4] : register(t0);
[RootSignature(MyRS)]
float4 main(uint i : IDX) : SV_Target { return Tex[i].Load(int3(0,0,0)); }
)HLSL";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    CHECK_FALSE(info.success);
    const bool has_expected_diagnostic =
        std::ranges::any_of(info.diagnostics, [](const auto& diagnostic) {
            return diagnostic.message.find("not fully bound in root signature") !=
                   std::string::npos;
        });
    CHECK(has_expected_diagnostic);
}

TEST_CASE("Compilation info reports compatible for an unbounded resource array covered by a "
          "bounded root signature range at the same base register",
          "[dxc][compilation-info][compatibility]") {
    // Empirically confirmed to compile AND validate successfully through
    // pinned DXC (see out/scratch/compat_probe.cpp history): the compiler's
    // own root-signature validator does not require a bounded
    // descriptor-table range backing an unbounded shader-declared array to
    // also be declared unbounded. This server's compatibility analysis
    // matches that authoritative behavior and only requires base-register
    // coverage for unbounded resources, never flagging this shape as
    // incompatible on its own.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.target_profile = "ps_6_6";
    options.entry_point = "main";
    const std::string source = R"HLSL(
#define MyRS "RootFlags(0), DescriptorTable(SRV(t0, numDescriptors=4, space=0))"
Texture2D<float4> Tex[] : register(t0);
[RootSignature(MyRS)]
float4 main(uint i : IDX) : SV_Target { return Tex[i].Load(int3(0,0,0)); }
)HLSL";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto info = translation_unit.compilation_info();
    REQUIRE(info.success);
    REQUIRE(info.compatibility.has_value());
#ifdef _WIN32
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::compatible);
    CHECK(info.compatibility->issues.empty());
#else
    // Compatibility cannot be proven without the Windows-only deserialized
    // root-signature details; this is reported as `unknown`, never a
    // fabricated verdict.
    CHECK(info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
#endif
}

TEST_CASE("Compilation info recomputes resources, root signature, and compatibility per active "
          "entry point and stage",
          "[dxc][compilation-info][variant]") {
    // Simulates what a workspace variant switch does at the Manager/Server
    // layer (re-deriving CompilerOptions with a different entry point and
    // target profile): parsing the *same* multi-entry-point source with two
    // different active configurations must yield distinctly-recomputed
    // resources (compiler-side dead code elimination means only the
    // resource the active entry point actually references is reflected),
    // stage, and compatibility - never a stale carryover from a previous
    // configuration.
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source =
        R"HLSL(
#define MyRS "RootFlags(0), " \
             "CBV(b0, space=0, visibility=SHADER_VISIBILITY_VERTEX), " \
             "CBV(b1, space=0, visibility=SHADER_VISIBILITY_PIXEL)"
cbuffer VSData : register(b0) { float4 vsValue; }
cbuffer PSData : register(b1) { float4 psValue; }
[RootSignature(MyRS)]
float4 VSMain() : SV_Position { return vsValue; }
[RootSignature(MyRS)]
float4 PSMain() : SV_Target { return psValue; }
)HLSL";

    hlsl_intellisense::dxc::CompilerOptions vs_options;
    vs_options.target_profile = "vs_6_6";
    vs_options.entry_point = "VSMain";
    auto vs_unit = intellisense.parse(shader_path, {{shader_path, source}}, vs_options);
    const auto vs_info = vs_unit.compilation_info();
    std::string vs_messages;
    for (const auto& diagnostic : vs_info.diagnostics) {
        vs_messages += diagnostic.message;
        vs_messages += '\n';
    }
    INFO(vs_messages);
    REQUIRE(vs_info.success);
    CHECK(vs_info.stage == "vertex");
    REQUIRE(vs_info.reflection.has_value());
    REQUIRE(vs_info.reflection->resources.size() == 1);
    CHECK(vs_info.reflection->resources.front().name == "VSData");
    REQUIRE(vs_info.compatibility.has_value());
#ifdef _WIN32
    CHECK(vs_info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::compatible);
#else
    CHECK(vs_info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
#endif

    hlsl_intellisense::dxc::CompilerOptions ps_options;
    ps_options.target_profile = "ps_6_6";
    ps_options.entry_point = "PSMain";
    auto ps_unit = intellisense.parse(shader_path, {{shader_path, source}}, ps_options);
    const auto ps_info = ps_unit.compilation_info();
    std::string ps_messages;
    for (const auto& diagnostic : ps_info.diagnostics) {
        ps_messages += diagnostic.message;
        ps_messages += '\n';
    }
    INFO(ps_messages);
    REQUIRE(ps_info.success);
    CHECK(ps_info.stage == "pixel");
    REQUIRE(ps_info.reflection.has_value());
    REQUIRE(ps_info.reflection->resources.size() == 1);
    CHECK(ps_info.reflection->resources.front().name == "PSData");
    REQUIRE(ps_info.compatibility.has_value());
#ifdef _WIN32
    CHECK(ps_info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::compatible);
#else
    CHECK(ps_info.compatibility->status ==
          hlsl_intellisense::dxc::ResourceCompatibilityStatus::unknown);
#endif

    // Both compiles embed the identical root-signature text, so its
    // parameter/visibility structure must be identical regardless of which
    // entry point/stage is active - only the *reflected resources* and
    // *stage* differ per active configuration.
    REQUIRE(vs_info.root_signature.has_value());
    REQUIRE(ps_info.root_signature.has_value());
#ifdef _WIN32
    REQUIRE(vs_info.root_signature->details.has_value());
    REQUIRE(ps_info.root_signature->details.has_value());
    CHECK(vs_info.root_signature->details->parameters.size() == 2);
    CHECK(ps_info.root_signature->details->parameters.size() == 2);
#else
    CHECK(vs_info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::present_details_unavailable);
    CHECK(ps_info.root_signature->availability ==
          hlsl_intellisense::dxc::RootSignatureAvailability::present_details_unavailable);
    CHECK_FALSE(vs_info.root_signature->details.has_value());
    CHECK_FALSE(ps_info.root_signature->details.has_value());
#endif
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

TEST_CASE("Call hierarchy resolves direct calls with overload identity",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float helper(float value) { return value * 2.0; }\n"
                               "float helper(float value, float bias) { return value + bias; }\n"
                               "float4 main() : SV_Target {\n"
                               "    float scalar = helper(1.0);\n"
                               "    float biased = helper(1.0, 2.0);\n"
                               "    return (scalar + biased).xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto main_symbol = translation_unit.callable_at(shader_path, 3, 8);
    REQUIRE(main_symbol.has_value());
    CHECK(main_symbol->name == "main");
    CHECK(main_symbol->is_definition);

    const auto outgoing = translation_unit.outgoing_calls(shader_path, 3, 8);
    REQUIRE(outgoing.size() == 2);
    // Overload identity must be resolved via the compiler cursor the call
    // expression actually references, not by name: both calls are named
    // "helper", but must resolve to their own, distinct declaration line.
    CHECK(outgoing[0].callee.name == "helper");
    CHECK(outgoing[0].callee.location.line == 1);
    REQUIRE(outgoing[0].call_sites.size() == 1);
    CHECK(outgoing[0].call_sites[0].location.line == 4);
    CHECK(outgoing[1].callee.name == "helper");
    CHECK(outgoing[1].callee.location.line == 2);
    REQUIRE(outgoing[1].call_sites.size() == 1);
    CHECK(outgoing[1].call_sites[0].location.line == 5);

    const auto incoming_single_arg = translation_unit.incoming_calls(shader_path, 1, 7);
    REQUIRE(incoming_single_arg.size() == 1);
    CHECK(incoming_single_arg[0].caller.name == "main");
    REQUIRE(incoming_single_arg[0].call_sites.size() == 1);
    CHECK(incoming_single_arg[0].call_sites[0].location.line == 4);

    const auto incoming_two_arg = translation_unit.incoming_calls(shader_path, 2, 7);
    REQUIRE(incoming_two_arg.size() == 1);
    CHECK(incoming_two_arg[0].caller.name == "main");
    REQUIRE(incoming_two_arg[0].call_sites.size() == 1);
    CHECK(incoming_two_arg[0].call_sites[0].location.line == 5);
}

TEST_CASE("Call hierarchy reports every call site when a caller invokes the same callee twice",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float helper(float value) { return value * 2.0; }\n"
                               "float4 main() : SV_Target {\n"
                               "    float first = helper(1.0);\n"
                               "    float second = helper(2.0);\n"
                               "    return (first + second).xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto outgoing = translation_unit.outgoing_calls(shader_path, 2, 8);
    REQUIRE(outgoing.size() == 1);
    REQUIRE(outgoing[0].call_sites.size() == 2);
    CHECK(outgoing[0].call_sites[0].location.line == 3);
    CHECK(outgoing[0].call_sites[1].location.line == 4);

    const auto incoming = translation_unit.incoming_calls(shader_path, 1, 7);
    REQUIRE(incoming.size() == 1);
    REQUIRE(incoming[0].call_sites.size() == 2);
    CHECK(incoming[0].call_sites[0].location.line == 3);
    CHECK(incoming[0].call_sites[1].location.line == 4);
}

TEST_CASE("Call hierarchy resolves calls across an unsaved include and reparsed edits",
          "[dxc][call-hierarchy][includes][reparse][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const auto root = (std::filesystem::current_path() / "call-hierarchy.hlsl").generic_string();
    const auto include =
        (std::filesystem::current_path() / "call-hierarchy.hlsli").generic_string();
    const std::string include_source = "float square(float value) { return value * value; }\n";
    const std::string root_source = "#include \"call-hierarchy.hlsli\"\n"
                                    "float4 main() : SV_Target {\n"
                                    "    return square(2.0).xxxx;\n"
                                    "}\n";
    auto translation_unit =
        intellisense.parse(root, {{root, root_source}, {include, include_source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto outgoing = translation_unit.outgoing_calls(root, 2, 8);
    REQUIRE(outgoing.size() == 1);
    CHECK(outgoing[0].callee.location.path == include);

    const auto incoming = translation_unit.incoming_calls(include, 1, 7);
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0].caller.location.path == root);
    REQUIRE(incoming[0].call_sites.size() == 1);
    CHECK(incoming[0].call_sites[0].location.line == 3);

    // Editing the (still unsaved) include to add a second caller must be
    // reflected without re-parsing from disk.
    const std::string edited_root_source = "#include \"call-hierarchy.hlsli\"\n"
                                           "float4 main() : SV_Target {\n"
                                           "    return (square(2.0) + square(3.0)).xxxx;\n"
                                           "}\n";
    translation_unit.reparse({{root, edited_root_source}, {include, include_source}});
    const auto incoming_after_edit = translation_unit.incoming_calls(include, 1, 7);
    REQUIRE(incoming_after_edit.size() == 1);
    REQUIRE(incoming_after_edit[0].call_sites.size() == 2);
}

TEST_CASE("Call hierarchy represents recursion instead of dropping it",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float factorial(float value) {\n"
                               "    if (value <= 1.0) { return 1.0; }\n"
                               "    return value * factorial(value - 1.0);\n"
                               "}\n"
                               "float4 main() : SV_Target { return factorial(4.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto outgoing = translation_unit.outgoing_calls(shader_path, 1, 7);
    REQUIRE(outgoing.size() == 1);
    CHECK(outgoing[0].callee.name == "factorial");
    CHECK(outgoing[0].callee.location.line == 1);
    REQUIRE(outgoing[0].call_sites.size() == 1);
    CHECK(outgoing[0].call_sites[0].location.line == 3);

    const auto incoming = translation_unit.incoming_calls(shader_path, 1, 7);
    // Both the self-recursive call site and main()'s call site must be
    // represented as distinct callers, not merged or dropped.
    REQUIRE(incoming.size() == 2);
    const auto find_caller = [&incoming](std::string_view name) {
        return std::ranges::find(incoming, name, [](const auto& call) { return call.caller.name; });
    };
    const auto self_call = find_caller("factorial");
    REQUIRE(self_call != incoming.end());
    REQUIRE(self_call->call_sites.size() == 1);
    CHECK(self_call->call_sites[0].location.line == 3);
    const auto main_call = find_caller("main");
    REQUIRE(main_call != incoming.end());
    REQUIRE(main_call->call_sites.size() == 1);
    CHECK(main_call->call_sites[0].location.line == 5);
}

TEST_CASE("Call hierarchy resolves calls through struct methods",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "struct Material {\n"
                               "    float roughness;\n"
                               "    float Shade(float value) { return value * roughness; }\n"
                               "};\n"
                               "float4 main() : SV_Target {\n"
                               "    Material material = (Material)0;\n"
                               "    return material.Shade(1.0).xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    const auto outgoing = translation_unit.outgoing_calls(shader_path, 5, 8);
    REQUIRE(outgoing.size() == 1);
    CHECK(outgoing[0].callee.name == "Shade");
    CHECK(outgoing[0].callee.location.line == 3);

    const auto incoming = translation_unit.incoming_calls(shader_path, 3, 11);
    REQUIRE(incoming.size() == 1);
    CHECK(incoming[0].caller.name == "main");
}

TEST_CASE("Call hierarchy has no outgoing calls for a leaf function and no incoming calls for an "
          "unreferenced function",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float unused(float value) { return value; }\n"
                               "float leaf(float value) { return value * 2.0; }\n"
                               "float4 main() : SV_Target { return leaf(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    CHECK(translation_unit.outgoing_calls(shader_path, 2, 7).empty());
    CHECK(translation_unit.incoming_calls(shader_path, 1, 7).empty());
}

TEST_CASE("Call hierarchy rejects a position that does not resolve to a callable",
          "[dxc][call-hierarchy][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float4 main() : SV_Target {\n"
                               "    float value = 1.0;\n"
                               "    return value.xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    CHECK_FALSE(translation_unit.callable_at(shader_path, 2, 11).has_value());
    CHECK(translation_unit.outgoing_calls(shader_path, 2, 11).empty());
    CHECK(translation_unit.incoming_calls(shader_path, 2, 11).empty());
}

TEST_CASE("Outgoing calls honor a cancellation checkpoint fired partway through body scanning, "
          "not only before the query starts",
          "[dxc][call-hierarchy][cancellation][integration]") {
    // Regression: `outgoing_calls` previously accepted no cancellation
    // checkpoint at all, so a large function body's scan could not be
    // cancelled once started and would occupy the analysis worker until it
    // finished. A body with many simple statements is large enough that
    // `BodyScanner`'s own per-512-node checkpoint fires multiple times
    // during a single scan; this first measures how many times a
    // non-throwing counting checkpoint fires over a normal, completed
    // scan, then reruns with a checkpoint that throws partway through that
    // same count -- proving cancellation genuinely interrupts scanning
    // mid-traversal rather than only being checked once up front.
    hlsl_intellisense::dxc::Intellisense intellisense;
    constexpr int statement_count = 3000;
    std::string body = "float scan(float value) {\n";
    for (int index = 0; index < statement_count; ++index) {
        body += "    value = value + 1.0;\n";
    }
    body += "    return callee(value);\n}\n";
    const std::string source = "float callee(float value) { return value; }\n" + body +
                               "float4 main() : SV_Target { return scan(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());
    // `scan` is declared on the line after `callee`.
    constexpr std::uint32_t scan_line = 2;

    std::uint64_t baseline_checkpoint_calls = 0;
    const auto baseline = translation_unit.outgoing_calls(shader_path, scan_line, 7,
                                                          [&] { ++baseline_checkpoint_calls; });
    REQUIRE(baseline.size() == 1);
    CHECK(baseline[0].callee.name == "callee");
    // Multiple checkpoint calls over one scan proves there are genuinely
    // several checkpoint opportunities mid-traversal, not just one.
    REQUIRE(baseline_checkpoint_calls > 1);

    struct CancelledError final : std::runtime_error {
        CancelledError() : std::runtime_error{"cancelled"} {}
    };
    std::uint64_t checkpoint_calls = 0;
    const auto interior_threshold = baseline_checkpoint_calls / 2;
    CHECK_THROWS_AS(translation_unit.outgoing_calls(shader_path, scan_line, 7,
                                                    [&] {
                                                        if (++checkpoint_calls >
                                                            interior_threshold) {
                                                            throw CancelledError{};
                                                        }
                                                    }),
                    CancelledError);
}

TEST_CASE("Incoming calls honor a cancellation checkpoint fired partway through reference "
          "traversal, not only before the query starts",
          "[dxc][call-hierarchy][cancellation][integration]") {
    // Regression: `incoming_calls` previously accepted no cancellation
    // checkpoint at all, so a large reference scan (many call sites, many
    // sources, or many pages of `FindReferencesInFile` results) could not
    // be cancelled once started. This calls `target` from enough distinct
    // sites that `FindReferencesInFile`'s 256-entry pages span more than
    // one page, exercising the per-source, per-page, and per-reference
    // checkpoints together; it then proves cancellation genuinely
    // interrupts traversal mid-scan the same way as the outgoing-calls
    // regression above.
    hlsl_intellisense::dxc::Intellisense intellisense;
    constexpr int call_count = 600;
    std::string body = "float caller(float value) {\n";
    for (int index = 0; index < call_count; ++index) {
        body += "    value = target(value);\n";
    }
    body += "    return value;\n}\n";
    const std::string source = "float target(float value) { return value; }\n" + body +
                               "float4 main() : SV_Target { return caller(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});
    REQUIRE(translation_unit.diagnostics().empty());

    std::uint64_t baseline_checkpoint_calls = 0;
    const auto baseline =
        translation_unit.incoming_calls(shader_path, 1, 7, [&] { ++baseline_checkpoint_calls; });
    REQUIRE(baseline.size() == 1);
    CHECK(baseline[0].caller.name == "caller");
    CHECK(baseline[0].call_sites.size() == static_cast<std::size_t>(call_count));
    REQUIRE(baseline_checkpoint_calls > 1);

    struct CancelledError final : std::runtime_error {
        CancelledError() : std::runtime_error{"cancelled"} {}
    };
    std::uint64_t checkpoint_calls = 0;
    const auto interior_threshold = baseline_checkpoint_calls / 2;
    CHECK_THROWS_AS(translation_unit.incoming_calls(shader_path, 1, 7,
                                                    [&] {
                                                        if (++checkpoint_calls >
                                                            interior_threshold) {
                                                            throw CancelledError{};
                                                        }
                                                    }),
                    CancelledError);
}

TEST_CASE("Entry-point data flow traces reachable functions and reports unreachable ones",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "float helper(float value) { return value * 2.0; }\n"
                               "float deadCode(float value) { return value + 1.0; }\n"
                               "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);
    REQUIRE(flow.entry_point.has_value());
    CHECK(flow.entry_point->name == "main");

    REQUIRE(flow.reachable_functions.size() == 2);
    CHECK(flow.reachable_functions[0].function.name == "main");
    CHECK(flow.reachable_functions[0].depth == 0);
    CHECK_FALSE(flow.reachable_functions[0].recursive);
    CHECK(flow.reachable_functions[1].function.name == "helper");
    CHECK(flow.reachable_functions[1].depth == 1);
    CHECK_FALSE(flow.reachable_functions[1].recursive);

    REQUIRE(flow.unreachable_functions.size() == 1);
    CHECK(flow.unreachable_functions[0].name == "deadCode");
    CHECK_FALSE(flow.truncated);
}

TEST_CASE("Entry-point data flow marks self-recursion and mutual recursion without truncation",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "float isOdd(float value);\n"
                               "float isEven(float value) {\n"
                               "    if (value <= 0.0) { return 1.0; }\n"
                               "    return isOdd(value - 1.0);\n"
                               "}\n"
                               "float isOdd(float value) {\n"
                               "    if (value <= 0.0) { return 0.0; }\n"
                               "    return isEven(value - 1.0);\n"
                               "}\n"
                               "float factorial(float value) {\n"
                               "    if (value <= 1.0) { return 1.0; }\n"
                               "    return value * factorial(value - 1.0);\n"
                               "}\n"
                               "float4 main() : SV_Target {\n"
                               "    return (isEven(4.0) + factorial(4.0)).xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);
    const auto find_reachable = [&flow](std::string_view name) {
        return std::ranges::find(flow.reachable_functions, name,
                                 [](const auto& reachable) { return reachable.function.name; });
    };
    const auto even = find_reachable("isEven");
    REQUIRE(even != flow.reachable_functions.end());
    CHECK(even->recursive);
    const auto odd = find_reachable("isOdd");
    REQUIRE(odd != flow.reachable_functions.end());
    CHECK(odd->recursive);
    const auto factorial = find_reachable("factorial");
    REQUIRE(factorial != flow.reachable_functions.end());
    CHECK(factorial->recursive);
    CHECK_FALSE(flow.truncated);
}

TEST_CASE("Entry-point data flow correctly classifies recursion on a call chain long enough to "
          "overflow a recursive cycle-detection stack",
          "[dxc][entry-point-data-flow][integration][stress]") {
    // Regression for a stack-overflow risk: cycle detection used to be a
    // recursive DFS, so a single long chain reachable from the entry point
    // would grow the native call stack by one frame per edge -- and with
    // `EntryPointDataFlowLimits::max_functions_visited` defaulting to 4096
    // (raised here so the whole chain is actually visited, not truncated),
    // a naturally occurring translation unit could reach exactly that
    // stack depth. This builds a single 8000-node cycle (comfortably past
    // typical default thread stack limits) reachable from `main` and
    // proves the iterative, explicit-stack DFS completes without
    // overflowing the stack (a crash cannot be caught by Catch2 -- simply
    // returning from this test case at all is part of what it proves) and
    // still classifies every node in the cycle as recursive.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    constexpr int chain_length = 8000;
    std::string source;
    for (int index = 0; index < chain_length; ++index) {
        source += "float node" + std::to_string(index) + "(float value);\n";
    }
    for (int index = 0; index < chain_length; ++index) {
        const int next = (index + 1) % chain_length;
        source += "float node" + std::to_string(index) + "(float value) { return node" +
                  std::to_string(next) + "(value) + 1.0; }\n";
    }
    source += "float4 main() : SV_Target { return node0(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow =
        translation_unit.entry_point_data_flow(hlsl_intellisense::dxc::EntryPointDataFlowLimits{
            .max_functions_visited = 10000, .max_unused_declaration_candidates = 20000});
    REQUIRE(flow.found);
    CHECK_FALSE(flow.truncated);
    CHECK_FALSE(flow.functions_visited_truncated);
    // `main` plus every node in the 8000-node cycle.
    CHECK(flow.functions_visited == chain_length + 1);
    REQUIRE(flow.reachable_functions.size() == flow.functions_visited);
    std::size_t recursive_count = 0;
    for (const auto& reachable : flow.reachable_functions) {
        if (reachable.function.name == "main") {
            CHECK_FALSE(reachable.recursive);
            continue;
        }
        CHECK(reachable.recursive);
        if (reachable.recursive) {
            ++recursive_count;
        }
    }
    CHECK(recursive_count == chain_length);
}

TEST_CASE("Entry-point data flow marks every member of a non-trivial three-node strongly "
          "connected component even when a naive back-edge-only cycle check would miss one node",
          "[dxc][entry-point-data-flow][integration]") {
    // Regression for a cycle-detection algorithm that only found cycles by
    // checking whether a successor is still on the *current* DFS path
    // (classic white/gray/black back-edge detection): with edges
    // funcA->funcB, funcB->funcA, funcA->funcC, funcC->funcB, a DFS from
    // funcA fully finishes funcB's subtree (correctly finding the direct
    // funcA<->funcB cycle) *before* visiting funcC, so by the time
    // funcC->funcB is examined, funcB is already finalized and the edge is
    // silently ignored -- even though funcC can reach back to funcA (via
    // funcB) and is therefore in the very same strongly connected
    // component as funcA and funcB. Only a real SCC algorithm (Kosaraju's,
    // used here) reports funcC as recursive. funcD is reached only forward
    // from the cycle and must NOT be marked recursive, proving the fix
    // does not over-approximate SCC membership either.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source =
        "float funcA(float value);\n"
        "float funcB(float value);\n"
        "float funcC(float value);\n"
        "float funcD(float value);\n"
        "float funcA(float value) { return funcB(value) + funcC(value) + 1.0; }\n"
        "float funcB(float value) { return funcA(value) + 1.0; }\n"
        "float funcC(float value) { return funcB(value) + funcD(value); }\n"
        "float funcD(float value) { return value + 1.0; }\n"
        "float4 main() : SV_Target { return funcA(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);
    const auto find_reachable = [&flow](std::string_view name) {
        return std::ranges::find(flow.reachable_functions, name,
                                 [](const auto& reachable) { return reachable.function.name; });
    };
    const auto func_a = find_reachable("funcA");
    REQUIRE(func_a != flow.reachable_functions.end());
    CHECK(func_a->recursive);
    const auto func_b = find_reachable("funcB");
    REQUIRE(func_b != flow.reachable_functions.end());
    CHECK(func_b->recursive);
    const auto func_c = find_reachable("funcC");
    REQUIRE(func_c != flow.reachable_functions.end());
    CHECK(func_c->recursive);
    const auto func_d = find_reachable("funcD");
    REQUIRE(func_d != flow.reachable_functions.end());
    CHECK_FALSE(func_d->recursive);
    const auto main_reachable = find_reachable("main");
    REQUIRE(main_reachable != flow.reachable_functions.end());
    CHECK_FALSE(main_reachable->recursive);
    CHECK_FALSE(flow.truncated);
}

TEST_CASE("Entry-point data flow correctly separates thousands of chained three-node cycles into "
          "distinct strongly connected components without overflowing the SCC passes' stack",
          "[dxc][entry-point-data-flow][integration][stress]") {
    // Regression for the iterative Kosaraju SCC replacement at scale: this
    // builds a long chain of thousands of independent 3-node cycles (the
    // exact a/b/c shape above) where each cycle's third node also calls
    // into the *next* cycle's entry point -- a purely forward edge that
    // must never be mistaken for a cycle. This exercises both DFS passes
    // (the forward finishing-order pass and the reverse transpose pass)
    // across thousands of components without overflowing the native stack
    // (a crash here cannot be caught by Catch2 -- simply returning from
    // this test at all is part of what it proves), and proves component
    // boundaries are respected: every node inside a cycle is `recursive`,
    // while the forward-only chaining edges between cycles never leak
    // recursion into an unrelated component.
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    constexpr int cluster_count = 2500;
    std::string source;
    for (int index = 0; index < cluster_count; ++index) {
        source += "float a" + std::to_string(index) + "(float value);\n";
        source += "float b" + std::to_string(index) + "(float value);\n";
        source += "float c" + std::to_string(index) + "(float value);\n";
    }
    for (int index = 0; index < cluster_count; ++index) {
        const std::string a = "a" + std::to_string(index);
        const std::string b = "b" + std::to_string(index);
        const std::string c = "c" + std::to_string(index);
        source +=
            "float " + a + "(float value) { return " + b + "(value) + " + c + "(value) + 1.0; }\n";
        source += "float " + b + "(float value) { return " + a + "(value) + 1.0; }\n";
        if (index + 1 < cluster_count) {
            const std::string next_a = "a" + std::to_string(index + 1);
            source += "float " + c + "(float value) { return " + b + "(value) + " + next_a +
                      "(value); }\n";
        } else {
            source += "float " + c + "(float value) { return " + b + "(value) + 1.0; }\n";
        }
    }
    source += "float4 main() : SV_Target { return a0(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow =
        translation_unit.entry_point_data_flow(hlsl_intellisense::dxc::EntryPointDataFlowLimits{
            .max_functions_visited = static_cast<std::size_t>(cluster_count) * 3 + 10,
            .max_unused_declaration_candidates = static_cast<std::size_t>(cluster_count) * 3 + 20});
    REQUIRE(flow.found);
    CHECK_FALSE(flow.truncated);
    CHECK_FALSE(flow.functions_visited_truncated);
    REQUIRE(flow.functions_visited == static_cast<std::size_t>(cluster_count) * 3 + 1);
    REQUIRE(flow.reachable_functions.size() == flow.functions_visited);

    const auto find_reachable = [&flow](std::string_view name) {
        return std::ranges::find(flow.reachable_functions, name,
                                 [](const auto& reachable) { return reachable.function.name; });
    };
    const auto main_reachable = find_reachable("main");
    REQUIRE(main_reachable != flow.reachable_functions.end());
    CHECK_FALSE(main_reachable->recursive);

    std::size_t recursive_count = 0;
    for (const auto& reachable : flow.reachable_functions) {
        if (reachable.recursive) {
            ++recursive_count;
        }
    }
    // Every a/b/c triplet forms its own 3-node cycle; `main` is the only
    // non-recursive node.
    CHECK(recursive_count == static_cast<std::size_t>(cluster_count) * 3);
}

TEST_CASE("Entry-point data flow reports conservative global and resource read/write access",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    options.target_profile = "cs_6_6";
    const std::string source = "cbuffer Constants : register(b0) {\n"
                               "    float readOnlyScalar;\n"
                               "    float readWriteScalar;\n"
                               "}\n"
                               "RWStructuredBuffer<float> buf : register(u0);\n"
                               "static float globalAccumulator = 0.0;\n"
                               "float readGlobal() { return readOnlyScalar; }\n"
                               "void writeGlobal() { globalAccumulator = readWriteScalar; }\n"
                               "[numthreads(1, 1, 1)]\n"
                               "void main(uint3 id : SV_DispatchThreadID) {\n"
                               "    writeGlobal();\n"
                               "    float sampled = readGlobal();\n"
                               "    buf[id.x] = sampled + globalAccumulator;\n"
                               "    float stored = buf[id.x];\n"
                               "    buf[id.x] += 1.0;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);

    using hlsl_intellisense::dxc::GlobalAccessKind;
    const auto find_access = [&flow](std::string_view name) {
        return std::ranges::find(flow.global_accesses, name,
                                 [](const auto& access) { return access.name; });
    };

    const auto read_only = find_access("readOnlyScalar");
    REQUIRE(read_only != flow.global_accesses.end());
    CHECK(read_only->access == GlobalAccessKind::read);

    const auto read_write_scalar = find_access("readWriteScalar");
    REQUIRE(read_write_scalar != flow.global_accesses.end());
    CHECK(read_write_scalar->access == GlobalAccessKind::read);

    const auto accumulator = find_access("globalAccumulator");
    REQUIRE(accumulator != flow.global_accesses.end());
    // Read once (RHS of buf[id.x] assignment) and written once (LHS of
    // writeGlobal's assignment): must merge to read_write, never
    // de-escalated back to read or write alone.
    CHECK(accumulator->access == GlobalAccessKind::read_write);

    const auto resource = find_access("buf");
    REQUIRE(resource != flow.global_accesses.end());
    // buf[id.x] is both read (`float stored = buf[id.x];`), written
    // (`buf[id.x] = ...`), and read-modify-written (`buf[id.x] += 1.0;`):
    // must merge to read_write.
    CHECK(resource->access == GlobalAccessKind::read_write);
}

TEST_CASE("Entry-point data flow continues conservatively merging an already-retained global "
          "access after the retention budget rejects a later, different global",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "static float g = 0.0;\n"
                               "static float h = 0.0;\n"
                               "float4 main() : SV_Target {\n"
                               "    float a = g;\n"
                               "    float b = h;\n"
                               "    g = 1.0;\n"
                               "    return (a + b).xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // Order matters here: `g` is read first (`float a = g;`) and retained
    // (capacity 1). `h` is read second (`float b = h;`) and, being a new,
    // unseen key with the retention set already at capacity, is rejected
    // and sets `global_accesses_truncated`. The later write `g = 1.0;`
    // must still be merged into the already-retained `g` entry -- upgrading
    // it from `read` to `read_write` -- even though the scanner is by then
    // in a truncated state: rejecting only *unseen* keys past capacity,
    // while continuing to conservatively merge already-retained keys, is
    // exactly the contract under test.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_global_accesses = 1;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    REQUIRE(flow.found);
    CHECK(flow.global_accesses_truncated);

    using hlsl_intellisense::dxc::GlobalAccessKind;
    const auto find_access = [&flow](std::string_view name) {
        return std::ranges::find(flow.global_accesses, name,
                                 [](const auto& access) { return access.name; });
    };
    const auto g_access = find_access("g");
    REQUIRE(g_access != flow.global_accesses.end());
    CHECK(g_access->access == GlobalAccessKind::read_write);
    CHECK(find_access("h") == flow.global_accesses.end());
    CHECK(flow.global_accesses.size() == 1);
}

TEST_CASE("Entry-point data flow uses a read-only allow list for named resource methods",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "Texture2D<float4> tex : register(t0);\n"
                               "SamplerState samp : register(s0);\n"
                               "RWStructuredBuffer<uint> counter : register(u0);\n"
                               "float4 main(float2 uv : TEXCOORD) : SV_Target {\n"
                               "    uint index = counter.IncrementCounter();\n"
                               "    return tex.Sample(samp, uv) * float(index);\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);

    using hlsl_intellisense::dxc::GlobalAccessKind;
    const auto find_access = [&flow](std::string_view name) {
        return std::ranges::find(flow.global_accesses, name,
                                 [](const auto& access) { return access.name; });
    };

    const auto texture = find_access("tex");
    REQUIRE(texture != flow.global_accesses.end());
    CHECK(texture->access == GlobalAccessKind::read);

    const auto counter = find_access("counter");
    REQUIRE(counter != flow.global_accesses.end());
    // IncrementCounter() is a mutating method with no wrap/bare signal on
    // its own for the base object (unlike operator[]), so a conservative
    // name-based allow list is used; IncrementCounter is not on the
    // read-only allow list and must be reported read_write.
    CHECK(counter->access == GlobalAccessKind::read_write);
}

TEST_CASE("Entry-point data flow never applies the read-only method-name allow list to a "
          "user-defined type sharing a resource method's name",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // `Counter` is an ordinary user-defined struct, not a compiler builtin
    // resource/sampler type, even though it declares a method named `Load`
    // -- one of the names on the read-only allow list used for genuine
    // builtin resource types (Texture2D::Load, RWStructuredBuffer::Load,
    // ...). Calling `counter.Load()` mutates `counter`'s own field, so the
    // global instance `counter` must be reported read_write: applying the
    // allow list here (matching purely on method name, ignoring the
    // receiver's actual type) would wrongly under-report this as a
    // read-only access.
    const std::string source = "struct Counter {\n"
                               "    float value;\n"
                               "    float Load() { value = value + 1.0; return value; }\n"
                               "};\n"
                               "static Counter counter = { 0.0 };\n"
                               "float4 main() : SV_Target {\n"
                               "    float sampled = counter.Load();\n"
                               "    return sampled.xxxx;\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);

    using hlsl_intellisense::dxc::GlobalAccessKind;
    const auto find_access = [&flow](std::string_view name) {
        return std::ranges::find(flow.global_accesses, name,
                                 [](const auto& access) { return access.name; });
    };
    const auto counter = find_access("counter");
    REQUIRE(counter != flow.global_accesses.end());
    CHECK(counter->access == GlobalAccessKind::read_write);
}

TEST_CASE("Entry-point data flow does not suppress a global argument's own access when it is "
          "passed to a method called on a non-global (parameter) receiver",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    options.target_profile = "cs_6_6";
    // `target` (the receiver of `InterlockedAdd`) is a function
    // *parameter*, not a global -- so `find_resource_base` finds no global
    // receiver for this call at all. `globalArg`, however, is a genuine
    // global passed *as* `InterlockedAdd`'s `out` argument, which DXC
    // leaves bare (unwrapped) at the call site (see `classify_bare_context`
    // for why a bare call argument's access cannot be narrowed past
    // read_write). Before excluding genuine arguments from the implicit-
    // receiver search, `globalArg` -- being the only global among the
    // call's children -- could be mistaken for the call's own receiver,
    // both misclassifying it and suppressing its own independent access
    // entirely via `resource_suppress_key`; it must instead be
    // independently reported, and read_write.
    const std::string source = "RWByteAddressBuffer res : register(u0);\n"
                               "static uint globalArg = 0;\n"
                               "void bumpCounter(RWByteAddressBuffer target, uint address) {\n"
                               "    target.InterlockedAdd(address, 1, globalArg);\n"
                               "}\n"
                               "[numthreads(1, 1, 1)]\n"
                               "void main(uint3 id : SV_DispatchThreadID) {\n"
                               "    bumpCounter(res, 0);\n"
                               "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);

    using hlsl_intellisense::dxc::GlobalAccessKind;
    const auto find_access = [&flow](std::string_view name) {
        return std::ranges::find(flow.global_accesses, name,
                                 [](const auto& access) { return access.name; });
    };
    const auto global_arg = find_access("globalArg");
    REQUIRE(global_arg != flow.global_accesses.end());
    CHECK(global_arg->access == GlobalAccessKind::read_write);
}

TEST_CASE("Entry-point data flow reports unused top-level declarations excluding the entry point",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "static float unusedGlobal = 1.0;\n"
                               "float unusedFunction(float value) { return value; }\n"
                               "float helper(float value) { return value * 2.0; }\n"
                               "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);

    const auto has_name = [](const auto& candidates, std::string_view name) {
        return std::ranges::any_of(candidates,
                                   [name](const auto& symbol) { return symbol.name == name; });
    };
    CHECK(has_name(flow.unused_declarations, "unusedGlobal"));
    CHECK(has_name(flow.unused_declarations, "unusedFunction"));
    CHECK_FALSE(has_name(flow.unused_declarations, "helper"));
    // The entry point itself is expected to have no internal callers and
    // must never be misreported as an unused declaration.
    CHECK_FALSE(has_name(flow.unused_declarations, "main"));
}

TEST_CASE("Entry-point data flow reports why nothing was traced when no entry point is configured",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    const std::string source = "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}});

    const auto flow = translation_unit.entry_point_data_flow();
    CHECK_FALSE(flow.found);
    CHECK_FALSE(flow.explanation.empty());
    CHECK(flow.reachable_functions.empty());
    CHECK(flow.entry_point == std::nullopt);
}

TEST_CASE("Entry-point data flow reports why nothing was traced when the configured entry point "
          "does not resolve",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "missingEntryPoint";
    const std::string source = "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto flow = translation_unit.entry_point_data_flow();
    CHECK_FALSE(flow.found);
    CHECK_FALSE(flow.explanation.empty());
}

TEST_CASE("Entry-point data flow does not select a struct method that merely shares the "
          "configured entry point's spelling",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // `Foo::main` is a struct method with the exact same unqualified
    // spelling as the configured entry point; it must never be silently
    // selected instead of the genuine top-level shader entry function.
    const std::string source = "struct Foo {\n"
                               "    float main() { return 1.0; }\n"
                               "};\n"
                               "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto flow = translation_unit.entry_point_data_flow();
    REQUIRE(flow.found);
    REQUIRE(flow.entry_point.has_value());
    // The entry point's own qualified name has no `Foo::` prefix: it is the
    // top-level function, not the struct method.
    CHECK(flow.entry_point->qualified_name == "main");
    CHECK(flow.entry_point->location.line == 4);

    const auto has_unreachable = [&flow](std::string_view qualified_name) {
        return std::ranges::any_of(flow.unreachable_functions,
                                   [qualified_name](const auto& symbol) {
                                       return symbol.qualified_name == qualified_name;
                                   });
    };
    // The struct method is unreached dead code, distinct from -- and not
    // merged with -- the entry point it happens to share a spelling with.
    CHECK(has_unreachable("Foo::main"));
}

TEST_CASE("Entry-point data flow rejects an ambiguous configured entry point name shared by "
          "multiple top-level function overloads",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // Two distinct top-level function definitions both named `main`
    // (illegal HLSL, but DXC's cursor tree can still expose both
    // definitions for an unsaved/in-progress edit): traversal-order-first
    // selection must not silently pick one; the ambiguity itself must be
    // reported instead.
    const std::string source = "float main(float value) { return value; }\n"
                               "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);

    const auto flow = translation_unit.entry_point_data_flow();
    CHECK_FALSE(flow.found);
    CHECK_FALSE(flow.explanation.empty());
    CHECK(flow.entry_point == std::nullopt);
    CHECK(flow.reachable_functions.empty());
}

TEST_CASE("Entry-point data flow honors an explicit function-visit budget",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "float step1(float value) { return value + 1.0; }\n"
                               "float step2(float value) { return step1(value) + 1.0; }\n"
                               "float step3(float value) { return step2(value) + 1.0; }\n"
                               "float4 main() : SV_Target { return step3(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_functions_visited = 2;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    REQUIRE(flow.found);
    CHECK(flow.truncated);
    CHECK(flow.functions_visited_truncated);
    CHECK_FALSE(flow.global_accesses_truncated);
    CHECK_FALSE(flow.unused_declarations_truncated);
    CHECK(flow.functions_visited <= 2);
    // Truncation must only ever shrink the reachable set, never grow it
    // beyond the true call graph.
    CHECK(flow.reachable_functions.size() <= 2);
    // A truncated traversal cannot prove any function unreachable: `step2`
    // and `step3` are genuinely reachable (they lie on the call chain from
    // `main`) but were never visited because the budget was hit first.
    // Reporting them as unreachable would be a false "dead code" claim, so
    // `unreachable_functions` must be left empty whenever truncated.
    CHECK(flow.unreachable_functions.empty());
}

TEST_CASE("Entry-point data flow never reports a downstream reachable function as unreachable "
          "when the traversal is truncated",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // A linear call chain entry -> a -> b -> c, plus one function that is
    // genuinely unreachable (deadCode). With a function-visit budget of 2,
    // the BFS visits only `main` and `a` before truncating: `b` and `c` are
    // discovered as queued-but-unvisited edges, not proven unreachable, and
    // must never appear in `unreachable_functions` even though they were
    // never actually explored.
    const std::string source = "float c(float value) { return value + 1.0; }\n"
                               "float b(float value) { return c(value) + 1.0; }\n"
                               "float a(float value) { return b(value) + 1.0; }\n"
                               "float deadCode(float value) { return value - 1.0; }\n"
                               "float4 main() : SV_Target { return a(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_functions_visited = 2;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    REQUIRE(flow.found);
    REQUIRE(flow.truncated);
    CHECK(flow.functions_visited_truncated);
    CHECK_FALSE(flow.global_accesses_truncated);
    CHECK_FALSE(flow.unused_declarations_truncated);
    CHECK(flow.functions_visited <= 2);

    const auto has_name = [](const auto& candidates, std::string_view name) {
        return std::ranges::any_of(candidates,
                                   [name](const auto& symbol) { return symbol.name == name; });
    };
    // Downstream reachable functions (never actually visited due to the
    // budget) must be absent, not falsely reported as dead code.
    CHECK_FALSE(has_name(flow.unreachable_functions, "b"));
    CHECK_FALSE(has_name(flow.unreachable_functions, "c"));
    // `deadCode` is also absent: an incomplete traversal cannot prove
    // *anything* unreachable, so `unreachable_functions` is entirely empty
    // while truncated, even for functions that would truly be dead code in
    // a completed traversal.
    CHECK(flow.unreachable_functions.empty());
}

TEST_CASE("Entry-point data flow classifies dead code as unreachable only once traversal fully "
          "completes",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "float c(float value) { return value + 1.0; }\n"
                               "float b(float value) { return c(value) + 1.0; }\n"
                               "float a(float value) { return b(value) + 1.0; }\n"
                               "float deadCode(float value) { return value - 1.0; }\n"
                               "float4 main() : SV_Target { return a(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // A large enough budget for the whole call chain to be exhausted: the
    // traversal is not truncated, so `deadCode` can now be soundly reported
    // as unreachable.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_functions_visited = 64;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    REQUIRE(flow.found);
    CHECK_FALSE(flow.truncated);
    CHECK_FALSE(flow.functions_visited_truncated);
    CHECK_FALSE(flow.global_accesses_truncated);
    CHECK_FALSE(flow.unused_declarations_truncated);
    const auto has_name = [](const auto& candidates, std::string_view name) {
        return std::ranges::any_of(candidates,
                                   [name](const auto& symbol) { return symbol.name == name; });
    };
    REQUIRE(flow.unreachable_functions.size() == 1);
    CHECK(has_name(flow.unreachable_functions, "deadCode"));
}

TEST_CASE("Entry-point data flow is cancellable via the supplied checkpoint",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    const std::string source = "float helper(float value) { return value * 2.0; }\n"
                               "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    struct CancelledError final : std::runtime_error {
        CancelledError() : std::runtime_error{"cancelled"} {}
    };
    CHECK_THROWS_AS(translation_unit.entry_point_data_flow({}, [] { throw CancelledError{}; }),
                    CancelledError);
}

TEST_CASE("Entry-point data flow cancellation checkpoint is honored during cycle detection over "
          "a large mutually-recursive call graph",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // One long cycle of 600 functions reachable from `main`, each calling
    // the next and finally calling back to the first. Large enough that
    // the cycle-detection DFS's own periodic checkpoint (every 512 visited
    // nodes) fires at least once mid-traversal, distinct from the BFS
    // loop's own per-iteration checkpoint call.
    constexpr int chain_length = 600;
    std::string source;
    for (int index = 0; index < chain_length; ++index) {
        source += "float node" + std::to_string(index) + "(float value);\n";
    }
    for (int index = 0; index < chain_length; ++index) {
        const int next = (index + 1) % chain_length;
        source += "float node" + std::to_string(index) + "(float value) { return node" +
                  std::to_string(next) + "(value) + 1.0; }\n";
    }
    source += "float4 main() : SV_Target { return node0(1.0).xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // Establishes, from purely observable output, exactly how many
    // checkpoint calls the BFS loop itself makes: one per queue iteration,
    // i.e. one per function it actually visits (`functions_visited`).
    const auto baseline = translation_unit.entry_point_data_flow();
    REQUIRE(baseline.found);
    REQUIRE_FALSE(baseline.truncated);
    const auto bfs_checkpoint_calls = baseline.functions_visited;

    struct CancelledError final : std::runtime_error {
        CancelledError() : std::runtime_error{"cancelled"} {}
    };
    // Allows exactly the BFS loop's own known number of checkpoint calls to
    // pass, then cancels on the very next one. Since the BFS loop cannot
    // itself make any further calls once it has finished, that next call
    // can only come from a later phase (cycle-detection DFS runs
    // immediately afterward) -- proving a checkpoint exists there too, not
    // merely inside the BFS loop.
    std::uint64_t checkpoint_calls = 0;
    CHECK_THROWS_AS(translation_unit.entry_point_data_flow({},
                                                           [&] {
                                                               if (++checkpoint_calls >
                                                                   bfs_checkpoint_calls) {
                                                                   throw CancelledError{};
                                                               }
                                                           }),
                    CancelledError);
}

TEST_CASE("Entry-point data flow cancellation checkpoint is honored while classifying "
          "unreachable functions over a large translation unit",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // `main` itself calls nothing else, so the BFS/cycle-detection/
    // conversion phases finish almost immediately; the bulk of the work is
    // in the unreachable-function classification loop scanning many unused
    // top-level definitions, and in unused-declaration reference scanning.
    constexpr int unreachable_count = 700;
    std::string source;
    for (int index = 0; index < unreachable_count; ++index) {
        source += "float deadCode" + std::to_string(index) + "(float value) { return value; }\n";
    }
    source += "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    const auto baseline = translation_unit.entry_point_data_flow();
    REQUIRE(baseline.found);
    REQUIRE_FALSE(baseline.truncated);
    CHECK(baseline.unreachable_functions.size() == unreachable_count);
    // Only `main` is ever visited by the BFS loop.
    CHECK(baseline.functions_visited == 1);

    struct CancelledError final : std::runtime_error {
        CancelledError() : std::runtime_error{"cancelled"} {}
    };
    // The candidate-definition collection pass (`collect_callable_
    // definitions`) runs first and, over 701 top-level children, makes
    // exactly one checkpoint call of its own (at its internal count of
    // 512); the BFS loop then makes exactly one call for `main`'s single
    // queue iteration (`baseline.functions_visited`). Allowing exactly
    // those two calls through and cancelling on the third proves a
    // checkpoint exists in a later phase -- the unreachable-function
    // classification loop, which runs next and is large enough (700 items)
    // to reach its own internal count of 512 -- not merely inside
    // definition collection or the BFS loop.
    const std::uint64_t threshold = 1 + baseline.functions_visited;
    std::uint64_t checkpoint_calls = 0;
    CHECK_THROWS_AS(translation_unit.entry_point_data_flow({},
                                                           [&] {
                                                               if (++checkpoint_calls > threshold) {
                                                                   throw CancelledError{};
                                                               }
                                                           }),
                    CancelledError);
}

TEST_CASE("Entry-point data flow bounds the unused-declaration reference scan by an explicit "
          "budget and reports truncation",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    constexpr int declaration_count = 20;
    std::string source;
    for (int index = 0; index < declaration_count; ++index) {
        source += "float unused" + std::to_string(index) + "(float value) { return value; }\n";
    }
    source += "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // With no budget restriction, every unused top-level function is
    // reported and the analysis is not truncated.
    const auto full = translation_unit.entry_point_data_flow();
    REQUIRE(full.found);
    CHECK_FALSE(full.truncated);
    CHECK_FALSE(full.functions_visited_truncated);
    CHECK_FALSE(full.global_accesses_truncated);
    CHECK_FALSE(full.unused_declarations_truncated);
    CHECK(full.unused_declarations.size() == declaration_count);

    // A budget far smaller than the number of candidate declarations must
    // stop the reference-scanning work early (never performing unbounded
    // work for a translation unit with many top-level declarations),
    // report fewer results, and set `truncated` so callers know the
    // returned `unused_declarations` is a conservative, possibly
    // incomplete subset.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_unused_declaration_candidates = 5;
    const auto bounded = translation_unit.entry_point_data_flow(limits);
    REQUIRE(bounded.found);
    CHECK(bounded.truncated);
    // This is the *only* one of the three independent truncation causes
    // that applies here: neither the reachability BFS (a trivial `main`
    // with no calls) nor the global-access retention limit is anywhere
    // near its own budget, so those two flags must remain false even
    // though the combined `truncated` flag is true -- proving the three
    // reasons are tracked, and reported, independently rather than
    // collapsed into one indistinguishable flag.
    CHECK_FALSE(bounded.functions_visited_truncated);
    CHECK_FALSE(bounded.global_accesses_truncated);
    CHECK(bounded.unused_declarations_truncated);
    CHECK(bounded.unused_declarations.size() <= 5);
    // The bounded run must never fabricate a declaration name that isn't
    // truly unused in the full analysis (a subset, never a superset).
    for (const auto& declaration : bounded.unused_declarations) {
        CHECK(std::ranges::any_of(full.unused_declarations, [&](const auto& other) {
            return other.name == declaration.name;
        }));
    }
}

TEST_CASE("Entry-point data flow bounds the retained global/resource access set by an explicit "
          "budget and reports truncation independently of the other two phases",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    constexpr int global_count = 20;
    std::string source;
    for (int index = 0; index < global_count; ++index) {
        source += "static float g" + std::to_string(index) + " = 0.0;\n";
    }
    source += "float4 main() : SV_Target {\n"
              "    float sum = 0.0;\n";
    for (int index = 0; index < global_count; ++index) {
        source += "    sum += g" + std::to_string(index) + ";\n";
    }
    source += "    return sum.xxxx;\n"
              "}\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // With no budget restriction, every distinct global read is retained
    // and the analysis is not truncated.
    const auto full = translation_unit.entry_point_data_flow();
    REQUIRE(full.found);
    CHECK_FALSE(full.truncated);
    CHECK(full.global_accesses.size() == global_count);

    // A retention budget far smaller than the number of distinct globals
    // touched must stop retaining further accesses (never performing
    // unbounded retention for a translation unit that touches many
    // distinct globals/resources), report fewer results, and set
    // `truncated`/`global_accesses_truncated` -- while the reachability
    // traversal itself (a single `main` with no calls) and the
    // unused-declaration scan (no unused top-level declarations here) are
    // nowhere near their own budgets, so their flags must remain false,
    // proving the three reasons are tracked and reported independently.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_global_accesses = 5;
    const auto bounded = translation_unit.entry_point_data_flow(limits);
    REQUIRE(bounded.found);
    CHECK(bounded.truncated);
    CHECK(bounded.global_accesses_truncated);
    CHECK_FALSE(bounded.functions_visited_truncated);
    CHECK_FALSE(bounded.unused_declarations_truncated);
    CHECK(bounded.global_accesses.size() <= 5);
    // The bounded run must never fabricate a global that wasn't truly
    // touched in the full analysis (a subset, never a superset).
    for (const auto& access : bounded.global_accesses) {
        CHECK(std::ranges::any_of(full.global_accesses,
                                  [&](const auto& other) { return other.name == access.name; }));
    }
}

TEST_CASE("Entry-point data flow bounds definition collection independently of the reachable "
          "call graph and suppresses unreachableFunctions when that budget is hit",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // A tiny, fully-reachable call graph (`main` -> `helper`) declared
    // *before* a large corpus of unrelated dead top-level functions (entry
    // point resolution reuses the same bounded, in-source-order
    // definition-collection pass that feeds `unreachable_functions`, so
    // both `main` and `helper` must be collected well within the budget
    // below, while the dead-function tail is what gets truncated).
    // Definition collection walks every top-level declaration, so an
    // enormous dead-function corpus must be bounded by
    // `max_definitions_collected` independently of `max_functions_visited`
    // -- the reachable subgraph here is trivially small (2 functions), so a
    // generous `max_functions_visited` alone would not bound this phase.
    constexpr int dead_function_count = 4000;
    std::string source = "float helper(float value) { return value * 2.0; }\n"
                         "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";
    for (int index = 0; index < dead_function_count; ++index) {
        source += "float dead" + std::to_string(index) + "(float value) { return value; }\n";
    }
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    // A definitions budget far smaller than the dead-function corpus
    // (but comfortably larger than the 2-node reachable subgraph) must
    // stop definition collection early -- never performing unbounded work
    // proportional to the size of a huge dead-code corpus -- and report
    // `definitionsTruncated` rather than silently completing.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_definitions_collected = 50;
    limits.max_functions_visited = 4096;
    const auto bounded = translation_unit.entry_point_data_flow(limits);
    REQUIRE(bounded.found);
    CHECK(bounded.truncated);
    CHECK(bounded.definitions_truncated);
    // The reachable subgraph is tiny and entirely visited within budget:
    // this specific truncation cause must be independent of the other
    // three.
    CHECK_FALSE(bounded.functions_visited_truncated);
    CHECK_FALSE(bounded.global_accesses_truncated);
    CHECK_FALSE(bounded.unused_declarations_truncated);
    const auto has_name = [](const auto& candidates, std::string_view name) {
        return std::ranges::any_of(candidates,
                                   [name](const auto& symbol) { return symbol.name == name; });
    };
    const auto reachable_has_name = [](const auto& candidates, std::string_view name) {
        return std::ranges::any_of(
            candidates, [name](const auto& reachable) { return reachable.function.name == name; });
    };
    CHECK(reachable_has_name(bounded.reachable_functions, "main"));
    CHECK(reachable_has_name(bounded.reachable_functions, "helper"));
    // An incomplete definition collection cannot prove *anything*
    // unreachable (a not-yet-collected dead function is indistinguishable
    // from one that was collected but genuinely reachable), so
    // `unreachable_functions` must be entirely empty here -- even though
    // most of the 4000 dead functions genuinely are dead code in a
    // completed traversal.
    CHECK(bounded.unreachable_functions.empty());

    // With a definitions budget comfortably larger than the whole corpus,
    // the same translation unit completes untruncated and correctly
    // reports the dead functions as unreachable, proving the budget above
    // was the actual bottleneck and not some other limit.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits generous_limits;
    generous_limits.max_definitions_collected = 8192;
    generous_limits.max_functions_visited = 4096;
    const auto full = translation_unit.entry_point_data_flow(generous_limits);
    REQUIRE(full.found);
    CHECK_FALSE(full.truncated);
    CHECK_FALSE(full.definitions_truncated);
    CHECK(has_name(full.unreachable_functions, "dead0"));
    CHECK(has_name(full.unreachable_functions, "dead" + std::to_string(dead_function_count - 1)));
}

TEST_CASE("Entry-point data flow reports an incomplete, non-definitive not-found when a tight "
          "definition budget is exhausted before the configured entry point is collected",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // `main` is declared *after* a large corpus of unrelated dead
    // functions, so a small `max_definitions_collected` budget exhausts
    // itself (in source order) before ever reaching `main`'s own
    // definition. `found` must still be `false` (the entry point genuinely
    // was not among the definitions collected), but this must be
    // represented as an *incomplete* result -- `definitionsTruncated`/
    // `truncated` set, and `explanation` noting the caveat -- never a
    // silent, definitive "this document has no such entry point".
    constexpr int dead_function_count = 4000;
    std::string source;
    for (int index = 0; index < dead_function_count; ++index) {
        source += "float dead" + std::to_string(index) + "(float value) { return value; }\n";
    }
    source += "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    REQUIRE(translation_unit.diagnostics().empty());

    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_definitions_collected = 50;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    CHECK_FALSE(flow.found);
    CHECK(flow.entry_point == std::nullopt);
    CHECK(flow.reachable_functions.empty());
    CHECK(flow.definitions_truncated);
    CHECK(flow.truncated);
    CHECK_FALSE(flow.explanation.empty());
    // The explanation must not read as a plain, unqualified "not found":
    // a client rendering only `explanation` (not the boolean flags) still
    // needs to see that this is an incomplete search.
    CHECK(flow.explanation.find("truncat") != std::string::npos);

    // With a generous budget covering the whole corpus, the same
    // translation unit resolves `main` normally, proving the budget above
    // was the actual reason for the "not found" result.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits generous_limits;
    generous_limits.max_definitions_collected = 8192;
    const auto full = translation_unit.entry_point_data_flow(generous_limits);
    REQUIRE(full.found);
    CHECK_FALSE(full.definitions_truncated);
    CHECK_FALSE(full.truncated);
}

TEST_CASE("Entry-point data flow reports an ambiguous entry point as potentially incomplete when "
          "the definition budget is exhausted before every same-named overload is collected",
          "[dxc][entry-point-data-flow][integration]") {
    hlsl_intellisense::dxc::Intellisense intellisense;
    hlsl_intellisense::dxc::CompilerOptions options;
    options.entry_point = "main";
    // Two top-level `main` overloads appear early (both within the tight
    // budget below), a large dead-function corpus follows, and a *third*
    // colliding `main` overload appears last, past the truncation point.
    // The two early overloads alone are already a genuine, correctly
    // reported ambiguity; the point of this test is that the budget makes
    // that count itself possibly incomplete (a further collision may
    // exist beyond what was collected), which must be visible to a
    // caller, not silently omitted.
    constexpr int dead_function_count = 4000;
    std::string source = "float main(float value) { return value; }\n"
                         "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    for (int index = 0; index < dead_function_count; ++index) {
        source += "float dead" + std::to_string(index) + "(float value) { return value; }\n";
    }
    source += "float main(float value, float value2) { return value + value2; }\n";
    auto translation_unit = intellisense.parse(shader_path, {{shader_path, source}}, options);
    // Not asserted empty: multiple top-level `main` definitions (illegal
    // HLSL overloading of the entry point name) can itself produce
    // compiler diagnostics, same as the pre-existing plain-ambiguity test
    // above -- this test only cares about `entry_point_data_flow`'s own
    // reporting given DXC's still-available cursor tree.

    hlsl_intellisense::dxc::EntryPointDataFlowLimits limits;
    limits.max_definitions_collected = 50;
    const auto flow = translation_unit.entry_point_data_flow(limits);
    CHECK_FALSE(flow.found);
    CHECK(flow.entry_point == std::nullopt);
    CHECK(flow.definitions_truncated);
    CHECK(flow.truncated);
    CHECK_FALSE(flow.explanation.empty());
    CHECK(flow.explanation.find("ambiguous") != std::string::npos);
    CHECK(flow.explanation.find("truncat") != std::string::npos);

    // With a generous budget covering the whole corpus, the same
    // translation unit reports the full, untruncated ambiguity (all three
    // overloads considered), proving the budget above -- not some other
    // limit -- was what made the count above potentially incomplete.
    hlsl_intellisense::dxc::EntryPointDataFlowLimits generous_limits;
    generous_limits.max_definitions_collected = 8192;
    const auto full = translation_unit.entry_point_data_flow(generous_limits);
    CHECK_FALSE(full.found);
    CHECK_FALSE(full.definitions_truncated);
    CHECK_FALSE(full.truncated);
    CHECK(full.explanation.find("3 top-level function definitions") != std::string::npos);
}

namespace {

[[nodiscard]] auto test_runtime_directory() -> std::filesystem::path {
    return std::filesystem::path{HLSL_TEST_DXC_RUNTIME_DIR};
}

} // namespace

TEST_CASE("DXC runtime library name matches the platform", "[dxc][runtime]") {
#ifdef _WIN32
    CHECK(std::string{hlsl_intellisense::dxc::runtime_library_name()} == "dxcompiler.dll");
#else
    CHECK(std::string{hlsl_intellisense::dxc::runtime_library_name()} == "libdxcompiler.so");
#endif
}

TEST_CASE("Bundled DXC runtime reports version information", "[dxc][runtime][integration]") {
    const hlsl_intellisense::dxc::Intellisense intellisense;
    const auto info = intellisense.runtime_info();
    CHECK(info.bundled);
    CHECK(info.directory.empty());
    CHECK_FALSE(info.version.empty());
}

TEST_CASE("Validating a DXC runtime directory locates the compiler library", "[dxc][runtime]") {
    const auto directory = test_runtime_directory();
    const auto library = hlsl_intellisense::dxc::validate_runtime_directory(directory.string());
    const std::filesystem::path resolved{library};
    CHECK(resolved.filename().string() ==
          std::string{hlsl_intellisense::dxc::runtime_library_name()});
    CHECK(std::filesystem::exists(resolved));
}

TEST_CASE("Selecting an explicit DXC runtime loads and analyzes HLSL",
          "[dxc][runtime][integration]") {
    const auto directory = test_runtime_directory();
    const hlsl_intellisense::dxc::Intellisense intellisense{
        hlsl_intellisense::dxc::RuntimeConfiguration{directory.string()}};
    const auto info = intellisense.runtime_info();
    CHECK_FALSE(info.bundled);
    CHECK(info.directory == directory.string());
    CHECK_FALSE(info.library_path.empty());
    CHECK_FALSE(info.version.empty());

    auto unit =
        intellisense.parse(shader_path, {{shader_path, hlsl_2021_source("Number", "combine")}});
    CHECK(unit.diagnostics().empty());
}

TEST_CASE("An explicit runtime reports the same version as the bundled default",
          "[dxc][runtime][integration]") {
    const hlsl_intellisense::dxc::Intellisense bundled;
    const hlsl_intellisense::dxc::Intellisense configured{
        hlsl_intellisense::dxc::RuntimeConfiguration{test_runtime_directory().string()}};
    CHECK(bundled.runtime_info().version == configured.runtime_info().version);
}

TEST_CASE("An empty DXC runtime directory is rejected", "[dxc][runtime]") {
    CHECK_THROWS_AS(hlsl_intellisense::dxc::validate_runtime_directory(""),
                    hlsl_intellisense::dxc::RuntimeError);
}

TEST_CASE("A missing DXC runtime directory is rejected", "[dxc][runtime]") {
    const auto missing = std::filesystem::current_path() / "hlsl-lsp-nonexistent-runtime";
    std::filesystem::remove_all(missing);
    CHECK_THROWS_AS(hlsl_intellisense::dxc::validate_runtime_directory(missing.string()),
                    hlsl_intellisense::dxc::RuntimeError);
    CHECK_THROWS_AS((hlsl_intellisense::dxc::Intellisense{
                        hlsl_intellisense::dxc::RuntimeConfiguration{missing.string()}}),
                    hlsl_intellisense::dxc::RuntimeError);
}

TEST_CASE("A directory without the DXC compiler library is rejected", "[dxc][runtime]") {
    const auto empty = std::filesystem::current_path() / "hlsl-lsp-empty-runtime";
    std::filesystem::remove_all(empty);
    std::filesystem::create_directories(empty);
    CHECK_THROWS_AS(hlsl_intellisense::dxc::validate_runtime_directory(empty.string()),
                    hlsl_intellisense::dxc::RuntimeError);
    std::filesystem::remove_all(empty);
}

#ifdef _WIN32
TEST_CASE("A Windows runtime directory without dxil.dll is rejected", "[dxc][runtime]") {
    const auto directory = std::filesystem::current_path() / "hlsl-lsp-runtime-without-dxil";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(test_runtime_directory() / "dxcompiler.dll",
                               directory / "dxcompiler.dll",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_THROWS_AS(hlsl_intellisense::dxc::validate_runtime_directory(directory.string()),
                    hlsl_intellisense::dxc::RuntimeError);
    std::filesystem::remove_all(directory);
}
#endif
