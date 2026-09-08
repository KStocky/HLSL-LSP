#include <hlsl_intellisense/analysis/worker_protocol.h>

#include <hlsl_intellisense/json_rpc/framing.h>

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace nlohmann {

template <typename Value>
struct adl_serializer<Value,
                      std::enable_if_t<std::is_integral_v<Value> && std::is_unsigned_v<Value> &&
                                       !std::is_same_v<Value, bool>>> {
    static void to_json(json& output, Value value) { nlohmann::detail::to_json(output, value); }

    static void from_json(const json& input, Value& value) {
        if (!input.is_number_unsigned()) {
            throw std::invalid_argument{"Unsigned worker protocol field has the wrong type"};
        }
        const auto raw = input.get_ref<const json::number_unsigned_t&>();
        if (raw > static_cast<json::number_unsigned_t>((std::numeric_limits<Value>::max)())) {
            throw std::out_of_range{"Unsigned worker protocol field is out of range"};
        }
        value = static_cast<Value>(raw);
    }
};

template <typename Value>
struct adl_serializer<Value, std::enable_if_t<std::is_floating_point_v<Value>>> {
    static void to_json(json& output, Value value) { nlohmann::detail::to_json(output, value); }

    static void from_json(const json& input, Value& value) {
        if (!input.is_number_float()) {
            throw std::invalid_argument{"Floating-point worker protocol field has the wrong type"};
        }
        const auto raw = input.get_ref<const json::number_float_t&>();
        if (!std::isfinite(raw) ||
            raw > static_cast<json::number_float_t>((std::numeric_limits<Value>::max)()) ||
            raw < static_cast<json::number_float_t>((std::numeric_limits<Value>::lowest)())) {
            throw std::out_of_range{"Floating-point worker protocol field is out of range"};
        }
        value = static_cast<Value>(raw);
    }
};

template <typename Value> struct adl_serializer<std::optional<Value>> {
    static void to_json(json& output, const std::optional<Value>& value) {
        output = value ? json(*value) : json(nullptr);
    }

    static void from_json(const json& input, std::optional<Value>& value) {
        if (input.is_null()) {
            value.reset();
        } else {
            value = input.get<Value>();
        }
    }
};

} // namespace nlohmann

// These serializers are deliberately implementation-only. They are the
// private, versioned wire representation between Manager and its DXC worker;
// they are not part of the LSP or any public client contract.
namespace hlsl_intellisense::dxc {
namespace {

using Json = nlohmann::json;

template <typename Enum> void encode_enum(Json& output, Enum value) {
    output = static_cast<std::underlying_type_t<Enum>>(value);
}

template <typename Enum>
void decode_enum(const Json& input, Enum& value, Enum maximum, std::string_view name) {
    if (!input.is_number_unsigned()) {
        throw std::invalid_argument{std::string{name} + " must be an unsigned integer"};
    }
    const auto raw = input.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(maximum)) {
        throw std::invalid_argument{std::string{name} + " is outside the supported range"};
    }
    value = static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(raw));
}

} // namespace

