#include <hlsl_intellisense/analysis/manager.h>
#include <hlsl_intellisense/dxc/intellisense.h>
#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/json_rpc/message.h>
#include <hlsl_intellisense/lsp/server.h>
#include <hlsl_intellisense/workspace/document_uri.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using hlsl_intellisense::json_rpc::Json;

class TestDirectory final {
  public:
    TestDirectory() {
        static std::size_t next_id{};
        path_ = std::filesystem::current_path() / ("lsp-server-tests-" + std::to_string(next_id++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    TestDirectory(const TestDirectory&) = delete;
    auto operator=(const TestDirectory&) -> TestDirectory& = delete;
    ~TestDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string frame(const Json& message) {
    const auto payload = message.dump();
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
}

// Matches `Scheduler::owner_for`'s own hash exactly: used by concurrency
// regressions below to deterministically place two roots on different
// scheduler workers (`hash(identity) % worker_count`) without needing to
// grow `worker_count` itself. Growing `worker_count` until two *fixed*
// identities' hashes happen to separate is unbounded in the worst case --
// on a platform/CI host where the two base paths' hashes share many common
// small factors (entirely possible, since the containing temp/work
// directory -- and therefore every identity's hash -- varies by host), that
// search can run away to an enormous worker count and thread pool. Instead,
// every test below keeps `worker_count` fixed at a small constant and
// searches a small, bounded number of *candidate root names* for the one
// root whose identity is free to vary, stopping as soon as one lands on a
// different worker than the other (fixed) root.
[[nodiscard]] std::uint64_t fnv1a_hash(std::string_view text) {
    std::uint64_t hash{14695981039346656037ULL};
    for (const auto character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::vector<Json> read_frames(const std::string& output) {
    std::istringstream stream{output};
    hlsl_intellisense::json_rpc::FrameReader reader{stream};
    std::vector<Json> messages;
    while (const auto payload = reader.read()) {
        messages.push_back(Json::parse(*payload));
    }
    return messages;
}

[[nodiscard]] std::string shader_uri() {
#ifdef _WIN32
    return "file:///C:/hlsl-lsp-tests/template.hlsl";
#else
    return "file:///hlsl-lsp-tests/template.hlsl";
#endif
}

[[nodiscard]] std::string workspace_uri() {
#ifdef _WIN32
    return "file:///C:/workspace";
#else
    return "file:///workspace";
#endif
}

[[nodiscard]] std::string valid_hlsl() {
    return "template<typename T>\n"
           "T combine(T left, T right) {\n"
           "    return left + right;\n"
           "}\n"
           "\n"
           "struct Number {\n"
           "    float value;\n"
           "    Number operator +(Number right) {\n"
           "        Number result = {value + right.value};\n"
           "        return result;\n"
           "    }\n"
           "};\n"
           "\n"
           "float4 main() : SV_Target {\n"
           "    Number left = {1.0};\n"
           "    Number right = {2.0};\n"
           "    Number sum = combine(left, right);\n"
           "    return sum.value.xxxx;\n"
           "}\n";
}

[[nodiscard]] Json request(std::int64_t id, std::string method, Json params = Json::object()) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", std::move(method)},
            {"params", std::move(params)}};
}

[[nodiscard]] Json notification(std::string method, Json params = Json::object()) {
    return {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

[[nodiscard]] Json request_without_params(std::int64_t id, std::string method) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}};
}

[[nodiscard]] Json notification_without_params(std::string method) {
    return {{"jsonrpc", "2.0"}, {"method", std::move(method)}};
}

[[nodiscard]] Json position_at(std::string_view source, std::size_t offset) {
    const auto position = hlsl_intellisense::workspace::lsp_position_at(source, offset);
    return {{"line", position.line}, {"character", position.character}};
}

} // namespace

TEST_CASE("LSP handler enforces lifecycle and invalid parameters", "[lsp][handler]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    const auto before_initialize = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "textDocument/completion", .params = Json::object()});
    REQUIRE(before_initialize.has_value());
    const auto* lifecycle_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*before_initialize);
    REQUIRE(lifecycle_error != nullptr);
    CHECK(lifecycle_error->error.code == -32002);

    const auto initialized = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "initialize",
        .params = Json{{"workspaceFolders",
                        Json::array({Json{{"uri", workspace_uri()}, {"name", "workspace"}}})}}});
    REQUIRE(initialized.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*initialized);
    REQUIRE(response != nullptr);
    CHECK(response->result["capabilities"]["positionEncoding"] == "utf-16");
    CHECK(response->result["capabilities"]["textDocumentSync"]["change"] == 2);
    CHECK(response->result["capabilities"].contains("completionProvider"));
    CHECK(response->result["capabilities"]["hoverProvider"] == true);
    CHECK(response->result["capabilities"]["signatureHelpProvider"]["triggerCharacters"] ==
          Json::array({"(", ","}));
    CHECK(response->result["capabilities"]["signatureHelpProvider"]["retriggerCharacters"] ==
          Json::array({")"}));
    CHECK(response->result["capabilities"]["inlayHintProvider"] == true);
    CHECK(response->result["capabilities"]["documentSymbolProvider"] == true);
    CHECK(response->result["capabilities"]["workspaceSymbolProvider"] == true);
    CHECK(response->result["capabilities"]["workspace"]["workspaceFolders"]["supported"] == true);

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    const auto invalid_completion = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/completion",
        .params = Json{{"textDocument", {{"uri", shader_uri()}}},
                       {"position", {{"line", -1}, {"character", 0}}}}});
    REQUIRE(invalid_completion.has_value());
    const auto* params_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*invalid_completion);
    REQUIRE(params_error != nullptr);
    CHECK(params_error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);

    for (const auto method :
         {"textDocument/hover", "textDocument/signatureHelp", "hlsl/memoryLayout"}) {
        const auto invalid = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{4},
            .method = method,
            .params = Json{{"textDocument", {{"uri", shader_uri()}}},
                           {"position", {{"line", 0}, {"character", -1}}}}});
        REQUIRE(invalid.has_value());
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*invalid);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
    }

    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "exit", .params = std::nullopt}));
    CHECK(server.exit_requested());
    CHECK(server.exit_code() == 1);
}

TEST_CASE("Server exposes memory layouts through hover and the custom protocol",
          "[lsp][memory-layout][integration]") {
    const auto uri = shader_uri();
    const std::string source = "// \xF0\x9F\x98\x80 UTF-16 prefix\n"
                               "struct Material { bool enabled; float3 colour; };\n"
                               "cbuffer Constants {\n"
                               "    float3 direction;\n"
                               "    float2 limits;\n"
                               "    float values[2];\n"
                               "    Material material;\n"
                               "};\n"
                               "cbuffer MatrixConstants {\n"
                               "    row_major float2x2 transform;\n"
                               "};\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"initializationOptions", {{"commandLinks", true}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto limits_offset = source.find("limits");
    REQUIRE(limits_offset != std::string::npos);
    const auto params = Json{{"textDocument", {{"uri", uri}}},
                             {"position", position_at(source, limits_offset + 2)}};
    const auto request_layout = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2}, .method = "hlsl/memoryLayout", .params = params});
    REQUIRE(request_layout.has_value());
    const auto* layout_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*request_layout);
    REQUIRE(layout_response != nullptr);
    const auto& layout = layout_response->result;
    INFO(layout.dump());
    CHECK(layout.size() == 8);
    CHECK(layout["name"] == "Constants");
    CHECK(layout["mode"] == "constantBuffer");
    CHECK(layout["allocationSize"] == 80);
    CHECK(layout["diagnostics"].empty());
    for (const auto key : {"size", "alignment", "allocationSize"}) {
        CHECK(layout[key].is_number_unsigned());
    }
    CHECK(layout["members"][0]["offset"] == 0);
    CHECK(layout["members"][1]["offset"] == 16);
    CHECK(layout["members"][1]["paddingBefore"] == 4);
    CHECK(layout["members"][2]["offset"] == 32);
    CHECK(layout["members"][2]["kind"] == "array");
    CHECK(layout["members"][2]["arrayStride"] == 16);
    CHECK(layout["members"][2]["arrayDimensions"] == Json::array({2}));
    REQUIRE(layout["members"][2]["members"].size() == 2);
    CHECK(layout["members"][2]["members"][0]["name"] == "[0]");
    CHECK(layout["members"][2]["members"][0]["arrayIndex"] == 0);
    CHECK(layout["members"][2]["members"][0]["offset"] == 0);
    CHECK(layout["members"][2]["members"][1]["arrayIndex"] == 1);
    CHECK(layout["members"][2]["members"][1]["offset"] == 16);
    CHECK(layout["members"][3]["offset"] == 64);
    REQUIRE(layout["members"][3]["members"].size() == 2);
    CHECK(layout["members"][3]["members"][1]["offset"] == 4);
    CHECK(layout["members"][0].size() == 9);
    CHECK(layout["members"][0].contains("name"));
    CHECK(layout["members"][0].contains("type"));
    CHECK(layout["members"][0]["kind"] == "vector");
    CHECK(layout["members"][0].contains("offset"));
    CHECK(layout["members"][0].contains("size"));
    CHECK(layout["members"][0].contains("allocationSize"));
    CHECK(layout["members"][0].contains("alignment"));
    CHECK(layout["members"][0].contains("paddingBefore"));
    CHECK(layout["members"][0].contains("members"));
    for (const auto key : {"offset", "size", "alignment", "paddingBefore"}) {
        CHECK(layout["members"][0][key].is_number_unsigned());
    }

    const auto transform_offset = source.find("transform");
    REQUIRE(transform_offset != std::string::npos);
    const auto matrix_result = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{20},
        .method = "hlsl/memoryLayout",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", position_at(source, transform_offset + 2)}}});
    REQUIRE(matrix_result.has_value());
    const auto* matrix_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*matrix_result);
    REQUIRE(matrix_response != nullptr);
    const auto& matrix = matrix_response->result["members"][0];
    CHECK(matrix["kind"] == "matrix");
    CHECK(matrix["matrixStride"] == 16);
    CHECK(matrix["rowMajor"] == true);
    REQUIRE(matrix["members"].size() == 2);

    const auto limits_name = source.find("limits");
    REQUIRE(limits_name != std::string::npos);
    const auto hover = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/hover",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", position_at(source, limits_name + 2)}}});
    REQUIRE(hover.has_value());
    const auto* hover_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*hover);
    REQUIRE(hover_response != nullptr);
    CHECK(hover_response->result["contents"]["kind"] == "markdown");
    const auto hover_text = hover_response->result["contents"]["value"].get<std::string>();
    CHECK(hover_text.find("size 8 bytes, alignment 4 bytes, packed offset 16 bytes") !=
          std::string::npos);
    CHECK(hover_text.find("[Memory Layout](command:hlsl.showMemoryLayout?") != std::string::npos);

    const auto no_layout = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{4},
        .method = "hlsl/memoryLayout",
        .params =
            Json{{"textDocument", {{"uri", uri}}}, {"position", {{"line", 0}, {"character", 1}}}}});
    REQUIRE(no_layout.has_value());
    const auto* no_layout_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*no_layout);
    REQUIRE(no_layout_response != nullptr);
    CHECK(no_layout_response->result.is_null());

    const std::string edited = "struct Material { double value; };\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", edited}}})}}}));
    const auto edited_offset = edited.find("value");
    const auto edited_layout = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{5},
        .method = "hlsl/memoryLayout",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", position_at(edited, edited_offset + 1)}}});
    REQUIRE(edited_layout.has_value());
    const auto* edited_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*edited_layout);
    REQUIRE(edited_response != nullptr);
    CHECK(edited_response->result["size"] == 8);
    CHECK(edited_response->result["members"][0]["type"] == "double");
}

TEST_CASE("Server exposes compiler-backed preprocessor exploration",
          "[lsp][preprocessor][integration]") {
    const auto uri = shader_uri();
    const std::string source = "#define ACTIVE_VALUE 7\n"
                               "#include \"missing.hlsli\"\n"
                               "#define HEADER_NAME \"dynamic.hlsli\"\n"
                               "#include HEADER_NAME\n"
                               "#if 0\n"
                               "float skippedValue;\n"
                               "#endif\n"
                               "float activeValue;\n";
    std::string server_log;
    hlsl_intellisense::lsp::Server server{[](const auto&) {},
                                          [&server_log](std::string_view message) {
                                              server_log.append(message);
                                              server_log.push_back('\n');
                                          }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto response = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "hlsl/preprocessorExplorer",
                                             .params = Json{{"textDocument", {{"uri", uri}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    if (const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*response)) {
        FAIL(error->error.message << '\n' << server_log);
    }
    REQUIRE(result != nullptr);
    INFO(result->result.dump());
    CHECK(result->result["rootUri"] == uri);
    REQUIRE(result->result["files"].size() == 1);
    REQUIRE(result->result["files"][0]["includes"].size() == 2);
    CHECK(result->result["files"][0]["includes"][0]["status"] == "missing");
    CHECK(result->result["files"][0]["includes"][1]["status"] == "dynamic");
    CHECK_FALSE(result->result["skippedRegions"].empty());
    REQUIRE_FALSE(result->result["macros"].empty());
    const auto active_macro = std::ranges::find_if(result->result["macros"], [](const auto& macro) {
        return macro["name"] == "ACTIVE_VALUE";
    });
    REQUIRE(active_macro != result->result["macros"].end());
    CHECK((*active_macro)["value"] == "7");
    CHECK((*active_macro)["source"] == "compiler");
    CHECK_FALSE(result->result["settings"].empty());
    const auto target_profile =
        std::ranges::find_if(result->result["settings"], [](const auto& setting) {
            return setting["name"] == "targetProfile";
        });
    REQUIRE(target_profile != result->result["settings"].end());
    CHECK((*target_profile)["value"] == "");
    CHECK((*target_profile)["origin"] == "not configured");
    CHECK_FALSE(result->result["diagnostics"].empty());
}

TEST_CASE("Memory layout protocol handles packoffset and conditionals via DXC",
          "[lsp][memory-layout][unsupported]") {
    const auto uri = shader_uri();
    // DXC handles packoffset natively; the layout is compiler-authoritative.
    const std::string source = "cbuffer Valid { float value : packoffset(c0); };\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto offset = source.find("value");
    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/memoryLayout",
        .params =
            Json{{"textDocument", {{"uri", uri}}}, {"position", position_at(source, offset + 1)}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    INFO(result->result.dump());
    // DXC compiles packoffset; no diagnostic expected.
    CHECK(result->result["diagnostics"].empty());

    // DXC compiles conditional preprocessing using the default macro state.
    const std::string conditional = "struct Conditional {\n"
                                    "#if FEATURE\n"
                                    "    float value;\n"
                                    "#else\n"
                                    "    double value;\n"
                                    "#endif\n"
                                    "};\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", conditional}}})}}}));
    const auto conditional_response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "hlsl/memoryLayout",
        .params =
            Json{{"textDocument", {{"uri", uri}}}, {"position", {{"line", 0}, {"character", 9}}}}});
    REQUIRE(conditional_response.has_value());
    const auto* conditional_result =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*conditional_response);
    REQUIRE(conditional_result != nullptr);
    // DXC compiles with default macros; the layout is valid.
    CHECK(conditional_result->result["diagnostics"].empty());
}

TEST_CASE("Server provides hierarchical document and searchable workspace symbols",
          "[lsp][symbols][navigation][integration]") {
    const auto uri = shader_uri();
    const auto lf_source = valid_hlsl();
    std::string source;
    source.reserve(lf_source.size() + std::ranges::count(lf_source, '\n'));
    for (const auto character : lf_source) {
        if (character == '\n') {
            source.push_back('\r');
        }

        source.push_back(character);
    }
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    const auto initialized = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()});
    REQUIRE(initialized.has_value());
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto document_symbols = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "textDocument/documentSymbol",
                                             .params = Json{{"textDocument", {{"uri", uri}}}}});
    REQUIRE(document_symbols.has_value());
    const auto* document_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*document_symbols);
    REQUIRE(document_response != nullptr);
    const auto& symbols = document_response->result;
    const auto number = std::ranges::find_if(
        symbols, [](const auto& symbol) { return symbol["name"] == "Number"; });
    REQUIRE(number != symbols.end());
    CHECK((*number)["kind"] == 23);
    CHECK((*number)["detail"] == "HLSL struct");
    REQUIRE((*number).contains("children"));
    CHECK(std::ranges::any_of((*number)["children"], [](const auto& symbol) {
        return symbol["name"] == "value" && symbol["kind"] == 8;
    }));
    const auto overloaded_operator = std::ranges::find_if(
        (*number)["children"], [](const auto& symbol) { return symbol["name"] == "operator+"; });
    REQUIRE(overloaded_operator != (*number)["children"].end());
    CHECK((*overloaded_operator)["kind"] == 25);
    CHECK((*overloaded_operator)["detail"] == "HLSL operator");
    CHECK((*overloaded_operator)["selectionRange"]["end"]["character"] == 21);
    CHECK(std::ranges::any_of(symbols, [](const auto& symbol) {
        return symbol["name"] == "main" && symbol["kind"] == 12;
    }));

    const auto workspace_symbols = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3}, .method = "workspace/symbol", .params = Json{{"query", "MAIN"}}});
    REQUIRE(workspace_symbols.has_value());
    const auto* workspace_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*workspace_symbols);
    REQUIRE(workspace_response != nullptr);
    REQUIRE(workspace_response->result.size() == 1);
    CHECK(workspace_response->result[0]["name"] == "main");
    CHECK(workspace_response->result[0]["kind"] == 12);
    CHECK(workspace_response->result[0]["containerName"] == "HLSL");
    CHECK(workspace_response->result[0]["location"]["uri"] == uri);
}

TEST_CASE("Document symbols convert DXC UTF-16 offsets in non-ASCII sources",
          "[lsp][symbols][unicode][integration]") {
    const auto uri = shader_uri();
    const std::string source = "// BMP: \xC3\x97; supplementary: \xF0\x9F\x98\x80\r\n"
                               "struct Payload { float value; };\r\n"
                               "float4 main() : SV_Target { return 1.0; }\r\n";
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto response = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "textDocument/documentSymbol",
                                             .params = Json{{"textDocument", {{"uri", uri}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto payload = std::ranges::find_if(
        result->result, [](const auto& symbol) { return symbol["name"] == "Payload"; });
    REQUIRE(payload != result->result.end());
    CHECK((*payload)["selectionRange"]["start"] == Json{{"line", 1}, {"character", 7}});
    const auto main = std::ranges::find_if(
        result->result, [](const auto& symbol) { return symbol["name"] == "main"; });
    REQUIRE(main != result->result.end());
    CHECK((*main)["selectionRange"]["start"] == Json{{"line", 2}, {"character", 7}});
}

TEST_CASE("Document symbols truncate compiler-expanded declaration floods",
          "[lsp][symbols][limits][integration]") {
    const auto uri = shader_uri();
    std::string source;
    for (std::size_t index = 0; index < 1100; ++index) {
        source += "static float value" + std::to_string(index) + ";\n";
    }
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto response = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "textDocument/documentSymbol",
                                             .params = Json{{"textDocument", {{"uri", uri}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    CHECK(result->result.size() == 1024);
    CHECK(std::ranges::any_of(notifications, [](const auto& notification) {
        return notification.method == "window/logMessage" && notification.params.has_value() &&
               (*notification.params)["message"].template get<std::string>().find("truncated") !=
                   std::string::npos;
    }));
}

TEST_CASE("Server provides semantic tokens and definitions", "[lsp][navigation][integration]") {
    const auto uri = shader_uri();
    const auto source = valid_hlsl();
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    const auto initialized = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()});
    REQUIRE(initialized.has_value());
    const auto* initialize_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*initialized);
    REQUIRE(initialize_response != nullptr);
    CHECK(initialize_response->result["capabilities"]["definitionProvider"] == true);
    const auto& provider = initialize_response->result["capabilities"]["semanticTokensProvider"];
    CHECK(provider["full"] == true);
    CHECK(provider["legend"]["tokenTypes"][11] == "keyword");

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto semantic = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "textDocument/semanticTokens/full",
                                             .params = Json{{"textDocument", {{"uri", uri}}}}});
    REQUIRE(semantic.has_value());
    const auto* semantic_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*semantic);
    REQUIRE(semantic_response != nullptr);
    const auto& data = semantic_response->result["data"];
    REQUIRE(!data.empty());
    CHECK(data.size() % 5 == 0);
    bool has_keyword{};
    for (std::size_t index = 3; index < data.size(); index += 5) {
        has_keyword = has_keyword || data[index] == 11;
    }
    CHECK(has_keyword);

    const auto definition = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/definition",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", {{"line", 16}, {"character", 20}}}}});
    REQUIRE(definition.has_value());
    const auto* definition_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*definition);
    REQUIRE(definition_response != nullptr);
    CHECK(definition_response->result["uri"] == uri);
    CHECK(definition_response->result["range"]["start"]["line"] == 1);
    CHECK(definition_response->result["range"]["start"]["character"] == 2);
}

TEST_CASE("Server provides UTF-16 hover and overload signature help from open buffers",
          "[lsp][hover][signature-help][integration]") {
    const auto uri = shader_uri();
    const std::string source =
        "float shade(float value, float bias) { return value + bias; }\n"
        "float shade(float value, float bias, float weight) { return value + bias * weight; }\n"
        "template<typename T, typename U> T convert(U value) { return value; }\n"
        "struct Material {\n"
        "  float Scale(float value) { return value; }\n"
        "  float Scale(float value, float bias) { return value + bias; }\n"
        "};\n"
        "float4 main() : SV_Target {\n"
        "  Material material;\n"
        "  /* \xF0\x9F\x98\x80 */ float value = shade(1.0, 2.0, 3.0);\n"
        "  value = material.Scale(value, 4.0);\n"
        "  float values[2] = {1.0, 2.0};\n"
        "  value = shade(convert<float, float>(value), values[uint(shade(0.0, 0.0, 0.0))], "
        "1.0);\n"
        "  value = shade(value, /* ignored (, [, {, <, */ 2.0, 3.0);\n"
        "  value = shade(value, \"ignored (, [, {, <, )\", 3.0);\n"
        "  value = shade(value < 1.0, value > 2.0, value);\n"
        "  value = shade(uint(value) << 1, uint(value) >> 1, value);\n"
        "  value = shade(convert<float, float>(convert<float, float>(value)), value, value);\n"
        "  return float4(value, value, value, 1.0);\n"
        "}\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto request_result = [&server, &uri](std::int64_t id, std::string method,
                                                const Json& request_position) {
        const auto result = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = id,
            .method = std::move(method),
            .params = Json{{"textDocument", {{"uri", uri}}}, {"position", request_position}}});
        REQUIRE(result.has_value());
        const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
        REQUIRE(response != nullptr);
        return response->result;
    };

    const auto value_offset = source.find("value = shade");
    REQUIRE(value_offset != std::string::npos);
    const auto hover =
        request_result(2, "textDocument/hover", position_at(source, value_offset + 1));
    CHECK(hover["contents"]["kind"] == "plaintext");
    CHECK(hover["contents"]["value"].get<std::string>().find("float value") != std::string::npos);
    CHECK(hover["contents"]["value"].get<std::string>().find("Type: float") != std::string::npos);
    CHECK(hover["range"]["start"] == position_at(source, value_offset));
    CHECK(hover["range"]["end"] == position_at(source, value_offset + 5));

    const auto method_expression = source.find("material.Scale(value");
    REQUIRE(method_expression != std::string::npos);
    const auto method_offset = method_expression + std::string_view{"material."}.size();
    const auto method_hover =
        request_result(3, "textDocument/hover", position_at(source, method_offset + 1));
    CHECK(method_hover["contents"]["value"].get<std::string>().find("Material::Scale") !=
          std::string::npos);
    CHECK(method_hover["contents"]["value"].get<std::string>().find(
              "float Scale(float value, float bias)") != std::string::npos);

    const auto function_call = source.find("shade(1.0, 2.0, 3.0)");
    REQUIRE(function_call != std::string::npos);
    const auto second_comma = source.find(',', source.find(',', function_call) + 1);
    const auto function_help =
        request_result(4, "textDocument/signatureHelp", position_at(source, second_comma + 1));
    CHECK(function_help["activeSignature"] == 0);
    CHECK(function_help["activeParameter"] == 2);
    REQUIRE(function_help["signatures"].size() == 2);
    CHECK(function_help["signatures"][0]["label"] ==
          "float shade(float value, float bias, float weight)");
    CHECK(function_help["signatures"][0]["parameters"][2]["label"] == "float weight");
    CHECK(function_help["signatures"][1]["activeParameter"] == 1);

    const auto method_comma = source.find(',', method_offset);
    const auto method_help =
        request_result(5, "textDocument/signatureHelp", position_at(source, method_comma + 1));
    CHECK(method_help["activeParameter"] == 1);
    REQUIRE(method_help["signatures"].size() == 2);
    CHECK(method_help["signatures"][0]["label"] ==
          "float Material::Scale(float value, float bias)");

    const auto nested_call = source.find("shade(convert<float, float>");
    REQUIRE(nested_call != std::string::npos);
    const auto nested_second_comma = source.find("], 1.0", nested_call) + 1;
    const auto nested_help = request_result(6, "textDocument/signatureHelp",
                                            position_at(source, nested_second_comma + 1));
    CHECK(nested_help["activeParameter"] == 2);

    const auto comment_call = source.find("shade(value, /*");
    REQUIRE(comment_call != std::string::npos);
    const auto comment_second_comma =
        source.find(',', source.find("*/", comment_call) + std::string_view{"*/"}.size());
    const auto comment_help = request_result(7, "textDocument/signatureHelp",
                                             position_at(source, comment_second_comma + 1));
    CHECK(comment_help["activeParameter"] == 2);

    const auto comment_word = source.find("ignored", comment_call);
    CHECK(request_result(8, "textDocument/hover", position_at(source, comment_word + 1)).is_null());
    CHECK(request_result(9, "textDocument/signatureHelp", position_at(source, comment_word + 1))
              .is_null());

    const auto string_call = source.find("shade(value, \"");
    REQUIRE(string_call != std::string::npos);
    const auto string_end = source.find("\",", string_call);
    const auto string_help =
        request_result(10, "textDocument/signatureHelp", position_at(source, string_end + 2));
    CHECK(string_help["activeParameter"] == 1);
    CHECK(std::ranges::any_of(string_help["signatures"], [](const auto& signature) {
        return signature["parameters"].size() == 3 && signature["activeParameter"] == 2;
    }));

    const auto comparison_call = source.find("shade(value < 1.0");
    REQUIRE(comparison_call != std::string::npos);
    const auto comparison_second_comma = source.find(", value);", comparison_call);
    const auto comparison_help = request_result(11, "textDocument/signatureHelp",
                                                position_at(source, comparison_second_comma + 1));
    CHECK(comparison_help["activeParameter"] == 2);

    const auto shift_call = source.find("shade(uint(value) << 1");
    REQUIRE(shift_call != std::string::npos);
    const auto shift_second_comma = source.find(", value);", shift_call);
    const auto shift_help = request_result(12, "textDocument/signatureHelp",
                                           position_at(source, shift_second_comma + 1));
    CHECK(shift_help["activeParameter"] == 2);

    const auto template_call = source.find("shade(convert<float, float>(convert<float, float>");
    REQUIRE(template_call != std::string::npos);
    const auto template_first_comma = source.find(")), value, value", template_call) + 2;
    const auto template_second_comma = source.find(',', template_first_comma + 1);
    const auto template_help = request_result(13, "textDocument/signatureHelp",
                                              position_at(source, template_second_comma + 1));
    CHECK(template_help["activeParameter"] == 2);

    const auto constructor = source.find("float4(value");
    CHECK(request_result(14, "textDocument/signatureHelp",
                         position_at(source, source.find('(', constructor) + 1))
              .is_null());
    CHECK(request_result(15, "textDocument/hover",
                         position_at(source, source.find("return float4") + 2))
              .is_null());
}

