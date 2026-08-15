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

TEST_CASE("Virtual mappings preserve logical include names for DXC unsaved files",
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

    REQUIRE(resolution.sources.size() == 3);
    CHECK(has_source(resolution, "/Engine/Common.hlsli"));
    CHECK(has_source(resolution, "/Engine/Nested.hlsli"));
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

    REQUIRE(resolution.sources.size() == 3);
    CHECK(has_source(resolution, "/Engine/Common.hlsli"));
    CHECK(has_source(resolution, "/Other/Common.hlsli"));
    CHECK(resolution.dependency_identities.size() == 1);
}
