#include <hlsl_intellisense/lsp/server.h>

#include <hlsl_intellisense/json_rpc/framing.h>
#include <hlsl_intellisense/workspace/configuration.h>
#include <hlsl_intellisense/workspace/error.h>
#include <hlsl_intellisense/workspace/include_resolver.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hlsl_intellisense::lsp {
namespace {

using json_rpc::HandlerError;
using json_rpc::Json;

[[noreturn]] void invalid_params(std::string_view message) {
    throw HandlerError{json_rpc::invalid_params_code, message};
}

[[nodiscard]] const Json& object_params(const std::optional<Json>& params) {
    if (!params.has_value() || !params->is_object()) {
        invalid_params("Expected object parameters");
    }
    return *params;
}

[[nodiscard]] const Json& member(const Json& object, std::string_view name) {
    const auto item = object.find(name);
    if (item == object.end()) {
        invalid_params(std::string{"Missing parameter: "} + std::string{name});
    }
    return *item;
}

[[nodiscard]] const Json& object_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_object()) {
        invalid_params(std::string{"Expected object: "} + std::string{name});
    }
    return value;
}

[[nodiscard]] std::string string_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_string()) {
        invalid_params(std::string{"Expected string: "} + std::string{name});
    }
    return value.get<std::string>();
}

[[nodiscard]] std::int64_t integer_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    if (!value.is_number_integer()) {
        invalid_params(std::string{"Expected integer: "} + std::string{name});
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] std::uint32_t unsigned_member(const Json& object, std::string_view name) {
    const auto value = integer_member(object, name);
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        invalid_params(std::string{"Expected non-negative 32-bit integer: "} + std::string{name});
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] workspace::Position position(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected position object");
    }
    return {.line = unsigned_member(value, "line"),
            .character = unsigned_member(value, "character")};
}

[[nodiscard]] workspace::Range range(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected range object");
    }
    return {.start = position(object_member(value, "start")),
            .end = position(object_member(value, "end"))};
}

[[nodiscard]] Json lsp_position(workspace::Position value) {
    return {{"line", value.line}, {"character", value.character}};
}

[[nodiscard]] Json lsp_range(workspace::Range value) {
    return {{"start", lsp_position(value.start)}, {"end", lsp_position(value.end)}};
}

[[nodiscard]] int diagnostic_severity(dxc::DiagnosticSeverity severity) {
    switch (severity) {
    case dxc::DiagnosticSeverity::ignored:
    case dxc::DiagnosticSeverity::note:
        return 3;
    case dxc::DiagnosticSeverity::warning:
        return 2;
    case dxc::DiagnosticSeverity::error:
    case dxc::DiagnosticSeverity::fatal:
        return 1;
    }
    return 1;
}

[[nodiscard]] int completion_kind(std::uint32_t cursor_kind) {
    if (cursor_kind == 6) {
        return 5;
    }
    if (cursor_kind == 7) {
        return 20;
    }
    if (cursor_kind == 8 || cursor_kind == 30) {
        return 3;
    }
    if (cursor_kind == 9 || cursor_kind == 10 || cursor_kind == 50) {
        return 6;
    }
    if (cursor_kind == 21 || cursor_kind == 26) {
        return 2;
    }
    if (cursor_kind == 24) {
        return 4;
    }
    if (cursor_kind == 5) {
        return 13;
    }
    if (cursor_kind == 22) {
        return 9;
    }
    if (cursor_kind == 2 || cursor_kind == 3 || cursor_kind == 4 || cursor_kind == 20 ||
        cursor_kind == 27 || cursor_kind == 31 || cursor_kind == 36 || cursor_kind == 43 ||
        cursor_kind == 45) {
        return 7;
    }
    if (cursor_kind == 501 || cursor_kind == 502) {
        return 14;
    }
    return 1;
}

[[nodiscard]] int symbol_kind(std::uint32_t cursor_kind, std::string_view name) {
    if (name.starts_with("operator")) {
        return 25;
    }
    switch (cursor_kind) {
    case 2:
    case 3:
        return 23;
    case 4:
    case 31:
    case 32:
        return 5;
    case 5:
        return 10;
    case 6:
        return 8;
    case 7:
        return 22;
    case 8:
    case 30:
        return 12;
    case 9:
        return 13;
    case 20:
    case 36:
        return 5;
    case 21:
    case 25:
        return 6;
    case 22:
        return 3;
    case 24:
        return 9;
    case 26:
        return 25;
    case 27:
    case 28:
    case 29:
        return 26;
    case 501:
        return 14;
    default:
        return 13;
    }
}

[[nodiscard]] std::string_view symbol_detail(const dxc::Symbol& symbol) {
    switch (symbol_kind(symbol.cursor_kind, symbol.name)) {
    case 3:
        return "HLSL namespace";
    case 5:
        return "HLSL type";
    case 6:
        return "HLSL method";
    case 8:
        return "HLSL field";
    case 9:
        return "HLSL constructor";
    case 10:
        return "HLSL enum";
    case 12:
        return "HLSL function";
    case 14:
        return "HLSL macro";
    case 22:
        return "HLSL enum member";
    case 23:
        return "HLSL struct";
    case 25:
        return "HLSL operator";
    case 26:
        return "HLSL type parameter";
    default:
        return "HLSL variable";
    }
}

[[nodiscard]] bool symbol_is_in_document(const dxc::Symbol& symbol,
                                         const workspace::SourceSnapshot& snapshot) {
    try {
        return workspace::DocumentUri::from_path(symbol.location.path).identity() ==
               snapshot.document_uri().identity();
    } catch (const workspace::DocumentError&) {
        return false;
    }
}

[[nodiscard]] std::size_t symbol_offset(std::string_view text, std::size_t offset,
                                        bool end_offset) {
    offset = (std::min)(offset, text.size());
    if (offset > 0 && offset < text.size() && text[offset - 1] == '\r' && text[offset] == '\n') {
        return end_offset ? offset - 1 : offset + 1;
    }
    return offset;
}

[[nodiscard]] workspace::Range symbol_range(const dxc::Symbol& symbol,
                                            const workspace::SourceSnapshot& snapshot) {
    const auto start =
        symbol_offset(snapshot.text(), static_cast<std::size_t>(symbol.start_offset), false);
    const auto normalized_end = symbol_offset(
        snapshot.text(), (std::max)(static_cast<std::size_t>(symbol.end_offset), start), true);
    const auto end = (std::max)(normalized_end, start);
    return {.start = workspace::lsp_position_at(snapshot.text(), start),
            .end = workspace::lsp_position_at(snapshot.text(), end)};
}

[[nodiscard]] workspace::Range symbol_selection_range(const dxc::Symbol& symbol,
                                                      const workspace::SourceSnapshot& snapshot) {
    const auto text_size = snapshot.text().size();
    const auto start =
        symbol_offset(snapshot.text(), static_cast<std::size_t>(symbol.location.offset), false);
    auto source_offset = start;
    auto name_offset = std::size_t{};
    while (source_offset < text_size && name_offset < symbol.name.size()) {
        if (snapshot.text()[source_offset] == symbol.name[name_offset]) {
            ++source_offset;
            ++name_offset;
        } else if (snapshot.text()[source_offset] == ' ' ||
                   snapshot.text()[source_offset] == '\t') {
            ++source_offset;
        } else {
            break;
        }
    }
    const auto end = symbol_offset(snapshot.text(),
                                   name_offset == symbol.name.size()
                                       ? source_offset
                                       : (std::min)(start + symbol.name.size(), text_size),
                                   true);
    return {.start = workspace::lsp_position_at(snapshot.text(), start),
            .end = workspace::lsp_position_at(snapshot.text(), end)};
}

void append_document_symbols(Json& output, const std::vector<dxc::Symbol>& symbols,
                             const workspace::SourceSnapshot& snapshot) {
    for (const auto& symbol : symbols) {
        Json children = Json::array();
        append_document_symbols(children, symbol.children, snapshot);
        if (!symbol_is_in_document(symbol, snapshot)) {
            output.insert(output.end(), children.begin(), children.end());
            continue;
        }

        Json item{{"name", symbol.name},
                  {"detail", symbol_detail(symbol)},
                  {"kind", symbol_kind(symbol.cursor_kind, symbol.name)},
                  {"range", lsp_range(symbol_range(symbol, snapshot))},
                  {"selectionRange", lsp_range(symbol_selection_range(symbol, snapshot))}};
        if (!children.empty()) {
            item["children"] = std::move(children);
        }
        output.push_back(std::move(item));
    }
}

[[nodiscard]] bool contains_case_insensitive(std::string_view text, std::string_view query) {
    return std::ranges::search(text, query, [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) ==
                      std::tolower(static_cast<unsigned char>(right));
           }).begin() != text.end();
}

[[nodiscard]] std::string_view layout_kind(dxc::MemoryLayoutKind kind) {
    return kind == dxc::MemoryLayoutKind::constant_buffer ? "constantBuffer" : "natural";
}

[[nodiscard]] std::string_view layout_element_kind(dxc::MemoryLayoutElementKind kind) {
    switch (kind) {
    case dxc::MemoryLayoutElementKind::scalar:
        return "scalar";
    case dxc::MemoryLayoutElementKind::vector:
        return "vector";
    case dxc::MemoryLayoutElementKind::matrix:
        return "matrix";
    case dxc::MemoryLayoutElementKind::array:
        return "array";
    case dxc::MemoryLayoutElementKind::record:
        return "record";
    }
    return "scalar";
}