TEST_CASE("Server provides hover and signature help for function-template calls",
          "[lsp][hover][signature-help][templates][integration]") {
    const auto check_call = [](std::string source, std::string_view call) {
        const auto uri = shader_uri();
        std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
        hlsl_intellisense::lsp::Server server{
            [&notifications](const auto& value) { notifications.push_back(value); }};
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "initialized", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

        const auto call_offset = source.find(call);
        REQUIRE(call_offset != std::string::npos);
        const auto hover = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{2},
            .method = "textDocument/hover",
            .params = Json{{"textDocument", {{"uri", uri}}},
                           {"position", position_at(source, call_offset + 1)}}});
        REQUIRE(hover.has_value());
        const auto* hover_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*hover);
        REQUIRE(hover_response != nullptr);
        CHECK(hover_response->result["contents"]["value"].get<std::string>().find(
                  "float conv(float value)") != std::string::npos);

        const auto open_parenthesis = source.find('(', call_offset);
        const auto signature = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{3},
            .method = "textDocument/signatureHelp",
            .params = Json{{"textDocument", {{"uri", uri}}},
                           {"position", position_at(source, open_parenthesis + 1)}}});
        REQUIRE(signature.has_value());
        const auto* signature_response =
            std::get_if<hlsl_intellisense::json_rpc::Response>(&*signature);
        REQUIRE(signature_response != nullptr);
        REQUIRE_FALSE(signature_response->result.is_null());
        CHECK(signature_response->result["signatures"][0]["label"] == "float conv(float value)");
        CHECK(signature_response->result["signatures"][0]["parameters"][0]["label"] ==
              "float value");
    };

    SECTION("explicit template arguments") {
        check_call("template<typename T, typename U> T conv(U value) { return (T)value; }\n"
                   "float4 main(float x : X) : SV_Target { return conv<float, float>(x).xxxx; }\n",
                   "conv<float, float>");
    }

    SECTION("inferred template arguments") {
        check_call("template<typename T> T conv(T value) { return value; }\n"
                   "float4 main(float x : X) : SV_Target { return conv(x).xxxx; }\n",
                   "conv(x)");
    }
}

TEST_CASE("Server supports hover and signature help with common source line endings",
          "[lsp][hover][signature-help][line-endings][integration]") {
    const auto check_line_ending = [](std::string_view line_ending) {
        const auto uri = shader_uri();
        const auto source = "float shade(float value) { return value; }" +
                            std::string{line_ending} +
                            "float4 main(float x : X) : SV_Target { return shade(x).xxxx; }";
        std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
        hlsl_intellisense::lsp::Server server{
            [&notifications](const auto& value) { notifications.push_back(value); }};
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "initialized", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

        const auto call = source.find("shade(x)");
        REQUIRE(call != std::string::npos);
        const auto hover = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{2},
            .method = "textDocument/hover",
            .params = Json{{"textDocument", {{"uri", uri}}},
                           {"position", position_at(source, call + 1)}}});
        REQUIRE(hover.has_value());
        const auto* hover_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*hover);
        REQUIRE(hover_response != nullptr);
        CHECK(hover_response->result["contents"]["value"].get<std::string>().find(
                  "float shade(float value)") != std::string::npos);
        CHECK(hover_response->result["range"]["start"] == position_at(source, call));

        const auto signature = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{3},
            .method = "textDocument/signatureHelp",
            .params = Json{{"textDocument", {{"uri", uri}}},
                           {"position", position_at(source, source.find('(', call) + 1)}}});
        REQUIRE(signature.has_value());
        const auto* signature_response =
            std::get_if<hlsl_intellisense::json_rpc::Response>(&*signature);
        REQUIRE(signature_response != nullptr);
        REQUIRE_FALSE(signature_response->result.is_null());
        CHECK(signature_response->result["signatures"][0]["label"] == "float shade(float value)");
    };

    SECTION("CR") { check_line_ending("\r"); }
    SECTION("LF") { check_line_ending("\n"); }
    SECTION("CRLF") { check_line_ending("\r\n"); }
}

TEST_CASE("Hover and signature help reparse unsaved edits", "[lsp][hover][signature-help]") {
    const auto uri = shader_uri();
    const std::string original = "float oldFunction(float value) { return value; }\n"
                                 "float4 main() : SV_Target { return oldFunction(1.0).xxxx; }\n";
    const std::string edited =
        "float newFunction(float value, float bias) { return value + bias; }\n"
        "float4 main() : SV_Target { return newFunction(1.0, 2.0).xxxx; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", original}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", edited}}})}}}));

    const auto hover_offset = edited.find("newFunction(1.0");
    const auto hover = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/hover",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", position_at(edited, hover_offset + 1)}}});
    REQUIRE(hover.has_value());
    const auto* hover_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*hover);
    REQUIRE(hover_response != nullptr);
    CHECK(hover_response->result["contents"]["value"].get<std::string>().find("newFunction") !=
          std::string::npos);
    CHECK(hover_response->result["contents"]["value"].get<std::string>().find("oldFunction") ==
          std::string::npos);

    const auto comma = edited.find(',', hover_offset);
    const auto signature = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/signatureHelp",
        .params =
            Json{{"textDocument", {{"uri", uri}}}, {"position", position_at(edited, comma + 1)}}});
    REQUIRE(signature.has_value());
    const auto* signature_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*signature);
    REQUIRE(signature_response != nullptr);
    CHECK(signature_response->result["activeParameter"] == 1);
    CHECK(signature_response->result["signatures"][0]["label"] ==
          "float newFunction(float value, float bias)");
}

TEST_CASE("Server filters and refreshes compiler-backed inlay hints",
          "[lsp][inlay-hints][configuration][integration]") {
    const auto uri = shader_uri();
    const std::string source = "float shade(float value, float bias) { return value + bias; }\n"
                               "float4 main() : SV_Target {\n"
                               "  auto first = shade(1.0, 2.0);\n"
                               "  auto second = shade(3.0, 4.0);\n"
                               "  return (first + second).xxxx;\n"
                               "}\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<hlsl_intellisense::json_rpc::Request> outbound_requests;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {},
        [&outbound_requests](const auto& value) { outbound_requests.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params =
            Json{{"capabilities", {{"workspace", {{"inlayHint", {{"refreshSupport", true}}}}}}},
                 {"initializationOptions", {{"hlsl", {{"languageVersion", "202x"}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));
    REQUIRE(outbound_requests.size() == 1);
    outbound_requests.clear();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Debug"}}}));
    REQUIRE(outbound_requests.size() == 1);
    CHECK(outbound_requests.back().method == "workspace/inlayHint/refresh");
    CHECK_FALSE(outbound_requests.back().params.has_value());
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Debug"}}}));
    CHECK(outbound_requests.size() == 1);

    const auto request_hints = [&server, &uri](std::int64_t id, const Json& requested_range) {
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = id,
            .method = "textDocument/inlayHint",
            .params = Json{{"textDocument", {{"uri", uri}}}, {"range", requested_range}}});
        REQUIRE(response.has_value());
        const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
        REQUIRE(result != nullptr);
        return result->result;
    };

    const Json first_declaration_range{{"start", {{"line", 2}, {"character", 0}}},
                                       {"end", {{"line", 3}, {"character", 0}}}};
    const auto first_hints = request_hints(2, first_declaration_range);
    REQUIRE_FALSE(first_hints.empty());
    CHECK(std::ranges::all_of(first_hints,
                              [](const auto& hint) { return hint["position"]["line"] == 2; }));
    CHECK(std::ranges::any_of(first_hints, [](const auto& hint) {
        return hint["label"] == ": float" && hint["kind"] == 1;
    }));
    CHECK(std::ranges::any_of(first_hints, [](const auto& hint) {
        return hint["label"] == "value:" && hint["kind"] == 2;
    }));
    CHECK(std::ranges::any_of(first_hints, [](const auto& hint) {
        return hint["label"] == "bias:" && hint["kind"] == 2;
    }));
    CHECK_FALSE(std::ranges::any_of(
        first_hints, [](const auto& hint) { return hint["label"] == "variant: Debug"; }));
    const Json original_full_range{{"start", {{"line", 0}, {"character", 0}}},
                                   {"end", {{"line", 6}, {"character", 0}}}};
    const auto original_hints = request_hints(20, original_full_range);
    CHECK(std::ranges::count_if(original_hints,
                                [](const auto& hint) { return hint.value("kind", 0) == 2; }) == 4);
    CHECK_FALSE(std::ranges::any_of(original_hints, [](const auto& hint) {
        return hint.value("kind", 0) == 2 && hint["position"]["line"] == 0;
    }));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings",
                        {{"hlsl.languageVersion", "202x"},
                         {"hlsl.inlayHints.types", false},
                         {"hlsl.inlayHints.parameters", false},
                         {"hlsl.inlayHints.activeVariant", false}}}}}));
    REQUIRE(outbound_requests.size() == 2);
    CHECK(outbound_requests.back().method == "workspace/inlayHint/refresh");
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings",
                        {{"hlsl.languageVersion", "202x"},
                         {"hlsl.inlayHints.types", false},
                         {"hlsl.inlayHints.parameters", false},
                         {"hlsl.inlayHints.activeVariant", false}}}}}));
    CHECK(outbound_requests.size() == 2);
    CHECK(request_hints(3, first_declaration_range).empty());

    const std::string edited =
        "float shade(float intensity, float offset) { return intensity + offset; }\n"
        "float4 main() : SV_Target { auto updated = shade(5.0, 6.0); return updated.xxxx; }\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", edited}}})}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params =
            Json{{"settings",
                  {{"hlsl",
                    {{"languageVersion", "202x"},
                     {"inlayHints",
                      {{"types", false}, {"parameters", true}, {"activeVariant", true}}}}}}}}}));
    REQUIRE(outbound_requests.size() == 4);
    CHECK(outbound_requests[0].id != outbound_requests[1].id);
    CHECK(outbound_requests[1].id != outbound_requests[2].id);
    const Json full_range{{"start", {{"line", 0}, {"character", 0}}},
                          {"end", {{"line", 2}, {"character", 0}}}};
    const auto edited_hints = request_hints(4, full_range);
    CHECK(std::ranges::any_of(edited_hints, [](const auto& hint) {
        return hint["label"] == "intensity:" && hint["kind"] == 2;
    }));
    CHECK(std::ranges::any_of(edited_hints, [](const auto& hint) {
        return hint["label"] == "offset:" && hint["kind"] == 2;
    }));
    CHECK_FALSE(std::ranges::any_of(edited_hints,
                                    [](const auto& hint) { return hint["label"] == ": float"; }));
    CHECK_FALSE(std::ranges::any_of(
        edited_hints, [](const auto& hint) { return hint["label"] == "variant: Debug"; }));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeClientDefaults",
        .params = Json{{"hlsl", {{"languageVersion", "2018"}}}}}));
    REQUIRE(outbound_requests.size() == 5);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWorkspaceFolders",
        .params = Json{
            {"event",
             {{"removed", Json::array()},
              {"added", Json::array({Json{{"uri", workspace_uri()}, {"name", "workspace"}}})}}}}}));
    REQUIRE(outbound_requests.size() == 6);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", uri}, {"type", 2}}})}}}));
    REQUIRE(outbound_requests.size() == 7);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings",
                        {{"hlsl.targetProfile", "ps_6_6"},
                         {"hlsl.dxcRuntimeDirectory", "missing-dxc-runtime"},
                         {"hlsl.inlayHints.types", false},
                         {"hlsl.inlayHints.parameters", true},
                         {"hlsl.inlayHints.activeVariant", true}}}}}));
    REQUIRE(outbound_requests.size() == 8);
    CHECK(std::ranges::all_of(outbound_requests, [](const auto& request) {
        return request.method == "workspace/inlayHint/refresh";
    }));
}

TEST_CASE("Inlay hint refresh requests require advertised client support",
          "[lsp][inlay-hints][refresh]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<hlsl_intellisense::json_rpc::Request> outbound_requests;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {},
        [&outbound_requests](const auto& value) { outbound_requests.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings", {{"hlsl.inlayHints.types", false}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Debug"}}}));
    CHECK(outbound_requests.empty());
}

TEST_CASE("Inlay hints cover bounded chunks beyond one MiB",
          "[lsp][inlay-hints][range][integration]") {
    const auto uri = shader_uri();
    std::string source = "float shade(float value) { return value; }\n"
                         "float4 main() : SV_Target {\n";
    for (std::size_t index = 0; index < 175; ++index) {
        source += "  float value" + std::to_string(index) + " = shade(1.0);\n";
    }
    source.append(1100U * 1024U, ' ');
    for (std::size_t index = 175; index < 350; ++index) {
        source += "  float value" + std::to_string(index) + " = shade(2.0);\n";
    }
    source += "return value349.xxxx;\n}\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"initializationOptions", {{"hlsl", {{"languageVersion", "202x"}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/inlayHint",
        .params = Json{
            {"textDocument", {{"uri", uri}}},
            {"range",
             {{"start", position_at(source, 0)}, {"end", position_at(source, source.size())}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    CHECK(std::ranges::count_if(result->result, [](const auto& hint) {
              return hint["label"] == "value:" && hint["kind"] == 2;
          }) == 350);
    const auto distant_argument = position_at(source, source.rfind("2.0"));
    CHECK(std::ranges::any_of(result->result, [&](const auto& hint) {
        return hint["label"] == "value:" && hint["position"] == distant_argument;
    }));
}

TEST_CASE("Inlay parameter scanning carries call state across bounded chunks",
          "[lsp][inlay-hints][range][integration]") {
    const auto uri = shader_uri();
    std::string source = "float shade(float value, float bias) { return value + bias; }\n"
                         "float4 main() : SV_Target { return shade(1.0";
    source.append(300U * 1024U, ' ');
    source += ", 2.0).xxxx; }\n";
    const auto second_argument = source.find("2.0");

    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/inlayHint",
        .params = Json{
            {"textDocument", {{"uri", uri}}},
            {"range",
             {{"start", position_at(source, second_argument)},
              {"end", position_at(source, second_argument + std::string_view{"2.0"}.size())}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    REQUIRE(result->result.size() == 1);
    CHECK(result->result[0]["label"] == "bias:");
    CHECK(result->result[0]["position"] == position_at(source, second_argument));
}

TEST_CASE("Inlay hints reject settings generations superseded during analysis",
          "[lsp][inlay-hints][stale][concurrency]") {
    const auto check_superseded = [](bool change_variant) {
        const auto uri = shader_uri();
        const std::string source = "float shade(float value) { return value; }\n"
                                   "float4 main() : SV_Target { return shade(1.0).xxxx; }\n";
        auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
        std::promise<void> entered;
        std::promise<void> release;
        auto released = release.get_future().share();
        hooks->before_interactive = [&](std::string_view) {
            entered.set_value();
            released.wait();
        };
        hlsl_intellisense::lsp::ServerOptions options;
        options.background_analysis = true;
        options.analysis_hooks = hooks;
        std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
        hlsl_intellisense::lsp::Server server{
            [&notifications](const auto& value) { notifications.push_back(value); }, {}, options};
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "initialized", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));
        server.wait_for_analysis();

        const hlsl_intellisense::json_rpc::Request request{
            .id = std::string{"inlay"},
            .method = "textDocument/inlayHint",
            .params = Json{{"textDocument", {{"uri", uri}}},
                           {"range",
                            {{"start", position_at(source, 0)},
                             {"end", position_at(source, source.size())}}}}};
        const auto cancellation = server.begin_request(request.id);
        auto response =
            std::async(std::launch::async, [&] { return server.handle(request, cancellation); });
        entered.get_future().wait();
        if (change_variant) {
            static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
                .method = "hlsl/didChangeActiveVariant",
                .params = Json{{"variant", "SupersedingVariant"}}}));
        } else {
            static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
                .method = "workspace/didChangeConfiguration",
                .params = Json{{"settings", {{"hlsl.inlayHints.parameters", false}}}}}));
        }
        release.set_value();

        const auto result = response.get();
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&result);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
        server.wait_for_analysis();
    };

    check_superseded(false);
    check_superseded(true);
}

TEST_CASE("Inlay invalidation and refresh survive failed dependent reanalysis",
          "[lsp][inlay-hints][refresh][failure][integration]") {
    TestDirectory directory;
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    const auto root_path = directory.path() / "root.hlsl";
    const auto include_path = directory.path() / "dependency.hlsli";
    {
        std::ofstream config{config_path};
        REQUIRE(config);
        config << R"({"root":true})";
    }
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float includeValue = 1.0;\n";
    }
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(root_path.string());
    const auto include =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    const std::string root_source =
        "#include \"dependency.hlsli\"\n"
        "float shade(float value) { return value; }\n"
        "float4 main() : SV_Target { return shade(includeValue).xxxx; }\n";

    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<hlsl_intellisense::json_rpc::Request> outbound_requests;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        options,
        [&outbound_requests](const auto& value) { outbound_requests.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params =
            Json{{"capabilities", {{"workspace", {{"inlayHint", {{"refreshSupport", true}}}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", root.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", root_source}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", include.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "static const float includeValue = 1.0;\n"}}}}}));
    server.wait_for_analysis();
    outbound_requests.clear();

    const hlsl_intellisense::json_rpc::Request hint_request{
        .id = std::string{"blocked-inlay"},
        .method = "textDocument/inlayHint",
        .params = Json{{"textDocument", {{"uri", root.uri()}}},
                       {"range",
                        {{"start", position_at(root_source, 0)},
                         {"end", position_at(root_source, root_source.size())}}}}};
    const auto cancellation = server.begin_request(hint_request.id);
    auto response =
        std::async(std::launch::async, [&] { return server.handle(hint_request, cancellation); });
    entered.get_future().wait();

    {
        std::ofstream config{config_path, std::ios::trunc};
        REQUIRE(config);
        config << "{ malformed";
    }
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{
            {"changes",
             Json::array({Json{
                 {"uri",
                  hlsl_intellisense::workspace::DocumentUri::from_path(config_path.string()).uri()},
                 {"type", 2}}})}}}));
    REQUIRE(outbound_requests.size() == 1);
    CHECK(std::ranges::none_of(notifications, [](const auto& notification) {
        return notification.method == "hlsl/configurationChanged";
    }));
    release.set_value();
    const auto superseded = response.get();
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&superseded);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    server.wait_for_analysis();
    CHECK(std::ranges::any_of(notifications, [](const auto& notification) {
        return notification.method == "hlsl/configurationChanged";
    }));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params =
            Json{{"textDocument", {{"uri", include.uri()}, {"version", 2}}},
                 {"contentChanges",
                  Json::array({Json{{"text", "static const float includeValue = 2.0;\n"}}})}}}));
    REQUIRE(outbound_requests.size() == 2);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didSave",
        .params = Json{{"textDocument", {{"uri", include.uri()}}}}}));
    REQUIRE(outbound_requests.size() == 3);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didClose",
        .params = Json{{"textDocument", {{"uri", include.uri()}}}}}));
    REQUIRE(outbound_requests.size() == 4);

    const auto other = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "other.hlsl").string());
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", other.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "float4 main() : SV_Target { return 1.0.xxxx; }\n"}}}}}));
    REQUIRE(outbound_requests.size() == 5);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeClientDefaults",
        .params = Json{{"hlsl", {{"languageVersion", "202x"}}}}}));
    REQUIRE(outbound_requests.size() == 6);
    CHECK(std::ranges::all_of(outbound_requests, [](const auto& request) {
        return request.method == "workspace/inlayHint/refresh";
    }));
}

TEST_CASE("References and rename preserve identity across open roots and disk includes",
          "[lsp][references][rename]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    const std::string include_text =
        "// \xC3\x97 \xF0\x9F\x98\x80\r\nstatic const float sharedValue = 1.0;\r\n";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << include_text;
    }
    const auto first = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "a.hlsl").string());
    const auto second = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "b.hlsl").string());
    const std::string first_text = "// \xC3\x97 \xF0\x9F\x98\x80\r\n"
                                   "#include \"shared.hlsli\"\r\n"
                                   "float4 main() : SV_Target { float sharedValue = 2.0; return "
                                   "(sharedValue + ::sharedValue).xxxx; }\r\n";
    const std::string second_text = "// \xE2\x86\x92\r\n"
                                    "#include \"shared.hlsli\"\r\n"
                                    "float4 main() : SV_Target { return sharedValue.xxxx; }\r\n";

    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {.semantic_tokens = true,
         .background_analysis = false,
         .request_worker_count = 4,
         .request_queue_capacity = 64,
         .analysis =
             {.scheduler = {.worker_count = 1, .queue_capacity = 8},
              .limits = {.max_translation_units = 1,
                         .max_translation_unit_estimated_bytes = std::size_t{64} * 1024U * 1024U,
                         .opaque_translation_unit_estimate = std::size_t{1024} * 1024U,
                         .include_cache = {.max_entries = 16,
                                           .max_estimated_bytes = std::size_t{1024} * 1024U}}},
         .analysis_hooks = {}}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] :
         std::array{std::pair{first.uri(), first_text}, std::pair{second.uri(), second_text}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }

    const auto selected = first_text.find("::sharedValue") + 3;
    const auto references = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/references",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, selected)},
                       {"context", {{"includeDeclaration", true}}}}});
    REQUIRE(references.has_value());
    const auto* reference_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*references);
    REQUIRE(reference_response != nullptr);
    REQUIRE(reference_response->result.size() == 3);

    const auto without_declaration = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/references",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, selected)},
                       {"context", {{"includeDeclaration", false}}}}});
    REQUIRE(without_declaration.has_value());
    const auto* without_declaration_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*without_declaration);
    REQUIRE(without_declaration_response != nullptr);
    CHECK(without_declaration_response->result.size() == 2);

    const auto prepare = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{4},
        .method = "textDocument/prepareRename",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, selected)}}});
    REQUIRE(prepare.has_value());
    const auto* prepare_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*prepare);
    REQUIRE(prepare_response != nullptr);
    CHECK(prepare_response->result["placeholder"] == "sharedValue");

    const auto rename = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{5},
        .method = "textDocument/rename",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, selected)},
                       {"newName", "renamedValue"}}});
    REQUIRE(rename.has_value());
    const auto* rename_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*rename);
    REQUIRE(rename_response != nullptr);
    const auto& changes = rename_response->result["documentChanges"];
    REQUIRE(changes.size() == 3);
    CHECK(std::ranges::count_if(changes, [](const auto& change) {
              return change["textDocument"]["version"].is_number_integer();
          }) == 2);
    CHECK(std::ranges::count_if(changes, [](const auto& change) {
              return change["textDocument"]["version"].is_null();
          }) == 1);
    CHECK(std::ranges::fold_left(changes, std::size_t{}, [](std::size_t count, const auto& change) {
              return count + change["edits"].size();
          }) == 3);

    const auto local = first_text.find("sharedValue = 2.0");
    const auto local_references = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{6},
        .method = "textDocument/references",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, local)},
                       {"context", {{"includeDeclaration", true}}}}});
    REQUIRE(local_references.has_value());
    const auto* local_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*local_references);
    REQUIRE(local_response != nullptr);
    CHECK(local_response->result.size() == 2);

    const auto invalid_rename = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{7},
        .method = "textDocument/rename",
        .params = Json{{"textDocument", {{"uri", first.uri()}}},
                       {"position", position_at(first_text, selected)},
                       {"newName", "float"}}});
    REQUIRE(invalid_rename.has_value());
    const auto* invalid_response =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*invalid_rename);
    REQUIRE(invalid_response != nullptr);
    CHECK(invalid_response->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
}

TEST_CASE("Rename rejects disk sources changed after analysis",
          "[lsp][references][rename][safety]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float sharedValue = 1.0;\n";
    }
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const std::string source =
        "#include \"shared.hlsli\"\nfloat4 main() : SV_Target { return sharedValue.xxxx; }\n";
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::atomic_int interactive_calls{};
    std::promise<void> reference_entered;
    std::promise<void> release_reference;
    auto released = release_reference.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        if (interactive_calls.fetch_add(1) + 1 == 2) {
            reference_entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{
            {"textDocument",
             {{"uri", root.uri()}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    auto response = std::async(std::launch::async, [&] {
        return server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{2},
            .method = "textDocument/rename",
            .params = Json{{"textDocument", {{"uri", root.uri()}}},
                           {"position", position_at(source, source.find("sharedValue"))},
                           {"newName", "renamedValue"}}});
    });
    reference_entered.get_future().wait();
    {
        std::ofstream include{include_path, std::ios::trunc};
        REQUIRE(include);
        include << "// externally changed\n";
    }
    release_reference.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
}

TEST_CASE("Server can disable semantic tokens for incompatible clients", "[lsp][navigation]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {.semantic_tokens = false,
         .background_analysis = false,
         .request_worker_count = 4,
         .request_queue_capacity = 64,
         .analysis = {},
         .analysis_hooks = {}}};

    const auto initialized = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()});
    REQUIRE(initialized.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*initialized);
    REQUIRE(response != nullptr);
    CHECK(response->result["serverInfo"]["version"] == "0.14.1");
    CHECK_FALSE(response->result["capabilities"].contains("semanticTokensProvider"));
    CHECK(response->result["capabilities"]["definitionProvider"] == true);
    CHECK(response->result["capabilities"]["referencesProvider"] == true);
    CHECK(response->result["capabilities"]["renameProvider"]["prepareProvider"] == true);
}

