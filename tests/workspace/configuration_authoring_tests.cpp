#include <hlsl_intellisense/workspace/configuration_authoring.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace workspace = hlsl_intellisense::workspace;

namespace {

class TestTree final {
  public:
    TestTree() {
        static std::size_t next_id{};
        root_ = std::filesystem::current_path() /
                ("configuration-authoring-tests-" + std::to_string(next_id++));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    ~TestTree() { std::filesystem::remove_all(root_); }

    [[nodiscard]] std::filesystem::path path(std::string_view relative = {}) const {
        return root_ / std::filesystem::path{relative};
    }

    void file(std::string_view relative, std::string_view content = {}) const {
        const auto destination = path(relative);
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream{destination};
        REQUIRE(stream);
        stream << content;
        REQUIRE(stream);
    }

  private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE("Configuration authoring discovery is bounded and excludes nested and output trees",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    tree.file("Shaders/main.hlsl");
    tree.file("Shaders/include.hlsli");
    tree.file("build/generated.hlsl");
    tree.file("vendor/library.hlsl");
    tree.file("Nested/shadertoolsconfig.json", "{}");
    tree.file("Nested/owned.hlsl");

    const auto discovery = workspace::discover_shader_files(tree.path());
    CHECK(discovery.shader_files ==
          std::vector{tree.path("Shaders/include.hlsli"), tree.path("Shaders/main.hlsl")});
    CHECK(discovery.nested_configurations ==
          std::vector{tree.path("Nested/shadertoolsconfig.json")});
    CHECK_FALSE(discovery.truncated);

    const auto bounded = workspace::discover_shader_files(
        tree.path(), workspace::ConfigurationDiscoveryLimits{.max_directories = 1});
    CHECK(bounded.truncated);
    CHECK(bounded.truncation_reason == "directoryLimit");
}

TEST_CASE("Configuration authoring discovery observes cancellation",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    tree.file("Shaders/main.hlsl");
    CHECK_THROWS_AS(workspace::discover_shader_files(tree.path(), {},
                                                     [] { throw std::runtime_error{"cancelled"}; }),
                    std::runtime_error);
}

TEST_CASE("Configuration previews are deterministic and preserve unmanaged configuration",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    tree.file("Includes/keep.hlsli");
    tree.file("Shaders/main.hlsl");
    const std::string existing = R"({
      "custom.toolSetting": {"keep": true},
      "hlsl.virtualDirectoryMappings": {"/Keep": "Includes"},
      "hlsl.variantsVersion": 1,
      "hlsl.variants": [{"name": "Keep", "hlsl.entryPoint": "Old"}],
      "hlsl.pipelinesVersion": 1,
      "hlsl.pipelines": [{
        "name": "Keep Pipeline",
        "stages": {
          "vertex": {"file": "Shaders/main.hlsl"},
          "pixel": {"file": "Shaders/main.hlsl"}
        }
      }]
    })";
    const std::vector selections{workspace::ConfigurationSelection{
        .file = tree.path("Shaders/main.hlsl"), .entry_point = "Main", .target_profile = "ps_6_6"}};

    const auto first = workspace::generate_configuration_preview(tree.path(), existing, selections);
    const auto second =
        workspace::generate_configuration_preview(tree.path(), existing, selections);
    REQUIRE(first.valid);
    CHECK(first.changed);
    CHECK(first.content == second.content);
    CHECK(workspace::configuration_content_hash(first.content) ==
          workspace::configuration_content_hash(second.content));

