#pragma once

#include <hlsl_intellisense/analysis/worker_process.h>
#include <hlsl_intellisense/dxc/intellisense.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::analysis {

inline constexpr std::size_t analysis_worker_max_payload_size = std::size_t{64} * 1024U * 1024U;
inline constexpr unsigned analysis_worker_protocol_version = 2;

enum class WorkerAnalysisKind : std::uint8_t { cache_hit, parsed, reparsed };

struct WorkerAnalysisResult {
    std::vector<dxc::Diagnostic> diagnostics;
    WorkerAnalysisKind kind{WorkerAnalysisKind::parsed};
};

struct WorkerDocumentSymbolsResult {
    std::vector<dxc::Symbol> symbols;
    bool truncated{};
};

// Type-safe client for the private worker protocol. JSON never escapes this
// boundary: every required field is validated before a result reaches
// analysis::Manager.
class WorkerClient final {
  public:
    explicit WorkerClient(WorkerProcessOptions options = {});
    WorkerClient(const WorkerClient&) = delete;
    auto operator=(const WorkerClient&) -> WorkerClient& = delete;
    WorkerClient(WorkerClient&&) = delete;
    auto operator=(WorkerClient&&) -> WorkerClient& = delete;
    ~WorkerClient();

    [[nodiscard]] WorkerAnalysisResult
    analyze(std::string root_identity, std::string cache_key, std::string path,
            std::vector<dxc::SourceFile> sources, dxc::CompilerOptions options,
            std::chrono::milliseconds timeout, const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Completion>
    complete(std::string_view root_identity, std::string path, std::uint32_t line,
             std::uint32_t column, std::chrono::milliseconds timeout,
             const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::Definition>
    definition(std::string_view root_identity, std::string path, std::uint32_t line,
               std::uint32_t column, std::chrono::milliseconds timeout,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Reference>
    references(std::string_view root_identity, std::string path, std::uint32_t line,
               std::uint32_t column, std::chrono::milliseconds timeout,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::Hover> hover(std::string_view root_identity, std::string path,
                                                  std::uint32_t line, std::uint32_t column,
                                                  std::chrono::milliseconds timeout,
                                                  const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::MemoryLayout>
    memory_layout(std::string_view root_identity, std::string path, std::uint32_t line,
                  std::uint32_t column, std::chrono::milliseconds timeout,
                  const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] dxc::CompilationInfo
    compilation_info(std::string_view root_identity, std::chrono::milliseconds timeout,
                     const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Signature>
    signatures(std::string_view root_identity, std::string path, std::uint32_t line,
               std::uint32_t column, std::chrono::milliseconds timeout,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::InlayHint>
    inlay_hints(std::string_view root_identity, std::string path,
                std::vector<dxc::SourceOffsetRange> ranges, std::vector<dxc::InlayCall> calls,
                dxc::InlayHintOptions options, std::chrono::milliseconds timeout,
                const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Token> tokens(std::string_view root_identity, std::string path,
                                                 std::chrono::milliseconds timeout,
                                                 const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::SourceRange>
    skipped_ranges(std::string_view root_identity, std::chrono::milliseconds timeout,
                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::MacroDefinition>
    macro_definitions(std::string_view root_identity, std::chrono::milliseconds timeout,
                      const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::Symbol> symbols(std::string_view root_identity,
                                                   std::chrono::milliseconds timeout,
                                                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] WorkerDocumentSymbolsResult
    document_symbols(std::string_view root_identity, std::string path, std::size_t max_symbols,
                     std::chrono::milliseconds timeout,
                     const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::optional<dxc::CallableSymbol>
    callable_at(std::string_view root_identity, std::string path, std::uint32_t line,
                std::uint32_t column, std::chrono::milliseconds timeout,
                const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::OutgoingCall>
    outgoing_calls(std::string_view root_identity, std::string path, std::uint32_t line,
                   std::uint32_t column, std::chrono::milliseconds timeout,
                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] std::vector<dxc::IncomingCall>
    incoming_calls(std::string_view root_identity, std::string path, std::uint32_t line,
                   std::uint32_t column, std::chrono::milliseconds timeout,
                   const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] dxc::EntryPointDataFlow
    entry_point_data_flow(std::string_view root_identity, dxc::EntryPointDataFlowLimits limits,
                          std::chrono::milliseconds timeout,
                          const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] bool verify_call_hierarchy_identity(
        std::string_view root_identity, std::string path, std::uint32_t line, std::uint32_t column,
        std::uint32_t expected_start_offset, std::uint32_t expected_cursor_kind,
        std::string expected_name, std::chrono::milliseconds timeout,
        const json_rpc::CancellationToken& cancellation);

    void erase(std::string_view root_identity, std::chrono::milliseconds timeout,
               const json_rpc::CancellationToken& cancellation);
    [[nodiscard]] dxc::RuntimeInfo runtime_info(std::chrono::milliseconds timeout,
                                                const json_rpc::CancellationToken& cancellation);
    void reset() noexcept;
    void shutdown() noexcept;

  private:
    WorkerProcess process_;
};

// Runs the private parent/worker protocol used to isolate uninterruptible DXC
// calls. This is deliberately not an LSP or JSON-RPC endpoint.
[[nodiscard]] int run_analysis_worker(std::istream& input, std::ostream& output,
                                      std::ostream& errors,
                                      const dxc::RuntimeConfiguration& runtime = {});

} // namespace hlsl_intellisense::analysis
