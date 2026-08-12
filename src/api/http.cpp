#include "monero_solo/http.hpp"
#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace monero_solo {
namespace {

constexpr std::size_t kHeaderLimit = 16U * 1024U;
constexpr std::size_t kQueueLimit = 4096U;
constexpr std::size_t kMaxConnectionsPerPeer = 2U;
constexpr std::size_t kRequestsPerConnection = 100U;
constexpr auto kConnectionLifetime = std::chrono::seconds(60);
constexpr auto kInitialHeaderDeadline = std::chrono::seconds(5);

struct HttpEndpoint {
    std::string host;
    std::string port;
};

HttpEndpoint split_endpoint(std::string_view value) {
    if (value.empty()) {
        throw std::invalid_argument("empty HTTP listen endpoint");
    }
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos || close + 2U > value.size() ||
            value[close + 1U] != ':') {
            throw std::invalid_argument("invalid bracketed HTTP endpoint");
        }
        return {std::string(value.substr(1U, close - 1U)),
                std::string(value.substr(close + 2U))};
    }
    const auto colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0U || colon + 1U == value.size() ||
        value.find(':') != colon) {
        throw std::invalid_argument("invalid HTTP endpoint");
    }
    return {std::string(value.substr(0U, colon)), std::string(value.substr(colon + 1U))};
}

std::string peer_string(const sockaddr_storage &storage) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (storage.ss_family == AF_INET) {
        const auto *address = reinterpret_cast<const sockaddr_in *>(&storage);
        if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) != nullptr) {
            return text.data();
        }
    }
    else if (storage.ss_family == AF_INET6) {
        const auto *address = reinterpret_cast<const sockaddr_in6 *>(&storage);
        if (IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)) {
            std::array<unsigned char, 4> mapped{};
            std::memcpy(mapped.data(), &address->sin6_addr.s6_addr[12], mapped.size());
            if (inet_ntop(AF_INET, mapped.data(), text.data(), text.size()) != nullptr) {
                return text.data();
            }
        }
        if (inet_ntop(AF_INET6, &address->sin6_addr, text.data(), text.size()) != nullptr) {
            return text.data();
        }
    }
    return "unknown";
}

