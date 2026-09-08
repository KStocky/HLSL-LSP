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
#include <cstdio>
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

[[nodiscard]] std::uint64_t unsigned64_member(const Json& object, std::string_view name) {
    const auto value = integer_member(object, name);
    if (value < 0) {
        invalid_params(std::string{"Expected non-negative integer: "} + std::string{name});
    }
    return static_cast<std::uint64_t>(value);
}

constexpr std::uint64_t max_json_safe_integer = 9'007'199'254'740'991ULL;

struct ComputeDimensions {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

struct ComputeHardwareProfile {
    std::string name;
    std::uint32_t wave_size{};
    std::uint32_t max_threads_per_group{};
    std::uint32_t max_threads_per_compute_unit{};
    std::uint32_t max_groups_per_compute_unit{};
    std::uint32_t shared_memory_bytes_per_compute_unit{};
};

[[nodiscard]] std::uint64_t checked_json_integer(std::uint64_t value,
                                                 std::string_view description) {
    if (value > max_json_safe_integer) {
        invalid_params(std::string{description} + " exceeds the protocol's exact integer bound");
    }
    return value;
}

[[nodiscard]] std::uint32_t positive_u32_member(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    std::uint64_t parsed{};
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value <= 0) {
            invalid_params(std::string{"Expected positive 32-bit integer: "} + std::string{name});
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        invalid_params(std::string{"Expected positive 32-bit integer: "} + std::string{name});
    }
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params(std::string{"Expected positive 32-bit integer: "} + std::string{name});
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] ComputeDimensions compute_dimensions(const Json& value, std::string_view name) {
    if (!value.is_object()) {
        invalid_params(std::string{"Expected object: "} + std::string{name});
    }
    return {.x = positive_u32_member(value, "x"),
            .y = positive_u32_member(value, "y"),
            .z = positive_u32_member(value, "z")};
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right,
                                             std::string_view description) {
    if (right != 0 && left > max_json_safe_integer / right) {
        invalid_params(std::string{description} + " exceeds the protocol's exact integer bound");
    }
    return checked_json_integer(left * right, description);
}

[[nodiscard]] std::uint64_t dimension_product(const ComputeDimensions& dimensions,
                                              std::string_view description) {
    return checked_multiply(checked_multiply(dimensions.x, dimensions.y, description), dimensions.z,
                            description);
}

[[nodiscard]] std::uint32_t ceil_divide(std::uint32_t numerator, std::uint32_t denominator) {
    return numerator / denominator + (numerator % denominator == 0 ? 0U : 1U);
}

[[nodiscard]] Json compute_dimensions_json(const ComputeDimensions& dimensions) {
    return {{"x", dimensions.x}, {"y", dimensions.y}, {"z", dimensions.z}};
}