TEST_CASE("Framed LSP session publishes diagnostics and completes HLSL 2021",
          "[lsp][protocol][integration]") {
    const auto uri = shader_uri();
    const auto valid = valid_hlsl();
    auto invalid = valid;
    invalid.replace(invalid.find("sum.value.xxxx"), std::string_view{"sum.value.xxxx"}.size(),
                    "missing");

    std::string input;
    input += frame(request(1, "initialize"));
    input += frame(notification("initialized"));
    input += frame(notification(
        "textDocument/didOpen",
        {{"textDocument",
          {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", invalid}}}}));
    input += frame(notification(
        "textDocument/didChange",
        {{"textDocument", {{"uri", uri}, {"version", 2}}},
         {"contentChanges", Json::array({Json{{"range",
                                               {{"start", {{"line", 17}, {"character", 11}}},
                                                {"end", {{"line", 17}, {"character", 18}}}}},
                                              {"rangeLength", 7},
                                              {"text", "sum.value.xxxx"}}})}}));
    input += frame(
        notification("textDocument/didSave", {{"textDocument", {{"uri", uri}}}, {"text", valid}}));
    input += frame(request(
        2, "textDocument/completion",
        {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 17}, {"character", 4}}}}));
    input += frame(notification("textDocument/didClose", {{"textDocument", {{"uri", uri}}}}));
    input += frame(request_without_params(3, "shutdown"));
    input += frame(notification_without_params("exit"));

    std::istringstream input_stream{input};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    CHECK(hlsl_intellisense::lsp::run(input_stream, output_stream, error_stream) == 0);
    INFO(error_stream.str());
    CHECK(error_stream.str().empty());

    const auto messages = read_frames(output_stream.str());
    REQUIRE(messages.size() >= 3);
    const auto initialize_response = std::ranges::find_if(
        messages, [](const auto& message) { return message.value("id", Json{}) == 1; });
    REQUIRE(initialize_response != messages.end());
    CHECK((*initialize_response)["result"]["capabilities"]["positionEncoding"] == "utf-16");
    const auto completion_response = std::ranges::find_if(
        messages, [](const auto& message) { return message.value("id", Json{}) == 2; });
    REQUIRE(completion_response != messages.end());
    CHECK((completion_response->contains("result") || completion_response->contains("error")));
    const auto shutdown_response = std::ranges::find_if(
        messages, [](const auto& message) { return message.value("id", Json{}) == 3; });
    REQUIRE(shutdown_response != messages.end());
    CHECK((*shutdown_response)["result"].is_null());
    for (const auto& message : messages) {
        if (message.value("method", "") == "textDocument/publishDiagnostics" &&
            message["params"].contains("version")) {
            CHECK(message["params"]["version"].get<std::int64_t>() >= 2);
        }
    }
}

TEST_CASE("Protocol tracing redacts source text by default", "[lsp][protocol][trace]") {
    const auto uri = shader_uri();
    const std::string secret_source =
        "float4 privateSourceMarker() : SV_Target { return 1.0.xxxx; }\n";
    std::string input;
    input += frame(request(1, "initialize"));
    input += frame(notification("initialized"));
    input += frame(notification(
        "textDocument/didOpen",
        {{"textDocument",
          {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", secret_source}}}}));
    input += frame(request_without_params(2, "shutdown"));
    input += frame(notification_without_params("exit"));

    std::istringstream input_stream{input};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    hlsl_intellisense::lsp::ServerOptions options;
    options.protocol_trace = true;
    CHECK(hlsl_intellisense::lsp::run(input_stream, output_stream, error_stream, options) == 0);
    CHECK(error_stream.str().find("privateSourceMarker") == std::string::npos);
    CHECK(error_stream.str().find(uri) == std::string::npos);
    CHECK(error_stream.str().find("completionProvider") == std::string::npos);
    CHECK(error_stream.str().find("<redacted ") != std::string::npos);
    CHECK(error_stream.str().find("trace receive") != std::string::npos);
    CHECK(error_stream.str().find("trace send") != std::string::npos);
    CHECK_FALSE(read_frames(output_stream.str()).empty());
}

TEST_CASE("Server applies workspace configuration to DXC analysis",
          "[lsp][configuration][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.preprocessorDefinitions":{"CONFIGURED":1}})";
        REQUIRE(config);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#ifndef CONFIGURED\n#error missing configuration\n#endif\n"
                                  "float4 main() : SV_Target { return 1.0.xxxx; }\n"}}}}}));

    REQUIRE(notifications.size() == 1);
    CHECK(notifications.front().method == "textDocument/publishDiagnostics");
    CHECK((*notifications.front().params)["diagnostics"].empty());
}

TEST_CASE("Server publishes and clears generation-safe analysis unavailable diagnostics",
          "[lsp][analysis][worker][timeout][diagnostics]") {
    TestDirectory directory;
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "timeout.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::ServerOptions options;
    options.analysis.worker_executable = HLSL_TEST_WORKER_HELPER;
    options.analysis.budgets.background_timeout = std::chrono::milliseconds{250};
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }, {}, options};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "// HLSL_TEST_BACKGROUND_HANG\n"}}}}}));

    REQUIRE(notifications.size() == 1);
    REQUIRE(notifications.front().method == "textDocument/publishDiagnostics");
    const auto& unavailable = (*notifications.front().params)["diagnostics"];
    REQUIRE(unavailable.size() == 1);
    CHECK(unavailable[0]["code"] == "hlsl-lsp/analysis-unavailable");
    CHECK(unavailable[0]["source"] == "hlsl-lsp");
    CHECK(unavailable[0]["data"]["reason"] == "timedOut");
    CHECK(unavailable[0]["data"]["version"] == 1);

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{
            {"textDocument", {{"uri", document.uri()}, {"version", 2}}},
            {"contentChanges",
             Json::array({Json{{"text", "float4 main() : SV_Target { return 1.0.xxxx; }\n"}}})}}}));

    REQUIRE(notifications.size() == 2);
    REQUIRE(notifications.back().method == "textDocument/publishDiagnostics");
    CHECK((*notifications.back().params)["version"] == 2);
    CHECK((*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("LSP shutdown promptly terminates pathological background analysis",
          "[lsp][analysis][worker][shutdown]") {
    TestDirectory directory;
    const auto path = directory.path() / "shutdown.hlsl";
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(path.string());
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis.worker_executable = HLSL_TEST_WORKER_HELPER;
    options.analysis.budgets.background_timeout = std::chrono::seconds{10};
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "// HLSL_TEST_BACKGROUND_HANG\n"}}}}}));

    const auto entered = std::filesystem::path{path.string() + ".worker-entered"};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!std::filesystem::exists(entered) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(std::filesystem::exists(entered));

    const auto started = std::chrono::steady_clock::now();
    const auto result = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2}, .method = "shutdown", .params = std::nullopt});
    REQUIRE(result.has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{2});
    server.wait_for_analysis();
}

TEST_CASE("Server resolves virtual include mappings for DXC",
          "[lsp][configuration][includes][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "Engine");
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.virtualDirectoryMappings":{"/Engine":"Engine"}})";
        REQUIRE(config);
    }
    {
        std::ofstream include{directory.path() / "Engine" / "Common.hlsli"};
        REQUIRE(include);
        include << "static const float4 engineValue = 1.0.xxxx;\n";
        REQUIRE(include);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "virtual.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#include \"/Engine/Common.hlsli\"\n"
                                  "float4 main() : SV_Target { return engineValue; }\n"}}}}}));

    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.front().params)["diagnostics"].empty());

    const auto definition = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/definition",
        .params = Json{{"textDocument", {{"uri", document.uri()}}},
                       {"position", {{"line", 1}, {"character", 38}}}}});
    REQUIRE(definition.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*definition);
    REQUIRE(response != nullptr);
    const auto include_uri = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "Engine" / "Common.hlsli").string());
    CHECK(response->result["uri"] == include_uri.uri());
    CHECK(response->result["range"]["start"]["line"] == 0);
    CHECK(response->result["range"]["start"]["character"] == 20);
}

TEST_CASE("Configured macro includes resolve through virtual mappings end to end",
          "[lsp][configuration][includes][preprocessor][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "Test" / "STF" / "AssertionsV1");
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    {
        std::ofstream config{config_path};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.preprocessorDefinitions": {
                "STF_ASSERTIONS": "\"/Test/STF/AssertionsV1/Framework.hlsli\""
            },
            "hlsl.virtualDirectoryMappings": {"/Test": "Test"}
        })";
        REQUIRE(config);
    }
    {
        std::ofstream include{directory.path() / "Test" / "STF" / "AssertionsV1" /
                              "Framework.hlsli"};
        REQUIRE(include);
        include << "static const float frameworkValue = 1.0;\n";
        REQUIRE(include);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "configured-macro.hlsl").string());
    const std::string source = "#include STF_ASSERTIONS\n"
                               "#define LOCAL_FEATURE 1\n"
                               "#if 0\n"
                               "float skippedValue;\n"
                               "#endif\n"
                               "float4 main() : SV_Target { return frameworkValue.xxxx; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));

    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.front().params)["diagnostics"].empty());
    const auto include_uri = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "Test" / "STF" / "AssertionsV1" / "Framework.hlsli").string());

    const auto include_definition = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/definition",
        .params = Json{{"textDocument", {{"uri", document.uri()}}},
                       {"position", position_at(source, source.find("STF_ASSERTIONS") + 3)}}});
    REQUIRE(include_definition.has_value());
    const auto* include_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*include_definition);
    REQUIRE(include_response != nullptr);
    CHECK(include_response->result["uri"] == include_uri.uri());

    const auto symbol_definition = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "textDocument/definition",
        .params = Json{{"textDocument", {{"uri", document.uri()}}},
                       {"position", position_at(source, source.find("frameworkValue") + 3)}}});
    REQUIRE(symbol_definition.has_value());
    const auto* symbol_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*symbol_definition);
    REQUIRE(symbol_response != nullptr);
    CHECK(symbol_response->result["uri"] == include_uri.uri());
    CHECK(symbol_response->result["range"]["start"]["line"] == 0);
    CHECK(symbol_response->result["range"]["start"]["character"] == 19);

    const auto explorer = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{4},
        .method = "hlsl/preprocessorExplorer",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(explorer.has_value());
    const auto* explorer_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*explorer);
    REQUIRE(explorer_response != nullptr);
    REQUIRE(explorer_response->result["files"][0]["includes"].size() == 1);
    const auto& include = explorer_response->result["files"][0]["includes"][0];
    CHECK(include["path"] == "STF_ASSERTIONS");
    CHECK(include["kind"] == "macro");
    CHECK(include["status"] == "resolved");
    CHECK(include["expandedPath"] == "/Test/STF/AssertionsV1/Framework.hlsli");
    CHECK(include["resolvedUri"] == include_uri.uri());
    CHECK(include["mapping"] == "/Test");
    CHECK(include["configurationMacro"] == "STF_ASSERTIONS");
    CHECK(include["configurationOrigin"] == config_path.generic_string());
    CHECK(include["configurationOriginUri"] ==
          hlsl_intellisense::workspace::DocumentUri::from_path(config_path.string()).uri());
#ifdef _WIN32
    CHECK(explorer_response->result["compilerAnalysis"]["skippedRegions"]["available"] == true);
    CHECK_FALSE(explorer_response->result["skippedRegions"].empty());
    CHECK(explorer_response->result["diagnostics"].empty());
#else
    CHECK(explorer_response->result["compilerAnalysis"]["skippedRegions"]["available"] == false);
    CHECK(explorer_response->result["skippedRegions"].empty());
    REQUIRE(explorer_response->result["compilerAnalysis"]["skippedRegions"].contains("reason"));
    CHECK(explorer_response->result["compilerAnalysis"]["skippedRegions"]["reason"]
              .template get<std::string>()
              .find("DXC 1.9") != std::string::npos);
    CHECK(std::ranges::any_of(explorer_response->result["diagnostics"], [](const auto& diagnostic) {
        return diagnostic.template get<std::string>().find("GetSkippedRanges") != std::string::npos;
    }));
#endif
    CHECK(explorer_response->result["compilerAnalysis"]["compilerMacros"]["available"] == true);
    CHECK(std::ranges::any_of(explorer_response->result["macros"], [](const auto& macro) {
        return macro["name"] == "LOCAL_FEATURE" && macro["source"] == "compiler";
    }));
    CHECK(std::ranges::any_of(explorer_response->result["macros"], [](const auto& macro) {
        return macro["name"] == "STF_ASSERTIONS" && macro["source"] == "configuration";
    }));

    const std::string edited = "#include STF_ASSERTIONS\n"
                               "#define LOCAL_FEATURE 1\n"
                               "#if 0\n"
                               "float skippedValue;\n"
                               "#endif\n"
                               "float4 main() : SV_Target { return missingValue.xxxx; }\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", document.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", edited}}})}}}));
    REQUIRE(notifications.size() == 2);
    const auto& diagnostics = (*notifications.back().params)["diagnostics"];
    const auto missing = std::ranges::find_if(diagnostics, [](const auto& diagnostic) {
        return diagnostic["message"].template get<std::string>().find("missingValue") !=
               std::string::npos;
    });
    REQUIRE(missing != diagnostics.end());
    CHECK((*missing)["range"]["start"] == position_at(edited, edited.find("missingValue")));
}

TEST_CASE("F12 on include paths opens quoted and search-path headers",
          "[lsp][navigation][includes][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "includes");
    const auto local_path = directory.path() / "local.hlsli";
    const auto shared_path = directory.path() / "includes" / "shared.hlsli";
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.additionalIncludeDirectories":["includes"]})";
        REQUIRE(config);
    }
    for (const auto& path : {local_path, shared_path}) {
        std::ofstream include{path};
        REQUIRE(include);
        include << "float includeValue;\n";
        REQUIRE(include);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const auto source = std::string{"#include \"local.hlsli\"\n"
                                    "#include <shared.hlsli>\n"
                                    "float4 main() : SV_Target { return 1.0.xxxx; }\n"};
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));

    const auto definition_at = [&server, &document](std::int64_t id, std::uint32_t line,
                                                    std::uint32_t character) {
        const auto result = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = id,
            .method = "textDocument/definition",
            .params = Json{{"textDocument", {{"uri", document.uri()}}},
                           {"position", {{"line", line}, {"character", character}}}}});
        REQUIRE(result.has_value());
        const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
        REQUIRE(response != nullptr);
        return response->result;
    };

    const auto local = definition_at(2, 0, 12);
    CHECK(local["uri"] ==
          hlsl_intellisense::workspace::DocumentUri::from_path(local_path.string()).uri());
    CHECK(local["range"]["start"] == Json{{"line", 0}, {"character", 0}});
    const auto shared = definition_at(3, 1, 13);
    CHECK(shared["uri"] ==
          hlsl_intellisense::workspace::DocumentUri::from_path(shared_path.string()).uri());
    CHECK(shared["range"]["start"] == Json{{"line", 0}, {"character", 0}});
}

TEST_CASE("Editing an open include reanalyzes dependent root shaders",
          "[lsp][includes][integration]") {
    TestDirectory directory;
    const auto root_path = directory.path() / "root.hlsl";
    const auto include_path = directory.path() / "dependency.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float4 includeValue = 1.0.xxxx;\n";
        REQUIRE(include);
    }

    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(root_path.string());
    const auto include =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<hlsl_intellisense::json_rpc::Request> outbound_requests;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {},
        [&outbound_requests](const auto& value) { outbound_requests.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params =
            Json{{"capabilities", {{"workspace", {{"inlayHint", {{"refreshSupport", true}}}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", root.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#include \"dependency.hlsli\"\n"
                                  "float4 main() : SV_Target { return includeValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", include.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "static const float4 otherValue = 1.0.xxxx;\n"}}}}}));
    REQUIRE(notifications.size() == 3);
    CHECK((*notifications.back().params)["uri"] == root.uri());
    CHECK(!(*notifications.back().params)["diagnostics"].empty());
    REQUIRE(outbound_requests.size() == 2);
    outbound_requests.clear();

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{
            {"textDocument", {{"uri", include.uri()}, {"version", 2}}},
            {"contentChanges",
             Json::array({Json{{"text", "static const float4 includeValue = 2.0.xxxx;\n"}}})}}}));
    REQUIRE(notifications.size() == 5);
    CHECK((*notifications.back().params)["uri"] == root.uri());
    CHECK((*notifications.back().params)["version"] == 1);
    INFO((*notifications.back().params)["diagnostics"].dump());
    CHECK((*notifications.back().params)["diagnostics"].empty());
    REQUIRE(outbound_requests.size() == 1);
    CHECK(outbound_requests.front().method == "workspace/inlayHint/refresh");
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didSave",
        .params = Json{{"textDocument", {{"uri", include.uri()}}},
                       {"text", "static const float4 includeValue = 2.0.xxxx;\n"}}}));
    REQUIRE(outbound_requests.size() == 2);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didClose",
        .params = Json{{"textDocument", {{"uri", include.uri()}}}}}));
    REQUIRE(outbound_requests.size() == 3);
    CHECK(std::ranges::all_of(outbound_requests, [](const auto& request) {
        return request.method == "workspace/inlayHint/refresh";
    }));
}

TEST_CASE("Configuration change notifications reload open shaders",
          "[lsp][configuration][integration]") {
    TestDirectory directory;
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    {
        std::ofstream config{config_path};
        REQUIRE(config);
        config << R"({"root":true})";
        REQUIRE(config);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#ifndef CONFIGURED\n#error missing configuration\n#endif\n"
                                  "float4 main() : SV_Target { return 1.0.xxxx; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    {
        std::ofstream config{config_path, std::ios::trunc};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.preprocessorDefinitions":{"CONFIGURED":1}})";
        REQUIRE(config);
    }
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "workspace/didChangeConfiguration",
                                                  .params = Json{{"settings", Json::object()}}}));

    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Watched file-group changes reanalyze matching open shaders",
          "[lsp][configuration][file-groups][integration]") {
    TestDirectory directory;
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    {
        std::ofstream config{config_path};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.fileGroups": [{
                "files": ["pixel-*.hlsl"],
                "hlsl.preprocessorDefinitions": {"CONFIGURED": 1}
            }]
        })";
        REQUIRE(config);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "compute-main.hlsl").string());
    const auto config = hlsl_intellisense::workspace::DocumentUri::from_path(config_path.string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#ifndef CONFIGURED\n#error missing configuration\n#endif\n"
                                  "[numthreads(1, 1, 1)] void main() {}\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    {
        std::ofstream changed{config_path, std::ios::trunc};
        REQUIRE(changed);
        changed << R"({
            "root": true,
            "hlsl.fileGroups": [{
                "files": ["compute-*.hlsl"],
                "hlsl.preprocessorDefinitions": {"CONFIGURED": 1}
            }]
        })";
        REQUIRE(changed);
    }
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", config.uri()}, {"type", 2}}})}}}));
    server.wait_for_analysis();

    REQUIRE(notifications.size() == 3);
    CHECK(notifications[1].method == "textDocument/publishDiagnostics");
    CHECK((*notifications[1].params)["uri"] == document.uri());
    CHECK((*notifications[1].params)["diagnostics"].empty());
    CHECK(notifications.back().method == "hlsl/configurationChanged");
    CHECK((*notifications.back().params)["uris"] == Json::array({config.uri()}));

    const auto parse_count = server.analysis_metrics().parse_count;
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", config.uri()}, {"type", 2}}})}}}));
    server.wait_for_analysis();
    CHECK(server.analysis_metrics().parse_count == parse_count);
    CHECK(notifications.size() == 3);
}

TEST_CASE("Creating and deleting watched configuration updates open shaders",
          "[lsp][configuration][watch][integration]") {
    TestDirectory directory;
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    const auto config = hlsl_intellisense::workspace::DocumentUri::from_path(config_path.string());
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#ifndef CONFIGURED\n#error missing configuration\n#endif\n"
                                  "float4 main() : SV_Target { return 1.0.xxxx; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK_FALSE((*notifications.back().params)["diagnostics"].empty());

    {
        std::ofstream created{config_path};
        REQUIRE(created);
        created << R"({"root":true,"hlsl.preprocessorDefinitions":{"CONFIGURED":1}})";
    }
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", config.uri()}, {"type", 1}}})}}}));
    server.wait_for_analysis();

    REQUIRE(notifications.size() == 3);
    CHECK(notifications[1].method == "textDocument/publishDiagnostics");
    CHECK((*notifications[1].params)["diagnostics"].empty());
    CHECK(notifications[2].method == "hlsl/configurationChanged");

    REQUIRE(std::filesystem::remove(config_path));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", config.uri()}, {"type", 3}}})}}}));
    server.wait_for_analysis();

    REQUIRE(notifications.size() == 5);
    CHECK(notifications[3].method == "textDocument/publishDiagnostics");
    CHECK_FALSE((*notifications[3].params)["diagnostics"].empty());
    CHECK(notifications[4].method == "hlsl/configurationChanged");
}

TEST_CASE("Closing an affected shader does not lose configuration completion",
          "[lsp][configuration][watch][cancellation]") {
    TestDirectory directory;
    const auto config_path = directory.path() / "shadertoolsconfig.json";
    {
        std::ofstream config{config_path};
        REQUIRE(config);
        config << R"({"root":true})";
    }
    const auto config = hlsl_intellisense::workspace::DocumentUri::from_path(config_path.string());
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "closing.hlsl").string());

    std::atomic_bool block_next_analysis{};
    std::promise<void> entered;
    std::promise<void> release;
    const auto released = release.get_future().share();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    hooks->before_analysis = [&](std::string_view, std::int64_t) {
        if (block_next_analysis.exchange(false)) {
            entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", valid_hlsl()}}}}}));
    server.wait_for_analysis();

    {
        std::ofstream changed{config_path, std::ios::trunc};
        REQUIRE(changed);
        changed << R"({"root":true,"hlsl.preprocessorDefinitions":{"CHANGED":1}})";
    }
    block_next_analysis.store(true);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", config.uri()}, {"type", 2}}})}}}));
    entered.get_future().wait();

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didClose",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}}));
    CHECK(std::ranges::any_of(notifications, [](const auto& notification) {
        return notification.method == "hlsl/configurationChanged";
    }));

    release.set_value();
    server.wait_for_analysis();
}

TEST_CASE("Typed editor settings override files and resolve from the workspace",
          "[lsp][configuration][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "shaders");
    std::filesystem::create_directories(directory.path() / "includes");
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.preprocessorDefinitions":{"FILE_SETTING":1}})";
        REQUIRE(config);
    }
    {
        std::ofstream include{directory.path() / "includes" / "Editor.hlsli"};
        REQUIRE(include);
        include << "static const float4 editorValue = 1.0.xxxx;\n";
        REQUIRE(include);
    }

    const auto workspace =
        hlsl_intellisense::workspace::DocumentUri::from_path(directory.path().string());
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shaders" / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<std::string> logs;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        [&logs](std::string_view message) { logs.emplace_back(message); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"workspaceFolders",
                        Json::array({Json{{"uri", workspace.uri()}, {"name", "workspace"}}})}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#include EDITOR_HEADER\n"
                                  "#ifndef EDITOR_SETTING\n#error missing editor setting\n#endif\n"
                                  "float4 main() : SV_Target { return editorValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings",
                        {{"hlsl",
                          {{"preprocessorDefinitions",
                            {{"EDITOR_SETTING", 1}, {"EDITOR_HEADER", "\"Editor.hlsli\""}}},
                           {"additionalIncludeDirectories", Json::array({"includes"})},
                           {"languageVersion", "2021"}}}}}}}));
    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["diagnostics"].empty());

    const auto explorer = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/preprocessorExplorer",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(explorer.has_value());
    const auto* explorer_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*explorer);
    REQUIRE(explorer_response != nullptr);
    const auto& configured_include = explorer_response->result["files"][0]["includes"][0];
    CHECK(configured_include["status"] == "resolved");
    CHECK(configured_include["expandedPath"] == "Editor.hlsli");
    CHECK(configured_include["configurationOrigin"] == "editor settings");
    CHECK_FALSE(configured_include.contains("configurationOriginUri"));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings", {{"hlsl", {{"preprocessorDefinitions", Json::array()}}}}}}}));
    CHECK(notifications.size() == 2);
    REQUIRE(logs.size() == 1);
    CHECK(logs.back().find("preprocessorDefinitions") != std::string::npos);

    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "workspace/didChangeConfiguration",
                                                  .params = Json{{"settings", Json::object()}}}));
    REQUIRE(notifications.size() == 3);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Invalid workspace folder changes are atomic", "[lsp][workspace]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    std::vector<std::string> logs;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        [&logs](std::string_view message) { logs.emplace_back(message); }};

    const auto failed_initialize = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"workspaceFolders",
                        Json::array({Json{{"uri", workspace_uri()}, {"name", "valid"}},
                                     Json{{"uri", "https://invalid"}, {"name", "invalid"}}})}}});
    REQUIRE(failed_initialize.has_value());
    CHECK(std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*failed_initialize) != nullptr);

    const auto successful_initialize = server.handle(
        hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                             .method = "initialize",
                                             .params = Json{{"workspaceFolders", Json::array()}}});
    REQUIRE(successful_initialize.has_value());
    CHECK(std::get_if<hlsl_intellisense::json_rpc::Response>(&*successful_initialize) != nullptr);
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWorkspaceFolders",
        .params = Json{
            {"event",
             {{"removed", Json::array({Json{{"uri", workspace_uri()}, {"name", "not-present"}}})},
              {"added", Json::array({Json{{"uri", "https://invalid"}, {"name", "invalid"}}})}}}}}));
    REQUIRE(logs.size() == 1);
    CHECK(logs.back().find("file URI") != std::string::npos);
}

