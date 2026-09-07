#include <hlsl_intellisense/analysis/manager.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;
namespace workspace = hlsl_intellisense::workspace;

namespace {

class TestDirectory final {
  public:
    TestDirectory() {
        static std::size_t next_id{};
        path_ = std::filesystem::current_path() /
                ("analysis-manager-tests-" + std::to_string(next_id++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
    ~TestDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class Gate final {
  public:
    void enter() {
        {
            std::scoped_lock lock{mutex_};
            entered_ = true;
        }
        changed_.notify_all();
    }

    void wait_until_entered() {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] { return entered_; });
    }

    void wait_until_released() {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] { return released_; });
    }

    void release() {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

[[nodiscard]] analysis::AnalysisOptions test_options(std::size_t units = 4) {
    return {.scheduler = {.worker_count = 1, .queue_capacity = 8},
            .limits = {.max_translation_units = units,
                       .max_translation_unit_estimated_bytes = std::size_t{64} * 1024U * 1024U,
                       .opaque_translation_unit_estimate = std::size_t{1024} * 1024U,
                       .include_cache = {.max_entries = 16,
                                         .max_estimated_bytes = std::size_t{1024} * 1024U}}};
}

[[nodiscard]] analysis::AnalysisInput
input(const workspace::DocumentUri& uri, std::int64_t version, std::string text,
      workspace::WorkspaceConfiguration configuration = {},
      std::vector<workspace::SourceSnapshot> extra_open_documents = {}) {
    workspace::SourceSnapshot root{uri, "hlsl", version, std::move(text)};
    std::vector<workspace::SourceSnapshot> documents;
    documents.push_back(root);
    documents.insert(documents.end(), std::make_move_iterator(extra_open_documents.begin()),
                     std::make_move_iterator(extra_open_documents.end()));
    return {.root = std::move(root),
            .open_documents = std::move(documents),
            .configuration = std::move(configuration)};
}

[[nodiscard]] std::string shader(std::string_view value = "1.0.xxxx") {
    return "float4 helper(float4 value) { return value; }\n"
           "float4 main() : SV_Target { return helper(" +
           std::string{value} + "); }\n";
}

} // namespace

TEST_CASE("Analysis cache measures cold parse, cache hit, reparse, and completion",
          "[analysis][cache][performance]") {
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    std::vector<std::int64_t> diagnostic_versions;
    std::mutex diagnostics_mutex;
    analysis::Manager manager{
        [&](const workspace::SourceSnapshot& snapshot, const auto&, std::uint64_t) {
            std::scoped_lock lock{diagnostics_mutex};
            diagnostic_versions.push_back(snapshot.version());
        },
        test_options()};

    manager.analyze(input(uri, 1, shader()));
    manager.wait_idle();
    auto metrics = manager.metrics();
    CHECK(metrics.parse_count == 1);
    CHECK(metrics.reparse_count == 0);
    CHECK(metrics.cache_misses == 1);
    CHECK(metrics.translation_units == 1);
    CHECK(metrics.translation_unit_estimated_bytes <=
          test_options().limits.max_translation_unit_estimated_bytes);

    manager.analyze(input(uri, 1, shader()));
    manager.wait_idle();
    metrics = manager.metrics();
    CHECK(metrics.cache_hits == 1);
    CHECK(metrics.parse_count == 1);

    manager.analyze(input(uri, 2, shader("2.0.xxxx")));
    manager.wait_idle();
    metrics = manager.metrics();
    CHECK(metrics.reparse_count == 1);
    CHECK(metrics.cache_misses == 2);

    json_rpc::CancellationToken cancellation;
    static_cast<void>(manager.complete(uri.identity(), 2, uri.path(), 2, 40, cancellation));
    metrics = manager.metrics();
    CHECK(metrics.completion_count == 1);
    // Diagnostics are (re-)reported for every analyze(), including the cache
    // hit (second call, same version 1): extraction from the already-parsed
    // translation unit is cheap, and always reporting keeps a consumer's
    // notion of "current generation" reconciled even when a reanalysis
    // resolves to an identical cache key (e.g. a config/variant change that
    // does not affect this particular document).
    CHECK(diagnostic_versions == std::vector<std::int64_t>{1, 1, 2});
}

TEST_CASE("Translation-unit cache evicts the least recently used idle root",
          "[analysis][cache][lru]") {
    TestDirectory directory;
    const auto first =
        workspace::DocumentUri::from_path((directory.path() / "first.hlsl").string());
    const auto second =
        workspace::DocumentUri::from_path((directory.path() / "second.hlsl").string());
    const auto third =
        workspace::DocumentUri::from_path((directory.path() / "third.hlsl").string());
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options(2)};

