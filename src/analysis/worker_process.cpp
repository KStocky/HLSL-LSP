#include <hlsl_intellisense/analysis/worker_process.h>

#include <hlsl_intellisense/analysis/worker_protocol.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace hlsl_intellisense::analysis {
namespace {

using namespace std::chrono_literals;
using Json = json_rpc::Json;

constexpr std::size_t max_header_size = std::size_t{8} * 1024U;
constexpr auto forced_exit_wait = 250ms;

class PipeFailure final : public std::runtime_error {
  public:
    PipeFailure(bool eof, std::string message)
        : std::runtime_error{std::move(message)}, eof_{eof} {}

    [[nodiscard]] bool eof() const noexcept { return eof_; }

  private:
    bool eof_;
};

[[nodiscard]] std::string system_message(std::string_view action, int error) {
    return std::string{action} + ": " + std::system_category().message(error);
}

#ifdef _WIN32

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_{handle} {}
    UniqueHandle(const UniqueHandle&) = delete;
    auto operator=(const UniqueHandle&) -> UniqueHandle& = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}
    auto operator=(UniqueHandle&& other) noexcept -> UniqueHandle& {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle_));
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_{};
};

[[nodiscard]] std::string windows_message(std::string_view action, DWORD error) {
    return system_message(action, static_cast<int>(error));
}

