#include <hlsl_intellisense/analysis/scheduler.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace analysis = hlsl_intellisense::analysis;
namespace json_rpc = hlsl_intellisense::json_rpc;

namespace {

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

} // namespace

TEST_CASE("Scheduler bounds its queue and coalesces superseded root versions",
          "[analysis][scheduler]") {
    analysis::Scheduler scheduler{{.worker_count = 1, .queue_capacity = 3}};
    Gate blocker;
    json_rpc::CancellationToken blocker_cancellation;
    REQUIRE(scheduler.submit("blocker", 0, analysis::WorkPriority::interactive,
                             blocker_cancellation, [&blocker](std::size_t, const auto&) {
                                 blocker.enter();
                                 blocker.wait_until_released();
                             }));
    blocker.wait_until_entered();

    std::vector<std::int64_t> observed;
    std::mutex observed_mutex;
    for (std::int64_t version = 1; version <= 3; ++version) {
        json_rpc::CancellationToken cancellation;
        REQUIRE(scheduler.submit("root", version, analysis::WorkPriority::background, cancellation,
                                 [version, &observed, &observed_mutex](std::size_t, const auto&) {
                                     std::scoped_lock lock{observed_mutex};
                                     observed.push_back(version);
                                 }));
    }
    blocker.release();
    scheduler.wait_idle();

    CHECK(observed == std::vector<std::int64_t>{3});
    const auto metrics = scheduler.metrics();
    CHECK(metrics.coalesced == 2);
    CHECK(metrics.peak_queued <= 3);
    CHECK(metrics.queued == 0);
}

TEST_CASE("Queued and running scheduler work observes cancellation deterministically",
          "[analysis][scheduler][cancellation]") {
    analysis::Scheduler scheduler{{.worker_count = 1, .queue_capacity = 2}};
    Gate blocker;
    json_rpc::CancellationToken blocker_cancellation;
    REQUIRE(scheduler.submit("blocker", 0, analysis::WorkPriority::interactive,
                             blocker_cancellation, [&blocker](std::size_t, const auto&) {
                                 blocker.enter();
                                 blocker.wait_until_released();
                             }));
    blocker.wait_until_entered();

    std::atomic_bool queued_ran{};
    json_rpc::CancellationToken queued_cancellation;
    REQUIRE(scheduler.submit("queued", 1, analysis::WorkPriority::interactive, queued_cancellation,
                             [&queued_ran](std::size_t, const auto&) { queued_ran.store(true); }));
    queued_cancellation.cancel();
    blocker.release();
    scheduler.wait_idle();
    CHECK_FALSE(queued_ran.load());

    Gate running;
    std::atomic_bool saw_cancellation{};
    json_rpc::CancellationToken running_cancellation;
    REQUIRE(scheduler.submit("running", 1, analysis::WorkPriority::interactive,
                             running_cancellation,
                             [&running, &saw_cancellation](std::size_t, const auto& cancellation) {
                                 running.enter();
                                 running.wait_until_released();
                                 saw_cancellation.store(cancellation.is_cancellation_requested());
                             }));
    running.wait_until_entered();
    running_cancellation.cancel();
    running.release();
    scheduler.wait_idle();
    CHECK(saw_cancellation.load());
}

TEST_CASE("Rejected background work does not cancel the running analysis",
          "[analysis][scheduler][capacity]") {
    analysis::Scheduler scheduler{{.worker_count = 1, .queue_capacity = 1}};
    Gate running;
    std::atomic_bool running_cancelled{};
    json_rpc::CancellationToken running_cancellation;
    REQUIRE(scheduler.submit("root", 1, analysis::WorkPriority::background, running_cancellation,
                             [&running, &running_cancelled](std::size_t, const auto& cancellation) {
                                 running.enter();
                                 running.wait_until_released();
                                 running_cancelled.store(cancellation.is_cancellation_requested());
                             }));
    running.wait_until_entered();

    json_rpc::CancellationToken queued_cancellation;
    REQUIRE(scheduler.submit("interactive", 1, analysis::WorkPriority::interactive,
                             queued_cancellation, [](std::size_t, const auto&) {}));
    json_rpc::CancellationToken rejected_cancellation;
    CHECK_FALSE(scheduler.submit("root", 2, analysis::WorkPriority::background,
                                 rejected_cancellation, [](std::size_t, const auto&) {}));

    running.release();
    scheduler.wait_idle();
    CHECK_FALSE(running_cancelled.load());
    CHECK(rejected_cancellation.is_cancellation_requested());
}

