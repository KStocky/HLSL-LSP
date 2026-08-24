#include <hlsl_intellisense/workspace/configuration.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace workspace = hlsl_intellisense::workspace;

namespace {

struct ConfigurationFailure {
    workspace::ConfigurationErrorCode code;
    std::filesystem::path file;
    std::string key;
    std::string message;
};

class TestTree final {
  public:
    TestTree() {
        static std::size_t next_id{};
        root_ = std::filesystem::current_path() /
                ("workspace-configuration-tests-" + std::to_string(next_id++));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    TestTree(const TestTree&) = delete;
    auto operator=(const TestTree&) -> TestTree& = delete;
    TestTree(TestTree&&) = delete;
    auto operator=(TestTree&&) -> TestTree& = delete;

    ~TestTree() { std::filesystem::remove_all(root_); }

    [[nodiscard]] auto path(std::string_view relative) const -> std::filesystem::path {
        return root_ / std::filesystem::path{relative};
    }

    void directory(std::string_view relative) const {
        std::filesystem::create_directories(path(relative));
    }

    // Both strings deliberately represent different domains.
    void file(std::string_view relative, std::string_view contents) const { // NOLINT
        const auto destination = path(relative);
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream{destination};
        REQUIRE(stream);
        stream << contents;
        REQUIRE(stream);
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] auto configuration_failure(const std::filesystem::path& shader_file)
    -> ConfigurationFailure {
    try {
        static_cast<void>(workspace::load_workspace_configuration_for_file(shader_file));
    } catch (const workspace::ConfigurationError& error) {
        return {.code = error.code(),
                .file = error.file(),
                .key = error.key(),
                .message = error.what()};
    }
    FAIL("Expected a ConfigurationError");
    return {.code = workspace::ConfigurationErrorCode::invalid_json,
            .file = {},
            .key = {},
            .message = {}};
}

[[nodiscard]] auto configuration_error(const std::filesystem::path& shader_directory)
    -> workspace::ConfigurationErrorCode {
    try {
        static_cast<void>(workspace::load_workspace_configuration(shader_directory));
    } catch (const workspace::ConfigurationError& error) {
        return error.code();
    }
    FAIL("Expected a ConfigurationError");
    return workspace::ConfigurationErrorCode::invalid_json;
}

} // namespace

TEST_CASE("Configuration discovery merges toward the shader and stops at root",
          "[workspace][configuration]") {
    TestTree tree;
    tree.directory("project/shaders/material");
    tree.directory("project/shared");
    tree.directory("project/shaders/local");
    tree.directory("project/engine");
    tree.directory("project/game");

    tree.file("shadertoolsconfig.json", R"({"hlsl.preprocessorDefinitions":{"IGNORED":1}})");
    tree.file("project/shadertoolsconfig.json", R"(
        {
          // Comments are accepted for compatibility with HLSL Tools.
          "root": true,
          "hlsl.preprocessorDefinitions": {
            "BASE": 1,
            "OVERRIDE": "far",
            "FEATURE": false
          },
          "hlsl.additionalIncludeDirectories": ["shared"],
          "hlsl.virtualDirectoryMappings": {
            "/Engine": "engine",
            "/Game": "game"
          },
          "hlsl.languageVersion": "2018",
          "hlsl.targetProfile": "ps_6_0",
          "hlsl.entryPoint": "FarMain",
          "hlsl.additionalArguments": ["-Zi"]
        })");
    tree.file("project/shaders/shadertoolsconfig.json", R"(
        {
          "hlsl.preprocessorDefinitions": {
            "OVERRIDE": 2,
            "FEATURE": true,
            "NO_VALUE": ""
          },
          "hlsl.additionalIncludeDirectories": ["local", "../shared"],
          "hlsl.virtualDirectoryMappings": {
            "/Game": "../game"
          },
          "hlsl.targetProfile": "cs_6_7",
          "hlsl.entryPoint": "Main",
          "hlsl.additionalArguments": ["-enable-16bit-types", "-O3"]
        })");

    const auto shader_directory = tree.path("project/shaders/material");
    const auto discovered = workspace::discover_configuration_files(shader_directory);
    REQUIRE(discovered.size() == 2);
    CHECK(discovered[0] == tree.path("project/shaders/shadertoolsconfig.json"));
    CHECK(discovered[1] == tree.path("project/shadertoolsconfig.json"));

    const auto configuration = workspace::load_workspace_configuration(shader_directory);
    CHECK(configuration.preprocessor_definitions.at("BASE") == "1");
    CHECK(configuration.preprocessor_definitions.at("OVERRIDE") == "2");
    CHECK(configuration.preprocessor_definitions.at("FEATURE") == "true");
    CHECK(configuration.preprocessor_definitions.at("NO_VALUE").empty());
    CHECK(!configuration.preprocessor_definitions.contains("IGNORED"));
    REQUIRE(configuration.additional_include_directories.size() == 2);
    CHECK(configuration.additional_include_directories[0] ==
          std::filesystem::weakly_canonical(tree.path("project/shaders/local")));
    CHECK(configuration.additional_include_directories[1] ==
          std::filesystem::weakly_canonical(tree.path("project/shared")));
    CHECK(configuration.virtual_directory_mappings.at("/Engine") ==
          std::filesystem::weakly_canonical(tree.path("project/engine")));
    CHECK(configuration.virtual_directory_mappings.at("/Game") ==
          std::filesystem::weakly_canonical(tree.path("project/game")));
    CHECK(configuration.language_version == "2018");
    CHECK(configuration.target_profile == "cs_6_7");
    CHECK(configuration.entry_point == "Main");
    CHECK(configuration.additional_arguments ==
          std::vector<std::string>{"-enable-16bit-types", "-O3"});
}

TEST_CASE("Configuration converts typed flags to deterministic DXC options",
          "[workspace][configuration]") {
    TestTree tree;
    tree.directory("shaders/includes");
    tree.file("shaders/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.preprocessorDefinitions": {
            "ZED": "value",
            "BOOL": false,
            "BARE": ""
          },
          "hlsl.additionalIncludeDirectories": ["includes"],
          "hlsl.languageVersion": "202x",
          "hlsl.targetProfile": "lib_6_8",
          "hlsl.entryPoint": "RayGeneration",
          "hlsl.additionalArguments": ["-spirv", "-fspv-target-env=vulkan1.3"]
        })");

