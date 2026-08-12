#include "monero_solo/config.hpp"
#include "monero_solo/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace monero_solo::detail {

using JobPostCommitFaultHook = void (*)();
void set_job_post_commit_fault_hook_for_testing(
    JobPostCommitFaultHook hook) noexcept;

} // namespace monero_solo::detail

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::string_view kWalletAddress =
    "44AFFq5kSiGBoZ4NMDwYtN18obc8AemS33DBLWs3H7otXft3XjrpDtQGv7SqSsa"
    "BYBb98uNbr2VBBEt7f2wfn3RVGQBEP3A";
constexpr std::string_view kTemplateBlob =
    "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
    "1a1b1c1d1e1fa1b2c3d402f685e40101ffba85e4010180e0a596bb1103404142"
    "434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f7e3301"
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
    "0210000000000000000000000000000000000004202122232425262728292a2b2c"
    "2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c"
    "4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c"
    "6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c"
    "8d8e8f909192939495969798999a9b9c9d9e9f";
constexpr std::string_view kHashingBlob =
    "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
    "1a1b1c1d1e1fa1b2c3d45ec8d04ebe937c6760b1e165cbc49a9a5b52d9c5308b"
    "56a090ddb029c8c719a905";
constexpr std::string_view kPreviousHash =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr std::string_view kSeedHash =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::uint64_t kHeight = 3'736'250;
constexpr std::size_t kReservedOffset = 131;

void require(const bool condition, const char *const message)
{
    if (!condition) throw std::runtime_error(message);
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

void send_all(const int descriptor, const std::string_view bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(descriptor, bytes.data() + offset,
                                    bytes.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        require(sent > 0, "test socket write failed");
        offset += static_cast<std::size_t>(sent);
    }
}

bool send_all_noexcept(const int descriptor, const std::string_view bytes) noexcept
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(descriptor, bytes.data() + offset,
                                    bytes.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string read_line(const int descriptor)
{
    std::string line;
    for (;;) {
        char byte{};
        const ssize_t received = ::recv(descriptor, &byte, 1, 0);
        if (received < 0 && errno == EINTR) continue;
        require(received == 1, "test socket line read failed");
        if (byte == '\n') return line;
        if (byte != '\r') line.push_back(byte);
    }
}

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

FileDescriptor connect_tcp(const std::uint16_t port)
{
    FileDescriptor client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(client.get() >= 0, "could not create TCP client");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(client.get(), reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)) == 0,
            "could not connect to loopback service");
    timeval timeout{5, 0};
    (void)::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout));
    return client;
}

