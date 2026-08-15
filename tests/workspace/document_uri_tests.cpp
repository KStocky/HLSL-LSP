#include <hlsl_intellisense/workspace/document_uri.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace workspace = hlsl_intellisense::workspace;

namespace {

template <typename Function>
[[nodiscard]] std::optional<workspace::DocumentErrorCode> error_from(Function&& function) {
    try {
        function();
    } catch (const workspace::DocumentError& error) {
        return error.code();
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("POSIX file URIs and paths have normalized identity", "[workspace][uri]") {
    const auto from_uri = workspace::DocumentUri::from_uri(
        "FILE://localhost/project/./shaders/../main%20file.hlsl", workspace::PathStyle::posix);
    const auto from_path =
        workspace::DocumentUri::from_path("/project/main file.hlsl", workspace::PathStyle::posix);

    CHECK(from_uri == from_path);
    CHECK(from_uri.uri() == "file:///project/main%20file.hlsl");
    CHECK(from_uri.path() == "/project/main file.hlsl");
    CHECK(from_uri.identity() == "/project/main file.hlsl");

    const auto different_case =
        workspace::DocumentUri::from_path("/project/Main file.hlsl", workspace::PathStyle::posix);
    CHECK(different_case.identity() != from_uri.identity());

    const auto backslash =
        workspace::DocumentUri::from_path(R"(/project/a\b.hlsl)", workspace::PathStyle::posix);
    CHECK(backslash.path() == R"(/project/a\b.hlsl)");
    CHECK(backslash.uri() == "file:///project/a%5Cb.hlsl");
}

TEST_CASE("Windows identity is testable and case insensitive on every host", "[workspace][uri]") {
    const auto from_uri = workspace::DocumentUri::from_uri(
        "file:///c:/Work/Shaders/../Main%20File.hlsl", workspace::PathStyle::windows);
    const auto from_path = workspace::DocumentUri::from_path(R"(C:\work\main file.hlsl)",
                                                             workspace::PathStyle::windows);

    CHECK(from_uri.identity() == from_path.identity());
    CHECK(from_uri.uri() == "file:///C:/Work/Main%20File.hlsl");
    CHECK(from_uri.path() == R"(C:\Work\Main File.hlsl)");
    CHECK(from_uri.identity() == R"(c:\work\main file.hlsl)");
}

TEST_CASE("Windows UNC file URIs normalize server and separators", "[workspace][uri]") {
    const auto from_uri = workspace::DocumentUri::from_uri(
        "file://SERVER/share/folder/../shader.hlsl", workspace::PathStyle::windows);
    const auto from_path = workspace::DocumentUri::from_path(R"(\\server\share\shader.hlsl)",
                                                             workspace::PathStyle::windows);

    CHECK(from_uri == from_path);
    CHECK(from_uri.uri() == "file://server/share/shader.hlsl");
    CHECK(from_uri.path() == R"(\\server\share\shader.hlsl)");

    const auto cannot_escape_share = workspace::DocumentUri::from_path(
        R"(\\server\share\..\shader.hlsl)", workspace::PathStyle::windows);
    CHECK(cannot_escape_share.uri() == "file://server/share/shader.hlsl");
}

TEST_CASE("Invalid file URI and path forms are rejected explicitly", "[workspace][uri]") {
    CHECK(error_from([] {
              static_cast<void>(workspace::DocumentUri::from_uri("https://example.com/a.hlsl",
                                                                 workspace::PathStyle::posix));
          }) == workspace::DocumentErrorCode::invalid_uri);
    CHECK(error_from([] {
              static_cast<void>(workspace::DocumentUri::from_uri("file:///bad%ZZ.hlsl",
                                                                 workspace::PathStyle::posix));
          }) == workspace::DocumentErrorCode::invalid_uri);
    CHECK(error_from([] {
              static_cast<void>(workspace::DocumentUri::from_path("relative/file.hlsl",
                                                                  workspace::PathStyle::posix));
          }) == workspace::DocumentErrorCode::invalid_path);
    CHECK(error_from([] {
              static_cast<void>(workspace::DocumentUri::from_uri("file://server/share/file.hlsl",
                                                                 workspace::PathStyle::posix));
          }) == workspace::DocumentErrorCode::invalid_uri);
}
