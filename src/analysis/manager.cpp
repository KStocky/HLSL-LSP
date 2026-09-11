#include <hlsl_intellisense/analysis/manager.h>

#include <hlsl_intellisense/analysis/worker_protocol.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace hlsl_intellisense::analysis {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_microseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void append_component(std::string& destination, std::string_view value) {
    destination += std::to_string(value.size());
    destination.push_back(':');
    destination.append(value);
    destination.push_back(';');
}

[[nodiscard]] std::string source_fingerprint(const workspace::IncludeResolution& resolution,
                                             std::string_view configuration, std::int64_t version) {
    std::string result;
    result.reserve(configuration.size() + 64);
    result += std::to_string(version);
    result.push_back('|');
    append_component(result, configuration);
    for (const auto& source : resolution.sources) {
        append_component(result, source.path);
        append_component(result, source.text);
    }
    std::vector<std::string_view> dependencies;
    dependencies.reserve(resolution.dependency_identities.size());
    for (const auto& dependency : resolution.dependency_identities) {
        dependencies.push_back(dependency);
    }
    std::ranges::sort(dependencies);
    for (const auto dependency : dependencies) {
        append_component(result, dependency);
    }
    result.push_back(resolution.has_dynamic_includes ? 'D' : 'S');
    return result;
}

[[nodiscard]] std::string wire_fingerprint(std::string_view value) {
    std::uint64_t first{14695981039346656037ULL};
    std::uint64_t second{1099511628211ULL};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        first = (first ^ byte) * 1099511628211ULL;
        second = (second ^ (byte + 0x9dU)) * 14029467366897019727ULL;
    }
    // The parent retains and compares the exact fingerprint. A collision in
    // this compact wire key can therefore only cause a cache-transition
    // mismatch and worker restart, never acceptance of stale analysis.
    return std::to_string(value.size()) + ':' + std::to_string(first) + ':' +
           std::to_string(second);
}

[[nodiscard]] std::size_t
estimate_sources(const workspace::IncludeResolution& resolution) noexcept {
    std::size_t bytes{};
    for (const auto& source : resolution.sources) {
        bytes += sizeof(dxc::SourceFile) + source.path.capacity() + source.text.capacity();
    }
    for (const auto& dependency : resolution.dependency_identities) {
        bytes += sizeof(std::string) + dependency.capacity();
    }
    return bytes;
}

[[nodiscard]] std::size_t estimate_snapshot(const workspace::SourceSnapshot& snapshot) noexcept {
    return sizeof(workspace::SourceSnapshot) + snapshot.uri().size() + snapshot.path().size() +
           snapshot.language_id().size() + snapshot.text().size();
}

[[nodiscard]] std::shared_ptr<AnalysisInput>
make_recovery_input(const AnalysisInput& input, const workspace::IncludeResolution& resolution) {
    auto recovery =
        std::make_shared<AnalysisInput>(AnalysisInput{.root = input.root,
                                                      .open_documents = {},
                                                      .configuration = input.configuration,
                                                      .generation = input.generation});
    recovery->open_documents.reserve(input.open_documents.size());
    for (const auto& document : input.open_documents) {
        if (resolution.has_dynamic_includes ||
            document.document_uri().identity() == input.root.document_uri().identity() ||
            resolution.dependency_identities.contains(document.document_uri().identity())) {
            recovery->open_documents.push_back(document);
        }
    }
    return recovery;
}

[[nodiscard]] std::size_t estimate_recovery_input(const AnalysisInput& input) noexcept {
    auto bytes = sizeof(AnalysisInput) + estimate_snapshot(input.root);
    for (const auto& document : input.open_documents) {
        bytes += estimate_snapshot(document);
    }
    return bytes;
}

template <typename Result> class AsyncResult final {
  public:
    [[nodiscard]] std::future<Result> future() { return promise_.get_future(); }

    void set_value(Result value) {
        std::scoped_lock lock{mutex_};
        if (completed_) {
            return;
        }
        completed_ = true;
        promise_.set_value(std::move(value));
    }

    void set_exception(const std::exception_ptr& exception) {
        std::scoped_lock lock{mutex_};
        if (completed_) {
            return;
        }
        completed_ = true;
        promise_.set_exception(exception);
    }

  private:
    std::mutex mutex_;
    bool completed_{};
    std::promise<Result> promise_;
};

[[nodiscard]] std::size_t worker_share(std::size_t total, std::size_t workers,
                                       std::size_t index) noexcept {
    return total / workers + (index < total % workers ? 1U : 0U);
}

[[nodiscard]] AnalysisOptions validate_options(AnalysisOptions options) {
    if (options.scheduler.worker_count == 0 ||
        options.scheduler.queue_capacity < options.scheduler.worker_count) {
        throw std::invalid_argument{"Scheduler queue capacity must be at least the worker count"};
    }
    if (options.limits.max_translation_units < options.scheduler.worker_count ||
        options.limits.max_translation_unit_estimated_bytes / options.scheduler.worker_count <
            options.limits.opaque_translation_unit_estimate ||
        options.limits.include_cache.max_entries < options.scheduler.worker_count ||
        options.limits.include_cache.max_estimated_bytes < options.scheduler.worker_count ||
        options.limits.opaque_translation_unit_estimate == 0) {
        throw std::invalid_argument{
            "Analysis cache limits must provide positive capacity for every worker"};
    }
    if (options.budgets.background_timeout <= std::chrono::milliseconds::zero() ||
        options.budgets.interactive_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument{"Analysis timeouts must be positive"};
    }
    return options;
}

[[nodiscard]] AnalysisUnavailable unavailable_result(const WorkerProcessError& error) {
    auto reason = AnalysisUnavailableReason::worker_error;
    switch (error.code()) {
    case WorkerProcessErrorCode::timed_out:
        reason = AnalysisUnavailableReason::timed_out;
        break;
    case WorkerProcessErrorCode::worker_exited:
    case WorkerProcessErrorCode::unexpected_eof:
    case WorkerProcessErrorCode::io_error:
        reason = AnalysisUnavailableReason::worker_crashed;
        break;
    case WorkerProcessErrorCode::malformed_reply:
    case WorkerProcessErrorCode::mismatched_reply:
        reason = AnalysisUnavailableReason::protocol_error;
        break;
    case WorkerProcessErrorCode::launch_failed:
        reason = AnalysisUnavailableReason::launch_failed;
        break;
    case WorkerProcessErrorCode::worker_error:
    case WorkerProcessErrorCode::cancelled:
        reason = AnalysisUnavailableReason::worker_error;
        break;
    }
    return {.reason = reason, .message = error.what()};
}

[[nodiscard]] bool automatically_recoverable(const WorkerProcessErrorCode code) noexcept {
    return code == WorkerProcessErrorCode::worker_exited ||
           code == WorkerProcessErrorCode::unexpected_eof ||
           code == WorkerProcessErrorCode::io_error;
}

