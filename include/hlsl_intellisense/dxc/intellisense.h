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

// The register class a resource binds through, derived from the raw
// D3D_SHADER_INPUT_TYPE reported by the compiler (several distinct
// D3D_SIT_* values map to the same register class; e.g. both textures and
// structured buffers bind as SRVs through 't' registers, and tbuffer -
// though a constant-buffer-like declaration in HLSL - was empirically
// confirmed to bind as an SRV through a 't' register, not a 'b' register).
enum class ResourceRegisterClass : std::uint8_t { cbv, srv, uav, sampler, unknown };

// Whether a reflected resource is used by the compiled shader, derived
// strictly from the raw D3D_SIF_UNUSED reflection flag. Pinned DXC
// (1.9.2607.13) was empirically observed to omit declared-but-unreferenced
// resources from reflection entirely (both at default optimization and
// under -Od) rather than emitting them with D3D_SIF_UNUSED set, so `unused`
// is not expected to occur for the single-stage shader profiles this server
// reflects; `unknown` covers any resource for which the flag's meaning
// cannot be established, rather than guessing.
enum class ResourceUsageStatus : std::uint8_t { used, unused, unknown };

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

    // The register class this resource binds through (cbv/srv/uav/sampler).
    ResourceRegisterClass register_class{ResourceRegisterClass::unknown};
    // The raw D3D_SHADER_INPUT_FLAGS bitmask exactly as reported by the
    // compiler, exposed unchanged so clients can inspect bits this server
    // does not itself interpret.
    std::uint32_t raw_flags{};
    // The raw uID (range identifier) reported by the compiler.
    std::uint32_t range_id{};
    // The raw NumSamples reported by the compiler. For SIT_STRUCTURED and
    // SIT_UAV_RWSTRUCTURED* resources the compiler reuses this field to
    // store the structured-buffer byte stride, not a sample count (an
    // empirically confirmed DXC/D3D reflection ABI quirk); for ordinary
    // non-multisampled textures it reads 0xFFFFFFFF ("not applicable").
    // This value is passed through unchanged; no semantics beyond what the
    // compiler reports are inferred.
    std::uint32_t sample_count{};
    // True when bind_count == 0, the sentinel ID3D12ShaderReflection uses
    // for an unbounded shader-side resource array (e.g. `Texture2D T[] :
    // register(t0, space1)`), empirically confirmed against pinned DXC.
    // This is distinct from the UINT_MAX sentinel root-signature descriptor
    // ranges use for unbounded ranges (see RootSignatureDescriptorRange).
    bool unbounded{};
    // True when `space` falls within the D3D12-reserved system space range
    // [0xfffffff0, 0xffffffff], used internally by the runtime/driver and
    // never user-addressable.
    bool system_reserved_space{};
    // Derived strictly from raw_flags & D3D_SIF_UNUSED; see
    // ResourceUsageStatus for why `unused`/`unknown` are not expected to
    // occur for the shader profiles this server reflects.
    ResourceUsageStatus usage{ResourceUsageStatus::unknown};
    // The declaration site of this resource in the current unsaved source
    // snapshot, populated only when the compiler-reflected resource name
    // matches exactly one declaration cursor found by the same DXC
    // IntelliSense parse index used for hover/go-to-definition/document
    // symbols (see TranslationUnit::symbols()) -- never inferred by text
    // parsing. Left empty when the name cannot be found at all, or when it
    // matches more than one declaration (a genuine ambiguity DXC's own
    // cursor tree cannot resolve, e.g. a same-named struct field
    // elsewhere), so a location is only ever reported when it is
    // compiler-unambiguous.
    std::optional<SourceLocation> source_location;
};

// One register range occupied by a single reflected resource within a
// ResourceBindingGroup, expressed so it can never overflow: `end_register`
// is only meaningful when `unbounded` is false.
struct ResourceBindingRange {
    std::string resource_name;
    std::uint32_t base_register{};
    bool unbounded{};
    std::uint32_t end_register{}; // valid only when !unbounded
};

// A provable overlap between two distinct reflected resource bindings that
// share the same register class and space. Only reported when the overlap
// can be established from the reflected register ranges alone; duplicate
// and self comparisons are never reported, and bindings in D3D12's
// system-reserved space range are excluded from collision detection.
struct ResourceBindingCollision {
    std::string first_resource;
    std::string second_resource;
    ResourceRegisterClass register_class{ResourceRegisterClass::unknown};
    std::uint32_t space{};
    std::string message;
};