#define HLSL_WORKER_ENUM(Type, Maximum)                                                            \
    void to_json(Json& output, const Type& value) { encode_enum(output, value); }                  \
    void from_json(const Json& input, Type& value) { decode_enum(input, value, Maximum, #Type); }

HLSL_WORKER_ENUM(DiagnosticSeverity, DiagnosticSeverity::fatal)
HLSL_WORKER_ENUM(MemoryLayoutKind, MemoryLayoutKind::constant_buffer)
HLSL_WORKER_ENUM(MemoryLayoutElementKind, MemoryLayoutElementKind::record)
HLSL_WORKER_ENUM(ResourceRegisterClass, ResourceRegisterClass::unknown)
HLSL_WORKER_ENUM(ResourceUsageStatus, ResourceUsageStatus::unknown)
HLSL_WORKER_ENUM(RootSignatureAvailability, RootSignatureAvailability::present_details_unavailable)
HLSL_WORKER_ENUM(RootSignatureVisibility, RootSignatureVisibility::unknown)
HLSL_WORKER_ENUM(RootSignatureRangeType, RootSignatureRangeType::unknown)
HLSL_WORKER_ENUM(RootSignatureParameterKind, RootSignatureParameterKind::root_descriptor)
HLSL_WORKER_ENUM(ResourceCompatibilityStatus, ResourceCompatibilityStatus::unknown)
HLSL_WORKER_ENUM(GlobalAccessKind, GlobalAccessKind::read_write)
HLSL_WORKER_ENUM(TokenKind, TokenKind::built_in_type)
HLSL_WORKER_ENUM(InlayHintCategory, InlayHintCategory::array_stride)

#undef HLSL_WORKER_ENUM

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceFile, path, text, rewritten)
void to_json(Json& output, const RuntimeInfo& value) {
    output = Json{{"directory", value.directory},
                  {"libraryPath", value.library_path},
                  {"version", value.version},
                  {"bundled", value.bundled}};
}

void from_json(const Json& input, RuntimeInfo& value) {
    input.at("directory").get_to(value.directory);
    input.at("libraryPath").get_to(value.library_path);
    input.at("version").get_to(value.version);
    input.at("bundled").get_to(value.bundled);
}

void to_json(Json& output, const CompilerOptions& value) {
    output = Json{{"languageVersion", value.language_version},
                  {"targetProfile", value.target_profile},
                  {"entryPoint", value.entry_point},
                  {"defines", value.defines},
                  {"includeDirectories", value.include_directories},
                  {"additionalArguments", value.additional_arguments}};
}

void from_json(const Json& input, CompilerOptions& value) {
    input.at("languageVersion").get_to(value.language_version);
    input.at("targetProfile").get_to(value.target_profile);
    input.at("entryPoint").get_to(value.entry_point);
    input.at("defines").get_to(value.defines);
    input.at("includeDirectories").get_to(value.include_directories);
    input.at("additionalArguments").get_to(value.additional_arguments);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceLocation, path, line, column, offset)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceRange, start, end)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MacroDefinition, name, value, location)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FixIt, range, replacement_text)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Diagnostic, severity, message, location, fix_its)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Completion, label, detail, cursor_kind)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Definition, name, location)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Reference, location, start_offset, end_offset)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Hover, name, qualified_name, display_name, type, declaration,
                                   cursor_kind, declaration_location, start_offset, end_offset)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemoryLayoutElement, name, type, kind, offset, size,
                                   allocation_size, alignment, array_stride, matrix_stride,
                                   row_major, array_index, array_dimensions, members)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemoryLayout, name, type, kind, size, allocation_size, alignment,
                                   packed_offset, selected_name, selected_type, selected_size,
                                   selected_alignment, supported, explanation, members)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationSignatureParameter, semantic_name, semantic_index,
                                   register_index, system_value, component_type, mask,
                                   read_write_mask, stream)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationResourceBinding, name, type, bind_point, bind_count,
                                   space, dimension, return_type, register_class, raw_flags,
                                   range_id, sample_count, unbounded, system_reserved_space, usage,
                                   source_location)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceBindingRange, resource_name, base_register, unbounded,
                                   end_register)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceBindingCollision, first_resource, second_resource,
                                   register_class, space, message)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceBindingGroup, register_class, space,
                                   system_reserved_space, ranges)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceBindingAnalysis, groups, collisions)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationThreadGroupSize, x, y, z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComputeBarrierLocation, label, location, start_offset,
                                   end_offset)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComputeGroupSharedDeclaration, name, type, declaration, location,
                                   start_offset, end_offset, bytes, size_unavailable_reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComputeWaveSize, known, min, max, preferred, min_max_source,
                                   preferred_source, explanation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComputeCompilerMetadata, barrier_locations_available,
                                   barrier_locations_truncated,
                                   barrier_locations_unavailable_reason, barrier_locations,
                                   group_shared_available, group_shared_truncated,
                                   group_shared_unavailable_reason, group_shared_total_bytes,
                                   group_shared_total_bytes_unavailable_reason,
                                   group_shared_declarations, wave_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComputeMetadataLimits, max_group_shared_declarations,
                                   max_barrier_locations)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationReflection, available, unavailable_reason,
                                   input_signature, output_signature, resources, thread_group_size,
                                   barrier_instruction_count, binding_analysis)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationOutput, size, type)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationDisassembly, available, text, unavailable_reason,
                                   truncated, original_size, displayed_size, format)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureDescriptorRange, type, num_descriptors, unbounded,
                                   base_register, space, raw_flags,
                                   offset_in_descriptors_from_table_start)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureRootConstants, shader_register, space,
                                   num_32bit_values)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureRootDescriptor, type, shader_register, space,
                                   raw_flags)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureParameter, kind, visibility,
                                   descriptor_table_ranges, constants, root_descriptor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureStaticSampler, shader_register, space, visibility,
                                   filter, address_u, address_v, address_w, mip_lod_bias,
                                   max_anisotropy, comparison_func, border_color, min_lod, max_lod)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureDetails, version, raw_flags,
                                   cbv_srv_uav_heap_directly_indexed, sampler_heap_directly_indexed,
                                   parameters, static_samplers)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootSignatureInfo, availability, unavailable_reason, details)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceCompatibilityIssue, resource_name, register_class, space,
                                   message)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationCompatibility, status, explanation, issues)

