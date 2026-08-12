#include "monero_solo/zmq.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <utility>

namespace monero_solo {
namespace {

constexpr int kZmqSubscribeSocket = 2;
constexpr int kZmqSubscribe = 6;
constexpr int kZmqLinger = 17;
constexpr int kZmqReceiveTimeout = 27;
constexpr std::size_t kMaxMessage = 1024U * 1024U;

template <typename T>
T symbol(void *library, const char *name) {
    dlerror();
    void *address = dlsym(library, name);
    if (address == nullptr || dlerror() != nullptr) {
        throw std::runtime_error(std::string("missing libzmq symbol: ") + name);
    }
    return reinterpret_cast<T>(address);
}

} // namespace

struct ZmqSubscriber::Api {
    using CtxNew = void *(*)();
    using CtxShutdown = int (*)(void *);
    using CtxTerm = int (*)(void *);
    using Socket = void *(*)(void *, int);
    using Close = int (*)(void *);
    using Connect = int (*)(void *, const char *);
    using SetSockOpt = int (*)(void *, int, const void *, std::size_t);
    using Receive = int (*)(void *, void *, std::size_t, int);
    using ErrorString = const char *(*)(int);
    using ErrorNumber = int (*)();

    void *library{};
    CtxNew ctx_new{};
    CtxShutdown ctx_shutdown{};
    CtxTerm ctx_term{};
    Socket socket{};
    Close close{};
    Connect connect{};
    SetSockOpt set_sock_opt{};
    Receive receive{};
    ErrorString error_string{};
    ErrorNumber error_number{};
    std::atomic<void *> context{nullptr};
    void *subscriber{};

    Api() {
        library = dlopen("libzmq.so.5", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            library = dlopen("libzmq.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (library == nullptr) {
            return;
        }
        try {
            ctx_new = symbol<CtxNew>(library, "zmq_ctx_new");
            ctx_shutdown = symbol<CtxShutdown>(library, "zmq_ctx_shutdown");
            ctx_term = symbol<CtxTerm>(library, "zmq_ctx_term");
            socket = symbol<Socket>(library, "zmq_socket");
            close = symbol<Close>(library, "zmq_close");
            connect = symbol<Connect>(library, "zmq_connect");
            set_sock_opt = symbol<SetSockOpt>(library, "zmq_setsockopt");
            receive = symbol<Receive>(library, "zmq_recv");
            error_string = symbol<ErrorString>(library, "zmq_strerror");
            error_number = symbol<ErrorNumber>(library, "zmq_errno");
        }
        catch (...) {
            dlclose(library);
            library = nullptr;
            throw;
        }
    }

    ~Api() {
        if (subscriber != nullptr && close != nullptr) {
            (void)close(subscriber);
        }
        if (void *published = context.load(std::memory_order_acquire);
            published != nullptr && ctx_term != nullptr) {
            (void)ctx_term(published);
        }
        if (library != nullptr) {
            dlclose(library);
        }
    }

    [[nodiscard]] bool loaded() const noexcept { return library != nullptr; }
    [[nodiscard]] std::string last_error() const {
        if (error_number == nullptr || error_string == nullptr) {
            return "unknown libzmq error";
        }
        const int code = error_number();
        const char *message = error_string(code);
        return message == nullptr ? "unknown libzmq error" : std::string(message);
    }
};

ZmqSubscriber::ZmqSubscriber(std::string endpoint, MessageHandler on_message,
                             ErrorHandler on_error)
    : endpoint_(std::move(endpoint)),
      on_message_(std::move(on_message)),
      on_error_(std::move(on_error)),
      api_(std::make_unique<Api>()) {
    if (endpoint_.empty() || !on_message_) {
        throw std::invalid_argument("ZMQ subscriber requires endpoint and handler");
    }
}

ZmqSubscriber::~ZmqSubscriber() { stop(); }

bool ZmqSubscriber::available() const noexcept { return api_ && api_->loaded(); }
bool ZmqSubscriber::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void ZmqSubscriber::start() {
    if (!available()) {
        throw std::runtime_error("libzmq is unavailable");
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    thread_ = std::jthread([this](std::stop_token) { run(); });
}

void ZmqSubscriber::stop() noexcept {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running && api_ && api_->ctx_shutdown != nullptr) {
        if (void *context = api_->context.load(std::memory_order_acquire);
            context != nullptr) {
            (void)api_->ctx_shutdown(context);
        }
    }
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void ZmqSubscriber::report(std::string_view message) noexcept {
    if (!on_error_) {
        return;
    }
    try {
        on_error_(message);
    }
    catch (...) {
    }
}

void ZmqSubscriber::run() noexcept {
    try {
        void *context = api_->ctx_new();
        api_->context.store(context, std::memory_order_release);
        if (context == nullptr) {
            report(api_->last_error());
            running_.store(false, std::memory_order_release);
            return;
        }
        api_->subscriber = api_->socket(context, kZmqSubscribeSocket);
        if (api_->subscriber == nullptr) {
            report(api_->last_error());
            running_.store(false, std::memory_order_release);
            return;
        }
        const int linger = 0;
        const int timeout = 500;
        static constexpr char topic[] = "json-minimal-chain_main";
        if (api_->set_sock_opt(api_->subscriber, kZmqLinger, &linger, sizeof(linger)) != 0 ||
            api_->set_sock_opt(api_->subscriber, kZmqReceiveTimeout, &timeout,
                               sizeof(timeout)) != 0 ||
            api_->set_sock_opt(api_->subscriber, kZmqSubscribe, topic,
                               sizeof(topic) - 1U) != 0 ||
            api_->connect(api_->subscriber, endpoint_.c_str()) != 0) {
            report(api_->last_error());
            running_.store(false, std::memory_order_release);
            return;
        }

        std::array<char, kMaxMessage> buffer{};
        while (running_.load(std::memory_order_acquire)) {
            const int count = api_->receive(api_->subscriber, buffer.data(), buffer.size(), 0);
            if (count < 0) {
                const int code = api_->error_number();
                if (code == EAGAIN || code == EINTR) {
                    continue;
                }
                if (running_.load(std::memory_order_acquire)) {
                    report(api_->last_error());
                }
                break;
            }
            const auto size = static_cast<std::size_t>(count);
            if (size > buffer.size()) {
                report("ZMQ notification exceeded the receive bound");
                continue;
            }
            try {
                on_message_(std::string_view(buffer.data(), size));
            }
            catch (...) {
                report("ZMQ notification handler failed");
            }
        }
    }
    catch (const std::exception &error) {
        report(error.what());
    }
    running_.store(false, std::memory_order_release);
}

} // namespace monero_solo
