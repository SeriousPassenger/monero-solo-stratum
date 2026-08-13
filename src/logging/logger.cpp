#include "monero_solo/logger.hpp"

#include "monero_solo/util.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace monero_solo::logging {
namespace {

class ScopedFd final {
  public:
    explicit ScopedFd(const int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() noexcept
    {
        if (fd_ >= 0) (void)::close(fd_);
    }

    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept
    {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

  private:
    int fd_;
};

class RecordBuilder final {
  public:
    RecordBuilder(char *data, const std::size_t capacity) noexcept
        : data_(data), capacity_(capacity)
    {
    }

    [[nodiscard]] bool append(const std::string_view text) noexcept
    {
        if (text.size() > capacity_ - size_) return false;
        std::memcpy(data_ + size_, text.data(), text.size());
        size_ += text.size();
        return true;
    }

    [[nodiscard]] bool append(const char value) noexcept
    {
        if (size_ == capacity_) return false;
        data_[size_++] = value;
        return true;
    }

    [[nodiscard]] bool append_json_string(const std::string_view value) noexcept
    {
        if (!append('"')) return false;
        for (const char character : value) {
            if (character == '"' || character == '\\') {
                if (!append('\\') || !append(character)) return false;
            } else if (!append(character)) {
                return false;
            }
        }
        return append('"');
    }

    [[nodiscard]] bool append_uint(const std::uint64_t value) noexcept
    {
        char encoded[std::numeric_limits<std::uint64_t>::digits10 + 2]{};
        const auto result = std::to_chars(
            encoded, encoded + sizeof(encoded), value);
        return result.ec == std::errc{} &&
               append(std::string_view(encoded,
                                       static_cast<std::size_t>(result.ptr - encoded)));
    }

    [[nodiscard]] const char *data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    char *data_;
    std::size_t capacity_;
    std::size_t size_{};
};

[[noreturn]] void throw_system_error(const char *operation, const int error_number)
{
    throw LoggerError(std::string(operation) + ": " +
                      std::strerror(error_number));
}

[[nodiscard]] bool valid_severity(const Severity severity) noexcept
{
    return static_cast<std::uint8_t>(severity) <=
           static_cast<std::uint8_t>(Severity::trace);
}

[[nodiscard]] bool valid_code(const std::string_view code) noexcept
{
    if (code.empty() || code.size() > Logger::max_code_bytes ||
        code.front() < 'a' || code.front() > 'z') {
        return false;
    }
    for (const char character : code) {
        const bool lower = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (!lower && !digit && character != '.' && character != '_' &&
            character != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_public_string(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > Logger::max_public_string_bytes) {
        return false;
    }
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character < 0x20U || character > 0x7eU) return false;
    }
    return true;
}

[[nodiscard]] std::string_view key_name(const PublicStringKey key) noexcept
{
    switch (key) {
    case PublicStringKey::component: return "component";
    case PublicStringKey::session_id: return "session_id";
    case PublicStringKey::connection_public_id: return "connection_public_id";
    case PublicStringKey::worker_public_id: return "worker_public_id";
    case PublicStringKey::template_public_id: return "template_public_id";
    case PublicStringKey::job_public_id: return "job_public_id";
    case PublicStringKey::share_public_id: return "share_public_id";
    case PublicStringKey::candidate_key: return "candidate_key";
    case PublicStringKey::round_public_id: return "round_public_id";
    case PublicStringKey::ban_public_id: return "ban_public_id";
    case PublicStringKey::delivery_public_id: return "delivery_public_id";
    case PublicStringKey::network: return "network";
    case PublicStringKey::wallet_address: return "wallet_address";
    case PublicStringKey::listener: return "listener";
    case PublicStringKey::daemon_endpoint: return "daemon_endpoint";
    case PublicStringKey::zmq_endpoint: return "zmq_endpoint";
    case PublicStringKey::mode: return "mode";
    case PublicStringKey::state: return "state";
    case PublicStringKey::status: return "status";
    case PublicStringKey::reason_code: return "reason_code";
    case PublicStringKey::peer: return "peer";
    case PublicStringKey::agent_profile: return "agent_profile";
    case PublicStringKey::seed_hash: return "seed_hash";
    case PublicStringKey::target: return "target";
    case PublicStringKey::target_encoding: return "target_encoding";
    case PublicStringKey::fetch_reason: return "fetch_reason";
    case PublicStringKey::assigned_difficulty: return "assigned_difficulty";
    case PublicStringKey::network_difficulty: return "network_difficulty";
    case PublicStringKey::actual_difficulty: return "actual_difficulty";
    case PublicStringKey::credited_difficulty: return "credited_difficulty";
    case PublicStringKey::nonce: return "nonce";
    case PublicStringKey::claimed_hash: return "claimed_hash";
    case PublicStringKey::computed_hash: return "computed_hash";
    case PublicStringKey::provenance: return "provenance";
    case PublicStringKey::key_count: break;
    }
    return {};
}

[[nodiscard]] std::string_view key_name(const IntegerKey key) noexcept
{
    switch (key) {
    case IntegerKey::connection_id: return "connection_id";
    case IntegerKey::worker_id: return "worker_id";
    case IntegerKey::job_id: return "job_id";
    case IntegerKey::template_id: return "template_id";
    case IntegerKey::share_id: return "share_id";
    case IntegerKey::candidate_id: return "candidate_id";
    case IntegerKey::round_id: return "round_id";
    case IntegerKey::ban_id: return "ban_id";
    case IntegerKey::delivery_id: return "delivery_id";
    case IntegerKey::height: return "height";
    case IntegerKey::attempt: return "attempt";
    case IntegerKey::duration_us: return "duration_us";
    case IntegerKey::count: return "count";
    case IntegerKey::bytes: return "bytes";
    case IntegerKey::queue_items: return "queue_items";
    case IntegerKey::queue_bytes: return "queue_bytes";
    case IntegerKey::error_number: return "error_number";
    case IntegerKey::exit_code: return "exit_code";
    case IntegerKey::signal_number: return "signal_number";
    case IntegerKey::difficulty: return "difficulty";
    case IntegerKey::generation: return "generation";
    case IntegerKey::seed_id: return "seed_id";
    case IntegerKey::thread_index: return "thread_index";
    case IntegerKey::sequence: return "sequence";
    case IntegerKey::listener_count: return "listener_count";
    case IntegerKey::reconciliation_id: return "reconciliation_id";
    case IntegerKey::cycle: return "cycle";
    case IntegerKey::verifier_queue_ns: return "verifier_queue_ns";
    case IntegerKey::verifier_hash_ns: return "verifier_hash_ns";
    case IntegerKey::verifier_total_ns: return "verifier_total_ns";
    case IntegerKey::key_count: break;
    }
    return {};
}

[[nodiscard]] std::string_view key_name(const SensitiveHexKey key) noexcept
{
    switch (key) {
    case SensitiveHexKey::private_job_entropy: return "private_job_entropy";
    case SensitiveHexKey::key_count: break;
    }
    return {};
}

[[nodiscard]] bool valid_sensitive_hex(const SensitiveHexKey key,
                                       const std::string_view value) noexcept
{
    const std::size_t expected =
        key == SensitiveHexKey::private_job_entropy ? 32U : 0U;
    if (expected == 0U || value.size() != expected) return false;
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

[[nodiscard]] bool append_timestamp(RecordBuilder &record) noexcept
{
    struct timespec now{};
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) return false;
    const std::time_t seconds = now.tv_sec;
    struct tm utc{};
    if (::gmtime_r(&seconds, &utc) == nullptr) return false;
    char timestamp[32]{};
    const int size = std::snprintf(
        timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
        utc.tm_min, utc.tm_sec, now.tv_nsec / 1000L);
    return size == 27 && record.append_json_string(
                             std::string_view(timestamp, static_cast<std::size_t>(size)));
}

[[nodiscard]] bool write_all(const int fd, const char *data,
                             const std::size_t size,
                             int &error_number) noexcept
{
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        error_number = written == 0 ? EIO : errno;
        return false;
    }
    return true;
}

} // namespace

Severity parse_severity(const std::string_view text)
{
    if (text == "error") return Severity::error;
    if (text == "warning") return Severity::warning;
    if (text == "info") return Severity::info;
    if (text == "debug") return Severity::debug;
    if (text == "trace") return Severity::trace;
    throw LoggerError("invalid logging severity");
}

std::string_view severity_name(const Severity severity) noexcept
{
    switch (severity) {
    case Severity::error: return "error";
    case Severity::warning: return "warning";
    case Severity::info: return "info";
    case Severity::debug: return "debug";
    case Severity::trace: return "trace";
    }
    return {};
}

void validate_file_configuration(const std::optional<std::string> &file)
{
    if (!file || file->empty()) return;
    if (file->find('\0') != std::string::npos) {
        throw LoggerError("log file path contains NUL");
    }
    validate_log_path(*file);
    const std::filesystem::path path(*file);
    const std::string filename = path.filename().string();
    const std::string parent = path.parent_path().string();
    if (filename.empty() || filename == "." || filename == ".." ||
        parent.empty()) {
        throw LoggerError("log file path does not name a file");
    }

    ScopedFd parent_fd(::open(parent.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent_fd.get() < 0) throw_system_error("could not open log parent", errno);
    struct stat parent_status{};
    if (::fstat(parent_fd.get(), &parent_status) != 0) {
        throw_system_error("could not inspect log parent", errno);
    }
    const uid_t effective_user = ::geteuid();
    const bool trusted_parent_owner = parent_status.st_uid == 0 ||
                                      parent_status.st_uid == effective_user;
    const bool safe_world_writable =
        (parent_status.st_mode & S_IWOTH) == 0 ||
        ((parent_status.st_mode & S_ISVTX) != 0 && parent_status.st_uid == 0);
    if (!S_ISDIR(parent_status.st_mode) || !trusted_parent_owner ||
        !safe_world_writable) {
        throw LoggerError("log parent ownership or mode is unsafe");
    }

    struct stat target{};
    if (::fstatat(parent_fd.get(), filename.c_str(), &target,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(target.st_mode) || target.st_uid != effective_user ||
            target.st_nlink != 1) {
            throw LoggerError(
                "log file must be a regular, singly-linked file owned by the service user");
        }
    }
    else if (errno != ENOENT) {
        throw_system_error("could not inspect log file", errno);
    }
}

Logger::Logger(const Severity threshold,
               const std::optional<std::string> &file,
               const bool allow_sensitive)
    : threshold_(threshold), allow_sensitive_(allow_sensitive)
{
    if (!valid_severity(threshold_)) {
        throw LoggerError("invalid logging threshold");
    }
    if (allow_sensitive_ &&
        (static_cast<std::uint8_t>(threshold_) <
             static_cast<std::uint8_t>(Severity::debug) ||
         !file || file->empty())) {
        throw LoggerError(
            "sensitive logging requires debug or trace and a file target");
    }
    if (!file || file->empty()) {
        fd_ = STDERR_FILENO;
        return;
    }
    validate_file_configuration(file);

    const std::filesystem::path path(*file);
    const std::string filename = path.filename().string();
    const std::string parent = path.parent_path().string();
    if (filename.empty() || filename == "." || filename == ".." || parent.empty()) {
        throw LoggerError("log file path does not name a file");
    }

    ScopedFd parent_fd(::open(parent.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent_fd.get() < 0) throw_system_error("could not open log parent", errno);

    struct stat parent_status{};
    if (::fstat(parent_fd.get(), &parent_status) != 0) {
        throw_system_error("could not inspect log parent", errno);
    }
    const uid_t effective_user = ::geteuid();
    const bool trusted_parent_owner = parent_status.st_uid == 0 ||
                                      parent_status.st_uid == effective_user;
    const bool safe_world_writable =
        (parent_status.st_mode & S_IWOTH) == 0 ||
        ((parent_status.st_mode & S_ISVTX) != 0 && parent_status.st_uid == 0);
    if (!S_ISDIR(parent_status.st_mode) || !trusted_parent_owner ||
        !safe_world_writable) {
        throw LoggerError("log parent ownership or mode is unsafe");
    }

    ScopedFd output(::openat(parent_fd.get(), filename.c_str(),
                             O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC |
                                 O_NOFOLLOW,
                             S_IRUSR | S_IWUSR));
    if (output.get() < 0) throw_system_error("could not open log file", errno);

    struct stat status{};
    if (::fstat(output.get(), &status) != 0) {
        throw_system_error("could not inspect log file", errno);
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != effective_user ||
        status.st_nlink != 1) {
        throw LoggerError("log file must be a regular, singly-linked file owned by the service user");
    }

    struct stat linked_status{};
    if (::fstatat(parent_fd.get(), filename.c_str(), &linked_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        throw_system_error("could not verify log file path", errno);
    }
    if (!S_ISREG(linked_status.st_mode) || linked_status.st_dev != status.st_dev ||
        linked_status.st_ino != status.st_ino) {
        throw LoggerError("log file path changed while it was opened");
    }
    if (::fchmod(output.get(), S_IRUSR | S_IWUSR) != 0) {
        throw_system_error("could not secure log file mode", errno);
    }
    if (::fstat(output.get(), &status) != 0) {
        throw_system_error("could not verify log file mode", errno);
    }
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw LoggerError("log file mode is unsafe");
    }

    fd_ = output.release();
    owns_fd_ = true;
}

Logger::~Logger() noexcept
{
    if (!owns_fd_ || fd_ < 0) return;
    try {
        const std::lock_guard<std::mutex> lock(mutex_);
        (void)::close(fd_);
        fd_ = -1;
    } catch (...) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

bool Logger::enabled(const Severity severity) const noexcept
{
    return valid_severity(severity) &&
           static_cast<std::uint8_t>(severity) <=
               static_cast<std::uint8_t>(threshold_);
}

void Logger::log(const Severity severity,
                 const std::string_view stable_code,
                 const std::initializer_list<PublicStringField> strings,
                 const std::initializer_list<IntegerField> integers,
                 const std::initializer_list<SensitiveHexField> sensitive) noexcept
{
    if (!valid_severity(severity)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!enabled(severity)) {
        filtered_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!valid_code(stable_code) ||
        strings.size() + integers.size() + sensitive.size() > max_fields ||
        (sensitive.size() != 0U &&
         (!allow_sensitive_ ||
          static_cast<std::uint8_t>(severity) <
              static_cast<std::uint8_t>(Severity::debug)))) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::array<bool, static_cast<std::size_t>(PublicStringKey::key_count)>
        seen_strings{};
    for (const PublicStringField &field : strings) {
        const std::size_t key = static_cast<std::size_t>(field.key);
        if (key >= seen_strings.size() || seen_strings[key] ||
            key_name(field.key).empty() || !valid_public_string(field.value)) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen_strings[key] = true;
    }
    std::array<bool, static_cast<std::size_t>(IntegerKey::key_count)>
        seen_integers{};
    for (const IntegerField &field : integers) {
        const std::size_t key = static_cast<std::size_t>(field.key);
        if (key >= seen_integers.size() || seen_integers[key] ||
            key_name(field.key).empty()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen_integers[key] = true;
    }
    std::array<bool, static_cast<std::size_t>(SensitiveHexKey::key_count)>
        seen_sensitive{};
    for (const SensitiveHexField &field : sensitive) {
        const std::size_t key = static_cast<std::size_t>(field.key);
        if (key >= seen_sensitive.size() || seen_sensitive[key] ||
            key_name(field.key).empty() ||
            !valid_sensitive_hex(field.key, field.value)) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen_sensitive[key] = true;
    }

    try {
        const std::lock_guard<std::mutex> lock(mutex_);
        RecordBuilder record(record_buffer_.data(), record_buffer_.size());
        bool valid = record.append("{\"time\":") && append_timestamp(record) &&
                     record.append(",\"severity\":") &&
                     record.append_json_string(severity_name(severity)) &&
                     record.append(",\"code\":") &&
                     record.append_json_string(stable_code) &&
                     record.append(",\"fields\":{");
        bool first = true;
        for (const PublicStringField &field : strings) {
            valid = valid && (first || record.append(',')) &&
                    record.append_json_string(key_name(field.key)) &&
                    record.append(':') && record.append_json_string(field.value);
            first = false;
        }
        for (const IntegerField &field : integers) {
            valid = valid && (first || record.append(',')) &&
                    record.append_json_string(key_name(field.key)) &&
                    record.append(':') && record.append_uint(field.value);
            first = false;
        }
        for (const SensitiveHexField &field : sensitive) {
            valid = valid && (first || record.append(',')) &&
                    record.append_json_string(key_name(field.key)) &&
                    record.append(':') && record.append_json_string(field.value);
            first = false;
        }
        valid = valid && record.append("}}\n");
        if (!valid) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int error_number = 0;
        if (!write_all(fd_, record.data(), record.size(), error_number)) {
            note_failure(error_number == 0 ? EIO : error_number);
            return;
        }
        written_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        note_failure(EIO);
    }
}

std::uint64_t Logger::written_records() const noexcept
{
    return written_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::filtered_records() const noexcept
{
    return filtered_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::rejected_records() const noexcept
{
    return rejected_.load(std::memory_order_relaxed);
}

std::uint64_t Logger::failed_records() const noexcept
{
    return failed_.load(std::memory_order_relaxed);
}

int Logger::last_error() const noexcept
{
    return last_error_.load(std::memory_order_relaxed);
}

void Logger::note_failure(const int error_number) noexcept
{
    failed_.fetch_add(1, std::memory_order_relaxed);
    last_error_.store(error_number, std::memory_order_relaxed);
}

} // namespace monero_solo::logging