using PsvWaveSize = decltype(std::declval<CompilationInfo>().psv_wave_size);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PsvWaveSize, available, min, max, unavailable_reason)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompilationInfo, entry_point, stage, target_profile,
                                   language_version, defines, compiler_arguments,
                                   include_directories, resolved_include_paths, success,
                                   diagnostics, output, disassembly, reflection, root_signature,
                                   compatibility, psv_wave_size, compute_metadata)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SignatureParameter, label, name, type)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Signature, label, qualified_name, cursor_kind, parameters)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Symbol, name, cursor_kind, location, start_offset, end_offset,
                                   extent, children)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CallableSymbol, name, qualified_name, signature, cursor_kind,
                                   location, start_offset, end_offset, is_definition)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutgoingCall, callee, call_sites)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IncomingCall, caller, call_sites)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GlobalAccess, name, qualified_name, cursor_kind, location,
                                   start_offset, end_offset, access)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReachableFunction, function, depth, recursive)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EntryPointDataFlowLimits, max_functions_visited,
                                   max_global_accesses, max_barrier_locations,
                                   max_unused_declaration_candidates, max_definitions_collected)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EntryPointDataFlow, found, explanation, entry_point,
                                   reachable_functions, unreachable_functions, unused_declarations,
                                   global_accesses, barrier_locations, truncated,
                                   functions_visited_truncated, definitions_truncated,
                                   global_accesses_truncated, barrier_locations_truncated,
                                   unused_declarations_truncated, functions_visited)
void to_json(Json& output, const Token& value) {
    output = Json{{"line", value.line},
                  {"column", value.column},
                  {"length", value.length},
                  {"kind", value.kind},
                  {"cursor_kind", value.cursor_kind}};
}

void from_json(const Json& input, Token& value) {
    const auto checked_u32 = [&input](std::string_view name) {
        const auto& field = input.at(std::string{name});
        if (!field.is_number_unsigned()) {
            throw std::invalid_argument{std::string{name} + " must be an unsigned integer"};
        }
        const auto raw = field.get<std::uint64_t>();
        if (raw > (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::out_of_range{std::string{name} + " is out of range"};
        }
        return static_cast<std::uint32_t>(raw);
    };
    value.line = checked_u32("line");
    value.column = checked_u32("column");
    value.length = checked_u32("length");
    input.at("kind").get_to(value.kind);
    value.cursor_kind = checked_u32("cursor_kind");
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InlayHintOptions, types, parameters, matrix_orientation,
                                   registers, packed_offsets, array_strides)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InlayCall, line, column, argument_offsets)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceOffsetRange, start, end)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InlayHint, offset, label, category)

} // namespace hlsl_intellisense::dxc