[[nodiscard]] json_rpc::HandlerError
interactive_error(const WorkerProcessError& error, bool externally_cancelled, bool shutting_down) {
    if (externally_cancelled) {
        return {json_rpc::request_cancelled_code, "Request cancelled"};
    }
    if (error.code() == WorkerProcessErrorCode::cancelled) {
        return {shutting_down ? json_rpc::server_cancelled_code : json_rpc::content_modified_code,
                shutting_down ? "Analysis server is shutting down"
                              : "Analysis was superseded; the DXC worker was restarted"};
    }
    if (error.code() == WorkerProcessErrorCode::malformed_reply ||
        error.code() == WorkerProcessErrorCode::mismatched_reply ||
        error.code() == WorkerProcessErrorCode::worker_error) {
        return {json_rpc::internal_error_code,
                "The DXC analysis worker returned an invalid result: " + std::string{error.what()}};
    }
    return {json_rpc::server_cancelled_code,
            "DXC analysis became unavailable; the worker was restarted: " +
                std::string{error.what()}};
}

} // namespace

struct Manager::Impl final {
    struct Entry final {
        std::string root_uri;
        std::string root_identity;
        std::string root_path;
        std::string cache_key;
        std::string configuration;
        std::vector<std::string> compiler_arguments;
        std::int64_t version{};
        // Parent-owned generation envelope. DXC and its translation unit stay
        // entirely in the child process.
        std::uint64_t generation{};
        std::unordered_set<std::string> dependencies;
        bool has_dynamic_includes{};
        std::size_t estimated_bytes{};
        std::uint64_t last_use{};
        bool resident{};
        bool recovery_attempted{};
        std::uint64_t epoch{};
        std::shared_ptr<AnalysisInput> recovery_input;
    };

    struct Recovery final {
        std::string root_identity;
        std::int64_t version{};
        std::uint64_t epoch{};
        std::shared_ptr<AnalysisInput> input;
    };

    struct WorkerState final {
        WorkerState(WorkerProcessOptions worker_options,
                    workspace::IncludeCacheLimits include_limits)
            : worker{std::move(worker_options)}, include_cache{include_limits} {}

        WorkerClient worker;
        workspace::IncludeMetadataCache include_cache;
        std::unordered_map<std::string, Entry> entries;
        std::uint64_t use_counter{};
        workspace::IncludeCacheMetrics last_include_metrics;
    };

    [[nodiscard]] static std::string worker_path(const Entry& entry, std::string requested_path) {
        try {
            if (workspace::DocumentUri::from_path(requested_path).identity() ==
                entry.root_identity) {
                return entry.root_path;
            }
        } catch (const workspace::DocumentError&) {
        }
        return requested_path;
    }

    Impl(DiagnosticsHandler diagnostics_handler, AnalysisOptions value,
         std::shared_ptr<AnalysisHooks> analysis_hooks, ErrorHandler error_handler,
         UnavailableHandler unavailable_handler)
        : diagnostics{std::move(diagnostics_handler)}, errors{std::move(error_handler)},
          unavailable{std::move(unavailable_handler)}, options{validate_options(std::move(value))},
          hooks{std::move(analysis_hooks)}, worker_states(options.scheduler.worker_count),
          scheduler{options.scheduler,
                    [this](std::size_t index) { worker_states[index].reset(); }} {
        if (!diagnostics) {
            throw std::invalid_argument{"Analysis manager requires a diagnostics handler"};
        }
        runtime_worker = std::make_shared<WorkerClient>(worker_process_options());
    }

    ~Impl() { shutdown(); }

    [[nodiscard]] WorkerProcessOptions worker_process_options() const {
        return {.executable = options.worker_executable,
                .dxc_runtime_directory = options.runtime.directory,
                .shutdown_timeout = std::chrono::milliseconds{500}};
    }

    [[nodiscard]] WorkerState& state(std::size_t index) {
        auto& state_value = worker_states[index];
        if (!state_value) {
            auto limits = options.limits.include_cache;
            limits.max_entries =
                worker_share(limits.max_entries, options.scheduler.worker_count, index);
            limits.max_estimated_bytes =
                worker_share(limits.max_estimated_bytes, options.scheduler.worker_count, index);
            state_value =
                std::make_unique<WorkerState>(worker_process_options(), std::move(limits));
        }
        return *state_value;
    }

    void update_include_metrics(WorkerState& state_value) {
        const auto latest = state_value.include_cache.metrics();
        include_hits.fetch_add(latest.hits - state_value.last_include_metrics.hits,
                               std::memory_order_relaxed);
        include_misses.fetch_add(latest.misses - state_value.last_include_metrics.misses,
                                 std::memory_order_relaxed);
        include_evictions.fetch_add(latest.evictions - state_value.last_include_metrics.evictions,
                                    std::memory_order_relaxed);
        if (latest.entries >= state_value.last_include_metrics.entries) {
            include_entries.fetch_add(latest.entries - state_value.last_include_metrics.entries,
                                      std::memory_order_relaxed);
        } else {
            include_entries.fetch_sub(state_value.last_include_metrics.entries - latest.entries,
                                      std::memory_order_relaxed);
        }
        if (latest.estimated_bytes >= state_value.last_include_metrics.estimated_bytes) {
            include_bytes.fetch_add(latest.estimated_bytes -
                                        state_value.last_include_metrics.estimated_bytes,
                                    std::memory_order_relaxed);
        } else {
            include_bytes.fetch_sub(state_value.last_include_metrics.estimated_bytes -
                                        latest.estimated_bytes,
                                    std::memory_order_relaxed);
        }
        state_value.last_include_metrics = latest;
    }

    void account_nonresident(Entry& entry) {
        if (!entry.resident) {
            return;
        }
        entry.resident = false;
        translation_unit_bytes.fetch_sub(entry.estimated_bytes, std::memory_order_relaxed);
        translation_units.fetch_sub(1, std::memory_order_relaxed);
    }

    void account_resident(Entry& entry) {
        if (entry.resident) {
            return;
        }
        entry.resident = true;
        translation_unit_bytes.fetch_add(entry.estimated_bytes, std::memory_order_relaxed);
        translation_units.fetch_add(1, std::memory_order_relaxed);
    }

    void remove_entry(WorkerState& state_value,
                      const std::unordered_map<std::string, Entry>::iterator& entry,
                      bool erase_worker_entry, const json_rpc::CancellationToken& cancellation) {
        if (erase_worker_entry && entry->second.resident) {
            state_value.worker.erase(entry->first, options.budgets.interactive_timeout,
                                     cancellation);
        }
        account_nonresident(entry->second);
        state_value.entries.erase(entry);
    }

