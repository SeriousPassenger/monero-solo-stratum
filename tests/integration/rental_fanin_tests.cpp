#include "monero_solo/stratum.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kClientCount = 200U;
constexpr std::size_t kSubmitCount = 16U;
constexpr std::size_t kSubmitWorkers = 4U;
constexpr std::uint64_t kInitialHeight = 4'000'000U;
constexpr std::uint64_t kRefreshedHeight = kInitialHeight + 1U;

[[noreturn]] void fail(std::string message)
{
    throw std::runtime_error(std::move(message));
}

void require(const bool condition, const std::string_view message)
{
    if (!condition) fail(std::string(message));
}

class FileDescriptor final {
public:
    explicit FileDescriptor(const int descriptor = -1) noexcept
        : descriptor_(descriptor) {}

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    ~FileDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    void reset(const int replacement = -1) noexcept
    {
        if (descriptor_ >= 0) (void)::close(descriptor_);
        descriptor_ = replacement;
    }

private:
    int descriptor_;
};

struct Client {
    FileDescriptor socket;
    std::string input;
    std::string source_address;
    std::string connection_id;
    std::string job_id;
};

std::uint16_t unused_tcp_port()
{
    FileDescriptor probe(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(probe.get() >= 0, "could not create TCP port probe");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(probe.get(), reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0,
            "could not bind TCP port probe");
    socklen_t length = sizeof(address);
    require(::getsockname(probe.get(), reinterpret_cast<sockaddr *>(&address),
                          &length) == 0,
            "could not inspect TCP port probe");
    return ntohs(address.sin_port);
}

bool source_address_available(const std::string_view encoded)
{
    FileDescriptor probe(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (probe.get() < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (::inet_pton(AF_INET, std::string(encoded).c_str(), &address.sin_addr) != 1) {
        return false;
    }
    return ::bind(probe.get(), reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0;
}

Client connect_from(const std::uint16_t port, const std::string &source)
{
    FileDescriptor socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(socket.get() >= 0, "could not create rental client socket");

    sockaddr_in local{};
    local.sin_family = AF_INET;
    require(::inet_pton(AF_INET, source.c_str(), &local.sin_addr) == 1,
            "could not encode rental source address");
    if (::bind(socket.get(), reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        fail("could not bind rental source address " + source + ": " +
             std::strerror(errno));
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&remote),
                  sizeof(remote)) != 0) {
        fail("could not connect rental client: " + std::string(std::strerror(errno)));
    }

    sockaddr_in actual{};
    socklen_t actual_length = sizeof(actual);
    require(::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&actual),
                          &actual_length) == 0,
            "could not inspect rental client source address");
    char actual_text[INET_ADDRSTRLEN]{};
    require(::inet_ntop(AF_INET, &actual.sin_addr, actual_text,
                        sizeof(actual_text)) != nullptr,
            "could not render rental client source address");
    require(source == actual_text, "kernel did not preserve requested source address");

    const int flags = ::fcntl(socket.get(), F_GETFL, 0);
    require(flags >= 0 && ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) == 0,
            "could not make rental client nonblocking");
    return Client{std::move(socket), {}, actual_text, {}, {}};
}

int remaining_poll_ms(const Clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (remaining <= 0ms) return 0;
    return static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
}

void send_until(const int descriptor, const std::string_view bytes,
                const Clock::time_point deadline)
{
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(descriptor, bytes.data() + offset,
                                    bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd writable{descriptor, POLLOUT, 0};
            const int timeout = remaining_poll_ms(deadline);
            require(timeout > 0 && ::poll(&writable, 1, timeout) > 0,
                    "timed out writing rental client request");
            continue;
        }
        fail("rental client write failed: " + std::string(std::strerror(errno)));
    }
}

