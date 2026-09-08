#include <hlsl_intellisense/analysis/manager.h>
#include <hlsl_intellisense/workspace/document_uri.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef HLSL_BENCHMARK_SHADER_DIR
#error HLSL_BENCHMARK_SHADER_DIR must identify the checked-in shader corpus
#endif

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;
namespace workspace = hlsl_intellisense::workspace;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error{"Unable to read benchmark shader: " + path.string()};
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::uint64_t microseconds_since(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

[[nodiscard]] analysis::AnalysisInput make_input(const workspace::DocumentUri& uri,
                                                 std::int64_t version,
                                                 const std::string& source) {
    workspace::SourceSnapshot root{uri, "hlsl", version, source};
    return {.root = root,
            .open_documents = std::vector<workspace::SourceSnapshot>{root},
            .configuration = {}};
}

[[nodiscard]] std::string generated_large_shader() {
    constexpr std::size_t function_count = 4096;
    std::ostringstream source;
    for (std::size_t index = 0; index < function_count; ++index) {
        source << "float generated" << index
               << "(float value) { return value + " << index << ".0; }\n";
    }
    source << "float4 main() : SV_Target { return generated" << function_count - 1U
           << "(1.0).xxxx; }\n";
    return source.str();
}

} // namespace

int main() {
    try {
        const std::filesystem::path shader_directory{HLSL_BENCHMARK_SHADER_DIR};
        const auto shader_path = shader_directory / "representative.hlsl";
        auto source = read_file(shader_path);
        const auto uri = workspace::DocumentUri::from_path(shader_path.string());
        std::atomic_size_t diagnostics{};
        std::atomic_size_t diagnostic_items{};
        std::vector<std::string> errors;
        analysis::AnalysisOptions options{
            .scheduler = {.worker_count = 1, .queue_capacity = 8},
            .limits = {.max_translation_units = 2,
                       .max_translation_unit_estimated_bytes =
                           std::size_t{64} * 1024U * 1024U,
                       .opaque_translation_unit_estimate = std::size_t{4} * 1024U * 1024U,
                       .include_cache = {.max_entries = 16,
                                         .max_estimated_bytes =
                                             std::size_t{1024} * 1024U}}};
        analysis::Manager manager{
            [&diagnostics, &diagnostic_items](const auto&, const auto& items, std::uint64_t) {
                diagnostics.fetch_add(1);
                diagnostic_items.fetch_add(items.size());
            },
            options, {},
            [&errors](std::string_view error) { errors.emplace_back(error); }};

        const auto cold_start = Clock::now();
        manager.analyze(make_input(uri, 1, source));
        manager.wait_idle();
        const auto cold_microseconds = microseconds_since(cold_start);

        const auto warm_start = Clock::now();
        manager.analyze(make_input(uri, 1, source));
        manager.wait_idle();
        const auto warm_microseconds = microseconds_since(warm_start);

        source += "\nstatic const float benchmarkEdit = 1.0;\n";
        const auto reparse_start = Clock::now();
        manager.analyze(make_input(uri, 2, source));
        manager.wait_idle();
        const auto reparse_microseconds = microseconds_since(reparse_start);

        json_rpc::CancellationToken cancellation;
        const auto completion_start = Clock::now();
        static_cast<void>(manager.complete(uri.identity(), 2, uri.path(), 18, 20, cancellation));
        const auto completion_microseconds = microseconds_since(completion_start);

        for (const auto name : {"second.hlsl", "third.hlsl"}) {
            const auto extra =
                workspace::DocumentUri::from_path((shader_directory / name).string());
            manager.analyze(make_input(extra, 1, source));
            manager.wait_idle();
        }

        const auto generated_source = generated_large_shader();
        const auto generated_uri =
            workspace::DocumentUri::from_path((shader_directory / "generated-large.hlsl").string());
        const auto generated_start = Clock::now();
        manager.analyze(make_input(generated_uri, 1, generated_source));
        manager.wait_idle();
        const auto generated_microseconds = microseconds_since(generated_start);

        const auto metrics = manager.metrics();
        std::cout << "{\n"
                  << "  \"coldParseWallMicroseconds\": " << cold_microseconds << ",\n"
                  << "  \"warmCacheWallMicroseconds\": " << warm_microseconds << ",\n"
                  << "  \"reparseWallMicroseconds\": " << reparse_microseconds << ",\n"
                  << "  \"completionWallMicroseconds\": " << completion_microseconds << ",\n"
                  << "  \"generatedLargeBytes\": " << generated_source.size() << ",\n"
                  << "  \"generatedLargeWallMicroseconds\": " << generated_microseconds << ",\n"
                  << "  \"parseCount\": " << metrics.parse_count << ",\n"
                  << "  \"reparseCount\": " << metrics.reparse_count << ",\n"
                  << "  \"cacheHits\": " << metrics.cache_hits << ",\n"
                  << "  \"cacheMisses\": " << metrics.cache_misses << ",\n"
                  << "  \"cacheEvictions\": " << metrics.cache_evictions << ",\n"
                  << "  \"translationUnits\": " << metrics.translation_units << ",\n"
                  << "  \"estimatedBytes\": "
                  << metrics.translation_unit_estimated_bytes << ",\n"
                  << "  \"includeCacheHits\": " << metrics.include_cache.hits << ",\n"
                  << "  \"includeCacheMisses\": " << metrics.include_cache.misses << "\n"
                  << "}\n";

        const auto structurally_valid =
            errors.empty() && diagnostics.load() >= 5 && diagnostic_items.load() == 0 &&
            generated_source.size() >= std::size_t{200} * 1024U &&
            generated_microseconds < std::uint64_t{30} * 1000U * 1000U &&
            metrics.parse_count >= 4 &&
            metrics.reparse_count >= 1 && metrics.completion_count == 1 &&
            metrics.cache_hits >= 1 && metrics.cache_misses >= 4 &&
            metrics.cache_evictions >= 1 && metrics.translation_units <= 2 &&
            metrics.translation_unit_estimated_bytes <=
                options.limits.max_translation_unit_estimated_bytes &&
            metrics.include_cache.entries <= options.limits.include_cache.max_entries &&
            metrics.include_cache.estimated_bytes <=
                options.limits.include_cache.max_estimated_bytes;
        return structurally_valid ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
