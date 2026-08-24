#include <hlsl_intellisense/workspace/document_store.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0 || size > 4096) {
        return 0;
    }
    constexpr std::string_view uri = "file:///fuzz/input.hlsl";
    hlsl_intellisense::workspace::DocumentStore store{
        hlsl_intellisense::workspace::PathStyle::posix};
    try {
        std::string initial;
        initial.reserve(size - 1);
        for (std::size_t index = 1; index < size; ++index) {
            initial.push_back(static_cast<char>('a' + data[index] % 26));
        }
        store.did_open(uri, "hlsl", 1, std::move(initial));
        std::int64_t version = 2;
        for (std::size_t offset = 1; offset + 4 < size && version < 128; offset += 5, ++version) {
            const auto current_size = store.document(uri).text.size();
            const auto first = current_size == 0 ? 0 : data[offset] % (current_size + 1);
            const auto remaining = current_size - first;
            const auto last = first + (remaining == 0 ? 0 : data[offset + 1] % (remaining + 1));
            const std::string replacement(1, static_cast<char>('a' + data[offset + 2] % 26));
            const std::array change{hlsl_intellisense::workspace::ContentChange{
                .range =
                    hlsl_intellisense::workspace::Range{
                        .start = {.line = 0, .character = static_cast<std::uint32_t>(first)},
                        .end = {.line = 0, .character = static_cast<std::uint32_t>(last)}},
                .range_length = last - first,
                .text = replacement}};
            store.did_change(uri, version, change);
        }
    } catch (const hlsl_intellisense::workspace::DocumentError&) {
    }
    return 0;
}