class MockDaemon final {
public:
    MockDaemon()
    {
        listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        require(listener_.get() >= 0, "could not create mock daemon listener");
        int reuse = 1;
        (void)::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                           sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener_.get(), reinterpret_cast<sockaddr *>(&address),
                       sizeof(address)) == 0,
                "could not bind mock daemon listener");
        require(::listen(listener_.get(), 16) == 0,
                "could not listen for mock daemon requests");
        socklen_t length = sizeof(address);
        require(::getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&address),
                              &length) == 0,
                "could not inspect mock daemon listener");
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this](std::stop_token token) { serve(token); });
    }

    MockDaemon(const MockDaemon &) = delete;
    MockDaemon &operator=(const MockDaemon &) = delete;

    ~MockDaemon() { stop(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void reject_template(const bool reject) noexcept
    {
        reject_template_.store(reject, std::memory_order_release);
    }

    void omit_status_for(const std::string_view method)
    {
        int fault = 0;
        if (method == "get_info") fault = 1;
        else if (method == "getblocktemplate") fault = 2;
        else if (!method.empty()) throw std::invalid_argument("invalid status-fault method");
        status_fault_.store(fault, std::memory_order_release);
    }

    [[nodiscard]] unsigned calls(const std::string &method) const
    {
        std::lock_guard lock(mutex_);
        const auto found = calls_.find(method);
        return found == calls_.end() ? 0U : found->second;
    }

    [[nodiscard]] std::optional<std::string> failure() const
    {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    static std::optional<std::size_t> content_length(const std::string_view headers)
    {
        std::size_t cursor = 0;
        while (cursor < headers.size()) {
            const auto end = headers.find("\r\n", cursor);
            const std::string_view line = headers.substr(
                cursor, (end == std::string_view::npos ? headers.size() : end) - cursor);
            constexpr std::string_view prefix = "Content-Length:";
            if (line.size() >= prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), line.begin(),
                           [](const char left, const char right) {
                               return static_cast<char>(std::tolower(
                                          static_cast<unsigned char>(left))) ==
                                      static_cast<char>(std::tolower(
                                          static_cast<unsigned char>(right)));
                           })) {
                std::string_view value = line.substr(prefix.size());
                while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                std::size_t parsed = 0;
                if (value.empty() ||
                    std::from_chars(value.data(), value.data() + value.size(), parsed).ec !=
                        std::errc{}) {
                    return std::nullopt;
                }
                return parsed;
            }
            if (end == std::string_view::npos) break;
            cursor = end + 2U;
        }
        return std::nullopt;
    }

    void record_failure(std::string message) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!failure_.has_value()) failure_ = std::move(message);
    }

    Json response_for(const Json &request)
    {
        if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
            !request.contains("id") || !request.at("id").is_number_integer() ||
            !request.contains("method") || !request.at("method").is_string() ||
            !request.contains("params")) {
            throw std::runtime_error("mock daemon received malformed JSON-RPC");
        }
        const std::string method = request.at("method").get<std::string>();
        {
            std::lock_guard lock(mutex_);
            ++calls_[method];
        }
        Json result;
        if (method == "get_info") {
            if (!request.at("params").is_object()) {
                throw std::runtime_error("get_info parameters were not an object");
            }
            result = {{"status", "OK"}, {"nettype", "mainnet"}};
            if (status_fault_.load(std::memory_order_acquire) == 1) {
                result.erase("status");
            }
        }
        else if (method == "getblocktemplate") {
            const Json &params = request.at("params");
            if (!params.is_object() ||
                params.value("wallet_address", "") != kWalletAddress ||
                params.value("reserve_size", 0) != 16) {
                throw std::runtime_error("runtime sent wrong template parameters");
            }
            result = {
                {"status", "OK"},
                {"blocktemplate_blob", kTemplateBlob},
                {"blockhashing_blob", kHashingBlob},
                {"height", kHeight},
                {"prev_hash", reject_template_.load(std::memory_order_acquire)
                                  ? std::string(64, 'f')
                                  : std::string(kPreviousHash)},
                {"seed_hash", kSeedHash},
                // This is monerod's normal wire representation when the
                // current and next RandomX seed do not differ.
                {"next_seed_hash", ""},
                {"reserved_offset", kReservedOffset},
                {"wide_difficulty", "ffffffffffffffffffffffffffffffff"},
            };
            if (status_fault_.load(std::memory_order_acquire) == 2) {
                result.erase("status");
            }
        }
        else if (method == "submitblock") {
            const Json &params = request.at("params");
            if (!params.is_array() || params.size() != 1U ||
                !params.at(0).is_string() || params.at(0).get<std::string>().empty()) {
                throw std::runtime_error("runtime sent malformed submitblock parameters");
            }
            result = {{"status", "OK"}};
        }
        else {
            throw std::runtime_error("runtime called unexpected daemon method: " + method);
        }
        return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", result}};
    }

    void handle(const int descriptor) noexcept
    {
        try {
            timeval timeout{5, 0};
            (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                               sizeof(timeout));
            std::string wire;
            std::size_t header_end = std::string::npos;
            std::optional<std::size_t> length;
            while (wire.size() < 128U * 1024U) {
                if (header_end != std::string::npos && length.has_value() &&
                    wire.size() >= header_end + *length) {
                    break;
                }
                char buffer[4096];
                const ssize_t received = ::recv(descriptor, buffer, sizeof(buffer), 0);
                if (received < 0 && errno == EINTR) continue;
                if (received <= 0) throw std::runtime_error("incomplete daemon HTTP request");
                wire.append(buffer, static_cast<std::size_t>(received));
                if (header_end == std::string::npos) {
                    const auto marker = wire.find("\r\n\r\n");
                    if (marker != std::string::npos) {
                        header_end = marker + 4U;
                        length = content_length(std::string_view(wire).substr(0, marker));
                        if (!length.has_value()) {
                            throw std::runtime_error("daemon request omitted Content-Length");
                        }
                    }
                }
            }
            if (header_end == std::string::npos || !length.has_value() ||
                wire.size() < header_end + *length) {
                throw std::runtime_error("oversized or incomplete daemon HTTP request");
            }
            const Json request = Json::parse(
                std::string_view(wire).substr(header_end, *length));
            const std::string body = response_for(request).dump();
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Connection: close\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            if (!send_all_noexcept(descriptor, response)) {
                throw std::runtime_error("mock daemon response write failed");
            }
        }
        catch (const std::exception &error) {
            record_failure(error.what());
        }
    }

    void serve(const std::stop_token token) noexcept
    {
        while (!token.stop_requested()) {
            const int accepted = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
            if (accepted < 0) {
                if (errno == EINTR) continue;
                if (!token.stop_requested()) record_failure("mock daemon accept failed");
                break;
            }
            FileDescriptor client(accepted);
            handle(client.get());
        }
    }

    void stop() noexcept
    {
        if (!thread_.joinable()) return;
        thread_.request_stop();
        if (listener_.get() >= 0) (void)::shutdown(listener_.get(), SHUT_RDWR);
        thread_.join();
        listener_.reset();
    }

    FileDescriptor listener_;
    std::uint16_t port_{};
    std::atomic<bool> reject_template_{false};
    std::atomic<int> status_fault_{0};
    mutable std::mutex mutex_;
    std::map<std::string, unsigned> calls_;
    std::optional<std::string> failure_;
    std::jthread thread_;
};