namespace hlsl_intellisense::analysis {

using Json = nlohmann::json;

void to_json(Json& output, const WorkerAnalysisKind& value) {
    output = static_cast<std::underlying_type_t<WorkerAnalysisKind>>(value);
}

void from_json(const Json& input, WorkerAnalysisKind& value) {
    if (!input.is_number_unsigned()) {
        throw std::invalid_argument{"WorkerAnalysisKind must be an unsigned integer"};
    }
    const auto raw = input.get<std::underlying_type_t<WorkerAnalysisKind>>();
    if (raw >
        static_cast<std::underlying_type_t<WorkerAnalysisKind>>(WorkerAnalysisKind::reparsed)) {
        throw std::invalid_argument{"WorkerAnalysisKind is outside the supported range"};
    }
    value = static_cast<WorkerAnalysisKind>(raw);
}

void to_json(Json& output, const WorkerAnalysisResult& value) {
    output = Json{{"diagnostics", value.diagnostics}, {"kind", value.kind}};
}

void from_json(const Json& input, WorkerAnalysisResult& value) {
    input.at("diagnostics").get_to(value.diagnostics);
    input.at("kind").get_to(value.kind);
}

void to_json(Json& output, const WorkerDocumentSymbolsResult& value) {
    output = Json{{"symbols", value.symbols}, {"truncated", value.truncated}};
}

void from_json(const Json& input, WorkerDocumentSymbolsResult& value) {
    input.at("symbols").get_to(value.symbols);
    input.at("truncated").get_to(value.truncated);
}

namespace {

template <typename Value> [[nodiscard]] Value required(const Json& object, std::string_view name) {
    if (!object.is_object()) {
        throw std::invalid_argument{"Analysis worker parameters must be an object"};
    }
    return object.at(std::string{name}).get<Value>();
}

[[nodiscard]] Json response(std::uint64_t id, Json result) {
    return Json{
        {"protocol", analysis_worker_protocol_version}, {"id", id}, {"result", std::move(result)}};
}

[[nodiscard]] Json error_response(std::optional<std::uint64_t> id, std::string_view message) {
    return Json{{"protocol", analysis_worker_protocol_version},
                {"id", id ? Json(*id) : Json(nullptr)},
                {"error", Json{{"message", message}}}};
}

struct WorkerEntry final {
    std::string cache_key;
    std::string path;
    std::vector<std::string> arguments;
    dxc::TranslationUnit translation_unit;
};

class Worker final {
  public:
    explicit Worker(const dxc::RuntimeConfiguration& runtime) : intellisense_{runtime} {}