[[nodiscard]] ComputeHardwareProfile compute_hardware_profile(const Json& value) {
    if (!value.is_object()) {
        invalid_params("Expected object: hardwareProfile");
    }
    auto name = string_member(value, "name");
    if (name.empty()) {
        invalid_params("hardwareProfile.name must not be empty");
    }
    return {.name = std::move(name),
            .wave_size = positive_u32_member(value, "waveSize"),
            .max_threads_per_group = positive_u32_member(value, "maxThreadsPerGroup"),
            .max_threads_per_compute_unit = positive_u32_member(value, "maxThreadsPerComputeUnit"),
            .max_groups_per_compute_unit = positive_u32_member(value, "maxGroupsPerComputeUnit"),
            .shared_memory_bytes_per_compute_unit =
                positive_u32_member(value, "sharedMemoryBytesPerComputeUnit")};
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

[[nodiscard]] std::optional<std::size_t> dxc_offset_at(std::string_view text, std::uint32_t line,
                                                       std::uint32_t column);

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
    if (symbol.extent.has_value()) {
        const auto start =
            dxc_offset_at(snapshot.text(), symbol.extent->start.line, symbol.extent->start.column);
        const auto end =
            dxc_offset_at(snapshot.text(), symbol.extent->end.line, symbol.extent->end.column);
        if (start.has_value() && end.has_value() && *start <= *end) {
            return {.start = workspace::lsp_position_at(snapshot.text(), *start),
                    .end = workspace::lsp_position_at(snapshot.text(), *end)};
        }
    }
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
    const auto location_offset =
        dxc_offset_at(snapshot.text(), symbol.location.line, symbol.location.column);
    const auto start = symbol_offset(
        snapshot.text(), location_offset.value_or(static_cast<std::size_t>(symbol.location.offset)),
        false);
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

// Byte-offset-based range clamping/mapping shared by call-hierarchy JSON
// construction, generalized to arbitrary `text` (rather than a
// currently-open document's own snapshot the way `symbol_range` is) since a
// call-hierarchy item or call site can live in any file the translation
// unit reads, open or not.
[[nodiscard]] workspace::Range offset_range(std::string_view text, std::uint32_t start_offset,
                                            std::uint32_t end_offset) {
    const auto start = symbol_offset(text, static_cast<std::size_t>(start_offset), false);
    const auto normalized_end =
        symbol_offset(text, (std::max)(static_cast<std::size_t>(end_offset), start), true);
    const auto end = (std::max)(normalized_end, start);
    return {.start = workspace::lsp_position_at(text, start),
            .end = workspace::lsp_position_at(text, end)};
}

[[nodiscard]] workspace::Range callable_range(const dxc::CallableSymbol& callable,
                                              std::string_view text) {
    return offset_range(text, callable.start_offset, callable.end_offset);
}

[[nodiscard]] workspace::Range
name_selection_range(std::string_view name, std::uint32_t location_offset, std::string_view text) {
    const auto text_size = text.size();
    const auto start = symbol_offset(text, static_cast<std::size_t>(location_offset), false);
    auto source_offset = start;
    auto name_offset = std::size_t{};
    while (source_offset < text_size && name_offset < name.size()) {
        if (text[source_offset] == name[name_offset]) {
            ++source_offset;
            ++name_offset;
        } else if (text[source_offset] == ' ' || text[source_offset] == '\t') {
            ++source_offset;
        } else {
            break;
        }
    }
    const auto end = symbol_offset(
        text,
        name_offset == name.size() ? source_offset : (std::min)(start + name.size(), text_size),
        true);
    return {.start = workspace::lsp_position_at(text, start),
            .end = workspace::lsp_position_at(text, end)};
}

[[nodiscard]] workspace::Range callable_selection_range(const dxc::CallableSymbol& callable,
                                                        std::string_view text) {
    return name_selection_range(callable.name, callable.location.offset, text);
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
                {"allocationSize", element.allocation_size},
                {"alignment", element.alignment},
                {"paddingBefore", padding_before},
                {"members", std::move(members)}};
    if (element.array_index.has_value()) {
        result["arrayIndex"] = *element.array_index;
    }
    if (element.kind == dxc::MemoryLayoutElementKind::array) {
        result["arrayStride"] = element.array_stride;
        result["arrayDimensions"] = element.array_dimensions;
    }
    if (element.kind == dxc::MemoryLayoutElementKind::matrix) {
        result["matrixStride"] = element.matrix_stride;
        result["rowMajor"] = element.row_major;
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
                {"barrierInstructionCount", reflection.barrier_instruction_count},
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
    if (info.disassembly.has_value()) {
        result["disassembly"] = Json{{"available", info.disassembly->available},
                                     {"text", info.disassembly->text},
                                     {"unavailableReason", info.disassembly->unavailable_reason},
                                     {"truncated", info.disassembly->truncated},
                                     {"originalSize", info.disassembly->original_size},
                                     {"displayedSize", info.disassembly->displayed_size},
                                     {"format", info.disassembly->format}};
    } else {
        result["disassembly"] = nullptr;
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

struct InlayCallSite {
    std::size_t callee_offset{};
    std::vector<std::uint32_t> argument_offsets;
};

[[nodiscard]] std::vector<InlayCallSite> inlay_call_sites(std::string_view text,
                                                          std::size_t range_start,
                                                          std::size_t range_end,
                                                          const std::function<void()>& checkpoint) {
    constexpr std::size_t chunk_bytes = std::size_t{256} * 1024U;

    struct Delimiter {
        char value{};
        std::optional<std::size_t> callee;
        std::vector<std::uint32_t> arguments;
        bool awaiting_argument{};
    };
    std::vector<Delimiter> delimiters;
    std::vector<InlayCallSite> result;
    LexicalState lexical_state = LexicalState::code;
    std::optional<std::size_t> last_identifier;
    const auto identifier = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
    };
    const auto callable_identifier = [&](std::size_t start) -> std::optional<std::size_t> {
        auto end = start;
        while (end < text.size() && identifier(text[end])) {
            ++end;
        }
        const auto name = text.substr(start, end - start);
        static constexpr std::string_view non_call_keywords[] = {
            "if", "for", "while", "switch", "sizeof", "alignof", "return"};
        return std::ranges::find(non_call_keywords, name) == std::ranges::end(non_call_keywords)
                   ? std::optional{start}
                   : std::nullopt;
    };
    const auto template_call_open = [&](std::size_t open) {
        auto state = LexicalState::code;
        std::size_t depth = 1;
        for (auto offset = open + 1; offset < text.size(); ++offset) {
            if ((offset - open) % chunk_bytes == 0) {
                checkpoint();
            }
            const auto character = text[offset];
            if (state == LexicalState::line_comment) {
                if (character == '\r' || character == '\n') {
                    state = LexicalState::code;
                }
                continue;
            }
            if (state == LexicalState::block_comment) {
                if (character == '*' && offset + 1 < text.size() && text[offset + 1] == '/') {
                    state = LexicalState::code;
                    ++offset;
                }
                continue;
            }
            if (state == LexicalState::string || state == LexicalState::character) {
                const auto quote = state == LexicalState::string ? '"' : '\'';
                if (character == '\\' && offset + 1 < text.size()) {
                    ++offset;
                } else if (character == quote) {
                    state = LexicalState::code;
                }
                continue;
            }
            if (character == '/' && offset + 1 < text.size() && text[offset + 1] == '/') {
                state = LexicalState::line_comment;
                ++offset;
            } else if (character == '/' && offset + 1 < text.size() && text[offset + 1] == '*') {
                state = LexicalState::block_comment;
                ++offset;
            } else if (character == '"' || character == '\'') {
                state = character == '"' ? LexicalState::string : LexicalState::character;
            } else if (character == '<') {
                ++depth;
            } else if (character == '>') {
                if (--depth == 0) {
                    while (++offset < text.size()) {
                        if (std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
                            continue;
                        }
                        return text[offset] == '(';
                    }
                    return false;
                }
            } else if (depth == 1 && (character == ';' || character == ')' || character == ']' ||
                                      character == '}')) {
                return false;
            }
        }
        return false;
    };
    const auto begin_argument = [&](std::size_t offset) {
        if (!delimiters.empty() && delimiters.back().callee &&
            delimiters.back().awaiting_argument && text[offset] != ')') {
            if (offset <= std::numeric_limits<std::uint32_t>::max()) {
                delimiters.back().arguments.push_back(static_cast<std::uint32_t>(offset));
            }
            delimiters.back().awaiting_argument = false;
        }
    };

    std::size_t next_checkpoint{};
    for (std::size_t offset = 0; offset < text.size();) {
        if (offset >= next_checkpoint) {
            checkpoint();
            next_checkpoint = offset + chunk_bytes;
        }
        const auto character = text[offset];
        if (lexical_state == LexicalState::line_comment) {
            if (character == '\r' || character == '\n') {
                lexical_state = LexicalState::code;
            }
            ++offset;
            continue;
        }
        if (lexical_state == LexicalState::block_comment) {
            if (character == '*' && offset + 1 < text.size() && text[offset + 1] == '/') {
                lexical_state = LexicalState::code;
                offset += 2;
            } else {
                ++offset;
            }
            continue;
        }
        if (lexical_state == LexicalState::string || lexical_state == LexicalState::character) {
            const auto quote = lexical_state == LexicalState::string ? '"' : '\'';
            if (character == '\\' && offset + 1 < text.size()) {
                offset += 2;
            } else {
                ++offset;
                if (character == quote) {
                    lexical_state = LexicalState::code;
                }
            }
            continue;
        }
        if (character == '/' && offset + 1 < text.size() && text[offset + 1] == '/') {
            lexical_state = LexicalState::line_comment;
            offset += 2;
            continue;
        }
        if (character == '/' && offset + 1 < text.size() && text[offset + 1] == '*') {
            lexical_state = LexicalState::block_comment;
            offset += 2;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            ++offset;
            continue;
        }
        begin_argument(offset);
        if (identifier(character)) {
            last_identifier = offset;
            do {
                ++offset;
                if (offset >= next_checkpoint) {
                    checkpoint();
                    next_checkpoint = offset + chunk_bytes;
                }
            } while (offset < text.size() && identifier(text[offset]));
            continue;
        }
        if (character == '"' || character == '\'') {
            lexical_state = character == '"' ? LexicalState::string : LexicalState::character;
            last_identifier.reset();
            ++offset;
            continue;
        }
        if (character == '(') {
            const auto callee =
                last_identifier ? callable_identifier(*last_identifier) : std::nullopt;
            delimiters.push_back(
                {.value = character, .callee = callee, .arguments = {}, .awaiting_argument = true});
            last_identifier.reset();
        } else if (character == '[' || character == '{' ||
                   (character == '<' && last_identifier && template_call_open(offset))) {
            delimiters.push_back({.value = character,
                                  .callee = character == '<' ? last_identifier : std::nullopt,
                                  .arguments = {},
                                  .awaiting_argument = false});
            last_identifier.reset();
        } else if (character == ',' && !delimiters.empty() && delimiters.back().value == '(' &&
                   delimiters.back().callee) {
            delimiters.back().awaiting_argument = true;
            last_identifier.reset();
        } else if (character == ')' || character == ']' || character == '}' || character == '>') {
            const auto expected =
                character == ')' ? '(' : (character == ']' ? '[' : (character == '}' ? '{' : '<'));
            const auto matching = std::ranges::find(delimiters.rbegin(), delimiters.rend(),
                                                    expected, &Delimiter::value);
            if (matching == delimiters.rend()) {
                last_identifier.reset();
                ++offset;
                continue;
            }
            const auto index =
                static_cast<std::size_t>(std::distance(delimiters.begin(), matching.base() - 1));
            if (character == ')' && delimiters[index].callee) {
                auto arguments = std::move(delimiters[index].arguments);
                const auto relevant = std::ranges::any_of(arguments, [&](std::uint32_t argument) {
                    return argument >= range_start && argument < range_end;
                });
                if (relevant) {
                    result.push_back({.callee_offset = *delimiters[index].callee,
                                      .argument_offsets = std::move(arguments)});
                }
            }
            const auto template_callee = character == '>' ? delimiters[index].callee : std::nullopt;
            delimiters.erase(delimiters.begin() + static_cast<std::ptrdiff_t>(index),
                             delimiters.end());
            last_identifier = template_callee;
        } else {
            last_identifier.reset();
        }
        ++offset;
    }
    return result;
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

[[nodiscard]] bool position_less(workspace::Position left, workspace::Position right) {
    return left.line != right.line ? left.line < right.line : left.character < right.character;
}

// Permissive overlap test used to decide whether a diagnostic's displayed range
// is relevant to a textDocument/codeAction request: ranges that merely touch
// (one's end equals the other's start) count as overlapping so a zero-width
// cursor position or a zero-width diagnostic point still matches, matching how
// LSP clients typically request quick fixes either at a cursor or by clicking a
// squiggle.
[[nodiscard]] bool ranges_overlap(workspace::Range left, workspace::Range right) {
    return !position_less(right.end, left.start) && !position_less(left.end, right.start);
}

// Builds the LSP diagnostic JSON item for `diagnostic`, including opaque
// correlation `data` (document URI, version, analysis generation, and the
// diagnostic's index within that generation's published set) and, when the
// diagnostic carries DXC fix-its, a `code` marking it as fixable. textDocument/
// codeAction never trusts data echoed back by the client; this shape exists so
// the same correlation is visible to the client and reproducible from the
// server's own cache.
[[nodiscard]] Json diagnostic_json(const workspace::SourceSnapshot& snapshot,
                                   const dxc::Diagnostic& diagnostic, std::uint64_t generation,
                                   std::size_t index) {
    Json item = {{"range", lsp_range(diagnostic_range(snapshot, diagnostic))},
                 {"severity", diagnostic_severity(diagnostic.severity)},
                 {"source", "dxc"},
                 {"message", diagnostic.message}};
    if (!diagnostic.fix_its.empty()) {
        item["code"] = "hlsl-lsp/dxc-fix-it";
    }
    item["data"] = {{"uri", snapshot.uri()},
                    {"version", snapshot.version()},
                    {"generation", generation},
                    {"index", index}};
    return item;
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

[[nodiscard]] InlayHintSettings inlay_hint_settings(const Json& settings) {
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
    const Json* nested_hints = nullptr;
    if (hlsl != nullptr) {
        if (const auto hints = hlsl->find("inlayHints"); hints != hlsl->end()) {
            if (!hints->is_object()) {
                invalid_params("hlsl.inlayHints must be an object");
            }
            nested_hints = &*hints;
        }
    }

    const auto boolean = [&](std::string_view name, bool fallback) {
        const Json* value = nullptr;
        if (nested_hints != nullptr) {
            const auto found = nested_hints->find(name);
            if (found != nested_hints->end()) {
                value = &*found;
            }
        }
        if (value == nullptr && hlsl != nullptr) {
            const auto found = hlsl->find("inlayHints." + std::string{name});
            if (found != hlsl->end()) {
                value = &*found;
            }
        }
        if (value == nullptr) {
            const auto found = settings.find("hlsl.inlayHints." + std::string{name});
            if (found != settings.end()) {
                value = &*found;
            }
        }
        if (value == nullptr) {
            return fallback;
        }
        if (!value->is_boolean()) {
            invalid_params("hlsl.inlayHints." + std::string{name} + " must be a boolean");
        }
        return value->get<bool>();
    };

    return {.types = boolean("types", true),
            .parameters = boolean("parameters", true),
            .matrix_orientation = boolean("matrixOrientation", false),
            .registers = boolean("registers", false),
            .packed_offsets = boolean("packedOffsets", false),
            .array_strides = boolean("arrayStrides", false),
            .active_variant = boolean("activeVariant", true)};
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
                                               const dxc::Reference& reference,
                                               std::string_view expected_name = {}) {
    auto start = dxc_offset_at(text, reference.location.line, reference.location.column);
    auto end = start.has_value() ? std::optional{*start + expected_name.size()} : std::nullopt;
    if (expected_name.empty() || !end.has_value() || *end > text.size() ||
        text.substr(*start, expected_name.size()) != expected_name) {
        start = static_cast<std::size_t>(reference.start_offset);
        end = static_cast<std::size_t>(reference.end_offset);
    }
    if (*start > *end || *end > text.size() ||
        (!expected_name.empty() && text.substr(*start, *end - *start) != expected_name)) {
        throw HandlerError{json_rpc::content_modified_code,
                           "A referenced source file changed after analysis"};
    }
    return {.start = workspace::lsp_position_at(text, *start),
            .end = workspace::lsp_position_at(text, *end)};
}

} // namespace

struct Server::ReferenceResult final {
    workspace::SourceSnapshot request;
    dxc::Definition target;
    std::vector<dxc::Reference> references;
};

Server::Server(NotificationSender sender, Logger logger, ServerOptions options,
               RequestSender request_sender)
    : sender_{std::move(sender)}, request_sender_{std::move(request_sender)},
      logger_{std::move(logger)}, options_{std::move(options)},
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
    dispatcher_.register_request_handler("textDocument/prepareCallHierarchy",
                                         [this](const auto& params, const auto& context) {
                                             return prepare_call_hierarchy(params, context);
                                         });
    dispatcher_.register_request_handler("callHierarchy/incomingCalls",
                                         [this](const auto& params, const auto& context) {
                                             return call_hierarchy_incoming_calls(params, context);
                                         });
    dispatcher_.register_request_handler("callHierarchy/outgoingCalls",
                                         [this](const auto& params, const auto& context) {
                                             return call_hierarchy_outgoing_calls(params, context);
                                         });
    dispatcher_.register_request_handler("hlsl/entryPointDataFlow",
                                         [this](const auto& params, const auto& context) {
                                             return entry_point_data_flow(params, context);
                                         });
    dispatcher_.register_request_handler(
        "textDocument/hover",
        [this](const auto& params, const auto& context) { return hover(params, context); });
    dispatcher_.register_request_handler(
        "hlsl/memoryLayout",
        [this](const auto& params, const auto& context) { return memory_layout(params, context); });
    dispatcher_.register_request_handler("hlsl/preprocessorExplorer",
                                         [this](const auto& params, const auto& context) {
                                             return preprocessor_explorer(params, context);
                                         });
    dispatcher_.register_request_handler("hlsl/compilationInfo",
                                         [this](const auto& params, const auto& context) {
                                             return compilation_info(params, context);
                                         });
    dispatcher_.register_request_handler("hlsl/computeVisualization",
                                         [this](const auto& params, const auto& context) {
                                             return compute_visualization(params, context);
                                         });
    dispatcher_.register_request_handler("textDocument/signatureHelp",
                                         [this](const auto& params, const auto& context) {
                                             return signature_help(params, context);
                                         });
    dispatcher_.register_request_handler(
        "textDocument/inlayHint",
        [this](const auto& params, const auto& context) { return inlay_hints(params, context); });
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
    dispatcher_.register_request_handler(
        "textDocument/codeAction",
        [this](const auto& params, const auto& context) { return code_action(params, context); });
    dispatcher_.register_request_handler(
        "workspace/executeCommand", [this](const auto& params) { return execute_command(params); });
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
    InlayHintSettings initial_inlay_hints;
    bool client_command_links = false;
    bool client_inlay_hint_refresh = false;
    if (const auto capabilities = value.find("capabilities");
        capabilities != value.end() && capabilities->is_object()) {
        if (const auto workspace = capabilities->find("workspace");
            workspace != capabilities->end() && workspace->is_object()) {
            if (const auto hints = workspace->find("inlayHint");
                hints != workspace->end() && hints->is_object()) {
                if (const auto refresh = hints->find("refreshSupport");
                    refresh != hints->end() && refresh->is_boolean()) {
                    client_inlay_hint_refresh = refresh->get<bool>();
                }
            }
        }
    }
    if (const auto initialization_options = value.find("initializationOptions");
        initialization_options != value.end() && !initialization_options->is_null()) {
        const auto defaults = configuration_overrides(*initialization_options);
        initial_inlay_hints = inlay_hint_settings(*initialization_options);
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
    inlay_hint_settings_ = initial_inlay_hints;
    command_links_ = client_command_links;
    client_inlay_hint_refresh_ = client_inlay_hint_refresh;
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
        {"callHierarchyProvider", true},
        {"signatureHelpProvider",
         {{"triggerCharacters", Json::array({"(", ","})},
          {"retriggerCharacters", Json::array({")"})}}},
        {"inlayHintProvider", true},
        {"documentSymbolProvider", true},
        {"workspaceSymbolProvider", true},
        {"codeActionProvider", {{"codeActionKinds", Json::array({"quickfix"})}}},
        {"executeCommandProvider", {{"commands", Json::array({"hlsl-lsp.selectVariant"})}}},
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
        locations.push_back(
            {{"uri", target.uri()},
             {"range", lsp_range(reference_range(text, reference, result.target.name))}});
    }
    return locations;
}

std::string Server::text_for_path(std::string_view path) const {
    std::string text;
    try {
        const auto target = workspace::DocumentUri::from_path(std::string{path});
        {
            std::scoped_lock state_lock{state_mutex_};
            if (documents_.contains(target.uri())) {
                text = documents_.snapshot(target.uri()).text();
            }
        }
        if (text.empty()) {
            std::ifstream stream{target.path(), std::ios::binary};
            text = {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }
    } catch (const workspace::DocumentError&) {
        return {};
    }
    return text;
}

Json Server::call_hierarchy_item(const dxc::CallableSymbol& callable, const std::string& root_uri,
                                 const std::string& root_identity, std::int64_t root_version,
                                 std::uint64_t root_generation) const {
    const auto target = workspace::DocumentUri::from_path(callable.location.path);
    const auto text = text_for_path(callable.location.path);
    const auto whole_range = text.empty() ? workspace::Range{} : callable_range(callable, text);
    const auto selection_range =
        text.empty() ? workspace::Range{} : callable_selection_range(callable, text);
    Json data{{"rootUri", root_uri},
              {"rootIdentity", root_identity},
              {"rootVersion", root_version},
              // Pins the exact compiled snapshot this callable was
              // resolved from (see `Manager::content_generation`'s
              // comments): an included-file edit or a configuration/
              // active-variant change can reparse this root without
              // changing `rootVersion`, so `rootVersion` alone cannot
              // detect it.
              {"generation", root_generation},
              {"path", callable.location.path},
              {"line", callable.location.line},
              {"column", callable.location.column},
              {"startOffset", callable.start_offset},
              {"cursorKind", callable.cursor_kind},
              {"name", callable.name}};
    return {{"name", callable.name},
            {"kind", symbol_kind(callable.cursor_kind, callable.name)},
            {"detail", callable.signature},
            {"uri", target.uri()},
            {"range", lsp_range(whole_range)},
            {"selectionRange", lsp_range(selection_range)},
            {"data", std::move(data)}};
}

Json Server::navigable_json(std::string_view name, std::uint32_t cursor_kind,
                            const dxc::SourceLocation& location, std::uint32_t start_offset,
                            std::uint32_t end_offset) const {
    const auto target = workspace::DocumentUri::from_path(location.path);
    const auto text = text_for_path(location.path);
    const auto whole_range =
        text.empty() ? workspace::Range{} : offset_range(text, start_offset, end_offset);
    const auto selection_range =
        text.empty() ? workspace::Range{} : name_selection_range(name, location.offset, text);
    return {{"name", std::string{name}},
            {"kind", symbol_kind(cursor_kind, name)},
            {"uri", target.uri()},
            {"range", lsp_range(whole_range)},
            {"selectionRange", lsp_range(selection_range)}};
}

Server::CallHierarchyItemData Server::parse_call_hierarchy_item_data(const Json& item) {
    if (!item.is_object()) {
        invalid_params("Call hierarchy item must be an object");
    }
    const auto& data = object_member(item, "data");
    CallHierarchyItemData result;
    result.root_uri = string_member(data, "rootUri");
    result.root_identity = string_member(data, "rootIdentity");
    result.root_version = integer_member(data, "rootVersion");
    result.generation = unsigned64_member(data, "generation");
    result.path = string_member(data, "path");
    result.line = unsigned_member(data, "line");
    result.column = unsigned_member(data, "column");
    result.start_offset = unsigned_member(data, "startOffset");
    result.cursor_kind = unsigned_member(data, "cursorKind");
    result.name = string_member(data, "name");
    return result;
}

Json Server::prepare_call_hierarchy(const std::optional<Json>& params,
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
                invalid_params("prepareCallHierarchy document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    const auto [line, column] = dxc_position(snapshot.text(), request_position);
    const auto callable =
        analysis_.callable_at(snapshot.document_uri().identity(), snapshot.version(),
                              snapshot.path(), line, column, context.cancellation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code,
                               "prepareCallHierarchy was superseded"};
        }
    }
    if (!callable.value.has_value()) {
        return nullptr;
    }
    // Builds the full response *before* the final staleness recheck below,
    // not after: `call_hierarchy_item` independently re-fetches the root's
    // (and, when the callable lives in an include, that include's) *live*
    // document text via `text_for_path` to turn `callable`'s offsets into
    // LSP line/column ranges. That fetch is a separately timed read of
    // mutable server state from the atomic `callable_at` call above, so a
    // concurrent edit landing in between could otherwise pair this item's
    // still-old offsets with already-new text -- exactly the "mixed old
    // offsets/new text" hazard the recheck immediately below exists to
    // catch, by rejecting outright whenever anything could have raced
    // *any* part of this construction, rather than only checking beforehand.
    auto item =
        call_hierarchy_item(*callable.value, snapshot.uri(), snapshot.document_uri().identity(),
                            snapshot.version(), callable.generation);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code,
                               "prepareCallHierarchy was superseded"};
        }
    }
    // `callable.generation` was captured atomically alongside `callable.value`
    // (see `WithGeneration`), but the document-version recheck above cannot
    // detect an included-file edit or a configuration/active-variant change
    // that reparses this root without bumping its own version -- exactly the
    // gap `data.generation` exists to close for *later* incoming/outgoing
    // requests. Re-fetching the content generation right *after* fully
    // building the item this handler is about to return closes that same
    // gap for this request too: a reanalysis racing anywhere between the
    // atomic `callable_at` call above and this point -- including during
    // the item construction above -- would otherwise let a returned
    // `CallHierarchyItem` embed a generation, or text-derived ranges, that
    // no longer describe the current analysis.
    if (options_.analysis_hooks && options_.analysis_hooks->before_call_hierarchy_revalidation) {
        options_.analysis_hooks->before_call_hierarchy_revalidation();
    }
    if (analysis_.content_generation(snapshot.document_uri().identity(), snapshot.version(),
                                     context.cancellation) != callable.generation) {
        throw HandlerError{json_rpc::content_modified_code, "prepareCallHierarchy was superseded"};
    }
    return Json::array({std::move(item)});
}