std::optional<std::string> take_line(std::string &input)
{
    const auto newline = input.find('\n');
    if (newline == std::string::npos) return std::nullopt;
    std::string line = input.substr(0U, newline);
    input.erase(0U, newline + 1U);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::vector<std::string> read_one_line_each(
    std::vector<Client> &clients, const std::vector<std::size_t> &indices,
    const std::chrono::milliseconds timeout, const std::string_view phase)
{
    const auto deadline = Clock::now() + timeout;
    std::vector<std::string> result(indices.size());
    std::vector<bool> pending(indices.size(), true);
    std::size_t remaining = indices.size();

    while (remaining != 0U) {
        for (std::size_t slot = 0; slot < indices.size(); ++slot) {
            if (!pending[slot]) continue;
            auto line = take_line(clients.at(indices[slot]).input);
            if (!line) continue;
            result[slot] = std::move(*line);
            pending[slot] = false;
            --remaining;
        }
        if (remaining == 0U) break;

        std::vector<pollfd> descriptors;
        std::vector<std::size_t> slots;
        descriptors.reserve(remaining);
        slots.reserve(remaining);
        for (std::size_t slot = 0; slot < indices.size(); ++slot) {
            if (!pending[slot]) continue;
            descriptors.push_back({clients.at(indices[slot]).socket.get(), POLLIN, 0});
            slots.push_back(slot);
        }

        const int wait_ms = remaining_poll_ms(deadline);
        if (wait_ms == 0) {
            fail(std::string(phase) + " timed out with " +
                 std::to_string(remaining) + " clients pending");
        }
        const int ready = ::poll(descriptors.data(), descriptors.size(), wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) {
            fail(std::string(phase) + " timed out with " +
                 std::to_string(remaining) + " clients pending");
        }
        require(ready > 0, "rental client poll failed");

        for (std::size_t position = 0; position < descriptors.size(); ++position) {
            if (descriptors[position].revents == 0) continue;
            if ((descriptors[position].revents & POLLNVAL) != 0) {
                fail(std::string(phase) + " encountered an invalid client socket");
            }
            Client &client = clients.at(indices.at(slots.at(position)));
            bool peer_closed = false;
            for (;;) {
                char buffer[4096]{};
                const ssize_t received = ::recv(client.socket.get(), buffer,
                                                sizeof(buffer), 0);
                if (received > 0) {
                    client.input.append(buffer, static_cast<std::size_t>(received));
                    require(client.input.size() <= 1024U * 1024U,
                            "rental client response exceeded test bound");
                    continue;
                }
                if (received == 0) {
                    peer_closed = true;
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                fail(std::string(phase) + " client read failed: " +
                     std::strerror(errno));
            }
            if (peer_closed && client.input.find('\n') == std::string::npos) {
                fail(std::string(phase) + " connection closed before a complete response");
            }
        }
    }
    return result;
}

template <class Predicate>
void eventually(Predicate predicate, const std::chrono::milliseconds timeout,
                const std::string_view message)
{
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    fail(std::string(message));
}

std::vector<std::size_t> client_indices(const std::size_t count)
{
    std::vector<std::size_t> indices;
    indices.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) indices.push_back(index);
    return indices;
}

std::string fixed_hex(const std::uint64_t value, const unsigned width)
{
    std::ostringstream encoded;
    encoded << std::hex << std::nouppercase << std::setfill('0')
            << std::setw(static_cast<int>(width)) << value;
    return encoded.str();
}

std::string job_id(const std::uint64_t height)
{
    return fixed_hex(height, 32U);
}

std::optional<std::size_t> process_thread_count()
{
    try {
        std::size_t count = 0U;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
            (void)entry;
            ++count;
        }
        return count;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<std::size_t> resident_kib()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmRSS:")) continue;
        std::istringstream value(line.substr(6U));
        std::size_t kib = 0U;
        std::string units;
        if (value >> kib >> units && units == "kB") return kib;
        return std::nullopt;
    }
    return std::nullopt;
}

struct SubmitGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released{};
    std::size_t active{};
    std::size_t maximum{};
    std::size_t started{};
    bool timed_out{};

    void release() noexcept
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_all();
    }
};

class GateRelease final {
public:
    explicit GateRelease(SubmitGate &gate) noexcept : gate_(gate) {}
    GateRelease(const GateRelease &) = delete;
    GateRelease &operator=(const GateRelease &) = delete;
    ~GateRelease() { gate_.release(); }

private:
    SubmitGate &gate_;
};

struct Diagnostics {
    std::chrono::milliseconds connect{};
    std::chrono::milliseconds login{};
    std::chrono::milliseconds refresh{};
    std::chrono::milliseconds submit{};
    std::chrono::milliseconds total{};
    std::optional<std::size_t> baseline_threads;
    std::optional<std::size_t> server_threads;
    std::optional<std::size_t> loaded_threads;
    std::optional<std::size_t> stopped_threads;
    std::optional<std::size_t> baseline_rss;
    std::optional<std::size_t> loaded_rss;
    bool distributed_sources{};
};