    [[nodiscard]] std::vector<Recovery> invalidate_worker(WorkerState& state_value,
                                                          std::string_view excluded_root = {}) {
        state_value.worker.reset();
        std::vector<Recovery> recoveries;
        recoveries.reserve(state_value.entries.size());
        for (auto& [root, entry] : state_value.entries) {
            account_nonresident(entry);
            if (root == excluded_root || entry.recovery_attempted || !entry.recovery_input) {
                continue;
            }
            entry.recovery_attempted = true;
            recoveries.push_back(Recovery{.root_identity = root,
                                          .version = entry.version,
                                          .epoch = entry.epoch,
                                          .input = entry.recovery_input});
        }
        return recoveries;
    }

    void schedule_recoveries(std::vector<Recovery> recoveries) {
        if (stopped.load(std::memory_order_acquire)) {
            return;
        }
        for (auto& recovery : recoveries) {
            {
                std::scoped_lock lock{metadata_mutex};
                const auto current = root_epochs.find(recovery.root_identity);
                if (current == root_epochs.end() || current->second != recovery.epoch) {
                    continue;
                }
            }
            json_rpc::CancellationToken cancellation;
            const auto scheduling_version =
                recovery.version == (std::numeric_limits<std::int64_t>::min)()
                    ? recovery.version
                    : recovery.version - 1;
            const auto submitted = scheduler.submit(
                recovery.root_identity, scheduling_version, WorkPriority::background, cancellation,
                [this, input = std::move(recovery.input), epoch = recovery.epoch](
                    std::size_t worker, const json_rpc::CancellationToken& token) {
                    analyze(*input, epoch, worker, token, true);
                },
                {}, false);
            if (!submitted && errors) {
                errors("Analysis recovery could not be queued");
            }
        }
    }