TEST_CASE("Client language defaults remain below shader-tools configuration",
          "[lsp][configuration][integration]") {
    TestDirectory directory;
    const auto configured_directory = directory.path() / "configured";
    std::filesystem::create_directories(configured_directory);
    {
        std::ofstream config{configured_directory / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.languageVersion":"2021"})";
        REQUIRE(config);
    }

    const auto default_document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "default.hlsl").string());
    const auto configured_document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (configured_directory / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"initializationOptions", {{"hlsl", {{"languageVersion", "2018"}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));

    const auto open_document = [&server](const auto& uri) {
        static_cast<void>(server.handle(
            hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                      .params = Json{{"textDocument",
                                                                      {{"uri", uri.uri()},
                                                                       {"languageId", "hlsl"},
                                                                       {"version", 1},
                                                                       {"text", valid_hlsl()}}}}}));
    };

    open_document(default_document);
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    open_document(configured_document);
    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeClientDefaults",
        .params = Json{{"hlsl", {{"languageVersion", "2021"}}}}}));
    // Both open documents are reanalyzed by reanalyze_all(): default_document
    // has a real configuration change (client default 2018 -> 2021) while
    // configured_document's effective configuration is unaffected (it has its
    // own root config), so its reanalysis resolves to an identical cache key.
    // default_document's diagnostics genuinely differ (2021 no longer flags
    // the same issue), so it republishes; configured_document's reanalysis
    // is a same-version, same-diagnostics cache hit (only its cached analysis
    // generation advances internally, which textDocument/codeAction depends
    // on staying current, covered separately), so it is not republished.
    REQUIRE(notifications.size() == 3);
    CHECK((*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Opening a previously missing include invalidates dependent roots",
          "[lsp][includes][integration]") {
    TestDirectory directory;
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const auto include = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "missing.hlsli").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", root.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#include \"missing.hlsli\"\n"
                                  "float4 main() : SV_Target { return includeValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", include.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "static const float4 includeValue = 1.0.xxxx;\n"}}}}}));

    REQUIRE(notifications.size() == 3);
    CHECK((*notifications.back().params)["uri"] == root.uri());
    CHECK((*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Watched disk include changes invalidate dependent roots",
          "[lsp][includes][integration]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "dependency.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float4 includeValue = 1.0.xxxx;\n";
        REQUIRE(include);
    }
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const auto include =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", root.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#include \"dependency.hlsli\"\n"
                                  "float4 main() : SV_Target { return includeValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.back().params)["diagnostics"].empty());

    {
        std::ofstream changed{include_path, std::ios::trunc};
        REQUIRE(changed);
        changed << "static const float4 otherValue = 1.0.xxxx;\n";
        REQUIRE(changed);
    }
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params = Json{{"changes", Json::array({Json{{"uri", include.uri()}, {"type", 2}}})}}}));

    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["uri"] == root.uri());
    CHECK(!(*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Macro includes use open buffers and conservative invalidation",
          "[lsp][includes][integration]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "dependency.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float4 includeValue = 1.0.xxxx;\n";
        REQUIRE(include);
    }
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const auto include =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", root.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#define HEADER \"dependency.hlsli\"\n#include HEADER\n"
                                  "float4 main() : SV_Target { return includeValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", include.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "static const float4 otherValue = 1.0.xxxx;\n"}}}}}));

    REQUIRE(notifications.size() == 3);
    CHECK((*notifications.back().params)["uri"] == root.uri());
    CHECK(!(*notifications.back().params)["diagnostics"].empty());
}

TEST_CASE("Server cancellation returns RequestCancelled while interactive work is active",
          "[lsp][cancellation][concurrency]") {
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }, {}, options};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{
            {"textDocument",
             {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", valid_hlsl()}}}}}));
    server.wait_for_analysis();

    const hlsl_intellisense::json_rpc::Request request{
        .id = std::string{"hover"},
        .method = "textDocument/hover",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"position", {{"line", 17}, {"character", 12}}}}};
    const auto cancellation = server.begin_request(request.id);
    auto response =
        std::async(std::launch::async, [&] { return server.handle(request, cancellation); });
    entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "$/cancelRequest", .params = Json{{"id", "hover"}}}));

    const auto result = response.get();
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);
    release.set_value();
    server.wait_for_analysis();
}

TEST_CASE("Superseded background analysis never publishes stale diagnostics",
          "[lsp][diagnostics][scheduling]") {
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> first_entered;
    std::promise<void> release_first;
    auto released = release_first.get_future().share();
    hooks->before_analysis = [&](std::string_view, std::int64_t version) {
        if (version == 1) {
            first_entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));

    auto invalid = valid_hlsl();
    invalid.replace(invalid.find("sum.value.xxxx"), std::string_view{"sum.value.xxxx"}.size(),
                    "missing");
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", invalid}}}}}));
    first_entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", valid_hlsl()}}})}}}));
    release_first.set_value();
    server.wait_for_analysis();

    REQUIRE(notifications.size() == 1);
    CHECK((*notifications.front().params)["version"] == 2);
    CHECK((*notifications.front().params)["diagnostics"].empty());
    const auto metrics = server.analysis_metrics();
    CHECK(metrics.parse_count == 1);
    CHECK(metrics.scheduler.cancelled >= 1);
}

TEST_CASE("Framed requests cancelled before execution return LSP RequestCancelled",
          "[lsp][protocol][cancellation]") {
    const auto uri = shader_uri();
    std::string input;
    input += frame(request(1, "initialize"));
    input += frame(notification("initialized"));
    input += frame(notification(
        "textDocument/didOpen",
        {{"textDocument",
          {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", valid_hlsl()}}}}));
    input += frame(request(
        2, "textDocument/completion",
        {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 17}, {"character", 4}}}}));
    input += frame(notification("$/cancelRequest", {{"id", 2}}));
    input += frame(request_without_params(3, "shutdown"));
    input += frame(notification_without_params("exit"));

    std::istringstream input_stream{input};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    CHECK(hlsl_intellisense::lsp::run(input_stream, output_stream, error_stream) == 0);
    INFO(error_stream.str());
    CHECK(error_stream.str().empty());
    const auto messages = read_frames(output_stream.str());
    const auto cancelled = std::ranges::find_if(
        messages, [](const auto& message) { return message.value("id", Json{}) == 2; });
    REQUIRE(cancelled != messages.end());
    CHECK((*cancelled)["error"]["code"] == hlsl_intellisense::json_rpc::request_cancelled_code);
}

TEST_CASE("Configuration file invalidation reparses only roots in its hierarchy",
          "[lsp][configuration][scheduling]") {
    TestDirectory directory;
    const auto first_directory = directory.path() / "first";
    const auto second_directory = directory.path() / "second";
    std::filesystem::create_directories(first_directory);
    std::filesystem::create_directories(second_directory);
    const auto configuration_path = first_directory / "shadertoolsconfig.json";
    {
        std::ofstream configuration{configuration_path};
        REQUIRE(configuration);
        configuration << R"({"root":true,"hlsl.languageVersion":"2021"})";
    }
    const auto first = hlsl_intellisense::workspace::DocumentUri::from_path(
        (first_directory / "first.hlsl").string());
    const auto second = hlsl_intellisense::workspace::DocumentUri::from_path(
        (second_directory / "second.hlsl").string());
    const auto configuration =
        hlsl_intellisense::workspace::DocumentUri::from_path(configuration_path.string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& document : {first, second}) {
        static_cast<void>(server.handle(
            hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                      .params = Json{{"textDocument",
                                                                      {{"uri", document.uri()},
                                                                       {"languageId", "hlsl"},
                                                                       {"version", 1},
                                                                       {"text", valid_hlsl()}}}}}));
    }
    REQUIRE(server.analysis_metrics().parse_count == 2);
    REQUIRE(notifications.size() == 2);

    {
        std::ofstream changed_configuration{configuration_path, std::ios::trunc};
        REQUIRE(changed_configuration);
        changed_configuration << R"({"root":true,"hlsl.languageVersion":"2018"})";
    }
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeWatchedFiles",
        .params =
            Json{{"changes", Json::array({Json{{"uri", configuration.uri()}, {"type", 2}}})}}}));
    server.wait_for_analysis();

    CHECK(server.analysis_metrics().parse_count == 3);
    REQUIRE(notifications.size() == 4);
    CHECK(notifications[2].method == "textDocument/publishDiagnostics");
    CHECK((*notifications[2].params)["uri"] == first.uri());
    CHECK(notifications.back().method == "hlsl/configurationChanged");
    CHECK((*notifications.back().params)["uris"] == Json::array({configuration.uri()}));
}

namespace {

[[nodiscard]] std::string runtime_json_path(std::string value) {
    std::ranges::replace(value, '\\', '/');
    return value;
}

[[nodiscard]] std::size_t
count_method(const std::vector<hlsl_intellisense::json_rpc::Notification>& items,
             std::string_view method) {
    std::size_t total{};
    for (const auto& item : items) {
        if (item.method == method) {
            ++total;
        }
    }
    return total;
}

[[nodiscard]] const hlsl_intellisense::json_rpc::Notification*
find_last(const std::vector<hlsl_intellisense::json_rpc::Notification>& items,
          std::string_view method) {
    const hlsl_intellisense::json_rpc::Notification* found{};
    for (const auto& item : items) {
        if (item.method == method) {
            found = &item;
        }
    }
    return found;
}

} // namespace

TEST_CASE("Server requests a controlled restart for a shadertoolsconfig DXC runtime",
          "[lsp][runtime][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.dxcRuntimeDirectory":")"
               << runtime_json_path(HLSL_TEST_DXC_RUNTIME_DIR) << R"("})";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", valid_hlsl()}}}}}));

    REQUIRE(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 1);
    const auto* restart = find_last(notifications, "hlsl/dxcRuntimeRestartRequired");
    REQUIRE(restart != nullptr);
    const auto restart_directory = (*restart->params)["directory"].get<std::string>();
    CHECK_FALSE(restart_directory.empty());
    CHECK_NOTHROW(hlsl_intellisense::dxc::validate_runtime_directory(restart_directory));

    const auto runtime = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2}, .method = "hlsl/dxcRuntime", .params = Json::object()});
    REQUIRE(runtime.has_value());
    const auto* runtime_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*runtime);
    REQUIRE(runtime_response != nullptr);
    CHECK(runtime_response->result["source"] == "bundled");
    CHECK(runtime_response->result["requiresRestart"] == true);
    CHECK_FALSE(runtime_response->result["version"].get<std::string>().empty());

    // A repeat evaluation of the same selection must not request another restart.
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "workspace/didChangeConfiguration",
                                                  .params = Json{{"settings", Json::object()}}}));
    CHECK(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 1);
}

TEST_CASE("Server reports an invalid shadertoolsconfig DXC runtime without restarting",
          "[lsp][runtime][integration]") {
    TestDirectory directory;
    const auto missing = directory.path() / "no-such-runtime";
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.dxcRuntimeDirectory":")"
               << runtime_json_path(missing.string()) << R"("})";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", valid_hlsl()}}}}}));

    CHECK(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 0);
    const auto* message = find_last(notifications, "window/showMessage");
    REQUIRE(message != nullptr);
    CHECK((*message->params)["type"] == 1);

    // The same invalid selection must not be reported repeatedly.
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "workspace/didChangeConfiguration",
                                                  .params = Json{{"settings", Json::object()}}}));
    CHECK(count_method(notifications, "window/showMessage") == 1);
    CHECK(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 0);
}

TEST_CASE("Server reevaluates DXC runtime conflicts when a document closes",
          "[lsp][runtime][integration]") {
    TestDirectory bundled_directory;
    TestDirectory configured_directory;
    {
        std::ofstream config{configured_directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({"root":true,"hlsl.dxcRuntimeDirectory":")"
               << runtime_json_path(HLSL_TEST_DXC_RUNTIME_DIR) << R"("})";
        REQUIRE(config);
    }
    const auto bundled_document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (bundled_directory.path() / "bundled.hlsl").string());
    const auto configured_document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (configured_directory.path() / "configured.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& document : {bundled_document, configured_document}) {
        static_cast<void>(server.handle(
            hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                      .params = Json{{"textDocument",
                                                                      {{"uri", document.uri()},
                                                                       {"languageId", "hlsl"},
                                                                       {"version", 1},
                                                                       {"text", valid_hlsl()}}}}}));
    }
    CHECK(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 0);

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didClose",
        .params = Json{{"textDocument", {{"uri", bundled_document.uri()}}}}}));

    REQUIRE(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 1);
    const auto* restart = find_last(notifications, "hlsl/dxcRuntimeRestartRequired");
    REQUIRE(restart != nullptr);
    CHECK(std::filesystem::equivalent((*restart->params)["directory"].get<std::string>(),
                                      HLSL_TEST_DXC_RUNTIME_DIR));
}

TEST_CASE("Server reports the bundled DXC runtime through hlsl/dxcRuntime",
          "[lsp][runtime][integration]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));

    const auto runtime = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2}, .method = "hlsl/dxcRuntime", .params = Json::object()});
    REQUIRE(runtime.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*runtime);
    REQUIRE(response != nullptr);
    CHECK(response->result["source"] == "bundled");
    CHECK(response->result["requiresRestart"] == false);
    CHECK_FALSE(response->result["version"].get<std::string>().empty());
    CHECK_FALSE(response->result["libraryPath"].get<std::string>().empty());
}

namespace {

void write_variant_config(const TestDirectory& directory) {
    std::ofstream config{directory.path() / "shadertoolsconfig.json"};
    REQUIRE(config);
    config << R"({
        "root": true,
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [
            { "name": "Alpha", "description": "Alpha permutation",
              "hlsl.preprocessorDefinitions": { "VARIANT_ALPHA": 1 } },
            { "name": "Beta",
              "hlsl.preprocessorDefinitions": { "VARIANT_BETA": 1 } }
        ]
    })";
    REQUIRE(config);
}

[[nodiscard]] std::string variant_shader() {
    // Each branch references a distinct undeclared identifier, so DXC reports a
    // recoverable "undeclared identifier" diagnostic that names the active
    // variant's macro. Unlike a fatal #error combined with an entry point, this
    // keeps the IntelliSense translation unit analyzable.
    return "float4 Main() : SV_Target {\n"
           "#if defined(VARIANT_ALPHA)\n    return marker_alpha;\n"
           "#elif defined(VARIANT_BETA)\n    return marker_beta;\n"
           "#else\n    return marker_none;\n#endif\n"
           "}\n";
}

[[nodiscard]] std::vector<std::string>
last_diagnostics(const std::vector<hlsl_intellisense::json_rpc::Notification>& items,
                 std::string_view uri) {
    std::vector<std::string> messages;
    for (const auto& item : items) {
        if (item.method == "textDocument/publishDiagnostics" && item.params &&
            item.params->value("uri", std::string{}) == uri) {
            messages.clear();
            for (const auto& diagnostic : item.params->value("diagnostics", Json::array())) {
                messages.push_back(diagnostic.value("message", std::string{}));
            }
        }
    }
    return messages;
}

[[nodiscard]] bool mentions(const std::vector<std::string>& messages, std::string_view marker) {
    return std::ranges::any_of(messages, [marker](const std::string& message) {
        return message.find(marker) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Server applies and reanalyzes shader variants on an unsaved buffer",
          "[lsp][variants][integration]") {
    TestDirectory directory;
    write_variant_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "variant.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", variant_shader()}}}}}));
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_none"));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Alpha"}}}));
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_alpha"));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Beta"}}}));
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_beta"));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", Json()}}}));
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_none"));
}

TEST_CASE("Server honors an initial active variant from initializationOptions",
          "[lsp][variants][integration]") {
    TestDirectory directory;
    write_variant_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "variant.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1},
        .method = "initialize",
        .params = Json{{"initializationOptions", {{"hlsl", {{"activeVariant", "Alpha"}}}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", variant_shader()}}}}}));
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_alpha"));
}

TEST_CASE("Configured macro include targets follow the active variant",
          "[lsp][variants][includes][preprocessor][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.additionalIncludeDirectories": ["."],
            "hlsl.variantsVersion": 1,
            "hlsl.variants": [
                {
                    "name": "Alpha",
                    "hlsl.preprocessorDefinitions": {
                        "VARIANT_HEADER": "\"Alpha.hlsli\""
                    }
                },
                {
                    "name": "Beta",
                    "hlsl.preprocessorDefinitions": {
                        "VARIANT_HEADER": "<Beta.hlsli>"
                    }
                }
            ]
        })";
        REQUIRE(config);
    }
    {
        std::ofstream alpha{directory.path() / "Alpha.hlsli"};
        REQUIRE(alpha);
        alpha << "static const float4 variantValue = 1.0.xxxx;\n";
        REQUIRE(alpha);
        std::ofstream beta{directory.path() / "Beta.hlsli"};
        REQUIRE(beta);
        beta << "static const float4 variantValue = 2.0.xxxx;\n";
        REQUIRE(beta);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "variant-include.hlsl").string());
    const std::string source = "#include /* \xCF\x80 configured */ VARIANT_HEADER\n"
                               "float4 main() : SV_Target { return variantValue; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));
    CHECK_FALSE(last_diagnostics(notifications, document.uri()).empty());

    const auto check_variant = [&](std::string_view name, std::string_view header,
                                   std::int64_t request_id) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "hlsl/didChangeActiveVariant",
            .params = Json{{"variant", std::string{name}}}}));
        CHECK(last_diagnostics(notifications, document.uri()).empty());
        const auto explorer = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = request_id,
            .method = "hlsl/preprocessorExplorer",
            .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
        REQUIRE(explorer.has_value());
        const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*explorer);
        REQUIRE(response != nullptr);
        const auto& include = response->result["files"][0]["includes"][0];
        CHECK(include["status"] == "resolved");
        CHECK(include["expandedPath"] == std::string{header});
        CHECK(include["configurationOrigin"] == "variant " + std::string{name});
        CHECK(include["line"] == 0);
        CHECK(include["character"] ==
              hlsl_intellisense::workspace::lsp_position_at(source, source.find("VARIANT_HEADER"))
                  .character);
    };

    check_variant("Alpha", "Alpha.hlsli", 2);
    check_variant("Beta", "Beta.hlsli", 3);
}

TEST_CASE("Later DXC macro arguments keep configured virtual includes compiler-owned",
          "[lsp][configuration][includes][preprocessor][arguments][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "Configured");
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.targetProfile": "ps_6_0",
            "hlsl.entryPoint": "main",
            "hlsl.preprocessorDefinitions": {
                "HEADER": "HEADER_TARGET",
                "HEADER_TARGET": "\"/Configured/configured.hlsli\""
            },
            "hlsl.virtualDirectoryMappings": {"/Configured": "Configured"},
            "hlsl.additionalArguments": ["-DHEADER=\"runtime.hlsli\""]
        })";
        REQUIRE(config);
    }
    {
        std::ofstream configured{directory.path() / "Configured" / "configured.hlsli"};
        REQUIRE(configured);
        configured << "static const float4 configuredValue = 1.0.xxxx;\n";
        REQUIRE(configured);
        std::ofstream runtime{directory.path() / "runtime.hlsli"};
        REQUIRE(runtime);
        runtime << "static const float4 runtimeValue = 2.0.xxxx;\n";
        REQUIRE(runtime);
    }

    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "argument-override.hlsl").string());
    const auto runtime_uri = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "runtime.hlsli").string());
    const std::string source = "#include HEADER\n"
                               "float4 main() : SV_Target { return runtimeValue; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));

    const auto messages = last_diagnostics(notifications, document.uri());
    CHECK_FALSE(mentions(messages, "runtimeValue"));
    CHECK_FALSE(mentions(messages, "runtime.hlsli"));

    const auto definition = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "textDocument/definition",
        .params = Json{{"textDocument", {{"uri", document.uri()}}},
                       {"position", position_at(source, source.find("runtimeValue") + 3)}}});
    REQUIRE(definition.has_value());
    const auto* definition_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*definition);
    REQUIRE(definition_response != nullptr);
    CHECK(definition_response->result["uri"] == runtime_uri.uri());
    CHECK(definition_response->result["range"]["start"]["line"] == 0);
    CHECK(definition_response->result["range"]["start"]["character"] == 20);

    const auto explorer = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "hlsl/preprocessorExplorer",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(explorer.has_value());
    const auto* explorer_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*explorer);
    REQUIRE(explorer_response != nullptr);
    const auto& include = explorer_response->result["files"][0]["includes"][0];
    CHECK(include["path"] == "HEADER");
    CHECK(include["kind"] == "macro");
    CHECK(include["status"] == "dynamic");
    CHECK_FALSE(include.contains("expandedPath"));
    CHECK_FALSE(include.contains("resolvedUri"));
    CHECK_FALSE(include.contains("configurationMacro"));
    CHECK_FALSE(explorer_response->result["diagnostics"].empty());
}

TEST_CASE("Server reports an invalid variant selection without applying it",
          "[lsp][variants][integration]") {
    TestDirectory directory;
    write_variant_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "variant.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", variant_shader()}}}}}));

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Ghost"}}}));
    const auto* message = find_last(notifications, "window/showMessage");
    REQUIRE(message != nullptr);
    CHECK((*message->params)["type"] == 2);
    // The unknown variant is not applied, so the document keeps its default macros.
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "marker_none"));

    // The same unresolved selection must not be reported repeatedly.
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "workspace/didChangeConfiguration",
                                                  .params = Json{{"settings", Json::object()}}}));
    CHECK(count_method(notifications, "window/showMessage") == 1);
}

TEST_CASE("Active variant inlay hints require an applicable defined variant",
          "[lsp][inlay-hints][variants][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.variantsVersion": 1,
            "hlsl.variants": [
                { "name": "Applicable", "files": ["shader.hlsl"] },
                { "name": "OtherFile", "files": ["other.hlsl"] }
            ]
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    const std::string source = "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));

    std::int64_t request_id = 2;
    const auto hints_for = [&](std::string_view variant) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", variant}}}));
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = request_id++,
            .method = "textDocument/inlayHint",
            .params = Json{{"textDocument", {{"uri", document.uri()}}},
                           {"range",
                            {{"start", position_at(source, 0)},
                             {"end", position_at(source, source.size())}}}}});
        if (!response.has_value()) {
            return Json::array();
        }
        const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
        if (result == nullptr) {
            return Json::array();
        }
        return result->result;
    };
    const auto has_variant_hint = [](const Json& hints, std::string_view label) {
        return std::ranges::any_of(hints, [label](const auto& hint) {
            return hint["label"].template get<std::string>() == label;
        });
    };

    CHECK(has_variant_hint(hints_for("Applicable"), "variant: Applicable"));
    CHECK_FALSE(has_variant_hint(hints_for("OtherFile"), "variant: OtherFile"));
    CHECK_FALSE(has_variant_hint(hints_for("Undefined"), "variant: Undefined"));
}

TEST_CASE("Server lists shader variants through hlsl/variants", "[lsp][variants][integration]") {
    TestDirectory directory;
    write_variant_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "variant.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", variant_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Alpha"}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/variants",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    CHECK(result->result["activeVariant"] == "Alpha");
    const auto& variants = result->result["variants"];
    REQUIRE(variants.size() == 2);
    bool found_alpha = false;
    bool found_beta = false;
    for (const auto& variant : variants) {
        if (variant["name"] == "Alpha") {
            found_alpha = true;
            CHECK(variant["applicable"] == true);
            CHECK(variant["description"] == "Alpha permutation");
            CHECK(variant["entryPoint"] == "");
        }
        if (variant["name"] == "Beta") {
            found_beta = true;
        }
    }
    CHECK(found_alpha);
    CHECK(found_beta);
}

TEST_CASE("Server omits variants that do not apply to the requested document",
          "[lsp][variants][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.variantsVersion": 1,
            "hlsl.variants": [
                {
                    "name": "Current",
                    "files": ["current.hlsl"],
                    "hlsl.entryPoint": "currentMain"
                },
                {
                    "name": "Other",
                    "files": ["other.hlsl"],
                    "hlsl.entryPoint": "otherMain"
                }
            ]
        })";
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "current.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", variant_shader()}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/variants",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto& variants = result->result["variants"];
    REQUIRE(variants.size() == 1);
    CHECK(variants[0]["name"] == "Current");
    CHECK(variants[0]["entryPoint"] == "currentMain");
}

TEST_CASE("A variant DXC runtime selection triggers a controlled restart",
          "[lsp][variants][runtime][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config
            << R"({"root":true,"hlsl.variantsVersion":1,"hlsl.variants":[{"name":"Custom","hlsl.dxcRuntimeDirectory":")"
            << runtime_json_path(HLSL_TEST_DXC_RUNTIME_DIR) << R"("}]})";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", valid_hlsl()}}}}}));
    CHECK(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 0);

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Custom"}}}));
    REQUIRE(count_method(notifications, "hlsl/dxcRuntimeRestartRequired") == 1);
    const auto* restart = find_last(notifications, "hlsl/dxcRuntimeRestartRequired");
    REQUIRE(restart != nullptr);
    CHECK(std::filesystem::equivalent((*restart->params)["directory"].get<std::string>(),
                                      HLSL_TEST_DXC_RUNTIME_DIR));
}

namespace {

void write_compilation_info_config(const TestDirectory& directory) {
    std::ofstream config{directory.path() / "shadertoolsconfig.json"};
    REQUIRE(config);
    config << R"({
        "root": true,
        "hlsl.targetProfile": "ps_6_6",
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [
            { "name": "Prod", "description": "Production entry point",
              "hlsl.entryPoint": "PSMain",
              "hlsl.preprocessorDefinitions": { "USE_TINT": 1 } }
        ]
    })";
    REQUIRE(config);
}

[[nodiscard]] std::string compilation_info_shader() {
    return "Texture2D<float4> MainTexture : register(t0);\n"
           "SamplerState MainSampler : register(s0);\n"
           "float4 PSMain(float4 position : SV_Position) : SV_Target {\n"
           "#if defined(USE_TINT)\n"
           "    return MainTexture.Sample(MainSampler, position.xy) * 2.0;\n"
           "#else\n"
           "    return MainTexture.Sample(MainSampler, position.xy);\n"
           "#endif\n"
           "}\n";
}

} // namespace

