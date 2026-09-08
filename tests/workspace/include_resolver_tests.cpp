#include <hlsl_intellisense/workspace/include_resolver.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace workspace = hlsl_intellisense::workspace;

namespace {

class TestTree final {
  public:
    TestTree() {
        static std::size_t next_id{};
        root_ = std::filesystem::current_path() /
                ("include-resolver-tests-" + std::to_string(next_id++));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    TestTree(const TestTree&) = delete;
    auto operator=(const TestTree&) -> TestTree& = delete;
    ~TestTree() { std::filesystem::remove_all(root_); }

    [[nodiscard]] std::filesystem::path path(std::string_view relative) const {
        return root_ / std::filesystem::path{relative};
    }

    void file(std::string_view relative, std::string_view contents) const { // NOLINT
        const auto destination = path(relative);
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream{destination, std::ios::binary};
        REQUIRE(stream);
        stream << contents;
        REQUIRE(stream);
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] workspace::SourceSnapshot snapshot(const std::filesystem::path& path,
                                                 std::string text) {
    return {workspace::DocumentUri::from_path(path.string()), "hlsl", 1, std::move(text)};
}

[[nodiscard]] bool has_source(const workspace::IncludeResolution& resolution,
                              std::string_view path) {
    return std::ranges::any_of(resolution.sources,
                               [path](const auto& source) { return source.path == path; });
}

[[nodiscard]] const hlsl_intellisense::dxc::SourceFile&
source_file(const workspace::IncludeResolution& resolution, const std::filesystem::path& path) {
    const auto logical_path = std::filesystem::absolute(path).lexically_normal().generic_string();
    const auto source = std::ranges::find(resolution.sources, logical_path,
                                          &hlsl_intellisense::dxc::SourceFile::path);
    REQUIRE(source != resolution.sources.end());
    return *source;
}

[[nodiscard]] const workspace::IncludeResolution::File&
root_file(const workspace::IncludeResolution& resolution, const std::filesystem::path& path) {
    const auto physical_path = std::filesystem::absolute(path).lexically_normal().generic_string();
    const auto file = std::ranges::find(resolution.files, physical_path,
                                        &workspace::IncludeResolution::File::physical_path);
    REQUIRE(file != resolution.files.end());
    return *file;
}

} // namespace

TEST_CASE("Include resolution uses open buffers and recursively tracks disk files",
          "[workspace][includes]") {
    TestTree tree;
    tree.file("include/disk.hlsli", "#include \"nested.hlsli\"\nfloat diskValue;\n");
    tree.file("include/nested.hlsli", "float nestedValue;\n");

    const auto root_path = tree.path("shaders/root.hlsl");
    const auto open_path = tree.path("shaders/open.hlsli");
    const auto root =
        snapshot(root_path, "#include \"open.hlsli\"\n#include <disk.hlsli>\nfloat4 main();\n");
    const std::vector open_documents{root, snapshot(open_path, "float unsavedValue;\n")};
    workspace::WorkspaceConfiguration configuration;
    configuration.additional_include_directories.push_back(tree.path("include"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);

    REQUIRE(resolution.sources.size() == 4);
    CHECK(has_source(resolution,
                     std::filesystem::absolute(root_path).lexically_normal().generic_string()));
    CHECK(has_source(resolution,
                     std::filesystem::absolute(open_path).lexically_normal().generic_string()));
    CHECK(has_source(resolution, std::filesystem::absolute(tree.path("include/disk.hlsli"))
                                     .lexically_normal()
                                     .generic_string()));
    CHECK(has_source(resolution, std::filesystem::absolute(tree.path("include/nested.hlsli"))
                                     .lexically_normal()
                                     .generic_string()));
    CHECK(resolution.dependency_identities.size() == 3);
}

TEST_CASE("Include resolution reports resolved missing cyclic and macro directives",
          "[workspace][includes][explorer]") {
    TestTree tree;
    tree.file("nested.hlsli", "#include \"root.hlsl\"\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"nested.hlsli\"\n"
                                          "#include \"missing.hlsli\"\n"
                                          "#define DYNAMIC_HEADER \"dynamic.hlsli\"\n"
                                          "#include DYNAMIC_HEADER\n");
    const std::vector open_documents{root};

    const auto resolution =
        workspace::resolve_includes(root, open_documents, workspace::WorkspaceConfiguration{});

    REQUIRE(resolution.files.size() == 2);
    const auto root_file = std::ranges::find(
        resolution.files, std::filesystem::absolute(root_path).lexically_normal().generic_string(),
        &workspace::IncludeResolution::File::physical_path);
    REQUIRE(root_file != resolution.files.end());
    REQUIRE(root_file->includes.size() == 3);
    CHECK(root_file->open);
    CHECK(root_file->includes[0].status == workspace::IncludeResolution::Status::resolved);
    CHECK(root_file->includes[0].resolved_path ==
          std::filesystem::absolute(tree.path("nested.hlsli")).lexically_normal().generic_string());
    CHECK(root_file->includes[1].status == workspace::IncludeResolution::Status::missing);
    CHECK(root_file->includes[2].status == workspace::IncludeResolution::Status::dynamic);
    CHECK(root_file->includes[2].requested_path == "DYNAMIC_HEADER");
    CHECK(resolution.has_dynamic_includes);

    const auto nested_file =
        std::ranges::find(resolution.files, tree.path("nested.hlsli").generic_string(),
                          &workspace::IncludeResolution::File::physical_path);
    REQUIRE(nested_file != resolution.files.end());
    REQUIRE(nested_file->includes.size() == 1);
    CHECK(nested_file->includes[0].status == workspace::IncludeResolution::Status::cyclic);
}

TEST_CASE("Virtual mappings provide logical and physical names for DXC unsaved files",
          "[workspace][includes][virtual]") {
    TestTree tree;
    tree.file("Engine/Common.hlsli", "#include \"Nested.hlsli\"\nfloat commonValue;\n");
    tree.file("Engine/Nested.hlsli", "#include \"/Engine/Common.hlsli\"\nfloat nestedValue;\n");

    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"/Engine/Common.hlsli\"\nfloat4 main();\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.virtual_directory_mappings.emplace("/Engine", tree.path("Engine"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);

    REQUIRE(resolution.sources.size() == 5);
    CHECK(has_source(resolution, "/Engine/Common.hlsli"));
    CHECK(has_source(resolution, "/Engine/Nested.hlsli"));
    CHECK(has_source(resolution, tree.path("Engine/Common.hlsli").generic_string()));
    CHECK(has_source(resolution, tree.path("Engine/Nested.hlsli").generic_string()));
    const auto root_source =
        std::ranges::find(resolution.sources,
                          std::filesystem::absolute(root_path).lexically_normal().generic_string(),
                          &hlsl_intellisense::dxc::SourceFile::path);
    REQUIRE(root_source != resolution.sources.end());
    CHECK(root_source->text.find(tree.path("Engine/Common.hlsli").generic_string()) !=
          std::string::npos);
    const auto root_file = std::ranges::find(
        resolution.files, std::filesystem::absolute(root_path).lexically_normal().generic_string(),
        &workspace::IncludeResolution::File::physical_path);
    REQUIRE(root_file != resolution.files.end());
    CHECK(root_file->source_text == root.text());
    REQUIRE(root_file->includes.size() == 1);
    CHECK(root_file->includes[0].path_offset == root.text().find("/Engine/Common.hlsli"));
    CHECK(resolution.dependency_identities.size() == 2);
}

TEST_CASE("Virtual mappings normalize separators and preserve aliases",
          "[workspace][includes][virtual]") {
    TestTree tree;
    tree.file("Engine/Common.hlsli", "float commonValue;\n");

    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"\\Engine\\Common.hlsli\"\n"
                                          "#include \"/Other/Common.hlsli\"\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.virtual_directory_mappings.emplace("\\Engine", tree.path("Engine"));
    configuration.virtual_directory_mappings.emplace("/Other", tree.path("Engine"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);

    REQUIRE(resolution.sources.size() == 4);
    CHECK(has_source(resolution, "/Engine/Common.hlsli"));
    CHECK(has_source(resolution, "/Other/Common.hlsli"));
    CHECK(has_source(resolution, tree.path("Engine/Common.hlsli").generic_string()));
    CHECK(resolution.dependency_identities.size() == 1);
}

TEST_CASE("Include navigation resolves quoted, search-path, and virtual includes",
          "[workspace][includes][navigation]") {
    TestTree tree;
    tree.file("shaders/local.hlsli", "float localValue;\n");
    tree.file("includes/shared.hlsli", "float sharedValue;\n");
    tree.file("Engine/Common.hlsli", "float engineValue;\n");

    const auto root_path = tree.path("shaders/root.hlsl");
    const auto text = std::string{"#include \"local.hlsli\"\n"
                                  "#include <shared.hlsli>\n"
                                  "#include \"/Engine/Common.hlsli\"\n"};
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.additional_include_directories.push_back(tree.path("includes"));
    configuration.virtual_directory_mappings.emplace("/Engine", tree.path("Engine"));

    const auto local = workspace::resolve_include_at(root, open_documents, configuration,
                                                     text.find("local.hlsli") + 2);
    const auto shared = workspace::resolve_include_at(root, open_documents, configuration,
                                                      text.find("shared.hlsli") + 2);
    const auto virtual_include = workspace::resolve_include_at(
        root, open_documents, configuration, text.find("/Engine/Common.hlsli") + 2);

    REQUIRE(local.has_value());
    CHECK(*local == std::filesystem::absolute(tree.path("shaders/local.hlsli")).lexically_normal());
    REQUIRE(shared.has_value());
    CHECK(*shared ==
          std::filesystem::absolute(tree.path("includes/shared.hlsli")).lexically_normal());
    REQUIRE(virtual_include.has_value());
    CHECK(*virtual_include ==
          std::filesystem::absolute(tree.path("Engine/Common.hlsli")).lexically_normal());
    CHECK_FALSE(
        workspace::resolve_include_at(root, open_documents, configuration, text.find("#include")));
}

TEST_CASE("Configured object-like include macros resolve quoted angle and alias chains",
          "[workspace][includes][macros]") {
    TestTree tree;
    tree.file("includes/shared.hlsli", "float sharedValue;\n");
    const auto root_path = tree.path("shaders/root.hlsl");
    const auto local_path = tree.path("shaders/local.hlsli");
    const std::string text = "#include LOCAL_ALIAS\n"
                             "#include SHARED_HEADER\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root, snapshot(local_path, "float unsavedLocalValue;\n")};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("LOCAL_ALIAS", "LOCAL_HEADER");
    configuration.preprocessor_definitions.emplace("LOCAL_HEADER", "\"local.hlsli\"");
    configuration.preprocessor_definitions.emplace("SHARED_HEADER", "<shared.hlsli>");
    configuration.additional_include_directories.push_back(tree.path("includes"));
    configuration.definition_origins.emplace("LOCAL_ALIAS", "editor settings");
    configuration.definition_origins.emplace("SHARED_HEADER", "variant Test");

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);

    REQUIRE(file.includes.size() == 2);
    CHECK(file.includes[0].status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes[0].macro_expanded);
    CHECK(file.includes[0].requested_path == "LOCAL_ALIAS");
    CHECK(file.includes[0].path_offset == text.find("LOCAL_ALIAS"));
    CHECK(file.includes[0].expanded_path == "local.hlsli");
    CHECK(file.includes[0].quoted);
    CHECK(file.includes[0].configuration_macro == "LOCAL_ALIAS");
    CHECK(file.includes[0].configuration_origin == "editor settings");
    CHECK(file.includes[0].resolved_path ==
          std::filesystem::absolute(local_path).lexically_normal().generic_string());
    CHECK(file.includes[1].status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes[1].expanded_path == "shared.hlsli");
    CHECK_FALSE(file.includes[1].quoted);
    CHECK(file.includes[1].configuration_origin == "variant Test");
    CHECK_FALSE(resolution.has_dynamic_includes);
    CHECK(resolution.dependency_identities.size() == 2);
    CHECK(has_source(resolution,
                     std::filesystem::absolute(local_path).lexically_normal().generic_string()));

    const auto local = workspace::resolve_include_at(root, open_documents, configuration,
                                                     text.find("LOCAL_ALIAS") + 3);
    const auto shared = workspace::resolve_include_at(root, open_documents, configuration,
                                                      text.find("SHARED_HEADER") + 3);
    REQUIRE(local.has_value());
    CHECK(*local == std::filesystem::absolute(local_path).lexically_normal());
    REQUIRE(shared.has_value());
    CHECK(*shared ==
          std::filesystem::absolute(tree.path("includes/shared.hlsli")).lexically_normal());
    CHECK(workspace::is_include_directive_at(text, text.find("LOCAL_ALIAS") + 3));
}

TEST_CASE("Later DXC macro switches keep directly configured includes dynamic",
          "[workspace][includes][macros][arguments][safety]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::vector<std::string>> mutations{
        {"-D", "HEADER=\"runtime.hlsli\""},
        {"-DHEADER=\"runtime.hlsli\""},
        {"/D", "HEADER=\"runtime.hlsli\""},
        {"/DHEADER=\"runtime.hlsli\""},
        {"-U", "HEADER"},
        {"-UHEADER"},
        {"/U", "HEADER"},
        {"/UHEADER"},
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        configuration.additional_arguments = mutations[index];
        const auto root_path = tree.path("direct-override-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();

        INFO("arguments: " << mutations[index].front());
        CHECK(include.status == workspace::IncludeResolution::Status::dynamic);
        CHECK(include.expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(has_source(resolution, std::filesystem::absolute(tree.path("configured.hlsli"))
                                               .lexically_normal()
                                               .generic_string()));
    }
}

TEST_CASE("Later DXC macro switches keep configured alias chains dynamic",
          "[workspace][includes][macros][arguments][aliases][safety]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "HEADER_TARGET");
    configuration.preprocessor_definitions.emplace("HEADER_TARGET", "\"configured.hlsli\"");

    const std::vector<std::vector<std::string>> mutations{
        {"-D", "HEADER_TARGET=\"runtime.hlsli\""},
        {"-DHEADER_TARGET=\"runtime.hlsli\""},
        {"/D", "HEADER_TARGET=\"runtime.hlsli\""},
        {"/DHEADER_TARGET=\"runtime.hlsli\""},
        {"-U", "HEADER_TARGET"},
        {"-UHEADER_TARGET"},
        {"/U", "HEADER_TARGET"},
        {"/UHEADER_TARGET"},
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        configuration.additional_arguments = mutations[index];
        const auto root_path = tree.path("alias-override-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();

        INFO("arguments: " << mutations[index].front());
        CHECK(include.status == workspace::IncludeResolution::Status::dynamic);
        CHECK(include.expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Unrelated additional DXC arguments preserve safe configured include expansion",
          "[workspace][includes][macros][arguments]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("unrelated-arguments.hlsl");
    const auto root = snapshot(root_path, "#include HEADER\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "HEADER_TARGET");
    configuration.preprocessor_definitions.emplace("HEADER_TARGET", "\"configured.hlsli\"");
    configuration.additional_arguments = {"-Zi",       "-spirv",     "-D", "UNRELATED=1",
                                          "/DOTHER=2", "-UOLD_NAME", "/U", "OLDER_NAME"};

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& include = root_file(resolution, root_path).includes.front();

    CHECK(include.status == workspace::IncludeResolution::Status::resolved);
    CHECK(include.expanded_path == "configured.hlsli");
    CHECK_FALSE(resolution.has_dynamic_includes);
}

TEST_CASE("Opaque or oversized additional DXC arguments disable configured include expansion",
          "[workspace][includes][macros][arguments][safety]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");
    std::vector<std::vector<std::string>> opaque_arguments{
        {"@compiler-options.rsp"},
        std::vector<std::string>(4097, "-Zi"),
        {std::string(1024U * 1024U + 1U, 'x')},
    };

    for (std::size_t index = 0; index < opaque_arguments.size(); ++index) {
        configuration.additional_arguments = std::move(opaque_arguments[index]);
        const auto root_path = tree.path("opaque-arguments-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();

        CHECK(include.status == workspace::IncludeResolution::Status::dynamic);
        CHECK(include.expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Configured ShaderTestFramework macro includes rewrite virtual paths for DXC",
          "[workspace][includes][macros][virtual]") {
    TestTree tree;
    tree.file("Test/STF/AssertionsV1/Framework.hlsli", "float frameworkValue;\n");
    const auto config_path = tree.path("shadertoolsconfig.json");
    const auto root_path = tree.path("shader.hlsl");
    const std::string text = "#include STF_ASSERTIONS\n"
                             "float4 main() : SV_Target { return frameworkValue.xxxx; }\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("STF_ASSERTIONS",
                                                   "\"/Test/STF/AssertionsV1/Framework.hlsli\"");
    configuration.definition_origins.emplace("STF_ASSERTIONS", config_path.generic_string());
    configuration.definition_origin_files.emplace("STF_ASSERTIONS", config_path);
    configuration.virtual_directory_mappings.emplace("/Test", tree.path("Test"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);
    REQUIRE(file.includes.size() == 1);
    const auto& include = file.includes.front();
    CHECK(include.status == workspace::IncludeResolution::Status::resolved);
    CHECK(include.macro_expanded);
    CHECK(include.expanded_path == "/Test/STF/AssertionsV1/Framework.hlsli");
    CHECK(include.virtual_mapping == "/Test");
    CHECK(include.configuration_origin_file == config_path.generic_string());
    CHECK(file.source_text == text);
    CHECK(resolution.has_rewritten_sources);

    const auto source =
        std::ranges::find(resolution.sources,
                          std::filesystem::absolute(root_path).lexically_normal().generic_string(),
                          &hlsl_intellisense::dxc::SourceFile::path);
    REQUIRE(source != resolution.sources.end());
    CHECK(source->rewritten);
    CHECK(source->text.find("STF_ASSERTIONS") == std::string::npos);
    CHECK(source->text.find(
              std::filesystem::absolute(tree.path("Test/STF/AssertionsV1/Framework.hlsli"))
                  .lexically_normal()
                  .generic_string()) != std::string::npos);
    CHECK(source->text.ends_with(text.substr(text.find("float4"))));
}

TEST_CASE("Only bounded configured object-like header tokens resolve",
          "[workspace][includes][macros][safety]") {
    TestTree tree;
    tree.file("ok.hlsli", "float okValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("SOURCE_HEADER", "\"ok.hlsli\"");
    configuration.preprocessor_definitions.emplace("FUNCTION_HEADER", "\"ok.hlsli\"");
    configuration.preprocessor_definitions.emplace("CYCLE_A", "CYCLE_B");
    configuration.preprocessor_definitions.emplace("CYCLE_B", "CYCLE_A");
    configuration.preprocessor_definitions.emplace("MALFORMED_HEADER", "\"ok.hlsli");
    configuration.preprocessor_definitions.emplace("MULTI_TOKEN_HEADER", "\"ok.hlsli\" trailing");
    configuration.preprocessor_definitions.emplace("TOO_LARGE", std::string(4097, 'A'));
    for (std::size_t index = 0; index < 32; ++index) {
        const auto name = index == 0 ? std::string{"TOO_DEEP"} : "DEPTH_" + std::to_string(index);
        const auto next = "DEPTH_" + std::to_string(index + 1);
        configuration.preprocessor_definitions.emplace(name, next);
    }
    configuration.preprocessor_definitions.emplace("DEPTH_32", "\"ok.hlsli\"");

    const std::vector<std::string> expressions{
        "FUNCTION_HEADER()",  "UNDEFINED_HEADER", "CYCLE_A",  "MALFORMED_HEADER",
        "MULTI_TOKEN_HEADER", "TOO_DEEP",         "TOO_LARGE"};
    for (std::size_t index = 0; index < expressions.size(); ++index) {
        const auto root_path = tree.path("dynamic-" + std::to_string(index) + ".hlsl");
        const auto text = "#include " + expressions[index] + "\n";
        const auto root = snapshot(root_path, text);
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& file = root_file(resolution, root_path);
        REQUIRE(file.includes.size() == 1);
        CHECK(file.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(workspace::resolve_include_at(root, open_documents, configuration,
                                                  text.find(expressions[index]) + 1));
    }

    const auto source_root_path = tree.path("source-defined.hlsl");
    const std::string source_text = "#define SOURCE_HEADER \"ok.hlsli\"\n"
                                    "#include SOURCE_HEADER\n";
    const auto source_root = snapshot(source_root_path, source_text);
    const std::vector source_documents{source_root};
    const auto source_resolution =
        workspace::resolve_includes(source_root, source_documents, configuration);
    CHECK(root_file(source_resolution, source_root_path).includes.front().status ==
          workspace::IncludeResolution::Status::dynamic);
}

TEST_CASE("Comment-bearing configured macro includes preserve exact-token validation",
          "[workspace][includes][macros][comments]") {
    TestTree tree;
    tree.file("ok.hlsli", "float okValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"ok.hlsli\"");
    configuration.preprocessor_definitions.emplace("OTHER_HEADER", "\"ok.hlsli\"");

    const auto root_path = tree.path("comments.hlsl");
    const std::string text = "#include HEADER // line comment\n"
                             "#include HEADER /* trailing block comment */\n"
                             "#include /* leading block comment */ HEADER\n"
                             "#include /* leading */ HEADER /* trailing */\n"
                             "#include OTHER_HEADER\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);

    REQUIRE(file.includes.size() == 5);
    CHECK(std::ranges::all_of(file.includes, [](const auto& include) {
        return include.status == workspace::IncludeResolution::Status::resolved &&
               include.macro_expanded && include.expanded_path == "ok.hlsli";
    }));
    CHECK_FALSE(resolution.has_dynamic_includes);
}

TEST_CASE("Logical preprocessing lines preserve include offsets through comments and splices",
          "[workspace][includes][macros][comments][splices][navigation]") {
    TestTree tree;
    tree.file("Configured/target.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/target.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const auto root_path = tree.path("logical-lines.hlsl");
    const std::string text = "/* \xCF\x80 leading\n"
                             "   block comment */ #inc\\\r\n"
                             "lude /* before */ HEA\\\n"
                             "DER /* after */\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);

    REQUIRE(file.includes.size() == 1);
    const auto& include = file.includes.front();
    CHECK(include.status == workspace::IncludeResolution::Status::resolved);
    CHECK(include.requested_path == "HEADER");
    CHECK(include.path_offset == text.find("HEA"));
    CHECK(include.expanded_path == "/Configured/target.hlsli");
    CHECK(
        workspace::resolve_include_at(root, open_documents, configuration, text.find("DER") + 1) ==
        std::filesystem::absolute(tree.path("Configured/target.hlsli")).lexically_normal());
    CHECK(workspace::is_include_directive_at(text, text.find("DER") + 1));

    const auto& rewritten = source_file(resolution, root_path).text;
    CHECK(rewritten.find("/* before */ \"") != std::string::npos);
    CHECK(rewritten.find("/* after */") != std::string::npos);
    CHECK(rewritten.find("HEA\\") == std::string::npos);
}

TEST_CASE("Literal includes remain navigable after multiline comments and keyword splices",
          "[workspace][includes][comments][splices][navigation]") {
    TestTree tree;
    tree.file("target.hlsli", "float targetValue;\n");
    const auto root_path = tree.path("literal-logical-line.hlsl");
    const std::string text = "/* leading block\n"
                             "comment */ #inc\\\n"
                             "lude \"target.hlsli\"\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};

    const auto resolution =
        workspace::resolve_includes(root, open_documents, workspace::WorkspaceConfiguration{});
    const auto& file = root_file(resolution, root_path);
    REQUIRE(file.includes.size() == 1);
    CHECK(file.includes.front().status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes.front().path_offset == text.find("target.hlsli"));
    CHECK(workspace::resolve_include_at(root, open_documents, workspace::WorkspaceConfiguration{},
                                        text.find("target.hlsli") + 3) ==
          std::filesystem::absolute(tree.path("target.hlsli")).lexically_normal());
}

TEST_CASE("Comments and spliced macro directives keep source definitions authoritative",
          "[workspace][includes][macros][comments][splices][ordering][regression]") {
    TestTree tree;
    tree.file("Configured/configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const std::vector<std::string> mutations{
        "// preceding line comment\n#define HEADER \"source.hlsli\"\n",
        "/* block comment starts\nand ends here */ #define HEADER \"source.hlsli\"\n",
        "/* prefix */ #def\\\nine HEADER \"source.hlsli\"\n",
        "/* prefix */ #un\\\ndef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("source-authority-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, mutations[index] + "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& file = root_file(resolution, root_path);

        REQUIRE(file.includes.size() == 1);
        CHECK(file.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(file.includes.front().expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(has_source(resolution,
                               std::filesystem::absolute(tree.path("Configured/configured.hlsli"))
                                   .lexically_normal()
                                   .generic_string()));
    }
}

TEST_CASE("Phase-two splicing forms comment delimiters before source macro scanning",
          "[workspace][includes][macros][comments][splices][ordering][regression]") {
    TestTree tree;
    tree.file("Configured/configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const std::vector<std::string> mutations{
        "/\\\n* formed block comment *\\\n/ #define HEADER \"source.hlsli\"\n",
        "/* block comment with \\\n"
        "an internal splice *\\\r\n"
        "/ #undef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("phase-two-authority-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, mutations[index] + "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& file = root_file(resolution, root_path);

        REQUIRE(file.includes.size() == 1);
        CHECK(file.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(file.includes.front().expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(has_source(resolution,
                               std::filesystem::absolute(tree.path("Configured/configured.hlsli"))
                                   .lexically_normal()
                                   .generic_string()));
    }
}

TEST_CASE("Line comments extended by splices hide macro-looking directives",
          "[workspace][includes][macros][comments][splices]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::string> comments{
        "// hidden #def\\\nine HEADER \"source.hlsli\"\n",
        "// hidden #un\\\ndef HEADER\n",
        "/\\\n/ formed line comment #define HEADER \"source.hlsli\"\n",
        "/\\\r\n/ formed and continued line comment \\\n"
        "#undef HEADER\n",
    };
    for (std::size_t index = 0; index < comments.size(); ++index) {
        const auto root_path = tree.path("commented-macro-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, comments[index] + "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();

        CHECK(include.status == workspace::IncludeResolution::Status::resolved);
        CHECK(include.expanded_path == "configured.hlsli");
        CHECK_FALSE(resolution.has_dynamic_includes);
    }
}

TEST_CASE("An initial UTF-8 BOM preserves root source macro authority",
          "[workspace][includes][macros][bom][ordering][regression]") {
    TestTree tree;
    tree.file("Configured/configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const std::vector<std::string> mutations{
        "#define HEADER \"source.hlsli\"\n",
        "#undef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("bom-root-" + std::to_string(index) + ".hlsl");
        const std::string text = "\xEF\xBB\xBF" + mutations[index] + "#include HEADER\n";
        const auto root = snapshot(root_path, text);
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& file = root_file(resolution, root_path);

        REQUIRE(file.includes.size() == 1);
        CHECK(file.includes.front().path_offset == text.rfind("HEADER"));
        CHECK(file.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(file.includes.front().expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(has_source(resolution,
                               std::filesystem::absolute(tree.path("Configured/configured.hlsli"))
                                   .lexically_normal()
                                   .generic_string()));
    }
}

TEST_CASE("An initial UTF-8 BOM preserves included-file source macro authority",
          "[workspace][includes][macros][bom][ordering][regression]") {
    TestTree tree;
    tree.file("Configured/configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const std::vector<std::string> mutations{
        "#define HEADER \"source.hlsli\"\n",
        "#undef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto child_name = "bom-child-" + std::to_string(index) + ".hlsli";
        const std::string child_text = "\xEF\xBB\xBF" + mutations[index] + "#include HEADER\n";
        tree.file(child_name, child_text);
        const auto root_path = tree.path("bom-include-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include \"" + child_name + "\"\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& child = root_file(resolution, tree.path(child_name));

        REQUIRE(child.includes.size() == 1);
        CHECK(child.includes.front().path_offset == child_text.rfind("HEADER"));
        CHECK(child.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(child.includes.front().expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(has_source(resolution,
                               std::filesystem::absolute(tree.path("Configured/configured.hlsli"))
                                   .lexically_normal()
                                   .generic_string()));
    }
}

TEST_CASE("UTF-8 BOM bytes away from source offset zero are not directive whitespace",
          "[workspace][includes][macros][bom][regression]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::string> prefixes{
        " \xEF\xBB\xBF#define HEADER \"source.hlsli\"\n",
        "\n\xEF\xBB\xBF#undef HEADER\n",
    };
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
        const auto root_path = tree.path("embedded-bom-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, prefixes[index] + "#include HEADER\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();

        CHECK(include.status == workspace::IncludeResolution::Status::resolved);
        CHECK(include.expanded_path == "configured.hlsli");
        CHECK_FALSE(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Directive keywords remain valid configured include macro identifiers",
          "[workspace][includes][macros][regression]") {
    TestTree tree;
    tree.file("define.hlsli", "float defineValue;\n");
    tree.file("undef.hlsli", "float undefValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("define", "\"define.hlsli\"");
    configuration.preprocessor_definitions.emplace("undef", "\"undef.hlsli\"");

    const auto root_path = tree.path("directive-identifiers.hlsl");
    const std::string text = "#include define\n#include undef\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);

    REQUIRE(file.includes.size() == 2);
    CHECK(file.includes[0].status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes[0].expanded_path == "define.hlsli");
    CHECK(file.includes[0].path_offset == text.find("define"));
    CHECK(file.includes[1].status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes[1].expanded_path == "undef.hlsli");
    CHECK(file.includes[1].path_offset == text.find("undef"));
    CHECK_FALSE(resolution.has_dynamic_includes);
}

TEST_CASE("Pathological logical lines conservatively disable configured include expansion",
          "[workspace][includes][macros][safety][limits]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("pathological.hlsl");
    const auto text =
        std::string(256U * 1024U + 1U, ' ') + "#define HEADER \"source.hlsli\"\n#include HEADER\n";
    const auto root = snapshot(root_path, text);
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);
    REQUIRE(file.includes.size() == 1);
    CHECK(file.includes.front().status == workspace::IncludeResolution::Status::dynamic);
    CHECK(resolution.has_dynamic_includes);
}

TEST_CASE("Configured macro includes reject extra tokens and unterminated comments",
          "[workspace][includes][macros][comments][safety]") {
    TestTree tree;
    tree.file("ok.hlsli", "float okValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"ok.hlsli\"");

    const std::vector<std::string> expressions{
        "HEADER /* comment */ trailing",
        "/* comment */ HEADER trailing",
        "HEADER /* unterminated",
    };
    for (std::size_t index = 0; index < expressions.size(); ++index) {
        const auto root_path = tree.path("invalid-comment-" + std::to_string(index) + ".hlsl");
        const auto text = "#include " + expressions[index] + "\n";
        const auto root = snapshot(root_path, text);
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& include = root_file(resolution, root_path).includes.front();
        CHECK(include.status == workspace::IncludeResolution::Status::dynamic);
        CHECK(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Parent source macro mutations precede nested include resolution",
          "[workspace][includes][macros][ordering]") {
    TestTree tree;
    tree.file("child.hlsli", "#include HEADER\n");
    tree.file("Configured/target.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/target.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const std::vector<std::string> mutations{
        "#define HEADER \"source.hlsli\"\n",
        "#undef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("before-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, mutations[index] + "#include \"child.hlsli\"\n");
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& child = root_file(resolution, tree.path("child.hlsli"));

        REQUIRE(child.includes.size() == 1);
        CHECK(child.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(child.includes.front().expanded_path.empty());
        CHECK(resolution.has_dynamic_includes);
        CHECK_FALSE(
            has_source(resolution, std::filesystem::absolute(tree.path("Configured/target.hlsli"))
                                       .lexically_normal()
                                       .generic_string()));
    }
}

TEST_CASE("Parent source macro mutations after an include do not affect its child",
          "[workspace][includes][macros][ordering]") {
    TestTree tree;
    tree.file("child.hlsli", "#include HEADER\n");
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::string> mutations{
        "#define HEADER \"source.hlsli\"\n",
        "#undef HEADER\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("after-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include \"child.hlsli\"\n" + mutations[index]);
        const std::vector open_documents{root};
        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& child = root_file(resolution, tree.path("child.hlsli"));

        REQUIRE(child.includes.size() == 1);
        CHECK(child.includes.front().status == workspace::IncludeResolution::Status::resolved);
        CHECK(child.includes.front().expanded_path == "configured.hlsli");
        CHECK_FALSE(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Repeated configured macro includes preserve source macro context for DXC",
          "[workspace][includes][macros][ordering][regression]") {
    TestTree tree;
    const std::string child_text = "#include HEADER\n";
    tree.file("child.hlsli", child_text);
    tree.file("Configured/configured.hlsli", "float configuredValue;\n");
    tree.file("source.hlsli", "float sourceValue;\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"child.hlsli\"\n"
                                          "#undef HEADER\n"
                                          "#define HEADER \"source.hlsli\"\n"
                                          "#include \"child.hlsli\"\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured", tree.path("Configured"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto child_path =
        std::filesystem::absolute(tree.path("child.hlsli")).lexically_normal().generic_string();
    CHECK(std::ranges::count(resolution.sources, child_path,
                             &hlsl_intellisense::dxc::SourceFile::path) == 1);
    CHECK(source_file(resolution, tree.path("child.hlsli")).text == child_text);
    const auto& child = root_file(resolution, tree.path("child.hlsli"));
    REQUIRE(child.includes.size() == 1);
    CHECK(child.includes.front().status == workspace::IncludeResolution::Status::dynamic);
    CHECK(child.includes.front().expanded_path.empty());
    CHECK(resolution.has_dynamic_includes);
    CHECK(has_source(resolution, std::filesystem::absolute(tree.path("Configured/configured.hlsli"))
                                     .lexically_normal()
                                     .generic_string()));
}

TEST_CASE("Configured macro fallback covers logical and physical aliases",
          "[workspace][includes][macros][virtual][ordering][regression]") {
    TestTree tree;
    const std::string child_text = "#include HEADER\n";
    tree.file("Mapped/child.hlsli", child_text);
    tree.file("Mapped/configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"/Mapped/child.hlsli\"\n"
                                          "#undef HEADER\n"
                                          "#include \"Mapped/child.hlsli\"\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"/Mapped/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Mapped", tree.path("Mapped"));

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto physical_child = std::filesystem::absolute(tree.path("Mapped/child.hlsli"))
                                    .lexically_normal()
                                    .generic_string();
    CHECK(std::ranges::count(resolution.sources, physical_child,
                             &hlsl_intellisense::dxc::SourceFile::path) == 1);
    CHECK(std::ranges::count(resolution.sources, std::string{"/Mapped/child.hlsli"},
                             &hlsl_intellisense::dxc::SourceFile::path) == 1);
    CHECK(source_file(resolution, tree.path("Mapped/child.hlsli")).text == child_text);
    const auto logical_source =
        std::ranges::find(resolution.sources, std::string{"/Mapped/child.hlsli"},
                          &hlsl_intellisense::dxc::SourceFile::path);
    REQUIRE(logical_source != resolution.sources.end());
    CHECK(logical_source->text == child_text);
    CHECK(resolution.has_dynamic_includes);
}

TEST_CASE("Preprocessing comments are whitespace in source macro directives",
          "[workspace][includes][macros][comments][ordering]") {
    TestTree tree;
    tree.file("first.hlsli", "#include HEADER\n");
    tree.file("second.hlsli", "#include HEADER\n");
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::string> mutations{
        "# /* before keyword */ undef /* before name */ HEADER\n",
        "#define/**/ /* before name */ HEADER \"source.hlsli\"\n",
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto root_path = tree.path("comments-ordering-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, "#include \"first.hlsli\"\n" + mutations[index] +
                                                  "#include \"second.hlsli\"\n");
        const std::vector open_documents{root};

        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        const auto& first = root_file(resolution, tree.path("first.hlsli"));
        const auto& second = root_file(resolution, tree.path("second.hlsli"));
        REQUIRE(first.includes.size() == 1);
        REQUIRE(second.includes.size() == 1);
        CHECK(first.includes.front().status == workspace::IncludeResolution::Status::resolved);
        CHECK(second.includes.front().status == workspace::IncludeResolution::Status::dynamic);
        CHECK(resolution.has_dynamic_includes);
    }
}

TEST_CASE("Malformed source macro comments make configured expansion unknown",
          "[workspace][includes][macros][comments][safety]") {
    TestTree tree;
    tree.file("child.hlsli", "#include HEADER\n");
    tree.file("configured.hlsli", "float configuredValue;\n");
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const std::vector<std::string> directives{
        "#define /* unterminated\n",
        "#undef /* unterminated\n",
        "#define / * malformed */ HEADER \"source.hlsli\"\n",
        "#undef / * malformed */ HEADER\n",
    };
    for (std::size_t index = 0; index < directives.size(); ++index) {
        const auto root_path = tree.path("malformed-" + std::to_string(index) + ".hlsl");
        const auto root = snapshot(root_path, directives[index] + "#include \"child.hlsli\"\n");
        const std::vector open_documents{root};

        const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
        CHECK(resolution.has_dynamic_includes);
        if (index < 2) {
            CHECK(resolution.files.size() == 1);
            continue;
        }
        const auto& child = root_file(resolution, tree.path("child.hlsli"));
        REQUIRE(child.includes.size() == 1);
        CHECK(child.includes.front().status == workspace::IncludeResolution::Status::dynamic);
    }
}

TEST_CASE("Conditional source macro mutations conservatively block configured expansion",
          "[workspace][includes][macros][ordering][safety]") {
    TestTree tree;
    tree.file("child.hlsli", "#include HEADER\n");
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("conditional.hlsl");
    const auto root = snapshot(root_path, "#if MAYBE_DEFINED\n"
                                          "#undef HEADER\n"
                                          "#endif\n"
                                          "#include \"child.hlsli\"\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "\"configured.hlsli\"");

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& child = root_file(resolution, tree.path("child.hlsli"));
    REQUIRE(child.includes.size() == 1);
    CHECK(child.includes.front().status == workspace::IncludeResolution::Status::dynamic);
    CHECK(resolution.has_dynamic_includes);
}

TEST_CASE("Source macros from included files prevent configured include guessing",
          "[workspace][includes][macros][safety]") {
    TestTree tree;
    tree.file("defines.hlsli", "#define SHARED_HEADER \"source.hlsli\"\n");
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include \"defines.hlsli\"\n"
                                          "#include SHARED_HEADER\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("SHARED_HEADER", "\"configured.hlsli\"");

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);
    REQUIRE(file.includes.size() == 2);
    CHECK(file.includes[0].status == workspace::IncludeResolution::Status::resolved);
    CHECK(file.includes[1].status == workspace::IncludeResolution::Status::dynamic);
    CHECK(resolution.has_dynamic_includes);
}

TEST_CASE("An earlier dynamic include prevents later configured include guessing",
          "[workspace][includes][macros][safety]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include UNKNOWN_HEADER\n"
                                          "#include CONFIGURED_HEADER\n");
    const std::vector open_documents{root};
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("CONFIGURED_HEADER", "\"configured.hlsli\"");

    const auto resolution = workspace::resolve_includes(root, open_documents, configuration);
    const auto& file = root_file(resolution, root_path);
    REQUIRE(file.includes.size() == 2);
    CHECK(std::ranges::all_of(file.includes, [](const auto& include) {
        return include.status == workspace::IncludeResolution::Status::dynamic;
    }));
}

TEST_CASE("Include metadata cache remains independent from configured macro expansion",
          "[workspace][includes][macros][cache]") {
    TestTree tree;
    tree.file("configured.hlsli", "float configuredValue;\n");
    const auto root_path = tree.path("root.hlsl");
    const auto root = snapshot(root_path, "#include CONFIGURED_HEADER\n");
    const std::vector open_documents{root};
    workspace::IncludeMetadataCache cache;

    const auto dynamic = workspace::resolve_includes(root, open_documents,
                                                     workspace::WorkspaceConfiguration{}, &cache);
    CHECK(root_file(dynamic, root_path).includes.front().status ==
          workspace::IncludeResolution::Status::dynamic);

    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("CONFIGURED_HEADER", "\"configured.hlsli\"");
    const auto resolved = workspace::resolve_includes(root, open_documents, configuration, &cache);
    CHECK(root_file(resolved, root_path).includes.front().status ==
          workspace::IncludeResolution::Status::resolved);
    CHECK(cache.metrics().hits == 1);
    CHECK(cache.metrics().misses == 2);
}

TEST_CASE("Include metadata cache has deterministic LRU count and memory bounds",
          "[workspace][includes][cache]") {
    hlsl_intellisense::workspace::IncludeMetadataCache cache{
        {.max_entries = 2, .max_estimated_bytes = 4096}};
    const std::string first_text = "#include \"first.hlsli\"\n";
    const std::string second_text = "#include \"second.hlsli\"\n";
    const std::string third_text = "#include \"third.hlsli\"\n";

    CHECK(cache.get("first", first_text).directives.front().path == "first.hlsli");
    CHECK(cache.get("second", second_text).directives.front().path == "second.hlsli");
    static_cast<void>(cache.get("first", first_text));
    static_cast<void>(cache.get("third", third_text));
    auto metrics = cache.metrics();
    CHECK(metrics.entries == 2);
    CHECK(metrics.estimated_bytes <= 4096);
    CHECK(metrics.hits == 1);
    CHECK(metrics.misses == 3);
    CHECK(metrics.evictions == 1);

    static_cast<void>(cache.get("second", second_text));
    metrics = cache.metrics();
    CHECK(metrics.misses == 4);
    CHECK(metrics.evictions == 2);
    CHECK(metrics.entries == 2);

    cache.invalidate("first");
    CHECK(cache.metrics().entries <= 2);

    hlsl_intellisense::workspace::IncludeMetadataCache byte_limited{
        {.max_entries = 4, .max_estimated_bytes = 128}};
    static_cast<void>(byte_limited.get("oversized", std::string(1024, 'x')));
    CHECK(byte_limited.metrics().entries == 0);
    CHECK(byte_limited.metrics().estimated_bytes == 0);
}