    manager.analyze(input(first, 1, shader("1.0.xxxx")));
    manager.analyze(input(second, 1, shader("2.0.xxxx")));
    manager.wait_idle();
    manager.analyze(input(first, 1, shader("1.0.xxxx")));
    manager.wait_idle();
    manager.analyze(input(third, 1, shader("3.0.xxxx")));
    manager.wait_idle();

    const auto metrics = manager.metrics();
    CHECK(metrics.translation_units == 2);
    CHECK(metrics.cache_evictions == 1);
    json_rpc::CancellationToken cancellation;
    CHECK_THROWS_AS(manager.symbols(second.identity(), 1, cancellation), json_rpc::HandlerError);
    CHECK_FALSE(manager.symbols(first.identity(), 1, cancellation).empty());
}

TEST_CASE("Analysis keys include version, compiler configuration, and dependency state",
          "[analysis][cache][keys]") {
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options()};

    workspace::WorkspaceConfiguration first_configuration;
    first_configuration.language_version = "2018";
    manager.analyze(input(uri, 1, shader(), first_configuration));
    manager.wait_idle();

    workspace::WorkspaceConfiguration second_configuration;
    second_configuration.language_version = "2021";
    manager.analyze(input(uri, 1, shader(), second_configuration));
    manager.wait_idle();
    auto metrics = manager.metrics();
    CHECK(metrics.parse_count == 2);
    CHECK(metrics.cache_misses == 2);

    manager.analyze(input(uri, 2, shader(), second_configuration));
    manager.wait_idle();
    metrics = manager.metrics();
    CHECK(metrics.reparse_count == 1);
    CHECK(metrics.cache_misses == 3);
    CHECK(analysis::Manager::configuration_fingerprint(first_configuration) !=
          analysis::Manager::configuration_fingerprint(second_configuration));
}

TEST_CASE("Dependency metadata invalidates only dependent roots",
          "[analysis][dependencies][invalidation]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float4 sharedValue = 1.0.xxxx;\n";
    }
    const auto include = workspace::DocumentUri::from_path(include_path.string());
    const auto dependent =
        workspace::DocumentUri::from_path((directory.path() / "dependent.hlsl").string());
    const auto independent =
        workspace::DocumentUri::from_path((directory.path() / "independent.hlsl").string());
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options()};
    manager.analyze(
        input(dependent, 1,
              "#include \"shared.hlsli\"\nfloat4 main() : SV_Target { return sharedValue; }\n"));
    manager.analyze(input(independent, 1, shader()));
    manager.wait_idle();

    const std::unordered_set changed{include.identity()};
    const auto affected = manager.dependent_root_uris(changed);
    REQUIRE(affected.size() == 1);
    CHECK(affected.front() == dependent.uri());

    {
        std::ofstream changed_include{include_path, std::ios::trunc};
        REQUIRE(changed_include);
        changed_include << "static const float4 sharedValue = 2.0.xxxx;\n";
    }
    manager.invalidate_include_metadata(changed);
    manager.analyze(
        input(dependent, 1,
              "#include \"shared.hlsli\"\nfloat4 main() : SV_Target { return sharedValue; }\n"));
    manager.wait_idle();
    CHECK(manager.metrics().reparse_count == 1);
}