// Resources grouped by register class and register space, each carrying the
// register range it occupies.
struct ResourceBindingGroup {
    ResourceRegisterClass register_class{ResourceRegisterClass::unknown};
    std::uint32_t space{};
    bool system_reserved_space{};
    std::vector<ResourceBindingRange> ranges;
};

// Deterministic grouping/collision analysis over the reflected resources of
// a single compilation, computed purely from already-reflected register
// data (no HLSL parsing or inference).
struct ResourceBindingAnalysis {
    std::vector<ResourceBindingGroup> groups;
    std::vector<ResourceBindingCollision> collisions;
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
    // Deterministic grouping/collision analysis over `resources`, computed
    // purely from the reflected register data above.
    ResourceBindingAnalysis binding_analysis;
};

// The compiled output DXC produced, independent of whether reflection could be
// extracted from it.
struct CompilationOutput {
    std::size_t size{};
    std::string type; // "dxil", "spirv", or "none" when no output was produced
};

// Whether an embedded root signature is present in the compiled DXIL
// container (DXC_PART_ROOT_SIGNATURE, DXIL container FourCC 'RTS0'), and if
// so, whether this platform can deserialize its details. Presence/absence
// detection itself works on every platform (it only inspects the DXIL
// container via IDxcUtils::GetDxilContainerPart); only detailed
// deserialization requires the Windows D3D12 runtime's
// ID3D12VersionedRootSignatureDeserializer, which this server never
// replaces with a custom RTS0 parser.
enum class RootSignatureAvailability : std::uint8_t {
    present,
    absent,
    not_applicable,
    present_details_unavailable
};

// Mirrors D3D12_SHADER_VISIBILITY.
enum class RootSignatureVisibility : std::uint8_t {
    all,
    vertex,
    hull,
    domain,
    geometry,
    pixel,
    amplification,
    mesh,
    unknown
};

// The register class a root-signature entry (range/root descriptor) grants
// access through. Mirrors D3D12_DESCRIPTOR_RANGE_TYPE for ranges; root
// descriptors and root constants are always cbv/srv/uav (never sampler).
enum class RootSignatureRangeType : std::uint8_t { srv, uav, cbv, sampler, unknown };

// One descriptor range within a descriptor-table root parameter, mirroring
// D3D12_DESCRIPTOR_RANGE1. `num_descriptors` is only meaningful when
// `unbounded` is false: unbounded ranges are reported via NumDescriptors ==
// UINT_MAX in the deserialized root signature, a *different* convention
// from the BindCount == 0 sentinel shader-side reflection uses for
// unbounded resource arrays (see CompilationResourceBinding::unbounded).
struct RootSignatureDescriptorRange {
    RootSignatureRangeType type{RootSignatureRangeType::srv};
    std::uint32_t num_descriptors{};
    bool unbounded{};
    std::uint32_t base_register{};
    std::uint32_t space{};
    std::uint32_t raw_flags{}; // D3D12_DESCRIPTOR_RANGE_FLAGS (version 1.1)
    std::uint32_t offset_in_descriptors_from_table_start{};
};

struct RootSignatureRootConstants {
    std::uint32_t shader_register{};
    std::uint32_t space{};
    std::uint32_t num_32bit_values{};
};

// A root CBV/SRV/UAV descriptor bound directly in the root signature
// (D3D12_ROOT_DESCRIPTOR1), rather than through a descriptor table.
struct RootSignatureRootDescriptor {
    RootSignatureRangeType type{RootSignatureRangeType::cbv};
    std::uint32_t shader_register{};
    std::uint32_t space{};
    std::uint32_t raw_flags{}; // D3D12_ROOT_DESCRIPTOR_FLAGS (version 1.1)
};

enum class RootSignatureParameterKind : std::uint8_t {
    descriptor_table,
    constants,
    root_descriptor
};

// One root parameter, mirroring D3D12_ROOT_PARAMETER1; exactly one of
// `descriptor_table`, `constants`, or `root_descriptor` is populated,
// matching `kind`.
struct RootSignatureParameter {
    RootSignatureParameterKind kind{RootSignatureParameterKind::descriptor_table};
    RootSignatureVisibility visibility{RootSignatureVisibility::all};
    std::vector<RootSignatureDescriptorRange> descriptor_table_ranges;
    std::optional<RootSignatureRootConstants> constants;
    std::optional<RootSignatureRootDescriptor> root_descriptor;
};