TEST_CASE("Server compiles hlsl/compilationInfo using DXC and honors the active variant",
          "[lsp][compilation-info][integration]") {
    TestDirectory directory;
    write_compilation_info_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compilation_info_shader()}}}}}));

    // Without an active variant, no explicit entry point is configured; the
    // root's declared entry function (PSMain) will not be found, so DXC
    // reports a structured compile failure rather than a fabricated success.
    {
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{2},
            .method = "hlsl/compilationInfo",
            .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
        REQUIRE(response.has_value());
        const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
        REQUIRE(result != nullptr);
        INFO(result->result.dump());
        CHECK(result->result["success"] == false);
        CHECK_FALSE(result->result["diagnostics"].empty());
        CHECK(result->result["activeVariant"].is_null());
        CHECK(result->result["targetProfile"] == "ps_6_6");
        CHECK(result->result["disassembly"].is_null());
    }

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Prod"}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "hlsl/compilationInfo",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto& info = result->result;
    INFO(info.dump());
    CHECK(info["activeVariant"] == "Prod");
    CHECK(info["entryPoint"] == "PSMain");
    CHECK(info["stage"] == "pixel");
    CHECK(info["targetProfile"] == "ps_6_6");
    CHECK(std::ranges::any_of(info["defines"],
                              [](const Json& define) { return define == "USE_TINT=1"; }));
    REQUIRE(info["success"] == true);
    REQUIRE(!info["output"].is_null());
    CHECK(info["output"]["type"] == "dxil");
    CHECK(info["output"]["size"].get<std::size_t>() > 0);
    REQUIRE(!info["disassembly"].is_null());
    CHECK(info["disassembly"]["available"] == true);
    CHECK(info["disassembly"]["format"] == "dxil");
    CHECK_FALSE(info["disassembly"]["text"].get<std::string>().empty());
    CHECK(info["disassembly"]["truncated"] == false);
    CHECK(info["disassembly"]["originalSize"] == info["disassembly"]["displayedSize"]);
    REQUIRE(!info["reflection"].is_null());
    CHECK(info["reflection"]["available"] == true);
    const auto& resources = info["reflection"]["resources"];
    CHECK(std::ranges::any_of(
        resources, [](const Json& resource) { return resource["name"] == "MainTexture"; }));
    CHECK(std::ranges::any_of(
        resources, [](const Json& resource) { return resource["name"] == "MainSampler"; }));
    CHECK(info["reflection"]["threadGroupSize"].is_null());
    CHECK(info["reflection"]["barrierInstructionCount"] == 0);

    // hlsl/compilationInfo backward-compatibly extends each resource with
    // compiler-owned register class, raw reflection flags, range id, sample
    // count, unbounded/system-reserved-space status, and a usage status
    // derived only from compiler flags.
    const auto texture_it = std::ranges::find_if(
        resources, [](const Json& resource) { return resource["name"] == "MainTexture"; });
    REQUIRE(texture_it != resources.end());
    const auto& texture = *texture_it;
    CHECK(texture["registerClass"] == "srv");
    CHECK(texture["rawFlags"].is_number_unsigned());
    CHECK(texture["rangeId"].is_number_unsigned());
    CHECK(texture["sampleCount"].is_number_unsigned());
    CHECK(texture["unbounded"] == false);
    CHECK(texture["systemReservedSpace"] == false);
    CHECK((texture["usage"] == "used" || texture["usage"] == "unknown"));

    // hlsl/compilationInfo also attaches the resource's declaration site,
    // reusing the same DXC IntelliSense cursor/document-symbol machinery
    // already used for hover/go-to-definition, as a proper LSP {uri, range}
    // location (0-based line/UTF-16 character) rather than a raw compiler
    // byte column, so clients can navigate straight to it.
    REQUIRE(!texture["sourceLocation"].is_null());
    CHECK(texture["sourceLocation"]["uri"] == document.uri());
    const auto& texture_range = texture["sourceLocation"]["range"];
    CHECK(texture_range["start"]["line"] == 0);
    CHECK(texture_range["start"]["character"] == 18);
    CHECK(texture_range["end"]["line"] == 0);
    CHECK(texture_range["end"]["character"] == 18 + std::string_view{"MainTexture"}.size());

    const auto sampler_it = std::ranges::find_if(
        resources, [](const Json& resource) { return resource["name"] == "MainSampler"; });
    REQUIRE(sampler_it != resources.end());
    CHECK((*sampler_it)["registerClass"] == "sampler");
    REQUIRE(!(*sampler_it)["sourceLocation"].is_null());
    CHECK((*sampler_it)["sourceLocation"]["range"]["start"]["line"] == 1);

    // hlsl/compilationInfo also surfaces deterministic resource-binding
    // analysis grouped by register class and register space.
    REQUIRE(!info["reflection"]["bindingAnalysis"].is_null());
    const auto& binding_analysis = info["reflection"]["bindingAnalysis"];
    REQUIRE(binding_analysis.contains("groups"));
    REQUIRE(binding_analysis.contains("collisions"));
    CHECK(binding_analysis["collisions"].empty());
    CHECK(std::ranges::any_of(binding_analysis["groups"], [](const Json& group) {
        return group["registerClass"] == "srv" && !group["ranges"].empty();
    }));
    CHECK(std::ranges::any_of(binding_analysis["groups"], [](const Json& group) {
        return group["registerClass"] == "sampler" && !group["ranges"].empty();
    }));

    // hlsl/compilationInfo reports embedded root-signature availability and
    // resource/root-signature compatibility even when no [RootSignature(...)]
    // attribute is present in the source.
    REQUIRE(!info["rootSignature"].is_null());
    CHECK(info["rootSignature"]["availability"] == "absent");
    CHECK(info["rootSignature"]["details"].is_null());
    REQUIRE(!info["compatibility"].is_null());
    CHECK(info["compatibility"]["status"] == "unknown");
}

TEST_CASE("Server recomputes reflected resource source locations after an unsaved edit moves the "
          "declaration",
          "[lsp][compilation-info][source-location][integration]") {
    // A stale cached location would keep pointing at the pre-edit line; the
    // very next hlsl/compilationInfo call after didChange must reflect the
    // current unsaved buffer, since sourceLocation is derived from the same
    // DXC IntelliSense parse index used for hover/go-to-definition, not a
    // fabricated or cached value.
    TestDirectory directory;
    write_compilation_info_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compilation_info_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Prod"}}}));

    const auto find_texture_location = [&](std::int64_t id) {
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = id,
            .method = "hlsl/compilationInfo",
            .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
        const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
        REQUIRE(result != nullptr);
        INFO(result->result.dump());
        REQUIRE(result->result["success"] == true);
        const auto& resources = result->result["reflection"]["resources"];
        const auto texture_it = std::ranges::find_if(
            resources, [](const Json& resource) { return resource["name"] == "MainTexture"; });
        REQUIRE(texture_it != resources.end());
        REQUIRE(!(*texture_it)["sourceLocation"].is_null());
        return (*texture_it)["sourceLocation"]["range"]["start"]["line"].get<std::uint32_t>();
    };

    CHECK(find_texture_location(2) == 0);

    const auto moved_text = "\n\n" + compilation_info_shader();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", document.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", moved_text}}})}}}));

    CHECK(find_texture_location(3) == 2);
}

TEST_CASE("Server reports SPIR-V compilation info without fabricated reflection",
          "[lsp][compilation-info][spirv][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.targetProfile": "ps_6_6",
            "hlsl.entryPoint": "main",
            "hlsl.additionalArguments": ["-spirv"]
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "float4 main() : SV_Target {\n    return 1.0.xxxx;\n}\n"}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/compilationInfo",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto& info = result->result;
    INFO(info.dump());
    REQUIRE(info["success"] == true);
    REQUIRE(!info["output"].is_null());
    CHECK(info["output"]["type"] == "spirv");
    REQUIRE(!info["disassembly"].is_null());
    CHECK(info["disassembly"]["available"] == false);
    CHECK(info["disassembly"]["format"] == "spirv");
    CHECK(info["disassembly"]["text"] == "");
    CHECK_FALSE(info["disassembly"]["unavailableReason"].get<std::string>().empty());
    REQUIRE(!info["reflection"].is_null());
    CHECK(info["reflection"]["available"] == false);
    CHECK_FALSE(info["reflection"]["unavailableReason"].get<std::string>().empty());

    // Root signatures are a DXIL-container concept; for SPIR-V output this is
    // reported distinctly as "not applicable" rather than absent.
    REQUIRE(!info["rootSignature"].is_null());
    CHECK(info["rootSignature"]["availability"] == "notApplicable");
    REQUIRE(!info["compatibility"].is_null());
    CHECK(info["compatibility"]["status"] == "unknown");
}

TEST_CASE("Server reports unavailable reflection and unknown compatibility for library-target "
          "DXIL compilation info",
          "[lsp][compilation-info][integration]") {
    // A `lib_*` target profile compiles successfully to DXIL but produces a
    // library container rather than a single-stage shader, so
    // ID3D12ShaderReflection is genuinely unavailable even though the
    // compile succeeded. `rootSignature` extraction reads the DXIL container
    // directly and does not depend on reflection, so it must stay non-null;
    // `compatibility` must likewise stay non-null, reporting "unknown"
    // rather than being fabricated from an empty resource list or left null.
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.targetProfile": "lib_6_3"
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "export float4 Shade(float4 color) {\n"
                                  "    return color;\n"
                                  "}\n"}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/compilationInfo",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto& info = result->result;
    INFO(info.dump());
    REQUIRE(info["success"] == true);
    REQUIRE(!info["output"].is_null());
    CHECK(info["output"]["type"] == "dxil");
    REQUIRE(!info["reflection"].is_null());
    CHECK(info["reflection"]["available"] == false);
    CHECK_FALSE(info["reflection"]["unavailableReason"].get<std::string>().empty());

    REQUIRE(!info["rootSignature"].is_null());
    CHECK(info["rootSignature"]["availability"] == "absent");
    REQUIRE(!info["compatibility"].is_null());
    CHECK(info["compatibility"]["status"] == "unknown");
    CHECK_FALSE(info["compatibility"]["explanation"].get<std::string>().empty());
    CHECK(info["compatibility"]["issues"].empty());
}

TEST_CASE("Server surfaces embedded root-signature details and resource compatibility over "
          "hlsl/compilationInfo",
          "[lsp][compilation-info][root-signature][integration]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.targetProfile": "ps_6_6",
            "hlsl.entryPoint": "main"
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "#define MyRS \"RootFlags(0), "
                                  "DescriptorTable(SRV(t0, numDescriptors=1, space=0)), "
                                  "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR)\"\n"
                                  "Texture2D<float4> Tex : register(t0);\n"
                                  "SamplerState Samp : register(s0);\n"
                                  "[RootSignature(MyRS)]\n"
                                  "float4 main() : SV_Target {\n"
                                  "    return Tex.Sample(Samp, float2(0, 0));\n"
                                  "}\n"}}}}}));

    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{2},
        .method = "hlsl/compilationInfo",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    const auto& info = result->result;
    INFO(info.dump());
    REQUIRE(info["success"] == true);

    REQUIRE(!info["rootSignature"].is_null());
#ifdef _WIN32
    // Windows: the official D3D12 deserializer is available, so this
    // server exposes the full parameter/range/static-sampler structure and
    // a concrete compatibility verdict.
    CHECK(info["rootSignature"]["availability"] == "present");
    REQUIRE(!info["rootSignature"]["details"].is_null());
    const auto& details = info["rootSignature"]["details"];
    CHECK((details["version"] == "1.0" || details["version"] == "1.1"));
    CHECK(details.contains("rawFlags"));
    CHECK(details.contains("cbvSrvUavHeapDirectlyIndexed"));
    CHECK(details.contains("samplerHeapDirectlyIndexed"));
    REQUIRE(!details["parameters"].empty());
    const auto& parameter = details["parameters"][0];
    CHECK(parameter["kind"] == "descriptorTable");
    REQUIRE(!parameter["descriptorTableRanges"].empty());
    const auto& range = parameter["descriptorTableRanges"][0];
    CHECK(range["type"] == "srv");
    CHECK(range["baseRegister"] == 0);
    CHECK(range["space"] == 0);
    REQUIRE(!details["staticSamplers"].empty());
    CHECK(details["staticSamplers"][0]["shaderRegister"] == 0);

    // Fully-covered resources compile cleanly and the compatibility analysis
    // reports "compatible" with no issues.
    REQUIRE(!info["compatibility"].is_null());
    CHECK(info["compatibility"]["status"] == "compatible");
    CHECK(info["compatibility"]["issues"].empty());
#else
    // Non-Windows (e.g. Linux): presence is still correctly detected via
    // IDxcUtils::GetDxilContainerPart (cross-platform), but detailed
    // deserialization requires the Windows-only
    // ID3D12VersionedRootSignatureDeserializer, so this server reports
    // "presentDetailsUnavailable" with an explicit reason instead of
    // fabricating details, and compatibility is reported as "unknown"
    // rather than a guessed verdict.
    CHECK(info["rootSignature"]["availability"] == "presentDetailsUnavailable");
    CHECK(info["rootSignature"]["details"].is_null());
    CHECK_FALSE(info["rootSignature"]["unavailableReason"].get<std::string>().empty());

    REQUIRE(!info["compatibility"].is_null());
    CHECK(info["compatibility"]["status"] == "unknown");
#endif
}

TEST_CASE("Server rejects hlsl/compilationInfo for invalid or unopened documents",
          "[lsp][compilation-info][validation]") {
    const auto uri = shader_uri();
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));

    // The document has never been opened.
    {
        const auto response = server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                                 .method = "hlsl/compilationInfo",
                                                 .params = Json{{"textDocument", {{"uri", uri}}}}});
        REQUIRE(response.has_value());
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*response);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
    }

    // Missing textDocument.uri entirely.
    {
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{3}, .method = "hlsl/compilationInfo", .params = Json::object()});
        REQUIRE(response.has_value());
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*response);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
    }

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", ""}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didClose", .params = Json{{"textDocument", {{"uri", uri}}}}}));

    // The document was opened and then closed.
    {
        const auto response = server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{4},
                                                 .method = "hlsl/compilationInfo",
                                                 .params = Json{{"textDocument", {{"uri", uri}}}}});
        REQUIRE(response.has_value());
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*response);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
    }
}

TEST_CASE("Server supersedes an in-flight hlsl/compilationInfo request when the document changes",
          "[lsp][compilation-info][safety]") {
    // A concurrent edit for the same root supersedes the in-flight interactive
    // work (distinct from an explicit $/cancelRequest), so the request surfaces
    // as ContentModified rather than completing against stale content.
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{
            {"textDocument",
             {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", valid_hlsl()}}}}}));
    server.wait_for_analysis();

    auto response = std::async(std::launch::async, [&] {
        return server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{2},
                                                 .method = "hlsl/compilationInfo",
                                                 .params = Json{{"textDocument", {{"uri", uri}}}}});
    });
    entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", valid_hlsl()}}})}}}));
    release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    server.wait_for_analysis();
}

TEST_CASE("Server cancellation returns RequestCancelled for hlsl/compilationInfo",
          "[lsp][compilation-info][cancellation]") {
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{
            {"textDocument",
             {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", valid_hlsl()}}}}}));
    server.wait_for_analysis();

    const hlsl_intellisense::json_rpc::Request request{.id = std::string{"compilation-info"},
                                                       .method = "hlsl/compilationInfo",
                                                       .params =
                                                           Json{{"textDocument", {{"uri", uri}}}}};
    const auto cancellation = server.begin_request(request.id);
    auto response =
        std::async(std::launch::async, [&] { return server.handle(request, cancellation); });
    entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "$/cancelRequest", .params = Json{{"id", "compilation-info"}}}));

    const auto result = response.get();
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);
    release.set_value();
    server.wait_for_analysis();
}

namespace {

[[nodiscard]] hlsl_intellisense::json_rpc::Request
code_action_request(std::int64_t id, std::string_view uri, Json range,
                    Json context = Json{{"diagnostics", Json::array()}}) {
    return hlsl_intellisense::json_rpc::Request{
        .id = id,
        .method = "textDocument/codeAction",
        .params = Json{{"textDocument", {{"uri", std::string{uri}}}},
                       {"range", std::move(range)},
                       {"context", std::move(context)}}};
}

[[nodiscard]] Json code_action_result(hlsl_intellisense::lsp::Server& server,
                                      const hlsl_intellisense::json_rpc::Request& request) {
    const auto result = server.handle(request);
    REQUIRE(result.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
    REQUIRE(response != nullptr);
    return response->result;
}

[[nodiscard]] Json zero_width_range(Json position) {
    return {{"start", position}, {"end", position}};
}

} // namespace

TEST_CASE("textDocument/codeAction returns a versioned quickfix for a verified DXC fix-it",
          "[lsp][code-action][fixit][integration]") {
    // The trailing UTF-16 surrogate-pair comment before the fix-it location
    // exercises UTF-16 position math on an unsaved (never-saved) open buffer:
    // the produced edit range must be measured in UTF-16 code units, not bytes
    // or codepoints.
    const std::string source = "float4 main() : SV_Target {\n"
                               "    /* \xF0\x9F\x98\x80 */ float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    const auto uri = shader_uri();
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto insertion_offset = source.find("1.0") + std::string_view{"1.0"}.size();
    const auto insertion_position = position_at(source, insertion_offset);

    const auto find_fix_it_action = [](const Json& actions) -> const Json* {
        for (const auto& action : actions) {
            if (action.contains("edit")) {
                return &action;
            }
        }
        return nullptr;
    };

    // A request range overlapping the fix-it location surfaces the action.
    const auto actions = code_action_result(
        server, code_action_request(2, uri, zero_width_range(insertion_position)));
    REQUIRE(actions.is_array());
    const auto* fix_it_action = find_fix_it_action(actions);
    REQUIRE(fix_it_action != nullptr);
    CHECK((*fix_it_action)["kind"] == "quickfix");
    CHECK((*fix_it_action)["title"].get<std::string>().find("expected ';'") != std::string::npos);
    REQUIRE((*fix_it_action)["diagnostics"].size() == 1);
    CHECK((*fix_it_action)["diagnostics"][0]["message"] == "expected ';' at end of declaration");
    const auto& document_changes = (*fix_it_action)["edit"]["documentChanges"];
    REQUIRE(document_changes.size() == 1);
    CHECK(document_changes[0]["textDocument"]["uri"] == uri);
    CHECK(document_changes[0]["textDocument"]["version"] == 1);
    REQUIRE(document_changes[0]["edits"].size() == 1);
    CHECK(document_changes[0]["edits"][0]["newText"] == ";");
    CHECK(document_changes[0]["edits"][0]["range"]["start"] == insertion_position);
    CHECK(document_changes[0]["edits"][0]["range"]["end"] == insertion_position);

    // A request range far from the diagnostic omits the fix-it.
    const auto far_actions = code_action_result(
        server, code_action_request(3, uri, zero_width_range(position_at(source, 0))));
    CHECK(find_fix_it_action(far_actions) == nullptr);

    // context.only excluding "quickfix" omits every action.
    const auto refactor_only =
        code_action_result(server, code_action_request(4, uri, zero_width_range(insertion_position),
                                                       Json{{"only", Json::array({"refactor"})}}));
    CHECK(refactor_only.empty());

    // context.only including "quickfix" still surfaces the action.
    const auto quickfix_only = code_action_result(
        server, code_action_request(5, uri, zero_width_range(insertion_position),
                                    Json{{"only", Json::array({"refactor", "quickfix"})}}));
    CHECK(find_fix_it_action(quickfix_only) != nullptr);
}

TEST_CASE("textDocument/codeAction offers independent multi-edit fixes and omits "
          "diagnostics DXC gave no fix-it for",
          "[lsp][code-action][fixit][integration]") {
    const std::string source = "float combine(float a, float b) { return a + b; }\n"
                               "float4 main() : SV_Target {\n"
                               "    float total = combin(1.0, 2.0);\n"
                               "    float other = totally_unresolvable_zzz_identifier;\n"
                               "    return float4(total, other, 0, 0);\n"
                               "}\n";
    const auto uri = shader_uri();
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto whole_document =
        Json{{"start", position_at(source, 0)}, {"end", position_at(source, source.size())}};
    const auto actions = code_action_result(server, code_action_request(2, uri, whole_document));
    REQUIRE(actions.is_array());

    // Exactly one action: the typo correction. The undeclared identifier with
    // no close match gets no fix-it (verified empirically) and is not an
    // include path, so include-recovery does not apply either.
    std::size_t edit_actions{};
    for (const auto& action : actions) {
        if (action.contains("edit")) {
            ++edit_actions;
            CHECK(action["edit"]["documentChanges"][0]["edits"][0]["newText"] == "combine");
            REQUIRE(action["diagnostics"].size() == 1);
            CHECK(action["diagnostics"][0]["message"].get<std::string>().find("did you mean") !=
                  std::string::npos);
        }
    }
    CHECK(edit_actions == 1);
}

TEST_CASE("textDocument/codeAction rejects malformed, overlapping, and cross-file fix-its via a "
          "diagnostics test seam while still offering unaffected fixes",
          "[lsp][code-action][safety]") {
    // Real DXC 1.9.2607.13 was not observed to produce malformed/overlapping/
    // cross-file fix-its; this exercises the rejection paths using the
    // AnalysisHooks::after_diagnostics test seam to corrupt genuinely-produced
    // diagnostics before the server caches them.
    const std::string source = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    hooks->after_diagnostics =
        [&source](std::vector<hlsl_intellisense::dxc::Diagnostic>& diagnostics) {
            REQUIRE(!diagnostics.empty());
            auto& fixable = diagnostics.front();
            REQUIRE(!fixable.fix_its.empty());
            const auto valid = fixable.fix_its.front();

            hlsl_intellisense::dxc::Diagnostic overlapping = fixable;
            overlapping.message = "test-seam: overlapping edits";
            // Widen the valid edit's range so it has non-zero width, then start a
            // second edit strictly inside that widened range: the two ranges
            // genuinely overlap and must be rejected wholesale.
            auto first_wide = valid;
            first_wide.range.end.offset = valid.range.start.offset + 2;
            auto second_overlapping = valid;
            second_overlapping.range.start.offset = valid.range.start.offset + 1;
            second_overlapping.range.end.offset = valid.range.start.offset + 3;
            overlapping.fix_its = {first_wide, second_overlapping};

            hlsl_intellisense::dxc::Diagnostic malformed_utf8 = fixable;
            malformed_utf8.message = "test-seam: malformed UTF-8 replacement";
            malformed_utf8.fix_its = {valid};
            malformed_utf8.fix_its.front().replacement_text = "\xFF\xFE";

            hlsl_intellisense::dxc::Diagnostic out_of_bounds = fixable;
            out_of_bounds.message = "test-seam: out-of-bounds offset";
            out_of_bounds.fix_its = {valid};
            out_of_bounds.fix_its.front().range.end.offset =
                static_cast<std::uint32_t>(source.size() + 1000);

            hlsl_intellisense::dxc::Diagnostic cross_file = fixable;
            cross_file.message = "test-seam: cross-file fix-it";
            cross_file.fix_its = {valid};
            cross_file.fix_its.front().range.start.path = "different-file.hlsl";
            cross_file.fix_its.front().range.end.path = "different-file.hlsl";

            diagnostics.push_back(overlapping);
            diagnostics.push_back(malformed_utf8);
            diagnostics.push_back(out_of_bounds);
            diagnostics.push_back(cross_file);
        };
    hlsl_intellisense::lsp::ServerOptions options;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto whole_document =
        Json{{"start", position_at(source, 0)}, {"end", position_at(source, source.size())}};
    const auto actions = code_action_result(server, code_action_request(2, uri, whole_document));
    REQUIRE(actions.is_array());

    std::vector<std::string> edit_titles;
    for (const auto& action : actions) {
        if (action.contains("edit")) {
            edit_titles.push_back(action["title"].get<std::string>());
        }
    }
    // Only the genuine, uncorrupted fix-it survives; every corrupted variant
    // (overlapping, malformed UTF-8, out-of-bounds, cross-file) is rejected
    // wholesale rather than partially applied.
    REQUIRE(edit_titles.size() == 1);
    CHECK(edit_titles.front().find("expected ';'") != std::string::npos);
    for (const auto& title : edit_titles) {
        CHECK(title.find("test-seam") == std::string::npos);
    }
}

TEST_CASE("textDocument/codeAction rejects two fix-it edits from the same diagnostic that start "
          "at the exact same offset, applying all-or-nothing semantics deterministically",
          "[lsp][code-action][safety][ordering]") {
    // Real DXC 1.9.2607.13 was not observed to produce two edits sharing a
    // start offset; this exercises the tie-rejection path using the
    // AnalysisHooks::after_diagnostics test seam. Two edits with an identical
    // start offset have no DXC-guaranteed relative order, so the whole
    // multi-edit fix must be rejected rather than depending on sort stability
    // to silently pick one order over another.
    const std::string source = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    const auto uri = shader_uri();
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    hooks->after_diagnostics = [](std::vector<hlsl_intellisense::dxc::Diagnostic>& diagnostics) {
        REQUIRE(!diagnostics.empty());
        auto& fixable = diagnostics.front();
        REQUIRE(!fixable.fix_its.empty());
        const auto valid = fixable.fix_its.front();

        hlsl_intellisense::dxc::Diagnostic equal_start = fixable;
        equal_start.message = "test-seam: equal-start edits";
        // Two zero-width edits at the identical start offset: neither
        // overlaps the other by the strict start-before-end definition,
        // but their relative order is genuinely ambiguous.
        auto first = valid;
        first.range.end.offset = first.range.start.offset;
        first.replacement_text = "A";
        auto second = valid;
        second.range.end.offset = second.range.start.offset;
        second.replacement_text = "B";
        equal_start.fix_its = {first, second};
        diagnostics.push_back(equal_start);
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto whole_document =
        Json{{"start", position_at(source, 0)}, {"end", position_at(source, source.size())}};
    const auto actions = code_action_result(server, code_action_request(2, uri, whole_document));
    REQUIRE(actions.is_array());

    std::vector<std::string> edit_titles;
    for (const auto& action : actions) {
        if (action.contains("edit")) {
            edit_titles.push_back(action["title"].get<std::string>());
        }
    }
    // Only the genuine fix-it survives; the synthetic equal-start diagnostic's
    // fix is rejected wholesale, never partially applied as just "A" or "B".
    REQUIRE(edit_titles.size() == 1);
    CHECK(edit_titles.front().find("expected ';'") != std::string::npos);
    for (const auto& title : edit_titles) {
        CHECK(title.find("test-seam") == std::string::npos);
    }
}

TEST_CASE("textDocument/codeAction omits results while a configuration/variant reanalysis is in "
          "flight, even though the document version has not changed",
          "[lsp][code-action][safety][generation]") {
    const auto uri = shader_uri();
    const std::string broken = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::atomic_int analysis_calls{};
    std::promise<void> second_entered;
    std::promise<void> release_second;
    auto released = release_second.get_future().share();
    hooks->before_analysis = [&](std::string_view, std::int64_t) {
        const auto call_index = analysis_calls.fetch_add(1);
        if (call_index == 1) {
            // The second analysis for this document: triggered by the variant
            // change below, at the *same* document version as the first.
            second_entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", broken}}}}}));
    server.wait_for_analysis();

    const auto whole_document =
        Json{{"start", position_at(broken, 0)}, {"end", position_at(broken, broken.size())}};

    // The initial analysis is cached: a request now finds the fix-it.
    const auto fresh = code_action_result(server, code_action_request(2, uri, whole_document));
    CHECK(std::ranges::any_of(fresh, [](const auto& action) { return action.contains("edit"); }));

    // Selecting an active variant bumps analysis_generations_ synchronously
    // (see Server::analyze_and_publish) and starts a background reanalysis,
    // without changing the document's *version* at all. Block that
    // reanalysis before it completes.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "SomeVariant"}}}));
    second_entered.get_future().wait();

    // The cached diagnostics are still from the pre-variant-change generation:
    // a request during this window must omit rather than reuse the stale
    // generation's actions, even though snapshot.version() has not changed.
    const auto stale = code_action_result(server, code_action_request(3, uri, whole_document));
    CHECK(stale.empty());

    release_second.set_value();
    server.wait_for_analysis();

    // Once the variant-triggered analysis completes, the same request
    // succeeds again against the new generation's diagnostics.
    const auto recovered = code_action_result(server, code_action_request(4, uri, whole_document));
    CHECK(
        std::ranges::any_of(recovered, [](const auto& action) { return action.contains("edit"); }));
}

TEST_CASE("textDocument/codeAction omits results for a stale document version and returns "
          "RequestCancelled when cancelled",
          "[lsp][code-action][safety][cancellation]") {
    const auto uri = shader_uri();
    const std::string broken = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::atomic_int analysis_calls{};
    std::promise<void> second_entered;
    std::promise<void> release_second;
    auto released = release_second.get_future().share();
    hooks->before_analysis = [&](std::string_view, std::int64_t version) {
        if (version == 2) {
            second_entered.set_value();
            released.wait();
        }
        analysis_calls.fetch_add(1);
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", broken}}}}}));
    server.wait_for_analysis();

    const auto whole_document =
        Json{{"start", position_at(broken, 0)}, {"end", position_at(broken, broken.size())}};

    // A version-1 request now finds the version-1 diagnostics already cached.
    const auto fresh = code_action_result(server, code_action_request(2, uri, whole_document));
    CHECK(std::ranges::any_of(fresh, [](const auto& action) { return action.contains("edit"); }));

    // Change to version 2 but block its analysis before it completes: the
    // cached diagnostics are still version 1, so a version-2 request must omit
    // (not error, not guess) rather than reuse stale content.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", broken}}})}}}));
    second_entered.get_future().wait();
    const auto stale = code_action_result(server, code_action_request(3, uri, whole_document));
    CHECK(stale.empty());

    // Cancellation is honored consistently with every other request handler.
    const hlsl_intellisense::json_rpc::Request cancelled_request{
        .id = std::int64_t{4},
        .method = "textDocument/codeAction",
        .params = Json{{"textDocument", {{"uri", uri}}},
                       {"range", whole_document},
                       {"context", {{"diagnostics", Json::array()}}}}};
    const auto cancellation = server.begin_request(cancelled_request.id);
    cancellation.cancel();
    const auto cancelled_result = server.handle(cancelled_request, cancellation);
    const auto* cancel_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&cancelled_result);
    REQUIRE(cancel_error != nullptr);
    CHECK(cancel_error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);

    release_second.set_value();
    server.wait_for_analysis();

    // Once the version-2 analysis completes, the same request now succeeds.
    const auto recovered = code_action_result(server, code_action_request(5, uri, whole_document));
    CHECK(
        std::ranges::any_of(recovered, [](const auto& action) { return action.contains("edit"); }));
}

TEST_CASE("A cache-hit reanalysis with identical diagnostics still refreshes the cached "
          "generation, so textDocument/codeAction stays current without a republish",
          "[lsp][code-action][diagnostics][generation]") {
    const auto uri = shader_uri();
    const std::string broken = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", broken}}}}}));
    server.wait_for_analysis();
    REQUIRE(notifications.size() == 1);

    const auto whole_document =
        Json{{"start", position_at(broken, 0)}, {"end", position_at(broken, broken.size())}};
    const auto fresh = code_action_result(server, code_action_request(2, uri, whole_document));
    CHECK(std::ranges::any_of(fresh, [](const auto& action) { return action.contains("edit"); }));

    // "Unrelated" is not referenced by this document or any configuration, so
    // the reanalysis it triggers via reanalyze_all() bumps this document's
    // analysis generation but reproduces byte-for-byte identical diagnostics:
    // a cache hit under the item-3 dedup rule, so no new
    // textDocument/publishDiagnostics is sent for it.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Unrelated"}}}));
    server.wait_for_analysis();
    CHECK(notifications.size() == 1);

    // The cached DiagnosticsRecord's generation must still have been
    // refreshed to match the bumped analysis generation: otherwise every
    // subsequent request would find a generation mismatch against its own
    // valid, unchanged diagnostics and incorrectly treat them as stale
    // forever, even though nothing was ever republished.
    const auto after_bump = code_action_result(server, code_action_request(3, uri, whole_document));
    CHECK(std::ranges::any_of(after_bump,
                              [](const auto& action) { return action.contains("edit"); }));
}

TEST_CASE("Diagnostics are republished only when their version or content actually changes",
          "[lsp][diagnostics][safety]") {
    const auto uri = shader_uri();
    const std::string broken = "float4 main() : SV_Target {\n"
                               "    float x = 1.0\n"
                               "    return x.xxxx;\n"
                               "}\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", broken}}}}}));
    server.wait_for_analysis();
    REQUIRE(notifications.size() == 1);
    CHECK(!last_diagnostics(notifications, uri).empty());

    // A reanalysis that reproduces identical diagnostics at the same document
    // version (see the generation-refresh test above) is not republished.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Unrelated"}}}));
    server.wait_for_analysis();
    REQUIRE(notifications.size() == 1);

    // A genuine edit that both bumps the document version and changes the
    // diagnostics (fixing the missing semicolon) always republishes.
    const std::string fixed = "float4 main() : SV_Target {\n"
                              "    float x = 1.0;\n"
                              "    return x.xxxx;\n"
                              "}\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", fixed}}})}}}));
    server.wait_for_analysis();
    REQUIRE(notifications.size() == 2);
    CHECK(last_diagnostics(notifications, uri).empty());
}