TEST_CASE("Scheduler owns roots deterministically and permits only independent concurrency",
          "[analysis][scheduler][concurrency]") {
    analysis::Scheduler scheduler{{.worker_count = 2, .queue_capacity = 4}};
    std::string first = "root-a";
    std::string second = "root-b";
    while (scheduler.owner_for(first) == scheduler.owner_for(second)) {
        second.push_back('-');
    }

    std::barrier both_workers{3};
    json_rpc::CancellationToken first_cancellation;
    json_rpc::CancellationToken second_cancellation;
    REQUIRE(scheduler.submit(
        first, 1, analysis::WorkPriority::interactive, first_cancellation,
        [&both_workers](std::size_t, const auto&) { both_workers.arrive_and_wait(); }));
    REQUIRE(scheduler.submit(
        second, 1, analysis::WorkPriority::interactive, second_cancellation,
        [&both_workers](std::size_t, const auto&) { both_workers.arrive_and_wait(); }));
    both_workers.arrive_and_wait();
    scheduler.wait_idle();

    CHECK(scheduler.owner_for(first) != scheduler.owner_for(second));
    CHECK(scheduler.owner_for(first) == scheduler.owner_for(first));

    std::atomic_int active{};
    std::atomic_int peak{};
    Gate first_same_root;
    json_rpc::CancellationToken third_cancellation;
    json_rpc::CancellationToken fourth_cancellation;
    REQUIRE(scheduler.submit(first, 2, analysis::WorkPriority::interactive, third_cancellation,
                             [&](std::size_t, const auto&) {
                                 const auto now = active.fetch_add(1) + 1;
                                 peak.store((std::max)(peak.load(), now));
                                 first_same_root.enter();
                                 first_same_root.wait_until_released();
                                 active.fetch_sub(1);
                             }));
    first_same_root.wait_until_entered();
    REQUIRE(scheduler.submit(first, 3, analysis::WorkPriority::interactive, fourth_cancellation,
                             [&](std::size_t, const auto&) {
                                 const auto now = active.fetch_add(1) + 1;
                                 peak.store((std::max)(peak.load(), now));
                                 active.fetch_sub(1);
                             }));
    first_same_root.release();
    scheduler.wait_idle();
    CHECK(peak.load() == 1);
}

TEST_CASE("Scheduler shutdown cancels work and performs cleanup on each owner",
          "[analysis][scheduler][shutdown]") {
    std::atomic_int cleanups{};
    std::atomic_bool cancelled{};
    {
        analysis::Scheduler scheduler{{.worker_count = 1, .queue_capacity = 2},
                                      [&cleanups](std::size_t) { cleanups.fetch_add(1); }};
        Gate running;
        json_rpc::CancellationToken cancellation;
        cancellation.on_cancel([&running] { running.release(); });
        REQUIRE(scheduler.submit("root", 1, analysis::WorkPriority::interactive, cancellation,
                                 [&running, &cancelled](std::size_t, const auto& token) {
                                     running.enter();
                                     running.wait_until_released();
                                     cancelled.store(token.is_cancellation_requested());
                                 }));
        running.wait_until_entered();
        scheduler.shutdown();
    }
    CHECK(cancelled.load());
    CHECK(cleanups.load() == 1);
}
