#include <hlsl_intellisense/workspace/document_uri.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace hlsl_intellisense::workspace {
namespace {

[[nodiscard]] PathStyle resolved_style(PathStyle style) {
    if (style != PathStyle::native) {
        return style;
    }
#ifdef _WIN32
    return PathStyle::windows;
#else
    return PathStyle::posix;
#endif
}

[[nodiscard]] char ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), ascii_lower);
    return value;
}

[[nodiscard]] bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
               return ascii_lower(lhs) == ascii_lower(rhs);
           });
}

[[nodiscard]] int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::string percent_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            throw DocumentError{DocumentErrorCode::invalid_uri, "Incomplete URI escape"};
        }
        const auto high = hex_value(value[index + 1]);
        const auto low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            throw DocumentError{DocumentErrorCode::invalid_uri, "Invalid URI escape"};
        }
        const auto decoded = static_cast<char>((high << 4) | low);
        if (decoded == '\0') {
            throw DocumentError{DocumentErrorCode::invalid_uri, "URI contains a null byte"};
        }
        result.push_back(decoded);
        index += 2;
    }
    return result;
}

[[nodiscard]] bool is_uri_path_byte(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~' || value == '/' || value == ':';
}

[[nodiscard]] std::string percent_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (is_uri_path_byte(byte)) {
            result.push_back(character);
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0F]);
        }
    }
    return result;
}

struct SegmentBounds {
    std::size_t start;
    std::size_t protected_segments{};
};

[[nodiscard]] std::vector<std::string> normalized_segments(std::string_view path,
                                                           SegmentBounds bounds) {
    std::vector<std::string> segments;
    auto start = bounds.start;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (segments.size() > bounds.protected_segments) {
                    segments.pop_back();
                }
            } else {
                segments.emplace_back(segment);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

[[nodiscard]] std::string join(const std::vector<std::string>& segments, char separator) {
    std::string result;
    for (const auto& segment : segments) {
        if (!result.empty()) {
            result.push_back(separator);
        }
        result += segment;
    }
    return result;
}

[[nodiscard]] DocumentUri make_posix(std::string path) {
    if (path.empty() || path.front() != '/') {
        throw DocumentError{DocumentErrorCode::invalid_path, "POSIX path must be absolute"};
    }
    const auto normalized = '/' + join(normalized_segments(path, {.start = 1}), '/');
    return DocumentUri::from_uri("file://" + percent_encode(normalized), PathStyle::posix);
}

[[nodiscard]] DocumentUri make_windows(std::string path) {
    std::ranges::replace(path, '\\', '/');
    if (path.starts_with("//")) {
        const auto segments = normalized_segments(path, {.start = 2, .protected_segments = 2});
        if (segments.size() < 2) {
            throw DocumentError{DocumentErrorCode::invalid_path,
                                "UNC path must include a server and share"};
        }
        const auto authority = lower_ascii(segments.front());
        std::vector<std::string> tail{segments.begin() + 1, segments.end()};
        return DocumentUri::from_uri("file://" + authority + "/" + percent_encode(join(tail, '/')),
                                     PathStyle::windows);
    }
    if (path.size() < 3 || std::isalpha(static_cast<unsigned char>(path[0])) == 0 ||
        path[1] != ':' || path[2] != '/') {
        throw DocumentError{DocumentErrorCode::invalid_path, "Windows path must be absolute"};
    }
    path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    const auto segments = normalized_segments(path, {.start = 3});
    std::string uri_path{"/"};
    uri_path.append(path, 0, 2);
    uri_path.push_back('/');
    uri_path += join(segments, '/');
    return DocumentUri::from_uri("file://" + percent_encode(uri_path), PathStyle::windows);
}

} // namespace

DocumentError::DocumentError(DocumentErrorCode code, std::string_view message)
    : std::runtime_error{std::string{message}}, code_{code} {}

DocumentErrorCode DocumentError::code() const noexcept { return code_; }

