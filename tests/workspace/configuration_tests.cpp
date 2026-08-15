#include <hlsl_intellisense/workspace/configuration.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace workspace = hlsl_intellisense::workspace;

namespace {

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