TEST_CASE("textDocument/codeAction offers a deterministic variant-select recovery action for an "
          "unresolved #include, executable via workspace/executeCommand",
          "[lsp][code-action][include][variants][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "variant_includes");
    {
        std::ofstream include{directory.path() / "variant_includes" / "extra.hlsli"};
        REQUIRE(include);
        include << "static const float extraValue = 1.0;\n";
    }
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.variantsVersion": 1,
            "hlsl.variants": [
                { "name": "WithExtraIncludes",
                  "hlsl.additionalIncludeDirectories": ["variant_includes"] }
            ]
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const std::string source = "#include \"extra.hlsli\"\n"
                               "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));
    // Confirms the #include really fails to resolve under the default config.
    CHECK(mentions(last_diagnostics(notifications, document.uri()), "extra.hlsli"));

    const auto whole_document =
        Json{{"start", position_at(source, 0)}, {"end", position_at(source, source.size())}};
    const auto actions =
        code_action_result(server, code_action_request(2, document.uri(), whole_document));
    REQUIRE(actions.is_array());
    const Json* recovery_action = nullptr;
    for (const auto& action : actions) {
        if (action.contains("command")) {
            recovery_action = &action;
        }
    }
    REQUIRE(recovery_action != nullptr);
    CHECK((*recovery_action)["kind"] == "quickfix");
    REQUIRE((*recovery_action)["diagnostics"].size() == 1);
    const auto& command = (*recovery_action)["command"];
    CHECK(command["command"] == "hlsl-lsp.selectVariant");
    REQUIRE(command["arguments"].size() == 1);
    CHECK(command["arguments"][0]["variant"] == "WithExtraIncludes");

    const auto execute_result = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{3},
        .method = "workspace/executeCommand",
        .params = Json{{"command", command["command"]}, {"arguments", command["arguments"]}}});
    REQUIRE(execute_result.has_value());
    const auto* execute_response =
        std::get_if<hlsl_intellisense::json_rpc::Response>(&*execute_result);
    REQUIRE(execute_response != nullptr);
    CHECK(execute_response->result.is_null());

    // A durable client (VS Code, Visual Studio) needs to persist this
    // server-driven selection itself, or a later settings resync would
    // silently overwrite it back to whatever the client still has stored.
    // The server reports its resulting authoritative variant via a narrow
    // custom notification so the client can update its own persisted
    // setting/cache and stay in sync.
    const auto variant_changed = std::ranges::find_if(notifications, [](const auto& notification) {
        return notification.method == "hlsl/activeVariantChanged";
    });
    REQUIRE(variant_changed != notifications.end());
    REQUIRE(variant_changed->params.has_value());
    CHECK((*variant_changed->params)["variant"] == "WithExtraIncludes");

    // Selecting the variant actually fixes the compile: the #include now
    // resolves and no diagnostics remain for the document.
    CHECK(last_diagnostics(notifications, document.uri()).empty());
}

TEST_CASE("hlsl/didChangeActiveVariant from the client does not echo "
          "hlsl/activeVariantChanged back, avoiding a settings-sync feedback loop",
          "[lsp][code-action][include][variants]") {
    // The client (VS Code's settings synchronizer, Visual Studio's
    // configuration cache) already owns and persists hlsl.activeVariant
    // itself before sending this notification, so the server must not also
    // report it back as if it were a server-originated change: only the
    // workspace/executeCommand hlsl-lsp.selectVariant path (which changes
    // *only* server state) needs that round trip.
    TestDirectory directory;
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); }};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", valid_hlsl()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "SomeVariant"}}}));
    CHECK(std::ranges::none_of(notifications, [](const auto& notification) {
        return notification.method == "hlsl/activeVariantChanged";
    }));
}

namespace {

[[nodiscard]] std::string call_hierarchy_shader() {
    return "float square(float x) { return x * x; }\n"
           "int square(int x) { return x * x; }\n"
           "\n"
           "float recurse(float x) {\n"
           "    if (x <= 0.0) {\n"
           "        return 0.0;\n"
           "    }\n"
           "    return recurse(x - 1.0) + square(x);\n"
           "}\n"
           "\n"
           "float4 main() : SV_Target {\n"
           "    return square(recurse(2.0)).xxxx;\n"
           "}\n";
}

[[nodiscard]] hlsl_intellisense::json_rpc::Request
prepare_call_hierarchy_request(std::int64_t id, std::string_view uri, Json position) {
    return hlsl_intellisense::json_rpc::Request{
        .id = id,
        .method = "textDocument/prepareCallHierarchy",
        .params =
            Json{{"textDocument", {{"uri", std::string{uri}}}}, {"position", std::move(position)}}};
}

[[nodiscard]] Json call_hierarchy_result(hlsl_intellisense::lsp::Server& server,
                                         const hlsl_intellisense::json_rpc::Request& request) {
    const auto result = server.handle(request);
    REQUIRE(result.has_value());
    if (const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result)) {
        INFO("code=" << error->error.code << " message=" << error->error.message);
        FAIL("Expected a successful response");
    }
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
    REQUIRE(response != nullptr);
    return response->result;
}

} // namespace

TEST_CASE("Call hierarchy prepares an item and reports outgoing calls with overload identity and "
          "recursion",
          "[lsp][call-hierarchy]") {
    const auto uri = shader_uri();
    const auto source = call_hierarchy_shader();
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    const auto main_offset = source.find("main(");
    const auto prepare_main = call_hierarchy_result(
        server, prepare_call_hierarchy_request(2, uri, position_at(source, main_offset)));
    REQUIRE(prepare_main.is_array());
    REQUIRE(prepare_main.size() == 1);
    const auto& main_item = prepare_main[0];
    CHECK(main_item["name"] == "main");

    const auto outgoing_from_main = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                     .method = "callHierarchy/outgoingCalls",
                                                     .params = Json{{"item", main_item}}});
    REQUIRE(outgoing_from_main.is_array());
    REQUIRE(outgoing_from_main.size() == 2);
    const auto find_callee = [&](std::string_view detail_contains) {
        return std::ranges::find_if(outgoing_from_main, [&](const Json& call) {
            const std::string detail = call["to"]["detail"].get<std::string>();
            return detail.find(detail_contains) != std::string::npos;
        });
    };
    const auto to_recurse = find_callee("recurse");
    const auto to_square_float = find_callee("float square");
    REQUIRE(to_recurse != outgoing_from_main.end());
    REQUIRE(to_square_float != outgoing_from_main.end());
    CHECK((*to_recurse)["fromRanges"].size() == 1);
    CHECK((*to_square_float)["fromRanges"].size() == 1);

    // Overload identity: `square(int)` is a distinct callable from
    // `square(float)` despite sharing a name; preparing at its own
    // declaration must resolve the int overload specifically, and it must
    // report no callers/callees (it is dead code, called by nobody).
    const auto square_int_offset = source.find("int square");
    const auto square_int_name_offset = source.find("square", square_int_offset);
    const auto prepare_square_int = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(4, uri, position_at(source, square_int_name_offset)));
    REQUIRE(prepare_square_int.is_array());
    REQUIRE(prepare_square_int.size() == 1);
    const auto& square_int_item = prepare_square_int[0];
    CHECK(square_int_item["detail"] == "int square(int x)");

    const auto square_int_outgoing = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{5},
                                                     .method = "callHierarchy/outgoingCalls",
                                                     .params = Json{{"item", square_int_item}}});
    CHECK(square_int_outgoing.empty());
    const auto square_int_incoming = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{6},
                                                     .method = "callHierarchy/incomingCalls",
                                                     .params = Json{{"item", square_int_item}}});
    CHECK(square_int_incoming.empty());

    // The float overload, in contrast, is called from both `main` and
    // `recurse` -- incomingCalls must report both callers.
    const auto square_float_item = (*to_square_float)["to"];
    const auto square_float_incoming = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{7},
                                                     .method = "callHierarchy/incomingCalls",
                                                     .params = Json{{"item", square_float_item}}});
    REQUIRE(square_float_incoming.is_array());
    CHECK(square_float_incoming.size() == 2);
    CHECK(std::ranges::any_of(square_float_incoming,
                              [](const Json& call) { return call["from"]["name"] == "main"; }));
    CHECK(std::ranges::any_of(square_float_incoming,
                              [](const Json& call) { return call["from"]["name"] == "recurse"; }));

    // Recursion must terminate and be represented rather than dropped: the
    // `recurse` node's own outgoing/incoming calls both include itself.
    const auto recurse_item = (*to_recurse)["to"];
    const auto recurse_outgoing = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{8},
                                                     .method = "callHierarchy/outgoingCalls",
                                                     .params = Json{{"item", recurse_item}}});
    REQUIRE(recurse_outgoing.is_array());
    CHECK(std::ranges::any_of(recurse_outgoing,
                              [](const Json& call) { return call["to"]["name"] == "recurse"; }));
    const auto recurse_incoming = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{9},
                                                     .method = "callHierarchy/incomingCalls",
                                                     .params = Json{{"item", recurse_item}}});
    REQUIRE(recurse_incoming.is_array());
    CHECK(std::ranges::any_of(recurse_incoming,
                              [](const Json& call) { return call["from"]["name"] == "main"; }));
    CHECK(std::ranges::any_of(recurse_incoming,
                              [](const Json& call) { return call["from"]["name"] == "recurse"; }));
}

TEST_CASE("Call hierarchy returns null for non-callable positions and rejects stale items with "
          "ContentModified",
          "[lsp][call-hierarchy][safety]") {
    const auto uri = shader_uri();
    const auto source = call_hierarchy_shader();
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params =
            Json{{"textDocument",
                  {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", source}}}}}));

    // Whitespace between statements is not a callable cursor.
    const auto blank_offset = source.find("\n\n") + 1;
    const auto blank_result =
        server.handle(prepare_call_hierarchy_request(2, uri, position_at(source, blank_offset)));
    REQUIRE(blank_result.has_value());
    if (const auto* error =
            std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*blank_result)) {
        INFO("code=" << error->error.code << " message=" << error->error.message);
        FAIL("Expected a successful response");
    }
    const auto* blank_response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*blank_result);
    REQUIRE(blank_response != nullptr);
    CHECK(blank_response->result.is_null());

    const auto main_offset = source.find("main(");
    const auto main_item = call_hierarchy_result(
        server, prepare_call_hierarchy_request(3, uri, position_at(source, main_offset)))[0];

    // A concurrent edit bumps the document version; the previously prepared
    // item's `data.rootVersion` is now stale and must be rejected rather
    // than silently resolved against whatever is currently at that
    // position.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", uri}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", source}}})}}}));

    const auto stale_outgoing =
        server.handle(hlsl_intellisense::json_rpc::Request{.id = std::int64_t{4},
                                                           .method = "callHierarchy/outgoingCalls",
                                                           .params = Json{{"item", main_item}}});
    REQUIRE(stale_outgoing.has_value());
    const auto* stale_outgoing_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*stale_outgoing);
    REQUIRE(stale_outgoing_error != nullptr);
    CHECK(stale_outgoing_error->error.code == hlsl_intellisense::json_rpc::content_modified_code);

    const auto stale_incoming =
        server.handle(hlsl_intellisense::json_rpc::Request{.id = std::int64_t{5},
                                                           .method = "callHierarchy/incomingCalls",
                                                           .params = Json{{"item", main_item}}});
    REQUIRE(stale_incoming.has_value());
    const auto* stale_incoming_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*stale_incoming);
    REQUIRE(stale_incoming_error != nullptr);
    CHECK(stale_incoming_error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
}

TEST_CASE("Call hierarchy rejects a prepared item as stale after an open include is edited, even "
          "though the root document's own version and the callee's own text are both unchanged",
          "[lsp][call-hierarchy][safety][cross-file]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    const std::string include_text = "float helper(float x) { return x * 2.0; }\n";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << include_text;
    }
    const auto include_uri =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "a.hlsl").string());
    const std::string root_text = "#include \"shared.hlsli\"\n"
                                  "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";

    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", include_uri.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", include_text}}}}}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", root.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", root_text}}}}}));

    const auto call_offset = root_text.find("helper(1.0)");
    const auto helper_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(2, root.uri(), position_at(root_text, call_offset)))[0];
    CHECK(helper_item["name"] == "helper");

    // Only the included file is edited (appending a new, unrelated function
    // after `helper`, so `helper`'s own text/offsets are byte-for-byte
    // identical to what was captured above); the root document's own
    // `didOpen`/`didChange` version is never touched. A staleness check
    // that only compares `data.rootVersion` against the root document's
    // current version, or that only re-validates `helper`'s own stored
    // identity fields (unaffected here since `helper` did not move), would
    // both wrongly treat this item as still valid.
    const std::string edited_include_text =
        include_text + "float unrelatedTrailing(float x) { return x + 1.0; }\n";
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", include_uri.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", edited_include_text}}})}}}));

    const auto stale_outgoing =
        server.handle(hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                           .method = "callHierarchy/outgoingCalls",
                                                           .params = Json{{"item", helper_item}}});
    REQUIRE(stale_outgoing.has_value());
    const auto* stale_outgoing_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*stale_outgoing);
    REQUIRE(stale_outgoing_error != nullptr);
    CHECK(stale_outgoing_error->error.code == hlsl_intellisense::json_rpc::content_modified_code);

    const auto stale_incoming =
        server.handle(hlsl_intellisense::json_rpc::Request{.id = std::int64_t{4},
                                                           .method = "callHierarchy/incomingCalls",
                                                           .params = Json{{"item", helper_item}}});
    REQUIRE(stale_incoming.has_value());
    const auto* stale_incoming_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*stale_incoming);
    REQUIRE(stale_incoming_error != nullptr);
    CHECK(stale_incoming_error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
}

TEST_CASE("Call hierarchy rejects a prepared item as stale after the active variant changes the "
          "effective compiler configuration, even though the document text is unchanged",
          "[lsp][call-hierarchy][safety]") {
    TestDirectory directory;
    {
        std::ofstream config{directory.path() / "shadertoolsconfig.json"};
        REQUIRE(config);
        config << R"({
            "root": true,
            "hlsl.targetProfile": "ps_6_6",
            "hlsl.variantsVersion": 1,
            "hlsl.variants": [
                { "name": "First", "hlsl.entryPoint": "PSMain" },
                { "name": "Second", "hlsl.entryPoint": "PSMainAlt" }
            ]
        })";
        REQUIRE(config);
    }
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    const std::string source = "float helper(float x) { return x * 2.0; }\n"
                               "float4 PSMain(float4 position : SV_Position) : SV_Target {\n"
                               "    return helper(position.x).xxxx;\n"
                               "}\n"
                               "float4 PSMainAlt(float4 position : SV_Position) : SV_Target {\n"
                               "    return helper(position.x * 2.0).xxxx;\n"
                               "}\n";

    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "First"}}}));

    const auto call_offset = source.find("helper(position.x)");
    const auto helper_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(2, document.uri(), position_at(source, call_offset)))[0];
    CHECK(helper_item["name"] == "helper");

    // Switching the active variant changes the effective `-E` compiler
    // argument (PSMain -> PSMainAlt) and therefore the translation unit
    // that gets recompiled, without touching the document's own text or
    // LSP version at all: `helper`'s own stored identity fields (path,
    // start offset, cursor kind, name) are unaffected, since the source
    // text is byte-for-byte identical either way.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Second"}}}));

    const auto stale_outgoing =
        server.handle(hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                           .method = "callHierarchy/outgoingCalls",
                                                           .params = Json{{"item", helper_item}}});
    REQUIRE(stale_outgoing.has_value());
    const auto* stale_outgoing_error =
        std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*stale_outgoing);
    REQUIRE(stale_outgoing_error != nullptr);
    CHECK(stale_outgoing_error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
}

TEST_CASE("Incoming calls expand across every open root that includes the callee, covering "
          "unsaved edits",
          "[lsp][call-hierarchy][cross-file]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "float helper(float x) { return x * 2.0; }\n";
    }
    const auto first = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "a.hlsl").string());
    const auto second = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "b.hlsl").string());
    const std::string first_text = "#include \"shared.hlsli\"\n"
                                   "float4 main() : SV_Target { return helper(1.0).xxxx; }\n";
    const std::string second_text = "#include \"shared.hlsli\"\n"
                                    "float4 alt() : SV_Target { return helper(2.0).xxxx; }\n";

    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] :
         std::array{std::pair{first.uri(), first_text}, std::pair{second.uri(), second_text}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }

    const auto call_offset = first_text.find("helper(1.0)");
    const auto helper_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(2, first.uri(), position_at(first_text, call_offset)))[0];
    CHECK(helper_item["name"] == "helper");

    const auto incoming = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                     .method = "callHierarchy/incomingCalls",
                                                     .params = Json{{"item", helper_item}}});
    REQUIRE(incoming.is_array());
    CHECK(incoming.size() == 2);
    CHECK(std::ranges::any_of(incoming,
                              [](const Json& call) { return call["from"]["name"] == "main"; }));
    CHECK(std::ranges::any_of(incoming,
                              [](const Json& call) { return call["from"]["name"] == "alt"; }));
}

TEST_CASE("Incoming calls keep two roots' results for the very same caller location distinct, "
          "each with its own root metadata and without duplicating call-site ranges",
          "[lsp][call-hierarchy][cross-file]") {
    // Regression: `caller` is defined exactly once, in a header included by
    // two different roots, so its own (path, start offset) is identical no
    // matter which root's translation unit resolved it. Two different
    // roots (different translation-unit/compiler contexts, and therefore
    // different `rootIdentity`/`generation` pairs even though `caller`'s
    // text never moves) both legitimately report `caller` as an incoming
    // caller of `helper` -- keying accumulation only by (path, offset)
    // would wrongly fold these into a single entry, silently keeping just
    // the first root's metadata while splicing in call sites resolved
    // under the *other* root's context, and -- since `caller`'s own call to
    // `helper` is at an identical offset from both roots' perspective --
    // duplicating what should be a single range within that one merged
    // entry. Each root's contribution must instead survive as its own
    // distinct entry, and neither entry may show a duplicated range.
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    const std::string include_text = "float helper(float x) { return x * 2.0; }\n"
                                     "float caller(float x) { return helper(x) + 1.0; }\n";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << include_text;
    }
    const auto first = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "a.hlsl").string());
    const auto second = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "b.hlsl").string());
    const std::string first_text =
        "#include \"shared.hlsli\"\n"
        "float4 main() : SV_Target { return (caller(1.0) + helper(3.0)).xxxx; }\n";
    const std::string second_text =
        "#include \"shared.hlsli\"\n"
        "float4 main() : SV_Target { return (caller(2.0) + helper(4.0)).xxxx; }\n";

    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] :
         std::array{std::pair{first.uri(), first_text}, std::pair{second.uri(), second_text}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }

    const auto call_offset = first_text.find("helper(3.0)");
    const auto helper_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(2, first.uri(), position_at(first_text, call_offset)))[0];
    CHECK(helper_item["name"] == "helper");

    const auto incoming = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                     .method = "callHierarchy/incomingCalls",
                                                     .params = Json{{"item", helper_item}}});
    REQUIRE(incoming.is_array());
    // `caller` (once per root, kept distinct) plus `main` (once per root,
    // always distinct since each root's own `main` is a different file).
    CHECK(incoming.size() == 4);

    std::vector<Json> caller_entries;
    for (const auto& call : incoming) {
        if (call["from"]["name"] == "caller") {
            caller_entries.push_back(call);
        }
    }
    REQUIRE(caller_entries.size() == 2);
    // Each root's `caller` entry must carry *that root's own* metadata, not
    // a metadata field copied from whichever root happened to be processed
    // first.
    const auto& first_root_identity = caller_entries[0]["from"]["data"]["rootIdentity"];
    const auto& second_root_identity = caller_entries[1]["from"]["data"]["rootIdentity"];
    CHECK(first_root_identity != second_root_identity);
    CHECK(std::ranges::any_of(caller_entries, [&](const Json& call) {
        return call["from"]["data"]["rootUri"] == first.uri();
    }));
    CHECK(std::ranges::any_of(caller_entries, [&](const Json& call) {
        return call["from"]["data"]["rootUri"] == second.uri();
    }));
    // Neither root's entry may show `caller`'s single call to `helper`
    // duplicated, even though that call site's own offset is identical
    // across both roots (it is the very same physical text).
    for (const auto& call : caller_entries) {
        REQUIRE(call["fromRanges"].is_array());
        CHECK(call["fromRanges"].size() == 1);
    }

    CHECK(std::ranges::count_if(
              incoming, [](const Json& call) { return call["from"]["name"] == "main"; }) == 2);
}