void close_fd(int &descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

bool write_all(int descriptor, std::string_view data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::send(descriptor, data.data() + offset,
                                       data.size() - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return result;
}

std::string trim_ascii(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return std::string(value);
}

std::optional<HttpRequest> parse_request(std::string_view encoded, std::string peer) {
    const auto line_end = encoded.find("\r\n");
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto line = encoded.substr(0U, line_end);
    const auto first_space = line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        line.find(' ', second_space + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    HttpRequest request;
    request.method = std::string(line.substr(0U, first_space));
    request.target = std::string(line.substr(first_space + 1U,
                                             second_space - first_space - 1U));
    request.version = std::string(line.substr(second_space + 1U));
    request.peer = std::move(peer);
    if (request.method.empty() || request.target.empty() ||
        (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") ||
        request.target.front() != '/') {
        return std::nullopt;
    }
    const auto query = request.target.find('?');
    request.path = request.target.substr(0U, query);
    request.query = query == std::string::npos ? "" : request.target.substr(query + 1U);

    std::size_t cursor = line_end + 2U;
    while (cursor + 2U <= encoded.size()) {
        const auto end = encoded.find("\r\n", cursor);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        if (end == cursor) {
            return request;
        }
        const auto header = encoded.substr(cursor, end - cursor);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return std::nullopt;
        }
        const std::string name = lowercase(header.substr(0U, colon));
        if (!std::all_of(name.begin(), name.end(), [](unsigned char byte) {
                return std::isalnum(byte) != 0 || byte == '-';
            })) {
            return std::nullopt;
        }
        if (!request.headers.emplace(name, trim_ascii(header.substr(colon + 1U))).second) {
            return std::nullopt;
        }
        cursor = end + 2U;
    }
    return std::nullopt;
}

std::string json_error(std::string_view code, std::string_view message) {
    return std::string("{\"schema_version\":1,\"generated_at\":\"") +
           format_rfc3339_utc_us(unix_time_us()) +
           "\",\"error\":{\"code\":\"" +
           std::string(code) + "\",\"message\":\"" + std::string(message) + "\"}}";
}

HttpResponse built_in_error(int status, std::string_view code,
                            std::string_view message) {
    return {status, json_error(code, message), {}};
}

} // namespace

struct HttpServer::Client {
    int descriptor{-1};
    std::string peer;
};

struct HttpServer::Bucket {
    double tokens{};
    std::chrono::steady_clock::time_point updated{std::chrono::steady_clock::now()};
};

std::string http_reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

HttpServer::HttpServer(HttpServerConfig config, Handler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {
    if (!handler_ || config_.listen.empty() || config_.max_connections == 0U ||
        config_.request_rate_per_second == 0U || config_.request_burst == 0U ||
        config_.max_pending_bytes_per_connection < 4096U) {
        throw std::invalid_argument("invalid HTTP server configuration");
    }
    config_.worker_threads = std::clamp<std::size_t>(config_.worker_threads, 1U, 64U);
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::string HttpServer::bound_endpoint() const { return bound_endpoint_; }

void HttpServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        const HttpEndpoint endpoint = split_endpoint(config_.listen);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
        addrinfo *raw = nullptr;
        const int lookup = getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &raw);
        if (lookup != 0) {
            throw std::runtime_error(std::string("HTTP bind lookup failed: ") + gai_strerror(lookup));
        }
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, &freeaddrinfo);
        for (const addrinfo *candidate = addresses.get(); candidate != nullptr;
             candidate = candidate->ai_next) {
            const int descriptor = ::socket(candidate->ai_family,
                                            candidate->ai_socktype | SOCK_CLOEXEC,
                                            candidate->ai_protocol);
            if (descriptor < 0) {
                continue;
            }
            int reuse = 1;
            (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (::bind(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
                ::listen(descriptor, 256) == 0) {
                listener_ = descriptor;
                break;
            }
            (void)::close(descriptor);
        }
        if (listener_ < 0) {
            throw std::runtime_error("could not bind HTTP listener");
        }
        bound_endpoint_ = config_.listen;
        workers_.reserve(config_.worker_threads);
        for (std::size_t index = 0; index < config_.worker_threads; ++index) {
            workers_.emplace_back([this](std::stop_token) { worker_loop(); });
        }
        accept_thread_ = std::jthread([this](std::stop_token) { accept_loop(); });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        if (listener_ >= 0) {
            (void)::shutdown(listener_, SHUT_RDWR);
        }
        close_fd(listener_);
        queue_condition_.notify_all();
        if (accept_thread_.joinable()) {
            accept_thread_.request_stop();
            accept_thread_.join();
        }
        for (auto &thread : workers_) thread.request_stop();
        queue_condition_.notify_all();
        for (auto &thread : workers_) {
            if (thread.joinable()) thread.join();
        }
        workers_.clear();
        throw;
    }
}

void HttpServer::stop() noexcept {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running && !accept_thread_.joinable() && workers_.empty() &&
        listener_ < 0) {
        return;
    }
    if (listener_ >= 0) {
        (void)::shutdown(listener_, SHUT_RDWR);
    }
    queue_condition_.notify_all();
    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
        accept_thread_.join();
    }
    // shutdown() wakes accept4(); keep the descriptor value stable until the
    // accept thread has stopped reading it.
    close_fd(listener_);
    for (auto &thread : workers_) {
        thread.request_stop();
    }
    queue_condition_.notify_all();
    for (auto &thread : workers_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workers_.clear();
    std::lock_guard lock(queue_mutex_);
    for (auto &client : queue_) {
        close_fd(client.descriptor);
    }
    queue_.clear();
    peer_active_.clear();
}

void HttpServer::accept_loop() noexcept {
    while (running()) {
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        const int client = ::accept4(listener_, reinterpret_cast<sockaddr *>(&storage),
                                     &length, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running()) {
                std::this_thread::yield();
            }
            continue;
        }
        if (active_.load(std::memory_order_relaxed) >= config_.max_connections) {
            (void)::close(client);
            continue;
        }
        try {
            std::string peer = peer_string(storage);
            {
                std::lock_guard lock(queue_mutex_);
                const auto existing = peer_active_.find(peer);
                if (queue_.size() >= kQueueLimit ||
                    (existing != peer_active_.end() &&
                     existing->second >= kMaxConnectionsPerPeer)) {
                    (void)::close(client);
                    continue;
                }
                auto [peer_count, inserted] = peer_active_.try_emplace(peer, 0U);
                (void)inserted;
                queue_.push_back({client, std::move(peer)});
                ++peer_count->second;
                active_.fetch_add(1U, std::memory_order_relaxed);
            }
            queue_condition_.notify_one();
        }
        catch (...) {
            // Allocation/address-format failures affect only this accepted
            // socket; the noexcept accept loop remains alive.
            (void)::close(client);
        }
    }
}