    [[nodiscard]] Json dispatch(const Json& request, bool& shutdown) {
        if (!request.is_object()) {
            throw std::invalid_argument{"Analysis worker request must be an object"};
        }
        const auto id = request.at("id").get<std::uint64_t>();
        if (request.at("protocol").get<unsigned>() != analysis_worker_protocol_version) {
            return error_response(id, "Unsupported analysis worker protocol version");
        }
        const auto method = request.at("method").get<std::string>();
        const auto& params = request.at("params");
        if (!params.is_object()) {
            throw std::invalid_argument{"Analysis worker parameters must be an object"};
        }

        if (method == "analyze") {
            return response(id, Json(analyze(params)));
        }
        if (method == "complete") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.complete(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "definition") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.definition_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "references") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.references_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "hover") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.hover_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "memoryLayout") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.memory_layout_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "compilationInfo") {
            return response(id, Json(find_entry(params).translation_unit.compilation_info()));
        }
        if (method == "signatures") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.signatures_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "inlayHints") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.inlay_hints(
                                    required<std::string>(params, "path"),
                                    required<std::vector<dxc::SourceOffsetRange>>(params, "ranges"),
                                    required<std::vector<dxc::InlayCall>>(params, "calls"),
                                    required<dxc::InlayHintOptions>(params, "options"))));
        }
        if (method == "tokens") {
            auto& entry = find_entry(params);
            return response(
                id, Json(entry.translation_unit.tokens(required<std::string>(params, "path"))));
        }
        if (method == "skippedRanges") {
            return response(id, Json(find_entry(params).translation_unit.skipped_ranges()));
        }
        if (method == "macroDefinitions") {
            return response(id, Json(find_entry(params).translation_unit.macro_definitions()));
        }
        if (method == "symbols") {
            return response(id, Json(find_entry(params).translation_unit.symbols()));
        }
        if (method == "documentSymbols") {
            auto& entry = find_entry(params);
            bool truncated = false;
            auto symbols = entry.translation_unit.symbols(
                required<std::string>(params, "path"), {},
                required<std::size_t>(params, "maxSymbols"), &truncated);
            return response(id, Json(WorkerDocumentSymbolsResult{.symbols = std::move(symbols),
                                                                 .truncated = truncated}));
        }
        if (method == "callableAt") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.callable_at(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "outgoingCalls") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.outgoing_calls(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "incomingCalls") {
            auto& entry = find_entry(params);
            return response(id, Json(entry.translation_unit.incoming_calls(
                                    required<std::string>(params, "path"),
                                    required<std::uint32_t>(params, "line"),
                                    required<std::uint32_t>(params, "column"))));
        }
        if (method == "entryPointDataFlow") {
            return response(id, Json(find_entry(params).translation_unit.entry_point_data_flow(
                                    required<dxc::EntryPointDataFlowLimits>(params, "limits"))));
        }
        if (method == "verifyCallHierarchyIdentity") {
            auto& entry = find_entry(params);
            const auto path = required<std::string>(params, "path");
            const auto line = required<std::uint32_t>(params, "line");
            const auto column = required<std::uint32_t>(params, "column");
            const auto expected_start_offset =
                required<std::uint32_t>(params, "expectedStartOffset");
            const auto expected_cursor_kind = required<std::uint32_t>(params, "expectedCursorKind");
            const auto expected_name = required<std::string>(params, "expectedName");
            const auto current = entry.translation_unit.callable_at(path, line, column);
            const auto matches =
                current.has_value() && current->start_offset == expected_start_offset &&
                current->cursor_kind == expected_cursor_kind && current->name == expected_name;
            return response(id, Json(matches));
        }
        if (method == "erase") {
            entries_.erase(required<std::string>(params, "rootIdentity"));
            return response(id, Json::object());
        }
        if (method == "runtimeInfo") {
            return response(id, Json(intellisense_.runtime_info()));
        }
        if (method == "shutdown") {
            shutdown = true;
            entries_.clear();
            return response(id, Json::object());
        }
        return error_response(id, "Unknown analysis worker method");
    }

  private:
    [[nodiscard]] WorkerEntry& find_entry(const Json& params) {
        const auto root_identity = required<std::string>(params, "rootIdentity");
        const auto entry = entries_.find(root_identity);
        if (entry == entries_.end()) {
            throw std::invalid_argument{"Analysis worker translation unit is unavailable"};
        }
        return entry->second;
    }

    [[nodiscard]] WorkerAnalysisResult analyze(const Json& params) {
        const auto root_identity = required<std::string>(params, "rootIdentity");
        const auto cache_key = required<std::string>(params, "cacheKey");
        const auto path = required<std::string>(params, "path");
        auto sources = required<std::vector<dxc::SourceFile>>(params, "sources");
        const auto options = required<dxc::CompilerOptions>(params, "compilerOptions");
        const auto arguments = options.arguments();
        auto entry = entries_.find(root_identity);
        auto kind = WorkerAnalysisKind::parsed;
        if (entry != entries_.end() && entry->second.cache_key == cache_key) {
            kind = WorkerAnalysisKind::cache_hit;
        } else if (sources.empty()) {
            throw std::invalid_argument{"Analysis worker requires sources for a cache miss"};
        } else if (entry != entries_.end() && entry->second.path == path &&
                   entry->second.arguments == arguments) {
            entry->second.translation_unit.reparse(std::move(sources));
            entry->second.cache_key = cache_key;
            kind = WorkerAnalysisKind::reparsed;
        } else {
            auto translation_unit = intellisense_.parse(path, std::move(sources), options);
            entries_.insert_or_assign(root_identity,
                                      WorkerEntry{.cache_key = cache_key,
                                                  .path = path,
                                                  .arguments = arguments,
                                                  .translation_unit = std::move(translation_unit)});
            entry = entries_.find(root_identity);
        }
        return {.diagnostics = entry->second.translation_unit.diagnostics(), .kind = kind};
    }

    dxc::Intellisense intellisense_;
    std::unordered_map<std::string, WorkerEntry> entries_;
};

