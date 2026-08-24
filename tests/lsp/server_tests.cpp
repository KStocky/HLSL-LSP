#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/json_rpc/message.h>
#include <hlsl_intellisense/lsp/server.h>
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

    for (const auto method : {"textDocument/hover", "textDocument/signatureHelp"}) {
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
    CHECK_FALSE(response->result["capabilities"].contains("semanticTokensProvider"));
    CHECK(response->result["capabilities"]["definitionProvider"] == true);
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
