#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace monero_solo::logging {

enum class Severity : std::uint8_t {
    error = 0,
    warning = 1,
    info = 2,
    debug = 3,
    trace = 4,
};

// String keys are intentionally closed rather than caller-provided.  This
// keeps log records to public operational/correlation data and prevents a
// convenient generic "message" or "data" field from becoming a secret/blob
// escape hatch.  Values must be printable ASCII and are bounded by Logger.
enum class PublicStringKey : std::uint8_t {
    component,
    session_id,
    connection_public_id,
    worker_public_id,
    template_public_id,
    job_id,
    share_public_id,
    candidate_key,
    round_public_id,
    ban_public_id,
    delivery_public_id,
    network,
    wallet_address,
    listener,
    daemon_endpoint,
    zmq_endpoint,
    mode,
    state,
    status,
    reason_code,
    peer,
    key_count,
};

enum class IntegerKey : std::uint8_t {
    connection_id,
    worker_id,
    job_id,
    template_id,
    share_id,
    candidate_id,
    round_id,
    ban_id,
    delivery_id,
    height,
    attempt,
    duration_us,
    count,
    bytes,
    queue_items,
    queue_bytes,
    error_number,
    exit_code,
    signal_number,
    difficulty,
    generation,
    seed_id,
    thread_index,
    sequence,
    listener_count,
    reconciliation_id,
    cycle,
    key_count,
};

struct PublicStringField {
    PublicStringKey key{};
    std::string_view value;
};

struct IntegerField {
    IntegerKey key{};
    std::uint64_t value{};
};

class LoggerError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Severity parse_severity(std::string_view text);
[[nodiscard]] std::string_view severity_name(Severity severity) noexcept;
// Performs the same parent/target trust checks used by Logger without opening,
// creating, truncating, chmodding, or otherwise mutating the configured path.
void validate_file_configuration(
    const std::optional<std::string> &file = std::nullopt);

class Logger final {
  public:
    static constexpr std::size_t max_code_bytes = 64;
    static constexpr std::size_t max_public_string_bytes = 256;
    static constexpr std::size_t max_fields = 16;
    static constexpr std::size_t max_record_bytes = 16384;

    explicit Logger(Severity threshold,
                    const std::optional<std::string> &file = std::nullopt);
    ~Logger() noexcept;

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    [[nodiscard]] bool enabled(Severity severity) const noexcept;

    // This is the only post-construction write operation.  It is noexcept,
    // bounded, serializes a complete JSONL record under one mutex, and never
    // accepts arbitrary JSON, byte arrays, or free-form field names.
    void log(Severity severity,
             std::string_view stable_code,
             std::initializer_list<PublicStringField> strings = {},
             std::initializer_list<IntegerField> integers = {}) noexcept;

    [[nodiscard]] Severity threshold() const noexcept { return threshold_; }
    [[nodiscard]] bool file_backed() const noexcept { return owns_fd_; }
    [[nodiscard]] std::uint64_t written_records() const noexcept;
    [[nodiscard]] std::uint64_t filtered_records() const noexcept;
    [[nodiscard]] std::uint64_t rejected_records() const noexcept;
    [[nodiscard]] std::uint64_t failed_records() const noexcept;
    [[nodiscard]] int last_error() const noexcept;

  private:
    void note_failure(int error_number) noexcept;

    Severity threshold_;
    int fd_{-1};
    bool owns_fd_{};
    mutable std::mutex mutex_;
    std::array<char, max_record_bytes> record_buffer_{};
    std::atomic<std::uint64_t> written_{};
    std::atomic<std::uint64_t> filtered_{};
    std::atomic<std::uint64_t> rejected_{};
    std::atomic<std::uint64_t> failed_{};
    std::atomic<int> last_error_{};
};

} // namespace monero_solo::logging
