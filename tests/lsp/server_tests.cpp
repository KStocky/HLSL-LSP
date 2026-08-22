#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/json_rpc/message.h>
#include <hlsl_intellisense/lsp/server.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

    static_cast<void>(server.handle(
        hlsl_intellisense::json_rpc::Notification{.method = "exit", .params = std::nullopt}));
    CHECK(server.exit_requested());
    CHECK(server.exit_code() == 1);
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

TEST_CASE("Server can disable semantic tokens for incompatible clients", "[lsp][navigation]") {
    std::vector<hlsl_intellisense::json_rpc::Notification> notifications;
    hlsl_intellisense::lsp::Server server{
        [&notifications](const auto& value) { notifications.push_back(value); },
        {},
        {.semantic_tokens = false}};

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
    REQUIRE(messages.size() == 7);
    CHECK(messages[0]["id"] == 1);
    CHECK(messages[0]["result"]["capabilities"]["positionEncoding"] == "utf-16");

    CHECK(messages[1]["method"] == "textDocument/publishDiagnostics");
    CHECK(messages[1]["params"]["uri"] == uri);
    CHECK(messages[1]["params"]["version"] == 1);
    REQUIRE(!messages[1]["params"]["diagnostics"].empty());
    const auto& diagnostic = messages[1]["params"]["diagnostics"][0];
    CHECK(diagnostic["source"] == "dxc");
    CHECK(diagnostic.contains("severity"));
    CHECK(diagnostic.contains("message"));
    CHECK(diagnostic["range"]["start"].contains("line"));
    CHECK(diagnostic["range"]["start"].contains("character"));
    CHECK(diagnostic["range"]["start"]["line"].get<std::uint32_t>() > 0);

    for (const auto index : {2U, 3U}) {
        CHECK(messages[index]["method"] == "textDocument/publishDiagnostics");
        CHECK(messages[index]["params"]["version"] == 2);
        CHECK(messages[index]["params"]["diagnostics"].empty());
    }

    CHECK(messages[4]["id"] == 2);
    const auto& items = messages[4]["result"]["items"];
    CHECK(std::ranges::any_of(items, [](const auto& item) {
        return item.value("label", "") == "Number" && item.contains("detail") &&
               item.contains("kind");
    }));
    CHECK(messages[5]["method"] == "textDocument/publishDiagnostics");
    CHECK(messages[5]["params"]["uri"] == uri);
    CHECK(messages[5]["params"]["diagnostics"].empty());
    CHECK(messages[6]["id"] == 3);
    CHECK(messages[6]["result"].is_null());
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
    REQUIRE(notifications.size() == 4);
    CHECK((*notifications[2].params)["diagnostics"].empty());
    CHECK((*notifications[3].params)["diagnostics"].empty());
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
