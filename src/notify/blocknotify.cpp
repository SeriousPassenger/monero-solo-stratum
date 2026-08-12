#include "monero_solo/blocknotify.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace monero_solo {
namespace {

constexpr std::size_t kStderrLimit = 4096U;
constexpr auto kKillGrace = std::chrono::seconds(5);

bool valid_tx_hash(std::string_view value) noexcept {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

int open_executable(const std::string &path) noexcept {
    const int descriptor = open(
        path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return -1;
    struct stat opened{};
    if (fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        (opened.st_mode & 0111) == 0) {
        (void)close(descriptor);
        return -1;
    }
    const int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

void append_with_limit(std::string &target, const char *data, std::size_t size) {
    for (std::size_t index = 0; index < size && target.size() < kStderrLimit;
         ++index) {
        const auto byte = static_cast<unsigned char>(data[index]);
        if (byte == '\n' || byte == '\r' || byte == '\t' ||
            (byte >= 0x20U && byte <= 0x7eU)) {
            target.push_back(static_cast<char>(byte));
        }
        else {
            // Stderr is untrusted storage/API text. Mapping arbitrary control
            // and non-UTF-8 bytes to ASCII keeps it valid, bounded, and NUL-free.
            target.push_back('?');
        }
    }
}

void close_fd(int &descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

} // namespace

BlockNotifyCommand BlockNotifyCommand::parse(std::string_view input,
                                             bool validate_executable) {
    BlockNotifyCommand command;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    bool argument_started = false;

    auto finish = [&] {
        if (argument_started) {
            command.arguments.push_back(current);
            current.clear();
            argument_started = false;
        }
    };

    for (const char character : input) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (escaped) {
            if (in_double && character != '"' && character != '\\') {
                throw std::invalid_argument("inside double quotes, backslash may escape only quote or backslash");
            }
            current.push_back(character);
            argument_started = true;
            escaped = false;
            continue;
        }
        if (in_single) {
            if (character == '\'') {
                in_single = false;
            }
            else {
                current.push_back(character);
            }
            argument_started = true;
            continue;
        }
        if (in_double) {
            if (character == '"') {
                in_double = false;
                argument_started = true;
            }
            else if (character == '\\') {
                escaped = true;
            }
            else {
                current.push_back(character);
                argument_started = true;
            }
            continue;
        }
        if (character == '\'') {
            in_single = true;
            argument_started = true;
        }
        else if (character == '"') {
            in_double = true;
            argument_started = true;
        }
        else if (character == '\\') {
            escaped = true;
            argument_started = true;
        }
        else if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
                 byte == '\f' || byte == '\v') {
            finish();
        }
        else {
            current.push_back(character);
            argument_started = true;
        }
    }
    if (escaped || in_single || in_double) {
        throw std::invalid_argument("unterminated blocknotify escape or quote");
    }
    finish();
    if (command.arguments.empty() || command.arguments.front().empty()) {
        throw std::invalid_argument("blocknotify command has empty argv");
    }
    bool placeholder = false;
    for (const auto &argument : command.arguments) {
        if (argument.find("%s") != std::string::npos) {
            placeholder = true;
        }
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("blocknotify argument contains NUL");
        }
    }
    if (!placeholder) {
        throw std::invalid_argument("blocknotify command must contain %s");
    }
    if (command.arguments.front().front() != '/') {
        throw std::invalid_argument("blocknotify executable must be absolute");
    }
    if (validate_executable) {
        const int descriptor = open_executable(command.arguments.front());
        if (descriptor < 0) {
            throw std::invalid_argument("blocknotify executable is not a regular executable");
        }
        (void)close(descriptor);
    }
    return command;
}

std::vector<std::string> BlockNotifyCommand::instantiate(
    std::string_view miner_tx_hash) const {
    if (!valid_tx_hash(miner_tx_hash)) {
        throw std::invalid_argument("miner transaction hash must be lowercase 64-hex");
    }
    std::vector<std::string> result = arguments;
    for (auto &argument : result) {
        std::size_t position = 0;
        while ((position = argument.find("%s", position)) != std::string::npos) {
            argument.replace(position, 2U, miner_tx_hash);
            position += miner_tx_hash.size();
        }
    }
    return result;
}

BlockNotifySupervisor::BlockNotifySupervisor(BlockNotifyCommand command,
                                             Claim claim, Complete complete)
    : command_(std::move(command)), claim_(std::move(claim)),
      complete_(std::move(complete)) {
    if (command_.arguments.empty() || !claim_ || !complete_) {
        throw std::invalid_argument("invalid blocknotify supervisor callbacks");
    }
}

BlockNotifySupervisor::~BlockNotifySupervisor() { stop(); }

bool BlockNotifySupervisor::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void BlockNotifySupervisor::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    thread_ = std::jthread([this](std::stop_token token) { loop(token); });
}

void BlockNotifySupervisor::wake() noexcept {
    wake_requested_.store(true, std::memory_order_release);
}

void BlockNotifySupervisor::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    wake();
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

std::chrono::seconds BlockNotifySupervisor::retry_delay(std::uint32_t attempts) {
    static constexpr std::array<unsigned, 6> delays{1U, 5U, 30U, 120U, 600U, 3600U};
    const std::size_t index = attempts == 0U
                                  ? 0U
                                  : std::min<std::size_t>(attempts - 1U, delays.size() - 1U);
    return std::chrono::seconds(delays[index]);
}

