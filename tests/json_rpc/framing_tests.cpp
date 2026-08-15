#include <hlsl_intellisense/json_rpc/framing.h>

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

namespace json_rpc = hlsl_intellisense::json_rpc;

namespace {

[[nodiscard]] std::string frame(std::string_view payload,
                                std::string_view header_name = "Content-Length") {
    return std::string{header_name} + ": " + std::to_string(payload.size()) + "\r\n\r\n" +
           std::string{payload};
}

void require_frame_error(std::string input, json_rpc::FrameErrorCode expected,
                         std::size_t maximum = json_rpc::default_max_payload_size) {
    std::istringstream stream{std::move(input)};
    json_rpc::FrameReader reader{stream, maximum};
    try {
        static_cast<void>(reader.read());
        FAIL("Expected FrameError");
    } catch (const json_rpc::FrameError& error) {
        CHECK(error.code() == expected);
    }
}

} // namespace

TEST_CASE("FrameReader reads valid request and notification frames", "[json-rpc][framing]") {
    const std::string request = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
    const std::string notification = R"({"jsonrpc":"2.0","method":"initialized","params":{}})";
    std::istringstream stream{frame(request, "content-length") + frame(notification)};
    json_rpc::FrameReader reader{stream};

    REQUIRE(reader.read() == request);
    REQUIRE(reader.read() == notification);
    CHECK(!reader.read().has_value());
}

TEST_CASE("FrameWriter emits only a complete protocol frame", "[json-rpc][framing]") {
    std::ostringstream stream;
    json_rpc::FrameWriter writer{stream};

    writer.write("{}");

    CHECK(stream.str() == "Content-Length: 2\r\n\r\n{}");
}

TEST_CASE("FrameReader rejects malformed or missing Content-Length", "[json-rpc][framing]") {
    SECTION("missing") {
        require_frame_error("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}",
                            json_rpc::FrameErrorCode::missing_content_length);
    }
    SECTION("not numeric") {
        require_frame_error("Content-Length: 1x\r\n\r\n{}",
                            json_rpc::FrameErrorCode::invalid_content_length);
    }
    SECTION("negative") {
        require_frame_error("Content-Length: -1\r\n\r\n",
                            json_rpc::FrameErrorCode::invalid_content_length);
    }
    SECTION("duplicate") {
        require_frame_error("Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
                            json_rpc::FrameErrorCode::invalid_content_length);
    }
    SECTION("LF termination") {
        require_frame_error("Content-Length: 2\n\n{}", json_rpc::FrameErrorCode::malformed_header);
    }
    SECTION("header without colon") {
        require_frame_error("Content-Length 2\r\n\r\n{}",
                            json_rpc::FrameErrorCode::malformed_header);
    }
}

TEST_CASE("FrameReader enforces its maximum and exact payload length", "[json-rpc][framing]") {
    SECTION("oversized") {
        require_frame_error("Content-Length: 3\r\n\r\n{}",
                            json_rpc::FrameErrorCode::payload_too_large, 2);
    }
    SECTION("numeric overflow") {
        require_frame_error("Content-Length: 999999999999999999999999999999\r\n\r\n",
                            json_rpc::FrameErrorCode::payload_too_large);
    }
    SECTION("truncated") {
        require_frame_error("Content-Length: 3\r\n\r\n{}",
                            json_rpc::FrameErrorCode::truncated_payload);
    }
}