[[nodiscard]] std::string_view resource_register_class(dxc::ResourceRegisterClass register_class) {
    switch (register_class) {
    case dxc::ResourceRegisterClass::cbv:
        return "cbv";
    case dxc::ResourceRegisterClass::srv:
        return "srv";
    case dxc::ResourceRegisterClass::uav:
        return "uav";
    case dxc::ResourceRegisterClass::sampler:
        return "sampler";
    case dxc::ResourceRegisterClass::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string_view resource_usage_status(dxc::ResourceUsageStatus usage) {
    switch (usage) {
    case dxc::ResourceUsageStatus::used:
        return "used";
    case dxc::ResourceUsageStatus::unused:
        return "unused";
    case dxc::ResourceUsageStatus::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string_view
root_signature_availability(dxc::RootSignatureAvailability availability) {
    switch (availability) {
    case dxc::RootSignatureAvailability::present:
        return "present";
    case dxc::RootSignatureAvailability::absent:
        return "absent";
    case dxc::RootSignatureAvailability::not_applicable:
        return "notApplicable";
    case dxc::RootSignatureAvailability::present_details_unavailable:
        return "presentDetailsUnavailable";
    }
    return "absent";
}

[[nodiscard]] std::string_view root_signature_visibility(dxc::RootSignatureVisibility visibility) {
    switch (visibility) {
    case dxc::RootSignatureVisibility::all:
        return "all";
    case dxc::RootSignatureVisibility::vertex:
        return "vertex";
    case dxc::RootSignatureVisibility::hull:
        return "hull";
    case dxc::RootSignatureVisibility::domain:
        return "domain";
    case dxc::RootSignatureVisibility::geometry:
        return "geometry";
    case dxc::RootSignatureVisibility::pixel:
        return "pixel";
    case dxc::RootSignatureVisibility::amplification:
        return "amplification";
    case dxc::RootSignatureVisibility::mesh:
        return "mesh";
    case dxc::RootSignatureVisibility::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string_view root_signature_range_type(dxc::RootSignatureRangeType type) {
    switch (type) {
    case dxc::RootSignatureRangeType::srv:
        return "srv";
    case dxc::RootSignatureRangeType::uav:
        return "uav";
    case dxc::RootSignatureRangeType::cbv:
        return "cbv";
    case dxc::RootSignatureRangeType::sampler:
        return "sampler";
    case dxc::RootSignatureRangeType::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string_view root_signature_parameter_kind(dxc::RootSignatureParameterKind kind) {
    switch (kind) {
    case dxc::RootSignatureParameterKind::descriptor_table:
        return "descriptorTable";
    case dxc::RootSignatureParameterKind::constants:
        return "constants";
    case dxc::RootSignatureParameterKind::root_descriptor:
        return "rootDescriptor";
    }
    return "descriptorTable";
}

[[nodiscard]] std::string_view
resource_compatibility_status(dxc::ResourceCompatibilityStatus status) {
    switch (status) {
    case dxc::ResourceCompatibilityStatus::compatible:
        return "compatible";
    case dxc::ResourceCompatibilityStatus::incompatible:
        return "incompatible";
    case dxc::ResourceCompatibilityStatus::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] Json layout_element_json(const dxc::MemoryLayoutElement& element,
                                       std::uint32_t padding_before) {
    Json members = Json::array();
    std::uint32_t previous_end{};
    for (const auto& member_value : element.members) {
        const auto padding =
            member_value.offset > previous_end ? member_value.offset - previous_end : 0U;
        members.push_back(layout_element_json(member_value, padding));
        previous_end = member_value.offset + member_value.size;
    }
    Json result{{"name", element.name},
                {"type", element.type},
                {"kind", layout_element_kind(element.kind)},
                {"offset", element.offset},
                {"size", element.size},
                {"alignment", element.alignment},
                {"paddingBefore", padding_before},
                {"members", std::move(members)}};
    if (element.array_index.has_value()) {
        result["arrayIndex"] = *element.array_index;
    }
    return result;
}

[[nodiscard]] Json memory_layout_json(const dxc::MemoryLayout& layout) {
    Json members = Json::array();
    std::uint32_t previous_end{};
    for (const auto& member_value : layout.members) {
        const auto padding =
            member_value.offset > previous_end ? member_value.offset - previous_end : 0U;
        members.push_back(layout_element_json(member_value, padding));
        previous_end = member_value.offset + member_value.size;
    }
    Json diagnostics = Json::array();
    if (!layout.explanation.empty()) {
        diagnostics.push_back(layout.explanation);
    }
    Json result{{"name", layout.name},
                {"type", layout.type},
                {"mode", layout_kind(layout.kind)},
                {"size", layout.size},
                {"allocationSize", layout.allocation_size},
                {"alignment", layout.alignment},
                {"diagnostics", std::move(diagnostics)},
                {"members", std::move(members)}};
    return result;
}

[[nodiscard]] Json string_array_json(const std::vector<std::string>& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

[[nodiscard]] Json
compilation_signature_parameter_json(const dxc::CompilationSignatureParameter& parameter) {
    return {
        {"semanticName", parameter.semantic_name},    {"semanticIndex", parameter.semantic_index},
        {"register", parameter.register_index},       {"systemValue", parameter.system_value},
        {"componentType", parameter.component_type},  {"mask", parameter.mask},
        {"readWriteMask", parameter.read_write_mask}, {"stream", parameter.stream}};
}

// Forward-declared: needed here but defined later in this file (with
// `append_semantic_token`), which itself needs types not yet declared this
// early.
[[nodiscard]] std::optional<std::size_t> dxc_offset_at(std::string_view text, std::uint32_t line,
                                                       std::uint32_t column);

// Builds an LSP `{uri, range}` location for a reflected resource's
// declaration site, converting the compiler's 1-based byte line/column into
// a 0-based UTF-16 range spanning the resource's name -- the same
// conversion `Server::definition` already performs for go-to-definition
// results, kept consistent here. `resource_location_texts` maps a source
// path to its full text (open-document buffer if available, otherwise a
// best-effort disk read), resolved once per distinct path by the caller.
// When the text for a location's path could not be resolved at all, the
// byte column is used directly as a best-effort UTF-16 character offset
// (correct for ASCII source, the overwhelming common case for register
// declarations) rather than omitting the location entirely.
[[nodiscard]] Json resource_source_location_json(
    const dxc::SourceLocation& location, const std::string& resource_name,
    const std::unordered_map<std::string, std::string>& resource_location_texts) {
    const auto target = workspace::DocumentUri::from_path(location.path);
    workspace::Position start{.line = location.line > 0 ? location.line - 1 : 0,
                              .character = location.column > 0 ? location.column - 1 : 0};
    const auto text_it = resource_location_texts.find(location.path);
    if (text_it != resource_location_texts.end() && !text_it->second.empty()) {
        if (const auto offset = dxc_offset_at(text_it->second, location.line, location.column)) {
            start = workspace::lsp_position_at(text_it->second, *offset);
        }
    }
    auto end = start;
    const auto name_length = workspace::utf16_length(resource_name);
    if (name_length <= std::numeric_limits<std::uint32_t>::max() - end.character) {
        end.character += static_cast<std::uint32_t>(name_length);
    }
    return {{"uri", target.uri()}, {"range", lsp_range({.start = start, .end = end})}};
}

[[nodiscard]] Json compilation_resource_binding_json(
    const dxc::CompilationResourceBinding& resource,
    const std::unordered_map<std::string, std::string>& resource_location_texts) {
    Json result{{"name", resource.name},
                {"type", resource.type},
                {"bindPoint", resource.bind_point},
                {"bindCount", resource.bind_count},
                {"space", resource.space},
                {"dimension", resource.dimension},
                {"returnType", resource.return_type},
                {"registerClass", resource_register_class(resource.register_class)},
                {"rawFlags", resource.raw_flags},
                {"rangeId", resource.range_id},
                {"sampleCount", resource.sample_count},
                {"unbounded", resource.unbounded},
                {"systemReservedSpace", resource.system_reserved_space},
                {"usage", resource_usage_status(resource.usage)}};
    result["sourceLocation"] =
        resource.source_location.has_value()
            ? resource_source_location_json(*resource.source_location, resource.name,
                                            resource_location_texts)
            : Json(nullptr);
    return result;
}

[[nodiscard]] Json resource_binding_range_json(const dxc::ResourceBindingRange& range) {
    Json result{{"resourceName", range.resource_name},
                {"baseRegister", range.base_register},
                {"unbounded", range.unbounded}};
    result["endRegister"] = range.unbounded ? Json(nullptr) : Json(range.end_register);
    return result;
}

[[nodiscard]] Json resource_binding_collision_json(const dxc::ResourceBindingCollision& collision) {
    return {{"firstResource", collision.first_resource},
            {"secondResource", collision.second_resource},
            {"registerClass", resource_register_class(collision.register_class)},
            {"space", collision.space},
            {"message", collision.message}};
}

[[nodiscard]] Json resource_binding_group_json(const dxc::ResourceBindingGroup& group) {
    Json ranges = Json::array();
    for (const auto& range : group.ranges) {
        ranges.push_back(resource_binding_range_json(range));
    }
    return {{"registerClass", resource_register_class(group.register_class)},
            {"space", group.space},
            {"systemReservedSpace", group.system_reserved_space},
            {"ranges", std::move(ranges)}};
}

[[nodiscard]] Json resource_binding_analysis_json(const dxc::ResourceBindingAnalysis& analysis) {
    Json groups = Json::array();
    for (const auto& group : analysis.groups) {
        groups.push_back(resource_binding_group_json(group));
    }
    Json collisions = Json::array();
    for (const auto& collision : analysis.collisions) {
        collisions.push_back(resource_binding_collision_json(collision));
    }
    return {{"groups", std::move(groups)}, {"collisions", std::move(collisions)}};
}

[[nodiscard]] Json compilation_reflection_json(
    const dxc::CompilationReflection& reflection,
    const std::unordered_map<std::string, std::string>& resource_location_texts) {
    Json input_signature = Json::array();
    for (const auto& parameter : reflection.input_signature) {
        input_signature.push_back(compilation_signature_parameter_json(parameter));
    }
    Json output_signature = Json::array();
    for (const auto& parameter : reflection.output_signature) {
        output_signature.push_back(compilation_signature_parameter_json(parameter));
    }
    Json resources = Json::array();
    for (const auto& resource : reflection.resources) {
        resources.push_back(compilation_resource_binding_json(resource, resource_location_texts));
    }
    Json result{{"available", reflection.available},
                {"unavailableReason", reflection.unavailable_reason},
                {"inputSignature", std::move(input_signature)},
                {"outputSignature", std::move(output_signature)},
                {"resources", std::move(resources)},
                {"bindingAnalysis", resource_binding_analysis_json(reflection.binding_analysis)}};
    if (reflection.thread_group_size.has_value()) {
        result["threadGroupSize"] = Json{{"x", reflection.thread_group_size->x},
                                         {"y", reflection.thread_group_size->y},
                                         {"z", reflection.thread_group_size->z}};
    } else {
        result["threadGroupSize"] = nullptr;
    }
    return result;
}

[[nodiscard]] Json
root_signature_descriptor_range_json(const dxc::RootSignatureDescriptorRange& range) {
    Json result{
        {"type", root_signature_range_type(range.type)},
        {"unbounded", range.unbounded},
        {"baseRegister", range.base_register},
        {"space", range.space},
        {"rawFlags", range.raw_flags},
        {"offsetInDescriptorsFromTableStart", range.offset_in_descriptors_from_table_start}};
    result["numDescriptors"] = range.unbounded ? Json(nullptr) : Json(range.num_descriptors);
    return result;
}

[[nodiscard]] Json
root_signature_root_constants_json(const dxc::RootSignatureRootConstants& constants) {
    return {{"shaderRegister", constants.shader_register},
            {"space", constants.space},
            {"num32BitValues", constants.num_32bit_values}};
}

[[nodiscard]] Json
root_signature_root_descriptor_json(const dxc::RootSignatureRootDescriptor& descriptor) {
    return {{"type", root_signature_range_type(descriptor.type)},
            {"shaderRegister", descriptor.shader_register},
            {"space", descriptor.space},
            {"rawFlags", descriptor.raw_flags}};
}

[[nodiscard]] Json root_signature_parameter_json(const dxc::RootSignatureParameter& parameter) {
    Json descriptor_table_ranges = Json::array();
    for (const auto& range : parameter.descriptor_table_ranges) {
        descriptor_table_ranges.push_back(root_signature_descriptor_range_json(range));
    }
    Json result{{"kind", root_signature_parameter_kind(parameter.kind)},
                {"visibility", root_signature_visibility(parameter.visibility)},
                {"descriptorTableRanges", std::move(descriptor_table_ranges)}};
    result["constants"] = parameter.constants.has_value()
                              ? root_signature_root_constants_json(*parameter.constants)
                              : Json(nullptr);
    result["rootDescriptor"] = parameter.root_descriptor.has_value()
                                   ? root_signature_root_descriptor_json(*parameter.root_descriptor)
                                   : Json(nullptr);
    return result;
}

[[nodiscard]] Json
root_signature_static_sampler_json(const dxc::RootSignatureStaticSampler& sampler) {
    return {{"shaderRegister", sampler.shader_register},
            {"space", sampler.space},
            {"visibility", root_signature_visibility(sampler.visibility)},
            {"filter", sampler.filter},
            {"addressU", sampler.address_u},
            {"addressV", sampler.address_v},
            {"addressW", sampler.address_w},
            {"mipLodBias", sampler.mip_lod_bias},
            {"maxAnisotropy", sampler.max_anisotropy},
            {"comparisonFunc", sampler.comparison_func},
            {"borderColor", sampler.border_color},
            {"minLod", sampler.min_lod},
            {"maxLod", sampler.max_lod}};
}

[[nodiscard]] Json root_signature_details_json(const dxc::RootSignatureDetails& details) {
    Json parameters = Json::array();
    for (const auto& parameter : details.parameters) {
        parameters.push_back(root_signature_parameter_json(parameter));
    }
    Json static_samplers = Json::array();
    for (const auto& sampler : details.static_samplers) {
        static_samplers.push_back(root_signature_static_sampler_json(sampler));
    }
    return {{"version", details.version},
            {"rawFlags", details.raw_flags},
            {"cbvSrvUavHeapDirectlyIndexed", details.cbv_srv_uav_heap_directly_indexed},
            {"samplerHeapDirectlyIndexed", details.sampler_heap_directly_indexed},
            {"parameters", std::move(parameters)},
            {"staticSamplers", std::move(static_samplers)}};
}

[[nodiscard]] Json root_signature_info_json(const dxc::RootSignatureInfo& root_signature) {
    Json result{{"availability", root_signature_availability(root_signature.availability)},
                {"unavailableReason", root_signature.unavailable_reason}};
    result["details"] = root_signature.details.has_value()
                            ? root_signature_details_json(*root_signature.details)
                            : Json(nullptr);
    return result;
}

[[nodiscard]] Json resource_compatibility_issue_json(const dxc::ResourceCompatibilityIssue& issue) {
    return {{"resourceName", issue.resource_name},
            {"registerClass", resource_register_class(issue.register_class)},
            {"space", issue.space},
            {"message", issue.message}};
}

[[nodiscard]] Json
compilation_compatibility_json(const dxc::CompilationCompatibility& compatibility) {
    Json issues = Json::array();
    for (const auto& issue : compatibility.issues) {
        issues.push_back(resource_compatibility_issue_json(issue));
    }
    return {{"status", resource_compatibility_status(compatibility.status)},
            {"explanation", compatibility.explanation},
            {"issues", std::move(issues)}};
}

[[nodiscard]] Json compilation_diagnostics_json(const std::vector<dxc::Diagnostic>& diagnostics) {
    Json result = Json::array();
    for (const auto& diagnostic : diagnostics) {
        result.push_back({{"severity", diagnostic_severity(diagnostic.severity)},
                          {"message", diagnostic.message},
                          {"path", diagnostic.location.path},
                          {"line", diagnostic.location.line},
                          {"column", diagnostic.location.column}});
    }
    return result;
}

[[nodiscard]] Json
compilation_info_json(const dxc::CompilationInfo& info,
                      const std::optional<std::string>& active_variant,
                      const std::unordered_map<std::string, std::string>& resource_location_texts) {
    Json result{{"entryPoint", info.entry_point},
                {"stage", info.stage},
                {"targetProfile", info.target_profile},
                {"languageVersion", info.language_version},
                {"defines", string_array_json(info.defines)},
                {"compilerArguments", string_array_json(info.compiler_arguments)},
                {"includeDirectories", string_array_json(info.include_directories)},
                {"resolvedIncludePaths", string_array_json(info.resolved_include_paths)},
                {"activeVariant", active_variant ? Json(*active_variant) : Json(nullptr)},
                {"success", info.success},
                {"diagnostics", compilation_diagnostics_json(info.diagnostics)}};
    if (info.output.has_value()) {
        result["output"] = Json{{"size", info.output->size}, {"type", info.output->type}};
    } else {
        result["output"] = nullptr;
    }
    if (info.reflection.has_value()) {
        result["reflection"] =
            compilation_reflection_json(*info.reflection, resource_location_texts);
    } else {
        result["reflection"] = nullptr;
    }
    if (info.root_signature.has_value()) {
        result["rootSignature"] = root_signature_info_json(*info.root_signature);
    } else {
        result["rootSignature"] = nullptr;
    }
    if (info.compatibility.has_value()) {
        result["compatibility"] = compilation_compatibility_json(*info.compatibility);
    } else {
        result["compatibility"] = nullptr;
    }
    return result;
}

[[nodiscard]] std::string percent_encode(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            result.push_back(character);
        } else {
            result.push_back('%');
            result.push_back(hexadecimal[byte >> 4]);
            result.push_back(hexadecimal[byte & 0x0F]);
        }
    }
    return result;
}

[[nodiscard]] std::string memory_layout_command(std::string_view uri,
                                                workspace::Position position_value) {
    const Json arguments = Json::array(
        {Json{{"textDocument", {{"uri", uri}}}, {"position", lsp_position(position_value)}}});
    return "command:hlsl.showMemoryLayout?" + percent_encode(arguments.dump());
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void append_workspace_symbols(Json& output, const std::vector<dxc::Symbol>& symbols,
                              const workspace::SourceSnapshot& snapshot, std::string_view query,
                              std::string_view container) {
    for (const auto& symbol : symbols) {
        if (symbol_is_in_document(symbol, snapshot) &&
            contains_case_insensitive(symbol.name, query)) {
            auto container_name = std::string{"HLSL"};
            if (!container.empty()) {
                container_name += " \xC2\xB7 ";
                container_name += container;
            }
            output.push_back({{"name", symbol.name},
                              {"kind", symbol_kind(symbol.cursor_kind, symbol.name)},
                              {"location",
                               {{"uri", snapshot.uri()},
                                {"range", lsp_range(symbol_selection_range(symbol, snapshot))}}},
                              {"containerName", std::move(container_name)}});
        }

        auto nested_container = std::string{container};
        if (symbol_is_in_document(symbol, snapshot)) {
            if (!nested_container.empty()) {
                nested_container += "::";
            }
            nested_container += symbol.name;
        }
        append_workspace_symbols(output, symbol.children, snapshot, query, nested_container);
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

enum class SemanticTokenType : std::uint8_t {
    namespace_name,
    type,
    class_name,
    enum_name,
    parameter,
    variable,
    property,
    enum_member,
    function,
    method,
    macro,
    keyword,
    comment,
    string,
    number,
    type_parameter
};

struct SemanticToken {
    workspace::Position start;
    std::uint32_t length{};
    SemanticTokenType type{};
};

[[nodiscard]] SemanticTokenType identifier_token_type(std::uint32_t cursor_kind) {
    if (cursor_kind == 22 || cursor_kind == 33 || cursor_kind == 46) {
        return SemanticTokenType::namespace_name;
    }
    if (cursor_kind == 2 || cursor_kind == 3 || cursor_kind == 4 || cursor_kind == 31 ||
        cursor_kind == 32) {
        return SemanticTokenType::class_name;
    }
    if (cursor_kind == 5) {
        return SemanticTokenType::enum_name;
    }
    if (cursor_kind == 6 || cursor_kind == 47 || cursor_kind == 102) {
        return SemanticTokenType::property;
    }
    if (cursor_kind == 7) {
        return SemanticTokenType::enum_member;
    }
    if (cursor_kind == 8 || cursor_kind == 30 || cursor_kind == 103) {
        return SemanticTokenType::function;
    }
    if (cursor_kind == 21 || cursor_kind == 24 || cursor_kind == 25 || cursor_kind == 26) {
        return SemanticTokenType::method;
    }
    if (cursor_kind == 10 || cursor_kind == 28) {
        return SemanticTokenType::parameter;
    }
    if (cursor_kind == 20 || cursor_kind == 36 || cursor_kind == 43 || cursor_kind == 45) {
        return SemanticTokenType::type;
    }
    if (cursor_kind == 27 || cursor_kind == 29) {
        return SemanticTokenType::type_parameter;
    }
    if (cursor_kind == 501 || cursor_kind == 502) {
        return SemanticTokenType::macro;
    }
    return SemanticTokenType::variable;
}

[[nodiscard]] SemanticTokenType semantic_token_type(const dxc::Token& token, std::string_view text,
                                                    std::size_t offset) {
    switch (token.kind) {
    case dxc::TokenKind::keyword:
        return SemanticTokenType::keyword;
    case dxc::TokenKind::built_in_type:
        return SemanticTokenType::type;
    case dxc::TokenKind::comment:
        return SemanticTokenType::comment;
    case dxc::TokenKind::literal:
        return offset < text.size() && (text[offset] == '"' || text[offset] == '\'')
                   ? SemanticTokenType::string
                   : SemanticTokenType::number;
    case dxc::TokenKind::identifier:
        return identifier_token_type(token.cursor_kind);
    case dxc::TokenKind::punctuation:
    case dxc::TokenKind::unknown:
        return SemanticTokenType::variable;
    }
    return SemanticTokenType::variable;
}

[[nodiscard]] std::optional<std::size_t> dxc_offset_at(std::string_view text, std::uint32_t line,
                                                       std::uint32_t column) {
    if (line == 0 || column == 0) {
        return std::nullopt;
    }
    std::size_t line_start = 0;
    for (std::uint32_t current = 1; current < line; ++current) {
        const auto newline = text.find_first_of("\r\n", line_start);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        line_start = newline + 1;
        if (text[newline] == '\r' && line_start < text.size() && text[line_start] == '\n') {
            ++line_start;
        }
    }
    const auto offset = line_start + column - 1;
    const auto line_end = text.find_first_of("\r\n", line_start);
    if (offset > (line_end == std::string_view::npos ? text.size() : line_end)) {
        return std::nullopt;
    }
    return offset;
}

void append_semantic_token(std::vector<SemanticToken>& result, std::string_view text,
                           const dxc::Token& token) {
    const auto token_offset = dxc_offset_at(text, token.line, token.column);
    if (!token_offset.has_value() || *token_offset >= text.size() ||
        token.kind == dxc::TokenKind::punctuation || token.kind == dxc::TokenKind::unknown) {
        return;
    }

    auto token_end = std::min(text.size(), *token_offset + token.length);
    if (token.kind == dxc::TokenKind::literal &&
        (text[*token_offset] == '"' || text[*token_offset] == '\'')) {
        const auto quote = text[*token_offset];
        for (auto offset = *token_offset + 1; offset < text.size(); ++offset) {
            if (text[offset] == '\\') {
                ++offset;
            } else if (text[offset] == quote) {
                token_end = offset + 1;
                break;
            }
        }
    }
    const auto type = semantic_token_type(token, text, *token_offset);
    for (auto segment_start = *token_offset; segment_start < token_end;) {
        const auto newline = text.find_first_of("\r\n", segment_start);
        const auto segment_end =
            newline == std::string_view::npos ? token_end : std::min(token_end, newline);
        if (segment_end > segment_start) {
            const auto start = workspace::lsp_position_at(text, segment_start);
            const auto length =
                workspace::utf16_length(text.substr(segment_start, segment_end - segment_start));
            if (length <= std::numeric_limits<std::uint32_t>::max()) {
                result.push_back(
                    {.start = start, .length = static_cast<std::uint32_t>(length), .type = type});
            }
        }
        if (segment_end == token_end) {
            break;
        }
        segment_start = segment_end + 1;
        if (text[segment_end] == '\r' && segment_start < token_end && text[segment_start] == '\n') {
            ++segment_start;
        }
    }
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
dxc_position(std::string_view text, workspace::Position request_position) {
    const auto offset = workspace::utf8_offset_at(text, request_position);
    std::size_t line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n' && text[line_start - 1] != '\r') {
        --line_start;
    }
    const auto byte_column = offset - line_start + 1;
    if (byte_column > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Completion position is too large");
    }
    return {request_position.line + 1, static_cast<std::uint32_t>(byte_column)};
}

struct CallContext {
    std::size_t callee_offset{};
    std::size_t active_parameter{};
};

enum class LexicalState : std::uint8_t { code, line_comment, block_comment, string, character };

struct LexicalPrefix {
    std::vector<bool> code;
    LexicalState state{LexicalState::code};
};

[[nodiscard]] LexicalPrefix lexical_prefix(std::string_view text, std::size_t limit) {
    LexicalPrefix result{.code = std::vector<bool>(limit, false)};
    for (std::size_t offset = 0; offset < limit;) {
        const auto character = text[offset];
        switch (result.state) {
        case LexicalState::code:
            if (character == '/' && offset + 1 < limit && text[offset + 1] == '/') {
                result.state = LexicalState::line_comment;
                offset += 2;
            } else if (character == '/' && offset + 1 < limit && text[offset + 1] == '*') {
                result.state = LexicalState::block_comment;
                offset += 2;
            } else if (character == '"') {
                result.state = LexicalState::string;
                ++offset;
            } else if (character == '\'') {
                result.state = LexicalState::character;
                ++offset;
            } else {
                result.code[offset] = true;
                ++offset;
            }
            break;
        case LexicalState::line_comment:
            if (character == '\r' || character == '\n') {
                result.state = LexicalState::code;
                result.code[offset] = true;
            }
            ++offset;
            break;
        case LexicalState::block_comment:
            if (character == '*' && offset + 1 < limit && text[offset + 1] == '/') {
                result.state = LexicalState::code;
                offset += 2;
            } else {
                ++offset;
            }
            break;
        case LexicalState::string:
        case LexicalState::character: {
            const auto quote = result.state == LexicalState::string ? '"' : '\'';
            if (character == '\\' && offset + 1 < limit) {
                offset += 2;
            } else {
                ++offset;
                if (character == quote) {
                    result.state = LexicalState::code;
                }
            }
            break;
        }
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t>
previous_code_offset(std::string_view text, const std::vector<bool>& code, std::size_t offset) {
    while (offset > 0) {
        --offset;
        if (code[offset] && std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
            return offset;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool template_close_follows(std::string_view text, const std::vector<bool>& code,
                                          std::size_t open) {
    std::size_t depth = 1;
    for (auto offset = open + 1; offset < code.size(); ++offset) {
        if (!code[offset]) {
            continue;
        }
        if (text[offset] == '<') {
            ++depth;
        } else if (text[offset] == '>') {
            if (--depth == 0) {
                auto next = offset + 1;
                while (next < code.size() &&
                       (!code[next] || std::isspace(static_cast<unsigned char>(text[next])) != 0)) {
                    ++next;
                }
                return next < code.size() && text[next] == '(';
            }
        } else if ((text[offset] == ';' || text[offset] == ')' || text[offset] == ']' ||
                    text[offset] == '}') &&
                   depth == 1) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool template_open(std::string_view text, const std::vector<bool>& code,
                                 std::size_t offset) {
    const auto previous = previous_code_offset(text, code, offset);
    if (!previous.has_value()) {
        return false;
    }
    const auto character = text[*previous];
    const auto possible_name = std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                               character == '_' || character == '>' || character == ']';
    return possible_name && template_close_follows(text, code, offset);
}

[[nodiscard]] std::optional<std::size_t>
callee_at(std::string_view text, const std::vector<bool>& code, std::size_t open_parenthesis) {
    auto previous = previous_code_offset(text, code, open_parenthesis);
    if (!previous.has_value()) {
        return std::nullopt;
    }
    if (text[*previous] == '>') {
        std::size_t depth = 1;
        auto offset = *previous;
        while (offset > 0 && depth != 0) {
            --offset;
            if (!code[offset]) {
                continue;
            }
            if (text[offset] == '>') {
                ++depth;
            } else if (text[offset] == '<') {
                --depth;
            }
        }
        if (depth != 0) {
            return std::nullopt;
        }
        previous = previous_code_offset(text, code, offset);
        if (!previous.has_value()) {
            return std::nullopt;
        }
    }

    auto start = *previous;
    const auto identifier_character = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
    };
    if (!identifier_character(text[start])) {
        return std::nullopt;
    }
    while (start > 0 && code[start - 1] && identifier_character(text[start - 1])) {
        --start;
    }
    const auto name = text.substr(start, *previous - start + 1);
    static constexpr std::string_view non_call_keywords[] = {"if",     "for",     "while", "switch",
                                                             "sizeof", "alignof", "return"};
    if (std::ranges::find(non_call_keywords, name) != std::ranges::end(non_call_keywords)) {
        return std::nullopt;
    }
    return start;
}

[[nodiscard]] std::optional<CallContext> call_context(std::string_view text,
                                                      std::size_t cursor_offset) {
    const auto lexical = lexical_prefix(text, cursor_offset);
    if (lexical.state != LexicalState::code) {
        return std::nullopt;
    }

    struct Delimiter {
        char value{};
        std::optional<std::size_t> callee;
        std::size_t active_parameter{};
    };
    std::vector<Delimiter> delimiters;
    for (std::size_t offset = 0; offset < cursor_offset; ++offset) {
        if (!lexical.code[offset]) {
            continue;
        }
        const auto character = text[offset];
        if (character == '(') {
            delimiters.push_back({.value = character,
                                  .callee = callee_at(text, lexical.code, offset),
                                  .active_parameter = 0});
        } else if (character == '[' || character == '{' ||
                   (character == '<' && ((!delimiters.empty() && delimiters.back().value == '<') ||
                                         template_open(text, lexical.code, offset)))) {
            delimiters.push_back(
                {.value = character, .callee = std::nullopt, .active_parameter = 0});
        } else if (character == ')' || character == ']' || character == '}' || character == '>') {
            const auto expected =
                character == ')' ? '(' : (character == ']' ? '[' : (character == '}' ? '{' : '<'));
            const auto matching = std::ranges::find(delimiters.rbegin(), delimiters.rend(),
                                                    expected, &Delimiter::value);
            if (matching != delimiters.rend()) {
                delimiters.erase(matching.base() - 1, delimiters.end());
            }
        } else if (character == ',' && !delimiters.empty() && delimiters.back().value == '(' &&
                   delimiters.back().callee.has_value()) {
            ++delimiters.back().active_parameter;
        }
    }

    const auto call =
        std::ranges::find_if(delimiters.rbegin(), delimiters.rend(), [](const auto& delimiter) {
            return delimiter.value == '(' && delimiter.callee.has_value();
        });
    if (call == delimiters.rend()) {
        return std::nullopt;
    }
    return CallContext{.callee_offset = *call->callee, .active_parameter = call->active_parameter};
}

[[nodiscard]] workspace::Range diagnostic_range(const workspace::SourceSnapshot& snapshot,
                                                const dxc::Diagnostic& diagnostic) {
    if (diagnostic.location.line == 0 || diagnostic.location.column == 0) {
        return {};
    }

    std::size_t line_start = 0;
    for (std::uint32_t line = 1; line < diagnostic.location.line; ++line) {
        const auto newline = snapshot.text().find('\n', line_start);
        if (newline == std::string::npos) {
            return {};
        }
        line_start = newline + 1;
    }
    const auto line_end = snapshot.text().find_first_of("\r\n", line_start);
    const auto end_offset = line_end == std::string::npos ? snapshot.text().size() : line_end;
    const auto byte_column = static_cast<std::size_t>(diagnostic.location.column - 1);
    if (byte_column > end_offset - line_start) {
        return {};
    }

    const auto offset = line_start + byte_column;
    try {
        const auto start = workspace::lsp_position_at(snapshot.text(), offset);
        auto end = start;
        if (offset < snapshot.text().size() && snapshot.text()[offset] != '\r' &&
            snapshot.text()[offset] != '\n') {
            const auto first = static_cast<unsigned char>(snapshot.text()[offset]);
            const std::size_t bytes =
                first < 0x80 ? 1 : (first < 0xE0 ? 2 : (first < 0xF0 ? 3 : 4));
            end = workspace::lsp_position_at(snapshot.text(), offset + bytes);
        }
        return {.start = start, .end = end};
    } catch (const workspace::DocumentError&) {
        return {};
    }
}

[[nodiscard]] bool same_document_path(std::string_view left, std::string_view right) {
    try {
        return workspace::DocumentUri::from_path(left).identity() ==
               workspace::DocumentUri::from_path(right).identity();
    } catch (const workspace::DocumentError&) {
        return left == right;
    }
}

[[nodiscard]] std::string workspace_folder_identity(const Json& folder) {
    try {
        return workspace::DocumentUri::from_uri(string_member(folder, "uri")).identity();
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }
}

[[nodiscard]] std::pair<std::string, std::filesystem::path> workspace_folder(const Json& folder) {
    try {
        auto uri = workspace::DocumentUri::from_uri(string_member(folder, "uri"));
        return {uri.identity(), std::filesystem::path{uri.path()}};
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }
}

[[nodiscard]] const Json* setting(const Json& settings, const Json* hlsl, std::string_view name) {
    if (hlsl != nullptr) {
        const auto nested = hlsl->find(name);
        if (nested != hlsl->end()) {
            return &*nested;
        }
    }
    const auto dotted = settings.find("hlsl." + std::string{name});
    return dotted == settings.end() ? nullptr : &*dotted;
}

[[nodiscard]] std::string setting_value(std::string_view name, const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number() || value.is_boolean()) {
        return value.dump();
    }
    invalid_params("hlsl." + std::string{name} + " values must be strings, numbers, or booleans");
}

[[nodiscard]] std::optional<std::optional<std::string>>
optional_string_setting(const Json& settings, const Json* hlsl, std::string_view name) {
    const auto* value = setting(settings, hlsl, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_null()) {
        return std::optional<std::optional<std::string>>{std::in_place, std::nullopt};
    }
    if (!value->is_string()) {
        invalid_params("hlsl." + std::string{name} + " must be a string or null");
    }
    return std::optional<std::optional<std::string>>{std::in_place, value->get<std::string>()};
}

// Produces a stable comparison key for a runtime directory. The key is the
// normalized absolute path, lowered on Windows so case-only differences do not
// look like a runtime change. An empty directory keys to the bundled default.
[[nodiscard]] std::string runtime_directory_key(const std::string& directory) {
    if (directory.empty()) {
        return {};
    }
    std::error_code error;
    const auto normalized =
        std::filesystem::absolute(std::filesystem::path{directory}, error).lexically_normal();
    std::string key = error ? directory : normalized.string();
#ifdef _WIN32
    std::ranges::transform(key, key.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
#endif
    return key;
}

[[nodiscard]] workspace::ConfigurationOverrides configuration_overrides(const Json& settings) {
    if (!settings.is_object()) {
        invalid_params("settings must be an object");
    }
    const Json* hlsl = nullptr;
    if (const auto nested = settings.find("hlsl"); nested != settings.end()) {
        if (!nested->is_object()) {
            invalid_params("hlsl settings must be an object");
        }
        hlsl = &*nested;
    }

    workspace::ConfigurationOverrides result;
    if (const auto* definitions = setting(settings, hlsl, "preprocessorDefinitions")) {
        if (!definitions->is_object()) {
            invalid_params("hlsl.preprocessorDefinitions must be an object");
        }
        std::map<std::string, std::string, std::less<>> values;
        for (const auto& [name, value] : definitions->items()) {
            if (name.empty()) {
                invalid_params("Preprocessor definition names must not be empty");
            }
            values.emplace(name, setting_value("preprocessorDefinitions", value));
        }
        result.preprocessor_definitions = std::move(values);
    }

    const auto path_array =
        [&](std::string_view name) -> std::optional<std::vector<std::filesystem::path>> {
        const auto* value = setting(settings, hlsl, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_array()) {
            invalid_params("hlsl." + std::string{name} + " must be an array of strings");
        }
        std::vector<std::filesystem::path> paths;
        paths.reserve(value->size());
        for (const auto& path : *value) {
            if (!path.is_string()) {
                invalid_params("hlsl." + std::string{name} + " must be an array of strings");
            }
            paths.emplace_back(path.get_ref<const std::string&>());
        }
        return paths;
    };
    result.additional_include_directories = path_array("additionalIncludeDirectories");

    if (const auto* mappings = setting(settings, hlsl, "virtualDirectoryMappings")) {
        if (!mappings->is_object()) {
            invalid_params("hlsl.virtualDirectoryMappings must be an object of string paths");
        }
        std::map<std::string, std::filesystem::path, std::less<>> values;
        for (const auto& [virtual_directory, real_directory] : mappings->items()) {
            if (!real_directory.is_string()) {
                invalid_params("hlsl.virtualDirectoryMappings must be an object of string paths");
            }
            values.emplace(virtual_directory, real_directory.get_ref<const std::string&>());
        }
        result.virtual_directory_mappings = std::move(values);
    }

    result.language_version = optional_string_setting(settings, hlsl, "languageVersion");
    result.target_profile = optional_string_setting(settings, hlsl, "targetProfile");
    result.entry_point = optional_string_setting(settings, hlsl, "entryPoint");

    if (const auto* arguments = setting(settings, hlsl, "additionalArguments")) {
        if (!arguments->is_array()) {
            invalid_params("hlsl.additionalArguments must be an array of strings");
        }
        std::vector<std::string> values;
        values.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            if (!argument.is_string()) {
                invalid_params("hlsl.additionalArguments must be an array of strings");
            }
            values.push_back(argument.get<std::string>());
        }
        result.additional_arguments = std::move(values);
    }

    if (const auto runtime = optional_string_setting(settings, hlsl, "dxcRuntimeDirectory")) {
        const auto& value = *runtime;
        if (value && !value->empty()) {
            result.dxc_runtime_directory.emplace(std::filesystem::path{*value});
        } else {
            result.dxc_runtime_directory.emplace(std::nullopt);
        }
    }
    return result;
}

[[nodiscard]] bool valid_rename_identifier(std::string_view value) {
    if (value.empty() ||
        (std::isalpha(static_cast<unsigned char>(value.front())) == 0 && value.front() != '_') ||
        !std::ranges::all_of(value.substr(1), [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
        })) {
        return false;
    }
    static constexpr std::string_view keywords[] = {"bool",
                                                    "break",
                                                    "Buffer",
                                                    "ByteAddressBuffer",
                                                    "case",
                                                    "catch",
                                                    "cbuffer",
                                                    "centroid",
                                                    "char",
                                                    "class",
                                                    "column_major",
                                                    "const",
                                                    "continue",
                                                    "default",
                                                    "delete",
                                                    "do",
                                                    "double",
                                                    "else",
                                                    "enum",
                                                    "explicit",
                                                    "extern",
                                                    "false",
                                                    "float",
                                                    "for",
                                                    "friend",
                                                    "globallycoherent",
                                                    "goto",
                                                    "groupshared",
                                                    "half",
                                                    "if",
                                                    "in",
                                                    "inline",
                                                    "inout",
                                                    "int",
                                                    "linear",
                                                    "long",
                                                    "matrix",
                                                    "namespace",
                                                    "new",
                                                    "nointerpolation",
                                                    "noperspective",
                                                    "operator",
                                                    "out",
                                                    "packoffset",
                                                    "precise",
                                                    "private",
                                                    "protected",
                                                    "public",
                                                    "register",
                                                    "return",
                                                    "row_major",
                                                    "RWBuffer",
                                                    "RWByteAddressBuffer",
                                                    "RWStructuredBuffer",
                                                    "sample",
                                                    "SamplerComparisonState",
                                                    "SamplerState",
                                                    "short",
                                                    "signed",
                                                    "sizeof",
                                                    "snorm",
                                                    "static",
                                                    "StructuredBuffer",
                                                    "struct",
                                                    "switch",
                                                    "tbuffer",
                                                    "template",
                                                    "Texture1D",
                                                    "Texture1DArray",
                                                    "Texture2D",
                                                    "Texture2DArray",
                                                    "Texture2DMS",
                                                    "Texture2DMSArray",
                                                    "Texture3D",
                                                    "TextureCube",
                                                    "TextureCubeArray",
                                                    "this",
                                                    "throw",
                                                    "true",
                                                    "try",
                                                    "typedef",
                                                    "typename",
                                                    "uint",
                                                    "uniform",
                                                    "union",
                                                    "unorm",
                                                    "unsigned",
                                                    "using",
                                                    "vector",
                                                    "virtual",
                                                    "void",
                                                    "volatile",
                                                    "while"};
    if (std::ranges::find(keywords, value) != std::ranges::end(keywords)) {
        return false;
    }
    static constexpr std::string_view scalar_types[] = {
        "bool",       "double",   "dword",      "float",    "half",      "int",
        "min10float", "min12int", "min16float", "min16int", "min16uint", "uint"};
    for (const auto scalar : scalar_types) {
        if (!value.starts_with(scalar)) {
            continue;
        }
        auto suffix = value.substr(scalar.size());
        if (suffix.empty()) {
            return false;
        }
        const auto dimension = [](char character) { return character >= '1' && character <= '4'; };
        if (suffix.size() == 1 && dimension(suffix[0])) {
            return false;
        }
        if (suffix.size() == 3 && dimension(suffix[0]) && suffix[1] == 'x' &&
            dimension(suffix[2])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] workspace::Range reference_range(std::string_view text,
                                               const dxc::Reference& reference) {
    const auto start = static_cast<std::size_t>(reference.start_offset);
    const auto end = static_cast<std::size_t>(reference.end_offset);
    if (start > end || end > text.size()) {
        throw HandlerError{json_rpc::content_modified_code,
                           "Reference source changed after analysis"};
    }
    return {.start = workspace::lsp_position_at(text, start),
            .end = workspace::lsp_position_at(text, end)};
}

} // namespace

struct Server::ReferenceResult final {
    workspace::SourceSnapshot request;
    dxc::Definition target;
    std::vector<dxc::Reference> references;
};

Server::Server(NotificationSender sender, Logger logger, ServerOptions options)
    : sender_{std::move(sender)}, logger_{std::move(logger)}, options_{std::move(options)},
      analysis_{[this](const auto& snapshot, const auto& diagnostics, std::uint64_t generation) {
                    analysis_completed(snapshot, diagnostics, generation);
                },
                options_.analysis, options_.analysis_hooks,
                [this](std::string_view message) { log(message); }} {
    if (!sender_) {
        throw std::invalid_argument{"The LSP server requires a notification sender"};
    }
    register_handlers();
}

Server::~Server() {
    cancel_all_requests();
    analysis_.shutdown();
}

void Server::register_handlers() {
    dispatcher_.register_request_handler("initialize",
                                         [this](const auto& params) { return initialize(params); });
    dispatcher_.register_request_handler("shutdown",
                                         [this](const auto& params) { return shutdown(params); });
    dispatcher_.register_request_handler(
        "textDocument/completion",
        [this](const auto& params, const auto& context) { return completion(params, context); });
    dispatcher_.register_request_handler(
        "textDocument/definition",
        [this](const auto& params, const auto& context) { return definition(params, context); });
    dispatcher_.register_request_handler(
        "textDocument/references",
        [this](const auto& params, const auto& context) { return references(params, context); });
    dispatcher_.register_request_handler("textDocument/prepareRename",
                                         [this](const auto& params, const auto& context) {
                                             return prepare_rename(params, context);
                                         });
    dispatcher_.register_request_handler(
        "textDocument/rename",
        [this](const auto& params, const auto& context) { return rename(params, context); });
    dispatcher_.register_request_handler(
        "textDocument/hover",
        [this](const auto& params, const auto& context) { return hover(params, context); });
    dispatcher_.register_request_handler(
        "hlsl/memoryLayout",
        [this](const auto& params, const auto& context) { return memory_layout(params, context); });
    dispatcher_.register_request_handler("hlsl/compilationInfo",
                                         [this](const auto& params, const auto& context) {
                                             return compilation_info(params, context);
                                         });
    dispatcher_.register_request_handler("textDocument/signatureHelp",
                                         [this](const auto& params, const auto& context) {
                                             return signature_help(params, context);
                                         });
    dispatcher_.register_request_handler("textDocument/documentSymbol",
                                         [this](const auto& params, const auto& context) {
                                             return document_symbols(params, context);
                                         });
    dispatcher_.register_request_handler("workspace/symbol",
                                         [this](const auto& params, const auto& context) {
                                             return workspace_symbols(params, context);
                                         });
    dispatcher_.register_request_handler(
        "hlsl/dxcRuntime", [this](const auto& params) { return dxc_runtime(params); });
    dispatcher_.register_request_handler("hlsl/variants",
                                         [this](const auto& params) { return variants(params); });
    if (options_.semantic_tokens) {
        dispatcher_.register_request_handler("textDocument/semanticTokens/full",
                                             [this](const auto& params, const auto& context) {
                                                 return semantic_tokens(params, context);
                                             });
    }
    dispatcher_.register_notification_handler("initialized",
                                              [this](const auto& params) { initialized(params); });
    dispatcher_.register_notification_handler("textDocument/didOpen",
                                              [this](const auto& params) { did_open(params); });
    dispatcher_.register_notification_handler("textDocument/didChange",
                                              [this](const auto& params) { did_change(params); });
    dispatcher_.register_notification_handler("textDocument/didSave",
                                              [this](const auto& params) { did_save(params); });
    dispatcher_.register_notification_handler("textDocument/didClose",
                                              [this](const auto& params) { did_close(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeConfiguration",
        [this](const auto& params) { did_change_configuration(params); });
    dispatcher_.register_notification_handler(
        "hlsl/didChangeClientDefaults",
        [this](const auto& params) { did_change_client_defaults(params); });
    dispatcher_.register_notification_handler(
        "hlsl/didChangeActiveVariant",
        [this](const auto& params) { did_change_active_variant(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeWorkspaceFolders",
        [this](const auto& params) { did_change_workspace_folders(params); });
    dispatcher_.register_notification_handler(
        "workspace/didChangeWatchedFiles",
        [this](const auto& params) { did_change_watched_files(params); });
    dispatcher_.register_notification_handler("exit", [this](const auto& params) { exit(params); });
}

std::optional<json_rpc::DispatchResponse> Server::handle(const json_rpc::Message& message) {
    if (const auto* request = std::get_if<json_rpc::Request>(&message)) {
        return handle(*request, dispatcher_.begin_request(request->id));
    }
    return dispatcher_.dispatch(message);
}

json_rpc::DispatchResponse Server::handle(const json_rpc::Request& request,
                                          const json_rpc::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        dispatcher_.finish_request(request.id, cancellation);
        return json_rpc::ErrorResponse{.id = request.id,
                                       .error = {.code = json_rpc::request_cancelled_code,
                                                 .message = "Request cancelled",
                                                 .data = std::nullopt}};
    }
    {
        std::scoped_lock lock{state_mutex_};
        if (state_ == State::uninitialized && request.method != "initialize") {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::awaiting_initialized) {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = -32002,
                                                     .message = "Server not initialized",
                                                     .data = std::nullopt}};
        }
        if (state_ == State::shutdown) {
            dispatcher_.finish_request(request.id, cancellation);
            return json_rpc::ErrorResponse{.id = request.id,
                                           .error = {.code = json_rpc::invalid_request_code,
                                                     .message = "Server has shut down",
                                                     .data = std::nullopt}};
        }
    }
    auto response = dispatcher_.dispatch(request, cancellation);
    dispatcher_.finish_request(request.id, cancellation);
    return response;
}

json_rpc::CancellationToken Server::begin_request(const json_rpc::RequestId& id) const {
    return dispatcher_.begin_request(id);
}

void Server::finish_request(const json_rpc::RequestId& id,
                            const json_rpc::CancellationToken& cancellation) const noexcept {
    dispatcher_.finish_request(id, cancellation);
}

void Server::cancel_all_requests() const noexcept { dispatcher_.cancel_all(); }

void Server::wait_for_analysis() { analysis_.wait_idle(); }

analysis::AnalysisMetrics Server::analysis_metrics() const noexcept { return analysis_.metrics(); }

bool Server::exit_requested() const noexcept {
    return exit_requested_.load(std::memory_order_acquire);
}

int Server::exit_code() const noexcept { return clean_shutdown_ ? 0 : 1; }

Json Server::initialize(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::uninitialized) {
        throw HandlerError{json_rpc::invalid_request_code, "Initialize may only be requested once"};
    }
    const auto& value = object_params(params);
    std::optional<std::string> client_default_language_version;
    std::optional<std::string> initial_active_variant;
    bool client_command_links = false;
    if (const auto initialization_options = value.find("initializationOptions");
        initialization_options != value.end() && !initialization_options->is_null()) {
        const auto defaults = configuration_overrides(*initialization_options);
        if (defaults.language_version) {
            client_default_language_version = *defaults.language_version;
        }
        if (const auto hlsl = initialization_options->find("hlsl");
            hlsl != initialization_options->end() && hlsl->is_object()) {
            if (const auto variant = hlsl->find("activeVariant");
                variant != hlsl->end() && variant->is_string() &&
                !variant->get_ref<const std::string&>().empty()) {
                initial_active_variant = variant->get<std::string>();
            }
        }
        if (const auto links = initialization_options->find("commandLinks");
            links != initialization_options->end() && links->is_boolean()) {
            client_command_links = links->get<bool>();
        }
    }
    std::unordered_map<std::string, std::filesystem::path> workspace_folders;
    if (const auto folders = value.find("workspaceFolders");
        folders != value.end() && !folders->is_null()) {
        if (!folders->is_array()) {
            invalid_params("workspaceFolders must be an array or null");
        }
        for (const auto& folder : *folders) {
            const auto [identity, path] = workspace_folder(folder);
            workspace_folders.insert_or_assign(identity, path);
        }
    } else if (const auto root_uri = value.find("rootUri");
               root_uri != value.end() && !root_uri->is_null()) {
        if (!root_uri->is_string()) {
            invalid_params("rootUri must be a string or null");
        }
        try {
            auto uri = workspace::DocumentUri::from_uri(root_uri->get_ref<const std::string&>());
            workspace_folders.insert_or_assign(uri.identity(), std::filesystem::path{uri.path()});
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }
    workspace_folders_ = std::move(workspace_folders);
    client_default_language_version_ = std::move(client_default_language_version);
    active_variant_ = std::move(initial_active_variant);
    command_links_ = client_command_links;
    state_ = State::awaiting_initialized;
    Json capabilities = {
        {"positionEncoding", "utf-16"},
        {"textDocumentSync",
         {{"openClose", true}, {"change", 2}, {"save", {{"includeText", true}}}}},
        {"completionProvider", {{"resolveProvider", false}}},
        {"definitionProvider", true},
        {"referencesProvider", true},
        {"renameProvider", {{"prepareProvider", true}}},
        {"hoverProvider", true},
        {"signatureHelpProvider",
         {{"triggerCharacters", Json::array({"(", ","})},
          {"retriggerCharacters", Json::array({")"})}}},
        {"documentSymbolProvider", true},
        {"workspaceSymbolProvider", true},
        {"workspace",
         {{"workspaceFolders", {{"supported", true}, {"changeNotifications", true}}}}}};
    if (options_.semantic_tokens) {
        capabilities["semanticTokensProvider"] = {
            {"legend",
             {{"tokenTypes",
               Json::array({"namespace", "type", "class", "enum", "parameter", "variable",
                            "property", "enumMember", "function", "method", "macro", "keyword",
                            "comment", "string", "number", "typeParameter"})},
              {"tokenModifiers", Json::array()}}},
            {"full", true},
            {"range", false}};
    }
    return {{"capabilities", std::move(capabilities)},
            {"serverInfo", {{"name", "HLSL-LSP"}, {"version", HLSL_LSP_VERSION}}}};
}

Json Server::shutdown(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (params.has_value() && !params->is_null()) {
        invalid_params("Shutdown does not accept parameters");
    }
    if (state_ != State::running) {
        throw HandlerError{json_rpc::invalid_request_code, "Shutdown is not valid now"};
    }
    state_ = State::shutdown;
    clean_shutdown_ = true;
    return nullptr;
}

Json Server::completion(const std::optional<Json>& params,
                        const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto& text_document = object_member(value, "textDocument");
    const auto uri = string_member(text_document, "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Completion document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto completions =
        analysis_.complete(snapshot.document_uri().identity(), snapshot.version(), snapshot.path(),
                           line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Completion was superseded"};
        }
    }

    Json items = Json::array();
    for (const auto& completion_item : completions) {
        items.push_back({{"label", completion_item.label},
                         {"detail", completion_item.detail},
                         {"kind", completion_kind(completion_item.cursor_kind)}});
    }
    return {{"isIncomplete", false}, {"items", std::move(items)}};
}

Json Server::definition(const std::optional<Json>& params,
                        const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Definition document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto utf8_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    workspace::WorkspaceConfiguration configuration;
    std::vector<workspace::SourceSnapshot> open_documents;
    {
        std::scoped_lock state_lock{state_mutex_};
        configuration = configuration_for(snapshot, editor_settings_);
        open_documents = documents_.open_snapshots();
    }
    context.cancellation.throw_if_cancellation_requested();
    const auto include_target =
        workspace::resolve_include_at(snapshot, open_documents, configuration, utf8_offset);
    if (include_target) {
        const auto target = workspace::DocumentUri::from_path(include_target->string());
        const workspace::Position start{};
        return {{"uri", target.uri()}, {"range", lsp_range({.start = start, .end = start})}};
    }

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto definition =
        analysis_.definition(snapshot.document_uri().identity(), snapshot.version(),
                             snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Definition was superseded"};
        }
    }
    if (!definition.has_value()) {
        return nullptr;
    }

    const auto target = workspace::DocumentUri::from_path(definition->location.path);
    std::string target_text;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (documents_.contains(target.uri())) {
            target_text = documents_.snapshot(target.uri()).text();
        }
    }
    if (target_text.empty()) {
        std::ifstream stream{target.path(), std::ios::binary};
        target_text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    workspace::Position start{
        .line = definition->location.line > 0 ? definition->location.line - 1 : 0,
        .character = definition->location.column > 0 ? definition->location.column - 1 : 0};
    if (!target_text.empty()) {
        if (const auto offset = dxc_offset_at(target_text, definition->location.line,
                                              definition->location.column)) {
            start = workspace::lsp_position_at(target_text, *offset);
        }
    }
    auto end = start;
    const auto name_length = workspace::utf16_length(definition->name);
    if (name_length <= std::numeric_limits<std::uint32_t>::max() - end.character) {
        end.character += static_cast<std::uint32_t>(name_length);
    }
    return {{"uri", target.uri()}, {"range", lsp_range({.start = start, .end = end})}};
}

Server::ReferenceResult Server::find_references(std::string_view uri,
                                                const workspace::Position& request_position,
                                                const json_rpc::RequestContext& context) {
    workspace::SourceSnapshot request = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Reference document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(request.uri());
    const auto [line, column] = dxc_position(request.text(), request_position);
    const auto target = analysis_.definition(request.document_uri().identity(), request.version(),
                                             request.path(), line, column, context.cancellation);
    if (!target.has_value()) {
        return {.request = std::move(request), .target = {}, .references = {}};
    }

    std::vector<dxc::Reference> found;
    const auto target_identity =
        workspace::DocumentUri::from_path(target->location.path).identity();
    for (const auto& root : analysis_.roots()) {
        context.cancellation.throw_if_cancellation_requested();
        if (root.root_identity != target_identity &&
            !root.dependency_identities.contains(target_identity)) {
            continue;
        }
        workspace::SourceSnapshot root_snapshot = [&] {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(root.root_uri) || !documents_.document(root.root_uri).open) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "A referenced root is no longer open"};
            }
            return documents_.snapshot(root.root_uri);
        }();
        analyze_and_publish(root_snapshot.uri());
        auto references = analysis_.references(root.root_identity, root_snapshot.version(),
                                               target->location.path, target->location.line,
                                               target->location.column, context.cancellation);
        {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(root_snapshot.uri()) ||
                !documents_.document(root_snapshot.uri()).open ||
                documents_.document(root_snapshot.uri()).version != root_snapshot.version()) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "A referenced root changed during analysis"};
            }
        }
        found.insert(found.end(), std::make_move_iterator(references.begin()),
                     std::make_move_iterator(references.end()));
    }

    std::ranges::sort(found, [](const auto& left, const auto& right) {
        const auto left_uri = workspace::DocumentUri::from_path(left.location.path);
        const auto right_uri = workspace::DocumentUri::from_path(right.location.path);
        return std::tie(left_uri.identity(), left.start_offset, left.end_offset) <
               std::tie(right_uri.identity(), right.start_offset, right.end_offset);
    });
    found.erase(std::ranges::unique(
                    found, {},
                    [](const auto& reference) {
                        return std::tuple{
                            workspace::DocumentUri::from_path(reference.location.path).identity(),
                            reference.start_offset, reference.end_offset};
                    })
                    .begin(),
                found.end());

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(request.uri())) {
            throw HandlerError{json_rpc::content_modified_code, "Reference request was superseded"};
        }
        const auto& latest = documents_.document(request.uri());
        if (!latest.open || latest.version != request.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Reference request was superseded"};
        }
    }
    return {.request = std::move(request), .target = *target, .references = std::move(found)};
}

Json Server::references(const std::optional<Json>& params,
                        const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));
    bool include_declaration = true;
    if (const auto request_context = value.find("context"); request_context != value.end()) {
        if (!request_context->is_object()) {
            invalid_params("Reference context must be an object");
        }
        if (const auto include = request_context->find("includeDeclaration");
            include != request_context->end()) {
            if (!include->is_boolean()) {
                invalid_params("includeDeclaration must be a boolean");
            }
            include_declaration = include->get<bool>();
        }
    }

    auto result = find_references(uri, request_position, context);
    Json locations = Json::array();
    for (const auto& reference : result.references) {
        const auto target = workspace::DocumentUri::from_path(reference.location.path);
        if (!include_declaration &&
            target.identity() ==
                workspace::DocumentUri::from_path(result.target.location.path).identity() &&
            reference.start_offset == result.target.location.offset) {
            continue;
        }
        std::string text;
        {
            std::scoped_lock state_lock{state_mutex_};
            if (documents_.contains(target.uri())) {
                text = documents_.snapshot(target.uri()).text();
            }
        }
        if (text.empty()) {
            std::ifstream stream{target.path(), std::ios::binary};
            if (!stream) {
                continue;
            }
            text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }
        const auto start = static_cast<std::size_t>(reference.start_offset);
        const auto end = static_cast<std::size_t>(reference.end_offset);
        if (start > end || end > text.size() ||
            text.substr(start, end - start) != result.target.name) {
            throw HandlerError{json_rpc::content_modified_code,
                               "A referenced source file changed after analysis"};
        }
        locations.push_back(
            {{"uri", target.uri()}, {"range", lsp_range(reference_range(text, reference))}});
    }
    return locations;
}

Json Server::prepare_rename(const std::optional<Json>& params,
                            const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));
    auto result = find_references(uri, request_position, context);
    if (result.target.name.empty()) {
        return nullptr;
    }

    const auto offset = workspace::utf8_offset_at(result.request.text(), request_position);
    for (const auto& reference : result.references) {
        if (workspace::DocumentUri::from_path(reference.location.path).identity() ==
                result.request.document_uri().identity() &&
            reference.start_offset <= offset && offset <= reference.end_offset) {
            return {{"range", lsp_range(reference_range(result.request.text(), reference))},
                    {"placeholder", result.target.name}};
        }
    }
    return nullptr;
}

Json Server::rename(const std::optional<Json>& params, const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));
    const auto new_name = string_member(value, "newName");
    if (!valid_rename_identifier(new_name)) {
        invalid_params("Rename requires a non-keyword HLSL identifier");
    }

    auto result = find_references(uri, request_position, context);
    if (result.target.name.empty()) {
        invalid_params("The selected token cannot be renamed");
    }

    struct FileEdits final {
        workspace::DocumentUri uri;
        std::optional<std::int64_t> version;
        std::string text;
        std::vector<dxc::Reference> references;
    };
    std::map<std::string, FileEdits, std::less<>> files;
    for (const auto& reference : result.references) {
        const auto target = workspace::DocumentUri::from_path(reference.location.path);
        auto [entry, inserted] = files.try_emplace(
            target.identity(),
            FileEdits{.uri = target, .version = std::nullopt, .text = {}, .references = {}});
        if (inserted) {
            std::scoped_lock state_lock{state_mutex_};
            if (documents_.contains(target.uri())) {
                const auto& state = documents_.document(target.uri());
                entry->second.text = state.text;
                if (state.open) {
                    entry->second.version = state.version;
                }
            }
        }
        entry->second.references.push_back(reference);
    }

    Json document_changes = Json::array();
    for (auto& [identity, file] : files) {
        static_cast<void>(identity);
        if (file.text.empty()) {
            std::ifstream stream{file.uri.path(), std::ios::binary};
            if (!stream) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "A referenced source file is no longer available"};
            }
            file.text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }
        Json edits = Json::array();
        for (const auto& reference : file.references) {
            const auto start = static_cast<std::size_t>(reference.start_offset);
            const auto end = static_cast<std::size_t>(reference.end_offset);
            if (start > end || end > file.text.size() ||
                file.text.substr(start, end - start) != result.target.name) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "A referenced source file changed after analysis"};
            }
            edits.push_back({{"range", lsp_range(reference_range(file.text, reference))},
                             {"newText", new_name}});
        }
        Json version = file.version.has_value() ? Json(*file.version) : Json(nullptr);
        document_changes.push_back(
            {{"textDocument", {{"uri", file.uri.uri()}, {"version", std::move(version)}}},
             {"edits", std::move(edits)}});
    }
    return {{"documentChanges", std::move(document_changes)}};
}