TEST_CASE("Incoming calls reject a candidate root's contribution when that root is reanalyzed "
          "strictly between the item's staleness check and its own query, even though the "
          "document's own version never changes",
          "[lsp][call-hierarchy][safety][concurrency]") {
    // Regression for a generation-based TOCTOU: `call_hierarchy_incoming_calls` validates
    // `data` once up front (capturing the generation the item was built against) and then,
    // per candidate root, issues a *separately timed* `incoming_calls` query. For the
    // candidate root that *is* the item's own root, nothing previously checked that this
    // second query's own generation still matched what validation confirmed -- an
    // included-file edit reanalyzing that root in between (without bumping its own document
    // version) could silently serve results computed against a newer analysis than the one
    // `data` was validated against.
    //
    // This test previously reproduced the window by pausing an *unrelated second root*
    // inside `AnalysisHooks::before_interactive` -- which fires on a `Manager` scheduler
    // worker thread, not the calling thread -- while the target root's own reanalysis ran
    // concurrently on a different worker. That design held a scheduler worker hostage for
    // the whole pause and, more importantly, guaranteed two roots' DXC-backed analyses were
    // genuinely in flight at the same time; that combination reproducibly crashed DXC's
    // IntelliSense implementation on Linux CI (SIGSEGV), even after bounding the worker
    // count could not resolve it, because the real hazard was concurrent DXC operations
    // across roots, not thread-pool size.
    //
    // This redesign uses a *single* root (plus one include) and pauses via
    // `AnalysisHooks::before_call_hierarchy_candidate_root` instead, which fires once per
    // candidate root at the very top of `call_hierarchy_incoming_calls`'s own per-root loop
    // -- strictly before that root's live snapshot/query -- but, critically, runs on the
    // *calling* (request-handling) thread, exactly like `before_call_hierarchy_revalidation`
    // (see `RevalidationFixture` below, which relies on the same "runs on the calling
    // thread, not a worker" property). Pausing there therefore never occupies a scheduler
    // worker, and with only one root total there is never a second root's analysis running
    // concurrently: the root's own reanalysis (triggered by the include edit below) runs to
    // completion entirely on its own, uncontended worker while the paused request thread is
    // blocked purely on a `std::promise`, not inside any DXC call.
    TestDirectory directory;
    const auto include_path = directory.path() / "helper.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "float helper(float x) { return x * 2.0; }\n";
    }
    const auto root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "root.hlsl").string());
    const auto include_uri =
        hlsl_intellisense::workspace::DocumentUri::from_path(include_path.string());
    const std::string root_text = "#include \"helper.hlsli\"\n"
                                  "float caller(float x) { return helper(x) + 1.0; }\n"
                                  "float4 main() : SV_Target { return caller(1.0).xxxx; }\n";
    const std::string include_text_before = "float helper(float x) { return x * 2.0; }\n";
    const std::string include_text_after = "float helper(float x) { return x * 2.0 + 1.0; }\n";

    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> paused_once{false};
    const auto root_identity = root.identity();
    hooks->before_call_hierarchy_candidate_root = [&](std::string_view identity) {
        if (identity == root_identity && !paused_once.exchange(true)) {
            entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] : std::array{std::pair{root.uri(), root_text},
                                              std::pair{include_uri.uri(), include_text_before}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }
    server.wait_for_analysis();

    const auto call_offset = root_text.find("helper(x)");
    const auto helper_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(2, root.uri(), position_at(root_text, call_offset)))[0];
    CHECK(helper_item["name"] == "helper");
    const std::uint64_t original_generation = helper_item["data"]["generation"];

    auto response = std::async(std::launch::async, [&] {
        return server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/incomingCalls",
                                                 .params = Json{{"item", helper_item}}});
    });
    entered.get_future().wait();

    // Edited while the loop is paused right before it would otherwise take this root's own
    // document snapshot: this reanalysis runs and completes on its own scheduler worker with
    // no other DXC operation in flight anywhere -- the only other activity right now is the
    // async thread above, blocked purely on a `std::promise`.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", include_uri.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", include_text_after}}})}}}));
    server.wait_for_analysis();

    // An ordinary (unpaused) prepare query for the same call site confirms the reanalysis
    // above actually changed the root's generation, independent of whatever the still-paused
    // incoming-calls request later observes.
    const auto reanalyzed_item = call_hierarchy_result(
        server,
        prepare_call_hierarchy_request(4, root.uri(), position_at(root_text, call_offset)))[0];
    const std::uint64_t reanalyzed_generation = reanalyzed_item["data"]["generation"];
    REQUIRE(reanalyzed_generation != original_generation);

    release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    server.wait_for_analysis();
}

TEST_CASE("Incoming calls treat a root with pending (placeholder) dependency metadata as a "
          "candidate instead of silently skipping it",
          "[lsp][call-hierarchy][safety][concurrency]") {
    // Regression: `Manager::analyze` publishes a placeholder `RootMetadata`
    // (empty `dependency_identities`, `has_dynamic_includes = true`) the
    // instant a reanalysis is *admitted* by the scheduler -- strictly
    // before that reanalysis actually runs and computes the root's real
    // dependency set (see `Manager::analyze`'s `admitted` callback and
    // `Impl::analyze`'s own `before_analysis` hook call at its very start,
    // before include resolution). `call_hierarchy_incoming_calls`
    // deliberately queries every currently open root unconditionally (see
    // that function's own comment on why any dependency-based candidate
    // filter -- including one that special-cased `has_dynamic_includes` --
    // was rejected in favor of this), so a root whose reanalysis is merely
    // *queued*, not yet finished, must still be queried and must still
    // contribute its caller once analysis catches up. This reproduces that
    // window deterministically: a reanalysis of the caller root (same
    // text, only the version bumps, so the call relationship itself never
    // changes) is queued and paused via `AnalysisHooks::before_analysis` --
    // which fires at the very start of the real analyze work -- strictly
    // before the incoming-calls request is even issued, so `Manager::
    // roots()` is guaranteed to still report the placeholder for that root
    // when this request queries it.
    TestDirectory directory;
    const auto helper_path = directory.path() / "helper.hlsli";
    const std::string helper_text = "float helper(float x) { return x * 2.0; }\n";
    {
        std::ofstream include{helper_path};
        REQUIRE(include);
        include << helper_text;
    }
    const auto helper = hlsl_intellisense::workspace::DocumentUri::from_path(helper_path.string());

    // The scheduler pins ALL work (both background reanalysis and
    // interactive queries) for a given root to a single worker, chosen by
    // hashing `root_identity` with the very same FNV-1a function
    // `Scheduler::owner_for` uses. If `caller_root` and `helper` happened
    // to hash to the same worker, pausing the caller root's background
    // analyze() would also block `helper`'s own (already fully analyzed)
    // self-query, since they'd share a worker. Rather than growing
    // `worker_count` until a *fixed* pair of identities happens to
    // separate -- unbounded in the worst case, since every identity's hash
    // also depends on the host's containing temp/work directory path,
    // which can make the search never terminate within a sane bound on
    // some hosts/CI configurations and spin up an enormous,
    // resource-exhausting thread pool -- `worker_count` is kept fixed at a
    // small constant, and instead a small, bounded number of candidate
    // names for the caller root are tried, stopping at the first one whose
    // identity hash lands on a different worker than `helper`'s own
    // (fixed) root.
    constexpr std::size_t worker_count{2};
    const auto helper_identity = helper.identity();
    const auto helper_hash = fnv1a_hash(helper_identity);
    std::optional<hlsl_intellisense::workspace::DocumentUri> caller_root;
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto candidate = hlsl_intellisense::workspace::DocumentUri::from_path(
            (directory.path() / ("callerRoot" + std::to_string(attempt) + ".hlsl")).string());
        if (fnv1a_hash(candidate.identity()) % worker_count != helper_hash % worker_count) {
            caller_root = std::move(candidate);
            break;
        }
    }
    REQUIRE(caller_root.has_value());
    const auto caller_identity = caller_root->identity();
    const std::string caller_text =
        "#include \"helper.hlsli\"\nfloat callerFn(float x) { return helper(x) + 1.0; }\n";

    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> paused_once{false};
    // Guarded by `version == 2`, not merely `identity == caller_identity`:
    // the caller root's own *initial* analyze (version 1, triggered by
    // `textDocument/didOpen` + the `wait_for_analysis()` below) would
    // otherwise also match and pause forever, since nothing releases the
    // hook until much later in this test.
    hooks->before_analysis = [&](std::string_view identity, std::int64_t version) {
        if (identity == caller_identity && version == 2 && !paused_once.exchange(true)) {
            entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    options.analysis.scheduler.worker_count = worker_count;
    options.analysis.scheduler.queue_capacity = std::max<std::size_t>(128, worker_count * 4);
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] : std::array{std::pair{helper.uri(), helper_text},
                                              std::pair{caller_root->uri(), caller_text}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }
    server.wait_for_analysis();

    // Prepared from `helper`'s own root/definition (never edited during the
    // race below), so the item's own root/version staleness check can
    // never itself be the reason a later assertion fails.
    const auto helper_item = call_hierarchy_result(
        server, prepare_call_hierarchy_request(
                    2, helper.uri(), position_at(helper_text, helper_text.find("helper"))))[0];
    CHECK(helper_item["name"] == "helper");

    // Queue (and, via the hook above, pause) a reanalysis of the caller
    // root: identical text, version bump only. `Manager::roots()` reports
    // the placeholder metadata for this root from this call's return until
    // `release` is fulfilled below.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", caller_root->uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", caller_text}}})}}}));
    entered.get_future().wait();

    auto response = std::async(std::launch::async, [&] {
        return server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/incomingCalls",
                                                 .params = Json{{"item", helper_item}}});
    });

    release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    if (const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result)) {
        INFO("code=" << error->error.code << " message=" << error->error.message);
        FAIL("Expected a successful response");
    }
    const auto* success = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
    REQUIRE(success != nullptr);
    const auto& incoming = success->result;
    REQUIRE(incoming.is_array());
    // The caller root must have actually been queried and contributed its
    // caller (there is no filter left to skip it on) once its reanalysis --
    // awaited via the paused hook above -- caught up with its real
    // dependency set.
    CHECK(std::ranges::any_of(incoming,
                              [](const Json& call) { return call["from"]["name"] == "callerFn"; }));
    server.wait_for_analysis();
}

TEST_CASE("Incoming calls final revalidation covers a candidate root that legitimately "
          "contributed zero callers at query time",
          "[lsp][call-hierarchy][safety][concurrency]") {
    // Regression: the final post-construction revalidation pass previously
    // derived its set of roots to recheck purely from `ordered`/`accumulated`
    // -- the accumulated *caller* entries -- so a candidate root that was
    // genuinely queried but returned zero callers was never revalidated at
    // all. A concurrent edit that adds a brand-new call from that root,
    // landing strictly between its own (empty) query and the final
    // revalidation pass, could then have its now-stale "no callers from
    // this root" silently stand instead of being rejected as
    // `ContentModified`. This reproduces that window using the existing
    // `before_call_hierarchy_revalidation` test hook (which fires after the
    // full response is constructed but before the final revalidation loop):
    // the candidate root below has zero calls to `helper` when queried, is
    // then edited (synchronously awaited) to add one while the hook is
    // paused, and the final revalidation must still catch that this
    // candidate root's generation has changed.
    TestDirectory directory;
    const auto helper_path = directory.path() / "helper.hlsli";
    const std::string helper_text = "float helper(float x) { return x * 2.0; }\n";
    {
        std::ofstream include{helper_path};
        REQUIRE(include);
        include << helper_text;
    }
    const auto helper = hlsl_intellisense::workspace::DocumentUri::from_path(helper_path.string());
    const auto candidate_root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "candidateRoot.hlsl").string());
    const std::string candidate_text_before =
        "#include \"helper.hlsli\"\nfloat4 main() : SV_Target { return 1.0.xxxx; }\n";
    const std::string candidate_text_after = "#include \"helper.hlsli\"\n"
                                             "float callerFn(float x) { return helper(x); }\n"
                                             "float4 main() : SV_Target { return 1.0.xxxx; }\n";

    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> paused_once{false};
    // Not assigned to `hooks->before_call_hierarchy_revalidation` yet: that
    // hook fires on *every* prepare/outgoing/incoming call this server
    // handles, including the plain `prepareCallHierarchy` setup call below
    // used only to obtain `helper_item` -- arming it that early would pause
    // forever on that unrelated call instead of the incoming-calls request
    // under test. Armed via `arm_pause()` immediately before that request is
    // launched instead.
    const auto arm_pause = [&] {
        hooks->before_call_hierarchy_revalidation = [&] {
            if (!paused_once.exchange(true)) {
                entered.set_value();
                released.wait();
            }
        };
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] :
         std::array{std::pair{helper.uri(), helper_text},
                    std::pair{candidate_root.uri(), candidate_text_before}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }
    server.wait_for_analysis();

    const auto helper_item = call_hierarchy_result(
        server, prepare_call_hierarchy_request(
                    2, helper.uri(), position_at(helper_text, helper_text.find("helper"))))[0];
    CHECK(helper_item["name"] == "helper");

    arm_pause();
    auto response = std::async(std::launch::async, [&] {
        return server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/incomingCalls",
                                                 .params = Json{{"item", helper_item}}});
    });
    entered.get_future().wait();

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", candidate_root.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", candidate_text_after}}})}}}));
    server.wait_for_analysis();

    release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    server.wait_for_analysis();
}

TEST_CASE("Incoming calls query a root even though its analysis_.roots() metadata snapshot "
          "showed it as unrelated to the target, and pick up a caller added to it after that "
          "snapshot was taken",
          "[lsp][call-hierarchy][safety][concurrency]") {
    // Regression: `call_hierarchy_incoming_calls` takes a single, one-shot
    // `analysis_.roots()` call at the top of its loop -- a point-in-time
    // *copy* of every root's metadata (`dependency_identities`,
    // `has_dynamic_includes`). An earlier version of this handler used that
    // copy to decide, per root, whether to even bother querying it. A root
    // genuinely unrelated to the target at the moment of that copy (empty
    // `dependency_identities`, `has_dynamic_includes == false`, e.g. it has
    // no `#include`s at all yet) can be edited an instant later -- to add
    // an `#include` of the target's file and a brand-new call to it --
    // concurrently with, or strictly between, that metadata copy and the
    // loop reaching this root's own turn. A filter keyed on the stale copy
    // would skip this root forever, silently omitting a caller that is
    // genuinely reachable through it under its *current* content. The fix
    // removes any such filter: every currently open root is queried
    // unconditionally, and (per-root) against a *freshly re-read* document
    // snapshot and a fresh `analyze_and_publish`, not against the
    // `roots()` copy's own point-in-time state. This test reproduces the
    // race using the `before_call_hierarchy_candidate_root` hook, which
    // fires at the very top of this root's own loop iteration -- strictly
    // before its live document snapshot is taken -- to deterministically
    // land the edit (and await its full reanalysis) inside that window,
    // then asserts the resulting response still contains the new caller.
    TestDirectory directory;
    const auto helper_path = directory.path() / "helper.hlsli";
    const std::string helper_text = "float helper(float x) { return x * 2.0; }\n";
    {
        std::ofstream include{helper_path};
        REQUIRE(include);
        include << helper_text;
    }
    const auto helper = hlsl_intellisense::workspace::DocumentUri::from_path(helper_path.string());
    const auto other_root = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "otherRoot.hlsl").string());
    // No `#include` at all: genuinely not dependent on `helper.hlsli` at
    // the moment `analysis_.roots()` is snapshotted below -- not merely
    // "not yet analyzed" (that race is the *other*, already-fixed,
    // placeholder-metadata regression above).
    const std::string other_text_before = "float4 main() : SV_Target { return 1.0.xxxx; }\n";
    const std::string other_text_after = "#include \"helper.hlsli\"\n"
                                         "float otherCallerFn(float x) { return helper(x); }\n"
                                         "float4 main() : SV_Target { return 1.0.xxxx; }\n";

    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> paused_once{false};
    const auto other_identity = other_root.identity();
    // Guarded to only match `other_root`'s own turn in the loop: this hook
    // fires once per candidate root (including `helper`'s own root), and
    // must not pause on any of the others.
    hooks->before_call_hierarchy_candidate_root = [&](std::string_view identity) {
        if (identity == other_identity && !paused_once.exchange(true)) {
            entered.set_value();
            released.wait();
        }
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    for (const auto& [uri, text] : std::array{std::pair{helper.uri(), helper_text},
                                              std::pair{other_root.uri(), other_text_before}}) {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didOpen",
            .params =
                Json{{"textDocument",
                      {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
    }
    server.wait_for_analysis();

    const auto helper_item = call_hierarchy_result(
        server, prepare_call_hierarchy_request(
                    2, helper.uri(), position_at(helper_text, helper_text.find("helper"))))[0];
    CHECK(helper_item["name"] == "helper");

    auto response = std::async(std::launch::async, [&] {
        return server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/incomingCalls",
                                                 .params = Json{{"item", helper_item}}});
    });
    entered.get_future().wait();

    // Edit `other_root` to add the include and the new call while the loop
    // is paused right before it would otherwise take `other_root`'s
    // document snapshot; fully await this edit's own reanalysis before
    // releasing, so the paused iteration is guaranteed to observe the
    // *post-edit* content once it resumes.
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params = Json{{"textDocument", {{"uri", other_root.uri()}, {"version", 2}}},
                       {"contentChanges", Json::array({Json{{"text", other_text_after}}})}}}));
    server.wait_for_analysis();

    release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    if (const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result)) {
        INFO("code=" << error->error.code << " message=" << error->error.message);
        FAIL("Expected a successful response");
    }
    const auto* success = std::get_if<hlsl_intellisense::json_rpc::Response>(&*result);
    REQUIRE(success != nullptr);
    const auto& incoming = success->result;
    REQUIRE(incoming.is_array());
    CHECK(std::ranges::any_of(
        incoming, [](const Json& call) { return call["from"]["name"] == "otherCallerFn"; }));
    server.wait_for_analysis();
}

TEST_CASE("Incoming-calls accumulation key treats distinct (path, offset, root identity) "
          "triples as distinct even when they would collide under naive ':'-joined string "
          "concatenation",
          "[lsp][call-hierarchy][safety]") {
    // Regression: `Server::call_hierarchy_incoming_calls` used to key its
    // per-caller accumulation map on a single delimiter-joined string
    // (`path + ':' + std::to_string(start_offset) + ':' + root_identity`).
    // That join is not injective: both `path` and `root_identity` are
    // arbitrary strings (in production, filesystem paths/identities) that
    // can themselves contain ':', so two entirely different triples can
    // produce the identical joined string -- silently merging two
    // unrelated callers' `call_sites` into a single accumulated entry. The
    // fix replaces the joined string with a structured
    // (path, start_offset, root_identity) key compared/hashed field-by-
    // field, which has no such ambiguity.
    //
    // A real, protocol-level collision through `callHierarchy/
    // incomingCalls` is impractical to construct on this platform: real
    // document paths are constrained by the filesystem (Windows disallows
    // ':' anywhere in a path component except the drive-letter separator),
    // so no two genuine, distinct document paths/identities can be made to
    // collide this way here. This test instead directly exercises a key
    // type -- structurally identical to `AccumulatedKey`/
    // `AccumulatedKeyHash` in `call_hierarchy_incoming_calls` (same fields,
    // same equality, same hash-combining scheme) -- against a pair of
    // triples deliberately chosen to collide under the old, rejected
    // string-concatenation scheme, proving the structured key keeps them
    // distinct.
    struct AccumulatedKey {
        std::string path;
        std::uint32_t start_offset{};
        std::string root_identity;

        [[nodiscard]] bool operator==(const AccumulatedKey&) const = default;
    };
    struct AccumulatedKeyHash {
        [[nodiscard]] std::size_t operator()(const AccumulatedKey& key) const noexcept {
            std::size_t seed = std::hash<std::string>{}(key.path);
            seed ^= std::hash<std::uint32_t>{}(key.start_offset) + 0x9e3779b9 + (seed << 6) +
                    (seed >> 2);
            seed ^= std::hash<std::string>{}(key.root_identity) + 0x9e3779b9 + (seed << 6) +
                    (seed >> 2);
            return seed;
        }
    };

    // Both triples below produce the identical legacy joined string
    // "foo:5:bar:6:baz" -- the first via path="foo", offset=5,
    // root_identity="bar:6:baz"; the second via path="foo:5:bar", offset=6,
    // root_identity="baz" -- purely because `root_identity` (first triple)
    // and `path` (second triple) each themselves contain ':'. They are
    // nonetheless two entirely different triples and must never be treated
    // as the same accumulation entry.
    const AccumulatedKey first{.path = "foo", .start_offset = 5, .root_identity = "bar:6:baz"};
    const AccumulatedKey second{.path = "foo:5:bar", .start_offset = 6, .root_identity = "baz"};

    const auto legacy_joined_key = [](const AccumulatedKey& key) {
        return key.path + ':' + std::to_string(key.start_offset) + ':' + key.root_identity;
    };
    // Confirms the premise: this pair genuinely would have collided under
    // the rejected scheme.
    REQUIRE(legacy_joined_key(first) == legacy_joined_key(second));
    REQUIRE_FALSE(first == second);

    std::unordered_map<AccumulatedKey, int, AccumulatedKeyHash> accumulated;
    accumulated.emplace(first, 1);
    accumulated.emplace(second, 2);

    // With the structured key, both triples get their own distinct
    // accumulation entry instead of the second silently colliding with
    // (and appearing to merge into) the first.
    CHECK(accumulated.size() == 2);
    CHECK(accumulated.at(first) == 1);
    CHECK(accumulated.at(second) == 2);
}

namespace {

// Shared setup for the three "revalidate after full response construction"
// regressions below (prepare/outgoing/incoming): a root that `#include`s a
// helper definition, calls it from both `caller` (for outgoing/incoming) and
// again directly (for the prepare-at-a-call-site pattern the rest of this
// file already uses). Editing the include bumps the root's content
// generation without ever touching the root document's own version, so a
// document-version-only check could never observe it -- only the
// generation-based recheck these three tests target can.
struct RevalidationFixture {
    TestDirectory directory;
    hlsl_intellisense::workspace::DocumentUri root;
    hlsl_intellisense::workspace::DocumentUri include;
    std::string root_text;
    std::shared_ptr<hlsl_intellisense::analysis::AnalysisHooks> hooks;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released;
    std::atomic<bool> paused_once{false};
    hlsl_intellisense::lsp::Server server;

    explicit RevalidationFixture(std::shared_ptr<hlsl_intellisense::analysis::AnalysisHooks> hooks_)
        : root{hlsl_intellisense::workspace::DocumentUri::from_path(
              (directory.path() / "root.hlsl").string())},
          include{hlsl_intellisense::workspace::DocumentUri::from_path(
              (directory.path() / "helper.hlsli").string())},
          root_text{"#include \"helper.hlsli\"\n"
                    "float caller(float x) { return helper(x) + 1.0; }\n"
                    "float4 main() : SV_Target { return caller(1.0).xxxx; }\n"},
          hooks{std::move(hooks_)}, released{release.get_future().share()},
          server{[](const auto&) {},
                 {},
                 [&] {
                     hlsl_intellisense::lsp::ServerOptions options;
                     options.background_analysis = true;
                     options.analysis_hooks = hooks;
                     return options;
                 }()} {
        // Not armed here: the hook is shared across *every* prepare/
        // outgoing/incoming call this fixture's `server` ever handles, and
        // some tests need one or more un-paused calls first (e.g. to
        // obtain the `CallHierarchyItem` an outgoing/incoming request
        // needs) before the specific call under test should pause. Call
        // `arm()` once that setup is done.
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
            .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "initialized", .params = Json::object()}));
        for (const auto& [uri, text] :
             std::array{std::pair{root.uri(), root_text},
                        std::pair{include.uri(),
                                  std::string{"float helper(float x) { return x * 2.0; }\n"}}}) {
            static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
                .method = "textDocument/didOpen",
                .params = Json{
                    {"textDocument",
                     {{"uri", uri}, {"languageId", "hlsl"}, {"version", 1}, {"text", text}}}}}));
        }
        server.wait_for_analysis();
    }

    // Arms the pause: the *next* prepare/outgoing/incoming call this
    // fixture's `server` handles (and only that one) will block inside
    // `before_call_hierarchy_revalidation` until `release` is fulfilled.
    void arm() {
        hooks->before_call_hierarchy_revalidation = [this] {
            if (!paused_once.exchange(true)) {
                entered.set_value();
                released.wait();
            }
        };
    }

    // Edits the include (bumping the root's generation without bumping the
    // root document's own version) and blocks until that reanalysis has
    // actually finished, while the paused request above sits inside the
    // hook -- the hook itself runs on the calling (async) thread, not on
    // any `Manager` scheduler worker, so it cannot block this reanalysis.
    void reanalyze_include() {
        static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
            .method = "textDocument/didChange",
            .params = Json{{"textDocument", {{"uri", include.uri()}, {"version", 2}}},
                           {"contentChanges",
                            Json::array({Json{{"text", "float helper(float x) { return x * 2.0 + "
                                                       "1.0; }\n"}}})}}}));
        server.wait_for_analysis();
    }
};

} // namespace