    void enforce_limits(WorkerState& state_value, std::size_t worker, std::string_view current_root,
                        const json_rpc::CancellationToken& cancellation) {
        const auto max_entries = worker_share(options.limits.max_translation_units,
                                              options.scheduler.worker_count, worker);
        const auto max_bytes = worker_share(options.limits.max_translation_unit_estimated_bytes,
                                            options.scheduler.worker_count, worker);
        auto state_bytes = [&state_value] {
            std::size_t total{};
            for (const auto& [root, entry] : state_value.entries) {
                static_cast<void>(root);
                total += entry.estimated_bytes;
            }
            return total;
        };
        while (state_value.entries.size() > max_entries || state_bytes() > max_bytes) {
            auto victim = state_value.entries.end();
            for (auto candidate = state_value.entries.begin();
                 candidate != state_value.entries.end(); ++candidate) {
                if (candidate->first == current_root && state_value.entries.size() > 1) {
                    continue;
                }
                if (victim == state_value.entries.end() ||
                    std::pair{candidate->second.last_use, candidate->first} <
                        std::pair{victim->second.last_use, victim->first}) {
                    victim = candidate;
                }
            }
            if (victim == state_value.entries.end()) {
                break;
            }
            remove_entry(state_value, victim, true, cancellation);
            cache_evictions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool is_current(std::string_view root_identity, std::uint64_t epoch,
                                  const json_rpc::CancellationToken& cancellation) const {
        if (cancellation.is_cancellation_requested()) {
            return false;
        }
        std::scoped_lock lock{metadata_mutex};
        const auto current = root_epochs.find(std::string{root_identity});
        return current != root_epochs.end() && current->second == epoch &&
               !cancellation.is_cancellation_requested();
    }

    void update_metadata(std::string_view root_identity, std::uint64_t epoch,
                         const RootMetadata& value,
                         const json_rpc::CancellationToken& cancellation) {
        std::scoped_lock lock{metadata_mutex};
        const std::string identity{root_identity};
        const auto current = root_epochs.find(identity);
        if (current != root_epochs.end() && current->second == epoch &&
            !cancellation.is_cancellation_requested()) {
            metadata.insert_or_assign(identity, value);
        }
    }

    void publish_unavailable(const AnalysisInput& input, std::uint64_t epoch,
                             const WorkerProcessError& error,
                             const json_rpc::CancellationToken& cancellation) {
        if (!unavailable || error.code() == WorkerProcessErrorCode::cancelled ||
            !is_current(input.root.document_uri().identity(), epoch, cancellation)) {
            return;
        }
        {
            std::scoped_lock lock{metadata_mutex};
            const auto root_identity = input.root.document_uri().identity();
            const auto previous = unavailable_generations.find(root_identity);
            if (previous != unavailable_generations.end() && previous->second == input.generation) {
                return;
            }
            unavailable_generations[root_identity] = input.generation;
        }
        try {
            unavailable(input.root, unavailable_result(error), input.generation);
        } catch (const std::exception& callback_error) {
            if (errors) {
                errors(callback_error.what());
            }
        }
    }

    void clear_unavailable(std::string_view root_identity) {
        std::scoped_lock lock{metadata_mutex};
        unavailable_generations.erase(std::string{root_identity});
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void analyze(AnalysisInput input, std::uint64_t epoch, std::size_t worker,
                 const json_rpc::CancellationToken& cancellation, bool recovery = false) {
        WorkerState* worker_state = nullptr;
        std::optional<Entry> candidate;
        try {
            if (hooks && hooks->before_analysis) {
                hooks->before_analysis(input.root.document_uri().identity(), input.root.version());
            }
            cancellation.throw_if_cancellation_requested();

            worker_state = &state(worker);
            const auto root_identity = input.root.document_uri().identity();
            if (!recovery) {
                if (const auto current = worker_state->entries.find(root_identity);
                    current != worker_state->entries.end()) {
                    current->second.recovery_attempted = false;
                }
            }

            auto resolution =
                workspace::resolve_includes(input.root, input.open_documents, input.configuration,
                                            &worker_state->include_cache);
            update_include_metrics(*worker_state);
            cancellation.throw_if_cancellation_requested();
            auto recovery_input = make_recovery_input(input, resolution);

            const auto configuration = Manager::configuration_fingerprint(input.configuration);
            auto cache_key = source_fingerprint(resolution, configuration, input.root.version());
            const auto root_uri = input.root.uri();
            const auto root_path = resolution.sources.front().path;
            const auto compiler_options = input.configuration.compiler_options();
            const auto compiler_arguments = compiler_options.arguments();
            const auto source_bytes = estimate_sources(resolution);
            std::size_t argument_bytes = compiler_arguments.capacity() * sizeof(std::string);
            for (const auto& argument : compiler_arguments) {
                argument_bytes += argument.capacity();
            }
            const auto estimate = options.limits.opaque_translation_unit_estimate + sizeof(Entry) +
                                  source_bytes + estimate_recovery_input(*recovery_input) +
                                  cache_key.capacity() + root_uri.capacity() +
                                  root_identity.capacity() + root_path.capacity() +
                                  configuration.capacity() + argument_bytes +
                                  resolution.dependency_identities.size() * (2U * sizeof(void*));

            auto existing = worker_state->entries.find(root_identity);
            const auto previous_generation = existing == worker_state->entries.end()
                                                 ? std::uint64_t{}
                                                 : existing->second.generation;
            const auto expected_kind =
                existing != worker_state->entries.end() && existing->second.resident &&
                        existing->second.cache_key == cache_key
                    ? WorkerAnalysisKind::cache_hit
                : existing != worker_state->entries.end() && existing->second.resident &&
                        existing->second.root_path == root_path &&
                        existing->second.compiler_arguments == compiler_arguments
                    ? WorkerAnalysisKind::reparsed
                    : WorkerAnalysisKind::parsed;

            if (expected_kind == WorkerAnalysisKind::cache_hit) {
                cache_hits.fetch_add(1, std::memory_order_relaxed);
            } else {
                cache_misses.fetch_add(1, std::memory_order_relaxed);
            }

            candidate = Entry{.root_uri = root_uri,
                              .root_identity = root_identity,
                              .root_path = root_path,
                              .cache_key = cache_key,
                              .configuration = configuration,
                              .compiler_arguments = compiler_arguments,
                              .version = input.root.version(),
                              .generation = previous_generation,
                              .dependencies = resolution.dependency_identities,
                              .has_dynamic_includes = resolution.has_dynamic_includes,
                              .estimated_bytes = estimate,
                              .last_use = ++worker_state->use_counter,
                              .resident = false,
                              .recovery_attempted = recovery,
                              .epoch = epoch,
                              .recovery_input = std::move(recovery_input)};

            update_metadata(root_identity, epoch,
                            RootMetadata{.root_uri = root_uri,
                                         .root_identity = root_identity,
                                         .version = input.root.version(),
                                         .configuration_fingerprint = configuration,
                                         .dependency_identities = resolution.dependency_identities,
                                         .has_dynamic_includes = resolution.has_dynamic_includes},
                            cancellation);

            const auto started = Clock::now();
            auto worker_sources = expected_kind == WorkerAnalysisKind::cache_hit
                                      ? std::vector<dxc::SourceFile>{}
                                      : std::move(resolution.sources);
            auto analysis_result = worker_state->worker.analyze(
                root_identity, wire_fingerprint(cache_key), root_path, std::move(worker_sources),
                compiler_options, options.budgets.background_timeout, cancellation);
            if (analysis_result.kind != expected_kind) {
                worker_state->worker.reset();
                throw WorkerProcessError{
                    WorkerProcessErrorCode::malformed_reply,
                    "Analysis worker cache transition did not match parent state"};
            }

            if (analysis_result.kind == WorkerAnalysisKind::parsed) {
                parse_microseconds.fetch_add(elapsed_microseconds(started),
                                             std::memory_order_relaxed);
                parse_count.fetch_add(1, std::memory_order_relaxed);
                candidate->generation =
                    generation_counter.fetch_add(1, std::memory_order_relaxed) + 1;
            } else if (analysis_result.kind == WorkerAnalysisKind::reparsed) {
                reparse_microseconds.fetch_add(elapsed_microseconds(started),
                                               std::memory_order_relaxed);
                reparse_count.fetch_add(1, std::memory_order_relaxed);
                candidate->generation =
                    generation_counter.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            existing = worker_state->entries.find(root_identity);
            if (existing != worker_state->entries.end()) {
                account_nonresident(existing->second);
            }
            candidate->recovery_attempted = false;
            auto [stored, inserted] =
                worker_state->entries.insert_or_assign(root_identity, std::move(*candidate));
            static_cast<void>(inserted);
            candidate.reset();
            account_resident(stored->second);

            const auto dependencies = stored->second.dependencies;
            const auto has_dynamic_includes = stored->second.has_dynamic_includes;
            auto diagnostics_result = std::move(analysis_result.diagnostics);
            enforce_limits(*worker_state, worker, root_identity, cancellation);
            cancellation.throw_if_cancellation_requested();
            {
                std::scoped_lock lock{metadata_mutex};
                if (root_epochs[root_identity] != epoch ||
                    cancellation.is_cancellation_requested()) {
                    return;
                }
                metadata.insert_or_assign(
                    root_identity, RootMetadata{.root_uri = root_uri,
                                                .root_identity = root_identity,
                                                .version = input.root.version(),
                                                .configuration_fingerprint = configuration,
                                                .dependency_identities = dependencies,
                                                .has_dynamic_includes = has_dynamic_includes});
            }
            cancellation.throw_if_cancellation_requested();
            if (hooks && hooks->after_diagnostics) {
                hooks->after_diagnostics(diagnostics_result);
            }
            clear_unavailable(root_identity);
            diagnostics(input.root, diagnostics_result, input.generation);
        } catch (const WorkerProcessError& error) {
            std::vector<Recovery> recoveries;
            if (worker_state != nullptr) {
                if (candidate) {
                    candidate->recovery_attempted =
                        recovery || !automatically_recoverable(error.code());
                }
                const auto current_identity = input.root.document_uri().identity();
                recoveries = invalidate_worker(*worker_state,
                                               candidate ? current_identity : std::string_view{});
                if (candidate) {
                    worker_state->entries.insert_or_assign(candidate->root_identity,
                                                           std::move(*candidate));
                    candidate.reset();
                    if (error.code() != WorkerProcessErrorCode::cancelled) {
                        auto& current = worker_state->entries.at(current_identity);
                        if (!current.recovery_attempted && current.recovery_input) {
                            current.recovery_attempted = true;
                            recoveries.push_back(Recovery{.root_identity = current.root_identity,
                                                          .version = current.version,
                                                          .epoch = current.epoch,
                                                          .input = current.recovery_input});
                        }
                    }
                }
                enforce_limits(*worker_state, worker, current_identity, cancellation);
                std::erase_if(recoveries, [&state = *worker_state](const Recovery& recovery_value) {
                    const auto entry = state.entries.find(recovery_value.root_identity);
                    return entry == state.entries.end() ||
                           entry->second.epoch != recovery_value.epoch;
                });
            }
            if (error.code() == WorkerProcessErrorCode::cancelled ||
                cancellation.is_cancellation_requested()) {
                schedule_recoveries(std::move(recoveries));
                return;
            }
            publish_unavailable(input, epoch, error, cancellation);
            if (errors) {
                errors(error.what());
            }
            schedule_recoveries(std::move(recoveries));
        } catch (const json_rpc::HandlerError&) {
            return;
        } catch (const std::exception& error) {
            if (errors) {
                errors(error.what());
            }
        }
    }

    void throw_if_query_cancelled(const json_rpc::CancellationToken& external,
                                  const json_rpc::CancellationToken& internal) const {
        if (!internal.is_cancellation_requested()) {
            return;
        }
        if (external.is_cancellation_requested()) {
            throw json_rpc::HandlerError{json_rpc::request_cancelled_code, "Request cancelled"};
        }
        throw json_rpc::HandlerError{
            stopped.load(std::memory_order_acquire) ? json_rpc::server_cancelled_code
                                                    : json_rpc::content_modified_code,
            stopped.load(std::memory_order_relaxed) ? "Analysis server is shutting down"
                                                    : "Analysis was superseded"};
    }

    template <typename Result, typename Operation>
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    [[nodiscard]] Result query(std::string root_identity, std::int64_t version,
                               const json_rpc::CancellationToken& cancellation,
                               Operation operation) {
        auto result = std::make_shared<AsyncResult<Result>>();
        auto future = result->future();
        json_rpc::CancellationToken work_cancellation;
        struct DeadlineState final {
            std::mutex mutex;
            std::condition_variable changed;
            bool completed{};
        };
        auto deadline_state = std::make_shared<DeadlineState>();
        auto deadline_expired = std::make_shared<std::atomic_bool>(false);
        auto externally_cancelled = std::make_shared<std::atomic_bool>(false);
        std::jthread deadline_timer{[state = deadline_state, deadline_expired, result,
                                     work_cancellation,
                                     timeout = options.budgets.interactive_timeout] {
            std::unique_lock lock{state->mutex};
            if (state->changed.wait_for(lock, timeout, [&state] { return state->completed; })) {
                return;
            }
            deadline_expired->store(true, std::memory_order_release);
            result->set_exception(std::make_exception_ptr(json_rpc::HandlerError{
                json_rpc::server_cancelled_code, "DXC analysis request timed out"}));
            work_cancellation.cancel();
        }};
        cancellation.on_cancel([result, work_cancellation, externally_cancelled] {
            externally_cancelled->store(true, std::memory_order_release);
            result->set_exception(std::make_exception_ptr(
                json_rpc::HandlerError{json_rpc::request_cancelled_code, "Request cancelled"}));
            work_cancellation.cancel();
        });
        work_cancellation.on_cancel([this, result, externally_cancelled, deadline_expired] {
            if (externally_cancelled->load(std::memory_order_acquire) ||
                deadline_expired->load(std::memory_order_acquire)) {
                return;
            }
            const auto shutting_down = stopped.load(std::memory_order_acquire);
            result->set_exception(std::make_exception_ptr(json_rpc::HandlerError{
                shutting_down ? json_rpc::server_cancelled_code : json_rpc::content_modified_code,
                shutting_down ? "Analysis server is shutting down" : "Analysis was superseded"}));
        });
        const auto submitted = scheduler.submit(
            root_identity, version, WorkPriority::interactive, work_cancellation,
            [this, result, root_identity, version, operation = std::move(operation), cancellation,
             deadline_expired](std::size_t worker,
                               const json_rpc::CancellationToken& token) mutable {
                try {
                    throw_if_query_cancelled(cancellation, token);
                    if (hooks && hooks->before_interactive) {
                        hooks->before_interactive(root_identity);
                    }
                    throw_if_query_cancelled(cancellation, token);
                    auto& worker_state = state(worker);
                    const auto entry = worker_state.entries.find(root_identity);
                    if (entry == worker_state.entries.end() || entry->second.version != version) {
                        throw json_rpc::HandlerError{json_rpc::content_modified_code,
                                                     "Analysis was superseded"};
                    }
                    if (!entry->second.resident) {
                        throw json_rpc::HandlerError{
                            json_rpc::server_cancelled_code,
                            "DXC analysis is unavailable while its worker is reconstructed"};
                    }
                    entry->second.last_use = ++worker_state.use_counter;
                    try {
                        auto value = operation(worker_state.worker, entry->second, token);
                        throw_if_query_cancelled(cancellation, token);
                        result->set_value(std::move(value));
                    } catch (const WorkerProcessError& error) {
                        const auto superseded = error.code() == WorkerProcessErrorCode::cancelled &&
                                                !cancellation.is_cancellation_requested() &&
                                                !deadline_expired->load(std::memory_order_acquire);
                        auto recoveries =
                            invalidate_worker(worker_state, superseded ? root_identity : "");
                        schedule_recoveries(std::move(recoveries));
                        throw interactive_error(error, cancellation.is_cancellation_requested(),
                                                stopped.load(std::memory_order_acquire));
                    }
                } catch (...) {
                    result->set_exception(std::current_exception());
                }
            });
        if (!submitted) {
            result->set_exception(std::make_exception_ptr(
                json_rpc::HandlerError{json_rpc::server_cancelled_code, "Analysis queue full"}));
        }
        try {
            auto value = future.get();
            {
                std::scoped_lock lock{deadline_state->mutex};
                deadline_state->completed = true;
            }
            deadline_state->changed.notify_all();
            return value;
        } catch (...) {
            {
                std::scoped_lock lock{deadline_state->mutex};
                deadline_state->completed = true;
            }
            deadline_state->changed.notify_all();
            throw;
        }
    }

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    void erase(std::string root_identity) {
        {
            std::scoped_lock lock{metadata_mutex};
            ++root_epochs[root_identity];
            metadata.erase(root_identity);
            unavailable_generations.erase(root_identity);
        }
        scheduler.cancel_root(root_identity);
        json_rpc::CancellationToken cancellation;
        auto erased_root = root_identity;
        static_cast<void>(scheduler.submit(
            root_identity, std::numeric_limits<std::int64_t>::max(), WorkPriority::interactive,
            cancellation,
            [this, root_identity = std::move(erased_root)](
                std::size_t worker, const json_rpc::CancellationToken& token) {
                if (!worker_states[worker]) {
                    return;
                }
                auto& state_value = *worker_states[worker];
                auto entry = state_value.entries.find(root_identity);
                if (entry == state_value.entries.end()) {
                    return;
                }
                try {
                    if (entry->second.resident) {
                        state_value.worker.erase(root_identity, options.budgets.interactive_timeout,
                                                 token);
                    }
                    account_nonresident(entry->second);
                    state_value.entries.erase(entry);
                } catch (const WorkerProcessError& error) {
                    auto recoveries = invalidate_worker(state_value, root_identity);
                    state_value.entries.erase(root_identity);
                    schedule_recoveries(std::move(recoveries));
                    if (error.code() != WorkerProcessErrorCode::cancelled && errors) {
                        errors(error.what());
                    }
                }
            }));
    }

    void shutdown() {
        if (!stopped.exchange(true, std::memory_order_acq_rel)) {
            runtime_worker->shutdown();
            scheduler.shutdown();
        }
    }

    [[nodiscard]] dxc::RuntimeInfo runtime_info() const {
        std::scoped_lock lock{runtime_info_mutex};
        if (cached_runtime_info) {
            return *cached_runtime_info;
        }
        json_rpc::CancellationToken cancellation;
        auto info = runtime_worker->runtime_info(options.budgets.interactive_timeout, cancellation);
        cached_runtime_info = std::move(info);
        runtime_worker->shutdown();
        return *cached_runtime_info;
    }

    DiagnosticsHandler diagnostics;
    ErrorHandler errors;
    UnavailableHandler unavailable;
    AnalysisOptions options;
    std::shared_ptr<AnalysisHooks> hooks;
    std::vector<std::unique_ptr<WorkerState>> worker_states;
    mutable std::shared_ptr<WorkerClient> runtime_worker;
    mutable std::mutex runtime_info_mutex;
    mutable std::optional<dxc::RuntimeInfo> cached_runtime_info;
    mutable std::mutex metadata_mutex;
    std::unordered_map<std::string, RootMetadata> metadata;
    std::unordered_map<std::string, std::uint64_t> root_epochs;
    std::unordered_map<std::string, std::uint64_t> unavailable_generations;
    std::atomic<std::uint64_t> generation_counter{};
    std::atomic_bool stopped{};
    std::atomic<std::uint64_t> parse_count{};
    std::atomic<std::uint64_t> reparse_count{};
    std::atomic<std::uint64_t> cache_hits{};
    std::atomic<std::uint64_t> cache_misses{};
    std::atomic<std::uint64_t> cache_evictions{};
    std::atomic<std::uint64_t> completion_count{};
    std::atomic<std::uint64_t> parse_microseconds{};
    std::atomic<std::uint64_t> reparse_microseconds{};
    std::atomic<std::uint64_t> completion_microseconds{};
    std::atomic<std::size_t> translation_units{};
    std::atomic<std::size_t> translation_unit_bytes{};
    std::atomic<std::uint64_t> include_hits{};
    std::atomic<std::uint64_t> include_misses{};
    std::atomic<std::uint64_t> include_evictions{};
    std::atomic<std::size_t> include_entries{};
    std::atomic<std::size_t> include_bytes{};
    Scheduler scheduler;
};

Manager::Manager(DiagnosticsHandler diagnostics, AnalysisOptions options,
                 std::shared_ptr<AnalysisHooks> hooks, ErrorHandler errors,
                 UnavailableHandler unavailable)
    : implementation_{std::make_unique<Impl>(std::move(diagnostics), std::move(options),
                                             std::move(hooks), std::move(errors),
                                             std::move(unavailable))} {}

Manager::~Manager() = default;

bool Manager::analyze(AnalysisInput input) {
    const auto root = input.root.document_uri().identity();
    const auto version = input.root.version();
    const auto root_uri = input.root.uri();
    const auto configuration = configuration_fingerprint(input.configuration);
    const auto epoch = std::make_shared<std::uint64_t>();
    json_rpc::CancellationToken cancellation;
    const auto submitted = implementation_->scheduler.submit(
        root, version, WorkPriority::background, cancellation,
        [implementation = implementation_.get(), input = std::move(input),
         epoch](std::size_t worker, const json_rpc::CancellationToken& token) mutable {
            implementation->analyze(std::move(input), *epoch, worker, token);
        },
        [implementation = implementation_.get(), root, root_uri, version, configuration, epoch] {
            std::scoped_lock lock{implementation->metadata_mutex};
            *epoch = ++implementation->root_epochs[root];
            implementation->metadata.insert_or_assign(
                root, RootMetadata{.root_uri = root_uri,
                                   .root_identity = root,
                                   .version = version,
                                   .configuration_fingerprint = configuration,
                                   .dependency_identities = {},
                                   .has_dynamic_includes = true});
        });
    if (!submitted && implementation_->errors &&
        !implementation_->stopped.load(std::memory_order_acquire)) {
        implementation_->errors("Analysis queue full; current document analysis was not queued");
    }
    return submitted;
}

void Manager::after_roots_idle(std::vector<std::string> roots, std::function<void()> callback) {
    if (!callback) {
        throw std::invalid_argument{"Analysis completion callback is required"};
    }
    std::ranges::sort(roots);
    const auto unique_end = std::ranges::unique(roots).begin();
    roots.erase(unique_end, roots.end());
    if (roots.empty()) {
        callback();
        return;
    }

    struct CompletionState {
        std::atomic_size_t remaining;
        std::function<void()> callback;
    };
    auto completion = std::make_shared<CompletionState>(roots.size(), std::move(callback));
    const auto complete_one = [completion] {
        if (completion->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            completion->callback();
        }
    };
    struct CompletionTicket {
        std::function<void()> complete;
        ErrorHandler report_error;
        ~CompletionTicket() {
            try {
                complete();
            } catch (const std::exception& error) {
                if (report_error) {
                    report_error(error.what());
                }
            } catch (...) {
                if (report_error) {
                    report_error("Analysis completion callback failed");
                }
            }
        }
    };
    for (auto& root : roots) {
        json_rpc::CancellationToken cancellation;
        auto ticket = std::make_shared<CompletionTicket>(complete_one, implementation_->errors);
        static_cast<void>(implementation_->scheduler.submit(
            std::move(root), std::numeric_limits<std::int64_t>::max(), WorkPriority::barrier,
            cancellation,
            [ticket = std::move(ticket)](std::size_t, const json_rpc::CancellationToken&) mutable {
                ticket.reset();
            }));
    }
}

void Manager::erase(std::string_view root_identity) {
    implementation_->erase(std::string{root_identity});
}

void Manager::invalidate_include_metadata(const std::unordered_set<std::string>& identities) {
    for (std::size_t worker = 0; worker < implementation_->options.scheduler.worker_count;
         ++worker) {
        std::string owner_key = "$include-cache-" + std::to_string(worker);
        while (implementation_->scheduler.owner_for(owner_key) != worker) {
            owner_key.push_back('-');
        }
        json_rpc::CancellationToken cancellation;
        static_cast<void>(implementation_->scheduler.submit(
            std::move(owner_key), 0, WorkPriority::interactive, cancellation,
            [implementation = implementation_.get(),
             identities](std::size_t index, const json_rpc::CancellationToken&) {
                if (!implementation->worker_states[index]) {
                    return;
                }
                auto& state = *implementation->worker_states[index];
                for (const auto& identity : identities) {
                    state.include_cache.invalidate(identity);
                }
                implementation->update_include_metrics(state);
            }));
    }
}

void Manager::wait_idle() { implementation_->scheduler.wait_idle(); }

void Manager::shutdown() { implementation_->shutdown(); }

std::vector<dxc::Completion> Manager::complete(std::string root_identity, std::int64_t version,
                                               std::string path, std::uint32_t line,
                                               std::uint32_t column,
                                               const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Completion>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            const auto start = Clock::now();
            auto result =
                worker.complete(entry.root_identity, entry.root_path, line, column,
                                implementation->options.budgets.interactive_timeout, token);
            implementation->completion_microseconds.fetch_add(elapsed_microseconds(start),
                                                              std::memory_order_relaxed);
            implementation->completion_count.fetch_add(1, std::memory_order_relaxed);
            return result;
        });
}

std::optional<dxc::Definition>
Manager::definition(std::string root_identity, std::int64_t version, std::string path,
                    std::uint32_t line, std::uint32_t column,
                    const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<dxc::Definition>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.definition(entry.root_identity, entry.root_path, line, column,
                                     implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::Reference> Manager::references(std::string root_identity, std::int64_t version,
                                                std::string path, std::uint32_t line,
                                                std::uint32_t column,
                                                const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Reference>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            return worker.references(entry.root_identity, Impl::worker_path(entry, requested_path),
                                     line, column,
                                     implementation->options.budgets.interactive_timeout, token);
        });
}

std::optional<dxc::Hover> Manager::hover(std::string root_identity, std::int64_t version,
                                         std::string path, std::uint32_t line, std::uint32_t column,
                                         const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<dxc::Hover>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.hover(entry.root_identity, entry.root_path, line, column,
                                implementation->options.budgets.interactive_timeout, token);
        });
}

