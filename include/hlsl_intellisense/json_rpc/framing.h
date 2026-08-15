#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hlsl_intellisense::json_rpc {

inline constexpr std::size_t default_max_payload_size = std::size_t{16} * 1024U * 1024U;

enum class FrameErrorCode {
    malformed_header,
    missing_content_length,
    invalid_content_length,
    payload_too_large,
    truncated_payload,
    input_error,
    output_error,
};

class FrameError final : public std::runtime_error {
  public:
    FrameError(FrameErrorCode code, std::string_view message);

    [[nodiscard]] FrameErrorCode code() const noexcept;

  private:
    FrameErrorCode code_;
};

class FrameReader final {
  public:
    explicit FrameReader(std::istream& input,
                         std::size_t max_payload_size = default_max_payload_size);

    [[nodiscard]] std::optional<std::string> read();

  private:
    std::istream& input_;
    std::size_t max_payload_size_;
};

class FrameWriter final {
  public:
    explicit FrameWriter(std::ostream& output);

    void write(std::string_view payload);

  private:
    std::ostream& output_;
};

} // namespace hlsl_intellisense::json_rpc
