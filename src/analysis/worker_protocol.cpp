#include <hlsl_intellisense/analysis/worker_protocol.h>

#include <hlsl_intellisense/json_rpc/framing.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hlsl_intellisense::analysis {
namespace {

using Json = nlohmann::json;

struct WorkerEntry final {
    std::string path;
    std::vector<std::string> arguments;
    dxc::TranslationUnit translation_unit;
};

[[nodiscard]] Json location_json(const dxc::SourceLocation& location) {
    return Json{{"path", location.path},
                {"line", location.line},
                {"column", location.column},
                {"offset", location.offset}};
}

[[nodiscard]] Json range_json(const dxc::SourceRange& range) {
    return Json{{"start", location_json(range.start)}, {"end", location_json(range.end)}};
}

[[nodiscard]] Json diagnostic_json(const dxc::Diagnostic& diagnostic) {
    Json fix_its = Json::array();
    for (const auto& fix_it : diagnostic.fix_its) {
        fix_its.push_back(Json{{"range", range_json(fix_it.range)},
                               {"replacementText", fix_it.replacement_text}});
    }
    return Json{{"severity", static_cast<std::uint8_t>(diagnostic.severity)},
                {"message", diagnostic.message},
                {"location", location_json(diagnostic.location)},
                {"fixIts", std::move(fix_its)}};
}

[[nodiscard]] dxc::CompilerOptions compiler_options(const Json& value) {
    dxc::CompilerOptions result;
    result.language_version = value.value("languageVersion", result.language_version);
    result.target_profile = value.value("targetProfile", std::string{});
    result.entry_point = value.value("entryPoint", std::string{});
    result.defines = value.value("defines", std::vector<std::string>{});
    result.include_directories = value.value("includeDirectories", std::vector<std::string>{});
    result.additional_arguments = value.value("additionalArguments", std::vector<std::string>{});
    return result;
}

[[nodiscard]] std::vector<dxc::SourceFile> source_files(const Json& values) {
    std::vector<dxc::SourceFile> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(dxc::SourceFile{.path = value.at("path").get<std::string>(),
                                         .text = value.at("text").get<std::string>(),
                                         .rewritten = value.value("rewritten", false)});
    }
    return result;
}

[[nodiscard]] Json response(std::uint64_t id, Json result) {
    return Json{
        {"protocol", analysis_worker_protocol_version}, {"id", id}, {"result", std::move(result)}};
}

[[nodiscard]] Json error_response(std::optional<std::uint64_t> id, std::string_view message) {
    Json value{{"protocol", analysis_worker_protocol_version},
               {"id", id ? Json(*id) : Json(nullptr)},
               {"error", Json{{"message", message}}}};
    return value;
}

class Worker final {
  public:
    explicit Worker(const dxc::RuntimeConfiguration& runtime) : intellisense_{runtime} {}

    [[nodiscard]] Json dispatch(const Json& request, bool& shutdown) {
        const auto id = request.at("id").get<std::uint64_t>();
        if (request.at("protocol").get<unsigned>() != analysis_worker_protocol_version) {
            return error_response(id, "Unsupported analysis worker protocol version");
        }
        const auto method = request.at("method").get<std::string>();
        const auto params = request.value("params", Json::object());
        if (method == "analyze") {
            return response(id, analyze(params));
        }
        if (method == "erase") {
            entries_.erase(params.at("rootIdentity").get<std::string>());
            return response(id, Json::object());
        }
        if (method == "runtimeInfo") {
            const auto info = intellisense_.runtime_info();
            return response(id, Json{{"directory", info.directory},
                                     {"libraryPath", info.library_path},
                                     {"version", info.version},
                                     {"bundled", info.bundled}});
        }
        if (method == "shutdown") {
            shutdown = true;
            entries_.clear();
            return response(id, Json::object());
        }
        return error_response(id, "Unknown analysis worker method");
    }

  private:
    [[nodiscard]] Json analyze(const Json& params) {
        const auto root_identity = params.at("rootIdentity").get<std::string>();
        const auto path = params.at("path").get<std::string>();
        auto sources = source_files(params.at("sources"));
        const auto options = compiler_options(params.at("compilerOptions"));
        const auto arguments = options.arguments();
        auto entry = entries_.find(root_identity);
        bool reparsed = false;
        if (entry != entries_.end() && entry->second.path == path &&
            entry->second.arguments == arguments) {
            entry->second.translation_unit.reparse(std::move(sources));
            reparsed = true;
        } else {
            auto translation_unit = intellisense_.parse(path, std::move(sources), options);
            if (entry != entries_.end()) {
                entries_.erase(entry);
            }
            entry = entries_
                        .emplace(root_identity,
                                 WorkerEntry{.path = path,
                                             .arguments = arguments,
                                             .translation_unit = std::move(translation_unit)})
                        .first;
        }

        Json diagnostics = Json::array();
        for (const auto& diagnostic : entry->second.translation_unit.diagnostics()) {
            diagnostics.push_back(diagnostic_json(diagnostic));
        }
        return Json{{"diagnostics", std::move(diagnostics)}, {"reparsed", reparsed}};
    }

    dxc::Intellisense intellisense_;
    std::unordered_map<std::string, WorkerEntry> entries_;
};

} // namespace

int run_analysis_worker(std::istream& input, std::ostream& output, std::ostream& errors,
                        const dxc::RuntimeConfiguration& runtime) {
    try {
        json_rpc::FrameReader reader{input, analysis_worker_max_payload_size};
        json_rpc::FrameWriter writer{output};
        Worker worker{runtime};
        bool shutdown = false;
        while (!shutdown) {
            const auto payload = reader.read();
            if (!payload) {
                break;
            }

            std::optional<std::uint64_t> id;
            Json reply;
            try {
                const auto request = Json::parse(*payload);
                if (const auto id_value = request.find("id");
                    id_value != request.end() && id_value->is_number_unsigned()) {
                    id = id_value->get<std::uint64_t>();
                }
                reply = worker.dispatch(request, shutdown);
            } catch (const std::exception& error) {
                reply = error_response(id, error.what());
            }
            writer.write(reply.dump());
        }
        return 0;
    } catch (const std::exception& error) {
        errors << "HLSL-LSP analysis worker: " << error.what() << '\n';
        return 1;
    }
}

} // namespace hlsl_intellisense::analysis