std::optional<dxc::MemoryLayout>
Manager::memory_layout(std::string root_identity, std::int64_t version, std::string path,
                       std::uint32_t line, std::uint32_t column,
                       const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<dxc::MemoryLayout>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.memory_layout(entry.root_identity, entry.root_path, line, column,
                                        implementation->options.budgets.interactive_timeout, token);
        });
}

WithGeneration<std::optional<dxc::MacroExpansion>>
Manager::macro_expansion(std::string root_identity, std::int64_t version, std::string path,
                         std::uint32_t line, std::uint32_t column,
                         const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<std::optional<dxc::MacroExpansion>>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token)
            -> WithGeneration<std::optional<dxc::MacroExpansion>> {
            return {.value = worker.macro_expansion(
                        entry.root_identity, Impl::worker_path(entry, requested_path), line, column,
                        implementation->options.budgets.interactive_timeout, token),
                    .generation = entry.generation};
        });
}

WithGeneration<std::optional<std::string>>
Manager::macro_name(std::string root_identity, std::int64_t version, std::string path,
                    std::uint32_t line, std::uint32_t column,
                    const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<std::optional<std::string>>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token)
            -> WithGeneration<std::optional<std::string>> {
            return {.value = worker.macro_name(
                        entry.root_identity, Impl::worker_path(entry, requested_path), line, column,
                        implementation->options.budgets.interactive_timeout, token),
                    .generation = entry.generation};
        });
}

