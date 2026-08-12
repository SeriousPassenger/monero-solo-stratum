#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace monero_solo {

struct EventStreamConfig {
    std::string unix_socket;
    unsigned permissions{0660U};
    std::size_t max_clients{8};
    std::size_t max_pending_bytes_per_client{1024U * 1024U};
};

class EventStream final {
public:
    EventStream(EventStreamConfig config, std::string session_public_id);
    EventStream(const EventStream &) = delete;
    EventStream &operator=(const EventStream &) = delete;
    ~EventStream();

    void start(std::uint64_t initial_high_watermark = 0);
    void stop() noexcept;
    void publish_committed(std::uint64_t event_id, std::string encoded_event);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t high_watermark() const noexcept;

private:
    struct Client;
    void loop() noexcept;
    void accept_clients();
    void service_clients();
    void close_client(std::size_t index) noexcept;

    EventStreamConfig config_;
    std::string session_public_id_;
    int listener_{-1};
    bool owns_socket_path_{};
    std::atomic<bool> running_{false};
    // Guarded by clients_mutex_ together with the subscriber set.
    std::uint64_t high_watermark_{};
    std::jthread thread_;
    mutable std::mutex clients_mutex_;
    std::vector<Client> clients_;
};

} // namespace monero_solo