TEST_CASE("Configured macro includes participate in dependency invalidation",
          "[analysis][dependencies][includes][macros]") {
    TestDirectory directory;
    const auto include_path = directory.path() / "configured.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float4 configuredValue = 1.0.xxxx;\n";
    }
    const auto include = workspace::DocumentUri::from_path(include_path.string());
    const auto root = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("CONFIGURED_HEADER", "\"configured.hlsli\"");
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options()};
    manager.analyze(input(root, 1,
                          "#include CONFIGURED_HEADER\n"
                          "float4 main() : SV_Target { return configuredValue; }\n",
                          configuration));
    manager.wait_idle();

    const auto roots = manager.roots();
    REQUIRE(roots.size() == 1);
    CHECK_FALSE(roots.front().has_dynamic_includes);
    CHECK(roots.front().dependency_identities.contains(include.identity()));
    const std::unordered_set changed{include.identity()};
    const auto affected = manager.dependent_root_uris(changed);
    REQUIRE(affected.size() == 1);
    CHECK(affected.front() == root.uri());

    {
        std::ofstream changed_include{include_path, std::ios::trunc};
        REQUIRE(changed_include);
        changed_include << "static const float4 configuredValue = 2.0.xxxx;\n";
    }
    manager.invalidate_include_metadata(changed);
    manager.analyze(input(root, 1,
                          "#include CONFIGURED_HEADER\n"
                          "float4 main() : SV_Target { return configuredValue; }\n",
                          configuration));
    manager.wait_idle();
    CHECK(manager.metrics().reparse_count == 1);
}

TEST_CASE("Later DXC macro definitions keep configured include dependencies dynamic",
          "[analysis][dependencies][includes][macros][arguments][integration]") {
    TestDirectory directory;
    std::filesystem::create_directories(directory.path() / "Configured");
    const auto configured_path = directory.path() / "Configured" / "configured.hlsli";
    {
        std::ofstream configured{configured_path};
        REQUIRE(configured);
        configured << "static const float4 configuredValue = 1.0.xxxx;\n";
    }
    {
        std::ofstream runtime{directory.path() / "runtime.hlsli"};
        REQUIRE(runtime);
        runtime << "static const float4 runtimeValue = 2.0.xxxx;\n";
    }

    const auto root = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    const auto configured = workspace::DocumentUri::from_path(configured_path.string());
    workspace::WorkspaceConfiguration configuration;
    configuration.preprocessor_definitions.emplace("HEADER", "HEADER_TARGET");
    configuration.preprocessor_definitions.emplace("HEADER_TARGET",
                                                   "\"/Configured/configured.hlsli\"");
    configuration.virtual_directory_mappings.emplace("/Configured",
                                                     directory.path() / "Configured");
    configuration.additional_arguments = {"-DHEADER=\"runtime.hlsli\""};

    std::vector<std::string> diagnostics;
    analysis::Manager manager{
        [&](const workspace::SourceSnapshot&, const auto& items, std::uint64_t) {
            diagnostics.clear();
            for (const auto& item : items) {
                diagnostics.push_back(item.message);
            }
        },
        test_options()};
    manager.analyze(input(root, 1,
                          "#include HEADER\n"
                          "float4 main() : SV_Target { return runtimeValue; }\n",
                          configuration));
    manager.wait_idle();

    const auto roots = manager.roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots.front().has_dynamic_includes);
    CHECK_FALSE(roots.front().dependency_identities.contains(configured.identity()));
    CHECK(std::ranges::none_of(diagnostics, [](const auto& message) {
        return message.find("runtimeValue") != std::string::npos ||
               message.find("runtime.hlsli") != std::string::npos;
    }));
}

TEST_CASE("Interactive analysis cancellation returns before blocked worker cleanup",
          "[analysis][cancellation]") {
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    auto hooks = std::make_shared<analysis::AnalysisHooks>();
    Gate interactive;
    hooks->before_interactive = [&](std::string_view) {
        interactive.enter();
        interactive.wait_until_released();
    };
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options(),
                              hooks};
    manager.analyze(input(uri, 1, shader()));
    manager.wait_idle();

    json_rpc::CancellationToken cancellation;
    auto request = std::async(std::launch::async,
                              [&] { return manager.symbols(uri.identity(), 1, cancellation); });
    interactive.wait_until_entered();
    cancellation.cancel();
    try {
        static_cast<void>(request.get());
        FAIL("Cancelled request unexpectedly returned symbols");
    } catch (const json_rpc::HandlerError& error) {
        CHECK(error.code() == json_rpc::request_cancelled_code);
    }

    interactive.release();
    manager.wait_idle();
    CHECK(manager.metrics().scheduler.cancelled >= 1);
}

