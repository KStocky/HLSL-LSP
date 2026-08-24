#include <hlsl_intellisense/analysis/manager.h>

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
    CHECK(diagnostic_versions == std::vector<std::int64_t>{1, 2});
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
