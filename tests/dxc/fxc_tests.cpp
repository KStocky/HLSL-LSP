#include "dxc/fxc.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
using namespace hlsl_intellisense::dxc;
using namespace hlsl_intellisense::dxc::detail;

CompilerOptions options(std::string profile = "ps_5_0") {
    CompilerOptions result;
    result.backend = CompilerBackend::fxc;
    result.target_profile = std::move(profile);
    result.entry_point = "main";
    return result;
}

const std::string main_path = "C:\\fxc-snapshots\\main.hlsl";

#ifdef _WIN32
const std::string strictness_sensitive_source =
    "texture tex; sampler s=sampler_state { Texture=<tex>; }; "
    "float4 main():SV_Target{return 1;}";
#endif

CompilationInfo compile(std::string text, CompilerOptions configuration = options()) {
    return compilation_info_from_fxc({{main_path, std::move(text), false}}, configuration,
                                     main_path);
}

bool message(const CompilationInfo& result, std::string_view text) {
    return std::ranges::any_of(result.diagnostics, [&](const auto& diagnostic) {
        return diagnostic.message.find(text) != std::string::npos;
    });
}
} // namespace

TEST_CASE("FXC backend reports platform capability and truthful configuration", "[fxc]") {
    const auto result = compile("float4 main() : SV_Target { return 1; }");
    REQUIRE(result.compiler_backend == "fxc");
    REQUIRE(result.language_version == "FXC legacy HLSL");
    REQUIRE_FALSE(result.psv_wave_size.available);
#ifdef _WIN32
    INFO((result.diagnostics.empty() ? "" : result.diagnostics.front().message));
    REQUIRE(result.success);
    REQUIRE(result.output->type == "dxbc");
    REQUIRE(result.output->size > 0);
    REQUIRE(std::filesystem::path(result.compiler_runtime_path).is_absolute());
    REQUIRE(result.disassembly->available);
    REQUIRE(result.disassembly->format == "dxbc");
    REQUIRE(result.disassembly->text.find("ps_5_0") != std::string::npos);
    REQUIRE(result.reflection->available);
    REQUIRE(result.reflection->statistics.has_value());
    REQUIRE(result.reflection->statistics->instruction_count > 0);
    REQUIRE(result.root_signature);
    REQUIRE(result.root_signature->availability == RootSignatureAvailability::absent);
    REQUIRE(result.compatibility->status == ResourceCompatibilityStatus::unknown);
#else
    REQUIRE_FALSE(result.success);
    REQUIRE(message(result, "Windows"));
    REQUIRE_FALSE(result.output);
#endif
}

TEST_CASE("FXC failure diagnostics retain backend provenance", "[fxc]") {
    auto configuration = options();
    configuration.fxc_runtime_path = "C:\\missing-fxc-runtime\\D3DCompiler_47.dll";
    const std::vector<SourceFile> sources{{main_path, "#error snapshot_failure\n", false}};
    const auto compilation = compilation_info_from_fxc(sources, configuration, main_path);
    REQUIRE_FALSE(compilation.success);
    REQUIRE_FALSE(compilation.diagnostics.empty());
    REQUIRE(std::ranges::all_of(compilation.diagnostics,
                                [](const auto& item) { return item.source == "fxc"; }));
    const auto invalid_runtime = preprocess_from_fxc(sources, configuration, main_path);
    REQUIRE_FALSE(invalid_runtime.available);
    REQUIRE_FALSE(invalid_runtime.diagnostics.empty());
    REQUIRE(std::ranges::all_of(invalid_runtime.diagnostics,
                                [](const auto& item) { return item.source == "fxc"; }));
    const auto preprocessing = preprocess_from_fxc(sources, options(), main_path);
    REQUIRE_FALSE(preprocessing.available);
    REQUIRE_FALSE(preprocessing.diagnostics.empty());
    REQUIRE(std::ranges::all_of(preprocessing.diagnostics,
                                [](const auto& item) { return item.source == "fxc"; }));
}