struct HttpResult {
    int status{};
    Json document;
};

HttpResult http_get(const std::uint16_t port, const std::string_view target)
{
    FileDescriptor client = connect_tcp(port);
    const std::string request = "GET " + std::string(target) +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send_all(client.get(), request);
    std::string response;
    for (;;) {
        char buffer[4096];
        const ssize_t received = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received == 0) break;
        require(received > 0, "API response read failed");
        response.append(buffer, static_cast<std::size_t>(received));
    }
    const auto first_space = response.find(' ');
    const auto second_space = first_space == std::string::npos
                                  ? std::string::npos
                                  : response.find(' ', first_space + 1U);
    const auto body = response.find("\r\n\r\n");
    require(first_space != std::string::npos && second_space != std::string::npos &&
                body != std::string::npos,
            "API returned malformed HTTP");
    return {std::stoi(response.substr(first_space + 1U,
                                      second_space - first_space - 1U)),
            Json::parse(response.substr(body + 4U))};
}

struct LoggedInClient {
    FileDescriptor socket;
    Json response;
};

LoggedInClient login_client(const std::uint16_t port,
                            const std::string_view password = {})
{
    FileDescriptor client = connect_tcp(port);
    const Json request = {
        {"id", 1},
        {"jsonrpc", "2.0"},
        {"method", "login"},
        {"params", {{"login", "runtime-test"}, {"pass", password},
                    {"agent", "runtime-e2e/1"}, {"rigid", "loopback"},
                    {"algo", Json::array({"rx/0"})}}},
    };
    send_all(client.get(), request.dump() + "\n");
    Json response = Json::parse(read_line(client.get()));
    require(response.at("error").is_null(), "Stratum login failed");
    return {std::move(client), std::move(response)};
}