TEST_CASE("Memory layout queries preserve cancellation and stale-version safety",
          "[analysis][memory-layout][cancellation]") {
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    auto hooks = std::make_shared<analysis::AnalysisHooks>();
    Gate interactive;
    hooks->before_interactive = [&](std::string_view) {
        interactive.enter();
        interactive.wait_until_released();
    };
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options(),
                              hooks};
    manager.analyze(input(uri, 1, "struct Data { float3 value; };\n"));
    manager.wait_idle();

    json_rpc::CancellationToken cancellation;
    auto request = std::async(std::launch::async, [&] {
        return manager.memory_layout(uri.identity(), 1, uri.path(), 1, 23, cancellation);
    });
    interactive.wait_until_entered();
    cancellation.cancel();
    try {
        static_cast<void>(request.get());
        FAIL("Cancelled memory layout unexpectedly returned");
    } catch (const json_rpc::HandlerError& error) {
        CHECK(error.code() == json_rpc::request_cancelled_code);
    }
    interactive.release();
    manager.wait_idle();

    json_rpc::CancellationToken current;
    CHECK_THROWS_AS(manager.memory_layout(uri.identity(), 2, uri.path(), 1, 23, current),
                    json_rpc::HandlerError);
}

TEST_CASE("Inlay hint queries preserve cancellation and stale-version safety",
          "[analysis][inlay-hints][cancellation]") {
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    const std::string source = "float shade(float value) { return value; }\n"
                               "float4 main() : SV_Target { return shade(1.0).xxxx; }\n";
    auto hooks = std::make_shared<analysis::AnalysisHooks>();
    Gate interactive;
    hooks->before_interactive = [&](std::string_view) {
        interactive.enter();
        interactive.wait_until_released();
    };
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options(),
                              hooks};
    manager.analyze(input(uri, 1, source));
    manager.wait_idle();

    json_rpc::CancellationToken cancellation;
    auto request = std::async(std::launch::async, [&] {
        return manager.inlay_hints(
            uri.identity(), 1, uri.path(),
            {{.start = 0, .end = static_cast<std::uint32_t>(source.size())}},
            {{.line = 2,
              .column = 36,
              .argument_offsets = {static_cast<std::uint32_t>(source.find("1.0"))}}},
            {}, cancellation);
    });
    interactive.wait_until_entered();
    cancellation.cancel();
    try {
        static_cast<void>(request.get());
        FAIL("Cancelled inlay hint request unexpectedly returned");
    } catch (const json_rpc::HandlerError& error) {
        CHECK(error.code() == json_rpc::request_cancelled_code);
    }
    interactive.release();
    manager.wait_idle();

    json_rpc::CancellationToken current;
    CHECK_THROWS_AS(
        manager.inlay_hints(uri.identity(), 2, uri.path(),
                            {{.start = 0, .end = static_cast<std::uint32_t>(source.size())}}, {},
                            {}, current),
        json_rpc::HandlerError);
}

TEST_CASE("Compilation info queries (root signature, binding analysis, compatibility) preserve "
          "cancellation and stale-version safety",
          "[analysis][compilation-info][cancellation]") {
    // Manager::compilation_info is the single entry point through which the
    // new resource-binding-analysis, root-signature, and compatibility data
    // reach the LSP layer; it must honor the same cancellation and
    // stale-version guarantees as every other interactive query, rather than
    // fabricating or reusing a result computed for a version the client has
    // since superseded.
    TestDirectory directory;
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    auto hooks = std::make_shared<analysis::AnalysisHooks>();
    Gate interactive;
    hooks->before_interactive = [&](std::string_view) {
        interactive.enter();
        interactive.wait_until_released();
    };
    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options(),
                              hooks};
    manager.analyze(input(uri, 1, shader()));
    manager.wait_idle();

    json_rpc::CancellationToken cancellation;
    auto request = std::async(std::launch::async, [&] {
        return manager.compilation_info(uri.identity(), 1, uri.path(), cancellation);
    });
    interactive.wait_until_entered();
    cancellation.cancel();
    try {
        static_cast<void>(request.get());
        FAIL("Cancelled compilation info request unexpectedly returned");
    } catch (const json_rpc::HandlerError& error) {
        CHECK(error.code() == json_rpc::request_cancelled_code);
    }
    interactive.release();
    manager.wait_idle();

    json_rpc::CancellationToken current;
    CHECK_THROWS_AS(manager.compilation_info(uri.identity(), 2, uri.path(), current),
                    json_rpc::HandlerError);
}