Json Server::hover(const std::optional<Json>& params, const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Hover document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto request_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    if (request_offset >= snapshot.text().size()) {
        return nullptr;
    }
    const auto lexical = lexical_prefix(snapshot.text(), request_offset + 1);
    if (!lexical.code[request_offset]) {
        return nullptr;
    }

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto information = analysis_.hover(snapshot.document_uri().identity(), snapshot.version(),
                                             snapshot.path(), line, column, context.cancellation);
    const auto layout =
        analysis_.memory_layout(snapshot.document_uri().identity(), snapshot.version(),
                                snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Hover was superseded"};
        }
    }
    if (!information.has_value() && !layout.has_value()) {
        return nullptr;
    }

    std::string contents;
    if (information.has_value() && !information->declaration.empty()) {
        contents += information->declaration;
    } else if (information.has_value() && !information->display_name.empty()) {
        contents += information->display_name;
    } else if (information.has_value()) {
        contents += information->name;
    } else {
        contents += layout->selected_type;
        contents += ' ';
        contents += layout->selected_name;
    }
    if (information.has_value() && !information->qualified_name.empty() &&
        information->qualified_name != information->display_name &&
        information->qualified_name != information->declaration) {
        contents += "\nSymbol: ";
        contents += information->qualified_name;
    }
    if (information.has_value() && !information->type.empty()) {
        contents += "\nType: ";
        contents += information->type;
    }
    if (layout.has_value()) {
        contents += "\n\n";
        if (layout->supported) {
            contents += "Memory layout: size ";
            contents += std::to_string(layout->selected_size);
            contents += " bytes, alignment ";
            contents += std::to_string(layout->selected_alignment);
            contents += " bytes";
            if (layout->packed_offset.has_value()) {
                contents += ", packed offset ";
                contents += std::to_string(*layout->packed_offset);
                contents += " bytes";
            }
        } else {
            contents += "Memory layout unavailable: ";
            contents += layout->explanation;
        }
        if (command_links_) {
            contents += "\n\n[Memory Layout](";
            contents += memory_layout_command(uri, request_position);
            contents += ')';
        }
    }

    Json result{{"contents",
                 {{"kind", layout.has_value() ? "markdown" : "plaintext"},
                  {"value", std::move(contents)}}}};
    if (information.has_value() && information->start_offset <= information->end_offset &&
        information->end_offset <= snapshot.text().size()) {
        result["range"] = lsp_range(
            {.start = workspace::lsp_position_at(snapshot.text(), information->start_offset),
             .end = workspace::lsp_position_at(snapshot.text(), information->end_offset)});
    }
    return result;
}