void BlockNotifySupervisor::loop(std::stop_token token) noexcept {
    while (running() && !token.stop_requested()) {
        std::optional<BlockNotifyDelivery> delivery;
        try {
            delivery = claim_();
        }
        catch (...) {
        }
        if (delivery.has_value()) {
            BlockNotifyResult result;
            try {
                result = execute(command_, delivery->miner_tx_hash);
            }
            catch (const std::exception &error) {
                result.error = error.what();
            }
            catch (...) {
                result.error = "blocknotify execution raised an unknown exception";
            }

            // Never silently abandon a row after its durable claim changed it
            // to running. Retry transient completion failures in-process; a
            // shutdown during a persistent database failure is recovered by
            // the normal startup running->pending transition.
            while (running() && !token.stop_requested()) {
                try {
                    complete_(*delivery, result,
                              retry_delay(delivery->attempt_count));
                    delivery.reset();
                    break;
                }
                catch (...) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if (!delivery.has_value()) continue;
        }
        for (unsigned tick = 0; tick < 10U && running() && !token.stop_requested(); ++tick) {
            if (wake_requested_.exchange(false, std::memory_order_acq_rel)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

BlockNotifyResult BlockNotifySupervisor::execute(
    const BlockNotifyCommand &command,
    std::string_view miner_tx_hash,
    std::chrono::seconds timeout) {
    BlockNotifyResult result;
    const auto arguments = command.instantiate(miner_tx_hash);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    std::array<char *, 3> environment{
        const_cast<char *>("LANG=C"), const_cast<char *>("TZ=UTC"), nullptr};
    int executable = open_executable(arguments.front());
    if (executable < 0) {
        result.error = "blocknotify executable failed launch-time validation";
        return result;
    }

    int error_pipe[2]{-1, -1};
    if (pipe2(error_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        close_fd(executable);
        result.error = "could not create blocknotify stderr pipe";
        return result;
    }
    const pid_t child = fork();
    if (child < 0) {
        close_fd(executable);
        close_fd(error_pipe[0]);
        close_fd(error_pipe[1]);
        result.error = "could not fork blocknotify process";
        return result;
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        const int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        const int null_output = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_input < 0 || null_output < 0 ||
            fcntl(executable, F_SETFD, 0) != 0 ||
            dup2(null_input, STDIN_FILENO) < 0 ||
            dup2(null_output, STDOUT_FILENO) < 0 ||
            dup2(error_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close_fd(error_pipe[0]);
        close_fd(error_pipe[1]);
        // Execute the exact O_NOFOLLOW-opened inode. Clearing FD_CLOEXEC is
        // required for shebang interpreters that reopen the script via /dev/fd.
        fexecve(executable, argv.data(), environment.data());
        _exit(127);
    }

    // Close the race between fork and the child's setpgid. EACCES means the
    // child already execed after successfully establishing its own group.
    (void)setpgid(child, child);
    close_fd(executable);
    close_fd(error_pipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool exited = false;
    int status = 0;
    std::array<char, 1024> buffer{};
    while (!exited && std::chrono::steady_clock::now() < deadline) {
        // Always drain the pipe, including bytes beyond the stored 4 KiB.
        // Otherwise a child writing a large stderr stream can block forever
        // once the pipe fills.
        for (;;) {
            const ssize_t count = read(error_pipe[0], buffer.data(), buffer.size());
            if (count > 0) {
                append_with_limit(result.stderr_excerpt, buffer.data(),
                                  static_cast<std::size_t>(count));
                continue;
            }
            break;
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            result.error = "waitpid failed for blocknotify process";
            break;
        }
        pollfd wait{error_pipe[0], POLLIN, 0};
        (void)poll(&wait, 1, 50);
    }
    if (!exited) {
        result.timed_out = true;
        if (kill(-child, SIGTERM) != 0 && errno == ESRCH) {
            (void)kill(child, SIGTERM);
        }
        const auto kill_deadline = std::chrono::steady_clock::now() + kKillGrace;
        while (std::chrono::steady_clock::now() < kill_deadline) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // The leader may exit on TERM while a descendant ignores it. Always
        // follow the grace period with a group-wide KILL; ESRCH is the normal
        // result when the group is already empty.
        (void)kill(-child, SIGKILL);
        if (!exited) {
            (void)kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    // Hooks are supervised as one process group. Even a nominally successful
    // leader may have daemonized descendants; do not let those escape the
    // configured lifetime or retain inherited resources.
    (void)kill(-child, SIGKILL);
    for (;;) {
        const ssize_t count = read(error_pipe[0], buffer.data(), buffer.size());
        if (count > 0) {
            append_with_limit(result.stderr_excerpt, buffer.data(),
                              static_cast<std::size_t>(count));
            continue;
        }
        break;
    }
    close_fd(error_pipe[0]);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.delivered = !result.timed_out && *result.exit_code == 0;
    }
    else if (WIFSIGNALED(status)) {
        result.term_signal = WTERMSIG(status);
    }
    if (!result.delivered && result.error.empty()) {
        result.error = result.timed_out ? "blocknotify timed out"
                                        : "blocknotify exited unsuccessfully";
    }
    return result;
}

} // namespace monero_solo
