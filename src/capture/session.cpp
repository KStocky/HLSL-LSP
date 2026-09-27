#include <hlsl_intellisense/capture/session.h>

#include "wire.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <bcrypt.h>
#include <sddl.h>
#else
#include <poll.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

namespace hlsl_intellisense::capture {
namespace {
bool random_bytes(unsigned char* bytes, std::size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return getrandom(bytes, size, 0) == static_cast<ssize_t>(size);
#endif
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }

    return result;
}

bool same_token(std::string_view supplied, std::string_view expected) {
    if (supplied.size() != expected.size()) {
        return false;
    }
    unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned>(static_cast<unsigned char>(supplied[i]) ^
                                            static_cast<unsigned char>(expected[i]));
    }
    return difference == 0;
}

#ifdef _WIN32
struct PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PrivateSecurity() {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return;
        }
        DWORD length = 0;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        std::vector<unsigned char> buffer(length);
        LPWSTR sid{};
        if (GetTokenInformation(token, TokenUser, buffer.data(), length, &length)) {
            (void)ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                                         &sid);
        }
        CloseHandle(token);
        if (sid) {
            const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
            (void)ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr);
            LocalFree(sid);
        }
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~PrivateSecurity() {
        if (descriptor) {
            LocalFree(descriptor);
        }
    }
};
#endif
} // namespace

struct Session::State {
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic_bool running{};
    std::string endpoint;
    std::string secret;
    std::map<std::string, Entry> entries;
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t overflow{};
#ifdef _WIN32
    PrivateSecurity security;
#else
    int listener{-1};
#endif
};

Session::Session() : state_{std::make_unique<State>()} {}
Session::~Session() { stop(); }

bool Session::start() {
    stop();
    std::array<unsigned char, 32> secret{};
    std::array<unsigned char, 12> identity{};
    if (!random_bytes(secret.data(), secret.size()) ||
        !random_bytes(identity.data(), identity.size())) {
        return false;
    }
    std::string endpoint;
#ifdef _WIN32
    if (!state_->security.descriptor) {
        return false;
    }
    endpoint = R"(\\.\pipe\hlsl-capture-)" + hex(identity.data(), identity.size());
    const std::wstring pipe_name(endpoint.begin(), endpoint.end());
    const auto first = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(detail::max_frame + 4), static_cast<DWORD>(detail::max_frame + 4), 0,
        &state_->security.attributes);
    if (first == INVALID_HANDLE_VALUE) {
        return false;
    }
