#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace monero_solo {

class ZmqSubscriber final {
public:
    using MessageHandler = std::function<void(std::string_view)>;
    using ErrorHandler = std::function<void(std::string_view)>;

    ZmqSubscriber(std::string endpoint, MessageHandler on_message,
                  ErrorHandler on_error = {});
    ZmqSubscriber(const ZmqSubscriber &) = delete;
    ZmqSubscriber &operator=(const ZmqSubscriber &) = delete;
    ~ZmqSubscriber();

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool available() const noexcept;

private:
    struct Api;
    void run() noexcept;
    void report(std::string_view message) noexcept;

    std::string endpoint_;
    MessageHandler on_message_;
    ErrorHandler on_error_;
    std::unique_ptr<Api> api_;
    std::jthread thread_;
    std::atomic<bool> running_{false};
};

} // namespace monero_solo