    const auto json = nlohmann::json::parse(first.content);
    CHECK(json["custom.toolSetting"]["keep"] == true);
    CHECK(json["hlsl.virtualDirectoryMappings"]["/Keep"] == "Includes");
    CHECK(json["hlsl.variants"][0]["name"] == "Keep");
    CHECK(json["hlsl.pipelines"][0]["name"] == "Keep Pipeline");
    REQUIRE(json["hlsl.fileGroups"].size() == 1);
    CHECK(json["hlsl.fileGroups"][0]["files"] == nlohmann::json::array({"Shaders/main.hlsl"}));
    CHECK(json["hlsl.fileGroups"][0]["hlsl.entryPoint"] == "Main");
    CHECK(json["hlsl.fileGroups"][0]["hlsl.targetProfile"] == "ps_6_6");
}

TEST_CASE("Configuration previews do not overwrite malformed existing content",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    const std::string malformed = R"({"hlsl.fileGroups": [)";
    const auto preview = workspace::generate_configuration_preview(tree.path(), malformed, {});
    CHECK_FALSE(preview.valid);
    CHECK_FALSE(preview.changed);
    CHECK(preview.content == malformed);
    REQUIRE(preview.errors.size() == 1);
    CHECK(preview.errors[0].code == "invalid-json");
    CHECK(preview.errors[0].field == "$");
}

TEST_CASE("Configuration preview validation reports production parser fields",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    const std::string invalid = R"({"hlsl.variants":[{"name":"Missing version"}]})";
    const auto preview = workspace::generate_configuration_preview(tree.path(), invalid, {});
    CHECK_FALSE(preview.valid);
    CHECK(preview.content == invalid);
    REQUIRE(preview.errors.size() == 1);
    CHECK(preview.errors[0].field == "hlsl.variantsVersion");
}

TEST_CASE("Edited drafts validate all supported settings and retain the original edit guard",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    tree.file("Includes/shared.hlsli");
    tree.file("Shaders/main.hlsl");
    const std::string original = R"({"custom.setting":{"keep":true}})";
    const std::string draft = R"({
      "custom.setting":{"keep":true},
      "root":true,
      "hlsl.languageVersion":"2021",
      "hlsl.additionalIncludeDirectories":["Includes"],
      "hlsl.virtualDirectoryMappings":{"/Shared":"Includes"},
      "hlsl.preprocessorDefinitions":{"LIGHT_COUNT":3},
      "hlsl.additionalArguments":["-Zi"],
      "hlsl.dxcRuntimeDirectory":"DXC",
      "hlsl.variantsVersion":1,
      "hlsl.variants":[{"name":"Debug","hlsl.entryPoint":"Main"}],
      "hlsl.pipelinesVersion":1,
      "hlsl.pipelines":[{"name":"Default","stages":{
        "vertex":{"file":"Shaders/main.hlsl"},
        "pixel":{"file":"Shaders/main.hlsl"}
      }}]
    })";
    const auto preview =
        workspace::generate_configuration_preview(tree.path(), original, {}, draft);
    CHECK(preview.valid);
    CHECK(preview.changed);
    CHECK(preview.content == draft);
    CHECK(nlohmann::json::parse(preview.content)["custom.setting"]["keep"] == true);

    const auto malformed = workspace::generate_configuration_preview(
        tree.path(), original, {}, R"({"hlsl.variants":[{"name":"Debug"}]})");
    REQUIRE_FALSE(malformed.valid);
    REQUIRE(malformed.errors.size() == 1);
    CHECK(malformed.errors[0].field == "hlsl.variantsVersion");

    const auto repaired =
        workspace::generate_configuration_preview(tree.path(), R"({"hlsl.variants":[)", {}, draft);
    CHECK(repaired.valid);
    CHECK(repaired.content == draft);
}

TEST_CASE("Empty configuration previews are schema-shaped and hash with SHA-256",
          "[workspace][configuration-authoring]") {
    TestTree tree;
    const auto preview = workspace::generate_configuration_preview(tree.path(), std::nullopt, {});
    REQUIRE(preview.valid);
    const auto json = nlohmann::json::parse(preview.content);
    CHECK(json["root"] == true);
    CHECK(json["hlsl.languageVersion"] == "2021");
    CHECK(json.contains("$schema"));
    const auto hash = workspace::configuration_content_hash(preview.content);
    CHECK(hash.starts_with("sha256:"));
    CHECK(hash.size() == 71);
    CHECK(workspace::configuration_content_hash("") ==
          "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
