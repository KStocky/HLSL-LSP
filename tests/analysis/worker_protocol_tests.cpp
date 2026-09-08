#include <hlsl_intellisense/analysis/worker_protocol.h>
#include <hlsl_intellisense/json_rpc/framing.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <sstream>
#include <string>

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;
using Json = nlohmann::json;

namespace {

void write_request(std::stringstream& input, std::uint64_t id, std::string method,
                   Json params = Json::object()) {
    json_rpc::FrameWriter writer{input};
    writer.write(Json{{"protocol", analysis::analysis_worker_protocol_version},
                      {"id", id},
                      {"method", std::move(method)},
                      {"params", std::move(params)}}
                     .dump());
}

[[nodiscard]] Json read_response(json_rpc::FrameReader& reader) {
    const auto payload = reader.read();
    REQUIRE(payload);
    return Json::parse(*payload);
}

[[nodiscard]] Json analyze_params(std::string source) {
    const auto cache_key = source;
    return Json{{"rootIdentity", "file:///worker-test.hlsl"},
                {"cacheKey", cache_key},
                {"path", "worker-test.hlsl"},
                {"sources", Json::array({Json{{"path", "worker-test.hlsl"},
                                              {"text", std::move(source)},
                                              {"rewritten", false}}})},
                {"compilerOptions", Json{{"languageVersion", "2021"},
                                         {"targetProfile", ""},
                                         {"entryPoint", ""},
                                         {"defines", Json::array()},
                                         {"includeDirectories", Json::array()},
                                         {"additionalArguments", Json::array()}}}};
}

} // namespace

TEST_CASE("Analysis worker retains and reparses translation units",
          "[analysis][worker][protocol]") {
    std::stringstream input;
    write_request(input, 1, "analyze",
                  analyze_params("float4 main() : SV_Target { return missing; }\n"));
    write_request(input, 2, "analyze",
                  analyze_params("float4 main() : SV_Target { return 1.0.xxxx; }\n"));
    write_request(input, 3, "shutdown");

    std::stringstream output;
    std::stringstream errors;
    REQUIRE(analysis::run_analysis_worker(input, output, errors) == 0);
    CHECK(errors.str().empty());

    json_rpc::FrameReader reader{output, analysis::analysis_worker_max_payload_size};
    const auto first = read_response(reader);
    CHECK(first.at("id") == 1);
    CHECK(first.at("result").at("kind").get<std::uint8_t>() ==
          static_cast<std::uint8_t>(analysis::WorkerAnalysisKind::parsed));
    CHECK_FALSE(first.at("result").at("diagnostics").empty());

    const auto second = read_response(reader);
    CHECK(second.at("id") == 2);
    CHECK(second.at("result").at("kind").get<std::uint8_t>() ==
          static_cast<std::uint8_t>(analysis::WorkerAnalysisKind::reparsed));
    CHECK(second.at("result").at("diagnostics").empty());

    const auto shutdown = read_response(reader);
    CHECK(shutdown.at("id") == 3);
    CHECK(shutdown.at("result").is_object());
    CHECK_FALSE(reader.read());
}

TEST_CASE("Analysis worker reports malformed and unknown requests without exiting",
          "[analysis][worker][protocol][errors]") {
    std::stringstream input;
    json_rpc::FrameWriter writer{input};
    writer.write("{");
    write_request(input, 2, "not-a-method");
    write_request(input, 3, "shutdown");

    std::stringstream output;
    std::stringstream errors;
    REQUIRE(analysis::run_analysis_worker(input, output, errors) == 0);

    json_rpc::FrameReader reader{output, analysis::analysis_worker_max_payload_size};
    const auto malformed = read_response(reader);
    CHECK(malformed.at("id").is_null());
    CHECK(malformed.at("error").at("message").is_string());
    const auto unknown = read_response(reader);
    CHECK(unknown.at("id") == 2);
    CHECK(unknown.at("error").at("message") == "Unknown analysis worker method");
    CHECK(read_response(reader).at("id") == 3);
}
