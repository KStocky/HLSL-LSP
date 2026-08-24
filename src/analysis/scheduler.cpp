#include <hlsl_intellisense/analysis/scheduler.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hlsl_intellisense::analysis {

struct Scheduler::Task final {
    std::string root;
    std::int64_t version{};
    WorkPriority priority{};
    json_rpc::CancellationToken cancellation;
    Work work;
};

struct Scheduler::Worker final {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Task> queue;
    std::unordered_map<std::string, json_rpc::CancellationToken> running;
};

Scheduler::Scheduler(SchedulerOptions options, WorkerCleanup cleanup)
    : options_{options}, cleanup_{std::move(cleanup)} {
    if (options_.worker_count == 0 || options_.queue_capacity < options_.worker_count) {
        throw std::invalid_argument{"Scheduler queue capacity must be at least the worker count"};
    }
    workers_.reserve(options_.worker_count);
    for (std::size_t index = 0; index < options_.worker_count; ++index) {
        workers_.push_back(std::make_unique<Worker>());
    }
    threads_.reserve(options_.worker_count);
    for (std::size_t index = 0; index < options_.worker_count; ++index) {
        threads_.emplace_back(
            [this, index](const std::stop_token& stop) { worker_loop(index, stop); });
    }
}

Scheduler::~Scheduler() { shutdown(); }