dxc::CompilationInfo Manager::compilation_info(std::string root_identity, std::int64_t version,
                                               std::string path,
                                               const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<dxc::CompilationInfo>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path)](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.compilation_info(
                entry.root_identity, implementation->options.budgets.interactive_timeout, token);
        });
}

WithGeneration<dxc::CompilationInfo>
Manager::compilation_info_with_generation(std::string root_identity, std::int64_t version,
                                          std::string path,
                                          const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<dxc::CompilationInfo>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path)](
            WorkerClient& worker, Impl::Entry& entry,
            const json_rpc::CancellationToken& token) -> WithGeneration<dxc::CompilationInfo> {
            static_cast<void>(requested_path);
            return {.value = worker.compilation_info(
                        entry.root_identity, implementation->options.budgets.interactive_timeout,
                        token),
                    .generation = entry.generation};
        });
}

std::vector<dxc::Signature> Manager::signatures(std::string root_identity, std::int64_t version,
                                                std::string path, std::uint32_t line,
                                                std::uint32_t column,
                                                const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Signature>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.signatures(entry.root_identity, entry.root_path, line, column,
                                     implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::InlayHint> Manager::inlay_hints(std::string root_identity, std::int64_t version,
                                                 std::string path,
                                                 std::vector<dxc::SourceOffsetRange> ranges,
                                                 std::vector<dxc::InlayCall> calls,
                                                 dxc::InlayHintOptions options,
                                                 const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::InlayHint>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path),
         ranges = std::move(ranges), calls = std::move(calls),
         options](WorkerClient& worker, Impl::Entry& entry,
                  const json_rpc::CancellationToken& token) mutable {
            static_cast<void>(requested_path);
            return worker.inlay_hints(entry.root_identity, entry.root_path, std::move(ranges),
                                      std::move(calls), options,
                                      implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::Token> Manager::tokens(std::string root_identity, std::int64_t version,
                                        std::string path,
                                        const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Token>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path)](
            WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token) {
            static_cast<void>(requested_path);
            return worker.tokens(entry.root_identity, entry.root_path,
                                 implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::SourceRange>
Manager::skipped_ranges(std::string root_identity, std::int64_t version,
                        const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::SourceRange>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get()](WorkerClient& worker, Impl::Entry& entry,
                                                 const json_rpc::CancellationToken& token) {
            return worker.skipped_ranges(
                entry.root_identity, implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::MacroDefinition>
Manager::macro_definitions(std::string root_identity, std::int64_t version,
                           const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::MacroDefinition>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get()](WorkerClient& worker, Impl::Entry& entry,
                                                 const json_rpc::CancellationToken& token) {
            return worker.macro_definitions(
                entry.root_identity, implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::Symbol> Manager::symbols(std::string root_identity, std::int64_t version,
                                          const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Symbol>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get()](WorkerClient& worker, Impl::Entry& entry,
                                                 const json_rpc::CancellationToken& token) {
            return worker.symbols(entry.root_identity,
                                  implementation->options.budgets.interactive_timeout, token);
        });
}

std::vector<dxc::Symbol> Manager::document_symbols(std::string root_identity, std::int64_t version,
                                                   const json_rpc::CancellationToken& cancellation,
                                                   bool& truncated) {
    constexpr std::size_t max_document_symbols = 1024;
    auto result = implementation_->query<WorkerDocumentSymbolsResult>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get()](WorkerClient& worker, Impl::Entry& entry,
                                                 const json_rpc::CancellationToken& token) {
            return worker.document_symbols(
                entry.root_identity, entry.root_path, max_document_symbols,
                implementation->options.budgets.interactive_timeout, token);
        });
    truncated = result.truncated;
    return std::move(result.symbols);
}

WithGeneration<std::optional<dxc::CallableSymbol>>
Manager::callable_at(std::string root_identity, std::int64_t version, std::string path,
                     std::uint32_t line, std::uint32_t column,
                     const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<std::optional<dxc::CallableSymbol>>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token)
            -> WithGeneration<std::optional<dxc::CallableSymbol>> {
            static_cast<void>(requested_path);
            return {.value = worker.callable_at(entry.root_identity, entry.root_path, line, column,
                                                implementation->options.budgets.interactive_timeout,
                                                token),
                    .generation = entry.generation};
        });
}

WithGeneration<std::vector<dxc::OutgoingCall>>
Manager::outgoing_calls(std::string root_identity, std::int64_t version, std::string path,
                        std::uint32_t line, std::uint32_t column,
                        const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<std::vector<dxc::OutgoingCall>>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token)
            -> WithGeneration<std::vector<dxc::OutgoingCall>> {
            return {.value = worker.outgoing_calls(
                        entry.root_identity, Impl::worker_path(entry, requested_path), line, column,
                        implementation->options.budgets.interactive_timeout, token),
                    .generation = entry.generation};
        });
}

