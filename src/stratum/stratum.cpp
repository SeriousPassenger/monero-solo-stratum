#include "monero_solo/stratum.hpp"
#include "monero_solo/monero.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace monero_solo {
namespace {

struct OutputChunk {
    std::string data;
    std::size_t offset{};
};

std::string id_key(const MinerRequestId &id) {
    if (std::holds_alternative<std::int64_t>(id)) {
        return "i:" + std::to_string(std::get<std::int64_t>(id));
    }
    return "s:" + std::get<std::string>(id);
}

nlohmann::json id_json(const MinerRequestId &id) {
    if (std::holds_alternative<std::int64_t>(id)) {
        return std::get<std::int64_t>(id);
    }
    return std::get<std::string>(id);
}

std::optional<MinerRequestId> parse_id(const nlohmann::json &document) {
    const auto iterator = document.find("id");
    if (iterator == document.end()) {
        return std::nullopt;
    }
    if (iterator->is_number_integer()) {
        return MinerRequestId{iterator->get<std::int64_t>()};
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return MinerRequestId{static_cast<std::int64_t>(value)};
        }
        return std::nullopt;
    }
    if (iterator->is_string()) {
        const auto &value = iterator->get_ref<const std::string &>();
        if (!value.empty() && value.size() <= 128U && value.find('\0') == std::string::npos) {
            return MinerRequestId{value};
        }
    }
    return std::nullopt;
}

bool constant_equal(std::string_view first, std::string_view second) {
    if (first.size() != second.size()) {
        std::array<unsigned char, 1> dummy{};
        (void)CRYPTO_memcmp(dummy.data(), dummy.data(), dummy.size());
        return false;
    }
    return CRYPTO_memcmp(first.data(), second.data(), first.size()) == 0;
}

