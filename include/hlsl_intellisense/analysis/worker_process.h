#pragma once

#include <hlsl_intellisense/json_rpc/dispatcher.h>
#include <hlsl_intellisense/json_rpc/message.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hlsl_intellisense::analysis {

enum class WorkerProcessErrorCode {
    launch_failed,
    io_error,
    timed_out,
    cancelled,
    unexpected_eof,
    worker_exited,
    malformed_reply,
    mismatched_reply,
    worker_error,
};

class WorkerProcessError final : public std::runtime_error {
  public:
    WorkerProcessError(WorkerProcessErrorCode code, std::string message);

    [[nodiscard]] WorkerProcessErrorCode code() const noexcept;

  private:
    WorkerProcessErrorCode code_;
};

struct WorkerProcessOptions {
    // An empty path selects hlsl-analysis-worker beside the current executable.
    std::filesystem::path executable;
    std::string dxc_runtime_directory;
    std::chrono::milliseconds shutdown_timeout{500};
};

// Owns one persistent analysis worker and serializes its private protocol.
// Transport and protocol failures discard the child so the next request starts
// a clean worker.
class WorkerProcess final {
  public:
    explicit WorkerProcess(WorkerProcessOptions options = {});
    WorkerProcess(const WorkerProcess&) = delete;
    auto operator=(const WorkerProcess&) -> WorkerProcess& = delete;
    WorkerProcess(WorkerProcess&&) = delete;
    auto operator=(WorkerProcess&&) -> WorkerProcess& = delete;
    ~WorkerProcess();

    [[nodiscard]] json_rpc::Json request(std::string_view method, json_rpc::Json params,
                                         std::chrono::milliseconds timeout,
                                         const json_rpc::CancellationToken& cancellation);

    // Attempts the protocol shutdown handshake, then forcibly terminates only
    // this controller's child if it does not exit within the configured bound.
    void shutdown() noexcept;

  private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace hlsl_intellisense::analysis
