#include <hlsl_intellisense/workspace/document_uri.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) {
        return 0;
    }
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    for (const auto style : {hlsl_intellisense::workspace::PathStyle::posix,
                             hlsl_intellisense::workspace::PathStyle::windows}) {
        try {
            static_cast<void>(hlsl_intellisense::workspace::DocumentUri::from_uri(input, style));
        } catch (const hlsl_intellisense::workspace::DocumentError&) {
        }
        try {
            static_cast<void>(hlsl_intellisense::workspace::DocumentUri::from_path(input, style));
        } catch (const hlsl_intellisense::workspace::DocumentError&) {
        }
    }
    return 0;
}