    const auto options =
        workspace::load_workspace_configuration(tree.path("shaders")).compiler_options();
    CHECK(options.language_version == "202x");
    CHECK(options.target_profile == "lib_6_8");
    CHECK(options.entry_point == "RayGeneration");
    CHECK(options.defines == std::vector<std::string>{"BARE", "BOOL=false", "ZED=value"});
    CHECK(options.include_directories ==
          std::vector<std::string>{
              std::filesystem::weakly_canonical(tree.path("shaders/includes")).string()});
    CHECK(options.additional_arguments ==
          std::vector<std::string>{"-spirv", "-fspv-target-env=vulkan1.3"});
}

TEST_CASE("Relative Unreal virtual mappings resolve against their defining config",
          "[workspace][configuration]") {
    TestTree tree;
    tree.directory("root/Engine/Shaders");
    tree.directory("root/Project/Shaders");
    tree.directory("root/shaders");
    tree.file("root/shadertoolsconfig.json", R"(
        {
          "hlsl.virtualDirectoryMappings": {
            "\\Engine": "Engine/Shaders",
            "/Project": "Project/Shaders"
          }
        })");

    const auto configuration = workspace::load_workspace_configuration(tree.path("root/shaders"));
    CHECK(configuration.virtual_directory_mappings.at("\\Engine") ==
          std::filesystem::weakly_canonical(tree.path("root/Engine/Shaders")));
    CHECK(configuration.virtual_directory_mappings.at("/Project") ==
          std::filesystem::weakly_canonical(tree.path("root/Project/Shaders")));
}

TEST_CASE("File groups match normalized filename and relative path globs in declaration order",
          "[workspace][configuration][file-groups]") {
    TestTree tree;
    tree.directory("project/base-includes");
    tree.directory("project/compute-includes");
    tree.directory("project/special-includes");
    tree.directory("project/base-virtual");
    tree.directory("project/compute-virtual");
    tree.directory("project/special-virtual");
    tree.directory("project/effects/nested");
    tree.directory("project/materials");
    tree.file("project/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.preprocessorDefinitions": {"BASE": 1, "ORDER": "base"},
          "hlsl.additionalIncludeDirectories": ["base-includes"],
          "hlsl.virtualDirectoryMappings": {"/Base": "base-virtual"},
          "hlsl.languageVersion": "2018",
          "hlsl.targetProfile": "lib_6_0",
          "hlsl.entryPoint": "BaseMain",
          "hlsl.additionalArguments": ["-Zi"],
          "hlsl.fileGroups": [
            {
              "name": "Compute files at any depth",
              "files": ["never/*.hlsl", "compute-?.hlsl"],
              "hlsl.preprocessorDefinitions": {
                "COMPUTE": true,
                "ORDER": "filename"
              },
              "hlsl.additionalIncludeDirectories": ["compute-includes"],
              "hlsl.virtualDirectoryMappings": {
                "/Compute": "compute-virtual",
                "/Selected": "compute-virtual"
              },
              "hlsl.languageVersion": "2021",
              "hlsl.targetProfile": "cs_6_6",
              "hlsl.entryPoint": "ComputeMain",
              "hlsl.additionalArguments": ["-enable-16bit-types"]
            },
            {
              "name": "Effects tree",
              "files": ["effects/**/*.hlsl"],
              "hlsl.preprocessorDefinitions": {"ORDER": "relative", "EFFECT": 2},
              "hlsl.additionalIncludeDirectories": ["special-includes"],
              "hlsl.virtualDirectoryMappings": {
                "/Selected": "special-virtual"
              },
              "hlsl.targetProfile": "cs_6_8",
              "hlsl.entryPoint": "SpecialMain",
              "hlsl.additionalArguments": ["-O3"]
            },
            {
              "name": "Backslash separators",
              "files": ["effects\\nested\\compute-?.hlsl"],
              "hlsl.preprocessorDefinitions": {"NORMALIZED": true}
            },
            {
              "name": "Does not match",
              "files": ["materials/*.hlsl"],
              "hlsl.preprocessorDefinitions": {"UNEXPECTED": 1}
            }
          ]
        })");