#ifdef _WIN32
TEST_CASE("FXC diagnostics retain compiler locations and successful warnings", "[fxc]") {
    const auto bad = compile("float4 main() : SV_Target { return missing; }");
    REQUIRE_FALSE(bad.success);
    REQUIRE(message(bad, "missing"));
    REQUIRE(std::ranges::any_of(bad.diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error && diagnostic.location.line == 1 &&
               diagnostic.source == "fxc" && diagnostic.location.line == 1 &&
               diagnostic.location.column > 0;
    }));
    const std::string warning = "float4 main() : SV_Target { float x = float2(1,2); return x; }";
    const auto success = compile(warning);
    REQUIRE(success.success);
    REQUIRE(std::ranges::any_of(success.diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::warning && diagnostic.source == "fxc";
    }));
    auto flags = options();
    flags.additional_arguments = {"/WX"};
    REQUIRE_FALSE(compile(warning, flags).success);
}

TEST_CASE("FXC diagnostic columns use one-based UTF-16 for unsaved UTF-8 snapshots", "[fxc]") {
    const std::string prefix = "float4 main():SV_Target { /*\xC3\xA9 \xF0\x9F\x98\x80*/ return ";
    const std::uint32_t expected_column = static_cast<std::uint32_t>(prefix.size() - 3 + 1);
    const std::string bad_line = prefix + "missing; }";
    auto require_location = [&](const CompilationInfo& result, const std::string& path,
                                std::uint32_t line, std::uint32_t line_start) {
        REQUIRE_FALSE(result.success);
        const auto found = std::ranges::find_if(result.diagnostics, [](const auto& item) {
            return item.message.find("undeclared identifier") != std::string::npos &&
                   item.message.find("missing") != std::string::npos;
        });
        REQUIRE(found != result.diagnostics.end());
        INFO(found->message);
        INFO("UTF-8 byte column: " << prefix.size() + 1);
        REQUIRE(found->location.path == path);
        REQUIRE(found->location.line == line);
        REQUIRE(found->location.column == expected_column);
        REQUIRE(found->location.offset == line_start + prefix.size());
    };
    require_location(compile(bad_line), main_path, 1, 0);
    const std::string include_path = "C:\\fxc-snapshots\\unicode.h";
    const auto included = compilation_info_from_fxc(
        {{main_path, "#include \"unicode.h\"\n", false}, {include_path, "\r\n" + bad_line, false}},
        options(), main_path);
    require_location(included, include_path, 2, 2);
}

TEST_CASE("FXC compiles snapshot-only nested includes and observes edits", "[fxc]") {
    auto flags = options();
    flags.defines = {"VALUE=7"};
    flags.include_directories = {"C:\\fxc-snapshots\\includes"};
    std::vector<SourceFile> sources{
        {main_path, "#include \"outer.h\"\nfloat4 main():SV_Target{return VALUE+inner();}", false},
        {"C:\\fxc-snapshots\\includes\\outer.h", "#include \"nested\\inner.h\"\n", false},
        {"C:\\fxc-snapshots\\includes\\nested\\inner.h", "float inner(){return 3;}", false}};
    const auto first = compilation_info_from_fxc(sources, flags, main_path);
    INFO((first.diagnostics.empty() ? "" : first.diagnostics.front().message));
    REQUIRE(first.success);
    REQUIRE(first.resolved_include_paths.size() == 2);
    const auto preprocessed = preprocess_from_fxc(sources, flags, main_path);
    REQUIRE(preprocessed.available);
    REQUIRE(preprocessed.text.find("return 3") != std::string::npos);
    REQUIRE(preprocessed.text.find("return 7") != std::string::npos);
    sources.back().text = "float inner(){return unknown;}";
    REQUIRE_FALSE(compilation_info_from_fxc(sources, flags, main_path).success);
    sources.pop_back();
    const auto missing = compilation_info_from_fxc(sources, flags, main_path);
    REQUIRE_FALSE(missing.success);
    REQUIRE(message(missing, "snapshot not supplied"));

    sources = {{main_path,
                "#include \"C:\\\\fxc-snapshots\\\\real.h\"\n"
                "float4 main():SV_Target{return value;}",
                true},
               {"c:\\FXC-SNAPSHOTS\\real.h", "static const float value=4;", false}};
    REQUIRE(compilation_info_from_fxc(sources, flags, "c:\\FXC-SNAPSHOTS\\MAIN.hlsl").success);
}

