#pragma once

#include <stdexcept>
#include <string_view>

namespace hlsl_intellisense::workspace {

enum class DocumentErrorCode {
    invalid_uri,
    invalid_path,
    duplicate_open,
    document_not_found,
    document_not_open,
    version_not_increasing,
    invalid_position,
    invalid_range,
    range_length_mismatch,
    malformed_utf8,
};

class DocumentError final : public std::runtime_error {
  public:
    DocumentError(DocumentErrorCode code, std::string_view message);

    [[nodiscard]] DocumentErrorCode code() const noexcept;

  private:
    DocumentErrorCode code_;
};

} // namespace hlsl_intellisense::workspace