// A static sampler declared directly in the root signature
// (D3D12_STATIC_SAMPLER_DESC), never requiring a descriptor-heap slot.
struct RootSignatureStaticSampler {
    std::uint32_t shader_register{};
    std::uint32_t space{};
    RootSignatureVisibility visibility{RootSignatureVisibility::all};
    std::uint32_t filter{};    // raw D3D12_FILTER
    std::uint32_t address_u{}; // raw D3D12_TEXTURE_ADDRESS_MODE
    std::uint32_t address_v{};
    std::uint32_t address_w{};
    float mip_lod_bias{};
    std::uint32_t max_anisotropy{};
    std::uint32_t comparison_func{}; // raw D3D12_COMPARISON_FUNC
    std::uint32_t border_color{};    // raw D3D12_STATIC_BORDER_COLOR
    float min_lod{};
    float max_lod{};
};

// The deserialized contents of an embedded root signature, always exposed
// through the version-1.1 shape (root descriptors/ranges carry
// D3D12_ROOT_DESCRIPTOR_FLAGS/D3D12_DESCRIPTOR_RANGE_FLAGS, defaulting to
// NONE when the signature was authored/serialized as version 1.0) while
// `version` reports the signature's true, unconverted version so clients
// are never misled about what was actually embedded.
struct RootSignatureDetails {
    std::string version;       // "1.0" or "1.1"
    std::uint32_t raw_flags{}; // D3D12_ROOT_SIGNATURE_FLAGS, exposed unchanged
    bool cbv_srv_uav_heap_directly_indexed{};
    bool sampler_heap_directly_indexed{};
    std::vector<RootSignatureParameter> parameters;
    std::vector<RootSignatureStaticSampler> static_samplers;
};

// The compiler-authoritative embedded root-signature result for a single
// compilation. Deserialization uses only the official
// D3D12CreateVersionedRootSignatureDeserializer API on Windows; there is no
// custom RTS0 binary parser anywhere in this codebase.
struct RootSignatureInfo {
    RootSignatureAvailability availability{RootSignatureAvailability::absent};
    // Populated for not_applicable and present_details_unavailable,
    // explaining why (e.g. "SPIR-V has no root signature concept" or "this
    // platform lacks the Windows D3D12 root-signature deserializer").
    std::string unavailable_reason;
    // Populated only when availability == present.
    std::optional<RootSignatureDetails> details;
};

// Whether reflected shader resources are provably covered by the
// deserialized root signature for the compiled entry point's stage. Only
// register class/space/range coverage, active-stage shader visibility, and
// static samplers are compared; bindless (ResourceDescriptorHeap /
// SamplerDescriptorHeap) accesses are invisible to reflection and are never
// guessed at.
enum class ResourceCompatibilityStatus : std::uint8_t { compatible, incompatible, unknown };

struct ResourceCompatibilityIssue {
    std::string resource_name;
    ResourceRegisterClass register_class{ResourceRegisterClass::unknown};
    std::uint32_t space{};
    std::string message;
};

struct CompilationCompatibility {
    ResourceCompatibilityStatus status{ResourceCompatibilityStatus::unknown};
    // Populated when status == unknown, explaining why compatibility could
    // not be determined (e.g. no embedded root signature, or root
    // signature details unavailable on this platform).
    std::string explanation;
    std::vector<ResourceCompatibilityIssue> issues;
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
    // Populated whenever compilation succeeds and produces non-empty
    // compiler output, for both DXIL and SPIR-V (absent only when
    // compilation failed or produced no output at all). For SPIR-V,
    // `availability` is reported as `not_applicable` rather than this field
    // itself being omitted -- root signatures are a Direct3D 12 binding
    // model concept, so "not applicable" is itself compiler-authoritative
    // information worth surfacing distinctly from "absent".
    std::optional<RootSignatureInfo> root_signature;
    // Populated whenever `root_signature` is populated, so a present root
    // signature is never paired with a null compatibility result. Reports
    // `ResourceCompatibilityStatus::unknown` with an explanation whenever
    // compatibility cannot be determined -- for example, no embedded root
    // signature, root signature details unavailable on this platform, or
    // shader reflection metadata unavailable for this compiled output.
    std::optional<CompilationCompatibility> compatibility;
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