std::uint64_t
Server::validate_call_hierarchy_item(const CallHierarchyItemData& data,
                                     const json_rpc::CancellationToken& cancellation) {
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(data.root_uri) || !documents_.document(data.root_uri).open ||
            documents_.document(data.root_uri).version != data.root_version) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Call hierarchy item is no longer valid"};
        }
    }
    // `data.root_version` alone cannot detect an included-file edit or a
    // configuration/active-variant change: `Manager::analyze` reparses the
    // root's translation unit for either without bumping the root
    // document's own version. Re-resolving the callable at its own stored
    // position and comparing both its content generation and its own
    // identity (start offset, cursor kind, name) against what was captured
    // when this item was built is what actually detects that the stored
    // `data` no longer describes the same symbol in the current analysis,
    // rather than trusting those stored fields unchecked.
    const auto generation = analysis_.verify_call_hierarchy_identity(
        data.root_identity, data.root_version, data.path, data.line, data.column, data.start_offset,
        data.cursor_kind, data.name, cancellation);
    if (!generation.has_value() || *generation != data.generation) {
        throw HandlerError{json_rpc::content_modified_code,
                           "Call hierarchy item no longer matches the analyzed source"};
    }
    return *generation;
}

Json Server::call_hierarchy_outgoing_calls(const std::optional<Json>& params,
                                           const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto data = parse_call_hierarchy_item_data(object_member(value, "item"));

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(data.root_uri) || !documents_.document(data.root_uri).open ||
            documents_.document(data.root_uri).version != data.root_version) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Call hierarchy item is no longer valid"};
        }
    }
    analyze_and_publish(data.root_uri);
    // Validates that `data` still describes the same symbol in the current
    // analysis (see `validate_call_hierarchy_item`'s comment). That check
    // and the `outgoing_calls` query below are still two separately timed
    // Manager operations, so a reanalysis (an included-file edit or a
    // configuration/active-variant change) could in principle land strictly
    // between them -- the explicit generation comparison below, not just
    // the document-version recheck that follows it, is what actually closes
    // that window: it rejects with ContentModified whenever the generation
    // `outgoing_calls` actually computed against differs from the one this
    // validation just confirmed matches `data`, rather than silently
    // returning callees resolved from a different analysis than the one the
    // caller's `data` was validated against.
    static_cast<void>(validate_call_hierarchy_item(data, context.cancellation));
    auto outgoing = analysis_.outgoing_calls(data.root_identity, data.root_version, data.path,
                                             data.line, data.column, context.cancellation);
    auto& calls = outgoing.value;
    if (outgoing.generation != data.generation) {
        throw HandlerError{json_rpc::content_modified_code,
                           "Call hierarchy item no longer matches the analyzed source"};
    }
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(data.root_uri) ||
            documents_.document(data.root_uri).version != data.root_version) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Call hierarchy item is no longer valid"};
        }
    }

    const auto caller_text = text_for_path(data.path);
    Json result = Json::array();
    for (const auto& call : calls) {
        Json from_ranges = Json::array();
        for (const auto& call_site : call.call_sites) {
            if (caller_text.empty()) {
                continue;
            }
            from_ranges.push_back(
                lsp_range(offset_range(caller_text, call_site.start_offset, call_site.end_offset)));
        }
        result.push_back({{"to", call_hierarchy_item(call.callee, data.root_uri, data.root_identity,
                                                     data.root_version, outgoing.generation)},
                          {"fromRanges", std::move(from_ranges)}});
    }
    // The checks above close the window between validation and the
    // `outgoing_calls` query itself, but `caller_text`/each callee's own
    // `call_hierarchy_item` construction re-fetch *live* document text
    // independently, after that query returned -- a further edit landing
    // during this construction (of the caller's own file, or of whichever
    // file a callee happens to be defined in) could otherwise pair
    // offsets computed against `outgoing.generation` with already-newer
    // text. One last generation recheck, strictly after the full response
    // is built, catches that remaining window too.
    if (options_.analysis_hooks && options_.analysis_hooks->before_call_hierarchy_revalidation) {
        options_.analysis_hooks->before_call_hierarchy_revalidation();
    }
    if (analysis_.content_generation(data.root_identity, data.root_version, context.cancellation) !=
        outgoing.generation) {
        throw HandlerError{json_rpc::content_modified_code,
                           "Call hierarchy item no longer matches the analyzed source"};
    }
    return result;
}