class TemporaryDatabase final {
public:
    explicit TemporaryDatabase(std::string tag)
    {
        path_ = std::filesystem::temp_directory_path() /
            ("mss-runtime-" + std::to_string(::getpid()) + "-" + std::move(tag) +
             ".sqlite3");
        remove();
    }

    TemporaryDatabase(const TemporaryDatabase &) = delete;
    TemporaryDatabase &operator=(const TemporaryDatabase &) = delete;
    ~TemporaryDatabase() { remove(); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
    void remove() noexcept
    {
        std::error_code ignored;
        (void)std::filesystem::remove(path_, ignored);
        (void)std::filesystem::remove(path_.string() + "-wal", ignored);
        (void)std::filesystem::remove(path_.string() + "-shm", ignored);
        (void)std::filesystem::remove(path_.string() + ".lock", ignored);
        (void)std::filesystem::remove(path_.string() + ".jsonl", ignored);
    }

    std::filesystem::path path_;
};

monero_solo::Config config_for(const MockDaemon &daemon,
                               const std::uint16_t stratum_port,
                               const std::uint16_t api_port,
                               const std::filesystem::path &database)
{
    const Json document = {
        {"schema_version", 1},
        {"network", "mainnet"},
        {"wallet_address", kWalletAddress},
        {"blocknotify", nullptr},
        {"stratum", {{"listen", Json::array({
                         "127.0.0.1:" + std::to_string(stratum_port)})},
                     {"access_password", nullptr}}},
        {"daemon", {{"rpc_url", "http://127.0.0.1:" +
                                    std::to_string(daemon.port())},
                    {"zmq_address", nullptr}, {"poll_interval_ms", 300000},
                    {"request_timeout_ms", 5000}}},
        {"difficulty", {{"mode", "fixed"}, {"value", 1}}},
        {"verifier", {{"enabled", false}}},
        {"entropy", Json::object()},
        {"database", {{"path", database.string()}}},
        {"events", {{"enabled", false}}},
        {"api", {{"enabled", true},
                 {"listen", "127.0.0.1:" + std::to_string(api_port)}}},
        {"defense", {{"enabled", false}}},
        {"logging", Json::object()},
    };
    return monero_solo::parse_config_json(
        document.dump(), {.validate_paths = false,
                          .validate_blocknotify_executable = false});
}

std::int64_t sqlite_scalar(const std::filesystem::path &path,
                           const std::string_view query)
{
    sqlite3 *raw = nullptr;
    require(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "could not open runtime test database");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, &sqlite3_close);
    sqlite3_stmt *statement_raw = nullptr;
    require(sqlite3_prepare_v2(database.get(), query.data(),
                               static_cast<int>(query.size()), &statement_raw,
                               nullptr) == SQLITE_OK,
            "could not prepare runtime database assertion");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
        statement_raw, &sqlite3_finalize);
    require(sqlite3_step(statement.get()) == SQLITE_ROW,
            "runtime database assertion returned no row");
    return sqlite3_column_int64(statement.get(), 0);
}

void throw_after_job_commit()
{
    throw std::runtime_error("injected post-commit job handoff failure");
}

