#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hlsl_intellisense::capture {

inline constexpr std::uint32_t protocol_version = 1;

struct Invocation {
    std::string source;
    std::string entry_point;
    std::string target_profile;
    std::string language_version;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::string> include_directories;
    std::vector<std::pair<std::string, std::string>> virtual_mappings;
    std::vector<std::string> arguments;
    std::string output_mode;
    std::string pipeline;
    std::string stage;
};

// Construct disconnected. report() never performs IPC or waits for a compiler.
// Destroy or disconnect before unloading the module containing this SDK.
class Client final {
  public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Returns false for unsupported or invalid endpoints. Replaces any prior session.
    bool connect(std::string endpoint, std::string token);
    void disconnect() noexcept;
    // False means disabled, malformed, oversized, or queue full. Never throws.
    bool report(const Invocation& invocation) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace hlsl_intellisense::capture