void HttpServer::worker_loop() noexcept {
    for (;;) {
        Client client;
        {
            std::unique_lock lock(queue_mutex_);
            queue_condition_.wait(lock, [this] { return !running() || !queue_.empty(); });
            if (queue_.empty()) {
                if (!running()) {
                    return;
                }
                continue;
            }
            client = std::move(queue_.front());
            queue_.pop_front();
        }
        const std::string &peer_name = client.peer;
        try {
            serve(client);
        }
        catch (...) {
            close_fd(client.descriptor);
        }
        {
            std::lock_guard lock(queue_mutex_);
            const auto peer = peer_active_.find(peer_name);
            if (peer != peer_active_.end() && --peer->second == 0U) {
                peer_active_.erase(peer);
            }
        }
        active_.fetch_sub(1U, std::memory_order_relaxed);
    }
}

bool HttpServer::admit(std::string_view peer, double &retry_after) {
    std::lock_guard lock(buckets_mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto iterator = buckets_.find(std::string(peer));
    if (iterator == buckets_.end()) {
        const std::size_t bucket_limit = std::max<std::size_t>(
            1024U, config_.max_connections * 16U);
        if (buckets_.size() >= bucket_limit) {
            const auto stale_after = std::max(
                std::chrono::seconds(60),
                std::chrono::seconds(static_cast<std::int64_t>(
                    (config_.request_burst + config_.request_rate_per_second - 1U) /
                    config_.request_rate_per_second)));
            std::erase_if(buckets_, [&](const auto &entry) {
                return now - entry.second.updated >= stale_after;
            });
        }
        if (buckets_.size() >= bucket_limit) {
            retry_after = 1.0;
            return false;
        }
        iterator = buckets_.try_emplace(std::string(peer)).first;
        iterator->second.tokens = static_cast<double>(config_.request_burst);
        iterator->second.updated = now;
    }
    Bucket &bucket = iterator->second;
    const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
    bucket.tokens = std::min(static_cast<double>(config_.request_burst),
                             bucket.tokens + elapsed *
                                 static_cast<double>(config_.request_rate_per_second));
    bucket.updated = now;
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        retry_after = 0.0;
        return true;
    }
    retry_after = (1.0 - bucket.tokens) /
                  static_cast<double>(config_.request_rate_per_second);
    return false;
}

