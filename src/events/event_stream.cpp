#include "monero_solo/event_stream.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace monero_solo {
namespace {

void close_descriptor(int &descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

std::string rfc3339_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm broken{};
    if (gmtime_r(&raw, &broken) == nullptr) {
        return "1970-01-01T00:00:00.000000Z";
    }
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ",
                  broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday,
                  broken.tm_hour, broken.tm_min, broken.tm_sec,
                  static_cast<long long>(micros));
    return buffer.data();
}

void make_nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("could not make event socket nonblocking");
    }
}

} // namespace

struct EventStream::Client {
    int descriptor{-1};
    std::deque<std::string> output;
    std::size_t front_offset{};
    std::size_t pending_bytes{};
};

EventStream::EventStream(EventStreamConfig config, std::string session_public_id)
    : config_(std::move(config)), session_public_id_(std::move(session_public_id)) {
    if (config_.unix_socket.empty() || config_.unix_socket.front() != '/' ||
        config_.max_clients == 0U || config_.max_pending_bytes_per_client < 4096U ||
        session_public_id_.size() != 32U) {
        throw std::invalid_argument("invalid event stream configuration");
    }
    if ((config_.permissions & 0007U) != 0U || (config_.permissions & 0111U) != 0U) {
        throw std::invalid_argument("unsafe event stream permissions");
    }
}

EventStream::~EventStream() { stop(); }

bool EventStream::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::uint64_t EventStream::high_watermark() const noexcept {
    std::lock_guard lock(clients_mutex_);
    return high_watermark_;
}

void EventStream::start(std::uint64_t initial_high_watermark) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (config_.unix_socket.size() >= sizeof(address.sun_path)) {
            throw std::invalid_argument("event stream socket path is too long");
        }
        struct stat existing{};
        if (lstat(config_.unix_socket.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
                throw std::runtime_error("refusing to unlink unsafe event stream path");
            }
            if (::unlink(config_.unix_socket.c_str()) != 0) {
                throw std::runtime_error("could not unlink stale event stream socket");
            }
        }
        else if (errno != ENOENT) {
            throw std::runtime_error("could not inspect event stream socket path");
        }
        listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            throw std::runtime_error("could not create event stream socket");
        }
        std::memcpy(address.sun_path, config_.unix_socket.c_str(),
                    config_.unix_socket.size() + 1U);
        if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)) != 0 ||
            ::chmod(config_.unix_socket.c_str(), config_.permissions) != 0 ||
            ::listen(listener_, static_cast<int>(std::min<std::size_t>(
                                    config_.max_clients, 1024U))) != 0) {
            throw std::runtime_error("could not bind event stream socket");
        }
        owns_socket_path_ = true;
        make_nonblocking(listener_);
        {
            std::lock_guard lock(clients_mutex_);
            high_watermark_ = initial_high_watermark;
        }
        thread_ = std::jthread([this](std::stop_token) { loop(); });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        close_descriptor(listener_);
        if (owns_socket_path_) {
            (void)::unlink(config_.unix_socket.c_str());
            owns_socket_path_ = false;
        }
        throw;
    }
}

void EventStream::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    close_descriptor(listener_);
    {
        std::lock_guard lock(clients_mutex_);
        for (auto &client : clients_) {
            close_descriptor(client.descriptor);
        }
        clients_.clear();
    }
    if (owns_socket_path_) {
        struct stat current{};
        if (lstat(config_.unix_socket.c_str(), &current) == 0 &&
            S_ISSOCK(current.st_mode) && current.st_uid == geteuid()) {
            (void)::unlink(config_.unix_socket.c_str());
        }
        owns_socket_path_ = false;
    }
}

void EventStream::publish_committed(std::uint64_t event_id,
                                    std::string encoded_event) {
    if (!running() || event_id == 0U) {
        return;
    }
    if (encoded_event.empty() || encoded_event.back() != '\n') {
        encoded_event.push_back('\n');
    }
    std::lock_guard lock(clients_mutex_);
    const std::uint64_t current = high_watermark_;
    if (event_id <= current) {
        return;
    }
    high_watermark_ = event_id;
    for (std::size_t index = clients_.size(); index-- > 0U;) {
        Client &client = clients_[index];
        if (encoded_event.size() > config_.max_pending_bytes_per_client ||
            client.pending_bytes > config_.max_pending_bytes_per_client - encoded_event.size()) {
            close_client(index);
            continue;
        }
        client.pending_bytes += encoded_event.size();
        client.output.push_back(encoded_event);
    }
}

void EventStream::loop() noexcept {
    while (running()) {
        try {
            accept_clients();
            service_clients();
        }
        catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void EventStream::accept_clients() {
    while (running()) {
        const int descriptor = ::accept4(listener_, nullptr, nullptr,
                                         SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (descriptor < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        std::lock_guard lock(clients_mutex_);
        if (clients_.size() >= config_.max_clients) {
            (void)::close(descriptor);
            continue;
        }
        const auto watermark = high_watermark_;
        std::string control =
            "{\"schema_version\":1,\"control\":\"stream_open\",\"session_id\":\"" +
            session_public_id_ + "\",\"time_utc\":\"" + rfc3339_now() +
            "\",\"last_committed_event_id\":\"" + std::to_string(watermark) + "\"}\n";
        Client client;
        client.descriptor = descriptor;
        client.pending_bytes = control.size();
        if (client.pending_bytes > config_.max_pending_bytes_per_client) {
            (void)::close(descriptor);
            continue;
        }
        client.output.push_back(std::move(control));
        clients_.push_back(std::move(client));
    }
}

void EventStream::service_clients() {
    std::lock_guard lock(clients_mutex_);
    for (std::size_t index = clients_.size(); index-- > 0U;) {
        Client &client = clients_[index];
        pollfd descriptor{client.descriptor,
                          static_cast<short>(POLLIN | (client.output.empty() ? 0 : POLLOUT)),
                          0};
        const int result = ::poll(&descriptor, 1, 0);
        if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close_client(index);
            continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            std::array<char, 1> unexpected{};
            const ssize_t count = ::recv(client.descriptor, unexpected.data(),
                                         unexpected.size(), MSG_DONTWAIT);
            if (count != 0) {
                close_client(index);
                continue;
            }
            close_client(index);
            continue;
        }
        while ((descriptor.revents & POLLOUT) != 0 && !client.output.empty()) {
            std::string &front = client.output.front();
            const ssize_t sent = ::send(client.descriptor,
                                        front.data() + client.front_offset,
                                        front.size() - client.front_offset,
                                        MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) {
                const auto amount = static_cast<std::size_t>(sent);
                client.front_offset += amount;
                client.pending_bytes -= amount;
                if (client.front_offset == front.size()) {
                    client.output.pop_front();
                    client.front_offset = 0;
                }
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            close_client(index);
            break;
        }
    }
}

void EventStream::close_client(std::size_t index) noexcept {
    close_descriptor(clients_[index].descriptor);
    if (index + 1U != clients_.size()) {
        clients_[index] = std::move(clients_.back());
    }
    clients_.pop_back();
}

} // namespace monero_solo