DocumentUri::DocumentUri(std::string uri, std::string path, std::string identity)
    : uri_{std::move(uri)}, path_{std::move(path)}, identity_{std::move(identity)} {}

DocumentUri DocumentUri::from_uri(std::string_view uri, PathStyle style) {
    const auto colon = uri.find(':');
    if (colon == std::string_view::npos ||
        !equals_ascii_case_insensitive(uri.substr(0, colon), "file")) {
        throw DocumentError{DocumentErrorCode::invalid_uri, "Only file URIs are supported"};
    }
    if (uri.find_first_of("?#", colon + 1) != std::string_view::npos) {
        throw DocumentError{DocumentErrorCode::invalid_uri,
                            "File URI query and fragment components are not supported"};
    }

    auto remainder = uri.substr(colon + 1);
    if (!remainder.starts_with("//")) {
        throw DocumentError{DocumentErrorCode::invalid_uri, "File URI must contain an authority"};
    }
    remainder.remove_prefix(2);
    const auto slash = remainder.find('/');
    if (slash == std::string_view::npos) {
        throw DocumentError{DocumentErrorCode::invalid_uri,
                            "File URI must contain an absolute path"};
    }
    auto authority = percent_decode(remainder.substr(0, slash));
    auto uri_path = percent_decode(remainder.substr(slash));
    if (authority.find_first_of("/\\") != std::string::npos) {
        throw DocumentError{DocumentErrorCode::invalid_uri,
                            "File URI authority contains a path separator"};
    }

    if (resolved_style(style) == PathStyle::posix) {
        if (!authority.empty() && !equals_ascii_case_insensitive(authority, "localhost")) {
            throw DocumentError{DocumentErrorCode::invalid_uri,
                                "Remote file authorities are not POSIX paths"};
        }
        const auto path = '/' + join(normalized_segments(uri_path, {.start = 1}), '/');
        return DocumentUri{"file://" + percent_encode(path), path, path};
    }

    std::ranges::replace(uri_path, '\\', '/');
    if (!authority.empty() && !equals_ascii_case_insensitive(authority, "localhost")) {
        authority = lower_ascii(std::move(authority));
        const auto segments = normalized_segments(uri_path, {.start = 1, .protected_segments = 1});
        if (segments.empty()) {
            throw DocumentError{DocumentErrorCode::invalid_uri,
                                "UNC file URI must include a share"};
        }
        const auto slash_path = join(segments, '/');
        auto path = "\\\\" + authority + "\\" + join(segments, '\\');
        return DocumentUri{"file://" + authority + "/" + percent_encode(slash_path),
                           std::move(path),
                           lower_ascii("\\\\" + authority + "\\" + join(segments, '\\'))};
    }

    if (uri_path.size() < 4 || uri_path.front() != '/' ||
        std::isalpha(static_cast<unsigned char>(uri_path[1])) == 0 || uri_path[2] != ':' ||
        uri_path[3] != '/') {
        throw DocumentError{DocumentErrorCode::invalid_uri,
                            "Windows file URI must contain an absolute drive path"};
    }
    const auto drive = static_cast<char>(std::toupper(static_cast<unsigned char>(uri_path[1])));
    const auto segments = normalized_segments(uri_path, {.start = 3});
    auto slash_path = std::string{"/"} + drive + ":/" + join(segments, '/');
    auto path = std::string{drive} + ":\\" + join(segments, '\\');
    auto identity = lower_ascii(path);
    return DocumentUri{"file://" + percent_encode(slash_path), std::move(path),
                       std::move(identity)};
}

DocumentUri DocumentUri::from_path(std::string_view path, PathStyle style) {
    if (resolved_style(style) == PathStyle::windows) {
        return make_windows(std::string{path});
    }
    return make_posix(std::string{path});
}

const std::string& DocumentUri::uri() const noexcept { return uri_; }

const std::string& DocumentUri::path() const noexcept { return path_; }

const std::string& DocumentUri::identity() const noexcept { return identity_; }

} // namespace hlsl_intellisense::workspace