[[nodiscard]] std::filesystem::path current_executable() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("GetModuleFileNameW failed", ::GetLastError())};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        if (buffer.size() >= std::size_t{1} << 16U) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     "Current executable path is too long"};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{argument};
    }
    std::wstring result{L'"'};
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] UniqueHandle inheritable_stderr() {
    const auto stderr_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (stderr_handle != nullptr && stderr_handle != INVALID_HANDLE_VALUE) {
        HANDLE duplicate{};
        if (::DuplicateHandle(::GetCurrentProcess(), stderr_handle, ::GetCurrentProcess(),
                              &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE) {
            return UniqueHandle{duplicate};
        }
    }
    return UniqueHandle{::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
}

class ChildProcess final {
  public:
    static std::shared_ptr<ChildProcess> launch(const std::filesystem::path& executable,
                                                const std::string& dxc_runtime_directory) {
        SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                       .lpSecurityDescriptor = nullptr,
                                       .bInheritHandle = TRUE};
        HANDLE child_stdin_raw{};
        HANDLE parent_stdin_raw{};
        if (::CreatePipe(&child_stdin_raw, &parent_stdin_raw, &attributes, 0) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("CreatePipe for stdin failed", ::GetLastError())};
        }
        UniqueHandle child_stdin{child_stdin_raw};
        UniqueHandle parent_stdin{parent_stdin_raw};

        HANDLE parent_stdout_raw{};
        HANDLE child_stdout_raw{};
        if (::CreatePipe(&parent_stdout_raw, &child_stdout_raw, &attributes, 0) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("CreatePipe for stdout failed", ::GetLastError())};
        }
        UniqueHandle parent_stdout{parent_stdout_raw};
        UniqueHandle child_stdout{child_stdout_raw};
        if (::SetHandleInformation(parent_stdin.get(), HANDLE_FLAG_INHERIT, 0) == FALSE ||
            ::SetHandleInformation(parent_stdout.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("SetHandleInformation failed", ::GetLastError())};
        }

        auto child_stderr = inheritable_stderr();
        if (child_stderr.get() == nullptr || child_stderr.get() == INVALID_HANDLE_VALUE) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     windows_message("Unable to inherit stderr", ::GetLastError())};
        }

        SIZE_T attribute_size{};
        static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size));
        std::vector<std::byte> attribute_storage(attribute_size);
        auto* attribute_list =
            reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (::InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("InitializeProcThreadAttributeList failed", ::GetLastError())};
        }
        struct AttributeListGuard final {
            PPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeListGuard() { ::DeleteProcThreadAttributeList(value); }
        } attribute_guard{attribute_list};

        HANDLE inherited_handles[]{child_stdin.get(), child_stdout.get(), child_stderr.get()};
        if (::UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        inherited_handles, sizeof(inherited_handles), nullptr,
                                        nullptr) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("UpdateProcThreadAttribute failed", ::GetLastError())};
        }

        std::wstring command_line = quote_argument(executable.wstring());
        if (!dxc_runtime_directory.empty()) {
            command_line += L" --dxc-runtime ";
            command_line += quote_argument(std::filesystem::path{dxc_runtime_directory}.wstring());
        }
        command_line.push_back(L'\0');

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_stdin.get();
        startup.StartupInfo.hStdOutput = child_stdout.get();
        startup.StartupInfo.hStdError = child_stderr.get();
        startup.lpAttributeList = attribute_list;
        PROCESS_INFORMATION process{};
        if (::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                             &startup.StartupInfo, &process) == FALSE) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                windows_message("CreateProcessW failed for " + executable.string(),
                                ::GetLastError())};
        }
        UniqueHandle thread{process.hThread};
        child_stdin.reset();
        child_stdout.reset();
        child_stderr.reset();
        return std::shared_ptr<ChildProcess>{
            new ChildProcess{UniqueHandle{process.hProcess}, std::move(parent_stdin),
                             std::move(parent_stdout), process.dwProcessId}};
    }

    ChildProcess(const ChildProcess&) = delete;
    auto operator=(const ChildProcess&) -> ChildProcess& = delete;
    ~ChildProcess() {
        terminate();
        close_pipes();
    }

    void write_all(std::string_view data) {
        std::size_t offset{};
        while (offset < data.size()) {
            const auto remaining = data.size() - offset;
            const auto chunk = static_cast<DWORD>(
                std::min(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written{};
            if (::WriteFile(stdin_write_.get(), data.data() + offset, chunk, &written, nullptr) ==
                FALSE) {
                const auto error = ::GetLastError();
                throw PipeFailure{error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA,
                                  windows_message("Writing to analysis worker failed", error)};
            }
            if (written == 0) {
                throw PipeFailure{false, "Writing to analysis worker made no progress"};
            }
            offset += written;
        }
    }

    void read_all(char* data, std::size_t size) {
        std::size_t offset{};
        while (offset < size) {
            const auto remaining = size - offset;
            const auto chunk = static_cast<DWORD>(
                std::min(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD read{};
            if (::ReadFile(stdout_read_.get(), data + offset, chunk, &read, nullptr) == FALSE) {
                const auto error = ::GetLastError();
                throw PipeFailure{error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF,
                                  windows_message("Reading from analysis worker failed", error)};
            }
            if (read == 0) {
                throw PipeFailure{true, "Analysis worker stdout reached EOF"};
            }
            offset += read;
        }
    }

    [[nodiscard]] std::optional<unsigned long> poll_exit() {
        std::scoped_lock lock{process_mutex_};
        if (process_.get() == nullptr) {
            return exit_code_;
        }
        DWORD code{};
        if (::GetExitCodeProcess(process_.get(), &code) == FALSE || code == STILL_ACTIVE) {
            return std::nullopt;
        }
        exit_code_ = code;
        process_.reset();
        return exit_code_;
    }

    [[nodiscard]] bool wait_for_exit(std::chrono::milliseconds timeout) {
        std::scoped_lock lock{process_mutex_};
        if (process_.get() == nullptr) {
            return true;
        }
        const auto bounded =
            std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<DWORD>::max());
        const auto result = ::WaitForSingleObject(process_.get(), static_cast<DWORD>(bounded));
        if (result != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code{};
        if (::GetExitCodeProcess(process_.get(), &code) != FALSE) {
            exit_code_ = code;
        }
        process_.reset();
        return true;
    }

    void terminate() noexcept {
        std::scoped_lock lock{process_mutex_};
        if (process_.get() == nullptr) {
            return;
        }
        static_cast<void>(::TerminateProcess(process_.get(), 0xC000013AL));
        const auto result =
            ::WaitForSingleObject(process_.get(), static_cast<DWORD>(forced_exit_wait.count()));
        if (result == WAIT_OBJECT_0) {
            DWORD code{};
            if (::GetExitCodeProcess(process_.get(), &code) != FALSE) {
                exit_code_ = code;
            }
            process_.reset();
        }
    }

    void close_pipes() noexcept {
        stdin_write_.reset();
        stdout_read_.reset();
    }

    [[nodiscard]] unsigned long pid() const noexcept { return pid_; }

  private:
    ChildProcess(UniqueHandle process, UniqueHandle stdin_write, UniqueHandle stdout_read,
                 unsigned long pid)
        : process_{std::move(process)}, stdin_write_{std::move(stdin_write)},
          stdout_read_{std::move(stdout_read)}, pid_{pid} {}

    std::mutex process_mutex_;
    UniqueHandle process_;
    UniqueHandle stdin_write_;
    UniqueHandle stdout_read_;
    unsigned long pid_{};
    std::optional<unsigned long> exit_code_;
};

#else

[[nodiscard]] std::filesystem::path current_executable() {
    std::vector<char> buffer(1024);
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     system_message("readlink(/proc/self/exe) failed", errno)};
        }
        const auto size = static_cast<std::size_t>(length);
        if (size < buffer.size()) {
            return std::filesystem::path{std::string{buffer.data(), size}};
        }
        if (buffer.size() >= std::size_t{1} << 20U) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     "Current executable path is too long"};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

class UniqueFd final {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_{fd} {}
    UniqueFd(const UniqueFd&) = delete;
    auto operator=(const UniqueFd&) -> UniqueFd& = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }
        fd_ = fd;
    }

  private:
    int fd_{-1};
};