Json Server::call_hierarchy_incoming_calls(const std::optional<Json>& params,
                                           const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto data = parse_call_hierarchy_item_data(object_member(value, "item"));

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(data.root_uri) || !documents_.document(data.root_uri).open ||
            documents_.document(data.root_uri).version != data.root_version) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Call hierarchy item is no longer valid"};
        }
    }
    analyze_and_publish(data.root_uri);
    // This item's own root/identity/position must still be valid. The
    // generation returned here describes exactly this validation check's
    // own, separately timed query; it is compared explicitly below against
    // whichever candidate root turns out to be `data.root_identity` itself
    // (see the loop), since that is the only candidate for which "the
    // generation this item was built against" is even a meaningful
    // expectation -- every other candidate root has its own, entirely
    // independent generation namespace (see `Manager::content_generation`'s
    // comment on what a generation counts), so there is nothing to compare
    // its generation to a priori and each simply uses its own current one.
    static_cast<void>(validate_call_hierarchy_item(data, context.cancellation));

    // Incoming callers can live in any currently open root that resolves the
    // target's own file as part of its translation unit -- not only the
    // root the item happened to be prepared from -- exactly the same
    // cross-root expansion `find_references` already performs for
    // textDocument/references. `data.path` must still be parseable into a
    // valid document identity; malformed input is rejected here, before any
    // root is queried, rather than surfacing as an obscure internal error
    // later.
    try {
        static_cast<void>(workspace::DocumentUri::from_path(data.path).identity());
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }

    struct Accumulated {
        dxc::CallableSymbol caller;
        std::string root_uri;
        std::string root_identity;
        std::int64_t root_version{};
        std::uint64_t generation{};
        std::vector<dxc::Reference> call_sites;
    };
    // Keyed by (caller path, caller start offset, *root identity*): two
    // different root/config contexts (different `#define`s, a different
    // active variant, or simply an unrelated other root) can each report a
    // caller at the very same textual path+offset while having resolved it
    // against incompatible compiler contexts (different macro expansions,
    // different reachable overloads, etc.). Keying on root identity too
    // keeps every context's result in its own distinct entry -- with its
    // own metadata and generation -- instead of silently retaining only the
    // first root's metadata while splicing in call sites computed under a
    // different context's analysis.
    //
    // A structured key (rather than a delimiter-joined string) is used
    // deliberately: `path` and `root_identity` are filesystem paths, which
    // on this platform (and in general) can themselves contain ':'
    // (e.g. a Windows drive letter, `C:\...`). Concatenating
    // `path + ':' + offset + ':' + root_identity` is therefore not
    // injective -- distinct (path, offset, root_identity) triples can
    // produce the identical joined string (for example, one entry's path
    // absorbing what another entry intended as its offset/root-identity
    // separator), which would silently merge two unrelated callers'
    // `call_sites` into one entry. Comparing/hashing the three fields
    // individually has no such ambiguity.
    struct AccumulatedKey {
        std::string path;
        std::uint32_t start_offset{};
        std::string root_identity;

        [[nodiscard]] bool operator==(const AccumulatedKey&) const = default;
    };
    struct AccumulatedKeyHash {
        [[nodiscard]] std::size_t operator()(const AccumulatedKey& key) const noexcept {
            std::size_t seed = std::hash<std::string>{}(key.path);
            seed ^= std::hash<std::uint32_t>{}(key.start_offset) + 0x9e3779b9 + (seed << 6) +
                    (seed >> 2);
            seed ^= std::hash<std::string>{}(key.root_identity) + 0x9e3779b9 + (seed << 6) +
                    (seed >> 2);
            return seed;
        }
    };
    std::unordered_map<AccumulatedKey, Accumulated, AccumulatedKeyHash> accumulated;
    // Every candidate root's query result, and the identity/version/
    // generation it was queried against, keyed by root identity,
    // independent of whether that query happened to yield any calls.
    // Recorded so the final revalidation pass after full response
    // construction can cover every root this response's correctness
    // actually depended on, not only the subset that happened to
    // contribute a caller (see that pass's own comment for why an
    // empty-result root still needs this).
    std::unordered_map<std::string, std::pair<std::int64_t, std::uint64_t>>
        queried_root_generations;

    // Every currently open root is queried, unconditionally -- there is no
    // "is this root even a plausible candidate" filter here (there
    // deliberately was one keyed on `dependency_identities`/
    // `has_dynamic_includes`, and it was removed; see below). `roots()`
    // returns a *copied snapshot* of each root's metadata, taken once
    // before this loop starts; a root copied into that snapshot as
    // "definitely not dependent on the target file" can be edited (a new
    // `#include` added) or reconfigured (active variant/`#define`s
    // changed) an instant later -- concurrently with, or strictly between,
    // this snapshot and this loop reaching that root -- making it a real
    // caller that a filter based on the stale copy would wrongly skip.
    // Because a skipped root is never queried, it is also never added to
    // `queried_root_generations` above, so the final revalidation pass
    // could not have caught the omission either: the root's true
    // dependency on the target only becomes visible once it is actually
    // queried against its *current* analysis. Querying every root
    // unconditionally removes that window entirely: every root's
    // dependency on the target is settled by its own `incoming_calls`
    // query against live, current analysis, not by trusting a
    // point-in-time metadata copy. This is not a new source of
    // unboundedness: `Server::workspace_symbols` already queries every
    // root in `analysis_.roots()` unconditionally (no dependency filter at
    // all), so the accepted bounded contract for "iterate every currently
    // open root" already exists elsewhere in this file -- the set is
    // bounded by however many roots the client currently has open, exactly
    // as it always has been for every exhaustive-root handler.
    for (const auto& root : analysis_.roots()) {
        context.cancellation.throw_if_cancellation_requested();
        if (options_.analysis_hooks &&
            options_.analysis_hooks->before_call_hierarchy_candidate_root) {
            options_.analysis_hooks->before_call_hierarchy_candidate_root(root.root_identity);
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
        auto incoming =
            analysis_.incoming_calls(root.root_identity, root_snapshot.version(), data.path,
                                     data.line, data.column, context.cancellation);
        auto& calls = incoming.value;
        queried_root_generations.insert_or_assign(
            root.root_identity, std::pair{root_snapshot.version(), incoming.generation});
        // Only meaningful when this candidate root *is* the item's own
        // root: closes the same TOCTOU window `outgoing_calls` guards
        // against above -- a reanalysis of `data.root_identity` landing
        // strictly between `validate_call_hierarchy_item` and this query
        // must not be allowed to silently serialize results computed
        // against a different analysis than the one `data` was validated
        // against.
        if (root.root_identity == data.root_identity && incoming.generation != data.generation) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Call hierarchy item no longer matches the analyzed source"};
        }
        {
            std::scoped_lock state_lock{state_mutex_};
            if (!documents_.contains(root_snapshot.uri()) ||
                !documents_.document(root_snapshot.uri()).open ||
                documents_.document(root_snapshot.uri()).version != root_snapshot.version()) {
                throw HandlerError{json_rpc::content_modified_code,
                                   "A referenced root changed during analysis"};
            }
        }
        for (auto& call : calls) {
            const AccumulatedKey key{.path = call.caller.location.path,
                                     .start_offset = call.caller.start_offset,
                                     .root_identity = root.root_identity};
            auto existing = accumulated.find(key);
            if (existing == accumulated.end()) {
                accumulated.emplace(key, Accumulated{.caller = call.caller,
                                                     .root_uri = root_snapshot.uri(),
                                                     .root_identity = root.root_identity,
                                                     .root_version = root_snapshot.version(),
                                                     .generation = incoming.generation,
                                                     .call_sites = std::move(call.call_sites)});
            } else {
                // Both entries share this root/generation (same key), so
                // merging their call sites is safe here -- unlike merging
                // across different roots, this cannot mix incompatible
                // compiler contexts.
                existing->second.call_sites.insert(existing->second.call_sites.end(),
                                                   std::make_move_iterator(call.call_sites.begin()),
                                                   std::make_move_iterator(call.call_sites.end()));
            }
        }
    }

    std::vector<Accumulated> ordered;
    ordered.reserve(accumulated.size());
    for (auto& [key, accumulated_entry] : accumulated) {
        static_cast<void>(key);
        ordered.push_back(std::move(accumulated_entry));
    }
    std::ranges::sort(ordered, [](const auto& left, const auto& right) {
        return std::tie(left.caller.location.path, left.caller.start_offset, left.root_identity) <
               std::tie(right.caller.location.path, right.caller.start_offset, right.root_identity);
    });

    Json result = Json::array();
    for (auto& entry : ordered) {
        std::ranges::sort(entry.call_sites, {}, &dxc::Reference::start_offset);
        // Two candidate-root iterations can both report the very same
        // physical call site (e.g. an overload resolved identically on
        // both sides of a header boundary); dedupe by offset range so a
        // single caller reference is never shown twice for one entry.
        entry.call_sites.erase(std::unique(entry.call_sites.begin(), entry.call_sites.end(),
                                           [](const auto& left, const auto& right) {
                                               return left.start_offset == right.start_offset &&
                                                      left.end_offset == right.end_offset;
                                           }),
                               entry.call_sites.end());
        const auto caller_text = text_for_path(entry.caller.location.path);
        Json from_ranges = Json::array();
        for (const auto& call_site : entry.call_sites) {
            if (caller_text.empty()) {
                continue;
            }
            from_ranges.push_back(
                lsp_range(offset_range(caller_text, call_site.start_offset, call_site.end_offset)));
        }
        result.push_back(
            {{"from", call_hierarchy_item(entry.caller, entry.root_uri, entry.root_identity,
                                          entry.root_version, entry.generation)},
             {"fromRanges", std::move(from_ranges)}});
    }

    // The per-root checks in the loop above only close the window between
    // *that root's own* query and the next root's; the final serialization
    // pass just above (each entry's own `call_hierarchy_item`/`caller_text`
    // construction, which independently re-fetches *live* document text)
    // runs strictly after every root has already been queried, so a
    // reanalysis of *any* queried root landing during that pass, or during
    // the earlier processing of a later root in the loop, would not
    // otherwise be caught. Revalidate every candidate root this response
    // actually *queried* -- `queried_root_generations`, captured immediately
    // after each root's own `incoming_calls` query above -- not merely the
    // subset that happened to contribute a caller: a candidate root that
    // legitimately returned zero callers at query time is just as capable
    // of being reanalyzed strictly afterward (e.g. a concurrent edit adds a
    // new call from that root) as one that did contribute, and omitting it
    // here would let a now-stale "no callers from this root" silently
    // stand instead of being rejected as `ContentModified`.
    if (options_.analysis_hooks && options_.analysis_hooks->before_call_hierarchy_revalidation) {
        options_.analysis_hooks->before_call_hierarchy_revalidation();
    }
    for (const auto& [root_identity, version_and_generation] : queried_root_generations) {
        context.cancellation.throw_if_cancellation_requested();
        const auto& [root_version, expected_generation] = version_and_generation;
        if (analysis_.content_generation(root_identity, root_version, context.cancellation) !=
            expected_generation) {
            throw HandlerError{json_rpc::content_modified_code,
                               "A referenced root changed during analysis"};
        }
    }
    return result;
}