TEST_CASE("FXC accepts native flags and rejects ambiguous or DXC-only overrides", "[fxc]") {
    const std::string source = "float4 main():SV_Target{return 1;}";
    for (const auto* flag :
         {"/Zi", "/Od", "/O0", "/O1", "/O2", "/O3", "/Zpr", "/Zpc", "/Ges", "/Gec", "/Gis", "/Gfa",
          "/Gfp", "/Vd", "/all_resources_bound", "/enable_unbounded_descriptor_tables", "-WX"}) {
        CAPTURE(flag);
        auto configuration = options();
        configuration.additional_arguments = {flag};
        const auto result = compile(source, configuration);
        INFO((result.diagnostics.empty() ? "" : result.diagnostics.front().message));
        REQUIRE(result.success);
    }
    for (const auto& arguments : std::vector<std::vector<std::string>>{{"/Zpr", "/Zpc"},
                                                                       {"/O0", "/O3"},
                                                                       {"/Od", "/O2"},
                                                                       {"/Gfa", "/Gfp"},
                                                                       {"/Ges", "/Gec"},
                                                                       {"-HV", "2021"},
                                                                       {"-spirv"},
                                                                       {"-T", "ps_5_0"},
                                                                       {"-Eother"},
                                                                       {"-DVALUE=3"},
                                                                       {"-Iinc"},
                                                                       {"-unknown"}}) {
        CAPTURE(arguments);
        auto configuration = options();
        configuration.additional_arguments = arguments;
        const auto result = compile(source, configuration);
        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.diagnostics.empty());
    }
    REQUIRE_FALSE(compile(source, options("ps_6_0")).success);
    REQUIRE_FALSE(compile(source, options("lib_6_3")).success);
    REQUIRE(compile(source, options("ps_4_0")).success);
    auto compute_flags = options("cs_5_0");
    compute_flags.additional_arguments = {"/res_may_alias"};
    REQUIRE(compile("[numthreads(1,1,1)] void main(){}", compute_flags).success);
}

TEST_CASE("FXC default compilation does not implicitly enable strictness", "[fxc]") {
    const auto default_result = compile(strictness_sensitive_source);
    REQUIRE(default_result.success);
    REQUIRE(default_result.output);
    REQUIRE(default_result.output->type == "dxbc");
    auto configuration = options();
    configuration.additional_arguments = {"/Ges"};
    const auto strict = compile(strictness_sensitive_source, configuration);
    REQUIRE_FALSE(strict.success);
    REQUIRE(message(strict, "X3086"));
    REQUIRE(message(strict, "untyped textures are deprecated in strict mode"));
    REQUIRE_FALSE(strict.output);
    configuration.additional_arguments = {"/Gec", "/Ges"};
    const auto conflicting = compile(strictness_sensitive_source, configuration);
    REQUIRE_FALSE(conflicting.success);
    REQUIRE(message(conflicting, "conflicting flags"));
}

TEST_CASE("FXC rejects SM2 and SM3 DX9 bytecode profiles", "[fxc]") {
    for (const auto* profile : {"vs_2_0", "ps_2_0", "vs_3_0", "ps_3_0"}) {
        CAPTURE(profile);
        const auto result = compile("float4 main():COLOR{return 1;}", options(profile));
        REQUIRE_FALSE(result.success);
        REQUIRE(message(result, "SM2/SM3 DX9 bytecode"));
        REQUIRE_FALSE(result.output);
        REQUIRE_FALSE(result.reflection);
        REQUIRE_FALSE(result.root_signature);
    }
}

TEST_CASE("FXC validates explicit runtime paths and runtime exports", "[fxc]") {
    auto configuration = options();
    const auto source = "float4 main():SV_Target{return 1;}";
    configuration.fxc_runtime_path = "D3DCompiler_47.dll";
    REQUIRE_FALSE(compile(source, configuration).success);
    configuration.fxc_runtime_path = "C:\\missing-fxc-runtime\\D3DCompiler_47.dll";
    REQUIRE_FALSE(compile(source, configuration).success);
    const auto default_result = compile(source);
    configuration.fxc_runtime_path = default_result.compiler_runtime_path;
    REQUIRE(compile(source, configuration).success);
    configuration.fxc_runtime_path =
        (std::filesystem::path(default_result.compiler_runtime_path).parent_path() / "version.dll")
            .string();
    const auto wrong = compile(source, configuration);
    REQUIRE_FALSE(wrong.success);
    REQUIRE(message(wrong, "export"));
}