std::string random_id() {
    std::array<std::uint8_t, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("getrandom failed for connection ID");
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output[index * 2U] = digits[bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return output;
}

template <std::size_t N>
bool decode_hex(std::string_view encoded, std::array<std::uint8_t, N> &output) {
    if (encoded.size() != N * 2U) {
        return false;
    }
    auto nibble = [](unsigned char byte) -> int {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < N; ++index) {
        const int high = nibble(static_cast<unsigned char>(encoded[index * 2U]));
        const int low = nibble(static_cast<unsigned char>(encoded[index * 2U + 1U]));
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string normalize_hex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

bool is_nicehash_agent(std::string_view agent) noexcept {
    static constexpr std::string_view marker = "nicehash";
    return std::search(
               agent.begin(), agent.end(), marker.begin(), marker.end(),
               [](char byte, char expected) {
                   return static_cast<char>(std::tolower(
                              static_cast<unsigned char>(byte))) == expected;
               }) != agent.end();
}

std::optional<std::uint32_t> compact_target(std::uint64_t difficulty) {
    if (difficulty == 0U ||
        difficulty > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    const auto target = static_cast<std::uint32_t>(
        std::numeric_limits<std::uint32_t>::max() / difficulty);
    if (target == 0U) return std::nullopt;
    return target;
}

std::string compact_target_hex(std::uint32_t target) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded(8U, '0');
    for (std::size_t byte_index = 0; byte_index < 4U; ++byte_index) {
        const auto byte = static_cast<std::uint8_t>(
            target >> (byte_index * 8U));
        encoded[byte_index * 2U] = digits[byte >> 4U];
        encoded[byte_index * 2U + 1U] = digits[byte & 0x0fU];
    }
    return encoded;
}

struct WireTarget {
    std::string value;
    std::string_view encoding{"le64"};
};

WireTarget wire_target(const StratumJob &job,
                       const MinerConnection &connection) {
    if (is_nicehash_agent(connection.agent)) {
        if (const auto target = compact_target(
                connection.assigned_difficulty)) {
            return {compact_target_hex(*target), "le32"};
        }
    }
    return {job.target, "le64"};
}

bool strict_keys(const nlohmann::json &object,
                 std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) return false;
    for (const auto &[key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return false;
        }
    }
    return true;
}

std::uint16_t peer_port(const sockaddr_storage &storage) {
    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in *>(&storage)->sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_port);
}

void close_fd(int &descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

} // namespace

struct StratumServer::Listener {
    int descriptor{-1};
    std::string endpoint;
};

struct StratumServer::Connection {
    int descriptor{-1};
    MinerConnection info;
    bool authenticated{};
    bool closing{};
    bool close_after_flush{};
    std::string close_after_flush_reason;
    std::size_t uncreditable_work_results{};
    std::string input;
    std::deque<OutputChunk> output;
    std::size_t output_bytes{};
    std::set<std::string> live_request_ids;
    std::map<std::string, std::string, std::less<>> job_network_difficulties;
    std::deque<std::string> job_difficulty_order;
    std::size_t pending_submits{};
    std::shared_ptr<std::atomic<std::uint64_t>> latest_queued_height{
        std::make_shared<std::atomic<std::uint64_t>>(0U)};
    std::chrono::steady_clock::time_point opened{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_activity{opened};
};

struct StratumServer::PendingSubmit {
    std::weak_ptr<Connection> connection;
    StratumSubmission submission;
    std::string request_key;
};

struct StratumServer::CompletedSubmit {
    std::weak_ptr<Connection> connection;
    MinerRequestId request_id;
    std::string request_key;
    ShareResponse response;
};

std::string share_disposition_message(ShareDisposition disposition) {
    switch (disposition) {
    case ShareDisposition::accepted: return "OK";
    case ShareDisposition::stale: return "Stale share";
    case ShareDisposition::duplicate: return "Duplicate share";
    case ShareDisposition::low_difficulty: return "Low difficulty share";
    case ShareDisposition::invalid_result: return "Invalid result";
    case ShareDisposition::unknown_job: return "Unknown job";
    case ShareDisposition::server_busy: return "Server busy";
    case ShareDisposition::verifier_failed: return "Server busy";
    case ShareDisposition::cancelled: return "Server busy";
    }
    return "Server busy";
}

StratumServer::StratumServer(StratumServerConfig config, JobProvider jobs,
                             SubmitHandler submits, DefenseEngine *defense,
                             ConnectionObserver observer,
                             AdmissionResolver admission_resolver,
                             FatalErrorHandler fatal_error_handler)
    : config_(std::move(config)), jobs_(std::move(jobs)),
      submits_(std::move(submits)), defense_(defense), observer_(std::move(observer)),
      admission_resolver_(std::move(admission_resolver)),
      fatal_error_handler_(std::move(fatal_error_handler)) {
    if (config_.listen.empty() || !jobs_ || !submits_ || config_.max_connections == 0U ||
        config_.max_connections_per_ip == 0U || config_.max_line_bytes < 1024U ||
        config_.max_output_bytes_per_connection < 4096U ||
        config_.difficulty_floor == 0U || config_.max_pending_submits == 0U ||
        config_.max_pending_submits_per_connection == 0U) {
        throw std::invalid_argument("invalid Stratum server configuration");
    }
    config_.submit_workers = std::clamp<std::size_t>(config_.submit_workers, 1U, 256U);
    config_.candidate_submit_workers = std::clamp<std::size_t>(
        config_.candidate_submit_workers, 1U, 16U);
    config_.candidate_submit_reserve = std::min(
        config_.candidate_submit_reserve, config_.max_pending_submits);
    config_.max_pending_submits_per_connection = std::min(
        config_.max_pending_submits_per_connection, config_.max_pending_submits);
}

void StratumServer::remember_job(
    const std::shared_ptr<Connection> &connection, const StratumJob &job) {
    if (job.network_difficulty.empty()) return;
    if (!connection->job_network_difficulties.contains(job.job_id)) {
        connection->job_difficulty_order.push_back(job.job_id);
    }
    connection->job_network_difficulties[job.job_id] =
        job.network_difficulty;
    while (connection->job_difficulty_order.size() > 64U) {
        connection->job_network_difficulties.erase(
            connection->job_difficulty_order.front());
        connection->job_difficulty_order.pop_front();
    }
}

StratumServer::~StratumServer() { stop(); }

bool StratumServer::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::size_t StratumServer::connection_count() const {
    std::lock_guard lock(state_mutex_);
    return connections_.size();
}

std::vector<std::string> StratumServer::bound_endpoints() const {
    std::lock_guard lock(state_mutex_);
    std::vector<std::string> result;
    for (const auto &listener : listeners_) result.push_back(listener.endpoint);
    return result;
}

void StratumServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    try {
        // Keep descriptor-table buckets and listener slots allocated for the
        // configured ceiling throughout the server lifetime. Bare-metal pool
        // deployments prefer predictable admission latency over reclaiming a
        // small amount of idle RAM.
        connections_.reserve(config_.max_connections);
        listeners_.reserve(config_.listen.size());
        for (const std::string &encoded : config_.listen) {
            std::string host;
            std::string port;
            if (!encoded.empty() && encoded.front() == '[') {
                const auto end = encoded.find(']');
                if (end == std::string::npos || end + 2U > encoded.size() || encoded[end + 1U] != ':')
                    throw std::invalid_argument("invalid Stratum IPv6 endpoint");
                host = encoded.substr(1U, end - 1U);
                port = encoded.substr(end + 2U);
            } else {
                const auto colon = encoded.rfind(':');
                if (colon == std::string::npos || encoded.find(':') != colon)
                    throw std::invalid_argument("invalid Stratum endpoint");
                host = encoded.substr(0U, colon);
                port = encoded.substr(colon + 1U);
            }
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
            addrinfo *raw = nullptr;
            const int status = getaddrinfo(host.c_str(), port.c_str(), &hints, &raw);
            if (status != 0) throw std::runtime_error(gai_strerror(status));
            std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, &freeaddrinfo);
            int descriptor = -1;
            for (auto *address = addresses.get(); address != nullptr; address = address->ai_next) {
                descriptor = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    address->ai_protocol);
                if (descriptor < 0) continue;
                int reuse = 1;
                (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                if (bind(descriptor, address->ai_addr, address->ai_addrlen) == 0 &&
                    listen(descriptor, SOMAXCONN) == 0) break;
                close_fd(descriptor);
            }
            if (descriptor < 0) throw std::runtime_error("failed to bind every Stratum listener");
            listeners_.push_back({descriptor, encoded});
        }
        int wake_descriptors[2]{-1, -1};
        if (pipe2(wake_descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
            throw std::runtime_error("failed to create Stratum event-loop wakeup pipe");
        }
        {
            std::lock_guard wake_lock(wake_mutex_);
            wake_read_descriptor_ = wake_descriptors[0];
            wake_write_descriptor_ = wake_descriptors[1];
        }
        submit_threads_.reserve(config_.submit_workers);
        for (std::size_t index = 0; index < config_.submit_workers; ++index)
            submit_threads_.emplace_back([this](std::stop_token token) {
                submit_worker(token, false);
            });
        // Claimed candidates never wait behind ordinary verifier work. Their
        // bounded prestarted pool is private to this queue, so one candidate's
        // RandomX continuation cannot serialize later durable journals.
        candidate_submit_threads_.reserve(config_.candidate_submit_workers);
        for (std::size_t index = 0; index < config_.candidate_submit_workers;
             ++index) {
            candidate_submit_threads_.emplace_back(
                [this](std::stop_token token) { submit_worker(token, true); });
        }
        event_thread_ = std::jthread([this](std::stop_token) { event_loop(); });
    } catch (...) {
        stop();
        throw;
    }
}

void StratumServer::stop() noexcept {
    // A fatal event-loop exception clears running_ itself. Thread/fd cleanup
    // must still happen when stop() is subsequently called.
    running_.store(false, std::memory_order_release);
    submit_condition_.notify_all();
    wake_event_loop();
    if (event_thread_.joinable()) {
        event_thread_.request_stop();
        if (event_thread_.get_id() != std::this_thread::get_id()) {
            event_thread_.join();
        }
    }
    // event_loop() snapshots and accepts through these descriptors. Its poll
    // timeout is bounded, so leave descriptor storage unchanged until join.
    for (auto &listener : listeners_) close_fd(listener.descriptor);
    for (auto &thread : submit_threads_) thread.request_stop();
    for (auto &thread : candidate_submit_threads_) thread.request_stop();
    submit_condition_.notify_all();
    for (auto &thread : submit_threads_) if (thread.joinable()) thread.join();
    submit_threads_.clear();
    for (auto &thread : candidate_submit_threads_) {
        if (thread.joinable()) thread.join();
    }
    candidate_submit_threads_.clear();
    {
        std::lock_guard wake_lock(wake_mutex_);
        close_fd(wake_read_descriptor_);
        close_fd(wake_write_descriptor_);
    }
    {
        std::lock_guard lock(submit_mutex_);
        pending_submits_.clear();
        pending_candidate_submits_.clear();
        completed_submits_.clear();
    }
    std::vector<std::shared_ptr<Connection>> stopped;
    {
        std::lock_guard lock(state_mutex_);
        stopped.reserve(connections_.size());
        for (auto &[descriptor, connection] : connections_) {
            (void)descriptor;
            if (!connection->closing) {
                connection->closing = true;
                stopped.push_back(connection);
            }
            close_fd(connection->descriptor);
        }
        connections_.clear();
        peer_connections_.clear();
        listeners_.clear();
    }
    if (observer_) {
        for (const auto &connection : stopped) {
            try { observer_(connection->info, "server stopping"); }
            catch (...) {}
        }
    }
}

void StratumServer::event_loop() noexcept {
    // Retain poll/snapshot capacity across iterations. A rental fan-in of a
    // few hundred sockets therefore does not allocate two vectors per wakeup.
    std::vector<pollfd> descriptors;
    std::vector<std::shared_ptr<Connection>> connections;
    try {
        // Deliberately reserve the configured ceiling rather than a small
        // heuristic. Pool operators can trade RAM for stable admission and
        // refresh latency explicitly through max_connections.
        descriptors.reserve(1U + listeners_.size() + config_.max_connections);
        connections.reserve(config_.max_connections);
    }
    catch (...) {
        report_event_loop_failure();
        return;
    }
    auto next_expiry_scan = std::chrono::steady_clock::now();
    while (running()) {
        try {
            if (jobs_refresh_requested_.exchange(false, std::memory_order_acq_rel)) {
                refresh_jobs_now();
            }
            descriptors.clear();
            connections.clear();
            descriptors.push_back({wake_read_descriptor_, POLLIN, 0});
            {
                std::lock_guard lock(state_mutex_);
                for (const auto &listener : listeners_) descriptors.push_back({listener.descriptor, POLLIN, 0});
                for (const auto &[fd, connection] : connections_) {
                    short events = POLLIN;
                    if (!connection->output.empty()) events = static_cast<short>(events | POLLOUT);
                    descriptors.push_back({fd, events, 0});
                    connections.push_back(connection);
                }
            }
            const int ready = poll(descriptors.data(), descriptors.size(), 1000);
            if (ready < 0 && errno != EINTR) continue;
            if ((descriptors.front().revents & POLLIN) != 0) {
                drain_event_loop_wakeup();
            }
            // stop() closes ingress conceptually before it drains admitted
            // submit work. Never accept or parse one last poll snapshot after
            // the stop wakeup has made running_ false.
            if (!running()) break;
            for (std::size_t index = 0; index < listeners_.size(); ++index)
                if ((descriptors[1U + index].revents & POLLIN) != 0) accept_ready(listeners_[index]);
            for (std::size_t index = 0; index < connections.size(); ++index) {
                const short events = descriptors[1U + listeners_.size() + index].revents;
                if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    close_connection(connections[index], "socket error"); continue;
                }
                if ((events & POLLIN) != 0) read_ready(connections[index]);
                if (!connections[index]->closing && (events & POLLOUT) != 0) {
                    write_ready(connections[index]);
                }
            }
            drain_submit_results();
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_expiry_scan) {
                close_expired();
                next_expiry_scan = now + std::chrono::seconds(1);
            }
        }
        catch (...) {
            report_event_loop_failure();
            break;
        }
    }
}