    const auto configuration = workspace::load_workspace_configuration_for_file(
        tree.path("project/effects/nested/compute-a.hlsl"));
    CHECK(configuration.preprocessor_definitions.at("BASE") == "1");
    CHECK(configuration.preprocessor_definitions.at("COMPUTE") == "true");
    CHECK(configuration.preprocessor_definitions.at("EFFECT") == "2");
    CHECK(configuration.preprocessor_definitions.at("NORMALIZED") == "true");
    CHECK(configuration.preprocessor_definitions.at("ORDER") == "relative");
    CHECK(!configuration.preprocessor_definitions.contains("UNEXPECTED"));
    CHECK(configuration.additional_include_directories ==
          std::vector<std::filesystem::path>{
              std::filesystem::weakly_canonical(tree.path("project/special-includes")),
              std::filesystem::weakly_canonical(tree.path("project/compute-includes")),
              std::filesystem::weakly_canonical(tree.path("project/base-includes"))});
    CHECK(configuration.virtual_directory_mappings.at("/Base") ==
          std::filesystem::weakly_canonical(tree.path("project/base-virtual")));
    CHECK(configuration.virtual_directory_mappings.at("/Compute") ==
          std::filesystem::weakly_canonical(tree.path("project/compute-virtual")));
    CHECK(configuration.virtual_directory_mappings.at("/Selected") ==
          std::filesystem::weakly_canonical(tree.path("project/special-virtual")));
    CHECK(configuration.language_version == "2021");
    CHECK(configuration.target_profile == "cs_6_8");
    CHECK(configuration.entry_point == "SpecialMain");
    CHECK(configuration.additional_arguments == std::vector<std::string>{"-O3"});

