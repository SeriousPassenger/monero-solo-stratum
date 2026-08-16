#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
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
    job_public_id,
    share_public_id,
    submission_id,
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
    agent_profile,
    seed_hash,
    target,
    target_encoding,
    fetch_reason,
    assigned_difficulty,
    network_difficulty,
    actual_difficulty,
    credited_difficulty,
    nonce,
    claimed_hash,
    computed_hash,
    provenance,
    key_count,
};

// Sensitive fields have a separate closed key space and an explicit logger
// construction gate. They can never be written to stderr, and their values
// must use the exact bounded encoding required by the key.
enum class SensitiveHexKey : std::uint8_t {
    private_job_entropy,
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
    verifier_queue_ns,
    verifier_hash_ns,
    verifier_total_ns,
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

struct SensitiveHexField {
    SensitiveHexKey key{};
    std::string_view value;
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
    static constexpr std::size_t max_public_string_bytes = 512;
    static constexpr std::size_t max_fields = 24;
    static constexpr std::size_t max_record_bytes = 16384;

    explicit Logger(Severity threshold,
                    const std::optional<std::string> &file = std::nullopt,
                    bool allow_sensitive = false);
    ~Logger() noexcept;

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    [[nodiscard]] bool enabled(Severity severity) const noexcept;

    // These are the only post-construction write operations. They are
    // noexcept, bounded, serialize a complete JSONL record under one mutex,
    // and never accept arbitrary JSON, byte arrays, or free-form field names.
    void log(Severity severity,
             std::string_view stable_code,
             std::initializer_list<PublicStringField> strings = {},
             std::initializer_list<IntegerField> integers = {},
             std::initializer_list<SensitiveHexField> sensitive = {}) noexcept;

    // Equivalent bounded typed-field entry point for callers that need to
    // conditionally omit a field (for example, a durable ID that does not
    // exist for transient-only work).
    void log_fields(Severity severity,
                    std::string_view stable_code,
                    std::span<const PublicStringField> strings,
                    std::span<const IntegerField> integers,
                    std::span<const SensitiveHexField> sensitive = {}) noexcept;

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
    bool allow_sensitive_{};
    mutable std::mutex mutex_;
    std::array<char, max_record_bytes> record_buffer_{};
    std::atomic<std::uint64_t> written_{};
    std::atomic<std::uint64_t> filtered_{};
    std::atomic<std::uint64_t> rejected_{};
    std::atomic<std::uint64_t> failed_{};
    std::atomic<int> last_error_{};
};

} // namespace monero_solo::logging
