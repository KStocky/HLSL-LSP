#pragma once

#include <hlsl_intellisense/json_rpc/dispatcher.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hlsl_intellisense::analysis {

struct SchedulerOptions {
    std::size_t worker_count{2};
    std::size_t queue_capacity{64};
};

struct SchedulerMetrics {
    std::uint64_t submitted{};
    std::uint64_t started{};
    std::uint64_t completed{};
    std::uint64_t cancelled{};
    std::uint64_t coalesced{};
    std::uint64_t rejected{};
    std::uint64_t failed{};
    std::size_t queued{};
    std::size_t peak_queued{};
    std::size_t active{};
};

enum class WorkPriority : std::uint8_t { background, interactive };

class Scheduler final {
  public:
    using Work = std::function<void(std::size_t, const json_rpc::CancellationToken&)>;
    using WorkerCleanup = std::function<void(std::size_t)>;

    explicit Scheduler(SchedulerOptions options = {}, WorkerCleanup cleanup = {});
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ~Scheduler();

    [[nodiscard]] bool submit(std::string root, std::int64_t version, WorkPriority priority,
                              json_rpc::CancellationToken cancellation, Work work,
                              const std::function<void()>& admitted = {});
    void cancel_root(std::string_view root);
    void wait_idle();
    void shutdown();

    [[nodiscard]] SchedulerMetrics metrics() const;
    [[nodiscard]] std::size_t owner_for(std::string_view root) const noexcept;

  private:
    struct Task;
    struct Worker;

    void worker_loop(std::size_t index, const std::stop_token& stop);
    void update_peak(std::size_t value) noexcept;

    SchedulerOptions options_;
    WorkerCleanup cleanup_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::jthread> threads_;
    std::atomic_bool accepting_{true};
    std::atomic<std::uint64_t> submitted_{};
    std::atomic<std::uint64_t> started_{};
    std::atomic<std::uint64_t> completed_{};
    std::atomic<std::uint64_t> cancelled_{};
    std::atomic<std::uint64_t> coalesced_{};
    std::atomic<std::uint64_t> rejected_{};
    std::atomic<std::uint64_t> failed_{};
    std::atomic<std::size_t> queued_{};
    std::atomic<std::size_t> peak_queued_{};
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> outstanding_{};
    mutable std::mutex idle_mutex_;
    std::condition_variable idle_;
};

} // namespace hlsl_intellisense::analysis