Diagnostics test_rental_fanin()
{
    Diagnostics diagnostics;
    const auto test_started = Clock::now();
    diagnostics.baseline_threads = process_thread_count();
    diagnostics.baseline_rss = resident_kib();

    const std::vector<std::string> desired_sources{
        "127.0.0.1", "127.0.0.2", "127.0.0.3"};
    diagnostics.distributed_sources = std::all_of(
        desired_sources.begin(), desired_sources.end(), source_address_available);
    const std::vector<std::string> sources = diagnostics.distributed_sources
        ? desired_sources
        : std::vector<std::string>{"127.0.0.1"};

    const std::uint16_t port = unused_tcp_port();
    std::atomic<std::uint64_t> current_height{kInitialHeight};
    std::atomic<std::size_t> job_provider_calls{};
    std::atomic<std::size_t> queued_jobs{};
    std::atomic<std::size_t> opened{};
    std::atomic<std::size_t> authenticated{};
    std::atomic<std::size_t> closed{};
    std::atomic<std::size_t> stopped{};
    std::atomic<std::size_t> submits_queued{};
    std::atomic<std::size_t> submits_completed{};
    std::atomic<std::size_t> submit_handler_calls{};
    std::atomic<bool> invalid_submission{};
    std::mutex peer_mutex;
    std::map<std::string, std::size_t> observed_peers;
    SubmitGate submit_gate;

    monero_solo::DefensePolicyConfig defense_config;
    defense_config.connection_rate_per_minute = 1U;
    defense_config.connection_burst = 1U;
    monero_solo::DefenseEngine defense(defense_config);

    monero_solo::StratumServerConfig config;
    config.listen = {"127.0.0.1:" + std::to_string(port)};
    config.access_password = "rental-secret";
    config.max_connections = kClientCount + 20U;
    config.max_connections_per_ip = diagnostics.distributed_sources
        ? 80U
        : kClientCount + 20U;
    config.login_timeout_ms = 30'000U;
    config.idle_timeout_ms = 60'000U;
    config.difficulty_floor = 2U;
    config.submit_workers = kSubmitWorkers;
    config.max_pending_submits = 64U;
    config.candidate_submit_reserve = 8U;
    config.max_pending_submits_per_connection = 2U;

    monero_solo::StratumServer server(
        config,
        [&](const monero_solo::MinerConnection &)
            -> std::optional<monero_solo::StratumJob> {
            ++job_provider_calls;
            const std::uint64_t height = current_height.load(std::memory_order_acquire);
            return monero_solo::StratumJob{
                .blob = std::string(152U, 'a'),
                .job_id = job_id(height),
                .target = "ffffffffffffff7f",
                .seed_hash = std::string(64U, 'c'),
                .height = height,
                .on_queued = [&](std::string_view, std::string_view) {
                    ++queued_jobs;
                },
                .network_difficulty = "1000000000",
            };
        },
        [&](const monero_solo::StratumSubmission &submission) {
            if (submission.request_sequence != 1U ||
                submission.connection.last_sent_height != kRefreshedHeight ||
                submission.latest_queued_height() != kRefreshedHeight ||
                submission.job_id != job_id(kRefreshedHeight) ||
                submission.claimed_hash_hex != std::string(64U, 'f') ||
                !std::holds_alternative<std::string>(submission.request_id)) {
                invalid_submission.store(true, std::memory_order_release);
            }
            ++submit_handler_calls;
            std::unique_lock lock(submit_gate.mutex);
            ++submit_gate.active;
            ++submit_gate.started;
            submit_gate.maximum = std::max(submit_gate.maximum, submit_gate.active);
            submit_gate.condition.notify_all();
            if (!submit_gate.condition.wait_for(lock, 20s, [&] {
                    return submit_gate.released;
                })) {
                submit_gate.timed_out = true;
            }
            --submit_gate.active;
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::accepted, "fanin_ok"};
        },
        &defense,
        [&](const monero_solo::MinerConnection &connection,
            const std::string_view event) {
            if (event == "opened") {
                ++opened;
                std::lock_guard lock(peer_mutex);
                ++observed_peers[connection.peer.text()];
            }
            else if (event == "authenticated") ++authenticated;
            else if (event == "submit_queued") ++submits_queued;
            else if (event == "submit_completed") ++submits_completed;
            else if (event == "server stopping") ++stopped;
            else ++closed;
        });
    server.start();
    GateRelease release_on_exit(submit_gate);
    require(server.running(), "single Stratum server did not start");
    require(server.bound_endpoints().size() == 1U,
            "fan-in regression did not use exactly one listener instance");
    diagnostics.server_threads = process_thread_count();

    std::vector<Client> clients;
    clients.reserve(kClientCount);
    const auto connect_started = Clock::now();
    const auto connect_deadline = connect_started + 20s;
    for (std::size_t index = 0U; index < kClientCount; ++index) {
        require(Clock::now() < connect_deadline,
                "200-client connect phase exceeded deterministic deadline");
        clients.push_back(connect_from(port, sources[index % sources.size()]));
    }
    diagnostics.connect = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - connect_started);
    eventually([&] { return server.connection_count() == kClientCount; }, 10s,
               "single Stratum instance did not retain all 200 connections");

    const auto indices = client_indices(kClientCount);
    const auto login_started = Clock::now();
    const auto login_send_deadline = login_started + 20s;
    for (std::size_t index = 0U; index < clients.size(); ++index) {
        const Json login{
            {"id", static_cast<std::uint64_t>(index + 1U)},
            {"jsonrpc", "2.0"},
            {"method", "login"},
            {"params", {
                {"login", "rental-worker-" + std::to_string(index)},
                {"pass", "rental-secret"},
                {"agent", "rental-fanin-regression"},
                {"rigid", "rig-" + std::to_string(index)},
                {"algo", Json::array({"rx/0"})},
            }},
        };
        send_until(clients[index].socket.get(), login.dump() + "\n",
                   login_send_deadline);
    }
    const auto login_lines = read_one_line_each(clients, indices, 20s, "login fan-in");
    for (std::size_t index = 0U; index < login_lines.size(); ++index) {
        const Json response = Json::parse(login_lines[index]);
        require(response.at("id") == index + 1U && response.at("error").is_null() &&
                    response.at("result").at("status") == "OK",
                "rental login response was not successful");
        const Json &job = response.at("result").at("job");
        require(job.at("height") == kInitialHeight &&
                    job.at("job_id") == job_id(kInitialHeight) &&
                    job.at("seed_hash") == std::string(64U, 'c') &&
                    job.at("algo") == "rx/0",
                "rental login did not issue the initial job");
        clients[index].connection_id =
            response.at("result").at("id").get<std::string>();
        clients[index].job_id = job.at("job_id").get<std::string>();
        require(clients[index].connection_id.size() == 32U,
                "rental login connection ID had the wrong shape");
    }
    diagnostics.login = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - login_started);
    require(opened.load(std::memory_order_acquire) == kClientCount &&
                authenticated.load(std::memory_order_acquire) == kClientCount &&
                server.connection_count() == kClientCount,
            "not every rental connection authenticated and remained live");

    {
        std::lock_guard lock(peer_mutex);
        std::map<std::string, std::size_t> expected;
        for (const Client &client : clients) ++expected[client.source_address];
        require(observed_peers == expected,
                "server did not observe the expected rental source-IP distribution");
        if (diagnostics.distributed_sources) {
            require(observed_peers.size() == 3U,
                    "three-source rental fan-in collapsed onto fewer IPs");
        }
    }

    diagnostics.loaded_threads = process_thread_count();
    diagnostics.loaded_rss = resident_kib();
    if (diagnostics.server_threads && diagnostics.loaded_threads) {
        require(*diagnostics.loaded_threads <= *diagnostics.server_threads + 4U,
                "200 live connections created per-connection server threads");
    }

    const auto refresh_started = Clock::now();
    current_height.store(kRefreshedHeight, std::memory_order_release);
    server.refresh_jobs();
    const auto refresh_lines = read_one_line_each(clients, indices, 20s, "job refresh fan-in");
    for (std::size_t index = 0U; index < refresh_lines.size(); ++index) {
        const Json notification = Json::parse(refresh_lines[index]);
        require(notification.at("method") == "job" &&
                    notification.at("params").at("height") == kRefreshedHeight &&
                    notification.at("params").at("job_id") == job_id(kRefreshedHeight),
                "refreshed job did not reach every rental connection");
        clients[index].job_id =
            notification.at("params").at("job_id").get<std::string>();
    }
    diagnostics.refresh = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - refresh_started);
    require(job_provider_calls.load(std::memory_order_acquire) == kClientCount * 2U &&
                queued_jobs.load(std::memory_order_acquire) == kClientCount * 2U,
            "initial and refreshed jobs were not queued exactly once per connection");

    const auto submit_indices = client_indices(kSubmitCount);
    const auto submit_started_at = Clock::now();
    const auto submit_send_deadline = submit_started_at + 20s;
    for (std::size_t index = 0U; index < kSubmitCount; ++index) {
        const Json submit{
            {"id", "share-" + std::to_string(index)},
            {"jsonrpc", "2.0"},
            {"method", "submit"},
            {"params", {
                {"id", clients[index].connection_id},
                {"job_id", clients[index].job_id},
                {"nonce", fixed_hex(index + 1U, 8U)},
                {"result", std::string(64U, 'f')},
                {"algo", "rx/0"},
            }},
        };
        send_until(clients[index].socket.get(), submit.dump() + "\n",
                   submit_send_deadline);
    }
    {
        std::unique_lock lock(submit_gate.mutex);
        require(submit_gate.condition.wait_for(lock, 10s, [&] {
                    return submit_gate.started >= kSubmitWorkers;
                }),
                "configured submit workers did not concurrently drain rental submits");
        require(submit_gate.active == kSubmitWorkers &&
                    submit_gate.maximum == kSubmitWorkers,
                "ordinary rental submits did not respect the configured worker bound");
        submit_gate.released = true;
    }
    submit_gate.condition.notify_all();
    const auto submit_lines = read_one_line_each(
        clients, submit_indices, 20s, "bounded rental submit fan-in");
    for (std::size_t index = 0U; index < submit_lines.size(); ++index) {
        const Json response = Json::parse(submit_lines[index]);
        require(response.at("id") == "share-" + std::to_string(index) &&
                    response.at("error").is_null() &&
                    response.at("result").at("status") == "OK",
                "bounded rental submit did not complete successfully");
    }
    diagnostics.submit = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - submit_started_at);
    require(submit_handler_calls.load(std::memory_order_acquire) == kSubmitCount &&
                submits_queued.load(std::memory_order_acquire) == kSubmitCount &&
                submits_completed.load(std::memory_order_acquire) == kSubmitCount &&
                !invalid_submission.load(std::memory_order_acquire),
            "bounded rental submissions lost admission metadata or completion");
    {
        std::lock_guard lock(submit_gate.mutex);
        require(!submit_gate.timed_out && submit_gate.active == 0U,
                "rental submit gate timed out or leaked active handlers");
    }

    for (Client &client : clients) client.socket.reset();
    eventually([&] { return server.connection_count() == 0U; }, 15s,
               "server did not cleanly reap all rental connections");
    require(closed.load(std::memory_order_acquire) == kClientCount &&
                stopped.load(std::memory_order_acquire) == 0U,
            "rental clients did not close cleanly before server shutdown");
    server.stop();
    require(!server.running(), "single Stratum server did not stop cleanly");
    diagnostics.stopped_threads = process_thread_count();
    if (diagnostics.baseline_threads && diagnostics.stopped_threads) {
        require(*diagnostics.stopped_threads <= *diagnostics.baseline_threads + 2U,
                "Stratum worker threads remained after clean shutdown");
    }
    diagnostics.total = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - test_started);
    return diagnostics;
}