void StratumServer::report_event_loop_failure() noexcept {
    // Normal stop wins this exchange and suppresses a false fatal report if a
    // callback happens to throw while shutdown is already under way.
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    submit_condition_.notify_all();
    if (was_running && fatal_error_handler_) {
        try { fatal_error_handler_(); }
        catch (...) {}
    }
}

void StratumServer::wake_event_loop() noexcept {
    std::lock_guard lock(wake_mutex_);
    if (wake_write_descriptor_ < 0) return;
    const std::uint8_t signal = 1U;
    for (;;) {
        const ssize_t count = write(wake_write_descriptor_, &signal, sizeof(signal));
        if (count >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EBADF) return;
        if (errno != EINTR) return;
    }
}

void StratumServer::drain_event_loop_wakeup() noexcept {
    std::array<std::uint8_t, 256> signals{};
    for (;;) {
        const ssize_t count = read(wake_read_descriptor_, signals.data(), signals.size());
        if (count > 0) continue;
        if (count < 0 && errno == EINTR) continue;
        return;
    }
}

void StratumServer::accept_ready(Listener &listener) {
    for (;;) {
        sockaddr_storage storage{}; socklen_t length = sizeof(storage);
        const int descriptor = accept4(listener.descriptor, reinterpret_cast<sockaddr *>(&storage),
                                       &length, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (descriptor < 0) { if (errno == EINTR) continue; return; }
        const int no_delay = 1;
        (void)setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY,
                         &no_delay, sizeof(no_delay));
        PeerAddress peer;
        try { peer = PeerAddress::from_socket(storage); } catch (...) { (void)close(descriptor); continue; }
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(state_mutex_);
            const auto count = peer_connections_.find(peer);
            const bool capped = connections_.size() >= config_.max_connections ||
                (count != peer_connections_.end() &&
                 count->second >= config_.max_connections_per_ip);
            // Password-protected rental endpoints commonly reconnect tens of
            // miners from one source IP at once. The hard global/per-IP caps
            // still bound unauthenticated sockets; authentication failures and
            // subsequent requests remain under the defense engine. Avoid
            // treating a legitimate connection burst as abuse before the
            // password can even be presented.
            const bool rate_limited = config_.access_password.empty() &&
                defense_ && !defense_->admit_connection(peer);
            if (capped || rate_limited) {
                (void)close(descriptor); continue;
            }
            connection = std::make_shared<Connection>();
            connection->input.reserve(config_.max_line_bytes);
            connection->descriptor = descriptor;
            connection->info.public_id = random_id();
            connection->info.peer = peer;
            connection->info.peer_port = peer_port(storage);
            connection->info.listen_address = listener.endpoint;
            connections_.emplace(descriptor, connection);
            ++peer_connections_[peer];
        }
        if (observer_) {
            try { observer_(connection->info, "opened"); }
            catch (...) { close_connection(connection, "observer failed"); }
        }
    }
}