Json Server::memory_layout(const std::optional<Json>& params,
                           const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));
    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Memory layout document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    try {
        static_cast<void>(workspace::utf8_offset_at(snapshot.text(), request_position));
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto layout =
        analysis_.memory_layout(snapshot.document_uri().identity(), snapshot.version(),
                                snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Memory layout was superseded"};
        }
    }
    return layout.has_value() ? memory_layout_json(*layout) : Json(nullptr);
}

Json Server::compilation_info(const std::optional<Json>& params,
                              const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Compilation info document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    const auto info =
        analysis_.compilation_info(snapshot.document_uri().identity(), snapshot.version(),
                                   snapshot.path(), context.cancellation);
    std::optional<std::string> active_variant;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Compilation info was superseded"};
        }
        active_variant = active_variant_;
    }

    // Resolves the source text for every distinct path a reflected
    // resource's `source_location` points into, so `sourceLocation` in the
    // response can carry a proper UTF-16 range rather than a raw byte
    // column. Prefers the open-document buffer (the current unsaved
    // snapshot) and falls back to disk, mirroring how `definition()`
    // resolves cross-file targets.
    std::unordered_map<std::string, std::string> resource_location_texts;
    if (info.reflection.has_value()) {
        for (const auto& resource : info.reflection->resources) {
            if (!resource.source_location.has_value()) {
                continue;
            }
            const auto& path = resource.source_location->path;
            if (path.empty() || resource_location_texts.contains(path)) {
                continue;
            }
            std::string text;
            const auto target_uri = workspace::DocumentUri::from_path(path).uri();
            {
                std::scoped_lock state_lock{state_mutex_};
                if (documents_.contains(target_uri)) {
                    text = documents_.snapshot(target_uri).text();
                }
            }
            if (text.empty()) {
                std::ifstream stream{path, std::ios::binary};
                text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
            }
            resource_location_texts.emplace(path, std::move(text));
        }
    }
    return compilation_info_json(info, active_variant, resource_location_texts);
}