    const auto zero_directory_glob =
        workspace::load_workspace_configuration_for_file(tree.path("project/effects/plain.hlsl"));
    CHECK(zero_directory_glob.preprocessor_definitions.at("EFFECT") == "2");
    CHECK(!zero_directory_glob.preprocessor_definitions.contains("COMPUTE"));

    const auto no_match = workspace::load_workspace_configuration_for_file(
        tree.path("project/effects/nested/other.hlsl"));
    CHECK(no_match.preprocessor_definitions.at("BASE") == "1");
    CHECK(no_match.preprocessor_definitions.at("EFFECT") == "2");
    CHECK(!no_match.preprocessor_definitions.contains("COMPUTE"));
    CHECK(!no_match.preprocessor_definitions.contains("NORMALIZED"));

    const auto no_groups =
        workspace::load_workspace_configuration_for_file(tree.path("project/unclassified.hlsl"));
    CHECK(no_groups.preprocessor_definitions ==
          std::map<std::string, std::string, std::less<>>{{"BASE", "1"}, {"ORDER", "base"}});
    CHECK(no_groups.target_profile == "lib_6_0");

    const auto directory_configuration =
        workspace::load_workspace_configuration(tree.path("project/effects/nested"));
    CHECK(directory_configuration.preprocessor_definitions ==
          std::map<std::string, std::string, std::less<>>{{"BASE", "1"}, {"ORDER", "base"}});
    CHECK(directory_configuration.target_profile == "lib_6_0");
}

TEST_CASE("File groups follow config and group precedence across nested directories",
          "[workspace][configuration][file-groups]") {
    TestTree tree;
    tree.directory("project/outer-normal");
    tree.directory("project/outer-group");
    tree.directory("project/shaders/inner-normal");
    tree.directory("project/shaders/inner-group");
    tree.directory("project/shaders/nested");
    tree.directory("outside");
    tree.file("project/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.preprocessorDefinitions": {
            "ORDER": "outer-normal",
            "OUTER_GROUP_AFTER_INNER_NORMAL": "outer-normal"
          },
          "hlsl.additionalIncludeDirectories": ["outer-normal"],
          "hlsl.fileGroups": [{
            "files": ["*.hlsl"],
            "hlsl.preprocessorDefinitions": {
              "ORDER": "outer-group",
              "OUTER_GROUP_AFTER_INNER_NORMAL": "outer-group"
            },
            "hlsl.additionalIncludeDirectories": ["outer-group"]
          }]
        })");
    tree.file("project/shaders/shadertoolsconfig.json", R"(
        {
          "hlsl.preprocessorDefinitions": {
            "ORDER": "inner-normal",
            "OUTER_GROUP_AFTER_INNER_NORMAL": "inner-normal"
          },
          "hlsl.additionalIncludeDirectories": ["inner-normal"],
          "hlsl.fileGroups": [{
            "files": ["nested/*.hlsl"],
            "hlsl.preprocessorDefinitions": {"ORDER": "inner-group"},
            "hlsl.additionalIncludeDirectories": ["inner-group"]
          }]
        })");

    const auto configuration = workspace::load_workspace_configuration_for_file(
        tree.path("project/shaders/nested/main.hlsl"));
    CHECK(configuration.preprocessor_definitions.at("ORDER") == "inner-group");
    CHECK(configuration.preprocessor_definitions.at("OUTER_GROUP_AFTER_INNER_NORMAL") ==
          "outer-group");
    CHECK(configuration.additional_include_directories ==
          std::vector<std::filesystem::path>{
              std::filesystem::weakly_canonical(tree.path("project/shaders/inner-group")),
              std::filesystem::weakly_canonical(tree.path("project/outer-group")),
              std::filesystem::weakly_canonical(tree.path("project/shaders/inner-normal")),
              std::filesystem::weakly_canonical(tree.path("project/outer-normal"))});

    const auto outside =
        workspace::load_workspace_configuration_for_file(tree.path("outside/main.hlsl"));
    CHECK(outside.preprocessor_definitions.empty());
}

