#include <hlsl_intellisense/workspace/document_store.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

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

constexpr std::string_view uri = "file:///workspace/test.hlsl";

} // namespace

TEST_CASE("Documents open with language, version, text, and clean state",
          "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::posix};
    store.did_open(uri, "hlsl", 7, "float4 main() {}");

    const auto& document = store.document(uri);
    CHECK(document.language_id == "hlsl");
    CHECK(document.version == 7);
    CHECK(document.text == "float4 main() {}");
    CHECK(document.open);
    CHECK(!document.dirty);
    CHECK(store.contains("file:///workspace/./test.hlsl"));

    CHECK(error_from([&] { store.did_open(uri, "hlsl", 8, "other"); }) ==
          workspace::DocumentErrorCode::duplicate_open);
}

TEST_CASE("Full and incremental changes update text and version", "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::posix};
    store.did_open(uri, "hlsl", 1, "one\r\ntw\xF0\x9F\x98\x80o\n");

    const std::array changes{
        workspace::ContentChange{.range = workspace::Range{.start = {.line = 1, .character = 2},
                                                           .end = {.line = 1, .character = 4}},
                                 .range_length = 2,
                                 .text = "X"},
        workspace::ContentChange{.range = workspace::Range{.start = {.line = 1, .character = 3},
                                                           .end = {.line = 1, .character = 4}},
                                 .range_length = 1,
                                 .text = "!"}};
    store.did_change(uri, 2, changes);

    const auto& document = store.document(uri);
    CHECK(document.text == "one\r\ntwX!\n");
    CHECK(document.version == 2);
    CHECK(document.dirty);
}

TEST_CASE("Ordered batches apply each range to the preceding change",
          "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::posix};
    store.did_open(uri, "hlsl", 1, "abc");

    const std::array changes{
        workspace::ContentChange{.range = workspace::Range{.start = {.line = 0, .character = 1},
                                                           .end = {.line = 0, .character = 1}},
                                 .range_length = 0,
                                 .text = "X"},
        workspace::ContentChange{.range = workspace::Range{.start = {.line = 0, .character = 2},
                                                           .end = {.line = 0, .character = 4}},
                                 .range_length = 2,
                                 .text = "Y"}};
    store.did_change(uri, 2, changes);
    CHECK(store.document(uri).text == "aXY");

    const std::array full_then_incremental{
        workspace::ContentChange{.range = std::nullopt, .range_length = std::nullopt, .text = "pq"},
        workspace::ContentChange{.range = workspace::Range{.start = {.line = 0, .character = 2},
                                                           .end = {.line = 0, .character = 2}},
                                 .range_length = 0,
                                 .text = "r"}};
    store.did_change(uri, 3, full_then_incremental);
    CHECK(store.document(uri).text == "pqr");
}

TEST_CASE("Invalid changes are rejected atomically and explicitly", "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::posix};
    store.did_open(uri, "hlsl", 10, "abc");

    CHECK(error_from([&] {
              const std::array changes{workspace::ContentChange{
                  .range = workspace::Range{.start = {.line = 0, .character = 0},
                                            .end = {.line = 0, .character = 1}},
                  .range_length = 2,
                  .text = "x"}};
              store.did_change(uri, 11, changes);
          }) == workspace::DocumentErrorCode::range_length_mismatch);
    CHECK(store.document(uri).text == "abc");
    CHECK(store.document(uri).version == 10);

    CHECK(error_from([&] {
              const std::array changes{workspace::ContentChange{
                  .range = workspace::Range{.start = {.line = 0, .character = 2},
                                            .end = {.line = 0, .character = 1}},
                  .range_length = std::nullopt,
                  .text = "x"}};
              store.did_change(uri, 11, changes);
          }) == workspace::DocumentErrorCode::invalid_range);
    CHECK(error_from([&] {
              const std::array changes{workspace::ContentChange{
                  .range = workspace::Range{.start = {.line = 4, .character = 0},
                                            .end = {.line = 4, .character = 0}},
                  .range_length = 0,
                  .text = "x"}};
              store.did_change(uri, 11, changes);
          }) == workspace::DocumentErrorCode::invalid_position);
    CHECK(error_from([&] {
              const std::array changes{workspace::ContentChange{
                  .range = std::nullopt, .range_length = std::nullopt, .text = "\xC0\xAF"}};
              store.did_change(uri, 11, changes);
          }) == workspace::DocumentErrorCode::malformed_utf8);
}

TEST_CASE("Versions increase monotonically and missing documents are rejected",
          "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::posix};
    store.did_open(uri, "hlsl", 4, "a");
    const std::array change{
        workspace::ContentChange{.range = std::nullopt, .range_length = std::nullopt, .text = "b"}};

    CHECK(error_from([&] { store.did_change(uri, 4, change); }) ==
          workspace::DocumentErrorCode::version_not_increasing);
    CHECK(error_from([&] { store.did_change(uri, 3, change); }) ==
          workspace::DocumentErrorCode::version_not_increasing);
    CHECK(error_from([&] { store.did_save("file:///workspace/missing.hlsl"); }) ==
          workspace::DocumentErrorCode::document_not_found);
}

TEST_CASE("Save, close, reopen, and snapshots preserve lifecycle semantics",
          "[workspace][document-store]") {
    workspace::DocumentStore store{workspace::PathStyle::windows};
    store.did_open("file:///C:/Work/Test.hlsl", "hlsl", 1, "old");
    const std::array change{workspace::ContentChange{
        .range = std::nullopt, .range_length = std::nullopt, .text = "new"}};
    store.did_change("file:///c:/work/test.hlsl", 2, change);

    const auto snapshot = store.snapshot("file:///C:/WORK/TEST.hlsl");
    store.did_save("file:///C:/work/test.hlsl", std::string{"saved"});
    CHECK(snapshot.text() == "new");
    CHECK(snapshot.version() == 2);
    CHECK(snapshot.language_id() == "hlsl");
    CHECK(snapshot.path() == R"(C:\Work\Test.hlsl)");
    CHECK(store.document("file:///c:/work/test.hlsl").text == "saved");
    CHECK(!store.document("file:///c:/work/test.hlsl").dirty);

    store.did_close("file:///c:/WORK/test.hlsl");
    CHECK(!store.document("file:///C:/Work/Test.hlsl").open);
    CHECK(error_from([&] { store.did_save("file:///C:/work/test.hlsl"); }) ==
          workspace::DocumentErrorCode::document_not_open);

    store.did_open("file:///c:/work/test.hlsl", "hlsl-next", 20, "reopened");
    CHECK(store.document("file:///C:/Work/Test.hlsl").open);
    CHECK(store.document("file:///C:/Work/Test.hlsl").version == 20);
    CHECK(store.document("file:///C:/Work/Test.hlsl").language_id == "hlsl-next");
}