class ScopedSigpipeBlock final {
  public:
    ScopedSigpipeBlock() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        active_ = ::pthread_sigmask(SIG_BLOCK, &set_, &old_set_) == 0;
        if (active_) {
            pending_before_ = is_pending();
        }
    }
    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    auto operator=(const ScopedSigpipeBlock&) -> ScopedSigpipeBlock& = delete;
    ~ScopedSigpipeBlock() {
        if (!active_) {
            return;
        }
        if (!pending_before_ && is_pending()) {
            timespec timeout{};
            static_cast<void>(::sigtimedwait(&set_, nullptr, &timeout));
        }
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &old_set_, nullptr));
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

  private:
    [[nodiscard]] bool is_pending() const noexcept {
        sigset_t pending{};
        return ::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1;
    }

    sigset_t set_{};
    sigset_t old_set_{};
    bool active_{};
    bool pending_before_{};
};

class ChildProcess final {
  public:
    static std::shared_ptr<ChildProcess> launch(const std::filesystem::path& executable,
                                                const std::string& dxc_runtime_directory) {
        int stdin_fds[2]{};
        if (::pipe2(stdin_fds, O_CLOEXEC) != 0) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     system_message("pipe2 for stdin failed", errno)};
        }
        UniqueFd child_stdin{stdin_fds[0]};
        UniqueFd parent_stdin{stdin_fds[1]};
        int stdout_fds[2]{};
        if (::pipe2(stdout_fds, O_CLOEXEC) != 0) {
            throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                     system_message("pipe2 for stdout failed", errno)};
        }
        UniqueFd parent_stdout{stdout_fds[0]};
        UniqueFd child_stdout{stdout_fds[1]};

        posix_spawn_file_actions_t actions{};
        auto action_error = ::posix_spawn_file_actions_init(&actions);
        if (action_error != 0) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                system_message("posix_spawn_file_actions_init failed", action_error)};
        }
        struct ActionsGuard final {
            posix_spawn_file_actions_t* value;
            ~ActionsGuard() { static_cast<void>(::posix_spawn_file_actions_destroy(value)); }
        } actions_guard{&actions};
        const auto add_action = [&](int result, std::string_view name) {
            if (result != 0) {
                throw WorkerProcessError{WorkerProcessErrorCode::launch_failed,
                                         system_message(name, result)};
            }
        };
        add_action(::posix_spawn_file_actions_adddup2(&actions, child_stdin.get(), STDIN_FILENO),
                   "posix_spawn stdin dup2 failed");
        add_action(::posix_spawn_file_actions_adddup2(&actions, child_stdout.get(), STDOUT_FILENO),
                   "posix_spawn stdout dup2 failed");
        add_action(::posix_spawn_file_actions_addclose(&actions, parent_stdin.get()),
                   "posix_spawn stdin close failed");
        add_action(::posix_spawn_file_actions_addclose(&actions, parent_stdout.get()),
                   "posix_spawn stdout close failed");
        if (child_stdin.get() != STDIN_FILENO) {
            add_action(::posix_spawn_file_actions_addclose(&actions, child_stdin.get()),
                       "posix_spawn child stdin close failed");
        }
        if (child_stdout.get() != STDOUT_FILENO) {
            add_action(::posix_spawn_file_actions_addclose(&actions, child_stdout.get()),
                       "posix_spawn child stdout close failed");
        }

        std::vector<std::string> arguments{executable.string()};
        if (!dxc_runtime_directory.empty()) {
            arguments.emplace_back("--dxc-runtime");
            arguments.push_back(dxc_runtime_directory);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        pid_t pid{};
        const auto spawn_error =
            ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
        if (spawn_error != 0) {
            throw WorkerProcessError{
                WorkerProcessErrorCode::launch_failed,
                system_message("posix_spawn failed for " + executable.string(), spawn_error)};
        }
        child_stdin.reset();
        child_stdout.reset();
        return std::shared_ptr<ChildProcess>{
            new ChildProcess{pid, std::move(parent_stdin), std::move(parent_stdout)}};
    }

    ChildProcess(const ChildProcess&) = delete;
    auto operator=(const ChildProcess&) -> ChildProcess& = delete;
    ~ChildProcess() {
        terminate();
        close_pipes();
    }

    void write_all(std::string_view data) {
        ScopedSigpipeBlock sigpipe;
        if (!sigpipe.active()) {
            throw PipeFailure{false, "Unable to block SIGPIPE for analysis worker write"};
        }
        std::size_t offset{};
        while (offset < data.size()) {
            const auto result =
                ::write(stdin_write_.get(), data.data() + offset, data.size() - offset);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw PipeFailure{errno == EPIPE,
                                  system_message("Writing to analysis worker failed", errno)};
            }
            if (result == 0) {
                throw PipeFailure{false, "Writing to analysis worker made no progress"};
            }
            offset += static_cast<std::size_t>(result);
        }
    }

    void read_all(char* data, std::size_t size) {
        std::size_t offset{};
        while (offset < size) {
            const auto result = ::read(stdout_read_.get(), data + offset, size - offset);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw PipeFailure{false,
                                  system_message("Reading from analysis worker failed", errno)};
            }
            if (result == 0) {
                throw PipeFailure{true, "Analysis worker stdout reached EOF"};
            }
            offset += static_cast<std::size_t>(result);
        }
    }

    [[nodiscard]] std::optional<int> poll_exit() {
        std::scoped_lock lock{process_mutex_};
        poll_exit_locked();
        return exit_status_;
    }

    [[nodiscard]] bool wait_for_exit(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            {
                std::scoped_lock lock{process_mutex_};
                poll_exit_locked();
                if (pid_ <= 0) {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(2ms);
        }
    }

    void terminate() noexcept {
        pid_t pid{};
        {
            std::scoped_lock lock{process_mutex_};
            poll_exit_locked();
            pid = pid_;
            if (pid <= 0) {
                return;
            }
            static_cast<void>(::kill(pid, SIGKILL));
        }
        static_cast<void>(wait_for_exit(forced_exit_wait));
    }

    void close_pipes() noexcept {
        stdin_write_.reset();
        stdout_read_.reset();
    }

    [[nodiscard]] long pid() const noexcept { return static_cast<long>(process_id_); }

  private:
    ChildProcess(pid_t pid, UniqueFd stdin_write, UniqueFd stdout_read)
        : pid_{pid}, process_id_{pid}, stdin_write_{std::move(stdin_write)},
          stdout_read_{std::move(stdout_read)} {}

    void poll_exit_locked() noexcept {
        if (pid_ <= 0) {
            return;
        }
        int status{};
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            exit_status_ = status;
            pid_ = -1;
        } else if (result < 0 && errno == ECHILD) {
            pid_ = -1;
        }
    }

    std::mutex process_mutex_;
    pid_t pid_{};
    pid_t process_id_{};
    UniqueFd stdin_write_;
    UniqueFd stdout_read_;
    std::optional<int> exit_status_;
};