TEST_CASE("File glob case sensitivity follows the host filesystem",
          "[workspace][configuration][file-groups]") {
    TestTree tree;
    tree.directory("project/shaders");
    tree.file("project/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.fileGroups": [{
            "files": ["Shaders/CASE?.HLSL"],
            "hlsl.preprocessorDefinitions": {"MATCHED": 1}
          }]
        })");

    const auto configuration =
        workspace::load_workspace_configuration_for_file(tree.path("project/shaders/case1.hlsl"));
#ifdef _WIN32
    CHECK(configuration.preprocessor_definitions.at("MATCHED") == "1");
#else
    CHECK(!configuration.preprocessor_definitions.contains("MATCHED"));
#endif
}

#ifdef _WIN32
TEST_CASE("Windows file globs use Unicode ordinal case-insensitive matching",
          "[workspace][configuration][file-groups]") {
    TestTree tree;
    tree.directory("project");
    tree.file("project/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.fileGroups": [{
            "files": ["\u00DCBER?.hlsl"],
            "hlsl.preprocessorDefinitions": {"MATCHED": 1}
          }]
        })");

    const auto configuration = workspace::load_workspace_configuration_for_file(
        tree.path("project") / std::filesystem::path{L"\u00FCber\U0001F600.hlsl"});
    CHECK(configuration.preprocessor_definitions.at("MATCHED") == "1");
}
#endif

TEST_CASE("Invalid file groups and globs report their configuration context",
          "[workspace][configuration][file-groups]") {
    TestTree tree;
    tree.directory("shader");

    SECTION("file groups must be an array") {
        tree.file("shader/shadertoolsconfig.json", R"({"hlsl.fileGroups":{}})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups");
    }

    SECTION("group must be an object") {
        tree.file("shader/shadertoolsconfig.json", R"({"hlsl.fileGroups":[[]]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0]");
    }

    SECTION("files are required and non-empty") {
        tree.file("shader/shadertoolsconfig.json", R"({"hlsl.fileGroups":[{"name":"missing"}]})");
        auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].files");

        tree.file("shader/shadertoolsconfig.json", R"({"hlsl.fileGroups":[{"files":[]}]})");
        failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].files");
    }

    SECTION("files must contain strings") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"name":1,"files":["*.hlsl"]}]})");
        auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].name");

        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"files":["*.hlsl",1]}]})");
        failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].files[1]");
    }

    SECTION("group properties use the documented flat shape") {
        tree.file(
            "shader/shadertoolsconfig.json",
            R"({"hlsl.fileGroups":[{"files":["*.hlsl"],"hlsl":{"targetProfile":"cs_6_6"}}]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].hlsl");
    }

    SECTION("group setting validation keeps group context") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"files":["*.hlsl"],"hlsl.additionalArguments":[1]}]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_type);
        CHECK(failure.key == "hlsl.fileGroups[0].hlsl.additionalArguments");
    }

    SECTION("malformed globstar is rejected") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"files":["shaders/**file.hlsl"]}]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_glob);
        CHECK(failure.key == "hlsl.fileGroups[0].files[0]");
        CHECK(failure.message.find("'**' must occupy") != std::string::npos);
    }

    SECTION("unsupported glob syntax is rejected") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"files":["shader[0-9].hlsl"]}]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_glob);
        CHECK(failure.key == "hlsl.fileGroups[0].files[0]");
    }

    SECTION("globs cannot leave config scope") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.fileGroups":[{"files":["../outside.hlsl"]}]})");
        const auto failure = configuration_failure(tree.path("shader/main.hlsl"));
        CHECK(failure.code == workspace::ConfigurationErrorCode::invalid_glob);
        CHECK(failure.key == "hlsl.fileGroups[0].files[0]");
    }
}

