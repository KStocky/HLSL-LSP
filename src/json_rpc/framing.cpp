#include <hlsl_intellisense/json_rpc/framing.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace hlsl_intellisense::json_rpc {
namespace {

constexpr std::size_t max_header_line_size = std::size_t{8} * 1024U;

enum class LineState : std::uint8_t {
    line,
    end_of_input,
};

[[nodiscard]] LineState read_crlf_line(std::istream& input, std::string& line) {
    line.clear();
    while (true) {
        const auto value = input.get();
        if (value == std::char_traits<char>::eof()) {
            if (input.bad()) {
                throw FrameError{FrameErrorCode::input_error, "Failed while reading headers"};
            }
            if (line.empty()) {
                return LineState::end_of_input;
            }
            throw FrameError{FrameErrorCode::malformed_header,
                             "Header block ended before a CRLF terminator"};
        }

        const auto character = static_cast<char>(value);
        if (character == '\n') {
            throw FrameError{FrameErrorCode::malformed_header,
                             "Header lines must be terminated by CRLF"};
        }
        if (character == '\r') {
            const auto next = input.get();
            if (next != '\n') {
                throw FrameError{FrameErrorCode::malformed_header,
                                 "Header lines must be terminated by CRLF"};
            }
            return LineState::line;
        }

        line.push_back(character);
        if (line.size() > max_header_line_size) {
            throw FrameError{FrameErrorCode::malformed_header, "Header line is too long"};
        }
    }
}

[[nodiscard]] std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace{" \t"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] bool equals_case_insensitive(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](char first, char second) {
               const auto first_value = static_cast<unsigned char>(first);
               const auto second_value = static_cast<unsigned char>(second);
               return std::tolower(first_value) == std::tolower(second_value);
           });
}

[[nodiscard]] std::size_t parse_content_length(std::string_view value,
                                               std::size_t max_payload_size) {
    if (value.empty() || !std::ranges::all_of(value, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        throw FrameError{FrameErrorCode::invalid_content_length,
                         "Content-Length must be an unsigned decimal integer"};
    }

    std::size_t length{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), length);
    if (result.ec == std::errc::result_out_of_range) {
        throw FrameError{FrameErrorCode::payload_too_large, "Content-Length is too large"};
    }
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw FrameError{FrameErrorCode::invalid_content_length, "Invalid Content-Length"};
    }
    if (length > max_payload_size) {
        throw FrameError{FrameErrorCode::payload_too_large,
                         "Content-Length exceeds the configured maximum"};
    }
    return length;
}

} // namespace

FrameError::FrameError(FrameErrorCode code, std::string_view message)
    : std::runtime_error{std::string{message}}, code_{code} {}

FrameErrorCode FrameError::code() const noexcept { return code_; }

FrameReader::FrameReader(std::istream& input, std::size_t max_payload_size)
    : input_{input}, max_payload_size_{max_payload_size} {}

std::optional<std::string> FrameReader::read() {
    std::optional<std::size_t> content_length;
    std::string line;
    bool read_any_header = false;

    while (true) {
        const auto state = read_crlf_line(input_, line);
        if (state == LineState::end_of_input) {
            if (!read_any_header) {
                return std::nullopt;
            }
            throw FrameError{FrameErrorCode::malformed_header,
                             "Header block ended before its terminating CRLF"};
        }
        if (line.empty()) {
            break;
        }
        read_any_header = true;

        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            throw FrameError{FrameErrorCode::malformed_header, "Header is missing ':'"};
        }
        const auto name = trim(std::string_view{line}.substr(0, separator));
        const auto value = trim(std::string_view{line}.substr(separator + 1U));
        if (name.empty()) {
            throw FrameError{FrameErrorCode::malformed_header, "Header name is empty"};
        }
        if (equals_case_insensitive(name, "Content-Length")) {
            if (content_length.has_value()) {
                throw FrameError{FrameErrorCode::invalid_content_length,
                                 "Content-Length must not be repeated"};
            }
            content_length = parse_content_length(value, max_payload_size_);
        }
    }

    if (!content_length.has_value()) {
        throw FrameError{FrameErrorCode::missing_content_length,
                         "Content-Length header is required"};
    }

    std::string payload(*content_length, '\0');
    if (!payload.empty()) {
        constexpr auto max_stream_size =
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());
        if (payload.size() > max_stream_size) {
            throw FrameError{FrameErrorCode::payload_too_large,
                             "Payload cannot be represented by this stream implementation"};
        }
        input_.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (input_.gcount() != static_cast<std::streamsize>(payload.size())) {
            if (input_.bad()) {
                throw FrameError{FrameErrorCode::input_error, "Failed while reading payload"};
            }
            throw FrameError{FrameErrorCode::truncated_payload,
                             "Input ended before the complete payload was read"};
        }
    }
    return payload;
}

FrameWriter::FrameWriter(std::ostream& output) : output_{output} {}

void FrameWriter::write(std::string_view payload) {
    constexpr auto max_stream_size =
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());
    if (payload.size() > max_stream_size) {
        throw FrameError{FrameErrorCode::payload_too_large,
                         "Payload cannot be represented by this stream implementation"};
    }
    output_ << "Content-Length: " << payload.size() << "\r\n\r\n";
    output_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output_.flush();
    if (!output_) {
        throw FrameError{FrameErrorCode::output_error, "Failed to write protocol frame"};
    }
}

} // namespace hlsl_intellisense::json_rpc