void StratumServer::read_ready(const std::shared_ptr<Connection> &connection) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = recv(connection->descriptor, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            connection->last_activity = std::chrono::steady_clock::now();
            connection->input.append(buffer.data(), static_cast<std::size_t>(count));
            if (connection->input.size() > config_.max_line_bytes && connection->input.find('\n') == std::string::npos) {
                if (defense_) defense_->record(connection->info.peer, AbuseKind::oversized_line);
                close_connection(connection, "oversized line"); return;
            }
            for (;;) {
                const auto newline = connection->input.find('\n');
                if (newline == std::string::npos) break;
                if (newline > config_.max_line_bytes) {
                    if (defense_) defense_->record(connection->info.peer, AbuseKind::oversized_line);
                    close_connection(connection, "oversized line"); return;
                }
                std::string line = connection->input.substr(0U, newline);
                connection->input.erase(0U, newline + 1U);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                process_line(connection, std::move(line));
                if (connection->closing) return;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close_connection(connection, "peer closed"); return;
    }
}

void StratumServer::write_ready(const std::shared_ptr<Connection> &connection) {
    while (!connection->output.empty()) {
        auto &chunk = connection->output.front();
        const ssize_t count = send(connection->descriptor, chunk.data.data() + chunk.offset,
                                   chunk.data.size() - chunk.offset, MSG_NOSIGNAL);
        if (count > 0) {
            const auto amount = static_cast<std::size_t>(count);
            chunk.offset += amount; connection->output_bytes -= amount;
            if (chunk.offset == chunk.data.size()) connection->output.pop_front();
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close_connection(connection, "write failed"); return;
    }
    if (connection->close_after_flush) {
        close_connection(connection, connection->close_after_flush_reason);
    }
}

void StratumServer::process_line(const std::shared_ptr<Connection> &connection,
                                 std::string line) {
    if (line.empty() || line.find('\0') != std::string::npos) {
        if (defense_) defense_->record(connection->info.peer, AbuseKind::malformed);
        close_connection(connection, "protocol violation"); return;
    }
    if (defense_ && !defense_->admit_request(connection->info.peer)) {
        // A depleted ordinary token bucket is infrastructure backpressure, not
        // malformed evidence. Sustained evasion is measured independently by
        // DefenseEngine's per-second hammer tracker.
        close_connection(connection, "request rate exceeded"); return;
    }
    bool duplicate = false; bool depth_exceeded = false;
    std::vector<std::set<std::string>> keys;
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(line, [&](int depth, nlohmann::json::parse_event_t event,
                                                   nlohmann::json &parsed) {
            if (depth < 0 || static_cast<std::size_t>(depth) > config_.max_json_depth) depth_exceeded = true;
            if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
            else if (event == nlohmann::json::parse_event_t::key && !keys.empty()) {
                if (!keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
            } else if (event == nlohmann::json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
            return true;
        }, true, false);
    } catch (...) { duplicate = true; }
    const auto request_id = parse_id(document);
    if (duplicate || depth_exceeded || document.is_discarded() ||
        !document.is_object() || !request_id.has_value()) {
        if (defense_) defense_->record(connection->info.peer, AbuseKind::malformed);
        close_connection(connection, "malformed JSON"); return;
    }
    const auto version = document.find("jsonrpc");
    if (version != document.end() && (!version->is_string() || *version != "2.0")) {
        queue_error(connection, *request_id, -32600, "Invalid Request"); return;
    }
    const auto method = document.find("method");
    const auto params = document.find("params");
    if (method == document.end() || !method->is_string() || params == document.end() || !params->is_object()) {
        queue_error(connection, *request_id, -32600, "Invalid Request"); return;
    }
    const std::string key = id_key(*request_id);
    if (!connection->live_request_ids.insert(key).second) {
        // The original request owns the ID. A duplicate must not erase that
        // ownership when its error response is queued.
        queue_error(connection, *request_id, -1, "Server busy"); return;
    }
    const std::string name = method->get<std::string>();
    if (!connection->authenticated && name != "login") {
        queue_error(connection, *request_id, -1, "Unauthenticated", key);
        if (defense_) defense_->record(connection->info.peer, AbuseKind::malformed);
    } else if (name == "login") handle_login(connection, document, *request_id, key);
    else if (name == "submit") handle_submit(connection, document, *request_id, key);
    else if (name == "keepalived") handle_keepalive(connection, document, *request_id, key);
    else {
        queue_error(connection, *request_id, -32601, "Method not found", key);
        if (defense_) defense_->record(connection->info.peer, AbuseKind::malformed);
    }
}

void StratumServer::handle_login(const std::shared_ptr<Connection> &connection,
                                 const nlohmann::json &document,
                                 const MinerRequestId &request_id, std::string request_key) {
    if (connection->authenticated) { queue_error(connection, request_id, -1, "Already authenticated", request_key); return; }
    const auto &params = document.at("params");
    if (!strict_keys(params, {"login", "pass", "agent", "rigid", "algo"}) ||
        !params.contains("login") || !params.at("login").is_string() ||
        !params.contains("pass") || !params.at("pass").is_string() ||
        (params.contains("agent") && !params.at("agent").is_string()) ||
        (params.contains("rigid") && !params.at("rigid").is_string())) {
        queue_error(connection, request_id, -1, "Invalid login", request_key); return;
    }
    std::string login = params.at("login").get<std::string>();
    const std::string pass = params.at("pass").get<std::string>();
    const std::string agent = params.value("agent", std::string{});
    const std::string rigid = params.value("rigid", std::string{});
    if (login.empty() || login.size() > 256U || rigid.size() > 256U || agent.size() > 512U ||
        login.find('\0') != std::string::npos || rigid.find('\0') != std::string::npos ||
        agent.find('\0') != std::string::npos || pass.find('\0') != std::string::npos) {
        queue_error(connection, request_id, -1, "Invalid login", request_key); return;
    }
    if (!config_.access_password.empty() && !constant_equal(pass, config_.access_password)) {
        queue_error(connection, request_id, -1, "Unauthenticated", request_key);
        if (defense_) defense_->record(connection->info.peer, AbuseKind::authentication_failure);
        return;
    }
    if (params.contains("algo")) {
        bool supported = false;
        if (params.at("algo").is_string()) supported = params.at("algo") == "rx/0";
        else if (params.at("algo").is_array() && !params.at("algo").empty()) {
            std::set<std::string> seen;
            for (const auto &algorithm : params.at("algo")) {
                if (!algorithm.is_string() || !seen.insert(algorithm.get<std::string>()).second) {
                    queue_error(connection, request_id, -1, "Unsupported algorithm", request_key); return;
                }
                supported = supported || algorithm == "rx/0";
            }
        }
        if (!supported) { queue_error(connection, request_id, -1, "Unsupported algorithm", request_key); return; }
    }
    std::uint64_t difficulty = config_.difficulty_floor;
    if (config_.minimum_difficulty) {
        const auto plus = login.rfind('+');
        if (plus != std::string::npos) {
            if (plus == 0U || plus + 1U == login.size()) {
                queue_error(connection, request_id, -1, "Invalid login", request_key); return;
            }
            std::uint64_t requested{};
            const auto [end, error] = std::from_chars(login.data() + plus + 1U,
                                                      login.data() + login.size(), requested);
            if (error != std::errc{} || end != login.data() + login.size() || requested == 0U) {
                queue_error(connection, request_id, -1, "Invalid login", request_key); return;
            }
            difficulty = std::max(difficulty, requested); login.resize(plus);
        }
    }
    connection->info.login = login; connection->info.rigid = rigid;
    connection->info.agent = agent;
    if (is_nicehash_agent(agent)) {
        // The legacy four-byte CryptoNote target is lossy. Verify and credit
        // the exact work difficulty represented by the target sent on wire.
        const auto target = compact_target(difficulty);
        if (!target) {
            queue_error(connection, request_id, -1,
                        "NiceHash difficulty is not representable",
                        request_key);
            return;
        }
        difficulty = std::numeric_limits<std::uint32_t>::max() / *target;
    }
    connection->info.assigned_difficulty = difficulty;
    const auto job = jobs_(connection->info);
    if (!job.has_value()) { queue_error(connection, request_id, -1, "Server busy", request_key); return; }
    const WireTarget target = wire_target(*job, connection->info);
    nlohmann::json encoded_job{{"blob", job->blob}, {"job_id", job->job_id},
                              {"target", target.value},
                              {"algo", "rx/0"},
                              {"height", job->height}, {"seed_hash", job->seed_hash}};
    nlohmann::json response{{"id", id_json(request_id)}, {"jsonrpc", "2.0"}, {"error", nullptr},
                            {"result", {{"id", connection->info.public_id}, {"job", encoded_job},
                                        {"extensions", {"algo", "keepalive"}}, {"status", "OK"}}}};
    if (!queue(connection, response.dump() + "\n", request_key, job->height)) return;
    remember_job(connection, *job);
    if (job->on_queued) {
        try { job->on_queued(target.value, target.encoding); } catch (...) {
            close_connection(connection, "job persistence callback failed");
            return;
        }
    }
    connection->authenticated = true; connection->info.last_sent_height = job->height;
    if (observer_) observer_(connection->info, "authenticated");
}

void StratumServer::handle_submit(const std::shared_ptr<Connection> &connection,
                                  const nlohmann::json &document,
                                  const MinerRequestId &request_id, std::string request_key) {
    if (!running()) return;
    if (defense_ && !defense_->admit_submit(connection->info.peer)) {
        queue_error(connection, request_id, -1, "Server busy", request_key); return;
    }
    const auto &params = document.at("params");
    if (!strict_keys(params, {"id", "job_id", "nonce", "result", "algo"}) ||
        !params.contains("id") || !params.at("id").is_string() ||
        params.at("id") != connection->info.public_id ||
        !params.contains("job_id") || !params.at("job_id").is_string() ||
        !params.contains("nonce") || !params.at("nonce").is_string() ||
        !params.contains("result") || !params.at("result").is_string() ||
        (params.contains("algo") && (!params.at("algo").is_string() || params.at("algo") != "rx/0"))) {
        queue_error(connection, request_id, -1, "Invalid result", request_key); return;
    }
    StratumSubmission submission;
    submission.connection = connection->info; submission.request_id = request_id;
    submission.received_unix_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    submission.job_id = normalize_hex(params.at("job_id").get<std::string>());
    std::array<std::uint8_t, 16> job_bytes{};
    const std::string nonce = params.at("nonce").get<std::string>();
    const std::string claimed = params.at("result").get<std::string>();
    if (!decode_hex(submission.job_id, job_bytes) || !decode_hex(nonce, submission.nonce) ||
        !decode_hex(claimed, submission.claimed_hash)) {
        queue_error(connection, request_id, -1, "Invalid result", request_key); return;
    }
    submission.claimed_hash_hex = normalize_hex(claimed);
    submission.latest_queued_height_view = connection->latest_queued_height;
    std::string network_difficulty;
    if (admission_resolver_) {
        try {
            StratumAdmission admission = admission_resolver_(submission);
            submission.job_lease = std::move(admission.job_lease);
            network_difficulty = std::move(admission.network_difficulty);
        }
        catch (...) {
            queue_error(connection, request_id, -1, "Server busy", request_key);
            return;
        }
    }
    else {
        const auto difficulty = connection->job_network_difficulties.find(
            submission.job_id);
        if (difficulty != connection->job_network_difficulties.end()) {
            network_difficulty = difficulty->second;
        }
    }
    const bool claimed_candidate =
        !network_difficulty.empty() &&
        meets_network_target(submission.claimed_hash, network_difficulty);
    {
        std::lock_guard lock(submit_mutex_);
        if (!running()) return;
        const std::size_t ordinary_limit =
            config_.max_pending_submits - config_.candidate_submit_reserve;
        const std::size_t total_pending = pending_submits_.size() +
                                          pending_candidate_submits_.size();
        if (connection->pending_submits >=
                config_.max_pending_submits_per_connection ||
            total_pending >= config_.max_pending_submits ||
            (!claimed_candidate && pending_submits_.size() >= ordinary_limit)) {
            queue_error(connection, request_id, -1, "Server busy", request_key); return;
        }
        // This queue admission is the syntactic-acceptance boundary: only now
        // consume the durable per-connection request sequence.
        submission.request_sequence = ++connection->info.request_sequence;
        PendingSubmit pending{connection, std::move(submission), request_key};
        if (claimed_candidate) {
            pending_candidate_submits_.push_back(std::move(pending));
        }
        else {
            pending_submits_.push_back(std::move(pending));
        }
        ++connection->pending_submits;
        if (observer_) {
            try { observer_(connection->info, "submit_queued"); }
            catch (...) {}
        }
    }
    // Candidate and ordinary workers wait on the same condition variable but
    // have disjoint predicates.  notify_one() may wake an ineligible lane and
    // strand work indefinitely, so wake both classes on admission.
    submit_condition_.notify_all();
}

void StratumServer::handle_keepalive(const std::shared_ptr<Connection> &connection,
                                     const nlohmann::json &document,
                                     const MinerRequestId &request_id, std::string request_key) {
    const auto &params = document.at("params");
    if (!strict_keys(params, {"id"}) || !params.contains("id") || !params.at("id").is_string() ||
        params.at("id") != connection->info.public_id) {
        queue_error(connection, request_id, -1, "Unauthenticated", request_key); return;
    }
    nlohmann::json response{{"id", id_json(request_id)}, {"jsonrpc", "2.0"}, {"error", nullptr},
                            {"result", {{"status", "KEEPALIVED"}}}};
    (void)queue(connection, response.dump() + "\n", request_key);
    connection->last_activity = std::chrono::steady_clock::now();
}

bool StratumServer::queue(const std::shared_ptr<Connection> &connection, std::string data,
                          std::optional<std::string> release_key,
                          std::optional<std::uint64_t> sent_height) {
    if (connection->closing || data.size() > config_.max_output_bytes_per_connection ||
        connection->output_bytes > config_.max_output_bytes_per_connection - data.size()) {
        close_connection(connection, "output queue exceeded"); return false;
    }
    connection->output_bytes += data.size();
    connection->output.push_back({std::move(data), 0U});
    // A complete JSON frame is queued atomically. Request IDs become reusable
    // at this point; slow socket transmission must not pin miner request IDs.
    if (release_key) connection->live_request_ids.erase(*release_key);
    if (sent_height) {
        connection->info.last_sent_height = *sent_height;
        connection->latest_queued_height->store(*sent_height,
                                                std::memory_order_release);
    }
    return true;
}

void StratumServer::queue_error(const std::shared_ptr<Connection> &connection,
                                const MinerRequestId &id, int code, std::string_view message,
                                std::optional<std::string> release_key) {
    nlohmann::json response{{"id", id_json(id)}, {"jsonrpc", "2.0"},
                            {"error", {{"code", code}, {"message", message}}}, {"result", nullptr}};
    (void)queue(connection, response.dump() + "\n", std::move(release_key));
}

void StratumServer::submit_worker(std::stop_token token,
                                  const bool candidate_lane) noexcept {
    for (;;) {
        PendingSubmit work;
        {
            std::unique_lock lock(submit_mutex_);
            submit_condition_.wait(lock, [this, &token, candidate_lane] {
                const auto &pending = candidate_lane
                                          ? pending_candidate_submits_
                                          : pending_submits_;
                return !running() || token.stop_requested() || !pending.empty();
            });
            auto &pending = candidate_lane ? pending_candidate_submits_
                                           : pending_submits_;
            if (pending.empty()) {
                if (!running() || token.stop_requested()) return;
                continue;
            }
            work = std::move(pending.front());
            pending.pop_front();
        }
        ShareResponse response;
        try { response = submits_(work.submission); }
        catch (...) { response = {ShareDisposition::verifier_failed, "submit_handler_failed"}; }
        if (observer_) {
            try { observer_(work.submission.connection, "submit_completed"); }
            catch (...) {}
        }
        try {
            std::lock_guard lock(submit_mutex_);
            completed_submits_.push_back({work.connection, work.submission.request_id,
                                          std::move(work.request_key), std::move(response)});
        }
        catch (...) {
            // Runtime handlers have already made their durable decision. If
            // even the bounded response route cannot allocate, stop this
            // listener cleanly instead of terminating the whole process from
            // the noexcept worker entry point.
            report_event_loop_failure();
            return;
        }
        wake_event_loop();
    }
}

void StratumServer::drain_submit_results() {
    std::deque<CompletedSubmit> results;
    { std::lock_guard lock(submit_mutex_); results.swap(completed_submits_); }
    for (auto &result : results) {
        auto connection = result.connection.lock();
        if (!connection || connection->closing) continue;
        if (connection->pending_submits != 0U) --connection->pending_submits;
        if (result.response.disposition == ShareDisposition::accepted) {
            nlohmann::json response{{"id", id_json(result.request_id)}, {"jsonrpc", "2.0"},
                                    {"error", nullptr}, {"result", {{"status", "OK"}}}};
            (void)queue(connection, response.dump() + "\n", result.request_key);
        } else {
            queue_error(connection, result.request_id, -1,
                        share_disposition_message(result.response.disposition), result.request_key);
            bool close_after_flush = false;
            std::string_view close_reason;
            if ((result.response.disposition == ShareDisposition::low_difficulty ||
                 result.response.disposition == ShareDisposition::stale) &&
                ++connection->uncreditable_work_results >= 64U) {
                close_after_flush = true;
                close_reason = "repeated uncreditable work";
            }
            if (result.response.internal_code == "duplicate_registry_full") {
                close_after_flush = true;
                close_reason = "duplicate registry capacity";
            }
            if (close_after_flush && !connection->closing) {
                connection->close_after_flush = true;
                connection->close_after_flush_reason = close_reason;
            }
            if (defense_ && result.response.disposition == ShareDisposition::duplicate)
                defense_->record(connection->info.peer, AbuseKind::duplicate);
            if (defense_ && result.response.disposition == ShareDisposition::unknown_job)
                defense_->record(connection->info.peer, AbuseKind::unknown_job);
        }
    }
}

void StratumServer::refresh_jobs() {
    jobs_refresh_requested_.store(true, std::memory_order_release);
    wake_event_loop();
}

void StratumServer::refresh_jobs_now() {
    std::vector<std::shared_ptr<Connection>> active;
    {
        std::lock_guard lock(state_mutex_);
        for (const auto &[descriptor, connection] : connections_) {
            (void)descriptor;
            active.push_back(connection);
        }
    }
    for (const auto &connection : active) {
        if (!running()) return;
        if (!connection->authenticated || connection->closing) continue;
        const auto job = jobs_(connection->info);
        if (!job) { close_connection(connection, "job unavailable"); continue; }
        const WireTarget target = wire_target(*job, connection->info);
        nlohmann::json notification{{"jsonrpc", "2.0"}, {"method", "job"},
                                    {"params", {{"blob", job->blob}, {"job_id", job->job_id},
                                                {"target", target.value},
                                                {"algo", "rx/0"},
                                                {"height", job->height}, {"seed_hash", job->seed_hash}}}};
        if (queue(connection, notification.dump() + "\n", std::nullopt,
                  job->height)) {
            remember_job(connection, *job);
            if (job->on_queued) {
                try { job->on_queued(target.value, target.encoding); }
                catch (...) {
                    close_connection(connection,
                                     "job persistence callback failed");
                }
            }
        }
    }
}

void StratumServer::close_expired() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Connection>> expired;
    { std::lock_guard lock(state_mutex_); for (const auto &[fd, connection] : connections_) {
        (void)fd;
        if (defense_ && defense_->banned(connection->info.peer)) {
            expired.push_back(connection);
            continue;
        }
        const auto limit = connection->authenticated ? config_.idle_timeout_ms : config_.login_timeout_ms;
        const auto since = connection->authenticated ? connection->last_activity : connection->opened;
        if (now - since >= std::chrono::milliseconds(limit)) expired.push_back(connection);
    }}
    for (auto &connection : expired) close_connection(connection, "timeout");
}

void StratumServer::close_connection(const std::shared_ptr<Connection> &connection,
                                     std::string_view reason) noexcept {
    if (connection->closing) return;
    connection->closing = true;
    if (observer_) { try { observer_(connection->info, reason); } catch (...) {} }
    const int descriptor = connection->descriptor;
    close_fd(connection->descriptor);
    std::lock_guard lock(state_mutex_);
    connections_.erase(descriptor);
    const auto iterator = peer_connections_.find(connection->info.peer);
    if (iterator != peer_connections_.end() && --iterator->second == 0U) peer_connections_.erase(iterator);
}

} // namespace monero_solo