Json Server::signature_help(const std::optional<Json>& params,
                            const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto request_position = position(object_member(value, "position"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Signature help document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto cursor_offset = [&] {
        try {
            return workspace::utf8_offset_at(snapshot.text(), request_position);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    const auto call = call_context(snapshot.text(), cursor_offset);
    if (!call.has_value()) {
        return nullptr;
    }
    const auto callee_position = workspace::lsp_position_at(snapshot.text(), call->callee_offset);
    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), callee_position);
    const auto signatures =
        analysis_.signatures(snapshot.document_uri().identity(), snapshot.version(),
                             snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Signature help was superseded"};
        }
    }
    if (signatures.empty()) {
        return nullptr;
    }

    Json items = Json::array();
    for (const auto& signature : signatures) {
        Json parameters = Json::array();
        for (const auto& parameter : signature.parameters) {
            parameters.push_back({{"label", parameter.label}});
        }
        Json item{{"label", signature.label}, {"parameters", std::move(parameters)}};
        if (!signature.parameters.empty()) {
            item["activeParameter"] =
                (std::min)(call->active_parameter, signature.parameters.size() - 1);
        }
        items.push_back(std::move(item));
    }

    Json result{{"signatures", std::move(items)}, {"activeSignature", 0}};
    if (!signatures.front().parameters.empty()) {
        result["activeParameter"] =
            (std::min)(call->active_parameter, signatures.front().parameters.size() - 1);
    }
    return result;
}

Json Server::document_symbols(const std::optional<Json>& params,
                              const json_rpc::RequestContext& context) {
    require_running();
    const auto uri = string_member(object_member(object_params(params), "textDocument"), "uri");
    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Document symbols require an open document");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    Json result = Json::array();
    analyze_and_publish(snapshot.uri());
    const auto symbols = analysis_.symbols(snapshot.document_uri().identity(), snapshot.version(),
                                           context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Document symbols were superseded"};
        }
    }
    append_document_symbols(result, symbols, snapshot);
    return result;
}