WithGeneration<std::vector<dxc::IncomingCall>>
Manager::incoming_calls(std::string root_identity, std::int64_t version, std::string path,
                        std::uint32_t line, std::uint32_t column,
                        const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<std::vector<dxc::IncomingCall>>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](WorkerClient& worker, Impl::Entry& entry, const json_rpc::CancellationToken& token)
            -> WithGeneration<std::vector<dxc::IncomingCall>> {
            return {.value = worker.incoming_calls(
                        entry.root_identity, Impl::worker_path(entry, requested_path), line, column,
                        implementation->options.budgets.interactive_timeout, token),
                    .generation = entry.generation};
        });
}

WithGeneration<dxc::EntryPointDataFlow>
Manager::entry_point_data_flow(std::string root_identity, std::int64_t version,
                               dxc::EntryPointDataFlowLimits limits,
                               const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<WithGeneration<dxc::EntryPointDataFlow>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), limits](
            WorkerClient& worker, Impl::Entry& entry,
            const json_rpc::CancellationToken& token) -> WithGeneration<dxc::EntryPointDataFlow> {
            return {.value = worker.entry_point_data_flow(
                        entry.root_identity, limits,
                        implementation->options.budgets.interactive_timeout, token),
                    .generation = entry.generation};
        });
}

std::uint64_t Manager::content_generation(std::string root_identity, std::int64_t version,
                                          const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::uint64_t>(
        std::move(root_identity), version, cancellation,
        [](WorkerClient&, Impl::Entry& entry, const json_rpc::CancellationToken&) {
            return entry.generation;
        });
}