TEST_CASE("FXC reflects resources signatures and original nested cbuffer offsets", "[fxc]") {
    const std::string source =
        "struct Nested { float2 uv; float value; };"
        "cbuffer Constants:register(b2) { float4 color; Nested data; float scale; };"
        "Texture2D image:register(t3); SamplerState samp:register(s1);"
        "float4 main(float2 uv:TEXCOORD0):SV_Target {"
        "return image.Sample(samp,uv+data.uv)*color*(data.value+scale); }";
    const auto result = compile(source);
    REQUIRE(result.success);
    REQUIRE(result.reflection->available);
    REQUIRE(result.reflection->resources.size() == 3);
    const auto& signature = result.reflection->input_signature.front();
    REQUIRE(signature.semantic_name == "TEXCOORD");
    REQUIRE(signature.component_type == "float32");
    REQUIRE(signature.mask == 3);
    REQUIRE(signature.interpolation == InterpolationMode::undefined);
    REQUIRE_FALSE(signature.interpolation_available);
    REQUIRE_FALSE(result.reflection->unavailable_reason.empty());
    REQUIRE(result.reflection->output_signature.front().system_value == "target");
    const auto& resources = result.reflection->resources;
    const auto image = std::ranges::find(resources, "image", &CompilationResourceBinding::name);
    REQUIRE(image != resources.end());
    REQUIRE(image->bind_point == 3);
    REQUIRE(image->dimension == "texture2d");
    REQUIRE(image->register_class == ResourceRegisterClass::srv);
    REQUIRE(image->space == 0);
    REQUIRE_FALSE(image->range_id_available);
    REQUIRE(result.reflection->binding_analysis.groups.size() == 3);
    ProbeTarget target;
    target.cbuffer_name = "Constants";
    target.selected_field = "data";
    const auto layout =
        memory_layout_from_fxc({{main_path, source, false}}, options(), main_path, target);
    REQUIRE(layout);
    REQUIRE_FALSE(layout->supported);
    REQUIRE(layout->size == 32);
    REQUIRE(layout->members.size() == 3);
    REQUIRE(layout->members[0].offset == 0);
    REQUIRE(layout->members[1].offset == 16);
    REQUIRE(layout->members[1].members.size() == 2);
    REQUIRE(layout->members[1].members[0].offset == 16);
    REQUIRE(layout->members[1].members[0].size == 8);
    REQUIRE(layout->members[1].members[1].offset == 24);
    REQUIRE(layout->packed_offset == 16);
    target = {};
    target.type_name = "Nested";
    const auto natural =
        memory_layout_from_fxc({{main_path, source, false}}, options(), main_path, target);
    REQUIRE_FALSE(natural->supported);
    REQUIRE(natural->members.empty());
}

TEST_CASE("FXC reflects compute thread groups and native statistics", "[fxc]") {
    const auto result =
        compile("RWStructuredBuffer<uint> output:register(u0);"
                "[numthreads(8,4,2)] void main(uint3 id:SV_DispatchThreadID){output[id.x]=id.y;}",
                options("cs_5_0"));
    REQUIRE(result.success);
    REQUIRE(result.reflection->thread_group_size);
    REQUIRE(result.reflection->thread_group_size->x == 8);
    REQUIRE(result.reflection->thread_group_size->y == 4);
    REQUIRE(result.reflection->thread_group_size->z == 2);
    REQUIRE(result.reflection->statistics->texture_store_instruction_count > 0);
}

TEST_CASE("FXC SM5.1 spaces are native or explicitly unavailable", "[fxc]") {
    const auto result = compile("Texture2D<float4> tex:register(t2,space7);"
                                "float4 main():SV_Target{return tex.Load(int3(0,0,0));}",
                                options("ps_5_1"));
    REQUIRE(result.success);
    REQUIRE(result.reflection);
    if (result.reflection->available) {
        REQUIRE(result.reflection->resources.size() == 1);
        REQUIRE(result.reflection->resources.front().space == 7);
        REQUIRE(result.reflection->resources.front().range_id_available);
        REQUIRE(result.reflection->binding_analysis.groups.front().space == 7);
    } else {
        REQUIRE(result.reflection->resources.empty());
        REQUIRE(result.reflection->unavailable_reason.find("SM5.1") != std::string::npos);
    }
}