Json Server::entry_point_data_flow(const std::optional<Json>& params,
                                   const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("hlsl/entryPointDataFlow document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    // `flow` and `generation` are fetched together, atomically, from the
    // same serialized manager operation (see `WithGeneration`'s comment):
    // fetching them via two separate `Manager` calls could straddle a
    // concurrent reanalysis (an included file edit or a
    // configuration/active-variant change reparses without bumping the
    // root's own document version) and tag this result with a generation
    // describing a *different* analysis than the one that actually
    // produced it.
    dxc::EntryPointDataFlowLimits limits;
    limits.max_unused_declaration_candidates = 16;
    limits.max_definitions_collected = 1024;
    const auto flow_with_generation = analysis_.entry_point_data_flow(
        snapshot.document_uri().identity(), snapshot.version(), limits, context.cancellation);
    const auto& flow = flow_with_generation.value;
    const auto generation = flow_with_generation.generation;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code,
                               "hlsl/entryPointDataFlow was superseded"};
        }
    }
    const auto root_uri = snapshot.uri();
    const auto root_identity = snapshot.document_uri().identity();
    const auto root_version = snapshot.version();

    Json result{{"found", flow.found}, {"explanation", flow.explanation}};
    if (flow.entry_point.has_value()) {
        result["entryPoint"] = call_hierarchy_item(*flow.entry_point, root_uri, root_identity,
                                                   root_version, generation);
    } else {
        // `Json{nullptr}` would construct a one-element array via the
        // initializer-list constructor; assignment is used instead so this
        // is unambiguously a JSON null.
        result["entryPoint"] = nullptr;
    }

    Json reachable = Json::array();
    for (const auto& node : flow.reachable_functions) {
        reachable.push_back(
            {{"function", call_hierarchy_item(node.function, root_uri, root_identity, root_version,
                                              generation)},
             {"depth", node.depth},
             {"recursive", node.recursive}});
    }
    result["reachableFunctions"] = std::move(reachable);

    Json unreachable = Json::array();
    for (const auto& function : flow.unreachable_functions) {
        unreachable.push_back(
            call_hierarchy_item(function, root_uri, root_identity, root_version, generation));
    }
    result["unreachableFunctions"] = std::move(unreachable);

    Json unused = Json::array();
    for (const auto& declaration : flow.unused_declarations) {
        unused.push_back(navigable_json(declaration.name, declaration.cursor_kind,
                                        declaration.location, declaration.start_offset,
                                        declaration.end_offset));
    }
    result["unusedDeclarations"] = std::move(unused);

    Json accesses = Json::array();
    for (const auto& access : flow.global_accesses) {
        auto entry = navigable_json(access.name, access.cursor_kind, access.location,
                                    access.start_offset, access.end_offset);
        entry["qualifiedName"] = access.qualified_name;
        entry["access"] = access.access == dxc::GlobalAccessKind::read    ? "read"
                          : access.access == dxc::GlobalAccessKind::write ? "write"
                                                                          : "readWrite";
        accesses.push_back(std::move(entry));
    }
    result["globalAccesses"] = std::move(accesses);

    result["truncated"] = flow.truncated;
    // Distinct per-phase truncation reasons: `truncated` alone cannot tell a
    // client which section(s) of this response may be incomplete, and the
    // four causes are independent (hitting one does not imply the others).
    // A client rendering `unreachableFunctions` as authoritative dead-code
    // information, for example, needs `functionsVisitedTruncated` *and*
    // `definitionsTruncated` specifically, not the blanket `truncated`
    // flag, since `globalAccessesTruncated`/`unusedDeclarationsTruncated`
    // do not affect that section's completeness at all.
    result["functionsVisitedTruncated"] = flow.functions_visited_truncated;
    result["definitionsTruncated"] = flow.definitions_truncated;
    result["globalAccessesTruncated"] = flow.global_accesses_truncated;
    result["unusedDeclarationsTruncated"] = flow.unused_declarations_truncated;
    result["functionsVisited"] = flow.functions_visited;

    // Serializing the traversal above (potentially thousands of reachable
    // functions/accesses/declarations) takes long enough that a reanalysis
    // could complete strictly between the atomic `entry_point_data_flow`
    // fetch above and this point, even though the version recheck right
    // after that fetch already passed. Re-verifying the content generation
    // one last time, immediately before handing this JSON back to the
    // caller, closes that remaining window: every `CallHierarchyItem.data`
    // embedded above was tagged with `generation`, so if the root's current
    // generation has since moved on, this response must be rejected rather
    // than returned describing an analysis that is no longer current.
    if (analysis_.content_generation(root_identity, root_version, context.cancellation) !=
        generation) {
        throw HandlerError{json_rpc::content_modified_code,
                           "hlsl/entryPointDataFlow was superseded"};
    }
    return result;
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
        const auto range = reference_range(result.request.text(), reference, result.target.name);
        const auto start = workspace::utf8_offset_at(result.request.text(), range.start);
        const auto end = workspace::utf8_offset_at(result.request.text(), range.end);
        if (workspace::DocumentUri::from_path(reference.location.path).identity() ==
                result.request.document_uri().identity() &&
            start <= offset && offset <= end) {
            return {{"range", lsp_range(range)}, {"placeholder", result.target.name}};
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
            edits.push_back(
                {{"range", lsp_range(reference_range(file.text, reference, result.target.name))},
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

Json Server::preprocessor_explorer(const std::optional<Json>& params,
                                   const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");

    const auto snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Preprocessor explorer document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    std::vector<workspace::SourceSnapshot> open_documents;
    ConfigurationState configuration_state;
    {
        std::scoped_lock state_lock{state_mutex_};
        open_documents = documents_.open_snapshots();
        configuration_state =
            ConfigurationState{.editor_settings = editor_settings_,
                               .client_default_language_version = client_default_language_version_,
                               .active_variant = active_variant_,
                               .workspace_folders = workspace_folders_};
    }
    context.cancellation.throw_if_cancellation_requested();

    const auto configuration = configuration_for(snapshot, configuration_state);
    auto resolution = workspace::resolve_includes(snapshot, open_documents, configuration);
    analyze_and_publish(snapshot.uri());
    bool skipped_regions_available = true;
    std::string skipped_regions_unavailable_reason;
    if (resolution.has_rewritten_sources && !dxc::supports_skipped_ranges_for_rewritten_sources()) {
        skipped_regions_available = false;
        skipped_regions_unavailable_reason =
            "Compiler skipped-region analysis is unavailable because DXC 1.9's Linux "
            "GetSkippedRanges API is unsafe for source buffers whose virtual includes were "
            "rewritten to physical paths. Resolver include metadata, configured macro "
            "provenance, compiler-reported source macros, and effective settings remain "
            "available.";
    }
    const auto skipped = skipped_regions_available
                             ? analysis_.skipped_ranges(snapshot.document_uri().identity(),
                                                        snapshot.version(), context.cancellation)
                             : std::vector<dxc::SourceRange>{};
    const auto compiler_macros = analysis_.macro_definitions(
        snapshot.document_uri().identity(), snapshot.version(), context.cancellation);

    std::unordered_map<std::string, std::string> compiler_source_texts;
    for (const auto& source : resolution.sources) {
        compiler_source_texts.try_emplace(
            std::filesystem::path{source.path}.lexically_normal().generic_string(), source.text);
    }

    const auto status_name = [](workspace::IncludeResolution::Status status) {
        switch (status) {
        case workspace::IncludeResolution::Status::resolved:
            return "resolved";
        case workspace::IncludeResolution::Status::missing:
            return "missing";
        case workspace::IncludeResolution::Status::cyclic:
            return "cyclic";
        case workspace::IncludeResolution::Status::dynamic:
            return "dynamic";
        }
        return "missing";
    };

    Json files = Json::array();
    for (const auto& file : resolution.files) {
        Json includes = Json::array();
        for (const auto& include : file.includes) {
            workspace::Position include_position{};
            if (include.path_offset <= file.source_text.size()) {
                include_position =
                    workspace::lsp_position_at(file.source_text, include.path_offset);
            }
            Json item{{"path", include.requested_path},
                      {"line", include_position.line},
                      {"character", include_position.character},
                      {"kind", include.macro_expanded ? "macro"
                               : include.quoted       ? "quoted"
                                                      : "angled"},
                      {"status", status_name(include.status)}};
            if (!include.expanded_path.empty()) {
                item["expandedPath"] = include.expanded_path;
            }
            if (!include.configuration_macro.empty()) {
                item["configurationMacro"] = include.configuration_macro;
            }
            if (!include.configuration_origin.empty()) {
                item["configurationOrigin"] = include.configuration_origin;
            }
            if (!include.configuration_origin_file.empty()) {
                item["configurationOriginUri"] =
                    workspace::DocumentUri::from_path(include.configuration_origin_file).uri();
            }
            if (!include.resolved_path.empty()) {
                item["resolvedUri"] =
                    workspace::DocumentUri::from_path(include.resolved_path).uri();
                item["logicalPath"] = include.logical_path;
            }
            if (!include.virtual_mapping.empty()) {
                item["mapping"] = include.virtual_mapping;
            }
            includes.push_back(std::move(item));
        }
        files.push_back({{"uri", workspace::DocumentUri::from_path(file.physical_path).uri()},
                         {"logicalPath", file.logical_path},
                         {"physicalPath", file.physical_path},
                         {"source", file.open ? "open" : "disk"},
                         {"includes", std::move(includes)}});
    }

    Json skipped_regions = Json::array();
    for (const auto& range : skipped) {
        const auto path =
            std::filesystem::path{range.start.path}.lexically_normal().generic_string();
        const auto text = compiler_source_texts.find(path);
        workspace::Position start{};
        workspace::Position end{};
        if (text != compiler_source_texts.end() && range.start.offset <= text->second.size() &&
            range.end.offset <= text->second.size()) {
            start = workspace::lsp_position_at(text->second, range.start.offset);
            end = workspace::lsp_position_at(text->second, range.end.offset);
        } else {
            start = {.line = range.start.line > 0 ? range.start.line - 1 : 0,
                     .character = range.start.column > 0 ? range.start.column - 1 : 0};
            end = {.line = range.end.line > 0 ? range.end.line - 1 : 0,
                   .character = range.end.column > 0 ? range.end.column - 1 : 0};
        }
        skipped_regions.push_back(
            {{"uri", workspace::DocumentUri::from_path(range.start.path).uri()},
             {"start", lsp_position(start)},
             {"end", lsp_position(end)}});
    }

    const auto setting_origin = [&configuration](std::string_view name,
                                                 std::string_view fallback) -> std::string_view {
        const auto found = configuration.setting_origins.find(name);
        return found != configuration.setting_origins.end() ? std::string_view{found->second}
                                                            : fallback;
    };
    Json macros = Json::array();
    for (const auto& macro : compiler_macros) {
        macros.push_back(
            {{"name", macro.name},
             {"value", macro.value},
             {"source", "compiler"},
             {"origin", macro.location.path},
             {"uri", workspace::DocumentUri::from_path(macro.location.path).uri()},
             {"line", macro.location.line > 0 ? macro.location.line - 1 : 0},
             {"character", macro.location.column > 0 ? macro.location.column - 1 : 0}});
    }
    for (const auto& [name, macro_value] : configuration.preprocessor_definitions) {
        const auto origin = configuration.definition_origins.find(name);
        Json macro{{"name", name},
                   {"value", macro_value},
                   {"source", "configuration"},
                   {"origin", origin != configuration.definition_origins.end() ? origin->second
                                                                               : "configuration"}};
        if (const auto origin_file = configuration.definition_origin_files.find(name);
            origin_file != configuration.definition_origin_files.end()) {
            macro["originUri"] =
                workspace::DocumentUri::from_path(origin_file->second.generic_string()).uri();
        }
        macros.push_back(std::move(macro));
    }

    Json settings = Json::array();
    const auto add_setting = [&settings, &configuration](std::string_view name,
                                                         const Json& setting_value,
                                                         std::string_view origin) {
        Json setting{{"name", name}, {"value", setting_value}, {"origin", std::string{origin}}};
        if (const auto origin_file = configuration.setting_origin_files.find(name);
            origin_file != configuration.setting_origin_files.end()) {
            setting["originUri"] =
                workspace::DocumentUri::from_path(origin_file->second.generic_string()).uri();
        }
        settings.push_back(std::move(setting));
    };
    add_setting("languageVersion", configuration.language_version.value_or("2021"),
                setting_origin("languageVersion", "built-in default"));
    add_setting("targetProfile", configuration.target_profile.value_or(""),
                setting_origin("targetProfile", "not configured"));
    add_setting("entryPoint", configuration.entry_point.value_or(""),
                setting_origin("entryPoint", "not configured"));
    Json include_directories = Json::array();
    for (const auto& directory : configuration.additional_include_directories) {
        include_directories.push_back(directory.generic_string());
    }
    add_setting("includeDirectories", include_directories,
                setting_origin("includeDirectories", "not configured"));
    Json virtual_mappings = Json::object();
    for (const auto& [prefix, directory] : configuration.virtual_directory_mappings) {
        virtual_mappings[prefix] = directory.generic_string();
    }
    add_setting("virtualDirectoryMappings", virtual_mappings,
                setting_origin("virtualDirectoryMappings", "not configured"));
    add_setting("additionalArguments", configuration.additional_arguments,
                setting_origin("additionalArguments", "not configured"));

    Json diagnostics = Json::array();
    if (resolution.has_dynamic_includes) {
        diagnostics.push_back(
            "Source-defined, function-like, undefined, cyclic, malformed, oversized, or "
            "multi-token include expressions, and configured macros changed or obscured by "
            "additional compiler arguments, remain compiler-owned and are shown without "
            "fabricating a resolved path.");
    }
    if (!skipped_regions_available) {
        diagnostics.push_back(skipped_regions_unavailable_reason);
    }

    Json skipped_regions_capability{{"available", skipped_regions_available}};
    if (!skipped_regions_available) {
        skipped_regions_capability["reason"] = skipped_regions_unavailable_reason;
    }
    Json compiler_analysis{
        {"skippedRegions", std::move(skipped_regions_capability)},
        {"compilerMacros", {{"available", true}}},
    };

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code,
                               "Preprocessor explorer was superseded"};
        }
    }
    return {{"rootUri", snapshot.uri()},
            {"files", std::move(files)},
            {"skippedRegions", std::move(skipped_regions)},
            {"macros", std::move(macros)},
            {"settings", std::move(settings)},
            {"compilerAnalysis", std::move(compiler_analysis)},
            {"diagnostics", std::move(diagnostics)}};
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

Json Server::compute_visualization(const std::optional<Json>& params,
                                   const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    std::optional<ComputeDimensions> requested_dispatch;
    if (const auto item = value.find("dispatchDimensions"); item != value.end()) {
        requested_dispatch = compute_dimensions(*item, "dispatchDimensions");
        static_cast<void>(dimension_product(*requested_dispatch, "dispatchDimensions"));
    }
    std::optional<ComputeHardwareProfile> hardware;
    if (const auto item = value.find("hardwareProfile"); item != value.end()) {
        hardware = compute_hardware_profile(*item);
    }

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("hlsl/computeVisualization document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    analyze_and_publish(snapshot.uri());
    const auto info_with_generation = analysis_.compilation_info_with_generation(
        snapshot.document_uri().identity(), snapshot.version(), snapshot.path(),
        context.cancellation);
    const auto& info = info_with_generation.value;
    const auto generation = info_with_generation.generation;
    const auto root_identity = snapshot.document_uri().identity();

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code,
                               "hlsl/computeVisualization was superseded"};
        }
    }

    const bool found = !info.entry_point.empty();
    Json result{
        {"applicable", false},
        {"found", found},
        {"explanation", ""},
        {"entryPoint", info.entry_point},
        {"stage", info.stage},
        {"targetProfile", info.target_profile},
        {"threadGroupSize", nullptr},
        {"dispatchDimensions", nullptr},
        {"groupCount", nullptr},
        {"launchedThreads", nullptr},
        {"inactiveThreads", nullptr},
        {"systemValues", Json::array()},
        {"barriers",
         {{"available", false},
          {"unavailableReason",
           "Barrier instruction reflection is unavailable until a compute shader is "
           "successfully compiled to reflected DXIL."},
          {"instructionCount", nullptr},
          {"locationsAvailable", false},
          {"locationsTruncated", false},
          {"locationsUnavailableReason",
           "Barrier source locations are not exposed by the current DXC reflection and "
           "cursor interfaces."},
          {"locations", Json::array()}}},
        {"groupShared",
         {{"available", false},
          {"unavailableReason",
           "Compiler-authoritative groupshared declaration sizes are not exposed by the "
           "current DXC reflection, cursor, type, or layout interfaces; source text is "
           "never parsed or guessed."},
          {"totalBytes", nullptr},
          {"totalBytesUnavailableReason",
           "Group-shared metadata is unavailable until a compute shader is analyzed."},
          {"truncated", false},
          {"declarations", Json::array()}}},
        {"waveSize",
         {{"known", false},
          {"min", nullptr},
          {"max", nullptr},
          {"preferred", nullptr},
          {"minMaxSource", nullptr},
          {"preferredSource", nullptr},
          {"explanation", "Stable PSV0 wave-size metadata is unavailable until a compute shader is "
                          "successfully compiled to DXIL."}}},
        {"occupancy", nullptr}};

    if (!found) {
        result["explanation"] =
            "No effective entry point is configured for the open document or active variant.";
    } else if (info.stage != "compute") {
        result["explanation"] = "The effective configured entry point targets the '" + info.stage +
                                "' stage, not compute.";
    } else if (!info.success) {
        result["explanation"] =
            "DXC could not compile the effective configured compute entry point.";
    } else if (!info.reflection.has_value() || !info.reflection->available) {
        result["explanation"] =
            info.reflection.has_value() && !info.reflection->unavailable_reason.empty()
                ? info.reflection->unavailable_reason
                : "DXC reflection is unavailable for the compiled compute entry point.";
    } else if (!info.reflection->thread_group_size.has_value() ||
               info.reflection->thread_group_size->x == 0 ||
               info.reflection->thread_group_size->y == 0 ||
               info.reflection->thread_group_size->z == 0) {
        result["explanation"] = "DXC reflection did not provide a valid compute thread-group size.";
    } else {
        const auto& reflected = *info.reflection->thread_group_size;
        const ComputeDimensions thread_group{.x = reflected.x, .y = reflected.y, .z = reflected.z};
        const auto dispatch = requested_dispatch.value_or(thread_group);
        const ComputeDimensions groups{.x = ceil_divide(dispatch.x, thread_group.x),
                                       .y = ceil_divide(dispatch.y, thread_group.y),
                                       .z = ceil_divide(dispatch.z, thread_group.z)};
        const auto threads_per_group = dimension_product(thread_group, "threadGroupSize");
        const auto logical_threads = dimension_product(dispatch, "dispatchDimensions");
        const auto group_total = dimension_product(groups, "groupCount");
        const auto launched_threads =
            checked_multiply(group_total, threads_per_group, "launchedThreads");

        result["applicable"] = true;
        result["explanation"] =
            "Computed from the effective configured entry point and DXC reflection.";
        result["threadGroupSize"] = compute_dimensions_json(thread_group);
        result["dispatchDimensions"] = compute_dimensions_json(dispatch);
        result["groupCount"] = compute_dimensions_json(groups);
        result["launchedThreads"] = launched_threads;
        result["inactiveThreads"] =
            checked_json_integer(launched_threads - logical_threads, "inactiveThreads");
        result["systemValues"] = Json::array(
            {Json{{"semantic", "SV_DispatchThreadID"},
                  {"name", "dispatchThreadId"},
                  {"formula", "groupId * numthreads + groupThreadId"},
                  {"description", "Global dispatch-space thread coordinate."}},
             Json{{"semantic", "SV_GroupID"},
                  {"name", "groupId"},
                  {"formula", "dispatch group coordinate"},
                  {"description", "Zero-based thread-group coordinate."}},
             Json{{"semantic", "SV_GroupThreadID"},
                  {"name", "groupThreadId"},
                  {"formula", "thread coordinate within [0, numthreads)"},
                  {"description", "Zero-based coordinate within the current group."}},
             Json{{"semantic", "SV_GroupIndex"},
                  {"name", "groupIndex"},
                  {"formula", "groupThreadId.x + groupThreadId.y * numthreads.x + "
                              "groupThreadId.z * numthreads.x * numthreads.y"},
                  {"description", "Flattened zero-based index within the current group."}}});
        result["barriers"] =
            Json{{"available", true},
                 {"unavailableReason", ""},
                 {"instructionCount", info.reflection->barrier_instruction_count},
                 {"locationsAvailable", false},
                 {"locationsTruncated", false},
                 {"locationsUnavailableReason",
                  "DXC reflection reports the compiled barrier instruction count but does not "
                  "expose reliable source locations for those instructions; locations are never "
                  "guessed from source text."},
                 {"locations", Json::array()}};

        const auto text_for_path = [this](const std::string& path) {
            const auto target_uri = workspace::DocumentUri::from_path(path).uri();
            {
                std::scoped_lock state_lock{state_mutex_};
                if (documents_.contains(target_uri)) {
                    return documents_.snapshot(target_uri).text();
                }
            }
            std::ifstream stream{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{stream},
                               std::istreambuf_iterator<char>{}};
        };
        if (info.compute_metadata.has_value()) {
            const auto& metadata = *info.compute_metadata;
            Json locations = Json::array();
            for (const auto& location : metadata.barrier_locations) {
                const auto text = text_for_path(location.location.path);
                locations.push_back(
                    {{"label", location.label},
                     {"uri", workspace::DocumentUri::from_path(location.location.path).uri()},
                     {"range", lsp_range(text.empty() ? workspace::Range{}
                                                      : offset_range(text, location.start_offset,
                                                                     location.end_offset))}});
            }
            result["barriers"]["locationsAvailable"] = metadata.barrier_locations_available;
            result["barriers"]["locationsTruncated"] = metadata.barrier_locations_truncated;
            result["barriers"]["locationsUnavailableReason"] =
                metadata.barrier_locations_unavailable_reason;
            result["barriers"]["locations"] = std::move(locations);

            Json declarations = Json::array();
            for (const auto& declaration : metadata.group_shared_declarations) {
                const auto text = text_for_path(declaration.location.path);
                declarations.push_back(
                    {{"name", declaration.name},
                     {"type", declaration.type},
                     {"declaration", declaration.declaration},
                     {"bytes",
                      declaration.bytes.has_value() ? Json(*declaration.bytes) : Json(nullptr)},
                     {"sizeUnavailableReason", declaration.size_unavailable_reason},
                     {"uri", workspace::DocumentUri::from_path(declaration.location.path).uri()},
                     {"range", lsp_range(text.empty() ? workspace::Range{}
                                                      : offset_range(text, declaration.start_offset,
                                                                     declaration.end_offset))}});
            }
            result["groupShared"] = Json{
                {"available", metadata.group_shared_available},
                {"unavailableReason", metadata.group_shared_unavailable_reason},
                {"totalBytes", metadata.group_shared_total_bytes.has_value()
                                   ? Json(checked_json_integer(*metadata.group_shared_total_bytes,
                                                               "groupShared.totalBytes"))
                                   : Json(nullptr)},
                {"totalBytesUnavailableReason",
                 metadata.group_shared_total_bytes_unavailable_reason},
                {"truncated", metadata.group_shared_truncated},
                {"declarations", std::move(declarations)}};
            result["waveSize"] =
                Json{{"known", metadata.wave_size.known},
                     {"min", metadata.wave_size.min.has_value() ? Json(*metadata.wave_size.min)
                                                                : Json(nullptr)},
                     {"max", metadata.wave_size.max.has_value() ? Json(*metadata.wave_size.max)
                                                                : Json(nullptr)},
                     {"preferred", metadata.wave_size.preferred.has_value()
                                       ? Json(*metadata.wave_size.preferred)
                                       : Json(nullptr)},
                     {"minMaxSource", metadata.wave_size.min_max_source.empty()
                                          ? Json(nullptr)
                                          : Json(metadata.wave_size.min_max_source)},
                     {"preferredSource", metadata.wave_size.preferred_source.empty()
                                             ? Json(nullptr)
                                             : Json(metadata.wave_size.preferred_source)},
                     {"explanation", metadata.wave_size.explanation}};
        }

        if (hardware.has_value()) {
            std::uint64_t resident_groups{};
            Json limiting_factors = Json::array();
            Json assumptions = Json::array();
            if (threads_per_group > hardware->max_threads_per_group) {
                limiting_factors.push_back(
                    "The reflected thread group exceeds hardware maxThreadsPerGroup.");
            } else {
                const auto waves_per_group =
                    threads_per_group / hardware->wave_size +
                    (threads_per_group % hardware->wave_size == 0 ? 0U : 1U);
                const auto allocated_lanes_per_group = checked_multiply(
                    waves_per_group, hardware->wave_size, "allocatedWaveLanesPerGroup");
                const auto groups_by_threads =
                    static_cast<std::uint64_t>(hardware->max_threads_per_compute_unit) /
                    allocated_lanes_per_group;
                resident_groups =
                    (std::min)(groups_by_threads,
                               static_cast<std::uint64_t>(hardware->max_groups_per_compute_unit));
                if (info.compute_metadata.has_value() &&
                    info.compute_metadata->group_shared_total_bytes.has_value() &&
                    *info.compute_metadata->group_shared_total_bytes > 0) {
                    const auto groups_by_shared =
                        static_cast<std::uint64_t>(hardware->shared_memory_bytes_per_compute_unit) /
                        *info.compute_metadata->group_shared_total_bytes;
                    resident_groups = (std::min)(resident_groups, groups_by_shared);
                    if (resident_groups == groups_by_shared) {
                        limiting_factors.push_back("Group-shared memory per compute unit.");
                    }
                }
                if (resident_groups == groups_by_threads) {
                    limiting_factors.push_back("Maximum threads per compute unit.");
                }
                if (resident_groups == hardware->max_groups_per_compute_unit) {
                    limiting_factors.push_back("Maximum groups per compute unit.");
                }
                if (groups_by_threads == 0) {
                    limiting_factors.push_back(
                        "One reflected thread group exceeds maxThreadsPerComputeUnit.");
                }
            }
            assumptions.push_back(
                "Register usage and register-file limits are unavailable and are not modeled.");
            if (!info.compute_metadata.has_value() ||
                !info.compute_metadata->group_shared_total_bytes.has_value()) {
                assumptions.push_back(
                    "Not every compiler-formatted groupshared declaration has a reflected size, "
                    "so the supplied shared-memory limit is not applied.");
            }
            if (info.compute_metadata.has_value() && info.compute_metadata->wave_size.known) {
                assumptions.push_back(
                    "Wave allocation uses the supplied hardware waveSize; the shader's "
                    "compiler-formatted WaveSize requirement is reported separately.");
            } else {
                assumptions.push_back(
                    "Wave allocation uses the supplied hardware waveSize; no compiler WaveSize "
                    "requirement was found.");
            }
            const auto resident_threads =
                checked_multiply(resident_groups, threads_per_group, "estimatedResidentThreads");
            const auto waves_per_group = threads_per_group / hardware->wave_size +
                                         (threads_per_group % hardware->wave_size == 0 ? 0U : 1U);
            const auto resident_waves =
                checked_multiply(resident_groups, waves_per_group, "estimatedResidentWaves");
            result["occupancy"] =
                Json{{"hardwareProfile", hardware->name},
                     {"estimatedResidentGroups",
                      checked_json_integer(resident_groups, "estimatedResidentGroups")},
                     {"estimatedResidentThreads", resident_threads},
                     {"estimatedResidentWaves", resident_waves},
                     {"limitingFactors", std::move(limiting_factors)},
                     {"assumptions", std::move(assumptions)}};
        }
    }

    context.cancellation.throw_if_cancellation_requested();
    if (analysis_.content_generation(root_identity, snapshot.version(), context.cancellation) !=
        generation) {
        throw HandlerError{json_rpc::content_modified_code,
                           "hlsl/computeVisualization was superseded"};
    }
    return result;
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

