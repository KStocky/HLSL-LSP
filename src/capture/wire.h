#pragma once

#include <hlsl_intellisense/capture/capture.h>

#include <nlohmann/json.hpp>

#include <array>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hlsl_intellisense::capture::detail {

using Json = nlohmann::json;
inline constexpr std::size_t max_frame = 32 * 1024;

inline bool valid(const Invocation& value) {
    if (value.source.empty() || value.target_profile.empty() || value.source.size() > 2048 ||
        value.entry_point.size() > 256 || value.target_profile.size() > 128 ||
        value.language_version.size() > 128 || value.output_mode.size() > 128 ||
        value.pipeline.size() > 256 || value.stage.size() > 128 || value.defines.size() > 64 ||
        value.include_directories.size() > 64 || value.virtual_mappings.size() > 64 ||
        value.arguments.size() > 64) {
        return false;
    }
    const auto strings_fit = [](const auto& values) {
        for (const auto& item : values) {
            if (item.size() > 2048) {
                return false;
            }
        }
        return true;
    };
    const auto pairs_fit = [](const auto& values) {
        for (const auto& [key, item] : values) {
            if (key.size() > 2048 || item.size() > 2048) {
                return false;
            }
        }
        return true;
    };
    return strings_fit(value.include_directories) && strings_fit(value.arguments) &&
           pairs_fit(value.defines) && pairs_fit(value.virtual_mappings);
}

inline Json encode(const Invocation& value) {
    return {{"source", value.source},
            {"entryPoint", value.entry_point},
            {"targetProfile", value.target_profile},
            {"languageVersion", value.language_version},
            {"defines", value.defines},
            {"includeDirectories", value.include_directories},
            {"virtualMappings", value.virtual_mappings},
            {"arguments", value.arguments},
            {"outputMode", value.output_mode},
            {"pipeline", value.pipeline},
            {"stage", value.stage}};
}

inline bool decode(const Json& json, Invocation& value) {
    if (!json.is_object() || json.size() != 11) {
        return false;
    }
    try {
        value.source = json.at("source").get<std::string>();
        value.entry_point = json.at("entryPoint").get<std::string>();
        value.target_profile = json.at("targetProfile").get<std::string>();
        value.language_version = json.at("languageVersion").get<std::string>();
        value.defines = json.at("defines").get<decltype(value.defines)>();
        value.include_directories =
            json.at("includeDirectories").get<decltype(value.include_directories)>();
        value.virtual_mappings = json.at("virtualMappings").get<decltype(value.virtual_mappings)>();
        value.arguments = json.at("arguments").get<decltype(value.arguments)>();
        value.output_mode = json.at("outputMode").get<std::string>();
        value.pipeline = json.at("pipeline").get<std::string>();
        value.stage = json.at("stage").get<std::string>();
    } catch (const Json::exception&) {
        return false;
    }
    return valid(value);
}

inline std::string frame(const Json& json) {
    auto payload = json.dump();
    if (payload.size() > max_frame) {
        return {};
    }
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string result(4, '\0');
    for (int i = 0; i < 4; ++i) {
        result[static_cast<std::size_t>(i)] = static_cast<char>(size >> (24 - 8 * i));
    }
    result += payload;
    return result;
}

#ifdef _WIN32
using Handle = HANDLE;
inline bool read_exact(Handle handle, char* bytes, std::size_t size) {
    // An overlapped operation bounds the time a peer may hold the sole listener.
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) {
        return false;
    }
    bool ok = true;
    while (size != 0 && ok) {
        DWORD count = 0;
        if (!ReadFile(handle, bytes, static_cast<DWORD>(size), &count, &operation)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(operation.hEvent, 250) != WAIT_OBJECT_0) {
                    CancelIoEx(handle, &operation);
                    DWORD cancelled = 0;
                    (void)GetOverlappedResult(handle, &operation, &cancelled, TRUE);
                    ok = false;
                } else if (!GetOverlappedResult(handle, &operation, &count, FALSE)) {
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        if (count == 0) {
            ok = false;
        }
        bytes += count;
        size -= count;
        ResetEvent(operation.hEvent);
        operation.Offset = 0;
        operation.OffsetHigh = 0;
    }
    CloseHandle(operation.hEvent);
    return ok;
}
inline bool write_exact(Handle handle, const char* bytes, std::size_t size) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) {
        return false;
    }
    DWORD count = 0;
    bool ok =
        size <= MAXDWORD && WriteFile(handle, bytes, static_cast<DWORD>(size), &count, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(operation.hEvent, 250) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &operation, &count, FALSE) != 0;
        } else {
            CancelIoEx(handle, &operation);
            DWORD cancelled = 0;
            (void)GetOverlappedResult(handle, &operation, &cancelled, TRUE);
        }
    }
    CloseHandle(operation.hEvent);
    return ok && count == size;
}
#else
using Handle = int;
inline bool read_exact(Handle handle, char* bytes, std::size_t size) {
    while (size != 0) {
        const auto count = recv(handle, bytes, size, 0);
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
inline bool write_exact(Handle handle, const char* bytes, std::size_t size) {
    while (size != 0) {
        const auto count = send(handle, bytes, size, MSG_NOSIGNAL);
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
#endif

inline bool send_frame(Handle handle, const Json& value) {
    const auto data = frame(value);
    return !data.empty() && write_exact(handle, data.data(), data.size());
}
inline bool receive_frame(Handle handle, Json& result) {
    std::array<unsigned char, 4> header{};
    if (!read_exact(handle, reinterpret_cast<char*>(header.data()), header.size())) {
        return false;
    }
    std::uint32_t size = 0;
    for (auto byte : header) {
        size = (size << 8) | byte;
    }
    if (size == 0 || size > max_frame) {
        return false;
    }
    std::string payload(size, '\0');
    if (!read_exact(handle, payload.data(), size)) {
        return false;
    }
    bool quoted = false;
    bool escaped = false;
    unsigned depth = 0;
    for (const char character : payload) {
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                quoted = false;
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == '{' || character == '[') {
            if (++depth > 16) {
                return false;
            }
        } else if (character == '}' || character == ']') {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    result = Json::parse(payload, nullptr, false);
    return !result.is_discarded();
}

} // namespace hlsl_intellisense::capture::detail
