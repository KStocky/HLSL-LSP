#pragma once

#include <hlsl_intellisense/workspace/error.h>

#include <string>
#include <string_view>

namespace hlsl_intellisense::workspace {

enum class PathStyle {
    native,
    posix,
    windows,
};

class DocumentUri {
  public:
    [[nodiscard]] static DocumentUri from_uri(std::string_view uri,
                                              PathStyle style = PathStyle::native);
    [[nodiscard]] static DocumentUri from_path(std::string_view path,
                                               PathStyle style = PathStyle::native);

    [[nodiscard]] const std::string& uri() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& identity() const noexcept;

    bool operator==(const DocumentUri&) const = default;

  private:
    DocumentUri(std::string uri, std::string path, std::string identity);

    std::string uri_;
    std::string path_;
    std::string identity_;
};

} // namespace hlsl_intellisense::workspace
