#pragma once

#include <hlsl_intellisense/dxc/intellisense.h>

#include <cstddef>
#include <istream>
#include <ostream>

namespace hlsl_intellisense::analysis {

inline constexpr std::size_t analysis_worker_max_payload_size = std::size_t{64} * 1024U * 1024U;
inline constexpr unsigned analysis_worker_protocol_version = 1;

// Runs the private parent/worker protocol used to isolate uninterruptible DXC
// calls. This is deliberately not an LSP or JSON-RPC endpoint.
[[nodiscard]] int run_analysis_worker(std::istream& input, std::ostream& output,
                                      std::ostream& errors,
                                      const dxc::RuntimeConfiguration& runtime = {});

} // namespace hlsl_intellisense::analysis
