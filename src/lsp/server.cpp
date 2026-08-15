#include <hlsl_intellisense/lsp/server.h>

#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/error.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hlsl_intellisense::lsp {
namespace {

using json_rpc::HandlerError;
using json_rpc::Json;

[[noreturn]] void invalid_params(std::string_view message) {
    throw HandlerError{json_rpc::invalid_params_code, message};
}

[[nodiscard]] const Json& object_params(const std::optional<Json>& params) {
    if (!params.has_value() || !params->is_object()) {
        invalid_params("Expected object parameters");
    }
    return *params;
}

[[nodiscard]] const Json& member(const Json& object, std::string_view name) {
    const auto item = object.find(name);
    if (item == object.end()) {
        invalid_params(std::string{"Missing parameter: "} + std::string{name});
    }
    return *item;
}

[[nodiscard]] const Json& object_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_object()) {
        invalid_params(std::string{"Expected object: "} + std::string{name});
    }
    return value;
}

[[nodiscard]] std::string string_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_string()) {
        invalid_params(std::string{"Expected string: "} + std::string{name});
    }
    return value.get<std::string>();
}

[[nodiscard]] std::int64_t integer_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_number_integer()) {
        invalid_params(std::string{"Expected integer: "} + std::string{name});
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] std::uint32_t unsigned_member(const Json& object, std::string_view name) {
    const auto value = integer_member(object, name);
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        invalid_params(std::string{"Expected non-negative 32-bit integer: "} + std::string{name});
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] workspace::Position position(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected position object");
    }
    return {.line = unsigned_member(value, "line"),
            .character = unsigned_member(value, "character")};
}

[[nodiscard]] workspace::Range range(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected range object");
    }
    return {.start = position(object_member(value, "start")),
            .end = position(object_member(value, "end"))};
}

[[nodiscard]] Json lsp_position(workspace::Position value) {
    return {{"line", value.line}, {"character", value.character}};
}

[[nodiscard]] Json lsp_range(workspace::Range value) {
    return {{"start", lsp_position(value.start)}, {"end", lsp_position(value.end)}};
}

[[nodiscard]] int diagnostic_severity(dxc::DiagnosticSeverity severity) {
    switch (severity) {
    case dxc::DiagnosticSeverity::ignored:
    case dxc::DiagnosticSeverity::note:
        return 3;
    case dxc::DiagnosticSeverity::warning:
        return 2;
    case dxc::DiagnosticSeverity::error:
    case dxc::DiagnosticSeverity::fatal:
        return 1;
    }
    return 1;
}