Json Server::workspace_symbols(const std::optional<Json>& params,
                               const json_rpc::RequestContext& context) {
    require_running();
    const auto query = string_member(object_params(params), "query");
    Json result = Json::array();
    for (const auto& root : analysis_.roots()) {
        workspace::SourceSnapshot snapshot = [&]() -> workspace::SourceSnapshot {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(root.root_uri)) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "Workspace symbols were superseded"};
            }
            return documents_.snapshot(root.root_uri);
        }();
        if (snapshot.document_uri().identity() != root.root_identity ||
            root.version != snapshot.version()) {
            continue;
        }
        analyze_and_publish(snapshot.uri());
        const auto symbols =
            analysis_.symbols(root.root_identity, root.version, context.cancellation);
        {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(snapshot.uri()) ||
                documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "Workspace symbols were superseded"};
            }
        }
        append_workspace_symbols(result, symbols, snapshot, query, {});
    }
    return result;
}

Json Server::semantic_tokens(const std::optional<Json>& params,
                             const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Semantic token document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    if (snapshot.text().size() > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Semantic token document is too large");
    }

    analyze_and_publish(snapshot.uri());
    const auto dxc_tokens = analysis_.tokens(snapshot.document_uri().identity(), snapshot.version(),
                                             snapshot.path(), context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Semantic tokens were superseded"};
        }
    }
    std::vector<SemanticToken> tokens;
    tokens.reserve(dxc_tokens.size());
    for (const auto& token : dxc_tokens) {
        append_semantic_token(tokens, snapshot.text(), token);
    }
    std::ranges::sort(tokens, [](const auto& left, const auto& right) {
        return std::pair{left.start.line, left.start.character} <
               std::pair{right.start.line, right.start.character};
    });

    Json data = Json::array();
    std::uint32_t previous_line{};
    std::uint32_t previous_character{};
    for (const auto& token : tokens) {
        const auto delta_line = token.start.line - previous_line;
        const auto delta_character =
            delta_line == 0 ? token.start.character - previous_character : token.start.character;
        data.push_back(delta_line);
        data.push_back(delta_character);
        data.push_back(token.length);
        data.push_back(static_cast<std::uint32_t>(token.type));
        data.push_back(0);
        previous_line = token.start.line;
        previous_character = token.start.character;
    }
    return {{"data", std::move(data)}};
}

void Server::initialized(const std::optional<Json>& params) {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::awaiting_initialized) {
        log("Ignoring initialized notification in an invalid lifecycle state");
        return;
    }
    if (params.has_value() && !params->is_object()) {
        log("Ignoring initialized notification with invalid parameters");
        return;
    }
    state_ = State::running;
}