#endif

[[nodiscard]] std::filesystem::path default_worker_executable() {
    auto path = current_executable();
#ifdef _WIN32
    path.replace_filename("hlsl-analysis-worker.exe");
#else
    path.replace_filename("hlsl-analysis-worker");
#endif
    return path;
}

[[nodiscard]] std::string frame(std::string_view payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + std::string{payload};
}

[[nodiscard]] bool ascii_equal_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto first = left[index];
        auto second = right[index];
        if (first >= 'A' && first <= 'Z') {
            first = static_cast<char>(first - 'A' + 'a');
        }
        if (second >= 'A' && second <= 'Z') {
            second = static_cast<char>(second - 'A' + 'a');
        }
        if (first != second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string read_frame(ChildProcess& child) {
    std::string headers;
    while (!headers.ends_with("\r\n\r\n")) {
        if (headers.size() >= max_header_size) {
            throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                     "Analysis worker reply header is too large"};
        }
        char character{};
        child.read_all(&character, 1);
        headers.push_back(character);
    }

    std::optional<std::size_t> content_length;
    std::size_t offset{};
    while (offset + 2U < headers.size()) {
        const auto end = headers.find("\r\n", offset);
        if (end == std::string::npos) {
            throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                     "Analysis worker reply has malformed headers"};
        }
        if (end == offset) {
            break;
        }
        const std::string_view line{headers.data() + offset, end - offset};
        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                     "Analysis worker reply header is missing ':'"};
        }
        auto name = line.substr(0, separator);
        auto value = line.substr(separator + 1U);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        if (ascii_equal_case_insensitive(name, "Content-Length")) {
            if (content_length) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker repeated Content-Length"};
            }
            std::size_t parsed{};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || result.ec != std::errc{} ||
                result.ptr != value.data() + value.size()) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker sent an invalid Content-Length"};
            }
            if (parsed > analysis_worker_max_payload_size) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker reply exceeds the payload limit"};
            }
            content_length = parsed;
        }
        offset = end + 2U;
    }
    if (!content_length) {
        throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                 "Analysis worker reply is missing Content-Length"};
    }
    std::string payload(*content_length, '\0');
    if (!payload.empty()) {
        child.read_all(payload.data(), payload.size());
    }
    return payload;
}

