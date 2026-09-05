#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::dxc {

struct SourceFile {
    std::string path;
    std::string text;
};

// Selects the DXC runtime a language-server process loads. An empty directory
// selects the bundled runtime resolved through the platform loader's default
// search path (the directory that contains the executable). A non-empty
// directory loads the platform DXC library from that directory instead. Because
// DXC IntelliSense is loaded once per process, this selection is process-wide
// and cannot vary per file.
struct RuntimeConfiguration {
    std::string directory;
};

// Describes the DXC runtime actually loaded into the process, for client and
// server diagnostics.
struct RuntimeInfo {
    std::string directory;
    std::string library_path;
    std::string version;
    bool bundled{true};
};

// Thrown when a selected DXC runtime cannot be validated or loaded. Callers turn
// this into an actionable configuration diagnostic rather than restarting.
class RuntimeError final : public std::runtime_error {
  public:
    explicit RuntimeError(const std::string& message);
};

// The platform-specific file name of the DXC compiler library that a runtime
// directory must provide (dxcompiler.dll on Windows, libdxcompiler.so
// elsewhere).
[[nodiscard]] std::string_view runtime_library_name() noexcept;

// Validates that `directory` contains a DXC runtime compatible with this
// platform and returns the absolute path of the compiler library. Throws
// RuntimeError with an actionable message when the directory is empty, missing,
// not a directory, or lacks the required library. An empty `directory` is
// rejected; the bundled default is selected by loading without a directory.
[[nodiscard]] std::string validate_runtime_directory(std::string_view directory);

struct CompilerOptions {
    std::string language_version{"2021"};
    std::string target_profile;
    std::string entry_point;
    std::vector<std::string> defines;
    std::vector<std::string> include_directories;
    std::vector<std::string> additional_arguments;

    [[nodiscard]] std::vector<std::string> arguments() const;
};

enum class DiagnosticSeverity : std::uint8_t { ignored, note, warning, error, fatal };

struct SourceLocation {
    std::string path;
    std::uint32_t line{};
    std::uint32_t column{};
    std::uint32_t offset{};
};

struct Diagnostic {
    DiagnosticSeverity severity{};
    std::string message;
    SourceLocation location;
};

struct Completion {
    std::string label;
    std::string detail;
    std::uint32_t cursor_kind{};
};

struct Definition {
    std::string name;
    SourceLocation location;
};

struct Reference {
    SourceLocation location;
    std::uint32_t start_offset{};
    std::uint32_t end_offset{};
};

struct Hover {
    std::string name;
    std::string qualified_name;
    std::string display_name;
    std::string type;
    std::string declaration;
    std::uint32_t cursor_kind{};
    SourceLocation declaration_location;
    std::uint32_t start_offset{};
    std::uint32_t end_offset{};
};

enum class MemoryLayoutKind : std::uint8_t { natural, constant_buffer };

enum class MemoryLayoutElementKind : std::uint8_t { scalar, vector, matrix, array, record };

struct MemoryLayoutElement {
    std::string name;
    std::string type;
    MemoryLayoutElementKind kind{MemoryLayoutElementKind::scalar};
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint32_t alignment{};
    std::uint32_t array_stride{};
    std::uint32_t matrix_stride{};
    bool row_major{};
    std::optional<std::uint32_t> array_index;
    std::vector<std::uint32_t> array_dimensions;
    std::vector<MemoryLayoutElement> members;
};

struct MemoryLayout {
    std::string name;
    std::string type;
    MemoryLayoutKind kind{MemoryLayoutKind::natural};
    std::uint32_t size{};
    std::uint32_t allocation_size{};
    std::uint32_t alignment{};
    std::optional<std::uint32_t> packed_offset;
    std::string selected_name;
    std::string selected_type;
    std::uint32_t selected_size{};
    std::uint32_t selected_alignment{};
    bool supported{true};
    std::string explanation;
    std::vector<MemoryLayoutElement> members;
};

// Describes one entry of a DXIL input/output signature parameter, populated
// from ID3D12ShaderReflection::GetInputParameterDesc/GetOutputParameterDesc.
struct CompilationSignatureParameter {
    std::string semantic_name;
    std::uint32_t semantic_index{};
    std::uint32_t register_index{};
    std::string system_value;   // e.g. "position", "target", "undefined"
    std::string component_type; // "uint32", "sint32", "float32", or "unknown"
    std::uint8_t mask{};
    std::uint8_t read_write_mask{};
    std::uint32_t stream{};
};

// Describes one bound resource, populated from
// ID3D12ShaderReflection::GetResourceBindingDesc.
struct CompilationResourceBinding {
    std::string name;
    std::string type; // e.g. "cbuffer", "texture", "uav_rwstructured"
    std::uint32_t bind_point{};
    std::uint32_t bind_count{};
    std::uint32_t space{};
    std::string dimension;   // e.g. "texture2d"; empty when not applicable
    std::string return_type; // e.g. "float"; empty when not applicable
};