template <typename Result>
[[nodiscard]] Result decode_result(WorkerProcess& process, Json value, std::string_view method) {
    try {
        return value.get<Result>();
    } catch (const std::exception& error) {
        process.reset();
        throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                 "Analysis worker returned a malformed '" + std::string{method} +
                                     "' result: " + error.what()};
    }
}

[[nodiscard]] Json root_params(std::string_view root_identity) {
    return Json{{"rootIdentity", root_identity}};
}

[[nodiscard]] Json position_params(std::string_view root_identity, std::string path,
                                   std::uint32_t line, std::uint32_t column) {
    return Json{{"rootIdentity", root_identity},
                {"path", std::move(path)},
                {"line", line},
                {"column", column}};
}

} // namespace

WorkerClient::WorkerClient(WorkerProcessOptions options) : process_{std::move(options)} {}

WorkerClient::~WorkerClient() = default;

WorkerAnalysisResult WorkerClient::analyze(std::string root_identity, std::string cache_key,
                                           std::string path, std::vector<dxc::SourceFile> sources,
                                           dxc::CompilerOptions options,
                                           std::chrono::milliseconds timeout,
                                           const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("analyze",
                                   Json{{"rootIdentity", std::move(root_identity)},
                                        {"cacheKey", std::move(cache_key)},
                                        {"path", std::move(path)},
                                        {"sources", std::move(sources)},
                                        {"compilerOptions", std::move(options)}},
                                   timeout, cancellation);
    return decode_result<WorkerAnalysisResult>(process_, std::move(result), "analyze");
}

std::vector<dxc::Completion>
WorkerClient::complete(std::string_view root_identity, std::string path, std::uint32_t line,
                       std::uint32_t column, std::chrono::milliseconds timeout,
                       const json_rpc::CancellationToken& cancellation) {
    auto result =
        process_.request("complete", position_params(root_identity, std::move(path), line, column),
                         timeout, cancellation);
    return decode_result<std::vector<dxc::Completion>>(process_, std::move(result), "complete");
}

