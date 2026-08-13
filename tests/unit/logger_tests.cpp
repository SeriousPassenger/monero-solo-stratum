#include "monero_solo/logger.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using monero_solo::logging::IntegerField;
using monero_solo::logging::IntegerKey;
using monero_solo::logging::Logger;
using monero_solo::logging::PublicStringField;
using monero_solo::logging::PublicStringKey;
using monero_solo::logging::Severity;
using monero_solo::logging::SensitiveHexField;
using monero_solo::logging::SensitiveHexKey;
using Json = nlohmann::json;

void require(const bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
    {
        char pattern[] = "/tmp/monero-solo-logger-test-XXXXXX";
        const char *created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        (void)std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] std::string file(const std::string &name) const
    {
        return (std::filesystem::path(path_) / name).string();
    }

  private:
    std::string path_;
};

class StderrCapture final {
  public:
    StderrCapture()
    {
        int descriptors[2]{};
        if (::pipe(descriptors) != 0) {
            throw std::runtime_error("stderr capture pipe failed");
        }
        read_fd_ = descriptors[0];
        saved_fd_ = ::dup(STDERR_FILENO);
        if (saved_fd_ < 0 || ::dup2(descriptors[1], STDERR_FILENO) < 0) {
            const int saved_error = errno;
            (void)::close(descriptors[0]);
            (void)::close(descriptors[1]);
            if (saved_fd_ >= 0) (void)::close(saved_fd_);
            throw std::runtime_error(std::string("stderr capture setup failed: ") +
                                     std::to_string(saved_error));
        }
        (void)::close(descriptors[1]);
        active_ = true;
    }

    ~StderrCapture() noexcept { restore(); }

    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;