bool Scheduler::submit(std::string root, std::int64_t version, WorkPriority priority,
                       json_rpc::CancellationToken cancellation, Work work,
                       const std::function<void()>& admitted) {
    if (root.empty() || !work) {
        throw std::invalid_argument{"Scheduled work requires a root and callable"};
    }
    if (!accepting_.load(std::memory_order_acquire)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        cancellation.cancel();
        return false;
    }

    auto& worker = *workers_[owner_for(root)];
    std::unique_lock lock{worker.mutex};
    if (!accepting_.load(std::memory_order_relaxed)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        cancellation.cancel();
        return false;
    }

    if (priority == WorkPriority::background) {
        const auto pending = std::ranges::find_if(worker.queue, [&root](const auto& task) {
            return task.priority == WorkPriority::background && task.root == root;
        });
        if (pending != worker.queue.end()) {
            if (version >= pending->version) {
                if (admitted) {
                    admitted();
                }
                if (const auto running = worker.running.find(root);
                    running != worker.running.end()) {
                    running->second.cancel();
                }
                pending->cancellation.cancel();
                *pending = Task{.root = std::move(root),
                                .version = version,
                                .priority = priority,
                                .cancellation = std::move(cancellation),
                                .work = std::move(work)};
            } else {
                cancellation.cancel();
            }
            coalesced_.fetch_add(1, std::memory_order_relaxed);
            submitted_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    const auto per_worker_capacity =
        options_.queue_capacity / options_.worker_count +
        (owner_for(root) < options_.queue_capacity % options_.worker_count ? 1U : 0U);
    if (worker.queue.size() >= per_worker_capacity) {
        auto removable = std::ranges::find_if(worker.queue, [](const auto& task) {
            return task.priority == WorkPriority::background;
        });
        if (removable == worker.queue.end()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            cancellation.cancel();
            return false;
        }
        removable->cancellation.cancel();
        worker.queue.erase(removable);
        queued_.fetch_sub(1, std::memory_order_relaxed);
        outstanding_.fetch_sub(1, std::memory_order_relaxed);
        coalesced_.fetch_add(1, std::memory_order_relaxed);
    }

    if (admitted) {
        admitted();
    }
    if (priority == WorkPriority::background) {
        if (const auto running = worker.running.find(root); running != worker.running.end()) {
            running->second.cancel();
        }
    }

    auto insert_at = worker.queue.end();
    if (priority == WorkPriority::interactive) {
        const auto same_root = std::ranges::find_if(worker.queue, [&root](const auto& task) {
            return task.priority == WorkPriority::background && task.root == root;
        });
        insert_at = same_root == worker.queue.end()
                        ? std::ranges::find_if(worker.queue,
                                               [](const auto& task) {
                                                   return task.priority == WorkPriority::background;
                                               })
                        : std::next(same_root);
    }
    worker.queue.insert(insert_at, Task{.root = std::move(root),
                                        .version = version,
                                        .priority = priority,
                                        .cancellation = std::move(cancellation),
                                        .work = std::move(work)});
    submitted_.fetch_add(1, std::memory_order_relaxed);
    const auto queued = queued_.fetch_add(1, std::memory_order_relaxed) + 1;
    outstanding_.fetch_add(1, std::memory_order_release);
    update_peak(queued);
    lock.unlock();
    worker.ready.notify_one();
    return true;
}

void Scheduler::cancel_root(std::string_view root) {
    auto& worker = *workers_[owner_for(root)];
    {
        std::scoped_lock lock{worker.mutex};
        for (auto task = worker.queue.begin(); task != worker.queue.end();) {
            if (task->root == root) {
                task->cancellation.cancel();
                task = worker.queue.erase(task);
                queued_.fetch_sub(1, std::memory_order_relaxed);
                outstanding_.fetch_sub(1, std::memory_order_relaxed);
                cancelled_.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++task;
            }
        }
        if (const auto running = worker.running.find(std::string{root});
            running != worker.running.end()) {
            running->second.cancel();
        }
    }
    idle_.notify_all();
}

void Scheduler::wait_idle() {
    std::unique_lock lock{idle_mutex_};
    idle_.wait(lock, [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
}

void Scheduler::shutdown() {
    if (!accepting_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    for (auto& worker : workers_) {
        {
            std::scoped_lock lock{worker->mutex};
            for (auto& task : worker->queue) {
                task.cancellation.cancel();
                cancelled_.fetch_add(1, std::memory_order_relaxed);
            }
            queued_.fetch_sub(worker->queue.size(), std::memory_order_relaxed);
            outstanding_.fetch_sub(worker->queue.size(), std::memory_order_relaxed);
            worker->queue.clear();
            for (auto& [root, cancellation] : worker->running) {
                static_cast<void>(root);
                cancellation.cancel();
            }
        }
        worker->ready.notify_all();
    }
    for (auto& thread : threads_) {
        thread.request_stop();
    }
    idle_.notify_all();
    threads_.clear();
}

SchedulerMetrics Scheduler::metrics() const {
    return {.submitted = submitted_.load(std::memory_order_relaxed),
            .started = started_.load(std::memory_order_relaxed),
            .completed = completed_.load(std::memory_order_relaxed),
            .cancelled = cancelled_.load(std::memory_order_relaxed),
            .coalesced = coalesced_.load(std::memory_order_relaxed),
            .rejected = rejected_.load(std::memory_order_relaxed),
            .failed = failed_.load(std::memory_order_relaxed),
            .queued = queued_.load(std::memory_order_relaxed),
            .peak_queued = peak_queued_.load(std::memory_order_relaxed),
            .active = active_.load(std::memory_order_relaxed)};
}

std::size_t Scheduler::owner_for(std::string_view root) const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : root) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash % options_.worker_count);
}

void Scheduler::worker_loop(std::size_t index, const std::stop_token& stop) {
    auto& worker = *workers_[index];
    for (;;) {
        Task task;
        {
            std::unique_lock lock{worker.mutex};
            worker.ready.wait(lock, stop, [&worker] { return !worker.queue.empty(); });
            if (worker.queue.empty()) {
                if (stop.stop_requested()) {
                    break;
                }
                continue;
            }
            task = std::move(worker.queue.front());
            worker.queue.pop_front();
            queued_.fetch_sub(1, std::memory_order_relaxed);
            worker.running.insert_or_assign(task.root, task.cancellation);
        }

        started_.fetch_add(1, std::memory_order_relaxed);
        active_.fetch_add(1, std::memory_order_relaxed);
        if (task.cancellation.is_cancellation_requested()) {
            cancelled_.fetch_add(1, std::memory_order_relaxed);
        } else {
            try {
                task.work(index, task.cancellation);
                if (task.cancellation.is_cancellation_requested()) {
                    cancelled_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    completed_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception&) {
                failed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
        outstanding_.fetch_sub(1, std::memory_order_release);
        {
            std::scoped_lock lock{worker.mutex};
            worker.running.erase(task.root);
        }
        idle_.notify_all();
    }
    if (cleanup_) {
        cleanup_(index);
    }
}

void Scheduler::update_peak(std::size_t value) noexcept {
    auto peak = peak_queued_.load(std::memory_order_relaxed);
    while (peak < value &&
           !peak_queued_.compare_exchange_weak(peak, value, std::memory_order_relaxed)) {
    }
}

} // namespace hlsl_intellisense::analysis