void test_post_commit_job_rollback()
{
    MockDaemon daemon;
    TemporaryDatabase database("job-rollback");
    const std::uint16_t stratum_port = unused_tcp_port();
    std::uint16_t api_port = unused_tcp_port();
    while (api_port == stratum_port) api_port = unused_tcp_port();
    monero_solo::Config config = config_for(
        daemon, stratum_port, api_port, database.path());

    {
        monero_solo::Runtime runtime(config);
        runtime.start();
        require(runtime.running() && runtime.ready(),
                "fault-injection runtime did not become ready");

        monero_solo::detail::set_job_post_commit_fault_hook_for_testing(
            &throw_after_job_commit);
        FileDescriptor miner = connect_tcp(stratum_port);
        const Json login = {
            {"id", 1},
            {"jsonrpc", "2.0"},
            {"method", "login"},
            {"params", {{"login", "rollback-test"}, {"pass", ""},
                        {"agent", "runtime-rollback/1"},
                        {"algo", Json::array({"rx/0"})}}},
        };
        send_all(miner.get(), login.dump() + "\n");
        const Json response = Json::parse(read_line(miner.get()));
        require(response.at("result").is_null() &&
                    response.at("error").at("message") == "Server busy",
                "post-commit job fault was not rejected safely");
        for (unsigned wait = 0; wait < 200U && runtime.running(); ++wait) {
            std::this_thread::sleep_for(5ms);
        }
        require(!runtime.running() && !runtime.ready(),
                "post-commit job fault did not fail-stop the runtime");
        miner.reset();
        runtime.stop();
    }
    monero_solo::detail::set_job_post_commit_fault_hook_for_testing(nullptr);

    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM private_jobs") == 1,
            "post-commit fault did not exercise a durable private job");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM private_jobs "
                          "WHERE retired_unix_us IS NULL") == 0,
            "post-commit fault stranded an active durable private job");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM duplicate_keys WHERE active=1") == 0,
            "post-commit fault leaked durable duplicate ownership");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM server_sessions "
                          "WHERE clean_shutdown=0 AND stopped_unix_us IS NOT NULL") == 1,
            "post-commit job fault was incorrectly recorded as a clean session");
}