void HttpServer::serve(Client client) {
    const auto opened = std::chrono::steady_clock::now();
    std::string pending;
    pending.reserve(kHeaderLimit);
    for (std::size_t request_count = 0;
         request_count < kRequestsPerConnection && running(); ++request_count) {
        while (pending.find("\r\n\r\n") == std::string::npos) {
            if (pending.size() >= kHeaderLimit ||
                std::chrono::steady_clock::now() - opened >=
                    (request_count == 0U ? kInitialHeaderDeadline
                                         : kConnectionLifetime)) {
                close_fd(client.descriptor);
                return;
            }
            pollfd wait{client.descriptor, POLLIN, 0};
            const int ready = ::poll(&wait, 1, 1000);
            if (ready == 0) {
                continue;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close_fd(client.descriptor);
                return;
            }
            std::array<char, 4096> buffer{};
            const ssize_t received = ::recv(client.descriptor, buffer.data(), buffer.size(), 0);
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            if (received <= 0) {
                close_fd(client.descriptor);
                return;
            }
            pending.append(buffer.data(), static_cast<std::size_t>(received));
        }

        const auto end = pending.find("\r\n\r\n") + 4U;
        const std::string encoded = pending.substr(0U, end);
        pending.erase(0U, end);
        const auto parsed = parse_request(encoded, client.peer);
        HttpResponse response;
        bool close_after = false;
        if (!parsed.has_value()) {
            response = built_in_error(400, "invalid_query", "Malformed HTTP request");
            close_after = true;
        }
        else {
            const auto content = parsed->headers.find("content-length");
            const auto transfer = parsed->headers.find("transfer-encoding");
            std::uint64_t content_length = 0;
            bool valid_content_length = true;
            if (content != parsed->headers.end()) {
                const char *const first = content->second.data();
                const char *const last = first + content->second.size();
                const auto parsed_length = std::from_chars(first, last, content_length);
                valid_content_length = first != last && parsed_length.ec == std::errc{} &&
                                       parsed_length.ptr == last;
            }
            if (transfer != parsed->headers.end() || !valid_content_length ||
                content_length != 0U) {
                response = built_in_error(400, "invalid_query", "Request body must be empty");
                close_after = true;
            }
            else {
                double retry = 0.0;
                if (!admit(parsed->peer, retry)) {
                    response = built_in_error(429, "rate_limited", "API request rate exceeded");
                    response.headers.emplace_back("Retry-After",
                        std::to_string(static_cast<unsigned>(std::max(1.0, std::ceil(retry)))));
                }
                else {
                    try {
                        response = handler_(*parsed);
                    }
                    catch (...) {
                        response = built_in_error(500, "query_failed", "Request failed");
                    }
                }
                const auto connection = parsed->headers.find("connection");
                close_after = parsed->version == "HTTP/1.0" ||
                              (connection != parsed->headers.end() &&
                               lowercase(connection->second) == "close");
            }
        }

        if (response.body.size() > config_.max_pending_bytes_per_connection) {
            close_fd(client.descriptor);
            return;
        }
        std::string wire = "HTTP/1.1 " + std::to_string(response.status) + " " +
                           http_reason_phrase(response.status) + "\r\n";
        wire += "Content-Type: application/json; charset=utf-8\r\n";
        wire += "Cache-Control: no-store\r\n";
        wire += "X-Content-Type-Options: nosniff\r\n";
        for (const auto &[name, value] : response.headers) {
            wire += name + ": " + value + "\r\n";
        }
        const bool final_request = request_count + 1U == kRequestsPerConnection ||
                                   std::chrono::steady_clock::now() - opened >=
                                       kConnectionLifetime;
        if (close_after || final_request) {
            wire += "Connection: close\r\n";
        }
        else {
            wire += "Connection: keep-alive\r\n";
        }
        wire += "Content-Length: " + std::to_string(response.body.size()) + "\r\n\r\n";
        wire += response.body;
        if (wire.size() > config_.max_pending_bytes_per_connection ||
            !write_all(client.descriptor, wire)) {
            close_fd(client.descriptor);
            return;
        }
        if (close_after || final_request) {
            close_fd(client.descriptor);
            return;
        }
    }
    close_fd(client.descriptor);
}

} // namespace monero_solo
