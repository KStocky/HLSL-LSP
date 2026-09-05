#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/json_rpc/message.h>
#include <hlsl_intellisense/lsp/server.h>
#include <hlsl_intellisense/workspace/document_uri.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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
    REQUIRE(layout["members"][2]["members"].size() == 2);
    CHECK(layout["members"][2]["members"][0]["arrayIndex"] == 0);
    CHECK(layout["members"][2]["members"][0]["offset"] == 0);
    CHECK(layout["members"][2]["members"][1]["arrayIndex"] == 1);
    CHECK(layout["members"][2]["members"][1]["offset"] == 16);
    CHECK(layout["members"][3]["offset"] == 64);
    REQUIRE(layout["members"][3]["members"].size() == 2);
    CHECK(layout["members"][3]["members"][1]["offset"] == 4);
    CHECK(layout["members"][0].size() == 8);
    CHECK(layout["members"][0].contains("name"));
    CHECK(layout["members"][0].contains("type"));
    CHECK(layout["members"][0]["kind"] == "vector");
    CHECK(layout["members"][0].contains("offset"));
    CHECK(layout["members"][0].contains("size"));
    CHECK(layout["members"][0].contains("alignment"));
    CHECK(layout["members"][0].contains("paddingBefore"));
    CHECK(layout["members"][0].contains("members"));
    for (const auto key : {"offset", "size", "alignment", "paddingBefore"}) {
        CHECK(layout["members"][0][key].is_number_unsigned());
    }

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

TEST_CASE("References and rename preserve identity across open roots and disk includes",
          "[lsp][references][rename]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    const std::string include_text = "static const float sharedValue = 1.0;\n";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << include_text;
    }
    const auto first = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "a.hlsl").string());
    const auto second = hlsl_intellisense::workspace::DocumentUri::from_path(
        (directory.path() / "b.hlsl").string());
    const std::string first_text = "#include \"shared.hlsli\"\n"
                                   "float4 main() : SV_Target { float sharedValue = 2.0; return "
                                   "(sharedValue + ::sharedValue).xxxx; }\n";
    const std::string second_text = "#include \"shared.hlsli\"\n"
                                    "float4 main() : SV_Target { return sharedValue.xxxx; }\n";

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
    CHECK(response->result["serverInfo"]["version"] == "0.10.0");
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

    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["uri"] == document.uri());
    CHECK((*notifications.back().params)["diagnostics"].empty());
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
                         {"text", "#include <Editor.hlsli>\n"
                                  "#ifndef EDITOR_SETTING\n#error missing editor setting\n#endif\n"
                                  "float4 main() : SV_Target { return editorValue; }\n"}}}}}));
    REQUIRE(notifications.size() == 1);
    CHECK(!(*notifications.back().params)["diagnostics"].empty());

    static_cast<void>(server.handle(hlsl_intellisense::json_rpc::Notification{
        .method = "workspace/didChangeConfiguration",
        .params = Json{{"settings",
                        {{"hlsl",
                          {{"preprocessorDefinitions", {{"EDITOR_SETTING", 1}}},
                           {"additionalIncludeDirectories", Json::array({"includes"})},
                           {"languageVersion", "2021"}}}}}}}));
    REQUIRE(notifications.size() == 2);
    CHECK((*notifications.back().params)["diagnostics"].empty());

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

    CHECK(server.analysis_metrics().parse_count == 3);
    REQUIRE(notifications.size() == 3);
    CHECK((*notifications.back().params)["uri"] == first.uri());
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
        }
        if (variant["name"] == "Beta") {
            found_beta = true;
        }
    }
    CHECK(found_alpha);
    CHECK(found_beta);
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
    REQUIRE(!info["reflection"].is_null());
    CHECK(info["reflection"]["available"] == true);
    const auto& resources = info["reflection"]["resources"];
    CHECK(std::ranges::any_of(
        resources, [](const Json& resource) { return resource["name"] == "MainTexture"; }));
    CHECK(std::ranges::any_of(
        resources, [](const Json& resource) { return resource["name"] == "MainSampler"; }));
    CHECK(info["reflection"]["threadGroupSize"].is_null());

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
    // A concurrent edit for the same root cancels the in-flight interactive
    // work (mirroring memory_layout/hover), so the request surfaces as
    // RequestCancelled rather than completing against stale content.
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
    CHECK(error->error.code == hlsl_intellisense::json_rpc::request_cancelled_code);
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