void print_optional(const std::string_view name,
                    const std::optional<std::size_t> value)
{
    std::cout << ' ' << name << '=';
    if (value) std::cout << *value;
    else std::cout << "unavailable";
}

} // namespace

int main()
{
    try {
        const Diagnostics diagnostics = test_rental_fanin();
        std::cout << "rental_fanin clients=" << kClientCount
                  << " sources=" << (diagnostics.distributed_sources ? 3 : 1)
                  << " connect_ms=" << diagnostics.connect.count()
                  << " login_ms=" << diagnostics.login.count()
                  << " refresh_ms=" << diagnostics.refresh.count()
                  << " submit_ms=" << diagnostics.submit.count()
                  << " total_ms=" << diagnostics.total.count();
        print_optional("threads_baseline", diagnostics.baseline_threads);
        print_optional("threads_server", diagnostics.server_threads);
        print_optional("threads_loaded", diagnostics.loaded_threads);
        print_optional("threads_stopped", diagnostics.stopped_threads);
        print_optional("rss_baseline_kib", diagnostics.baseline_rss);
        print_optional("rss_loaded_kib", diagnostics.loaded_rss);
        if (!diagnostics.distributed_sources) {
            std::cout << " source_binding=fallback_127.0.0.1";
        }
        std::cout << '\n';
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "rental_fanin_tests: " << error.what() << '\n';
        return 1;
    }
}
