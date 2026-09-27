#include <hlsl_intellisense/capture/capture.h>

#include "wire.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/un.h>
#endif

namespace hlsl_intellisense::capture {

namespace {
void transmit(const std::string& endpoint, const std::string& bytes) {
#ifdef _WIN32
    const std::wstring name(endpoint.begin(), endpoint.end());
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        if (detail::write_exact(pipe, bytes.data(), bytes.size())) {
            detail::Json response;
            if (detail::receive_frame(pipe, response)) {
                const char confirmation = 1;
                (void)detail::write_exact(pipe, &confirmation, 1);
            }
        }
        CloseHandle(pipe);
    }
#else
    const int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_fd < 0) {
        return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint.size() < sizeof(address.sun_path)) {
        std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
        const auto connected =
            connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        bool ready = connected == 0;
        if (!ready && errno == EINPROGRESS) {
            pollfd descriptor{socket_fd, POLLOUT, 0};
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            ready = poll(&descriptor, 1, 250) > 0 &&
                    getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                    socket_error == 0;
        }
        if (ready) {
            (void)fcntl(socket_fd, F_SETFL, fcntl(socket_fd, F_GETFL) & ~O_NONBLOCK);
            timeval timeout{0, 250000};
            (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (detail::write_exact(socket_fd, bytes.data(), bytes.size())) {
                detail::Json response;
                (void)detail::receive_frame(socket_fd, response);
            }
        }
    }
    close(socket_fd);
#endif
}
} // namespace

struct Client::State {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> pending;
    std::thread worker;
    std::atomic_bool enabled{};
    bool stopping{};
    std::string endpoint;
    std::string token;
};

Client::Client() : state_{std::make_unique<State>()} {}
Client::~Client() { disconnect(); }

bool Client::connect(std::string endpoint, std::string token) {
    disconnect();
#ifdef _WIN32
    if (!endpoint.starts_with(R"(\\.\pipe\hlsl-capture-)")) {
        return false;
    }
#else
    if (endpoint.empty() || endpoint.size() >= sizeof(sockaddr_un::sun_path) ||
        endpoint.front() != '/') {
        return false;
    }
#endif
    if (token.size() != 64) {
        return false;
    }
    for (const char digit : token) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return false;
        }
    }
    {
        std::lock_guard lock{state_->mutex};
        state_->endpoint = std::move(endpoint);
        state_->token = std::move(token);
        state_->stopping = false;
        state_->enabled = true;
    }
    state_->worker = std::thread([state = state_.get()] {
        for (;;) {
            std::string bytes;
            {
                std::unique_lock lock{state->mutex};
                state->ready.wait(lock, [&] { return state->stopping || !state->pending.empty(); });
                if (state->pending.empty()) {
                    return;
                }
                bytes = std::move(state->pending.front());
                state->pending.pop_front();
            }
            transmit(state->endpoint, bytes);
            std::fill(bytes.begin(), bytes.end(), '\0');
        }
    });
    return true;
}

void Client::disconnect() noexcept {
    state_->enabled = false;
    {
        std::lock_guard lock{state_->mutex};
        state_->stopping = true;
    }
    state_->ready.notify_one();
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
    std::lock_guard lock{state_->mutex};
    std::fill(state_->token.begin(), state_->token.end(), '\0');
    state_->token.clear();
    state_->endpoint.clear();
}

bool Client::report(const Invocation& invocation) noexcept {
    if (!state_->enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    try {
        std::unique_lock lock{state_->mutex, std::try_to_lock};
        if (!lock.owns_lock() || !state_->enabled || state_->pending.size() >= 64) {
            return false;
        }
        if (!detail::valid(invocation)) {
            return false;
        }
        auto bytes = detail::frame({{"version", protocol_version},
                                    {"token", state_->token},
                                    {"event", detail::encode(invocation)}});
        if (bytes.empty()) {
            return false;
        }
        state_->pending.push_back(std::move(bytes));
        state_->ready.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace hlsl_intellisense::capture