struct CompilationThreadGroupSize {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

// DXIL reflection extracted via IDxcUtils::CreateReflection and
// ID3D12ShaderReflection. Unavailable for non-DXIL output (e.g. SPIR-V); in
// that case `available` is false and `unavailable_reason` explains why,
// rather than fabricating empty-but-misleading arrays.
struct CompilationReflection {
    bool available{true};
    std::string unavailable_reason;
    std::vector<CompilationSignatureParameter> input_signature;
    std::vector<CompilationSignatureParameter> output_signature;
    std::vector<CompilationResourceBinding> resources;
    std::optional<CompilationThreadGroupSize> thread_group_size;
};

// The compiled output DXC produced, independent of whether reflection could be
// extracted from it.
struct CompilationOutput {
    std::size_t size{};
    std::string type; // "dxil", "spirv", or "none" when no output was produced
};

// The effective compilation configuration and compiler-authoritative result
// for an open document, produced by actually invoking DXC on the real source
// and all resolved in-memory include sources. DXC is the sole authority; a
// compilation failure is reported through `success` and `diagnostics`, never
// as an exception or a success-shaped empty result.
struct CompilationInfo {
    std::string entry_point;
    std::string stage;
    std::string target_profile;
    std::string language_version;
    std::vector<std::string> defines;
    std::vector<std::string> compiler_arguments;
    std::vector<std::string> include_directories;
    std::vector<std::string> resolved_include_paths;
    bool success{};
    std::vector<Diagnostic> diagnostics;
    std::optional<CompilationOutput> output;
    std::optional<CompilationReflection> reflection;
};

struct SignatureParameter {
    std::string label;
    std::string name;
    std::string type;
};

struct Signature {
    std::string label;
    std::string qualified_name;
    std::uint32_t cursor_kind{};
    std::vector<SignatureParameter> parameters;
};

struct Symbol {
    std::string name;
    std::uint32_t cursor_kind{};
    SourceLocation location;
    std::uint32_t start_offset{};
    std::uint32_t end_offset{};
    std::vector<Symbol> children;
};

enum class TokenKind : std::uint8_t {
    punctuation,
    keyword,
    identifier,
    literal,
    comment,
    unknown,
    built_in_type
};

struct Token {
    std::uint32_t line{};
    std::uint32_t column{};
    std::uint32_t length{};
    TokenKind kind{TokenKind::unknown};
    std::uint32_t cursor_kind{};
};

class TranslationUnit final {
  public:
    TranslationUnit(TranslationUnit&&) noexcept;
    TranslationUnit& operator=(TranslationUnit&&) noexcept;
    TranslationUnit(const TranslationUnit&) = delete;
    TranslationUnit& operator=(const TranslationUnit&) = delete;
    ~TranslationUnit();

    [[nodiscard]] std::vector<Diagnostic> diagnostics() const;
    [[nodiscard]] std::vector<Completion> complete(std::string_view path, std::uint32_t line,
                                                   std::uint32_t column) const;
    [[nodiscard]] std::optional<Definition> definition_at(std::string_view path, std::uint32_t line,
                                                          std::uint32_t column) const;
    [[nodiscard]] std::vector<Reference> references_at(std::string_view path, std::uint32_t line,
                                                       std::uint32_t column) const;
    [[nodiscard]] std::optional<Hover> hover_at(std::string_view path, std::uint32_t line,
                                                std::uint32_t column) const;
    [[nodiscard]] std::optional<MemoryLayout>
    memory_layout_at(std::string_view path, std::uint32_t line, std::uint32_t column) const;
    // Compiles the actual root source and all resolved in-memory include
    // sources with the effective compiler arguments and returns the
    // compiler-authoritative configuration and reflection. DXC is invoked
    // directly; there is no fallback parser for HLSL source.
    [[nodiscard]] CompilationInfo compilation_info() const;
    [[nodiscard]] std::vector<Signature> signatures_at(std::string_view path, std::uint32_t line,
                                                       std::uint32_t column) const;
    [[nodiscard]] std::vector<Token> tokens(std::string_view path) const;
    [[nodiscard]] std::vector<Symbol> symbols() const;

    void reparse(std::vector<SourceFile> files);

  private:
    struct Impl;

    explicit TranslationUnit(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;

    friend class Intellisense;
};

class Intellisense final {
  public:
    Intellisense();
    explicit Intellisense(const RuntimeConfiguration& runtime);
    Intellisense(Intellisense&&) noexcept;
    Intellisense& operator=(Intellisense&&) noexcept;
    Intellisense(const Intellisense&) = delete;
    Intellisense& operator=(const Intellisense&) = delete;
    ~Intellisense();

    [[nodiscard]] TranslationUnit parse(std::string root_path, std::vector<SourceFile> files,
                                        const CompilerOptions& options = {}) const;

    [[nodiscard]] RuntimeInfo runtime_info() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> implementation_;

    friend class TranslationUnit;
};

} // namespace hlsl_intellisense::dxc
