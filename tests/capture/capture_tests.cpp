#include <hlsl_intellisense/capture/capture.h>
#include <hlsl_intellisense/capture/session.h>

#include "../../src/capture/wire.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cstdlib>
#include <sys/stat.h>
#include <sys/un.h>
#endif

namespace {
using namespace hlsl_intellisense::capture;
using namespace std::chrono_literals;

#ifndef _WIN32
struct RuntimeDirectory {
    std::filesystem::path path =
        std::filesystem::current_path() / ("capture-runtime-" + std::to_string(getpid()));
    std::string old = std::getenv("XDG_RUNTIME_DIR") ? std::getenv("XDG_RUNTIME_DIR") : "";
    bool had_old = std::getenv("XDG_RUNTIME_DIR") != nullptr;
    RuntimeDirectory() {
        std::filesystem::create_directory(path);
        (void)chmod(path.c_str(), 0700);
        (void)setenv("XDG_RUNTIME_DIR", path.string().c_str(), 1);
    }
    ~RuntimeDirectory() {
        if (had_old) {
            (void)setenv("XDG_RUNTIME_DIR", old.c_str(), 1);
        } else {
            (void)unsetenv("XDG_RUNTIME_DIR");
        }
        std::filesystem::remove_all(path);
    }
};
#endif

bool wait_for(const Session& session, std::uint64_t accepted, std::uint64_t rejected) {
    for (int i = 0; i < 100; ++i) {
        const auto snapshot = session.snapshot();
        if (snapshot.accepted >= accepted && snapshot.rejected >= rejected) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void raw(const std::string& endpoint, const std::string& payload) {
#ifdef _WIN32
    const std::wstring path(endpoint.begin(), endpoint.end());
    HANDLE peer = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && peer == INVALID_HANDLE_VALUE; ++i) {
        peer = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (peer == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(10ms);
        }
    }
    REQUIRE(peer != INVALID_HANDLE_VALUE);
    REQUIRE(detail::write_exact(peer, payload.data(), payload.size()));
    detail::Json response;
    REQUIRE(detail::receive_frame(peer, response));
    const char confirmation = 1;
    REQUIRE(detail::write_exact(peer, &confirmation, 1));
    CloseHandle(peer);
#else
    const auto peer = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(peer >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    REQUIRE(connect(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(detail::write_exact(peer, payload.data(), payload.size()));
    timeval timeout{0, 500000};
    REQUIRE(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    detail::Json response;
    REQUIRE(detail::receive_frame(peer, response));
    close(peer);
#endif
}
} // namespace

TEST_CASE("Capture SDK is opt-in, deduplicates and rejects stale or invalid frames", "[capture]") {
#ifndef _WIN32
    RuntimeDirectory runtime;
#endif
    Session session;
    Client client;
    Invocation invocation;
    invocation.source = "virtual:/material/gradient.hlsl";
    invocation.entry_point = "Main";
    invocation.target_profile = "ps_6_7";
    invocation.defines = {{"QUALITY", "1"}};
    CHECK_FALSE(client.report(invocation));
    REQUIRE(session.start());
    const auto endpoint = session.snapshot().endpoint;
    REQUIRE(client.connect(endpoint, session.token()));
    {
        auto oversized_identity = invocation;
        oversized_identity.source = std::string(2049, 'x');
        CHECK_FALSE(client.report(oversized_identity));
    }
    REQUIRE(client.report(invocation));
    REQUIRE(client.report(invocation));
    const bool received = wait_for(session, 2, 0);
    INFO("accepted=" << session.snapshot().accepted << " rejected=" << session.snapshot().rejected);
    REQUIRE(received);
    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.entries.size() == 1);
    CHECK(snapshot.entries.front().count == 2);
    CHECK(snapshot.entries.front().invocation.source == invocation.source);

    raw(endpoint, detail::frame({{"version", 1},
                                 {"token", std::string(64, '0')},
                                 {"event", detail::encode(invocation)}}));
    REQUIRE(wait_for(session, 2, 1));
    raw(endpoint,
        detail::frame(
            {{"version", 2}, {"token", session.token()}, {"event", detail::encode(invocation)}}));
    REQUIRE(wait_for(session, 2, 2));
    raw(endpoint,
        detail::frame({{"version", 1}, {"token", session.token()}, {"event", {{"source", 4}}}}));
    REQUIRE(wait_for(session, 2, 3));
    std::string oversized(4, '\0');
    oversized[1] = 1;
    raw(endpoint, oversized);
    REQUIRE(wait_for(session, 2, 4));
    std::string nested(17, '[');
    nested += "0";
    nested += std::string(17, ']');
    std::string depth_frame(4, '\0');
    const auto size = static_cast<unsigned>(nested.size());
    depth_frame[3] = static_cast<char>(size);
    depth_frame += nested;
    raw(endpoint, depth_frame);
    REQUIRE(wait_for(session, 2, 5));
    client.disconnect();
    session.stop();
    CHECK_FALSE(client.report(invocation));
    CHECK_FALSE(session.snapshot().active);
}

TEST_CASE("Capture bounds distinct permutations and retains total occurrences", "[capture]") {
#ifndef _WIN32
    RuntimeDirectory runtime;
#endif
    Session session;
    REQUIRE(session.start());
    const auto endpoint = session.snapshot().endpoint;
    const auto token = session.token();
    Invocation invocation;
    invocation.source = "virtual:/generated/fixture.hlsl";
    invocation.target_profile = "cs_6_7";
    for (int i = 0; i < 260; ++i) {
        INFO("permutation=" << i << " accepted=" << session.snapshot().accepted
                            << " rejected=" << session.snapshot().rejected);
        invocation.defines = {{"PERMUTATION", std::to_string(i)}};
        raw(endpoint,
            detail::frame(
                {{"version", 1}, {"token", token}, {"event", detail::encode(invocation)}}));
    }
    REQUIRE(wait_for(session, 260, 0));
    const auto snapshot = session.snapshot();
    CHECK(snapshot.entries.size() == 256);
    CHECK(snapshot.overflow == 4);
    CHECK(snapshot.accepted == 260);
}
