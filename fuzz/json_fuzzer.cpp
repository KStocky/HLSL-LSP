#include <hlsl_intellisense/json_rpc/message.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    static_cast<void>(hlsl_intellisense::json_rpc::parse_message(input));
    return 0;
}