#else
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    struct stat directory{};
    if (!runtime || stat(runtime, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != getuid() || (directory.st_mode & 077) != 0) {
        return false;
    }
    endpoint = std::string(runtime) + "/hlsl-capture-" + hex(identity.data(), identity.size());
    if (endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
        return false;
    }
    const int first = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (first < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    if (bind(first, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(first);
        return false;
    }
    if (chmod(endpoint.c_str(), 0600) != 0 || listen(first, 8) != 0) {
        close(first);
        unlink(endpoint.c_str());
        return false;
    }
    state_->listener = first;
#endif
    {
        std::lock_guard lock{state_->mutex};
        state_->endpoint = std::move(endpoint);
        state_->secret = hex(secret.data(), secret.size());
        state_->entries.clear();
        state_->accepted = state_->rejected = state_->overflow = 0;
    }
    state_->running = true;
    state_->worker = std::thread([state = state_.get()
#ifdef _WIN32
                                      ,
                                  first
#endif
    ] {
#ifdef _WIN32
        HANDLE pipe = first;
        const std::wstring name(state->endpoint.begin(), state->endpoint.end());
#endif
        while (state->running) {
#ifdef _WIN32
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool connected = ConnectNamedPipe(pipe, &operation) != 0;
            DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
            if (!connected && connect_error == ERROR_IO_PENDING) {
                while (state->running &&
                       WaitForSingleObject(operation.hEvent, 100) == WAIT_TIMEOUT) {
                }
                DWORD transferred = 0;
                connected =
                    state->running && GetOverlappedResult(pipe, &operation, &transferred, FALSE);
                connect_error = connected ? ERROR_SUCCESS : GetLastError();
                if (!connected) {
                    CancelIoEx(pipe, &operation);
                    DWORD cancelled = 0;
                    (void)GetOverlappedResult(pipe, &operation, &cancelled, TRUE);
                }
            } else if (!connected && connect_error == ERROR_PIPE_CONNECTED) {
                connected = true;
            }
            CloseHandle(operation.hEvent);
            if (!connected &&
                (connect_error == ERROR_NO_DATA || connect_error == ERROR_BROKEN_PIPE) &&
                state->running) {
                DisconnectNamedPipe(pipe);
                continue;
            }
            if (!state->running) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                break;
            }
            if (!connected) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                break;
            }
            const auto peer = pipe;
#else
            pollfd descriptor{state->listener, POLLIN, 0};
            if (poll(&descriptor, 1, 100) <= 0) {
                continue;
            }
            const auto peer = accept4(state->listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (peer < 0) {
                continue;
            }
            timeval timeout{0, 250000};
            (void)setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            const bool authorized_user =
                getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
                credentials.uid == getuid();
#endif
            detail::Json message;
            const bool received =
#ifdef _WIN32
                true &&
#else
                authorized_user &&
#endif
                detail::receive_frame(peer, message);
            Invocation invocation;
            bool valid = false;
            std::string key;
            if (received && message.is_object()) {
                try {
                    valid = message.size() == 3 && message.at("version") == protocol_version &&
                            message.at("token").is_string() &&
                            same_token(message.at("token").get<std::string>(), state->secret) &&
                            detail::decode(message.at("event"), invocation);
                    if (valid) {
                        key = detail::encode(invocation).dump();
                    }
                } catch (const detail::Json::exception&) {
                    valid = false;
                }
            }
            {
                std::lock_guard lock{state->mutex};
                if (!valid) {
                    ++state->rejected;
                } else {
                    ++state->accepted;
                    auto found = state->entries.find(key);
                    if (found != state->entries.end()) {
                        ++found->second.count;
                    } else if (state->entries.size() < 256) {
                        state->entries.emplace(std::move(key), Entry{std::move(invocation), 1});
                    } else {
                        ++state->overflow;
                    }
                }
            }
            (void)detail::send_frame(peer, {{"status", valid ? "accepted" : "rejected"}});
#ifdef _WIN32
            char confirmation{};
            (void)detail::read_exact(peer, &confirmation, 1);
            DisconnectNamedPipe(peer);
            CloseHandle(peer);
            pipe = INVALID_HANDLE_VALUE;
            if (state->running) {
                pipe = CreateNamedPipeW(
                    name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                    static_cast<DWORD>(detail::max_frame + 4),
                    static_cast<DWORD>(detail::max_frame + 4), 0, &state->security.attributes);
                if (pipe == INVALID_HANDLE_VALUE) {
                    break;
                }
            }
#else
            close(peer);
#endif
        }
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
#endif
        state->running = false;
    });
    return true;
}

void Session::stop() noexcept {
    state_->running = false;
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
#ifndef _WIN32
    if (state_->listener >= 0) {
        close(state_->listener);
        state_->listener = -1;
        unlink(state_->endpoint.c_str());
    }
#endif
    std::lock_guard lock{state_->mutex};
    std::fill(state_->secret.begin(), state_->secret.end(), '\0');
    state_->secret.clear();
    state_->endpoint.clear();
    state_->entries.clear();
    state_->accepted = state_->rejected = state_->overflow = 0;
}

std::string Session::token() const {
    std::lock_guard lock{state_->mutex};
    return state_->secret;
}

Snapshot Session::snapshot() const {
    std::lock_guard lock{state_->mutex};
    Snapshot result;
    result.active = state_->running;
    result.endpoint = state_->endpoint;
    result.accepted = state_->accepted;
    result.rejected = state_->rejected;
    result.overflow = state_->overflow;
    for (const auto& [key, entry] : state_->entries) {
        (void)key;
        result.entries.push_back(entry);
    }
    return result;
}

} // namespace hlsl_intellisense::capture