    [[nodiscard]] std::string finish()
    {
        restore_stderr();
        std::string result;
        char buffer[1024]{};
        for (;;) {
            const ssize_t count = ::read(read_fd_, buffer, sizeof(buffer));
            if (count > 0) {
                result.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("stderr capture read failed");
            break;
        }
        (void)::close(read_fd_);
        read_fd_ = -1;
        active_ = false;
        return result;
    }

  private:
    void restore_stderr() noexcept
    {
        if (saved_fd_ >= 0) {
            (void)::dup2(saved_fd_, STDERR_FILENO);
            (void)::close(saved_fd_);
            saved_fd_ = -1;
        }
    }

    void restore() noexcept
    {
        if (!active_) return;
        restore_stderr();
        if (read_fd_ >= 0) (void)::close(read_fd_);
        read_fd_ = -1;
        active_ = false;
    }

    int saved_fd_{-1};
    int read_fd_{-1};
    bool active_{};
};

[[nodiscard]] std::vector<std::string> read_lines(const std::string &path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("could not read logger output");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

void require_timestamp(const std::string &timestamp)
{
    require(timestamp.size() == 27, "timestamp does not have microsecond width");
    require(timestamp[4] == '-' && timestamp[7] == '-' &&
                timestamp[10] == 'T' && timestamp[13] == ':' &&
                timestamp[16] == ':' && timestamp[19] == '.' &&
                timestamp[26] == 'Z',
            "timestamp is not RFC 3339 UTC");
    for (std::size_t index = 0; index < timestamp.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 ||
            index == 16 || index == 19 || index == 26) {
            continue;
        }
        require(timestamp[index] >= '0' && timestamp[index] <= '9',
                "timestamp contains a nondigit component");
    }
}

template <typename Function>
void rejects_construction(Function function, const char *message)
{
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void test_severity_and_json_file()
{
    require(monero_solo::logging::parse_severity("warning") == Severity::warning,
            "severity parser returned wrong value");
    require(monero_solo::logging::severity_name(Severity::trace) == "trace",
            "severity name returned wrong value");
    rejects_construction(
        [] { (void)monero_solo::logging::parse_severity("WARN"); },
        "noncanonical severity was accepted");

    TemporaryDirectory directory;
    const std::string path = directory.file("server.jsonl");
    {
        std::ofstream initial(path);
        initial << "";
    }
    require(::chmod(path.c_str(), 0666) == 0, "could not prepare permissive log mode");

    {
        Logger logger(Severity::info, path);
        require(logger.file_backed(), "file logger was not marked file-backed");
        logger.log(Severity::info, "runtime.start",
                   {{PublicStringKey::component, "runtime"},
                    {PublicStringKey::network, "mainnet"},
                    {PublicStringKey::state, "ready\\\"now"}},
                   {{IntegerKey::height, 123},
                    {IntegerKey::difficulty,
                     std::numeric_limits<std::uint64_t>::max()}});
        logger.log(Severity::debug, "runtime.filtered");
        logger.log(Severity::error, "daemon.unavailable",
                   {{PublicStringKey::reason_code, "transport_error"}});
        require(logger.written_records() == 2, "written record count is wrong");
        require(logger.filtered_records() == 1, "filtered record count is wrong");

        const auto lines = read_lines(path);
        require(lines.size() == 2, "records were not visible without stream flushing");
        const Json first = Json::parse(lines.front());
        require(first.size() == 4 && first["severity"] == "info" &&
                    first["code"] == "runtime.start",
                "structured logger envelope is wrong");
        require_timestamp(first["time"].get<std::string>());
        require(first["fields"]["state"] == "ready\\\"now" &&
                    first["fields"]["height"] == 123 &&
                    first["fields"]["difficulty"] ==
                        std::numeric_limits<std::uint64_t>::max(),
                "structured logger fields did not round-trip");
    }

    struct stat status{};
    require(::stat(path.c_str(), &status) == 0, "could not stat log output");
    require((status.st_mode & 0777) == 0600, "logger did not force mode 0600");

    {
        Logger logger(Severity::info, path);
        logger.log(Severity::warning, "runtime.appended");
    }
    require(read_lines(path).size() == 3, "logger did not append to existing file");
}

void test_bounded_rejection()
{
    TemporaryDirectory directory;
    const std::string path = directory.file("bounds.jsonl");
    Logger logger(Severity::trace, path);

    logger.log(Severity::info, "Bad.code");
    logger.log(Severity::info, "empty.value",
               {{PublicStringKey::state, ""}});
    logger.log(Severity::info, "control.value",
               {{PublicStringKey::state, "line\nbreak"}});
    const std::string oversized(Logger::max_public_string_bytes + 1, 'x');
    logger.log(Severity::info, "large.value",
               {{PublicStringKey::state, oversized}});
    logger.log(Severity::info, "duplicate.field",
               {{PublicStringKey::state, "one"},
                {PublicStringKey::state, "two"}});
    logger.log(Severity::info, "invalid.string.key",
               {{static_cast<PublicStringKey>(255), "value"}});
    logger.log(Severity::info, "invalid.integer.key", {},
               {{static_cast<IntegerKey>(255), 1}});
    logger.log(Severity::info, "too.many.fields", {},
               {{IntegerKey::connection_id, 1}, {IntegerKey::worker_id, 2},
                {IntegerKey::template_id, 3}, {IntegerKey::share_id, 4},
                {IntegerKey::candidate_id, 5}, {IntegerKey::round_id, 6},
                {IntegerKey::ban_id, 7}, {IntegerKey::delivery_id, 8},
                {IntegerKey::height, 9}, {IntegerKey::attempt, 10},
                {IntegerKey::duration_us, 11}, {IntegerKey::count, 12},
                {IntegerKey::bytes, 13}, {IntegerKey::queue_items, 14},
                {IntegerKey::queue_bytes, 15}, {IntegerKey::error_number, 16},
                {IntegerKey::exit_code, 17}, {IntegerKey::signal_number, 18},
                {IntegerKey::difficulty, 19}, {IntegerKey::generation, 20},
                {IntegerKey::seed_id, 21}, {IntegerKey::thread_index, 22},
                {IntegerKey::sequence, 23}, {IntegerKey::listener_count, 24},
                {IntegerKey::reconciliation_id, 25}});
    logger.log(static_cast<Severity>(255), "invalid.severity");
    logger.log(Severity::trace, "valid.record", {},
               {{IntegerKey::count, std::numeric_limits<std::uint64_t>::max()}});

    require(logger.rejected_records() == 9, "invalid record count is wrong");
    require(logger.written_records() == 1, "valid bounded record was not written");
    const auto lines = read_lines(path);
    require(lines.size() == 1, "a rejected record reached the output");
    require(Json::parse(lines.front())["fields"]["count"] ==
                std::numeric_limits<std::uint64_t>::max(),
            "maximum unsigned integer did not round-trip");
}

void test_path_safety()
{
    TemporaryDirectory directory;
    rejects_construction(
        [] { Logger logger(Severity::info, std::string("relative.jsonl")); },
        "relative log path was accepted");

    const std::string target = directory.file("target.jsonl");
    {
        std::ofstream output(target);
        output << "target\n";
    }
    const std::string symlink_path = directory.file("symlink.jsonl");
    require(::symlink(target.c_str(), symlink_path.c_str()) == 0,
            "could not create log symlink fixture");
    rejects_construction(
        [&] { Logger logger(Severity::info, symlink_path); },
        "log symlink was accepted");

    const std::string hardlink_path = directory.file("hardlink.jsonl");
    require(::link(target.c_str(), hardlink_path.c_str()) == 0,
            "could not create log hardlink fixture");
    rejects_construction(
        [&] { Logger logger(Severity::info, hardlink_path); },
        "multiply-linked log file was accepted");
}

void test_stderr_and_failure_accounting()
{
    StderrCapture capture;
    Logger stderr_logger(Severity::info, std::optional<std::string>{""});
    require(!stderr_logger.file_backed(), "empty log path did not select stderr");
    stderr_logger.log(Severity::warning, "stderr.record",
                      {{PublicStringKey::component, "logger"}});
    const std::string output = capture.finish();
    require(stderr_logger.written_records() == 1, "stderr write was not counted");
    const Json record = Json::parse(output);
    require(record["code"] == "stderr.record" &&
                record["severity"] == "warning",
            "stderr JSONL record is wrong");

    const int saved_stderr = ::dup(STDERR_FILENO);
    require(saved_stderr >= 0, "could not save stderr for failure test");
    require(::close(STDERR_FILENO) == 0, "could not close stderr fixture");
    stderr_logger.log(Severity::error, "stderr.closed");
    require(::dup2(saved_stderr, STDERR_FILENO) >= 0,
            "could not restore stderr after failure test");
    (void)::close(saved_stderr);
    require(stderr_logger.failed_records() == 1 &&
                stderr_logger.last_error() == EBADF,
            "write failure was not contained and counted");
}

void test_concurrent_records()
{
    TemporaryDirectory directory;
    const std::string path = directory.file("threads.jsonl");
    Logger logger(Severity::info, path);
    constexpr std::uint64_t thread_count = 6;
    constexpr std::uint64_t records_per_thread = 40;
    std::vector<std::thread> threads;
    for (std::uint64_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {
        threads.emplace_back([&logger, thread_index] {
            for (std::uint64_t sequence = 0; sequence < records_per_thread;
                 ++sequence) {
                logger.log(Severity::info, "concurrency.record",
                           {{PublicStringKey::component, "logger"}},
                           {{IntegerKey::thread_index, thread_index},
                            {IntegerKey::sequence, sequence}});
            }
        });
    }
    for (std::thread &thread : threads) thread.join();

    const std::uint64_t expected = thread_count * records_per_thread;
    require(logger.written_records() == expected,
            "concurrent logger dropped a record");
    require(logger.failed_records() == 0 && logger.rejected_records() == 0,
            "concurrent logger reported an error");
    const auto lines = read_lines(path);
    require(lines.size() == expected, "concurrent records interleaved or vanished");
    std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
    for (const std::string &line : lines) {
        const Json record = Json::parse(line);
        require(record["code"] == "concurrency.record",
                "concurrent record envelope is corrupt");
        require_timestamp(record["time"].get<std::string>());
        identities.emplace(record["fields"]["thread_index"].get<std::uint64_t>(),
                           record["fields"]["sequence"].get<std::uint64_t>());
    }
    require(identities.size() == expected, "concurrent record identity was duplicated");
}

void test_sensitive_file_gate_and_distinct_job_ids()
{
    TemporaryDirectory directory;
    const std::string path = directory.file("sensitive.jsonl");

    rejects_construction(
        [] { Logger logger(Severity::debug, std::nullopt, true); },
        "sensitive stderr logger was accepted");
    rejects_construction(
        [&] { Logger logger(Severity::info, path, true); },
        "sensitive info logger was accepted");

    Logger denied(Severity::debug, path);
    denied.log(Severity::debug, "job.denied", {}, {},
               {{SensitiveHexKey::private_job_entropy,
                 "0123456789abcdef0123456789abcdef"}});
    require(denied.rejected_records() == 1,
            "sensitive field was accepted without the construction gate");

    Logger allowed(Severity::debug, path, true);
    allowed.log(Severity::info, "job.info", {}, {},
                {{SensitiveHexKey::private_job_entropy,
                  "abcdef0123456789abcdef0123456789"}});
    allowed.log(
        Severity::debug, "job.queued",
        {{PublicStringKey::job_public_id,
          "0123456789abcdef0123456789abcdef"}},
        {{IntegerKey::job_id, 42}},
        {{SensitiveHexKey::private_job_entropy,
          "abcdef0123456789abcdef0123456789"}});
    allowed.log(Severity::debug, "job.uppercase", {}, {},
                {{SensitiveHexKey::private_job_entropy,
                  "ABCDEF0123456789abcdef0123456789"}});
    allowed.log(Severity::debug, "job.short", {}, {},
                {{SensitiveHexKey::private_job_entropy, "00"}});
    require(allowed.written_records() == 1 &&
                allowed.rejected_records() == 3,
            "sensitive hex validation counts are wrong");

    const auto lines = read_lines(path);
    require(lines.size() == 1, "unexpected sensitive log record count");
    const Json fields = Json::parse(lines.front()).at("fields");
    require(fields.at("job_public_id") ==
                "0123456789abcdef0123456789abcdef" &&
                fields.at("job_id") == 42 &&
                fields.at("private_job_entropy") ==
                    "abcdef0123456789abcdef0123456789",
            "sensitive or distinct job fields did not round-trip");
}

} // namespace

int main()
{
    try {
        test_severity_and_json_file();
        test_bounded_rejection();
        test_path_safety();
        test_stderr_and_failure_accounting();
        test_concurrent_records();
        test_sensitive_file_gate_and_distinct_job_ids();
        std::cout << "logger tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "logger tests failed: " << error.what() << '\n';
        return 1;
    }
}
