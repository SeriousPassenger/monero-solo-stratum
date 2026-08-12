#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace monero_solo {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string, std::less<>> headers;
    std::string peer;
};

struct HttpResponse {
    int status{200};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpServerConfig {
    std::string listen;
    std::size_t max_connections{64};
    std::uint32_t request_rate_per_second{20};
    std::uint32_t request_burst{40};
    std::size_t max_pending_bytes_per_connection{2U * 1024U * 1024U};
    std::size_t worker_threads{4};
};

class HttpServer final {
public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;

    HttpServer(HttpServerConfig config, Handler handler);
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;
    ~HttpServer();

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string bound_endpoint() const;

private:
    struct Client;
    struct Bucket;
    void accept_loop() noexcept;
    void worker_loop() noexcept;
    void serve(Client client);
    [[nodiscard]] bool admit(std::string_view peer, double &retry_after);

    HttpServerConfig config_;
    Handler handler_;
    int listener_{-1};
    std::string bound_endpoint_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> active_{0};
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<Client> queue_;
    std::unordered_map<std::string, std::size_t> peer_active_;
    std::mutex buckets_mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

[[nodiscard]] std::string http_reason_phrase(int status);

} // namespace monero_solo
