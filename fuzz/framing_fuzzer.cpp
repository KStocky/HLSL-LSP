#include <hlsl_intellisense/json_rpc/framing.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::istringstream input{std::string{reinterpret_cast<const char*>(data), size},
                             std::ios::in | std::ios::binary};
    hlsl_intellisense::json_rpc::FrameReader reader{input, 4096};
    try {
        while (reader.read().has_value()) {
        }
    } catch (const hlsl_intellisense::json_rpc::FrameError&) {
    }
    return 0;
}