TEST_CASE("Entry-point data flow generation stays paired with the analysis that produced it "
          "across an intervening reanalysis",
          "[analysis][entry-point-data-flow][concurrency][generation]") {
    // Regression for a TOCTOU bug: a result from `Manager::entry_point_data_flow`
    // and the content generation used to tag `CallHierarchyItem.data` must
    // both describe the *same* analysis. The old design fetched them via
    // two separate `Manager` calls (`entry_point_data_flow` then a
    // standalone `content_generation`); an included file's edit reparsing
    // this root *without bumping its own document version* could complete
    // in the gap between those two calls, tagging a result computed from
    // the *old* content with a generation describing the *new* content
    // instead. `WithGeneration` fixes this by reading the generation from
    // inside the exact same query invocation that computes the result, so
    // the pair can never straddle an intervening reanalysis.
    //
    // This is deliberately a fully sequential (non-racy) reproduction
    // rather than a genuinely concurrent one: the scheduler already
    // cancels any in-flight *interactive* query the instant a background
    // reanalysis is submitted for the same root (see
    // `Scheduler::submit`'s `running->second.cancel()` for
    // `WorkPriority::background`), so a query cannot observe a reanalysis
    // completing *while it is itself running* -- the only way the bug
    // manifested was a reanalysis slipping in *between* two already-
    // completed, separately-issued calls, which this test reproduces
    // deterministically without any timing dependency.
    TestDirectory directory;
    const auto include_path = directory.path() / "shared.hlsli";
    {
        std::ofstream include{include_path};
        REQUIRE(include);
        include << "static const float sharedValue = 1.0;\n";
    }
    const auto include = workspace::DocumentUri::from_path(include_path.string());
    const auto uri = workspace::DocumentUri::from_path((directory.path() / "root.hlsl").string());
    const std::string source = "#include \"shared.hlsli\"\n"
                               "float4 main() : SV_Target { return sharedValue.xxxx; }\n";
    workspace::WorkspaceConfiguration configuration;
    configuration.target_profile = "ps_6_6";
    configuration.entry_point = "main";

    analysis::Manager manager{[](const auto&, const auto&, std::uint64_t) {}, test_options()};
    manager.analyze(input(uri, 1, source, configuration));
    manager.wait_idle();

    json_rpc::CancellationToken cancellation;
    const auto baseline = manager.entry_point_data_flow(uri.identity(), 1, {}, cancellation);
    REQUIRE(baseline.value.found);
    bool baseline_has_shared_value = false;
    for (const auto& access : baseline.value.global_accesses) {
        if (access.name == "sharedValue") {
            baseline_has_shared_value = true;
        }
    }
    CHECK(baseline_has_shared_value);

    // Reparse this root (same document version) purely because its include
    // changed -- exactly the class of event that bumps `generation` without
    // touching `version`, and the class of event the old split-call design
    // could straddle.
    {
        std::ofstream changed_include{include_path, std::ios::trunc};
        REQUIRE(changed_include);
        changed_include << "static const float sharedValue = 2.0;\n"
                           "static const float extraValue = 3.0;\n";
    }
    const std::unordered_set changed{include.identity()};
    manager.invalidate_include_metadata(changed);
    manager.analyze(input(uri, 1, source, configuration));
    manager.wait_idle();
    CHECK(manager.metrics().reparse_count == 1);

    // A standalone generation fetch issued *after* the reanalysis reflects
    // the *new* content -- this is what the old code would have wrongly
    // paired with `baseline`'s already-computed, pre-reanalysis value had
    // it fetched the generation as a second, separate call at this point.
    const auto post_reanalysis_generation =
        manager.content_generation(uri.identity(), 1, cancellation);
    CHECK(post_reanalysis_generation != baseline.generation);

    // A fresh, atomic `entry_point_data_flow` call correctly pairs its own
    // (new) value with the (new) generation that produced it -- proving
    // `WithGeneration` never lets a value and a generation from different
    // analyses become associated with each other.
    const auto reparsed = manager.entry_point_data_flow(uri.identity(), 1, {}, cancellation);
    CHECK(reparsed.generation == post_reanalysis_generation);
    CHECK(reparsed.generation != baseline.generation);
}