TEST_CASE("FXC native blob-part query reports embedded root-signature presence", "[fxc]") {
    const auto result = compile("[RootSignature(\"RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)\")]"
                                "float4 main():SV_Target{return 1;}",
                                options("ps_5_1"));
    INFO((result.diagnostics.empty() ? "" : result.diagnostics.front().message));
    REQUIRE(result.success);
    REQUIRE(result.root_signature);
    REQUIRE(result.root_signature->availability ==
            RootSignatureAvailability::present_details_unavailable);
    REQUIRE(result.root_signature->unavailable_reason.find("D3DGetBlobPart") != std::string::npos);
    REQUIRE_FALSE(result.root_signature->details);
    REQUIRE(result.compatibility);
    REQUIRE(result.compatibility->status == ResourceCompatibilityStatus::unknown);
    REQUIRE(result.compatibility->explanation.find("deserialization") != std::string::npos);
}

TEST_CASE("FXC matrix flags are reflected without synthesizing strides", "[fxc]") {
    const std::string source = "cbuffer Matrices { float2x3 transform; float4 values[2]; };"
                               "float4 main():SV_Target{return float4(transform[0],1)+values[1];}";
    ProbeTarget target;
    target.cbuffer_name = "Matrices";
    for (const auto* flag : {"/Zpr", "/Zpc"}) {
        auto configuration = options();
        configuration.additional_arguments = {flag};
        const auto reflected =
            memory_layout_from_fxc({{main_path, source, false}}, configuration, main_path, target);
        REQUIRE(reflected);
        INFO(reflected->explanation);
        REQUIRE_FALSE(reflected->supported);
        REQUIRE(reflected->members.size() == 2);
        REQUIRE(reflected->members[0].kind == MemoryLayoutElementKind::matrix);
        REQUIRE(reflected->members[0].row_major == (std::string_view(flag) == "/Zpr"));
        REQUIRE(reflected->members[0].matrix_stride == 0);
        REQUIRE(reflected->members[1].kind == MemoryLayoutElementKind::array);
        REQUIRE(reflected->members[1].array_dimensions == std::vector<std::uint32_t>{2});
        REQUIRE(reflected->explanation.find("unavailable") != std::string::npos);
    }
}

TEST_CASE("FXC native output agrees with SDK fxc when installed", "[fxc]") {
    // This optional independent oracle uses CreateProcess, never a command shell.
    // The backend itself has no executable dependency.
    const std::filesystem::path sdk = "C:\\Program Files (x86)\\Windows Kits\\10\\bin";
    std::filesystem::path executable;
    if (std::filesystem::is_directory(sdk))
        for (const auto& version : std::filesystem::directory_iterator(sdk)) {
            const auto candidate = version.path() / "x64" / "fxc.exe";
            if (std::filesystem::is_regular_file(candidate) &&
                candidate.native() > executable.native())
                executable = candidate;
        }
    if (executable.empty())
        SKIP("Windows SDK fxc.exe is not installed.");
    struct Files {
        std::filesystem::path path =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "out" /
            "verification" / ("fxc-api-" + std::to_string(GetCurrentProcessId()));
        Files() { std::filesystem::create_directories(path); }
        ~Files() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } files;
    const auto source_path = files.path / "comparison.hlsl";
    const auto output_path = files.path / "comparison.dxbc";
    const auto& source = strictness_sensitive_source;
    {
        std::ofstream stream(source_path, std::ios::binary);
        stream << source;
        REQUIRE(stream.good());
    }
    auto command = L"\"" + executable.native() + L"\" /nologo /T ps_5_0 /E main /Fo \"" +
                   output_path.native() + L"\" \"" + source_path.native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    const auto wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 1);
    DWORD exit_code{};
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REQUIRE(wait == WAIT_OBJECT_0);
    REQUIRE(exit_code == 0);
    const auto native = compilation_info_from_fxc({{source_path.string(), source, false}},
                                                  options(), source_path.string());
    REQUIRE(native.success);
    REQUIRE(native.output->size == std::filesystem::file_size(output_path));
    std::ifstream stream(output_path, std::ios::binary);
    char magic[4]{};
    stream.read(magic, sizeof(magic));
    REQUIRE(std::string(magic, sizeof(magic)) == "DXBC");
}
#endif