[[nodiscard]] int completion_kind(std::uint32_t cursor_kind) {
    if (cursor_kind == 6) {
        return 5;
    }
    if (cursor_kind == 7) {
        return 20;
    }
    if (cursor_kind == 8 || cursor_kind == 30) {
        return 3;
    }
    if (cursor_kind == 9 || cursor_kind == 10 || cursor_kind == 50) {
        return 6;
    }
    if (cursor_kind == 21 || cursor_kind == 26) {
        return 2;
    }
    if (cursor_kind == 24) {
        return 4;
    }
    if (cursor_kind == 5) {
        return 13;
    }
    if (cursor_kind == 22) {
        return 9;
    }
    if (cursor_kind == 2 || cursor_kind == 3 || cursor_kind == 4 || cursor_kind == 20 ||
        cursor_kind == 27 || cursor_kind == 31 || cursor_kind == 36 || cursor_kind == 43 ||
        cursor_kind == 45) {
        return 7;
    }
    if (cursor_kind == 501 || cursor_kind == 502) {
        return 14;
    }
    return 1;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
dxc_position(std::string_view text, workspace::Position request_position) {
    const auto offset = workspace::utf8_offset_at(text, request_position);
    std::size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    const auto byte_column = offset - line_start + 1;
    if (byte_column > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Completion position is too large");
    }
    return {request_position.line + 1, static_cast<std::uint32_t>(byte_column)};
}

[[nodiscard]] workspace::Range diagnostic_range(const workspace::SourceSnapshot& snapshot,
                                                const dxc::Diagnostic& diagnostic) {
    if (diagnostic.location.line == 0 || diagnostic.location.column == 0) {
        return {};
    }

    std::size_t line_start = 0;
    for (std::uint32_t line = 1; line < diagnostic.location.line; ++line) {
        const auto newline = snapshot.text().find('\n', line_start);
        if (newline == std::string::npos) {
            return {};
        }
        line_start = newline + 1;
    }
    const auto line_end = snapshot.text().find_first_of("\r\n", line_start);
    const auto end_offset = line_end == std::string::npos ? snapshot.text().size() : line_end;
    const auto byte_column = static_cast<std::size_t>(diagnostic.location.column - 1);
    if (byte_column > end_offset - line_start) {
        return {};
    }

    const auto offset = line_start + byte_column;
    try {
        const auto start = workspace::lsp_position_at(snapshot.text(), offset);
        auto end = start;
        if (offset < snapshot.text().size() && snapshot.text()[offset] != '\r' &&
            snapshot.text()[offset] != '\n') {
            const auto first = static_cast<unsigned char>(snapshot.text()[offset]);
            const std::size_t bytes =
                first < 0x80 ? 1 : (first < 0xE0 ? 2 : (first < 0xF0 ? 3 : 4));
            end = workspace::lsp_position_at(snapshot.text(), offset + bytes);
        }
        return {.start = start, .end = end};
    } catch (const workspace::DocumentError&) {
        return {};
    }
}

} // namespace

Server::Server(NotificationSender sender, Logger logger)
    : sender_{std::move(sender)}, logger_{std::move(logger)} {
    if (!sender_) {
        throw std::invalid_argument{"The LSP server requires a notification sender"};
    }
    register_handlers();
}

void Server::register_handlers() {
    dispatcher_.register_request_handler("initialize",
                                         [this](const auto& params) { return initialize(params); });
    dispatcher_.register_request_handler("shutdown",
                                         [this](const auto& params) { return shutdown(params); });
    dispatcher_.register_request_handler("textDocument/completion",
                                         [this](const auto& params) { return completion(params); });
    dispatcher_.register_notification_handler("initialized",
                                              [this](const auto& params) { initialized(params); });
    dispatcher_.register_notification_handler("textDocument/didOpen",
                                              [this](const auto& params) { did_open(params); });
    dispatcher_.register_notification_handler("textDocument/didChange",
                                              [this](const auto& params) { did_change(params); });
    dispatcher_.register_notification_handler("textDocument/didSave",
                                              [this](const auto& params) { did_save(params); });
    dispatcher_.register_notification_handler("textDocument/didClose",
                                              [this](const auto& params) { did_close(params); });
    dispatcher_.register_notification_handler("exit", [this](const auto& params) { exit(params); });
}