void test_runtime_end_to_end()
{
    MockDaemon daemon;
    TemporaryDatabase database("good");
    const std::uint16_t stratum_port = unused_tcp_port();
    std::uint16_t api_port = unused_tcp_port();
    while (api_port == stratum_port) api_port = unused_tcp_port();
    monero_solo::Config config = config_for(
        daemon, stratum_port, api_port, database.path());
    const std::string log_path = database.path().string() + ".jsonl";
    const std::string configured_secret = "runtime-stratum-secret-1";
    {
        std::error_code ignored;
        (void)std::filesystem::remove(log_path, ignored);
    }
    config.logging.file = log_path;
    config.stratum.access_password = configured_secret;

    std::string first_job_id;
    {
        monero_solo::Runtime runtime(config);
        runtime.start();
        require(runtime.running() && runtime.ready(),
                "runtime did not become ready");

        const HttpResult ready = http_get(api_port, "/v1/health/ready");
        require(ready.status == 200 && ready.document.at("data").at("ready") == true &&
                    ready.document.at("data").at("height") == kHeight,
                "runtime readiness API was not healthy");

        LoggedInClient miner = login_client(stratum_port, configured_secret);
        const Json &login_result = miner.response.at("result");
        const Json &job = login_result.at("job");
        const std::string connection_id = login_result.at("id").get<std::string>();
        first_job_id = job.at("job_id").get<std::string>();
        require(connection_id.size() == 32U && first_job_id.size() == 32U &&
                    job.at("height") == kHeight && job.at("seed_hash") == kSeedHash &&
                    job.at("target") == "ffffffffffffffff" &&
                    job.at("blob").get<std::string>() != kHashingBlob,
                "runtime did not issue the expected private Stratum job");

        const std::string claimed_hash =
            std::string(48, 'f') + "feffffffffffffff";
        const Json submit = {
            {"id", "share-1"},
            {"jsonrpc", "2.0"},
            {"method", "submit"},
            {"params", {{"id", connection_id}, {"job_id", first_job_id},
                        {"nonce", "01020304"}, {"result", claimed_hash},
                        {"algo", "rx/0"}}},
        };
        send_all(miner.socket.get(), submit.dump() + "\n");
        const Json accepted = Json::parse(read_line(miner.socket.get()));
        require(accepted.at("error").is_null() &&
                    accepted.at("result").at("status") == "OK",
                "trusted ordinary share was not accepted");

        const HttpResult shares = http_get(
            api_port, "/v1/shares?status=accepted&limit=10");
        require(shares.status == 200 && shares.document.at("data").size() == 1U,
                "accepted share was not visible through the API");
        const Json &share = shares.document.at("data").at(0);
        require(share.at("status") == "accepted" &&
                    share.at("provenance") == "claimed" &&
                    share.at("claimed_hash") == claimed_hash &&
                    share.at("claimed_meets_share_target") == true &&
                    share.at("claimed_meets_network_target") == false &&
                    share.at("credited_difficulty") == "1",
                "persisted trusted share fields were wrong");

        const auto submit_candidate = [&](std::string_view request_id,
                                          std::string_view claimed_hash) {
            const Json candidate_submit = {
                {"id", request_id},
                {"jsonrpc", "2.0"},
                {"method", "submit"},
                {"params", {{"id", connection_id}, {"job_id", first_job_id},
                            {"nonce", "05060708"}, {"result", claimed_hash},
                            {"algo", "rx/0"}}},
            };
            send_all(miner.socket.get(), candidate_submit.dump() + "\n");
            return Json::parse(read_line(miner.socket.get()));
        };
        const Json first_candidate = submit_candidate(
            "candidate-1", std::string(64, '0'));
        require(first_candidate.at("error").is_null() &&
                    first_candidate.at("result").at("status") == "OK",
                "first trusted network candidate did not receive ordinary credit");
        const Json rotated_replay = submit_candidate(
            "candidate-2", "0100000000000000000000000000000000000000000000000000000000000000");
        require(rotated_replay.at("result").is_null() &&
                    rotated_replay.at("error").at("message") == "Duplicate share",
                "rotating the claimed hash bypassed frozen-candidate replay detection");
        for (unsigned attempt = 0; attempt < 100U &&
                                   daemon.calls("submitblock") != 1U;
             ++attempt) {
            std::this_thread::sleep_for(10ms);
        }
        require(daemon.calls("submitblock") == 1U,
                "frozen-candidate replay created another daemon sequence");
        miner.socket.reset();
        runtime.stop();
        require(!runtime.running() && !runtime.ready(),
                "runtime did not stop cleanly");
    }
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM duplicate_keys WHERE active=1") == 0,
            "clean job retirement leaked an active durable duplicate key");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM private_jobs "
                          "WHERE retired_unix_us IS NULL") == 0,
            "clean shutdown left a private job active");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM connections "
                          "WHERE closed_unix_us IS NULL") == 0,
            "clean shutdown left a connection active");

    {
        monero_solo::Runtime runtime(config);
        runtime.start();
        require(runtime.running() && runtime.ready(),
                "runtime did not become ready after restart");
        const HttpResult recovered = http_get(
            api_port, "/v1/shares?status=accepted&limit=10");
        require(recovered.status == 200 && recovered.document.at("data").size() == 2U &&
                    recovered.document.at("data").at(0).at("job_id") == first_job_id,
                "persisted accepted share was not recovered after restart");
        LoggedInClient miner = login_client(stratum_port, configured_secret);
        require(miner.response.at("result").at("job").at("job_id") != first_job_id,
                "restart reused a private job identity");
        miner.socket.reset();
        runtime.stop();
    }

    std::set<std::string> log_codes;
    {
        std::ifstream input(log_path);
        require(static_cast<bool>(input), "runtime JSONL log was not created");
        std::string encoded;
        std::string complete_log;
        while (std::getline(input, encoded)) {
            complete_log += encoded;
            complete_log.push_back('\n');
            const Json record = Json::parse(encoded);
            require(record.size() == 4U && record.at("time").is_string() &&
                        record.at("severity").is_string() &&
                        record.at("code").is_string() &&
                        record.at("fields").is_object(),
                    "runtime JSONL log record has an invalid envelope");
            log_codes.insert(record.at("code").get<std::string>());
        }
        require(complete_log.find(configured_secret) == std::string::npos,
                "runtime JSONL log leaked a configured secret");
    }
    for (const std::string_view code : {
             "runtime.starting", "verifier.disabled", "daemon.ready",
             "template.refreshed", "runtime.ready", "candidate.journaled",
             "candidate.attempt_started", "candidate.attempt_completed",
             "candidate.terminal", "runtime.stopping", "runtime.stopped"}) {
        require(log_codes.contains(std::string(code)),
                "runtime JSONL log omitted a required lifecycle code");
    }
    {
        std::error_code ignored;
        (void)std::filesystem::remove(log_path, ignored);
    }

    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM server_sessions") == 2,
            "restart did not persist two sessions");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM server_sessions "
                          "WHERE clean_shutdown=1 AND stopped_unix_us IS NOT NULL") == 2,
            "clean shutdown was not persisted for both sessions");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM public_templates") == 2,
            "startup template was not durably persisted per session");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM shares WHERE status='accepted' "
                          "AND provenance='claimed'") == 2,
            "trusted accepted share did not survive restart");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM candidates") == 1 &&
                sqlite_scalar(database.path(),
                              "SELECT count(*) FROM candidate_attempts") == 1 &&
                sqlite_scalar(database.path(),
                              "SELECT count(*) FROM shares WHERE status='duplicate'") == 1,
            "trusted frozen-candidate replay was not journaled idempotently");
    require(sqlite_scalar(database.path(),
                          "SELECT count(*) FROM duplicate_keys WHERE active=1") == 0,
            "restart leaked an ordinary durable duplicate key");

    TemporaryDatabase rejected_database("rejected");
    daemon.reject_template(true);
    const monero_solo::Config rejected = config_for(
        daemon, stratum_port, api_port, rejected_database.path());
    bool mismatch_rejected = false;
    try {
        monero_solo::Runtime runtime(rejected);
        runtime.start();
    }
    catch (const monero_solo::ValidationError &error) {
        mismatch_rejected =
            std::string_view(error.what()).find("previous hash") != std::string_view::npos;
    }
    require(mismatch_rejected,
            "runtime accepted a daemon prev_hash/template mismatch");
    require(sqlite_scalar(rejected_database.path(),
                          "SELECT count(*) FROM public_templates") == 0,
            "rejected template was persisted as valid");
    require(sqlite_scalar(rejected_database.path(),
                          "SELECT count(*) FROM server_sessions "
                          "WHERE clean_shutdown=0 AND stopped_unix_us IS NOT NULL") == 1,
            "failed startup was incorrectly recorded as a clean session");
    daemon.reject_template(false);

    const auto require_missing_status_rejected =
        [&](const std::string_view method, const std::string &tag) {
            TemporaryDatabase fault_database(tag);
            daemon.omit_status_for(method);
            const monero_solo::Config fault = config_for(
                daemon, stratum_port, api_port, fault_database.path());
            bool rejected_status = false;
            try {
                monero_solo::Runtime runtime(fault);
                runtime.start();
            }
            catch (const monero_solo::ValidationError &error) {
                rejected_status = std::string_view(error.what()).find(
                    "non-OK status") != std::string_view::npos;
            }
            daemon.omit_status_for("");
            require(rejected_status,
                    "runtime accepted a daemon result without status");
            require(sqlite_scalar(fault_database.path(),
                                  "SELECT count(*) FROM public_templates") == 0,
                    "status-invalid template was persisted as valid");
        };
    require_missing_status_rejected("get_info", "missing-info-status");
    require_missing_status_rejected("getblocktemplate", "missing-template-status");

    require(daemon.calls("get_info") == 5U &&
                daemon.calls("getblocktemplate") == 4U,
            "runtime startup did not use the expected daemon RPC sequence");
    if (const auto failure = daemon.failure(); failure.has_value()) {
        throw std::runtime_error("mock daemon failure: " + *failure);
    }
}

} // namespace

int main()
{
    try {
        test_post_commit_job_rollback();
        test_runtime_end_to_end();
        std::cout << "runtime integration tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "runtime integration tests failed: " << error.what() << '\n';
        return 1;
    }
}
