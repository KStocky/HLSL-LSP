#pragma once

#include <hlsl_intellisense/capture/capture.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hlsl_intellisense::capture {

struct Entry {
    Invocation invocation;
    std::uint64_t count{};
};

struct Snapshot {
    bool active{};
    std::string endpoint;
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t overflow{};
    std::vector<Entry> entries;
};

// One local session per LSP server. No configuration files or shader contents are read.
class Session final {
  public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Start fails closed if a private local endpoint cannot be established.
    bool start();
    void stop() noexcept;
    [[nodiscard]] std::string token() const;
    [[nodiscard]] Snapshot snapshot() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace hlsl_intellisense::capture