TEST_CASE("Invalid configuration is reported without fallback", "[workspace][configuration]") {
    TestTree tree;
    tree.directory("shader");

    SECTION("invalid JSON") {
        tree.file("shader/shadertoolsconfig.json", R"({"root": true,)");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::invalid_json);
    }

    SECTION("wrong property type") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.additionalIncludeDirectories": "includes"})");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::invalid_type);
    }

    SECTION("wrong definition value type") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.preprocessorDefinitions":{"BAD":[]}})");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::invalid_type);
    }

    SECTION("invalid virtual key") {
        tree.directory("real");
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.virtualDirectoryMappings":{"Engine":"../real"}})");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::invalid_virtual_directory);
    }

    SECTION("missing include directory") {
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.additionalIncludeDirectories":["missing"]})");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::missing_path);
    }

    SECTION("configured path is a file") {
        tree.file("not-a-directory", "content");
        tree.file("shader/shadertoolsconfig.json",
                  R"({"hlsl.virtualDirectoryMappings":{"/File":"../not-a-directory"}})");
        CHECK(configuration_error(tree.path("shader")) ==
              workspace::ConfigurationErrorCode::path_not_directory);
    }
}

TEST_CASE("Editor settings replace file properties and resolve from the workspace",
          "[workspace][configuration]") {
    TestTree tree;
    tree.directory("workspace/editor-includes");
    tree.directory("workspace/editor-virtual");
    tree.directory("workspace/shaders");
    tree.file("workspace/shadertoolsconfig.json", R"(
        {
          "root": true,
          "hlsl.preprocessorDefinitions": {"FILE": 1},
          "hlsl.additionalIncludeDirectories": ["shaders"],
          "hlsl.virtualDirectoryMappings": {"/File": "shaders"},
          "hlsl.languageVersion": "2018",
          "hlsl.targetProfile": "ps_6_0",
          "hlsl.entryPoint": "FileMain",
          "hlsl.additionalArguments": ["-Zi"],
          "hlsl.fileGroups": [{
            "files": ["main.hlsl"],
            "hlsl.preprocessorDefinitions": {"GROUP": 1},
            "hlsl.additionalIncludeDirectories": ["shaders"],
            "hlsl.virtualDirectoryMappings": {"/Group": "shaders"},
            "hlsl.languageVersion": "2021",
            "hlsl.targetProfile": "cs_6_6",
            "hlsl.entryPoint": "GroupMain",
            "hlsl.additionalArguments": ["-O3"]
          }]
        })");

    workspace::ConfigurationOverrides overrides;
    overrides.preprocessor_definitions =
        std::map<std::string, std::string, std::less<>>{{"EDITOR", "2"}};
    overrides.additional_include_directories =
        std::vector<std::filesystem::path>{"editor-includes"};
    overrides.virtual_directory_mappings =
        std::map<std::string, std::filesystem::path, std::less<>>{{"/Editor", "editor-virtual"}};
    overrides.language_version = std::optional<std::string>{};
    overrides.target_profile = std::optional<std::string>{"cs_6_8"};
    overrides.entry_point = std::optional<std::string>{};
    overrides.additional_arguments = std::vector<std::string>{};

    const auto configuration = workspace::apply_configuration_overrides(
        workspace::load_workspace_configuration_for_file(tree.path("workspace/shaders/main.hlsl")),
        overrides, tree.path("workspace"));

    CHECK(configuration.preprocessor_definitions ==
          std::map<std::string, std::string, std::less<>>{{"EDITOR", "2"}});
    CHECK(configuration.additional_include_directories ==
          std::vector<std::filesystem::path>{
              std::filesystem::weakly_canonical(tree.path("workspace/editor-includes"))});
    CHECK(
        configuration.virtual_directory_mappings ==
        std::map<std::string, std::filesystem::path, std::less<>>{
            {"/Editor", std::filesystem::weakly_canonical(tree.path("workspace/editor-virtual"))}});
    CHECK(!configuration.language_version.has_value());
    CHECK(configuration.target_profile == "cs_6_8");
    CHECK(!configuration.entry_point.has_value());
    CHECK(configuration.additional_arguments.empty());
}