std::optional<dxc::Definition>
WorkerClient::definition(std::string_view root_identity, std::string path, std::uint32_t line,
                         std::uint32_t column, std::chrono::milliseconds timeout,
                         const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("definition",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::optional<dxc::Definition>>(process_, std::move(result), "definition");
}

std::vector<dxc::Reference>
WorkerClient::references(std::string_view root_identity, std::string path, std::uint32_t line,
                         std::uint32_t column, std::chrono::milliseconds timeout,
                         const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("references",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::vector<dxc::Reference>>(process_, std::move(result), "references");
}

std::optional<dxc::Hover> WorkerClient::hover(std::string_view root_identity, std::string path,
                                              std::uint32_t line, std::uint32_t column,
                                              std::chrono::milliseconds timeout,
                                              const json_rpc::CancellationToken& cancellation) {
    auto result =
        process_.request("hover", position_params(root_identity, std::move(path), line, column),
                         timeout, cancellation);
    return decode_result<std::optional<dxc::Hover>>(process_, std::move(result), "hover");
}

std::optional<dxc::MemoryLayout>
WorkerClient::memory_layout(std::string_view root_identity, std::string path, std::uint32_t line,
                            std::uint32_t column, std::chrono::milliseconds timeout,
                            const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("memoryLayout",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::optional<dxc::MemoryLayout>>(process_, std::move(result),
                                                           "memoryLayout");
}

dxc::CompilationInfo
WorkerClient::compilation_info(std::string_view root_identity, std::chrono::milliseconds timeout,
                               const json_rpc::CancellationToken& cancellation) {
    auto result =
        process_.request("compilationInfo", root_params(root_identity), timeout, cancellation);
    return decode_result<dxc::CompilationInfo>(process_, std::move(result), "compilationInfo");
}

std::vector<dxc::Signature>
WorkerClient::signatures(std::string_view root_identity, std::string path, std::uint32_t line,
                         std::uint32_t column, std::chrono::milliseconds timeout,
                         const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("signatures",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::vector<dxc::Signature>>(process_, std::move(result), "signatures");
}

std::vector<dxc::InlayHint> WorkerClient::inlay_hints(
    std::string_view root_identity, std::string path, std::vector<dxc::SourceOffsetRange> ranges,
    std::vector<dxc::InlayCall> calls, dxc::InlayHintOptions options,
    std::chrono::milliseconds timeout, const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("inlayHints",
                                   Json{{"rootIdentity", root_identity},
                                        {"path", std::move(path)},
                                        {"ranges", std::move(ranges)},
                                        {"calls", std::move(calls)},
                                        {"options", options}},
                                   timeout, cancellation);
    return decode_result<std::vector<dxc::InlayHint>>(process_, std::move(result), "inlayHints");
}

std::vector<dxc::Token> WorkerClient::tokens(std::string_view root_identity, std::string path,
                                             std::chrono::milliseconds timeout,
                                             const json_rpc::CancellationToken& cancellation) {
    auto params = root_params(root_identity);
    params["path"] = std::move(path);
    auto result = process_.request("tokens", std::move(params), timeout, cancellation);
    return decode_result<std::vector<dxc::Token>>(process_, std::move(result), "tokens");
}

std::vector<dxc::SourceRange>
WorkerClient::skipped_ranges(std::string_view root_identity, std::chrono::milliseconds timeout,
                             const json_rpc::CancellationToken& cancellation) {
    auto result =
        process_.request("skippedRanges", root_params(root_identity), timeout, cancellation);
    return decode_result<std::vector<dxc::SourceRange>>(process_, std::move(result),
                                                        "skippedRanges");
}

std::vector<dxc::MacroDefinition>
WorkerClient::macro_definitions(std::string_view root_identity, std::chrono::milliseconds timeout,
                                const json_rpc::CancellationToken& cancellation) {
    auto result =
        process_.request("macroDefinitions", root_params(root_identity), timeout, cancellation);
    return decode_result<std::vector<dxc::MacroDefinition>>(process_, std::move(result),
                                                            "macroDefinitions");
}

std::vector<dxc::Symbol> WorkerClient::symbols(std::string_view root_identity,
                                               std::chrono::milliseconds timeout,
                                               const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("symbols", root_params(root_identity), timeout, cancellation);
    return decode_result<std::vector<dxc::Symbol>>(process_, std::move(result), "symbols");
}

WorkerDocumentSymbolsResult
WorkerClient::document_symbols(std::string_view root_identity, std::string path,
                               std::size_t max_symbols, std::chrono::milliseconds timeout,
                               const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("documentSymbols",
                                   Json{{"rootIdentity", root_identity},
                                        {"path", std::move(path)},
                                        {"maxSymbols", max_symbols}},
                                   timeout, cancellation);
    return decode_result<WorkerDocumentSymbolsResult>(process_, std::move(result),
                                                      "documentSymbols");
}

std::optional<dxc::CallableSymbol>
WorkerClient::callable_at(std::string_view root_identity, std::string path, std::uint32_t line,
                          std::uint32_t column, std::chrono::milliseconds timeout,
                          const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("callableAt",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::optional<dxc::CallableSymbol>>(process_, std::move(result),
                                                             "callableAt");
}

std::vector<dxc::OutgoingCall>
WorkerClient::outgoing_calls(std::string_view root_identity, std::string path, std::uint32_t line,
                             std::uint32_t column, std::chrono::milliseconds timeout,
                             const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("outgoingCalls",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::vector<dxc::OutgoingCall>>(process_, std::move(result),
                                                         "outgoingCalls");
}

std::vector<dxc::IncomingCall>
WorkerClient::incoming_calls(std::string_view root_identity, std::string path, std::uint32_t line,
                             std::uint32_t column, std::chrono::milliseconds timeout,
                             const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("incomingCalls",
                                   position_params(root_identity, std::move(path), line, column),
                                   timeout, cancellation);
    return decode_result<std::vector<dxc::IncomingCall>>(process_, std::move(result),
                                                         "incomingCalls");
}

dxc::EntryPointDataFlow WorkerClient::entry_point_data_flow(
    std::string_view root_identity, dxc::EntryPointDataFlowLimits limits,
    std::chrono::milliseconds timeout, const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("entryPointDataFlow",
                                   Json{{"rootIdentity", root_identity}, {"limits", limits}},
                                   timeout, cancellation);
    return decode_result<dxc::EntryPointDataFlow>(process_, std::move(result),
                                                  "entryPointDataFlow");
}

bool WorkerClient::verify_call_hierarchy_identity(std::string_view root_identity, std::string path,
                                                  std::uint32_t line, std::uint32_t column,
                                                  std::uint32_t expected_start_offset,
                                                  std::uint32_t expected_cursor_kind,
                                                  std::string expected_name,
                                                  std::chrono::milliseconds timeout,
                                                  const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("verifyCallHierarchyIdentity",
                                   Json{{"rootIdentity", root_identity},
                                        {"path", std::move(path)},
                                        {"line", line},
                                        {"column", column},
                                        {"expectedStartOffset", expected_start_offset},
                                        {"expectedCursorKind", expected_cursor_kind},
                                        {"expectedName", std::move(expected_name)}},
                                   timeout, cancellation);
    return decode_result<bool>(process_, std::move(result), "verifyCallHierarchyIdentity");
}

void WorkerClient::erase(std::string_view root_identity, std::chrono::milliseconds timeout,
                         const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("erase", root_params(root_identity), timeout, cancellation);
    if (!result.is_object()) {
        process_.reset();
        throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                 "Analysis worker returned a malformed 'erase' result"};
    }
}

dxc::RuntimeInfo WorkerClient::runtime_info(std::chrono::milliseconds timeout,
                                            const json_rpc::CancellationToken& cancellation) {
    auto result = process_.request("runtimeInfo", Json::object(), timeout, cancellation);
    return decode_result<dxc::RuntimeInfo>(process_, std::move(result), "runtimeInfo");
}

void WorkerClient::reset() noexcept { process_.reset(); }

void WorkerClient::shutdown() noexcept { process_.shutdown(); }

int run_analysis_worker(std::istream& input, std::ostream& output, std::ostream& errors,
                        const dxc::RuntimeConfiguration& runtime) {
    try {
        json_rpc::FrameReader reader{input, analysis_worker_max_payload_size};
        json_rpc::FrameWriter writer{output};
        Worker worker{runtime};
        bool shutdown = false;
        while (!shutdown) {
            const auto payload = reader.read();
            if (!payload) {
                break;
            }

            std::optional<std::uint64_t> id;
            Json reply;
            try {
                const auto request = Json::parse(*payload);
                if (request.is_object()) {
                    if (const auto id_value = request.find("id");
                        id_value != request.end() && id_value->is_number_unsigned()) {
                        id = id_value->get<std::uint64_t>();
                    }
                }
                reply = worker.dispatch(request, shutdown);
            } catch (const std::exception& error) {
                reply = error_response(id, error.what());
            }
            writer.write(reply.dump());
        }
        return 0;
    } catch (const std::exception& error) {
        errors << "HLSL-LSP analysis worker: " << error.what() << '\n';
        return 1;
    }
}

} // namespace hlsl_intellisense::analysis