TEST_CASE("Prepare call hierarchy rejects a response constructed against an analysis that was "
          "superseded strictly between item construction and the final revalidation check",
          "[lsp][call-hierarchy][safety][concurrency]") {
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    RevalidationFixture fixture{hooks};

    const auto call_offset = fixture.root_text.find("helper(x)");
    fixture.arm();
    auto response = std::async(std::launch::async, [&] {
        return fixture.server.handle(prepare_call_hierarchy_request(
            2, fixture.root.uri(), position_at(fixture.root_text, call_offset)));
    });
    fixture.entered.get_future().wait();

    fixture.reanalyze_include();
    fixture.release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    fixture.server.wait_for_analysis();
}

TEST_CASE("Outgoing calls reject a response constructed against an analysis that was superseded "
          "strictly between the query and the final revalidation check",
          "[lsp][call-hierarchy][safety][concurrency]") {
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    RevalidationFixture fixture{hooks};

    // Prepare `caller`'s own item first, *before* arming the pause: the
    // hook fires on every prepare/outgoing/incoming call this fixture's
    // server handles, so obtaining the item must happen while unarmed or
    // this synchronous call itself would block forever waiting on
    // `released`.
    const auto caller_name_offset = fixture.root_text.find("caller");
    const auto caller_item = call_hierarchy_result(
        fixture.server,
        prepare_call_hierarchy_request(2, fixture.root.uri(),
                                       position_at(fixture.root_text, caller_name_offset)))[0];
    REQUIRE(caller_item["name"] == "caller");

    fixture.arm();
    auto response = std::async(std::launch::async, [&] {
        return fixture.server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/outgoingCalls",
                                                 .params = Json{{"item", caller_item}}});
    });
    fixture.entered.get_future().wait();

    fixture.reanalyze_include();
    fixture.release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    fixture.server.wait_for_analysis();
}

TEST_CASE("Incoming calls reject a response constructed against an analysis that was superseded "
          "strictly between the final per-root loop and the final revalidation check",
          "[lsp][call-hierarchy][safety][concurrency]") {
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    RevalidationFixture fixture{hooks};

    const auto call_offset = fixture.root_text.find("helper(x)");
    const auto helper_item = call_hierarchy_result(
        fixture.server, prepare_call_hierarchy_request(
                            2, fixture.root.uri(), position_at(fixture.root_text, call_offset)))[0];
    REQUIRE(helper_item["name"] == "helper");

    fixture.arm();
    auto response = std::async(std::launch::async, [&] {
        return fixture.server.handle(
            hlsl_intellisense::json_rpc::Request{.id = std::int64_t{3},
                                                 .method = "callHierarchy/incomingCalls",
                                                 .params = Json{{"item", helper_item}}});
    });
    fixture.entered.get_future().wait();

    fixture.reanalyze_include();
    fixture.release.set_value();

    const auto result = response.get();
    REQUIRE(result.has_value());
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::content_modified_code);
    fixture.server.wait_for_analysis();
}

namespace {

void write_entry_point_data_flow_config(const TestDirectory& directory) {
    std::ofstream config{directory.path() / "shadertoolsconfig.json"};
    REQUIRE(config);
    config << R"({
        "root": true,
        "hlsl.targetProfile": "ps_6_6",
        "hlsl.variantsVersion": 1,
        "hlsl.variants": [
            { "name": "Prod", "description": "Production entry point",
              "hlsl.entryPoint": "PSMain" }
        ]
    })";
    REQUIRE(config);
}

[[nodiscard]] std::string entry_point_data_flow_shader() {
    return "Texture2D<float4> InputTexture : register(t0);\n"
           "SamplerState InputSampler : register(s0);\n"
           "static float scratch;\n"
           "\n"
           "float square(float x) { return x * x; }\n"
           "int square(int x) { return x * x; }\n"
           "\n"
           "float unusedHelper(float x) { return x + 1.0; }\n"
           "\n"
           "float4 PSMain(float4 position : SV_Position) : SV_Target {\n"
           "    scratch = position.x;\n"
           "    float useScratch = scratch;\n"
           "    float4 sampled = InputTexture.Sample(InputSampler, position.xy);\n"
           "    return sampled * square(useScratch);\n"
           "}\n";
}

} // namespace

TEST_CASE("hlsl/entryPointDataFlow reports no configured entry point before a variant is selected",
          "[lsp][entry-point-data-flow]") {
    TestDirectory directory;
    write_entry_point_data_flow_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", entry_point_data_flow_shader()}}}}}));

    const auto response = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{
                    .id = std::int64_t{2},
                    .method = "hlsl/entryPointDataFlow",
                    .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    INFO(response.dump());
    CHECK(response["found"] == false);
    CHECK_FALSE(response["explanation"].get<std::string>().empty());
    CHECK(response["entryPoint"].is_null());
    CHECK(response["reachableFunctions"].empty());
    CHECK(response["unreachableFunctions"].empty());
    CHECK(response["globalAccesses"].empty());
    CHECK(response["truncated"] == false);
    CHECK(response["functionsVisitedTruncated"] == false);
    CHECK(response["globalAccessesTruncated"] == false);
    CHECK(response["unusedDeclarationsTruncated"] == false);
}

TEST_CASE("hlsl/entryPointDataFlow traces reachable functions and conservative global/resource "
          "access for the active variant's entry point",
          "[lsp][entry-point-data-flow][integration]") {
    TestDirectory directory;
    write_entry_point_data_flow_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", entry_point_data_flow_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Prod"}}}));

    const auto response = call_hierarchy_result(
        server, hlsl_intellisense::json_rpc::Request{
                    .id = std::int64_t{2},
                    .method = "hlsl/entryPointDataFlow",
                    .params = Json{{"textDocument", {{"uri", document.uri()}}}}});
    INFO(response.dump());
    REQUIRE(response["found"] == true);
    REQUIRE(!response["entryPoint"].is_null());
    CHECK(response["entryPoint"]["name"] == "PSMain");
    CHECK(response["truncated"] == false);
    CHECK(response["functionsVisitedTruncated"] == false);
    CHECK(response["globalAccessesTruncated"] == false);
    CHECK(response["unusedDeclarationsTruncated"] == false);
    CHECK(response["functionsVisited"].get<std::size_t>() >= 2);

    const auto& reachable = response["reachableFunctions"];
    CHECK(std::ranges::any_of(reachable, [](const Json& node) {
        return node["function"]["name"] == "PSMain" && node["depth"] == 0;
    }));
    CHECK(std::ranges::any_of(reachable, [](const Json& node) {
        const std::string detail = node["function"]["detail"].get<std::string>();
        return detail.find("float square") != std::string::npos && node["depth"] == 1;
    }));

    const auto& unreachable = response["unreachableFunctions"];
    CHECK(std::ranges::any_of(
        unreachable, [](const Json& item) { return item["detail"] == "int square(int x)"; }));
    CHECK(std::ranges::any_of(unreachable,
                              [](const Json& item) { return item["name"] == "unusedHelper"; }));

    const auto& unused = response["unusedDeclarations"];
    CHECK(std::ranges::any_of(unused,
                              [](const Json& item) { return item["name"] == "unusedHelper"; }));
    // Only the int overload of `square` has zero references anywhere in the
    // snapshot (the float overload is called from PSMain), so it is the
    // sole "square" entry reported here.
    CHECK(std::ranges::any_of(unused, [](const Json& item) { return item["name"] == "square"; }));

    const auto& accesses = response["globalAccesses"];
    const auto texture_access = std::ranges::find_if(
        accesses, [](const Json& access) { return access["name"] == "InputTexture"; });
    REQUIRE(texture_access != accesses.end());
    CHECK((*texture_access)["access"] == "read");
    const auto scratch_access = std::ranges::find_if(
        accesses, [](const Json& access) { return access["name"] == "scratch"; });
    REQUIRE(scratch_access != accesses.end());
    CHECK((*scratch_access)["access"] == "readWrite");

    // Each reported node carries navigation-ready location data usable by
    // either editor client (VS Code, Visual Studio) without further
    // round-trips.
    CHECK(response["entryPoint"]["uri"] == document.uri());
    REQUIRE(response["entryPoint"]["range"]["start"].contains("line"));
    REQUIRE((*texture_access)["uri"] == document.uri());
    REQUIRE((*texture_access)["selectionRange"]["start"].contains("character"));
}

TEST_CASE("Server cancellation returns RequestCancelled for hlsl/entryPointDataFlow",
          "[lsp][entry-point-data-flow][cancellation]") {
    TestDirectory directory;
    write_entry_point_data_flow_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "shader.hlsl").string());
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", entry_point_data_flow_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Prod"}}}));
    server.wait_for_analysis();

    const hlsl_intellisense::json_rpc::Request request{
        .id = std::string{"entry-point-data-flow"},
        .method = "hlsl/entryPointDataFlow",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}};
    const auto cancellation = server.begin_request(request.id);
    auto response =
        std::async(std::launch::async, [&] { return server.handle(request, cancellation); });
    entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "$/cancelRequest", .params = Json{{"id", "entry-point-data-flow"}}}));

    const auto result = response.get();
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);
    release.set_value();
    server.wait_for_analysis();
}

namespace {

void write_compute_visualization_config(const TestDirectory& directory,
                                        std::string_view target_profile = "cs_6_6",
                                        std::string_view entry_point = "CSMain") {
    std::ofstream config{directory.path() / "shadertoolsconfig.json"};
    REQUIRE(config);
    config << "{\n"
              "  \"root\": true,\n"
              "  \"hlsl.targetProfile\": \""
           << target_profile
           << "\",\n"
              "  \"hlsl.variantsVersion\": 1,\n"
              "  \"hlsl.variants\": [\n"
              "    { \"name\": \"Configured\", \"hlsl.entryPoint\": \""
           << entry_point
           << "\" }\n"
              "  ]\n"
              "}\n";
    REQUIRE(config);
}

[[nodiscard]] std::string compute_visualization_shader() {
    return "RWStructuredBuffer<uint> Output : register(u0);\n"
           "groupshared uint Tile[32];\n"
           "[numthreads(8, 4, 1)]\n"
           "void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID,\n"
           "            uint3 groupThreadId : SV_GroupThreadID) {\n"
           "    Tile[groupThreadId.x] = dispatchThreadId.x;\n"
           "    GroupMemoryBarrierWithGroupSync();\n"
           "    Output[dispatchThreadId.x] = Tile[groupThreadId.x];\n"
           "}\n";
}

[[nodiscard]] Json compute_visualization_result(hlsl_intellisense::lsp::Server& server,
                                                std::int64_t id, const std::string& uri,
                                                Json options = Json::object()) {
    options["textDocument"] = Json{{"uri", uri}};
    const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = id, .method = "hlsl/computeVisualization", .params = std::move(options)});
    REQUIRE(response.has_value());
    const auto* result = std::get_if<hlsl_intellisense::json_rpc::Response>(&*response);
    REQUIRE(result != nullptr);
    return result->result;
}

} // namespace

TEST_CASE("hlsl/computeVisualization uses the configured compute variant and DXC reflection",
          "[lsp][compute-visualization][integration]") {
    TestDirectory directory;
    write_compute_visualization_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "compute.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compute_visualization_shader()}}}}}));

    const auto not_configured = compute_visualization_result(server, 2, document.uri());
    CHECK(not_configured["found"] == false);
    CHECK(not_configured["applicable"] == false);
    CHECK_FALSE(not_configured["explanation"].get<std::string>().empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));
    const auto result = compute_visualization_result(server, 3, document.uri());
    INFO(result.dump());
    CHECK(result["found"] == true);
    CHECK(result["applicable"] == true);
    CHECK(result["entryPoint"] == "CSMain");
    CHECK(result["stage"] == "compute");
    CHECK(result["targetProfile"] == "cs_6_6");
    CHECK(result["threadGroupSize"] == Json{{"x", 8}, {"y", 4}, {"z", 1}});
    CHECK(result["dispatchDimensions"] == Json{{"x", 8}, {"y", 4}, {"z", 1}});
    CHECK(result["groupCount"] == Json{{"x", 1}, {"y", 1}, {"z", 1}});
    CHECK(result["launchedThreads"] == 32);
    CHECK(result["inactiveThreads"] == 0);
    CHECK(result["systemValues"].size() == 4);
    CHECK(result["barriers"]["available"] == true);
    CHECK(result["barriers"]["instructionCount"] == 1);
    CHECK(result["barriers"]["locationsAvailable"] == true);
    CHECK(result["barriers"]["locationsTruncated"] == false);
    REQUIRE(result["barriers"]["locations"].size() == 1);
    CHECK(result["barriers"]["locations"][0]["label"] == "GroupMemoryBarrierWithGroupSync");
    CHECK(result["barriers"]["locations"][0]["uri"] == document.uri());
    CHECK(result["barriers"]["locations"][0]["range"]["start"] ==
          Json{{"line", 6}, {"character", 4}});
    CHECK(result["groupShared"]["available"] == true);
    CHECK(result["groupShared"]["truncated"] == false);
    CHECK(result["groupShared"]["totalBytes"] == 128);
    REQUIRE(result["groupShared"]["declarations"].size() == 1);
    CHECK(result["groupShared"]["declarations"][0]["name"] == "Tile");
    CHECK(result["groupShared"]["declarations"][0]["type"] == "uint [32]");
    CHECK(result["groupShared"]["declarations"][0]["declaration"] == "groupshared uint Tile[32]");
    CHECK(result["groupShared"]["declarations"][0]["bytes"] == 128);
    CHECK(result["groupShared"]["declarations"][0]["range"]["start"] ==
          Json{{"line", 1}, {"character", 0}});
    CHECK(result["waveSize"]["known"] == false);
    CHECK(result["occupancy"].is_null());

    // The compiler count and source-location availability are independent.
    // Removing the barrier makes zero authoritative; it does not make DXC
    // source-location enumeration available.
    auto without_barrier = compute_visualization_shader();
    constexpr std::string_view barrier_statement = "    GroupMemoryBarrierWithGroupSync();\n";
    const auto barrier = without_barrier.find(barrier_statement);
    REQUIRE(barrier != std::string::npos);
    without_barrier.erase(barrier, barrier_statement.size());
    const auto tile_count = without_barrier.find("Tile[32]");
    REQUIRE(tile_count != std::string::npos);
    without_barrier.replace(tile_count, std::string_view{"Tile[32]"}.size(), "Tile[16]");
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didChange",
        .params =
            Json{{"textDocument", {{"uri", document.uri()}, {"version", 2}}},
                 {"contentChanges", Json::array({Json{{"text", std::move(without_barrier)}}})}}}));
    server.wait_for_analysis();
    const auto no_barriers = compute_visualization_result(server, 4, document.uri());
    CHECK(no_barriers["barriers"]["available"] == true);
    CHECK(no_barriers["barriers"]["instructionCount"] == 0);
    CHECK(no_barriers["barriers"]["locationsAvailable"] == true);
    CHECK(no_barriers["barriers"]["locations"].empty());
    CHECK(no_barriers["groupShared"]["totalBytes"] == 64);
    CHECK(no_barriers["groupShared"]["declarations"][0]["type"] == "uint [16]");
}

TEST_CASE("hlsl/computeVisualization computes exact and edge dispatch geometry and occupancy",
          "[lsp][compute-visualization][geometry]") {
    TestDirectory directory;
    write_compute_visualization_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "compute.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compute_visualization_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));

    // dispatchDimensions is the desired logical workload in threads, so an
    // exact multiple maps directly to Dispatch() group counts.
    const auto exact = compute_visualization_result(
        server, 2, document.uri(), Json{{"dispatchDimensions", {{"x", 16}, {"y", 8}, {"z", 1}}}});
    CHECK(exact["groupCount"] == Json{{"x", 2}, {"y", 2}, {"z", 1}});
    CHECK(exact["launchedThreads"] == 128);
    CHECK(exact["inactiveThreads"] == 0);

    // A non-divisible logical workload is rounded up per axis; inactive
    // threads are the launched rectangular extent minus logical elements.
    const auto edge =
        compute_visualization_result(server, 3, document.uri(),
                                     Json{{"dispatchDimensions", {{"x", 17}, {"y", 9}, {"z", 1}}},
                                          {"hardwareProfile",
                                           {{"name", "Test GPU"},
                                            {"waveSize", 32},
                                            {"maxThreadsPerGroup", 1024},
                                            {"maxThreadsPerComputeUnit", 2048},
                                            {"maxGroupsPerComputeUnit", 8},
                                            {"sharedMemoryBytesPerComputeUnit", 65536}}}});
    CHECK(edge["groupCount"] == Json{{"x", 3}, {"y", 3}, {"z", 1}});
    CHECK(edge["launchedThreads"] == 288);
    CHECK(edge["inactiveThreads"] == 135);
    REQUIRE(!edge["occupancy"].is_null());
    CHECK(edge["occupancy"]["hardwareProfile"] == "Test GPU");
    CHECK(edge["occupancy"]["estimatedResidentGroups"] == 8);
    CHECK(edge["occupancy"]["estimatedResidentThreads"] == 256);
    CHECK(edge["occupancy"]["estimatedResidentWaves"] == 8);
    CHECK_FALSE(edge["occupancy"]["limitingFactors"].empty());
    CHECK(edge["occupancy"]["assumptions"].size() >= 2);

    const auto shared_limited =
        compute_visualization_result(server, 4, document.uri(),
                                     Json{{"hardwareProfile",
                                           {{"name", "Shared-memory limited GPU"},
                                            {"waveSize", 32},
                                            {"maxThreadsPerGroup", 1024},
                                            {"maxThreadsPerComputeUnit", 2048},
                                            {"maxGroupsPerComputeUnit", 8},
                                            {"sharedMemoryBytesPerComputeUnit", 256}}}});
    CHECK(shared_limited["occupancy"]["estimatedResidentGroups"] == 2);
    CHECK(std::ranges::find(shared_limited["occupancy"]["limitingFactors"],
                            "Group-shared memory per compute unit.") !=
          shared_limited["occupancy"]["limitingFactors"].end());

    const auto shared_exhausted =
        compute_visualization_result(server, 5, document.uri(),
                                     Json{{"hardwareProfile",
                                           {{"name", "Insufficient shared memory"},
                                            {"waveSize", 32},
                                            {"maxThreadsPerGroup", 1024},
                                            {"maxThreadsPerComputeUnit", 2048},
                                            {"maxGroupsPerComputeUnit", 8},
                                            {"sharedMemoryBytesPerComputeUnit", 64}}}});
    CHECK(shared_exhausted["occupancy"]["estimatedResidentGroups"] == 0);
    CHECK(std::ranges::find(shared_exhausted["occupancy"]["limitingFactors"],
                            "Group-shared memory per compute unit.") !=
          shared_exhausted["occupancy"]["limitingFactors"].end());
    CHECK(std::ranges::find(shared_exhausted["occupancy"]["limitingFactors"],
                            "One reflected thread group exceeds maxThreadsPerComputeUnit.") ==
          shared_exhausted["occupancy"]["limitingFactors"].end());

    // Partial waves cannot be shared by independent thread groups. A
    // 32-thread group on wave64 hardware consumes 64 resident lanes, so a
    // 64-thread compute-unit limit permits one group, not two.
    const auto partial_wave =
        compute_visualization_result(server, 5, document.uri(),
                                     Json{{"hardwareProfile",
                                           {{"name", "Wave64 test GPU"},
                                            {"waveSize", 64},
                                            {"maxThreadsPerGroup", 1024},
                                            {"maxThreadsPerComputeUnit", 64},
                                            {"maxGroupsPerComputeUnit", 2},
                                            {"sharedMemoryBytesPerComputeUnit", 65536}}}});
    REQUIRE(!partial_wave["occupancy"].is_null());
    CHECK(partial_wave["occupancy"]["estimatedResidentGroups"] == 1);
    CHECK(partial_wave["occupancy"]["estimatedResidentThreads"] == 32);
    CHECK(partial_wave["occupancy"]["estimatedResidentWaves"] == 1);
}

TEST_CASE("hlsl/computeVisualization serializes compiler-formatted WaveSize requirements",
          "[lsp][compute-visualization][wave-size]") {
    TestDirectory directory;
    write_compute_visualization_config(directory, "cs_6_8");
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "wave.hlsl").string());
    const std::string source = "[WaveSize(32, 64, 64)]\n"
                               "[numthreads(8, 1, 1)]\n"
                               "void CSMain() {}\n";
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "textDocument/didOpen",
                                                  .params = Json{{"textDocument",
                                                                  {{"uri", document.uri()},
                                                                   {"languageId", "hlsl"},
                                                                   {"version", 1},
                                                                   {"text", source}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));

    const auto result = compute_visualization_result(server, 2, document.uri());
    REQUIRE(result["applicable"] == true);
    CHECK(result["waveSize"]["known"] == true);
    CHECK(result["waveSize"]["min"] == 32);
    CHECK(result["waveSize"]["max"] == 64);
    CHECK(result["waveSize"]["preferred"] == 64);
    CHECK(result["waveSize"]["minMaxSource"] == "psv0");
    CHECK(result["waveSize"]["preferredSource"] == "compilerFormattedEntryCursor");
    CHECK(result["waveSize"]["explanation"].get<std::string>().find("PSV0") != std::string::npos);
}

TEST_CASE("hlsl/computeVisualization rejects malformed, non-positive, and overflowing inputs",
          "[lsp][compute-visualization][validation]") {
    TestDirectory directory;
    write_compute_visualization_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "compute.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compute_visualization_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));

    const std::vector<Json> invalid_options{
        Json{{"dispatchDimensions", {{"x", 0}, {"y", 1}, {"z", 1}}}},
        Json{{"dispatchDimensions", {{"x", 1.5}, {"y", 1}, {"z", 1}}}},
        Json{{"dispatchDimensions", {{"x", 4'294'967'296ULL}, {"y", 1}, {"z", 1}}}},
        Json{{"dispatchDimensions", {{"x", 4'294'967'295ULL}, {"y", 4'294'967'295ULL}, {"z", 1}}}},
        // The logical workload product is still exactly representable, but
        // ceil-dividing by 8x4x1 launches 2^53 threads, which is not.
        Json{{"dispatchDimensions", {{"x", 4'294'967'295ULL}, {"y", 2'097'152}, {"z", 1}}}},
        Json{{"hardwareProfile",
              {{"name", ""},
               {"waveSize", 32},
               {"maxThreadsPerGroup", 1024},
               {"maxThreadsPerComputeUnit", 2048},
               {"maxGroupsPerComputeUnit", 8},
               {"sharedMemoryBytesPerComputeUnit", 65536}}}},
        Json{{"hardwareProfile",
              {{"name", "Bad GPU"},
               {"waveSize", 0},
               {"maxThreadsPerGroup", 1024},
               {"maxThreadsPerComputeUnit", 2048},
               {"maxGroupsPerComputeUnit", 8},
               {"sharedMemoryBytesPerComputeUnit", 65536}}}}};
    std::int64_t id = 2;
    for (auto options : invalid_options) {
        options["textDocument"] = Json{{"uri", document.uri()}};
        const auto response = server.handle(hlsl_intellisense::json_rpc::Request{
            .id = id++, .method = "hlsl/computeVisualization", .params = std::move(options)});
        REQUIRE(response.has_value());
        const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&*response);
        REQUIRE(error != nullptr);
        CHECK(error->error.code == hlsl_intellisense::json_rpc::invalid_params_code);
    }
}

TEST_CASE("hlsl/computeVisualization reports a configured non-compute shader as not applicable",
          "[lsp][compute-visualization]") {
    TestDirectory directory;
    write_compute_visualization_config(directory, "ps_6_6", "PSMain");
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "pixel.hlsl").string());
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", "float4 PSMain() : SV_Target { return 1.0; }\n"}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));

    const auto result = compute_visualization_result(server, 2, document.uri());
    CHECK(result["found"] == true);
    CHECK(result["applicable"] == false);
    CHECK(result["stage"] == "pixel");
    CHECK(result["threadGroupSize"].is_null());
    CHECK(result["occupancy"].is_null());
}

TEST_CASE("Server cancellation returns RequestCancelled for hlsl/computeVisualization",
          "[lsp][compute-visualization][cancellation]") {
    TestDirectory directory;
    write_compute_visualization_config(directory);
    const auto document = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "compute.hlsl").string());
    auto hooks = std::make_shared<hlsl_intellisense::analysis::AnalysisHooks>();
    std::promise<void> entered;
    std::promise<void> release;
    const auto released = release.get_future().share();
    hooks->before_interactive = [&](std::string_view) {
        entered.set_value();
        released.wait();
    };
    hlsl_intellisense::lsp::ServerOptions options;
    options.background_analysis = true;
    options.analysis_hooks = hooks;
    hlsl_intellisense::lsp::Server server{[](const auto&) {}, {}, options};
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "initialized", .params = Json::object()}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "textDocument/didOpen",
        .params = Json{{"textDocument",
                        {{"uri", document.uri()},
                         {"languageId", "hlsl"},
                         {"version", 1},
                         {"text", compute_visualization_shader()}}}}}));
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "hlsl/didChangeActiveVariant", .params = Json{{"variant", "Configured"}}}));
    server.wait_for_analysis();

    const hlsl_intellisense::json_rpc::Request request{
        .id = std::string{"compute-visualization"},
        .method = "hlsl/computeVisualization",
        .params = Json{{"textDocument", {{"uri", document.uri()}}}}};
    const auto cancellation = server.begin_request(request.id);
    auto response =
        std::async(std::launch::async, [&] { return server.handle(request, cancellation); });
    entered.get_future().wait();
    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "$/cancelRequest", .params = Json{{"id", "compute-visualization"}}}));

    const auto result = response.get();
    const auto* error = std::get_if<hlsl_intellisense::json_rpc::ErrorResponse>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);
    release.set_value();
    server.wait_for_analysis();
}

TEST_CASE("initialize advertises callHierarchyProvider", "[lsp][call-hierarchy]") {
    hlsl_intellisense::lsp::Server server{[](const auto&) {}};
    const auto initialized = server.handle(hlsl_intellisense::json_rpc::Request{
        .id = std::int64_t{1}, .method = "initialize", .params = Json::object()});
    REQUIRE(initialized.has_value());
    const auto* response = std::get_if<hlsl_intellisense::json_rpc::Response>(&*initialized);
    REQUIRE(response != nullptr);
    CHECK(response->result["capabilities"]["callHierarchyProvider"] == true);
}