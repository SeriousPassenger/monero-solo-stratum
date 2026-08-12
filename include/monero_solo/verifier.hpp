/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#ifndef MONERO_SOLO_VERIFIER_HPP
#define MONERO_SOLO_VERIFIER_HPP

#include <monero_stratum_pow_verifier.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <functional>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace monero_solo::verifier {

inline constexpr std::size_t kSeedHashSize = 32;
using SeedHash = std::array<std::uint8_t, kSeedHashSize>;
using PowHash = std::array<std::uint8_t, MSPV_HASH_SIZE>;

enum class MemoryMode {
    light,
    fast,
};

enum class LargePages {
    disabled,
    try_enable,
    require,
};

enum class JitMode {
    disabled,
    enabled,
    secure,
};

enum class AesMode {
    automatic,
    software,
};

enum class LogLevel {
    error,
    warning,
    info,
    debug,
    trace,
};

enum class ShutdownMode {
    drain,
    cancel_pending,
};

/*
 * These callbacks execute on verifier-owned threads. They must return quickly,
 * must not throw, and must not call any Verifier member. A wake callback should
 * do no more than signal an event-loop primitive (for example uv_async_send).
 * The adapter catches an accidental exception, but doing so is only a safety
 * net and does not make throwing callbacks supported.
 */
using WakeCallback = std::function<void()>;
using LogCallback = std::function<void(LogLevel, std::string_view)>;

struct Config {
    std::uint32_t workers = 4;
    std::uint32_t seed_init_threads = 4;
    std::uint32_t pending_capacity = 256;
    std::uint32_t max_outstanding = 512;
    std::uint32_t max_input_size = 4096;
    std::uint32_t max_seeds = 2;
    std::uint64_t max_buffered_input_bytes = 16U * 1024U * 1024U;
    MemoryMode memory_mode = MemoryMode::fast;
    LargePages large_pages = LargePages::try_enable;
    JitMode jit = JitMode::secure;
    AesMode aes = AesMode::automatic;
    LogLevel log_level = LogLevel::info;
    WakeCallback wake;
    LogCallback log;
};

/* A callback-free view used by configuration tests and API presentation. */
struct NativeConfigSnapshot {
    std::uint32_t abi_version = 0;
    std::uint32_t worker_count = 0;
    std::uint32_t seed_init_threads = 0;
    std::uint32_t pending_capacity = 0;
    std::uint32_t max_outstanding = 0;
    std::uint32_t max_input_size = 0;
    std::uint32_t max_seed_key_size = 0;
    std::uint32_t max_seeds = 0;
    std::uint64_t max_buffered_input_bytes = 0;
    mspv_memory_mode memory_mode = MSPV_MEMORY_LIGHT;
    mspv_large_page_mode large_pages = MSPV_LARGE_PAGES_DISABLED;
    std::uint32_t options = MSPV_OPTION_NONE;
    mspv_log_level log_level = MSPV_LOG_INFO;
};

class Error final : public std::runtime_error {
public:
    Error(std::string operation,
          mspv_status status,
          mspv_status cleanup_status = MSPV_OK);

    [[nodiscard]] mspv_status status() const noexcept { return status_; }
    [[nodiscard]] mspv_status cleanup_status() const noexcept
    {
        return cleanup_status_;
    }
    [[nodiscard]] const std::string &operation() const noexcept
    {
        return operation_;
    }

private:
    std::string operation_;
    mspv_status status_;
    mspv_status cleanup_status_;
};

struct SeedSnapshot {
    mspv_seed_id seed_id = 0;
    SeedHash seed_hash{};
    mspv_seed_state state = MSPV_SEED_PREPARING;
    mspv_status last_error = MSPV_OK;
    std::uint32_t key_size = 0;
    std::uint32_t queued_jobs = 0;
    std::uint32_t running_jobs = 0;
    std::uint64_t prepare_ns = 0;
    bool memory_uses_large_pages = false;
    bool all_vms_use_large_pages = false;
    std::uint64_t retained_job_references = 0;
    std::uint64_t tracked_verifications = 0;
    bool release_requested = false;
    bool release_started = false;
};

struct SeedQuery {
    mspv_status status = MSPV_OK;
    SeedSnapshot seed{};
};

enum class SubmissionKind {
    hash_only,
    verify_claim,
};

struct SubmitResult {
    mspv_status status = MSPV_OK;
    std::uint64_t ticket = 0;
};

enum class Correlation {
    matched,
    unknown_user_tag,
    ticket_mismatch,
    seed_mismatch,
    ticket_and_seed_mismatch,
};

struct Completion {
    mspv_result result = MSPV_RESULT_FAILED;
    mspv_status error = MSPV_INTERNAL_ERROR;
    mspv_comparison comparison = MSPV_COMPARISON_NOT_REQUESTED;
    std::uint64_t ticket = 0;
    std::uint64_t user_tag = 0;
    mspv_seed_id seed_id = 0;
    PowHash hash{};
    std::uint64_t queue_ns = 0;
    std::uint64_t hash_ns = 0;
    std::uint64_t total_ns = 0;
    SubmissionKind submission_kind = SubmissionKind::verify_claim;
    Correlation correlation = Correlation::unknown_user_tag;
    std::uint64_t expected_ticket = 0;
    mspv_seed_id expected_seed_id = 0;
};

/*
 * terminal_status is MSPV_TIMEOUT when the completion queue was empty,
 * MSPV_CLOSED after a successful shutdown and complete drain, MSPV_OK when
 * max_count stopped the drain early, or an unexpected verifier error.
 */
struct DrainResult {
    std::vector<Completion> completions;
    mspv_status terminal_status = MSPV_TIMEOUT;
};