std::optional<json_rpc::DispatchResponse> Server::handle(const json_rpc::Message& message) {
    if (const auto* request = std::get_if<json_rpc::Request>(&message)) {
        if (state_ == State::uninitialized && request->method != "initialize") {
            return json_rpc::ErrorResponse{.id = request->id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::awaiting_initialized) {
            return json_rpc::ErrorResponse{.id = request->id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::shutdown) {
            return json_rpc::ErrorResponse{.id = request->id,
                                           .error = {.code = json_rpc::invalid_request_code,
                                                     .message = "Server has shut down",
                                                     .data = std::nullopt}};
        }
    }
    return dispatcher_.dispatch(message);
}

bool Server::exit_requested() const noexcept { return exit_requested_; }

int Server::exit_code() const noexcept { return clean_shutdown_ ? 0 : 1; }

Json Server::initialize(const std::optional<Json>& params) {
    if (state_ != State::uninitialized) {
        throw HandlerError{json_rpc::invalid_request_code, "Initialize may only be requested once"};
    }
    static_cast<void>(object_params(params));
    state_ = State::awaiting_initialized;
    return {{"capabilities",
             {{"positionEncoding", "utf-16"},
              {"textDocumentSync",
               {{"openClose", true}, {"change", 2}, {"save", {{"includeText", true}}}}},
              {"completionProvider", {{"resolveProvider", false}}}}},
            {"serverInfo", {{"name", "HLSL-LSP"}, {"version", "0.1.0"}}}};
}

Json Server::shutdown(const std::optional<Json>& params) {
    if (params.has_value() && !params->is_null()) {
        invalid_params("Shutdown does not accept parameters");
    }
    if (state_ != State::running) {
        throw HandlerError{json_rpc::invalid_request_code, "Shutdown is not valid now"};
    }
    state_ = State::shutdown;
    clean_shutdown_ = true;
    return nullptr;
}

Json Server::completion(const std::optional<Json>& params) {
    require_running();
    const auto& value = object_params(params);
    const auto& text_document = object_member(value, "textDocument");
    const auto uri = string_member(text_document, "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Completion document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    const auto analysis = analyses_.find(snapshot.document_uri().identity());
    if (analysis == analyses_.end() || analysis->second.version != snapshot.version()) {
        invalid_params("Completion analysis is stale");
    }
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto completions =
        analysis->second.translation_unit.complete(snapshot.path(), line, column);

    Json items = Json::array();
    for (const auto& completion_item : completions) {
        items.push_back({{"label", completion_item.label},
                         {"detail", completion_item.detail},
                         {"kind", completion_kind(completion_item.cursor_kind)}});
    }
    return {{"isIncomplete", false}, {"items", std::move(items)}};
}

void Server::initialized(const std::optional<Json>& params) {
    if (state_ != State::awaiting_initialized) {
        log("Ignoring initialized notification in an invalid lifecycle state");
        return;
    }
    if (params.has_value() && !params->is_object()) {
        log("Ignoring initialized notification with invalid parameters");
        return;
    }
    state_ = State::running;
}

void Server::did_open(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& document = object_member(object_params(params), "textDocument");
        const auto uri = string_member(document, "uri");
        documents_.did_open(uri, string_member(document, "languageId"),
                            integer_member(document, "version"), string_member(document, "text"));
        analyze_and_publish(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto& document = object_member(value, "textDocument");
        const auto uri = string_member(document, "uri");
        const auto& raw_changes = member(value, "contentChanges");
        if (!raw_changes.is_array() || raw_changes.empty()) {
            invalid_params("contentChanges must be a non-empty array");
        }

        std::vector<workspace::ContentChange> changes;
        changes.reserve(raw_changes.size());
        for (const auto& raw_change : raw_changes) {
            if (!raw_change.is_object()) {
                invalid_params("Each content change must be an object");
            }
            workspace::ContentChange change{.range = std::nullopt,
                                            .range_length = std::nullopt,
                                            .text = string_member(raw_change, "text")};
            if (const auto item = raw_change.find("range"); item != raw_change.end()) {
                change.range = range(*item);
            }
            if (const auto item = raw_change.find("rangeLength"); item != raw_change.end()) {
                if (!item->is_number_unsigned() && !item->is_number_integer()) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                const auto length = item->get<std::int64_t>();
                if (length < 0) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                change.range_length = static_cast<std::size_t>(length);
            }
            changes.push_back(std::move(change));
        }
        documents_.did_change(uri, integer_member(document, "version"), changes);
        analyze_and_publish(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_save(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto uri = string_member(object_member(value, "textDocument"), "uri");
        std::optional<std::string> text;
        if (const auto item = value.find("text"); item != value.end()) {
            if (!item->is_string()) {
                invalid_params("Save text must be a string");
            }
            text = item->get<std::string>();
        }
        documents_.did_save(uri, std::move(text));
        analyze_and_publish(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_close(const std::optional<Json>& params) {
    try {
        require_running();
        const auto uri = string_member(object_member(object_params(params), "textDocument"), "uri");
        const auto snapshot = documents_.snapshot(uri);
        documents_.did_close(uri);
        analyses_.erase(snapshot.document_uri().identity());
        sender_(json_rpc::Notification{
            .method = "textDocument/publishDiagnostics",
            .params = Json{{"uri", snapshot.uri()}, {"diagnostics", Json::array()}}});
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::exit(const std::optional<Json>& params) {
    if (params.has_value() && !params->is_null()) {
        log("Exit notification does not accept parameters");
    }
    exit_requested_ = true;
}

void Server::analyze_and_publish(std::string_view uri) {
    const auto snapshot = documents_.snapshot(uri);
    const auto identity = snapshot.document_uri().identity();
    auto analysis = analyses_.find(identity);
    const auto open_documents = documents_.open_snapshots();
    std::vector<dxc::SourceFile> sources;
    sources.reserve(open_documents.size());
    std::ranges::transform(open_documents, std::back_inserter(sources), [](const auto& document) {
        return dxc::SourceFile{document.path(), document.text()};
    });
    if (analysis == analyses_.end()) {
        dxc::CompilerOptions options;
        const auto shader_directory = std::filesystem::path{snapshot.path()}.parent_path();
        std::error_code error;
        if (std::filesystem::is_directory(shader_directory, error)) {
            options = workspace::load_workspace_configuration(shader_directory).compiler_options();
        } else if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error{"Unable to inspect shader directory",
                                                    shader_directory, error};
        }
        auto translation_unit = intellisense_.parse(snapshot.path(), std::move(sources), options);
        analysis =
            analyses_.emplace(identity, Analysis{snapshot.version(), std::move(translation_unit)})
                .first;
    } else {
        analysis->second.translation_unit.reparse(std::move(sources));
        analysis->second.version = snapshot.version();
    }

    const auto latest = documents_.snapshot(uri);
    if (latest.version() == analysis->second.version) {
        publish_diagnostics(latest, analysis->second.translation_unit.diagnostics());
    }
}

void Server::publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                                 const std::vector<dxc::Diagnostic>& diagnostics) {
    Json items = Json::array();
    for (const auto& diagnostic : diagnostics) {
        if (!diagnostic.location.path.empty() && diagnostic.location.path != snapshot.path()) {
            continue;
        }
        items.push_back({{"range", lsp_range(diagnostic_range(snapshot, diagnostic))},
                         {"severity", diagnostic_severity(diagnostic.severity)},
                         {"source", "dxc"},
                         {"message", diagnostic.message}});
    }
    sender_(json_rpc::Notification{.method = "textDocument/publishDiagnostics",
                                   .params = Json{{"uri", snapshot.uri()},
                                                  {"version", snapshot.version()},
                                                  {"diagnostics", std::move(items)}}});
}

void Server::require_running() const {
    if (state_ != State::running) {
        throw HandlerError{-32002, "Server not initialized"};
    }
}

void Server::log(std::string_view message) const {
    if (logger_) {
        logger_(message);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run(std::istream& input, std::ostream& output, std::ostream& errors) {
    try {
        json_rpc::FrameWriter writer{output};
        Server server{
            [&writer](const json_rpc::Notification& notification) {
                writer.write(json_rpc::serialize(json_rpc::Message{notification}));
            },
            [&errors](std::string_view message) { errors << "HLSL-LSP: " << message << '\n'; }};
        json_rpc::FrameReader reader{input};

        while (!server.exit_requested()) {
            const auto payload = reader.read();
            if (!payload.has_value()) {
                break;
            }
            const auto parsed = json_rpc::parse_message(*payload);
            if (parsed.error.has_value()) {
                writer.write(json_rpc::serialize(json_rpc::DispatchResponse{*parsed.error}));
                continue;
            }
            if (const auto response = server.handle(*parsed.message); response.has_value()) {
                writer.write(json_rpc::serialize(*response));
            }
        }
        return server.exit_requested() ? server.exit_code() : 0;
    } catch (const std::exception& error) {
        errors << "HLSL-LSP: " << error.what() << '\n';
        return 1;
    }
}

} // namespace hlsl_intellisense::lsp