[[nodiscard]] std::string exit_message(const std::shared_ptr<ChildProcess>& child,
                                       std::string_view prefix) {
    const auto exit = child->poll_exit();
    if (!exit) {
        return std::string{prefix};
    }
#ifdef _WIN32
    return std::string{prefix} + " (process " + std::to_string(child->pid()) + ", exit code " +
           std::to_string(*exit) + ')';
#else
    std::string detail;
    if (WIFEXITED(*exit)) {
        detail = "exit code " + std::to_string(WEXITSTATUS(*exit));
    } else if (WIFSIGNALED(*exit)) {
        detail = "signal " + std::to_string(WTERMSIG(*exit));
    } else {
        detail = "status " + std::to_string(*exit);
    }
    return std::string{prefix} + " (process " + std::to_string(child->pid()) + ", " + detail + ')';
#endif
}

enum class AbortReason : std::uint8_t { none, timeout, cancellation };

class RequestAbort final {
  public:
    explicit RequestAbort(std::shared_ptr<ChildProcess> child) : child_{std::move(child)} {}

    void abort(AbortReason reason) noexcept {
        {
            std::scoped_lock lock{mutex_};
            if (finished_ || reason_ != AbortReason::none) {
                return;
            }
            reason_ = reason;
        }
        child_->terminate();
        condition_.notify_all();
    }