struct StatsResult {
    mspv_status status = MSPV_OK;
    mspv_stats stats{};
};

class Verifier final {
public:
    explicit Verifier(Config config);
    ~Verifier() noexcept;

    Verifier(const Verifier &) = delete;
    Verifier &operator=(const Verifier &) = delete;
    Verifier(Verifier &&) = delete;
    Verifier &operator=(Verifier &&) = delete;

    [[nodiscard]] static NativeConfigSnapshot map_config(const Config &config);

    /* Async seed preparation. The adapter retains the 32-byte key storage. */
    [[nodiscard]] SeedQuery prepare_seed(const SeedHash &seed_hash);
    [[nodiscard]] mspv_status wait_seed_ready(
        mspv_seed_id seed_id,
        std::uint32_t timeout_ms = MSPV_WAIT_FOREVER);
    [[nodiscard]] SeedQuery seed_snapshot(mspv_seed_id seed_id);
    [[nodiscard]] std::vector<SeedSnapshot> seed_snapshots();
    [[nodiscard]] mspv_status activate_seed(mspv_seed_id seed_id);
    [[nodiscard]] mspv_status deactivate_seed();

    /*
     * Retained-job references and accepted MSPV work both delay a requested
     * release. request_seed_release() is idempotent and begins native release
     * only after the seed is noncurrent and all such references are gone.
     */
    [[nodiscard]] mspv_status retain_seed(mspv_seed_id seed_id);
    [[nodiscard]] mspv_status drop_seed_reference(mspv_seed_id seed_id);
    [[nodiscard]] mspv_status request_seed_release(mspv_seed_id seed_id);
    [[nodiscard]] mspv_status wait_seed_released(
        mspv_seed_id seed_id,
        std::uint32_t timeout_ms = MSPV_WAIT_FOREVER);

    [[nodiscard]] SubmitResult submit_verify(
        mspv_seed_id seed_id,
        std::span<const std::uint8_t> input,
        const PowHash &claimed_hash,
        std::uint64_t user_tag);
    [[nodiscard]] SubmitResult submit_hash(
        mspv_seed_id seed_id,
        std::span<const std::uint8_t> input,
        std::uint64_t user_tag);

    /* Always polls MSPV with timeout zero; it never blocks for a completion. */
    [[nodiscard]] DrainResult drain_completions(
        std::size_t max_count = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] StatsResult stats();

    /*
     * Shutdown stops callbacks before returning. Completions remain available
     * and must be drained until terminal_status is MSPV_CLOSED before destroy.
     */
    [[nodiscard]] mspv_status shutdown(ShutdownMode mode);
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_shutdown() const noexcept;

    /* Coalescible notifier state; clear before draining to avoid lost wakes. */
    [[nodiscard]] bool consume_wakeup() noexcept;
    [[nodiscard]] std::uint64_t callback_exception_count() const noexcept;
    [[nodiscard]] std::size_t tracked_submission_count() const noexcept;

private:
    enum class Lifecycle {
        running,
        shutdown,
        failed,
    };

    struct SeedRecord;
    struct SubmissionSlot;

    [[nodiscard]] static mspv_memory_mode map_memory_mode(MemoryMode mode);
    [[nodiscard]] static mspv_large_page_mode map_large_pages(LargePages mode);
    [[nodiscard]] static mspv_log_level map_log_level(LogLevel level);
    [[nodiscard]] static LogLevel unmap_log_level(mspv_log_level level);
    [[nodiscard]] static std::uint32_t map_options(JitMode jit, AesMode aes);
    [[nodiscard]] static mspv_shutdown_mode map_shutdown_mode(ShutdownMode mode);

    static void native_notify(void *opaque) noexcept;
    static void native_log(void *opaque,
                           mspv_log_level level,
                           const char *message) noexcept;

    [[nodiscard]] SeedRecord *find_seed_locked(mspv_seed_id seed_id);
    [[nodiscard]] const SeedRecord *find_seed_locked(
        mspv_seed_id seed_id) const;
    [[nodiscard]] SeedRecord *find_resident_seed_by_hash_locked(
        const SeedHash &seed_hash);
    [[nodiscard]] SeedQuery seed_snapshot_locked(SeedRecord &record);
    [[nodiscard]] mspv_status maybe_begin_seed_release_locked(
        SeedRecord &record);

    [[nodiscard]] SubmitResult submit_locked(
        SubmissionKind kind,
        mspv_seed_id seed_id,
        std::span<const std::uint8_t> input,
        const PowHash *claimed_hash,
        std::uint64_t user_tag);
    [[nodiscard]] std::size_t find_submission_slot_locked(
        std::uint64_t user_tag) const noexcept;
    [[nodiscard]] std::size_t find_submission_insert_slot_locked(
        std::uint64_t user_tag,
        bool &already_present) const noexcept;
    void erase_submission_slot_locked(std::size_t slot_index) noexcept;
    [[nodiscard]] Completion correlate_completion_locked(
        const mspv_completion &native_completion);
    void discard_completions_locked() noexcept;

    Config config_;
    mspv_config native_config_{};
    mspv_context *context_ = nullptr;

    mutable std::mutex mutex_;
    Lifecycle lifecycle_ = Lifecycle::failed;
    mspv_seed_id current_seed_id_ = 0;
    // The native verifier may retain seed_hash.data() until prepare finishes.
    // Node-stable storage lets completed records be erased without moving any
    // still-preparing record or invalidating that pointer.
    std::list<SeedRecord> seeds_;
    std::vector<SubmissionSlot> submissions_;
    std::size_t tracked_submissions_ = 0;

    std::atomic<bool> wake_pending_{false};
    std::atomic<std::uint64_t> callback_exceptions_{0};
};

} // namespace monero_solo::verifier

#endif