void Server::did_open(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& document = object_member(object_params(params), "textDocument");
        const auto uri = string_member(document, "uri");
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_open(uri, string_member(document, "languageId"),
                                integer_member(document, "version"),
                                string_member(document, "text"));
        }
        analyze_affected(uri);
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto& document = object_member(value, "textDocument");
        const auto uri = string_member(document, "uri");
        const auto& raw_changes = member(value, "contentChanges");
        if (!raw_changes.is_array() || raw_changes.empty()) {
            invalid_params("contentChanges must be a non-empty array");
        }

        std::vector<workspace::ContentChange> changes;
        changes.reserve(raw_changes.size());
        for (const auto& raw_change : raw_changes) {
            if (!raw_change.is_object()) {
                invalid_params("Each content change must be an object");
            }
            workspace::ContentChange change{.range = std::nullopt,
                                            .range_length = std::nullopt,
                                            .text = string_member(raw_change, "text")};
            if (const auto item = raw_change.find("range"); item != raw_change.end()) {
                change.range = range(*item);
            }
            if (const auto item = raw_change.find("rangeLength"); item != raw_change.end()) {
                if (!item->is_number_unsigned() && !item->is_number_integer()) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                const auto length = item->get<std::int64_t>();
                if (length < 0) {
                    invalid_params("rangeLength must be a non-negative integer");
                }
                change.range_length = static_cast<std::size_t>(length);
            }
            changes.push_back(std::move(change));
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_change(uri, integer_member(document, "version"), changes);
        }
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_save(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        const auto uri = string_member(object_member(value, "textDocument"), "uri");
        std::optional<std::string> text;
        if (const auto item = value.find("text"); item != value.end()) {
            if (!item->is_string()) {
                invalid_params("Save text must be a string");
            }
            text = item->get<std::string>();
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_save(uri, std::move(text));
        }
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_close(const std::optional<Json>& params) {
    try {
        require_running();
        const auto uri = string_member(object_member(object_params(params), "textDocument"), "uri");
        workspace::SourceSnapshot snapshot = [&] {
            std::scoped_lock state_lock{state_mutex_};
            return documents_.snapshot(uri);
        }();
        const std::unordered_set changed{snapshot.document_uri().identity()};
        auto affected_roots =
            analysis_.dependent_root_uris(changed, snapshot.document_uri().identity());
        {
            std::scoped_lock state_lock{state_mutex_};
            documents_.did_close(uri);
            ++analysis_generations_[snapshot.document_uri().identity()];
        }
        analysis_.erase(snapshot.document_uri().identity());
        for (const auto& root_uri : affected_roots) {
            analyze_and_publish(root_uri);
        }
        sender_(json_rpc::Notification{
            .method = "textDocument/publishDiagnostics",
            .params = Json{{"uri", snapshot.uri()}, {"diagnostics", Json::array()}}});
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_configuration(const std::optional<Json>& params) {
    try {
        require_running();
        const auto candidate =
            configuration_overrides(object_member(object_params(params), "settings"));
        {
            std::scoped_lock state_lock{state_mutex_};
            for (const auto& document : documents_.open_snapshots()) {
                static_cast<void>(configuration_for(document, candidate));
            }
            editor_settings_ = candidate;
        }
        reanalyze_all();
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_client_defaults(const std::optional<Json>& params) {
    try {
        require_running();
        const auto defaults = configuration_overrides(object_params(params));
        if (!defaults.language_version) {
            invalid_params("hlsl.languageVersion must be provided");
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            client_default_language_version_ = *defaults.language_version;
        }
        reanalyze_all();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_active_variant(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& value = object_params(params);
        std::optional<std::string> variant;
        if (const auto item = value.find("variant"); item != value.end() && !item->is_null()) {
            if (!item->is_string()) {
                invalid_params("variant must be a string or null");
            }
            auto name = item->get<std::string>();
            if (!name.empty()) {
                variant = std::move(name);
            }
        }
        bool changed{};
        {
            std::scoped_lock state_lock{state_mutex_};
            changed = active_variant_ != variant;
            active_variant_ = variant;
            if (changed) {
                reported_variant_issue_key_.reset();
            }
        }
        if (changed) {
            // A variant change reanalyzes open documents; only a differing runtime
            // selection escalates to the controlled restart shared with issue #14.
            reanalyze_all();
            reevaluate_runtime_selection();
        }
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_workspace_folders(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& event = object_member(object_params(params), "event");
        const auto& removed = member(event, "removed");
        const auto& added = member(event, "added");
        if (!removed.is_array() || !added.is_array()) {
            invalid_params("Workspace folder changes must contain added and removed arrays");
        }
        std::unordered_map<std::string, std::filesystem::path> workspace_folders;
        {
            std::scoped_lock state_lock{state_mutex_};
            workspace_folders = workspace_folders_;
        }
        for (const auto& folder : removed) {
            workspace_folders.erase(workspace_folder_identity(folder));
        }
        for (const auto& folder : added) {
            const auto [identity, path] = workspace_folder(folder);
            workspace_folders.insert_or_assign(identity, path);
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            workspace_folders_ = std::move(workspace_folders);
        }
        reanalyze_all();
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::did_change_watched_files(const std::optional<Json>& params) {
    try {
        require_running();
        const auto& changes = member(object_params(params), "changes");
        if (!changes.is_array()) {
            invalid_params("Watched file changes must be an array");
        }

        std::unordered_set<std::string> changed_identities;
        std::vector<std::string> changed_configuration_directories;
        for (const auto& change : changes) {
            try {
                const auto changed = workspace::DocumentUri::from_uri(string_member(change, "uri"));
                changed_identities.insert(changed.identity());
                auto filename = std::filesystem::path{changed.path()}.filename().string();
#ifdef _WIN32
                std::ranges::transform(filename, filename.begin(), [](char value) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                });
#endif
                if (filename == workspace::configuration_file_name) {
                    changed_configuration_directories.push_back(
                        workspace::DocumentUri::from_path(
                            std::filesystem::path{changed.path()}.parent_path().string())
                            .identity());
                }
            } catch (const workspace::DocumentError& error) {
                invalid_params(error.what());
            }
        }

        analysis_.invalidate_include_metadata(changed_identities);
        std::vector<std::string> affected_roots = analysis_.dependent_root_uris(changed_identities);
        std::unordered_set<std::string> affected_root_identities;
        for (const auto& root : analysis_.roots()) {
            if (std::ranges::find(affected_roots, root.root_uri) != affected_roots.end()) {
                affected_root_identities.insert(root.root_identity);
            }
        }

        const auto in_changed_configuration_scope =
            [&changed_configuration_directories](std::string_view identity) {
                return std::ranges::any_of(
                    changed_configuration_directories, [identity](const auto& directory) {
#ifdef _WIN32
                        constexpr char separator = '\\';
#else
                        constexpr char separator = '/';
#endif
                        if (!identity.starts_with(directory)) {
                            return false;
                        }
                        if (!directory.empty() && directory.back() == separator) {
                            return identity.size() > directory.size();
                        }
                        return identity.size() > directory.size() &&
                               identity[directory.size()] == separator;
                    });
            };
        std::vector<workspace::SourceSnapshot> open_documents;
        {
            std::scoped_lock state_lock{state_mutex_};
            open_documents = documents_.open_snapshots();
        }
        for (const auto& document : open_documents) {
            const auto& identity = document.document_uri().identity();
            if (in_changed_configuration_scope(identity)) {
                if (affected_root_identities.emplace(identity).second) {
                    affected_roots.push_back(document.uri());
                }
            }
        }
        for (const auto& root_uri : affected_roots) {
            analyze_and_publish(root_uri);
        }
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void Server::exit(const std::optional<Json>& params) {
    if (params.has_value() && !params->is_null()) {
        log("Exit notification does not accept parameters");
    }
    exit_requested_.store(true, std::memory_order_release);
}

void Server::analyze_affected(std::string_view uri) {
    const auto changed = [&] {
        std::scoped_lock state_lock{state_mutex_};
        return documents_.snapshot(uri);
    }();
    const std::unordered_set changed_identities{changed.document_uri().identity()};
    analysis_.invalidate_include_metadata(changed_identities);
    auto affected_roots =
        analysis_.dependent_root_uris(changed_identities, changed.document_uri().identity());

    analyze_and_publish(uri);
    for (const auto& root_uri : affected_roots) {
        analyze_and_publish(root_uri);
    }
}

void Server::analyze_and_publish(std::string_view uri) {
    analysis::AnalysisInput input = [&] {
        std::scoped_lock state_lock{state_mutex_};
        const auto& state = documents_.document(uri);
        if (!state.open) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Document was closed before analysis"};
        }
        auto snapshot = documents_.snapshot(uri);
        const auto generation = ++analysis_generations_[snapshot.document_uri().identity()];
        return analysis::AnalysisInput{.root = snapshot,
                                       .open_documents = documents_.open_snapshots(),
                                       .configuration =
                                           configuration_for(snapshot, editor_settings_),
                                       .generation = generation};
    }();
    analysis_.analyze(std::move(input));
    if (!options_.background_analysis) {
        analysis_.wait_idle();
    }
}

workspace::WorkspaceConfiguration
Server::configuration_for(const workspace::SourceSnapshot& snapshot,
                          const workspace::ConfigurationOverrides& overrides) const {
    workspace::WorkspaceConfiguration configuration;
    const auto shader_directory = std::filesystem::path{snapshot.path()}.parent_path();
    std::error_code error;
    if (std::filesystem::is_directory(shader_directory, error)) {
        configuration = workspace::load_workspace_configuration_for_file(snapshot.path());
    } else if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error{"Unable to inspect shader directory",
                                                shader_directory, error};
    }
    // The active variant is applied on top of the file-derived configuration but
    // below editor overrides, so a selected variant beats client defaults while an
    // explicit editor setting still wins. Unknown or inapplicable selections are
    // reported by reevaluate_variant_selection rather than throwing here.
    if (active_variant_) {
        static_cast<void>(workspace::apply_variant(configuration, *active_variant_));
    }
    if (!configuration.language_version && client_default_language_version_) {
        configuration.language_version = client_default_language_version_;
    }
    return workspace::apply_configuration_overrides(std::move(configuration), overrides,
                                                    configuration_base_directory(snapshot.path()));
}

std::filesystem::path Server::configuration_base_directory(std::string_view shader_path) const {
    auto directory = std::filesystem::absolute(std::filesystem::path{shader_path}.parent_path())
                         .lexically_normal();
    auto candidate = directory;
    while (!candidate.empty()) {
        try {
            const auto identity = workspace::DocumentUri::from_path(candidate.string()).identity();
            if (const auto folder = workspace_folders_.find(identity);
                folder != workspace_folders_.end()) {
                return folder->second;
            }
        } catch (const workspace::DocumentError&) {
            break;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return directory;
}

void Server::reanalyze_all() {
    const auto open_documents = [&] {
        std::scoped_lock state_lock{state_mutex_};
        return documents_.open_snapshots();
    }();
    for (const auto& document : open_documents) {
        analyze_and_publish(document.uri());
    }
}

std::string Server::loaded_runtime_directory() const { return options_.analysis.runtime.directory; }

void Server::reevaluate_runtime_selection() {
    std::optional<std::string> notify_directory;
    std::string notify_reason;
    std::string issue_message;
    bool issue_is_error = false;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (state_ != State::running) {
            return;
        }
        const auto open_documents = documents_.open_snapshots();
        if (open_documents.empty()) {
            return;
        }

        std::optional<std::optional<std::filesystem::path>> desired;
        std::string desired_key;
        std::string desired_label;
        bool conflict = false;
        std::string conflict_label;
        for (const auto& snapshot : open_documents) {
            std::optional<std::filesystem::path> selection;
            try {
                selection = configuration_for(snapshot, editor_settings_).dxc_runtime_directory;
            } catch (const std::exception&) {
                // Configuration errors already surface through analysis diagnostics.
                continue;
            }
            const auto label = selection ? selection->string() : std::string{"bundled default"};
            const auto key = runtime_directory_key(selection ? selection->string() : std::string{});
            if (!desired) {
                desired = std::move(selection);
                desired_key = key;
                desired_label = label;
            } else if (key != desired_key) {
                conflict = true;
                conflict_label = label;
                break;
            }
        }
        if (!desired) {
            return;
        }

        if (conflict) {
            const auto key = "conflict:" + desired_key + "|" + conflict_label;
            if (reported_runtime_issue_key_ != key) {
                reported_runtime_issue_key_ = key;
                issue_message = "Open HLSL documents select different DXC runtimes ('" +
                                desired_label + "' and '" + conflict_label +
                                "'). DXC is loaded per process, so the active runtime is unchanged "
                                "until the conflict is resolved.";
            }
            requested_runtime_key_.reset();
        } else {
            const auto loaded_key = runtime_directory_key(options_.analysis.runtime.directory);
            const auto desired_runtime = desired.value_or(std::nullopt);
            const std::string desired_directory =
                desired_runtime ? desired_runtime->string() : std::string{};
            if (desired_key == loaded_key) {
                requested_runtime_key_.reset();
                reported_runtime_issue_key_.reset();
            } else {
                try {
                    if (!desired_directory.empty()) {
                        static_cast<void>(dxc::validate_runtime_directory(desired_directory));
                    }
                    if (requested_runtime_key_ != desired_key) {
                        requested_runtime_key_ = desired_key;
                        reported_runtime_issue_key_.reset();
                        notify_directory = desired_directory;
                        notify_reason = "The HLSL workspace selected the " + desired_label +
                                        " DXC runtime. Restarting the language server to load it.";
                    }
                } catch (const dxc::RuntimeError& error) {
                    const auto key = "invalid:" + desired_key;
                    if (reported_runtime_issue_key_ != key) {
                        reported_runtime_issue_key_ = key;
                        issue_message = std::string{"The selected DXC runtime cannot be used: "} +
                                        error.what() + ". The active runtime is unchanged.";
                        issue_is_error = true;
                    }
                }
            }
        }
    }

    if (notify_directory) {
        log(notify_reason);
        sender_(json_rpc::Notification{
            .method = "hlsl/dxcRuntimeRestartRequired",
            .params = Json{{"directory", *notify_directory}, {"reason", notify_reason}}});
    }
    if (!issue_message.empty()) {
        log(issue_message);
        sender_(json_rpc::Notification{
            .method = "window/showMessage",
            .params = Json{{"type", issue_is_error ? 1 : 2}, {"message", issue_message}}});
    }
}

Json Server::dxc_runtime(const std::optional<Json>& params) {
    require_running();
    if (params.has_value() && !params->is_null() && !params->is_object()) {
        invalid_params("hlsl/dxcRuntime does not accept parameters");
    }
    dxc::RuntimeInfo info;
    std::string error_message;
    try {
        info = analysis_.dxc_runtime_info();
    } catch (const std::exception& error) {
        error_message = error.what();
    }
    std::string requested;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (requested_runtime_key_) {
            requested = *requested_runtime_key_;
        }
    }
    Json result = {{"source", info.bundled ? "bundled" : "configured"},
                   {"directory", info.directory},
                   {"libraryPath", info.library_path},
                   {"version", info.version},
                   {"requiresRestart", !requested.empty()}};
    if (!error_message.empty()) {
        result["error"] = error_message;
    }
    return result;
}

void Server::reevaluate_variant_selection() {
    std::string issue_message;
    bool issue_is_error = false;
    std::string issue_key;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (state_ != State::running) {
            return;
        }
        const auto open_documents = documents_.open_snapshots();
        if (open_documents.empty()) {
            return;
        }

        std::string schema_error;
        bool has_variants = false;
        bool active_defined = false;
        bool active_applicable = false;
        for (const auto& snapshot : open_documents) {
            workspace::WorkspaceConfiguration configuration;
            try {
                configuration = configuration_for(snapshot, editor_settings_);
            } catch (const workspace::ConfigurationError& error) {
                if (error.code() == workspace::ConfigurationErrorCode::invalid_variant &&
                    schema_error.empty()) {
                    schema_error = error.what();
                }
                continue;
            } catch (const std::exception&) {
                continue;
            }
            if (!configuration.variants.empty()) {
                has_variants = true;
            }
            if (active_variant_) {
                for (const auto& variant : configuration.variants) {
                    if (variant.name == *active_variant_) {
                        active_defined = true;
                        active_applicable = active_applicable || variant.applicable;
                    }
                }
            }
        }

        if (!schema_error.empty()) {
            issue_key = "schema:" + schema_error;
            issue_message = "Invalid shader variant configuration: " + schema_error;
            issue_is_error = true;
        } else if (active_variant_ && !active_applicable && (active_defined || has_variants)) {
            issue_key = (active_defined ? "inapplicable:" : "undefined:") + *active_variant_;
            issue_message =
                active_defined
                    ? "The selected shader variant '" + *active_variant_ +
                          "' is not applicable to any open HLSL document; those documents use "
                          "their default configuration."
                    : "The selected shader variant '" + *active_variant_ +
                          "' is not defined for the open HLSL documents.";
        }

        if (issue_key.empty()) {
            reported_variant_issue_key_.reset();
            return;
        }
        if (reported_variant_issue_key_ == issue_key) {
            return;
        }
        reported_variant_issue_key_ = issue_key;
    }

    log(issue_message);
    sender_(json_rpc::Notification{
        .method = "window/showMessage",
        .params = Json{{"type", issue_is_error ? 1 : 2}, {"message", issue_message}}});
}

Json Server::variants(const std::optional<Json>& params) {
    require_running();
    std::optional<std::string> target_uri;
    if (params.has_value() && !params->is_null()) {
        if (!params->is_object()) {
            invalid_params("hlsl/variants parameters must be an object");
        }
        if (const auto document = params->find("textDocument");
            document != params->end() && document->is_object()) {
            if (const auto uri = document->find("uri");
                uri != document->end() && uri->is_string()) {
                target_uri = uri->get<std::string>();
            }
        } else if (const auto uri = params->find("uri"); uri != params->end() && uri->is_string()) {
            target_uri = uri->get<std::string>();
        }
    }

    struct Aggregate {
        std::string name;
        std::string description;
        bool is_default{};
        bool applicable{};
    };
    std::vector<Aggregate> aggregates;
    std::unordered_map<std::string, std::size_t> index;
    std::optional<std::string> active;
    {
        std::scoped_lock state_lock{state_mutex_};
        active = active_variant_;
        std::vector<workspace::SourceSnapshot> snapshots;
        if (target_uri) {
            if (documents_.contains(*target_uri)) {
                snapshots.push_back(documents_.snapshot(*target_uri));
            }
        } else {
            snapshots = documents_.open_snapshots();
        }
        for (const auto& snapshot : snapshots) {
            workspace::WorkspaceConfiguration configuration;
            try {
                configuration = configuration_for(snapshot, editor_settings_);
            } catch (const std::exception&) {
                continue;
            }
            for (const auto& variant : configuration.variants) {
                const auto found = index.find(variant.name);
                if (found == index.end()) {
                    index.emplace(variant.name, aggregates.size());
                    aggregates.push_back(Aggregate{.name = variant.name,
                                                   .description = variant.description,
                                                   .is_default = variant.is_default,
                                                   .applicable = variant.applicable});
                } else {
                    auto& aggregate = aggregates[found->second];
                    aggregate.applicable = aggregate.applicable || variant.applicable;
                    aggregate.is_default = aggregate.is_default || variant.is_default;
                    if (aggregate.description.empty()) {
                        aggregate.description = variant.description;
                    }
                }
            }
        }
    }

    Json variant_list = Json::array();
    for (const auto& aggregate : aggregates) {
        variant_list.push_back(Json{{"name", aggregate.name},
                                    {"description", aggregate.description},
                                    {"default", aggregate.is_default},
                                    {"applicable", aggregate.applicable}});
    }
    return Json{{"activeVariant", active ? Json(*active) : Json(nullptr)},
                {"variants", std::move(variant_list)}};
}

void Server::analysis_completed(const workspace::SourceSnapshot& snapshot,
                                const std::vector<dxc::Diagnostic>& diagnostics,
                                std::uint64_t generation) {
    std::scoped_lock state_lock{state_mutex_};
    if (!documents_.contains(snapshot.uri())) {
        return;
    }
    const auto& state = documents_.document(snapshot.uri());
    const auto expected = analysis_generations_.find(snapshot.document_uri().identity());
    const auto latest = documents_.snapshot(snapshot.uri());
    if (state.open && expected != analysis_generations_.end() && expected->second == generation &&
        latest.version() == snapshot.version()) {
        publish_diagnostics(latest, diagnostics);
    }
}

void Server::publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                                 const std::vector<dxc::Diagnostic>& diagnostics) {
    Json items = Json::array();
    for (const auto& diagnostic : diagnostics) {
        if (!diagnostic.location.path.empty() &&
            !same_document_path(diagnostic.location.path, snapshot.path())) {
            continue;
        }
        items.push_back({{"range", lsp_range(diagnostic_range(snapshot, diagnostic))},
                         {"severity", diagnostic_severity(diagnostic.severity)},
                         {"source", "dxc"},
                         {"message", diagnostic.message}});
    }
    sender_(json_rpc::Notification{.method = "textDocument/publishDiagnostics",
                                   .params = Json{{"uri", snapshot.uri()},
                                                  {"version", snapshot.version()},
                                                  {"diagnostics", std::move(items)}}});
}

void Server::require_running() const {
    std::scoped_lock state_lock{state_mutex_};
    if (state_ != State::running) {
        throw HandlerError{-32002, "Server not initialized"};
    }
}

void Server::log(std::string_view message) const {
    if (logger_) {
        logger_(message);
    }
}

namespace {

Json summarize_protocol_payload(const Json& value, std::size_t payload_size) {
    if (!value.is_object()) {
        return "<redacted " + std::to_string(payload_size) + " byte payload>";
    }

    Json summary = Json::object();
    if (const auto jsonrpc = value.find("jsonrpc");
        jsonrpc != value.end() && jsonrpc->is_string()) {
        summary["jsonrpc"] = *jsonrpc;
    }
    if (const auto method = value.find("method"); method != value.end() && method->is_string()) {
        summary["method"] = *method;
    }
    if (const auto id = value.find("id"); id != value.end()) {
        summary["id"] = id->is_number() || id->is_null() ? *id : Json{"<redacted>"};
    }

    constexpr std::array content_keys{"params", "result", "error"};
    for (const auto key : content_keys) {
        if (value.contains(key)) {
            summary[key] = "<redacted " + std::to_string(payload_size) + " byte protocol payload>";
        }
    }
    return summary;
}

void trace_payload(std::ostream& errors, std::mutex& mutex, std::string_view direction,
                   std::string_view payload, bool include_source) {
    auto value = Json::parse(payload, nullptr, false);
    std::scoped_lock lock{mutex};
    if (value.is_discarded()) {
        errors << "HLSL-LSP trace " << direction << ": <unparseable " << payload.size()
               << " bytes>\n";
        return;
    }
    if (!include_source) {
        value = summarize_protocol_payload(value, payload.size());
    }
    errors << "HLSL-LSP trace " << direction << ": " << value.dump() << '\n';
}

class RequestExecutor final {
  public:
    RequestExecutor(std::size_t worker_count, std::size_t capacity) : capacity_{capacity} {
        if (worker_count == 0 || capacity == 0) {
            throw std::invalid_argument{"Request executor limits must be positive"};
        }
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            static_cast<void>(index);
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    RequestExecutor(const RequestExecutor&) = delete;
    RequestExecutor& operator=(const RequestExecutor&) = delete;
    ~RequestExecutor() { shutdown(); }

    [[nodiscard]] bool submit(std::function<void()> task) {
        if (!task) {
            throw std::invalid_argument{"Request task must be callable"};
        }
        {
            std::scoped_lock lock{mutex_};
            if (stopping_ || queue_.size() >= capacity_) {
                return false;
            }
            queue_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::scoped_lock lock{mutex_};
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        ready_.notify_all();
        workers_.clear();
    }

    void rethrow_if_failed() {
        std::scoped_lock lock{failure_mutex_};
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

  private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task();
            } catch (...) {
                std::scoped_lock lock{failure_mutex_};
                if (!failure_) {
                    failure_ = std::current_exception();
                }
            }
        }
    }

    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool stopping_{};
};

} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run(std::istream& input, std::ostream& output, std::ostream& errors, ServerOptions options) {
    try {
        options.background_analysis = true;
        std::mutex output_mutex;
        std::mutex error_mutex;
        json_rpc::FrameWriter writer{output};
        const auto write_payload = [&](const std::string& payload) {
            if (options.protocol_trace) {
                trace_payload(errors, error_mutex, "send", payload, options.trace_source);
            }
            std::scoped_lock lock{output_mutex};
            writer.write(payload);
        };
        Server server{[&write_payload](const json_rpc::Notification& notification) {
                          write_payload(json_rpc::serialize(json_rpc::Message{notification}));
                      },
                      [&errors, &error_mutex](std::string_view message) {
                          std::scoped_lock lock{error_mutex};
                          errors << "HLSL-LSP: " << message << '\n';
                      },
                      options};
        RequestExecutor requests{options.request_worker_count, options.request_queue_capacity};
        json_rpc::FrameReader reader{input};

        while (!server.exit_requested()) {
            const auto payload = reader.read();
            if (!payload.has_value()) {
                break;
            }
            if (options.protocol_trace) {
                trace_payload(errors, error_mutex, "receive", *payload, options.trace_source);
            }
            const auto parsed = json_rpc::parse_message(*payload);
            if (parsed.error.has_value()) {
                write_payload(json_rpc::serialize(json_rpc::DispatchResponse{*parsed.error}));
                continue;
            }
            if (const auto* request = std::get_if<json_rpc::Request>(&*parsed.message)) {
                const auto cancellation = server.begin_request(request->id);
                if (request->method == "initialize" || request->method == "shutdown") {
                    const auto response = server.handle(*request, cancellation);
                    write_payload(json_rpc::serialize(response));
                    continue;
                }
                const auto accepted =
                    requests.submit([&server, &write_payload, request = *request, cancellation] {
                        const auto response = server.handle(request, cancellation);
                        write_payload(json_rpc::serialize(response));
                    });
                if (!accepted) {
                    cancellation.cancel();
                    server.finish_request(request->id, cancellation);
                    const json_rpc::ErrorResponse response{
                        .id = request->id,
                        .error = {
                            .code = -32000, .message = "Request queue full", .data = std::nullopt}};
                    write_payload(json_rpc::serialize(json_rpc::DispatchResponse{response}));
                }
            } else {
                static_cast<void>(server.handle(*parsed.message));
            }
        }
        server.cancel_all_requests();
        requests.shutdown();
        requests.rethrow_if_failed();
        server.wait_for_analysis();
        return server.exit_requested() ? server.exit_code() : 0;
    } catch (const std::exception& error) {
        errors << "HLSL-LSP: " << error.what() << '\n';
        return 1;
    }
}

} // namespace hlsl_intellisense::lsp