    [[nodiscard]] AbortReason finish() noexcept {
        std::scoped_lock lock{mutex_};
        finished_ = true;
        condition_.notify_all();
        return reason_;
    }

    void wait_until(std::chrono::steady_clock::time_point deadline) noexcept {
        std::unique_lock lock{mutex_};
        if (condition_.wait_until(lock, deadline, [this] { return finished_; })) {
            return;
        }
        lock.unlock();
        abort(AbortReason::timeout);
    }

  private:
    std::shared_ptr<ChildProcess> child_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    AbortReason reason_{AbortReason::none};
    bool finished_{};
};

[[noreturn]] void throw_abort(AbortReason reason, std::chrono::milliseconds timeout) {
    if (reason == AbortReason::cancellation) {
        throw WorkerProcessError{WorkerProcessErrorCode::cancelled,
                                 "Analysis worker request was cancelled"};
    }
    throw WorkerProcessError{WorkerProcessErrorCode::timed_out,
                             "Analysis worker request timed out after " +
                                 std::to_string(timeout.count()) + " ms"};
}

} // namespace

WorkerProcessError::WorkerProcessError(WorkerProcessErrorCode code, std::string message)
    : std::runtime_error{std::move(message)}, code_{code} {}

WorkerProcessErrorCode WorkerProcessError::code() const noexcept { return code_; }

class WorkerProcess::Impl final {
  public:
    explicit Impl(WorkerProcessOptions options) : options_{std::move(options)} {
        if (options_.executable.empty()) {
            options_.executable = default_worker_executable();
        }
        if (options_.shutdown_timeout < 0ms) {
            options_.shutdown_timeout = 0ms;
        }
    }