Json Server::inlay_hints(const std::optional<Json>& params,
                         const json_rpc::RequestContext& context) {
    require_running();
    constexpr std::size_t chunk_bytes = std::size_t{256} * 1024U;
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto requested_range = range(object_member(value, "range"));

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Inlay hints require an open document");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();

    std::size_t start_offset{};
    std::size_t end_offset{};
    try {
        start_offset = workspace::utf8_offset_at(snapshot.text(), requested_range.start);
        end_offset = workspace::utf8_offset_at(snapshot.text(), requested_range.end);
    } catch (const workspace::DocumentError& error) {
        invalid_params(error.what());
    }
    if (end_offset < start_offset) {
        invalid_params("Inlay hint range end must not precede its start");
    }
    if (start_offset > std::numeric_limits<std::uint32_t>::max() ||
        end_offset > std::numeric_limits<std::uint32_t>::max()) {
        invalid_params("Inlay hint range is too large");
    }

    InlayHintSettings settings;
    ConfigurationState configuration_state;
    std::uint64_t hint_generation{};
    {
        std::scoped_lock state_lock{state_mutex_};
        settings = inlay_hint_settings_;
        configuration_state =
            ConfigurationState{.editor_settings = editor_settings_,
                               .client_default_language_version = client_default_language_version_,
                               .active_variant = active_variant_,
                               .workspace_folders = workspace_folders_};
        hint_generation = inlay_hint_generation_;
    }

    analyze_and_publish(snapshot.uri());
    const auto require_current_generation = [&] {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version() ||
            inlay_hint_generation_ != hint_generation) {
            throw HandlerError{json_rpc::content_modified_code, "Inlay hints were superseded"};
        }
    };

    Json result = Json::array();
    auto variant_selection = workspace::VariantSelection::undefined;
    if (settings.active_variant && configuration_state.active_variant &&
        !configuration_state.active_variant->empty() && start_offset == 0 && end_offset > 0) {
        static_cast<void>(configuration_for(snapshot, configuration_state, &variant_selection));
    }
    if (settings.active_variant && configuration_state.active_variant &&
        !configuration_state.active_variant->empty() &&
        variant_selection == workspace::VariantSelection::applied && start_offset == 0 &&
        end_offset > 0) {
        result.push_back({{"position", lsp_position(workspace::Position{})},
                          {"label", "variant: " + *configuration_state.active_variant}});
    }

    std::vector<dxc::SourceOffsetRange> ranges;
    std::vector<dxc::InlayCall> calls;
    for (auto chunk_start = start_offset; chunk_start < end_offset;) {
        context.cancellation.throw_if_cancellation_requested();
        require_current_generation();
        const auto chunk_end = (std::min)(end_offset, chunk_start + chunk_bytes);
        ranges.push_back({.start = static_cast<std::uint32_t>(chunk_start),
                          .end = static_cast<std::uint32_t>(chunk_end)});
        chunk_start = chunk_end;
    }
    if (settings.parameters && start_offset < end_offset) {
        for (auto& call : inlay_call_sites(snapshot.text(), start_offset, end_offset, [&] {
                 context.cancellation.throw_if_cancellation_requested();
                 require_current_generation();
             })) {
            const auto position = workspace::lsp_position_at(snapshot.text(), call.callee_offset);
            const auto [line, column] = dxc_position(snapshot.text(), position);
            calls.push_back({.line = line,
                             .column = column,
                             .argument_offsets = std::move(call.argument_offsets)});
        }
    }

    std::vector<dxc::InlayHint> hints;
    try {
        hints = analysis_.inlay_hints(snapshot.document_uri().identity(), snapshot.version(),
                                      snapshot.path(), std::move(ranges), std::move(calls),
                                      {.types = settings.types,
                                       .parameters = settings.parameters,
                                       .matrix_orientation = settings.matrix_orientation,
                                       .registers = settings.registers,
                                       .packed_offsets = settings.packed_offsets,
                                       .array_strides = settings.array_strides},
                                      context.cancellation);
    } catch (const HandlerError&) {
        require_current_generation();
        throw;
    }
    require_current_generation();
    for (const auto& hint : hints) {
        Json item{
            {"position", lsp_position(workspace::lsp_position_at(snapshot.text(), hint.offset))},
            {"label", hint.label}};
        if (hint.category == dxc::InlayHintCategory::type) {
            item["kind"] = 1;
        } else if (hint.category == dxc::InlayHintCategory::parameter) {
            item["kind"] = 2;
            item["paddingRight"] = true;
        }
        result.push_back(std::move(item));
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
    bool truncated{};
    const auto symbols = analysis_.document_symbols(
        snapshot.document_uri().identity(), snapshot.version(), context.cancellation, truncated);
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(snapshot.uri()) ||
            documents_.snapshot(snapshot.uri()).version() != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Document symbols were superseded"};
        }
    }
    append_document_symbols(result, symbols, snapshot);
    if (truncated) {
        sender_(json_rpc::Notification{
            .method = "window/logMessage",
            .params =
                Json{{"type", 2},
                     {"message", "Document symbols were truncated at 1024 declarations to keep the "
                                 "request responsive."}}});
    }
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
    bool refresh_inlay_hints{};
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
        invalidate_inlay_hints(false);
        refresh_inlay_hints = true;
        analyze_affected(uri);
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_change(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
        invalidate_inlay_hints(false);
        refresh_inlay_hints = true;
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_save(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
        invalidate_inlay_hints(false);
        refresh_inlay_hints = true;
        analyze_affected(uri);
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_close(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
            diagnostics_by_identity_.erase(snapshot.document_uri().identity());
        }
        invalidate_inlay_hints(false);
        refresh_inlay_hints = true;
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
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_change_configuration(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
    try {
        require_running();
        const auto& settings = object_member(object_params(params), "settings");
        const auto candidate = configuration_overrides(settings);
        const auto candidate_inlay_hints = inlay_hint_settings(settings);
        bool inlay_inputs_changed{};
        {
            std::scoped_lock state_lock{state_mutex_};
            for (const auto& document : documents_.open_snapshots()) {
                static_cast<void>(configuration_for(document, candidate));
            }
            inlay_inputs_changed =
                editor_settings_ != candidate || inlay_hint_settings_ != candidate_inlay_hints;
            editor_settings_ = candidate;
            inlay_hint_settings_ = candidate_inlay_hints;
        }
        if (inlay_inputs_changed) {
            invalidate_inlay_hints(false);
            refresh_inlay_hints = true;
        }
        reanalyze_all();
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_change_client_defaults(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
    try {
        require_running();
        const auto defaults = configuration_overrides(object_params(params));
        if (!defaults.language_version) {
            invalid_params("hlsl.languageVersion must be provided");
        }
        bool changed{};
        {
            std::scoped_lock state_lock{state_mutex_};
            changed = client_default_language_version_ != *defaults.language_version;
            client_default_language_version_ = *defaults.language_version;
        }
        if (changed) {
            invalidate_inlay_hints(false);
            refresh_inlay_hints = true;
        }
        reanalyze_all();
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_change_active_variant(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
            invalidate_inlay_hints(false);
            refresh_inlay_hints = true;
            // A variant change reanalyzes open documents; only a differing runtime
            // selection escalates to the controlled restart shared with issue #14.
            reanalyze_all();
            reevaluate_runtime_selection();
        }
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::invalidate_inlay_hints(bool refresh) {
    {
        std::scoped_lock state_lock{state_mutex_};
        ++inlay_hint_generation_;
    }
    if (refresh) {
        request_inlay_hint_refresh();
    }
}

void Server::request_inlay_hint_refresh() {
    {
        std::scoped_lock state_lock{state_mutex_};
        if (!client_inlay_hint_refresh_ || !request_sender_ || state_ != State::running) {
            return;
        }
    }
    request_sender_(
        json_rpc::Request{.id = next_outbound_request_id_.fetch_add(1, std::memory_order_relaxed),
                          .method = "workspace/inlayHint/refresh",
                          .params = std::nullopt});
}

void Server::finalize_inlay_hint_refresh() noexcept {
    try {
        request_inlay_hint_refresh();
    } catch (const std::exception& error) {
        try {
            log(error.what());
        } catch (...) {
            static_cast<void>(
                std::fputs("HLSL-LSP: failed to log inlay-hint refresh error\n", stderr));
        }
    } catch (...) {
        static_cast<void>(std::fputs("HLSL-LSP: inlay-hint refresh failed\n", stderr));
    }
}

void Server::did_change_workspace_folders(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
        bool changed{};
        {
            std::scoped_lock state_lock{state_mutex_};
            changed = workspace_folders_ != workspace_folders;
            workspace_folders_ = std::move(workspace_folders);
        }
        if (changed) {
            invalidate_inlay_hints(false);
            refresh_inlay_hints = true;
        }
        reanalyze_all();
        reevaluate_runtime_selection();
        reevaluate_variant_selection();
    } catch (const std::exception& error) {
        log(error.what());
    }
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
    }
}

void Server::did_change_watched_files(const std::optional<Json>& params) {
    bool refresh_inlay_hints{};
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
        const bool hints_changed = !changes.empty();
        if (hints_changed) {
            invalidate_inlay_hints(false);
            refresh_inlay_hints = true;
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
    if (refresh_inlay_hints) {
        finalize_inlay_hint_refresh();
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
Server::base_configuration_for(const workspace::SourceSnapshot& snapshot) const {
    return base_configuration_for(snapshot, client_default_language_version_);
}

workspace::WorkspaceConfiguration
Server::base_configuration_for(const workspace::SourceSnapshot& snapshot,
                               const std::optional<std::string>& client_default_language_version) {
    workspace::WorkspaceConfiguration configuration;
    const auto shader_directory = std::filesystem::path{snapshot.path()}.parent_path();
    std::error_code error;
    if (std::filesystem::is_directory(shader_directory, error)) {
        configuration = workspace::load_workspace_configuration_for_file(snapshot.path());
    } else if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error{"Unable to inspect shader directory",
                                                shader_directory, error};
    }
    if (!configuration.language_version && client_default_language_version) {
        configuration.language_version = client_default_language_version;
        configuration.setting_origins["languageVersion"] = "client defaults";
    }
    return configuration;
}

workspace::WorkspaceConfiguration
Server::configuration_for(const workspace::SourceSnapshot& snapshot,
                          const workspace::ConfigurationOverrides& overrides) const {
    return configuration_for(snapshot, ConfigurationState{.editor_settings = overrides,
                                                          .client_default_language_version =
                                                              client_default_language_version_,
                                                          .active_variant = active_variant_,
                                                          .workspace_folders = workspace_folders_});
}

workspace::WorkspaceConfiguration
Server::configuration_for(const workspace::SourceSnapshot& snapshot,
                          const ConfigurationState& state,
                          workspace::VariantSelection* active_variant_selection) {
    auto configuration = base_configuration_for(snapshot, state.client_default_language_version);
    // The active variant is applied on top of the file-derived configuration but
    // below editor overrides, so a selected variant beats client defaults while an
    // explicit editor setting still wins. Unknown or inapplicable selections are
    // reported by reevaluate_variant_selection rather than throwing here.
    if (state.active_variant) {
        const auto selection = workspace::apply_variant(configuration, *state.active_variant);
        if (active_variant_selection != nullptr) {
            *active_variant_selection = selection;
        }
    }
    return workspace::apply_configuration_overrides(
        std::move(configuration), state.editor_settings,
        configuration_base_directory(snapshot.path(), state.workspace_folders));
}

workspace::WorkspaceConfiguration
Server::variant_configuration_for(const workspace::SourceSnapshot& snapshot,
                                  std::string_view variant_name,
                                  const workspace::ConfigurationOverrides& overrides) const {
    return variant_configuration_for(
        snapshot, variant_name,
        ConfigurationState{.editor_settings = overrides,
                           .client_default_language_version = client_default_language_version_,
                           .active_variant = active_variant_,
                           .workspace_folders = workspace_folders_});
}

workspace::WorkspaceConfiguration
Server::variant_configuration_for(const workspace::SourceSnapshot& snapshot,
                                  std::string_view variant_name, const ConfigurationState& state) {
    auto configuration = base_configuration_for(snapshot, state.client_default_language_version);
    static_cast<void>(workspace::apply_variant(configuration, variant_name));
    return workspace::apply_configuration_overrides(
        std::move(configuration), state.editor_settings,
        configuration_base_directory(snapshot.path(), state.workspace_folders));
}

std::filesystem::path Server::configuration_base_directory(std::string_view shader_path) const {
    return configuration_base_directory(shader_path, workspace_folders_);
}

std::filesystem::path Server::configuration_base_directory(
    std::string_view shader_path,
    const std::unordered_map<std::string, std::filesystem::path>& workspace_folders) {
    auto directory = std::filesystem::absolute(std::filesystem::path{shader_path}.parent_path())
                         .lexically_normal();
    auto candidate = directory;
    while (!candidate.empty()) {
        try {
            const auto identity = workspace::DocumentUri::from_path(candidate.string()).identity();
            if (const auto folder = workspace_folders.find(identity);
                folder != workspace_folders.end()) {
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

Server::ConfigurationState Server::snapshot_configuration_state() const {
    std::scoped_lock state_lock{state_mutex_};
    return ConfigurationState{.editor_settings = editor_settings_,
                              .client_default_language_version = client_default_language_version_,
                              .active_variant = active_variant_,
                              .workspace_folders = workspace_folders_};
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

std::optional<Json> Server::fix_it_code_action(const workspace::SourceSnapshot& snapshot,
                                               const dxc::Diagnostic& diagnostic,
                                               const Json& diagnostic_item) const {
    struct PendingEdit {
        std::size_t start;
        std::size_t end;
        std::string replacement;
    };
    std::vector<PendingEdit> edits;
    edits.reserve(diagnostic.fix_its.size());
    for (const auto& fix_it : diagnostic.fix_its) {
        // A fix-it with no attributed file, or attributed to a different file
        // than the document this action is for, is never safe to apply here:
        // reject the whole (possibly multi-edit) fix rather than guessing.
        if (fix_it.range.start.path.empty() || fix_it.range.end.path.empty() ||
            !same_document_path(fix_it.range.start.path, snapshot.path()) ||
            !same_document_path(fix_it.range.end.path, snapshot.path())) {
            return std::nullopt;
        }
        const auto start = static_cast<std::size_t>(fix_it.range.start.offset);
        const auto end = static_cast<std::size_t>(fix_it.range.end.offset);
        if (start > end || end > snapshot.text().size()) {
            return std::nullopt;
        }
        try {
            // utf16_length fully decodes its argument, so this also validates
            // that DXC's replacement text is well-formed UTF-8 before it is ever
            // written into a document.
            static_cast<void>(workspace::utf16_length(fix_it.replacement_text));
        } catch (const workspace::DocumentError&) {
            return std::nullopt;
        }
        edits.push_back(
            PendingEdit{.start = start, .end = end, .replacement = fix_it.replacement_text});
    }
    if (edits.empty()) {
        return std::nullopt;
    }

    // stable_sort preserves DXC's own reported relative order for any edits
    // that end up adjacent after sorting by start offset, keeping the result
    // deterministic across runs/builds rather than depending on an
    // unspecified tie-break order.
    std::ranges::stable_sort(edits, {}, &PendingEdit::start);
    for (std::size_t index = 1; index < edits.size(); ++index) {
        // A strict overlap, or two edits that start at the exact same offset,
        // can never be applied together safely: DXC's fix-it list gives no
        // guaranteed semantic for which of two same-start edits (e.g. two
        // zero-width insertions at one point) should be applied first, so
        // concatenation order cannot be inferred rather than guessed. Reject
        // the entire multi-edit fix rather than silently picking an order.
        if (edits[index].start < edits[index - 1].end ||
            edits[index].start == edits[index - 1].start) {
            return std::nullopt;
        }
    }

    Json lsp_edits = Json::array();
    for (const auto& edit : edits) {
        try {
            const auto start = workspace::lsp_position_at(snapshot.text(), edit.start);
            const auto end = workspace::lsp_position_at(snapshot.text(), edit.end);
            lsp_edits.push_back({{"range", lsp_range({.start = start, .end = end})},
                                 {"newText", edit.replacement}});
        } catch (const workspace::DocumentError&) {
            return std::nullopt;
        }
    }

    Json document_changes = Json::array(
        {Json{{"textDocument", {{"uri", snapshot.uri()}, {"version", snapshot.version()}}},
              {"edits", std::move(lsp_edits)}}});
    std::string title = "Apply DXC fix-it: " + diagnostic.message;
    if (edits.size() > 1) {
        title += " (" + std::to_string(edits.size()) + " edits)";
    }
    return Json{{"title", std::move(title)},
                {"kind", "quickfix"},
                {"diagnostics", Json::array({diagnostic_item})},
                {"isPreferred", true},
                {"edit", {{"documentChanges", std::move(document_changes)}}}};
}

std::optional<Json> Server::include_recovery_action(
    const workspace::SourceSnapshot& snapshot,
    const std::vector<workspace::SourceSnapshot>& open_documents, const dxc::Diagnostic& diagnostic,
    const Json& diagnostic_item, const workspace::WorkspaceConfiguration& active_configuration,
    const std::optional<std::string>& active_variant,
    const std::function<const workspace::WorkspaceConfiguration&(std::string_view)>&
        variant_configuration_for_name) {
    // Fix-it diagnostics are handled by fix_it_code_action; this recovery path
    // only ever applies to diagnostics DXC gave no fix-it for.
    if (!diagnostic.fix_its.empty() || diagnostic.location.line == 0 ||
        diagnostic.location.column == 0) {
        return std::nullopt;
    }
    const auto displayed_range = diagnostic_range(snapshot, diagnostic);
    if (displayed_range == workspace::Range{}) {
        return std::nullopt;
    }
    std::size_t offset{};
    try {
        offset = workspace::utf8_offset_at(snapshot.text(), displayed_range.start);
    } catch (const workspace::DocumentError&) {
        return std::nullopt;
    }

    // resolve_include_at only returns a location when `offset` falls inside an
    // #include directive's path span; this is how the diagnostic is
    // structurally (not textually) recognized as an unresolved-include problem.
    // A non-null result means either the diagnostic is not on an include path at
    // all, or the include already resolves, so there is nothing to recover.
    if (workspace::resolve_include_at(snapshot, open_documents, active_configuration, offset)
            .has_value()) {
        return std::nullopt;
    }

    for (const auto& variant : active_configuration.variants) {
        if (!variant.applicable || (active_variant && *active_variant == variant.name)) {
            continue;
        }
        const auto& candidate = variant_configuration_for_name(variant.name);
        if (!workspace::resolve_include_at(snapshot, open_documents, candidate, offset)
                 .has_value()) {
            continue;
        }
        // Proven by resolve_include_at using this variant's own declared
        // include directories: never a fabricated path.
        return Json{
            {"title", "Select shader variant '" + variant.name + "' (resolves this #include)"},
            {"kind", "quickfix"},
            {"diagnostics", Json::array({diagnostic_item})},
            {"command",
             {{"title", "Select HLSL shader variant"},
              {"command", "hlsl-lsp.selectVariant"},
              {"arguments", Json::array({Json{{"variant", variant.name}}})}}}};
    }
    return std::nullopt;
}

Json Server::code_action(const std::optional<Json>& params,
                         const json_rpc::RequestContext& context) {
    require_running();
    const auto& value = object_params(params);
    const auto uri = string_member(object_member(value, "textDocument"), "uri");
    const auto requested_range = range(object_member(value, "range"));
    const auto& request_context = object_member(value, "context");
    if (const auto raw_diagnostics = request_context.find("diagnostics");
        raw_diagnostics != request_context.end() && !raw_diagnostics->is_array()) {
        invalid_params("context.diagnostics must be an array");
    }
    // context.diagnostics is intentionally never read beyond this shape check:
    // actions are always derived from the server's own current diagnostic
    // state, never from a client-echoed payload.
    bool accept_quickfix = true;
    if (const auto only = request_context.find("only");
        only != request_context.end() && !only->is_null()) {
        if (!only->is_array()) {
            invalid_params("context.only must be an array of CodeActionKind strings");
        }
        accept_quickfix = false;
        for (const auto& kind : *only) {
            if (!kind.is_string()) {
                invalid_params("context.only entries must be strings");
            }
            // Per the LSP CodeActionKind hierarchy, an `only` entry matches when
            // it is a prefix of (or equal to) the produced kind, e.g. requesting
            // "quickfix" matches a produced "quickfix" or "quickfix.foo" action.
            // Every action this server produces has the plain "quickfix" kind
            // (no sub-kind), so only the exact string, or an empty/whitespace
            // prefix, can match; a more specific requested sub-kind such as
            // "quickfix.foo" must not match our plainly-kinded action.
            if (std::string_view{"quickfix"}.starts_with(kind.get_ref<const std::string&>())) {
                accept_quickfix = true;
            }
        }
    }
    if (!accept_quickfix) {
        return Json::array();
    }

    workspace::SourceSnapshot snapshot = [&] {
        std::scoped_lock state_lock{state_mutex_};
        try {
            const auto& state = documents_.document(uri);
            if (!state.open) {
                invalid_params("Code action document is not open");
            }
            return documents_.snapshot(uri);
        } catch (const workspace::DocumentError& error) {
            invalid_params(error.what());
        }
    }();
    context.cancellation.throw_if_cancellation_requested();

    std::vector<dxc::Diagnostic> diagnostics;
    std::uint64_t generation{};
    std::vector<workspace::SourceSnapshot> open_documents;
    {
        std::scoped_lock state_lock{state_mutex_};
        const auto identity = snapshot.document_uri().identity();
        const auto record = diagnostics_by_identity_.find(identity);
        const auto current_generation_entry = analysis_generations_.find(identity);
        const auto current_generation = current_generation_entry == analysis_generations_.end()
                                            ? std::uint64_t{}
                                            : current_generation_entry->second;
        if (record == diagnostics_by_identity_.end() ||
            record->second.version != snapshot.version() ||
            record->second.generation != current_generation) {
            // Diagnostics were never computed for exactly this snapshot version,
            // the document has changed since, or a configuration/variant
            // reanalysis has already started (bumping analysis_generations_)
            // without a matching completed analysis cached yet: omit rather
            // than guess or offer actions derived from stale diagnostics.
            return Json::array();
        }
        diagnostics = record->second.diagnostics;
        generation = record->second.generation;
        open_documents = documents_.open_snapshots();
    }

    // Diagnostics overlapping the requested range, split into those DXC gave a
    // fix-it for (handled by fix_it_code_action, which needs no configuration)
    // and those that are structurally on an unresolved #include directive path
    // (candidates for include_recovery_action). Recognizing the latter uses
    // only the snapshot's own text (is_include_directive_at is a cheap textual
    // check, no configuration or filesystem access), so this pass decides
    // whether any configuration needs to be loaded at all for this request
    // before paying for it, rather than reloading it once per diagnostic.
    struct OverlappingDiagnostic {
        std::size_t index;
        Json item;
        bool is_include_candidate{};
    };
    std::vector<OverlappingDiagnostic> overlapping;
    bool needs_configuration = false;
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        context.cancellation.throw_if_cancellation_requested();
        const auto& diagnostic = diagnostics[index];
        if (diagnostic.location.line == 0 || diagnostic.location.column == 0) {
            continue;
        }
        const auto displayed_range = diagnostic_range(snapshot, diagnostic);
        if (displayed_range == workspace::Range{} ||
            !ranges_overlap(displayed_range, requested_range)) {
            continue;
        }
        OverlappingDiagnostic entry{
            .index = index, .item = diagnostic_json(snapshot, diagnostic, generation, index)};
        if (diagnostic.fix_its.empty()) {
            try {
                const auto offset =
                    workspace::utf8_offset_at(snapshot.text(), displayed_range.start);
                entry.is_include_candidate =
                    workspace::is_include_directive_at(snapshot.text(), offset);
            } catch (const workspace::DocumentError&) {
                entry.is_include_candidate = false;
            }
            needs_configuration = needs_configuration || entry.is_include_candidate;
        }
        overlapping.push_back(std::move(entry));
    }

    // Computed at most once per request (not once per diagnostic), and only
    // when at least one overlapping diagnostic actually needs it. State is
    // snapshotted under a single brief lock; the (possibly disk-I/O-bound)
    // configuration computation itself then runs unlocked.
    std::optional<ConfigurationState> configuration_state;
    workspace::WorkspaceConfiguration active_configuration;
    std::optional<std::string> active_variant;
    std::unordered_map<std::string, workspace::WorkspaceConfiguration> variant_configurations;
    if (needs_configuration) {
        configuration_state = snapshot_configuration_state();
        active_configuration = configuration_for(snapshot, *configuration_state);
        active_variant = configuration_state->active_variant;
    }
    const auto variant_config =
        [&](std::string_view variant_name) -> const workspace::WorkspaceConfiguration& {
        auto [found, inserted] = variant_configurations.try_emplace(std::string{variant_name});
        if (inserted) {
            found->second = variant_configuration_for(snapshot, variant_name, *configuration_state);
        }
        return found->second;
    };

    Json actions = Json::array();
    for (const auto& entry : overlapping) {
        context.cancellation.throw_if_cancellation_requested();
        const auto& diagnostic = diagnostics[entry.index];
        if (!diagnostic.fix_its.empty()) {
            if (auto action = fix_it_code_action(snapshot, diagnostic, entry.item)) {
                actions.push_back(std::move(*action));
            }
        } else if (entry.is_include_candidate) {
            if (auto action =
                    include_recovery_action(snapshot, open_documents, diagnostic, entry.item,
                                            active_configuration, active_variant, variant_config)) {
                actions.push_back(std::move(*action));
            }
        }
    }

    {
        std::scoped_lock state_lock{state_mutex_};
        if (!documents_.contains(uri) || !documents_.document(uri).open ||
            documents_.document(uri).version != snapshot.version()) {
            throw HandlerError{json_rpc::content_modified_code, "Code action was superseded"};
        }
    }
    return actions;
}

Json Server::execute_command(const std::optional<Json>& params) {
    require_running();
    const auto& value = object_params(params);
    const auto command = string_member(value, "command");
    if (command != "hlsl-lsp.selectVariant") {
        throw HandlerError{json_rpc::invalid_params_code, "Unknown command: " + command};
    }
    Json argument = Json::object();
    if (const auto arguments = value.find("arguments");
        arguments != value.end() && !arguments->is_null()) {
        if (!arguments->is_array() || arguments->empty() || !(*arguments)[0].is_object()) {
            invalid_params("hlsl-lsp.selectVariant requires a single {variant} argument object");
        }
        argument = (*arguments)[0];
    }
    Json variant_params = Json::object();
    if (const auto variant = argument.find("variant"); variant != argument.end()) {
        if (!variant->is_string() && !variant->is_null()) {
            invalid_params("variant must be a string or null");
        }
        variant_params["variant"] = *variant;
    } else {
        variant_params["variant"] = nullptr;
    }
    // Reuses the same validated state mutation and reanalysis path a client
    // hlsl/didChangeActiveVariant notification takes, so command-driven variant
    // selection stays consistent with the notification-driven path.
    did_change_active_variant(variant_params);

    // Unlike a client-originated hlsl/didChangeActiveVariant notification (the
    // client already owns that state, e.g. a VS Code setting or a Visual
    // Studio configuration cache, and applies it before telling the server),
    // this command changes *only* server state: nothing else durably records
    // the new selection. Without this notification, the client's own
    // persisted/displayed active variant would silently diverge from the
    // server's the next time anything resynchronizes settings (client
    // restart, an unrelated configuration change, etc.), which would then
    // overwrite the server's variant right back. Report the server's
    // resulting authoritative value (not merely the requested one) so a
    // client always converges to the true current state even if multiple
    // selectVariant commands race.
    std::optional<std::string> resulting_variant;
    {
        std::scoped_lock state_lock{state_mutex_};
        resulting_variant = active_variant_;
    }
    sender_(json_rpc::Notification{
        .method = "hlsl/activeVariantChanged",
        .params = Json{{"variant", resulting_variant ? Json(*resulting_variant) : Json(nullptr)}}});
    return nullptr;
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
        std::vector<dxc::Diagnostic> filtered;
        filtered.reserve(diagnostics.size());
        for (const auto& diagnostic : diagnostics) {
            if (!diagnostic.location.path.empty() &&
                !same_document_path(diagnostic.location.path, latest.path())) {
                continue;
            }
            filtered.push_back(diagnostic);
        }

        const auto identity = snapshot.document_uri().identity();
        const auto existing = diagnostics_by_identity_.find(identity);
        // A cache hit (identical document version and diagnostics content,
        // where only the analysis generation advanced) still must refresh the
        // cached record so textDocument/codeAction sees the current
        // generation and does not treat itself as stale, but must not
        // republish a byte-identical textDocument/publishDiagnostics payload
        // to the client: interactive typing can retrigger reanalysis of
        // unchanged content many times (e.g. every keystroke inside a
        // comment, or a configuration/variant reanalysis that reproduces the
        // same diagnostics), and resending the same notification on every
        // such cache hit is wasted client/server work with no observable
        // effect for the user. A first publish (no existing record), a
        // different document version, or genuinely different diagnostics
        // always republishes.
        const bool unchanged = existing != diagnostics_by_identity_.end() &&
                               existing->second.version == latest.version() &&
                               existing->second.diagnostics == filtered;
        diagnostics_by_identity_.insert_or_assign(identity,
                                                  DiagnosticsRecord{.version = latest.version(),
                                                                    .generation = generation,
                                                                    .diagnostics = filtered});
        if (!unchanged) {
            publish_diagnostics(latest, filtered, generation);
        }
    }
}

void Server::publish_diagnostics(const workspace::SourceSnapshot& snapshot,
                                 const std::vector<dxc::Diagnostic>& diagnostics,
                                 std::uint64_t generation) {
    Json items = Json::array();
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        items.push_back(diagnostic_json(snapshot, diagnostics[index], generation, index));
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
                      options,
                      [&write_payload](const json_rpc::Request& request) {
                          write_payload(json_rpc::serialize(json_rpc::Message{request}));
                      }};
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