std::optional<std::uint64_t> Manager::verify_call_hierarchy_identity(
    std::string root_identity, std::int64_t version, std::string path, std::uint32_t line,
    std::uint32_t column, std::uint32_t expected_start_offset, std::uint32_t expected_cursor_kind,
    std::string expected_name, const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<std::uint64_t>>(
        std::move(root_identity), version, cancellation,
        [implementation = implementation_.get(), requested_path = std::move(path), line, column,
         expected_start_offset, expected_cursor_kind, expected_name = std::move(expected_name)](
            WorkerClient& worker, Impl::Entry& entry,
            const json_rpc::CancellationToken& token) -> std::optional<std::uint64_t> {
            if (!worker.verify_call_hierarchy_identity(
                    entry.root_identity, Impl::worker_path(entry, requested_path), line, column,
                    expected_start_offset, expected_cursor_kind, expected_name,
                    implementation->options.budgets.interactive_timeout, token)) {
                return std::nullopt;
            }
            return entry.generation;
        });
}

std::vector<RootMetadata> Manager::roots() const {
    std::scoped_lock lock{implementation_->metadata_mutex};
    std::vector<RootMetadata> result;
    result.reserve(implementation_->metadata.size());
    for (const auto& [identity, metadata] : implementation_->metadata) {
        static_cast<void>(identity);
        result.push_back(metadata);
    }
    std::ranges::sort(result, {}, &RootMetadata::root_identity);
    return result;
}

std::vector<std::string>
Manager::dependent_root_uris(const std::unordered_set<std::string>& changed_identities,
                             std::string_view except_root) const {
    std::scoped_lock lock{implementation_->metadata_mutex};
    std::vector<std::pair<std::string, std::string>> affected;
    for (const auto& [identity, metadata] : implementation_->metadata) {
        if (identity == except_root) {
            continue;
        }
        const auto dependent =
            metadata.has_dynamic_includes ||
            std::ranges::any_of(changed_identities, [&metadata](const auto& changed) {
                return metadata.dependency_identities.contains(changed);
            });
        if (dependent) {
            affected.emplace_back(identity, metadata.root_uri);
        }
    }
    std::ranges::sort(affected);
    std::vector<std::string> result;
    result.reserve(affected.size());
    std::ranges::transform(affected, std::back_inserter(result),
                           [](const auto& item) { return item.second; });
    return result;
}

AnalysisMetrics Manager::metrics() const noexcept {
    return {.scheduler = implementation_->scheduler.metrics(),
            .parse_count = implementation_->parse_count.load(std::memory_order_relaxed),
            .reparse_count = implementation_->reparse_count.load(std::memory_order_relaxed),
            .cache_hits = implementation_->cache_hits.load(std::memory_order_relaxed),
            .cache_misses = implementation_->cache_misses.load(std::memory_order_relaxed),
            .cache_evictions = implementation_->cache_evictions.load(std::memory_order_relaxed),
            .completion_count = implementation_->completion_count.load(std::memory_order_relaxed),
            .parse_microseconds =
                implementation_->parse_microseconds.load(std::memory_order_relaxed),
            .reparse_microseconds =
                implementation_->reparse_microseconds.load(std::memory_order_relaxed),
            .completion_microseconds =
                implementation_->completion_microseconds.load(std::memory_order_relaxed),
            .translation_units = implementation_->translation_units.load(std::memory_order_relaxed),
            .translation_unit_estimated_bytes =
                implementation_->translation_unit_bytes.load(std::memory_order_relaxed),
            .include_cache = {
                .hits = implementation_->include_hits.load(std::memory_order_relaxed),
                .misses = implementation_->include_misses.load(std::memory_order_relaxed),
                .evictions = implementation_->include_evictions.load(std::memory_order_relaxed),
                .entries = implementation_->include_entries.load(std::memory_order_relaxed),
                .estimated_bytes = implementation_->include_bytes.load(std::memory_order_relaxed)}};
}

std::string
Manager::configuration_fingerprint(const workspace::WorkspaceConfiguration& configuration) {
    std::string result;
    const auto options = configuration.compiler_options();
    for (const auto& argument : options.arguments()) {
        append_component(result, argument);
    }
    for (const auto& [virtual_path, physical_path] : configuration.virtual_directory_mappings) {
        append_component(result, virtual_path);
        append_component(result, physical_path.generic_string());
    }
    return result;
}

dxc::RuntimeInfo Manager::dxc_runtime_info() const { return implementation_->runtime_info(); }

} // namespace hlsl_intellisense::analysis
