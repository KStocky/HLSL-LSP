#include <hlsl_intellisense/analysis/manager.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
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
    return options;
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
        std::unordered_set<std::string> dependencies;
        bool has_dynamic_includes{};
        std::size_t estimated_bytes{};
        std::uint64_t last_use{};
        dxc::TranslationUnit translation_unit;
    };

    struct WorkerState final {
        WorkerState(workspace::IncludeCacheLimits include_limits) : include_cache{include_limits} {}

        dxc::Intellisense intellisense;
        workspace::IncludeMetadataCache include_cache;
        std::unordered_map<std::string, Entry> entries;
        std::uint64_t use_counter{};
        workspace::IncludeCacheMetrics last_include_metrics;
    };

    Impl(DiagnosticsHandler diagnostics_handler, AnalysisOptions value,
         std::shared_ptr<AnalysisHooks> analysis_hooks, ErrorHandler error_handler)
        : diagnostics{std::move(diagnostics_handler)}, errors{std::move(error_handler)},
          options{validate_options(value)}, hooks{std::move(analysis_hooks)},
          worker_states(options.scheduler.worker_count),
          scheduler{options.scheduler,
                    [this](std::size_t index) { worker_states[index].reset(); }} {
        if (!diagnostics) {
            throw std::invalid_argument{"Analysis manager requires a diagnostics handler"};
        }
    }

    ~Impl() { shutdown(); }

    [[nodiscard]] WorkerState& state(std::size_t index) {
        auto& state = worker_states[index];
        if (!state) {
            auto limits = options.limits.include_cache;
            limits.max_entries =
                worker_share(limits.max_entries, options.scheduler.worker_count, index);
            limits.max_estimated_bytes =
                worker_share(limits.max_estimated_bytes, options.scheduler.worker_count, index);
            state = std::make_unique<WorkerState>(limits);
        }
        return *state;
    }

    void update_include_metrics(WorkerState& state) {
        const auto latest = state.include_cache.metrics();
        include_hits.fetch_add(latest.hits - state.last_include_metrics.hits,
                               std::memory_order_relaxed);
        include_misses.fetch_add(latest.misses - state.last_include_metrics.misses,
                                 std::memory_order_relaxed);
        include_evictions.fetch_add(latest.evictions - state.last_include_metrics.evictions,
                                    std::memory_order_relaxed);
        if (latest.entries >= state.last_include_metrics.entries) {
            include_entries.fetch_add(latest.entries - state.last_include_metrics.entries,
                                      std::memory_order_relaxed);
        } else {
            include_entries.fetch_sub(state.last_include_metrics.entries - latest.entries,
                                      std::memory_order_relaxed);
        }
        if (latest.estimated_bytes >= state.last_include_metrics.estimated_bytes) {
            include_bytes.fetch_add(latest.estimated_bytes -
                                        state.last_include_metrics.estimated_bytes,
                                    std::memory_order_relaxed);
        } else {
            include_bytes.fetch_sub(state.last_include_metrics.estimated_bytes -
                                        latest.estimated_bytes,
                                    std::memory_order_relaxed);
        }
        state.last_include_metrics = latest;
    }

    void remove_entry(WorkerState& state,
                      const std::unordered_map<std::string, Entry>::iterator& entry) {
        translation_unit_bytes.fetch_sub(entry->second.estimated_bytes, std::memory_order_relaxed);
        translation_units.fetch_sub(1, std::memory_order_relaxed);
        state.entries.erase(entry);
    }

    void enforce_limits(WorkerState& state, std::size_t worker, std::string_view current_root) {
        const auto max_entries = worker_share(options.limits.max_translation_units,
                                              options.scheduler.worker_count, worker);
        const auto max_bytes = worker_share(options.limits.max_translation_unit_estimated_bytes,
                                            options.scheduler.worker_count, worker);
        auto state_bytes = [&state] {
            std::size_t total{};
            for (const auto& [root, entry] : state.entries) {
                static_cast<void>(root);
                total += entry.estimated_bytes;
            }
            return total;
        };
        while (state.entries.size() > max_entries || state_bytes() > max_bytes) {
            auto victim = state.entries.end();
            for (auto candidate = state.entries.begin(); candidate != state.entries.end();
                 ++candidate) {
                if (candidate->first == current_root && state.entries.size() > 1) {
                    continue;
                }
                if (victim == state.entries.end() ||
                    std::pair{candidate->second.last_use, candidate->first} <
                        std::pair{victim->second.last_use, victim->first}) {
                    victim = candidate;
                }
            }
            if (victim == state.entries.end()) {
                break;
            }
            remove_entry(state, victim);
            cache_evictions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void analyze(AnalysisInput input, std::uint64_t epoch, std::size_t worker,
                 const json_rpc::CancellationToken& cancellation) {
        try {
            if (hooks && hooks->before_analysis) {
                hooks->before_analysis(input.root.document_uri().identity(), input.root.version());
            }
            cancellation.throw_if_cancellation_requested();

            auto& worker_state = state(worker);
            auto resolution = workspace::resolve_includes(
                input.root, input.open_documents, input.configuration, &worker_state.include_cache);
            update_include_metrics(worker_state);
            cancellation.throw_if_cancellation_requested();

            const auto configuration = Manager::configuration_fingerprint(input.configuration);
            auto cache_key = source_fingerprint(resolution, configuration, input.root.version());
            const auto root_identity = input.root.document_uri().identity();
            const auto root_uri = input.root.uri();
            const auto root_path = resolution.sources.front().path;
            const auto compiler_options = input.configuration.compiler_options();
            const auto compiler_arguments = compiler_options.arguments();
            auto entry = worker_state.entries.find(root_identity);
            bool was_cache_hit{};
            if (entry != worker_state.entries.end() && entry->second.cache_key == cache_key) {
                cache_hits.fetch_add(1, std::memory_order_relaxed);
                entry->second.last_use = ++worker_state.use_counter;
                was_cache_hit = true;
            } else {
                cache_misses.fetch_add(1, std::memory_order_relaxed);
                const auto source_bytes = estimate_sources(resolution);
                std::size_t argument_bytes = compiler_arguments.capacity() * sizeof(std::string);
                for (const auto& argument : compiler_arguments) {
                    argument_bytes += argument.capacity();
                }
                const auto estimate =
                    options.limits.opaque_translation_unit_estimate + sizeof(Entry) + source_bytes +
                    cache_key.capacity() + root_uri.capacity() + root_identity.capacity() +
                    root_path.capacity() + configuration.capacity() + argument_bytes +
                    resolution.dependency_identities.size() * (2U * sizeof(void*));
                if (entry != worker_state.entries.end() && entry->second.root_path == root_path &&
                    entry->second.compiler_arguments == compiler_arguments) {
                    const auto start = Clock::now();
                    entry->second.translation_unit.reparse(std::move(resolution.sources));
                    reparse_microseconds.fetch_add(elapsed_microseconds(start),
                                                   std::memory_order_relaxed);
                    reparse_count.fetch_add(1, std::memory_order_relaxed);
                    translation_unit_bytes.fetch_sub(entry->second.estimated_bytes,
                                                     std::memory_order_relaxed);
                    entry->second.root_uri = root_uri;
                    entry->second.cache_key = std::move(cache_key);
                    entry->second.configuration = configuration;
                    entry->second.version = input.root.version();
                    entry->second.dependencies = std::move(resolution.dependency_identities);
                    entry->second.has_dynamic_includes = resolution.has_dynamic_includes;
                    entry->second.estimated_bytes = estimate;
                    entry->second.last_use = ++worker_state.use_counter;
                    translation_unit_bytes.fetch_add(estimate, std::memory_order_relaxed);
                } else {
                    if (entry != worker_state.entries.end()) {
                        remove_entry(worker_state, entry);
                    }
                    const auto start = Clock::now();
                    auto translation_unit = worker_state.intellisense.parse(
                        root_path, std::move(resolution.sources), compiler_options);
                    parse_microseconds.fetch_add(elapsed_microseconds(start),
                                                 std::memory_order_relaxed);
                    parse_count.fetch_add(1, std::memory_order_relaxed);
                    auto [inserted, created] = worker_state.entries.emplace(
                        root_identity,
                        Entry{.root_uri = root_uri,
                              .root_identity = root_identity,
                              .root_path = root_path,
                              .cache_key = std::move(cache_key),
                              .configuration = configuration,
                              .compiler_arguments = compiler_arguments,
                              .version = input.root.version(),
                              .dependencies = std::move(resolution.dependency_identities),
                              .has_dynamic_includes = resolution.has_dynamic_includes,
                              .estimated_bytes = estimate,
                              .last_use = ++worker_state.use_counter,
                              .translation_unit = std::move(translation_unit)});
                    if (!created) {
                        throw std::logic_error{"Unable to replace analysis cache entry"};
                    }
                    entry = inserted;
                    translation_units.fetch_add(1, std::memory_order_relaxed);
                    translation_unit_bytes.fetch_add(estimate, std::memory_order_relaxed);
                }
            }

            if (cancellation.is_cancellation_requested()) {
                enforce_limits(worker_state, worker, root_identity);
                cancellation.throw_if_cancellation_requested();
            }
            if (was_cache_hit) {
                std::scoped_lock lock{metadata_mutex};
                if (root_epochs[root_identity] != epoch ||
                    cancellation.is_cancellation_requested()) {
                    return;
                }
                metadata.insert_or_assign(
                    root_identity,
                    RootMetadata{.root_uri = root_uri,
                                 .root_identity = root_identity,
                                 .version = input.root.version(),
                                 .configuration_fingerprint = configuration,
                                 .dependency_identities = entry->second.dependencies,
                                 .has_dynamic_includes = entry->second.has_dynamic_includes});
                return;
            }
            const auto dependencies = entry->second.dependencies;
            const auto has_dynamic_includes = entry->second.has_dynamic_includes;
            auto diagnostics_result = entry->second.translation_unit.diagnostics();
            enforce_limits(worker_state, worker, root_identity);
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
            diagnostics(input.root, diagnostics_result, input.generation);
        } catch (const json_rpc::HandlerError&) {
            return;
        } catch (const std::exception& error) {
            if (errors) {
                errors(error.what());
            }
        }
    }

    template <typename Result, typename Operation>
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    [[nodiscard]] Result query(std::string root_identity, std::int64_t version,
                               const json_rpc::CancellationToken& cancellation,
                               Operation operation) {
        auto result = std::make_shared<AsyncResult<Result>>();
        auto future = result->future();
        cancellation.on_cancel([result] {
            result->set_exception(std::make_exception_ptr(
                json_rpc::HandlerError{json_rpc::request_cancelled_code, "Request cancelled"}));
        });
        const auto submitted = scheduler.submit(
            root_identity, version, WorkPriority::interactive, cancellation,
            [this, result, root_identity, version, operation = std::move(operation)](
                std::size_t worker, const json_rpc::CancellationToken& token) mutable {
                try {
                    token.throw_if_cancellation_requested();
                    if (hooks && hooks->before_interactive) {
                        hooks->before_interactive(root_identity);
                    }
                    token.throw_if_cancellation_requested();
                    auto& worker_state = state(worker);
                    const auto entry = worker_state.entries.find(root_identity);
                    if (entry == worker_state.entries.end() || entry->second.version != version) {
                        throw json_rpc::HandlerError{json_rpc::content_modified_code,
                                                     "Analysis was superseded"};
                    }
                    entry->second.last_use = ++worker_state.use_counter;
                    auto value = operation(entry->second);
                    token.throw_if_cancellation_requested();
                    result->set_value(std::move(value));
                } catch (...) {
                    result->set_exception(std::current_exception());
                }
            });
        if (!submitted) {
            result->set_exception(std::make_exception_ptr(
                json_rpc::HandlerError{json_rpc::server_cancelled_code, "Analysis queue full"}));
        }
        return future.get();
    }

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    void erase(std::string root_identity) {
        {
            std::scoped_lock lock{metadata_mutex};
            ++root_epochs[root_identity];
            metadata.erase(root_identity);
        }
        scheduler.cancel_root(root_identity);
        json_rpc::CancellationToken cancellation;
        auto erased_root = root_identity;
        static_cast<void>(
            scheduler.submit(root_identity, std::numeric_limits<std::int64_t>::max(),
                             WorkPriority::interactive, cancellation,
                             [this, root_identity = std::move(erased_root)](
                                 std::size_t worker, const json_rpc::CancellationToken&) {
                                 if (!worker_states[worker]) {
                                     return;
                                 }
                                 auto& state = *worker_states[worker];
                                 if (const auto entry = state.entries.find(root_identity);
                                     entry != state.entries.end()) {
                                     remove_entry(state, entry);
                                 }
                             }));
    }

    void shutdown() {
        if (!stopped.exchange(true, std::memory_order_acq_rel)) {
            scheduler.shutdown();
        }
    }

    DiagnosticsHandler diagnostics;
    ErrorHandler errors;
    AnalysisOptions options;
    std::shared_ptr<AnalysisHooks> hooks;
    std::vector<std::unique_ptr<WorkerState>> worker_states;
    mutable std::mutex metadata_mutex;
    std::unordered_map<std::string, RootMetadata> metadata;
    std::unordered_map<std::string, std::uint64_t> root_epochs;
    std::atomic_bool stopped;
    std::atomic<std::uint64_t> parse_count;
    std::atomic<std::uint64_t> reparse_count;
    std::atomic<std::uint64_t> cache_hits;
    std::atomic<std::uint64_t> cache_misses;
    std::atomic<std::uint64_t> cache_evictions;
    std::atomic<std::uint64_t> completion_count;
    std::atomic<std::uint64_t> parse_microseconds;
    std::atomic<std::uint64_t> reparse_microseconds;
    std::atomic<std::uint64_t> completion_microseconds;
    std::atomic<std::size_t> translation_units;
    std::atomic<std::size_t> translation_unit_bytes;
    std::atomic<std::uint64_t> include_hits;
    std::atomic<std::uint64_t> include_misses;
    std::atomic<std::uint64_t> include_evictions;
    std::atomic<std::size_t> include_entries;
    std::atomic<std::size_t> include_bytes;
    Scheduler scheduler;
};

Manager::Manager(DiagnosticsHandler diagnostics, AnalysisOptions options,
                 std::shared_ptr<AnalysisHooks> hooks, ErrorHandler errors)
    : implementation_{std::make_unique<Impl>(std::move(diagnostics), options, std::move(hooks),
                                             std::move(errors))} {}

Manager::~Manager() = default;

void Manager::analyze(AnalysisInput input) {
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
    if (!submitted && implementation_->errors) {
        implementation_->errors("Analysis queue full; current document analysis was not queued");
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
        [implementation = implementation_.get(), requested_path = std::move(path), line,
         column](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            const auto start = Clock::now();
            auto result = entry.translation_unit.complete(entry.root_path, line, column);
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
        [requested_path = std::move(path), line, column](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            return entry.translation_unit.definition_at(entry.root_path, line, column);
        });
}

std::vector<dxc::Reference> Manager::references(std::string root_identity, std::int64_t version,
                                                std::string path, std::uint32_t line,
                                                std::uint32_t column,
                                                const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Reference>>(
        std::move(root_identity), version, cancellation,
        [requested_path = std::move(path), line, column](Impl::Entry& entry) {
            return entry.translation_unit.references_at(requested_path, line, column);
        });
}

std::optional<dxc::Hover> Manager::hover(std::string root_identity, std::int64_t version,
                                         std::string path, std::uint32_t line, std::uint32_t column,
                                         const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<dxc::Hover>>(
        std::move(root_identity), version, cancellation,
        [requested_path = std::move(path), line, column](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            return entry.translation_unit.hover_at(entry.root_path, line, column);
        });
}

std::optional<dxc::MemoryLayout>
Manager::memory_layout(std::string root_identity, std::int64_t version, std::string path,
                       std::uint32_t line, std::uint32_t column,
                       const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::optional<dxc::MemoryLayout>>(
        std::move(root_identity), version, cancellation,
        [requested_path = std::move(path), line, column](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            return entry.translation_unit.memory_layout_at(entry.root_path, line, column);
        });
}

std::vector<dxc::Signature> Manager::signatures(std::string root_identity, std::int64_t version,
                                                std::string path, std::uint32_t line,
                                                std::uint32_t column,
                                                const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Signature>>(
        std::move(root_identity), version, cancellation,
        [requested_path = std::move(path), line, column](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            return entry.translation_unit.signatures_at(entry.root_path, line, column);
        });
}

std::vector<dxc::Token> Manager::tokens(std::string root_identity, std::int64_t version,
                                        std::string path,
                                        const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Token>>(
        std::move(root_identity), version, cancellation,
        [requested_path = std::move(path)](Impl::Entry& entry) {
            static_cast<void>(requested_path);
            return entry.translation_unit.tokens(entry.root_path);
        });
}

std::vector<dxc::Symbol> Manager::symbols(std::string root_identity, std::int64_t version,
                                          const json_rpc::CancellationToken& cancellation) {
    return implementation_->query<std::vector<dxc::Symbol>>(
        std::move(root_identity), version, cancellation,
        [](Impl::Entry& entry) { return entry.translation_unit.symbols(); });
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

} // namespace hlsl_intellisense::analysis