    [[nodiscard]] Json request(std::string_view method, Json params,
                               std::chrono::milliseconds timeout,
                               const json_rpc::CancellationToken& cancellation) {
        if (method.empty()) {
            throw std::invalid_argument{"Analysis worker method must not be empty"};
        }
        if (timeout <= 0ms) {
            throw WorkerProcessError{WorkerProcessErrorCode::timed_out,
                                     "Analysis worker request timed out before it started"};
        }
        if (cancellation.is_cancellation_requested()) {
            throw WorkerProcessError{WorkerProcessErrorCode::cancelled,
                                     "Analysis worker request was cancelled"};
        }
        if (stopping_.load(std::memory_order_acquire)) {
            throw WorkerProcessError{WorkerProcessErrorCode::worker_exited,
                                     "Analysis worker process is shutting down"};
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock request_lock{request_mutex_, std::defer_lock};
        while (!request_lock.try_lock_until(
            std::min(deadline, std::chrono::steady_clock::now() + 5ms))) {
            if (stopping_.load(std::memory_order_acquire)) {
                throw WorkerProcessError{WorkerProcessErrorCode::worker_exited,
                                         "Analysis worker process is shutting down"};
            }
            if (cancellation.is_cancellation_requested()) {
                throw WorkerProcessError{WorkerProcessErrorCode::cancelled,
                                         "Analysis worker request was cancelled"};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw WorkerProcessError{WorkerProcessErrorCode::timed_out,
                                         "Analysis worker request timed out after " +
                                             std::to_string(timeout.count()) + " ms"};
            }
        }
        if (cancellation.is_cancellation_requested()) {
            throw WorkerProcessError{WorkerProcessErrorCode::cancelled,
                                     "Analysis worker request was cancelled"};
        }
        if (stopping_.load(std::memory_order_acquire)) {
            throw WorkerProcessError{WorkerProcessErrorCode::worker_exited,
                                     "Analysis worker process is shutting down"};
        }
        const auto child = ensure_child();
        if (std::chrono::steady_clock::now() >= deadline) {
            throw WorkerProcessError{WorkerProcessErrorCode::timed_out,
                                     "Analysis worker request timed out after " +
                                         std::to_string(timeout.count()) + " ms"};
        }
        const auto id = next_id_++;
        return transact(child, id, method, std::move(params), timeout, deadline, cancellation);
    }

    void shutdown() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::unique_lock request_lock{request_mutex_, std::try_to_lock};
        if (!request_lock.owns_lock()) {
            std::shared_ptr<ChildProcess> active_child;
            {
                std::scoped_lock lock{child_mutex_};
                active_child = std::exchange(child_, nullptr);
            }
            if (active_child) {
                active_child->terminate();
            }
            return;
        }

        std::shared_ptr<ChildProcess> child;
        {
            std::scoped_lock lock{child_mutex_};
            child = std::exchange(child_, nullptr);
        }
        if (!child) {
            return;
        }

        const auto timeout = options_.shutdown_timeout;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        try {
            json_rpc::CancellationToken cancellation;
            static_cast<void>(transact(child, next_id_++, "shutdown", Json::object(), timeout,
                                       deadline, cancellation, false));
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(deadline - std::chrono::steady_clock::now(),
                         std::chrono::steady_clock::duration::zero()));
            if (!child->wait_for_exit(remaining)) {
                child->terminate();
            }
        } catch (const std::exception&) {
            child->terminate();
        }
        child->close_pipes();
    }

    void reset() noexcept {
        std::shared_ptr<ChildProcess> child;
        {
            std::scoped_lock lock{child_mutex_};
            child = std::exchange(child_, nullptr);
        }
        if (child) {
            child->terminate();
            child->close_pipes();
        }
    }

  private:
    [[nodiscard]] std::shared_ptr<ChildProcess> ensure_child() {
        std::scoped_lock lock{child_mutex_};
        if (stopping_.load(std::memory_order_acquire)) {
            throw WorkerProcessError{WorkerProcessErrorCode::worker_exited,
                                     "Analysis worker process is shutting down"};
        }
        if (!child_) {
            child_ = ChildProcess::launch(options_.executable, options_.dxc_runtime_directory);
        }
        return child_;
    }

    void discard(const std::shared_ptr<ChildProcess>& child) noexcept {
        {
            std::scoped_lock lock{child_mutex_};
            if (child_ == child) {
                child_.reset();
            }
        }
        child->terminate();
        child->close_pipes();
    }

    [[nodiscard]] Json transact(const std::shared_ptr<ChildProcess>& child, std::uint64_t id,
                                std::string_view method, Json params,
                                std::chrono::milliseconds timeout,
                                std::chrono::steady_clock::time_point deadline,
                                const json_rpc::CancellationToken& cancellation,
                                bool discard_on_failure = true) {
        const auto abort = std::make_shared<RequestAbort>(child);
        cancellation.on_cancel([weak_abort = std::weak_ptr<RequestAbort>{abort}] {
            if (const auto operation = weak_abort.lock()) {
                operation->abort(AbortReason::cancellation);
            }
        });
        std::jthread timer{[abort, deadline] { abort->wait_until(deadline); }};

        try {
            const auto payload = Json{
                {"protocol", analysis_worker_protocol_version},
                {"id", id},
                {"method", method},
                {"params",
                 std::move(params)}}.dump();
            child->write_all(frame(payload));
            const auto reply_payload = read_frame(*child);
            const auto abort_reason = abort->finish();
            if (abort_reason != AbortReason::none) {
                if (discard_on_failure) {
                    discard(child);
                }
                throw_abort(abort_reason, timeout);
            }

            Json reply;
            try {
                reply = Json::parse(reply_payload);
            } catch (const Json::exception& error) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker returned malformed JSON: " +
                                             std::string{error.what()}};
            }
            if (!reply.is_object()) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker reply must be a JSON object"};
            }
            const auto protocol = reply.find("protocol");
            const auto reply_id = reply.find("id");
            if (protocol == reply.end() || !protocol->is_number_unsigned() ||
                protocol->get<unsigned>() != analysis_worker_protocol_version ||
                reply_id == reply.end() || !reply_id->is_number_unsigned() ||
                reply_id->get<std::uint64_t>() != id) {
                throw WorkerProcessError{
                    WorkerProcessErrorCode::mismatched_reply,
                    "Analysis worker reply protocol version or request id did not match"};
            }
            if (const auto error = reply.find("error"); error != reply.end()) {
                if (!error->is_object()) {
                    throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                             "Analysis worker error reply is malformed"};
                }
                const auto message = error->find("message");
                if (message == error->end() || !message->is_string()) {
                    throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                             "Analysis worker error reply has no message"};
                }
                throw WorkerProcessError{WorkerProcessErrorCode::worker_error,
                                         message->get<std::string>()};
            }
            const auto result = reply.find("result");
            if (result == reply.end()) {
                throw WorkerProcessError{WorkerProcessErrorCode::malformed_reply,
                                         "Analysis worker reply has neither result nor error"};
            }
            return *result;
        } catch (const PipeFailure& error) {
            const auto abort_reason = abort->finish();
            bool exited = child->poll_exit().has_value();
            if (error.eof() && !exited) {
                exited = child->wait_for_exit(10ms);
            }
            const auto message =
                exited ? exit_message(child, "Analysis worker closed its protocol stream")
                       : std::string{"Analysis worker closed its protocol stream"};
            if (discard_on_failure) {
                discard(child);
            }
            if (abort_reason != AbortReason::none) {
                throw_abort(abort_reason, timeout);
            }
            if (error.eof()) {
                throw WorkerProcessError{exited ? WorkerProcessErrorCode::worker_exited
                                                : WorkerProcessErrorCode::unexpected_eof,
                                         message};
            }
            throw WorkerProcessError{WorkerProcessErrorCode::io_error, error.what()};
        } catch (const WorkerProcessError& error) {
            const auto abort_reason = abort->finish();
            if (discard_on_failure && error.code() != WorkerProcessErrorCode::worker_error) {
                discard(child);
            }
            if (abort_reason != AbortReason::none) {
                throw_abort(abort_reason, timeout);
            }
            throw;
        } catch (...) {
            const auto abort_reason = abort->finish();
            if (discard_on_failure) {
                discard(child);
            }
            if (abort_reason != AbortReason::none) {
                throw_abort(abort_reason, timeout);
            }
            throw;
        }
    }

    WorkerProcessOptions options_;
    std::timed_mutex request_mutex_;
    std::mutex child_mutex_;
    std::shared_ptr<ChildProcess> child_;
    std::uint64_t next_id_{1};
    std::atomic_bool stopping_{};
};

WorkerProcess::WorkerProcess(WorkerProcessOptions options)
    : impl_{std::make_shared<Impl>(std::move(options))} {}

WorkerProcess::~WorkerProcess() { shutdown(); }

Json WorkerProcess::request(std::string_view method, Json params, std::chrono::milliseconds timeout,
                            const json_rpc::CancellationToken& cancellation) {
    const auto implementation = impl_;
    return implementation->request(method, std::move(params), timeout, cancellation);
}

void WorkerProcess::reset() noexcept {
    const auto implementation = impl_;
    if (implementation) {
        implementation->reset();
    }
}

void WorkerProcess::shutdown() noexcept {
    const auto implementation = impl_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace hlsl_intellisense::analysis
