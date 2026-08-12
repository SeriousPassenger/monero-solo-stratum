/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/verifier.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace monero_solo::verifier {
namespace {

constexpr std::uint32_t kMaximumWorkers = 256;
constexpr std::uint32_t kMaximumCapacity = 1'000'000;
constexpr std::uint32_t kMaximumInputSize = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaximumSeeds = 64;
constexpr std::uint64_t kMaximumBufferedInputBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

std::string status_text(const mspv_status status)
{
    const char *const text = mspv_status_string(status);
    return text == nullptr ? std::string("unknown") : std::string(text);
}

std::size_t submission_table_size(const std::uint32_t max_outstanding)
{
    const std::size_t wanted =
        std::max<std::size_t>(8, static_cast<std::size_t>(max_outstanding) * 2U);
    std::size_t result = 1;
    while (result < wanted) {
        result <<= 1U;
    }
    return result;
}

std::uint64_t mix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

} // namespace

struct Verifier::SeedRecord {
    SeedHash seed_hash{};
    mspv_seed_id seed_id = 0;
    mspv_seed_state state = MSPV_SEED_PREPARING;
    mspv_status last_error = MSPV_OK;
    std::uint32_t key_size = static_cast<std::uint32_t>(kSeedHashSize);
    std::uint32_t queued_jobs = 0;
    std::uint32_t running_jobs = 0;
    std::uint64_t prepare_ns = 0;
    bool memory_uses_large_pages = false;
    bool all_vms_use_large_pages = false;
    std::uint64_t retained_job_references = 0;
    std::uint64_t tracked_verifications = 0;
    bool release_requested = false;
    bool release_started = false;
    bool removed = false;
};

struct Verifier::SubmissionSlot {
    enum class State : std::uint8_t {
        empty,
        occupied,
        tombstone,
    };

    State state = State::empty;
    std::uint64_t user_tag = 0;
    std::uint64_t ticket = 0;
    mspv_seed_id seed_id = 0;
    SubmissionKind kind = SubmissionKind::verify_claim;
};

Error::Error(std::string operation,
             const mspv_status status,
             const mspv_status cleanup_status)
    : std::runtime_error([&]() {
          std::ostringstream message;
          message << operation << ": " << status_text(status);
          if (cleanup_status != MSPV_OK) {
              message << " (cleanup: " << status_text(cleanup_status) << ')';
          }
          return message.str();
      }()),
      operation_(std::move(operation)),
      status_(status),
      cleanup_status_(cleanup_status)
{
}

mspv_memory_mode Verifier::map_memory_mode(const MemoryMode mode)
{
    switch (mode) {
    case MemoryMode::light:
        return MSPV_MEMORY_LIGHT;
    case MemoryMode::fast:
        return MSPV_MEMORY_FAST;
    }
    throw Error("verifier.memory_mode", MSPV_INVALID_CONFIG);
}

mspv_large_page_mode Verifier::map_large_pages(const LargePages mode)
{
    switch (mode) {
    case LargePages::disabled:
        return MSPV_LARGE_PAGES_DISABLED;
    case LargePages::try_enable:
        return MSPV_LARGE_PAGES_TRY;
    case LargePages::require:
        return MSPV_LARGE_PAGES_REQUIRE;
    }
    throw Error("verifier.large_pages", MSPV_INVALID_CONFIG);
}

mspv_log_level Verifier::map_log_level(const LogLevel level)
{
    switch (level) {
    case LogLevel::error:
        return MSPV_LOG_ERROR;
    case LogLevel::warning:
        return MSPV_LOG_WARNING;
    case LogLevel::info:
        return MSPV_LOG_INFO;
    case LogLevel::debug:
        return MSPV_LOG_DEBUG;
    case LogLevel::trace:
        return MSPV_LOG_TRACE;
    }
    throw Error("verifier.log_level", MSPV_INVALID_CONFIG);
}

LogLevel Verifier::unmap_log_level(const mspv_log_level level)
{
    switch (level) {
    case MSPV_LOG_ERROR:
        return LogLevel::error;
    case MSPV_LOG_WARNING:
        return LogLevel::warning;
    case MSPV_LOG_INFO:
        return LogLevel::info;
    case MSPV_LOG_DEBUG:
        return LogLevel::debug;
    case MSPV_LOG_TRACE:
        return LogLevel::trace;
    default:
        return LogLevel::error;
    }
}

std::uint32_t Verifier::map_options(const JitMode jit, const AesMode aes)
{
    std::uint32_t options = MSPV_OPTION_NONE;
    switch (jit) {
    case JitMode::disabled:
        options |= MSPV_OPTION_DISABLE_JIT;
        break;
    case JitMode::enabled:
        break;
    case JitMode::secure:
        options |= MSPV_OPTION_SECURE_JIT;
        break;
    default:
        throw Error("verifier.jit", MSPV_INVALID_CONFIG);
    }

    switch (aes) {
    case AesMode::automatic:
        break;
    case AesMode::software:
        options |= MSPV_OPTION_DISABLE_HARD_AES;
        break;
    default:
        throw Error("verifier.aes", MSPV_INVALID_CONFIG);
    }
    return options;
}

mspv_shutdown_mode Verifier::map_shutdown_mode(const ShutdownMode mode)
{
    switch (mode) {
    case ShutdownMode::drain:
        return MSPV_SHUTDOWN_DRAIN;
    case ShutdownMode::cancel_pending:
        return MSPV_SHUTDOWN_CANCEL_PENDING;
    }
    return MSPV_SHUTDOWN_CANCEL_PENDING;
}

NativeConfigSnapshot Verifier::map_config(const Config &config)
{
    if (config.workers == 0 || config.workers > kMaximumWorkers ||
        config.seed_init_threads == 0 ||
        config.seed_init_threads > kMaximumWorkers ||
        config.pending_capacity == 0 ||
        config.pending_capacity > kMaximumCapacity ||
        config.max_outstanding < config.pending_capacity ||
        config.max_outstanding > kMaximumCapacity ||
        config.max_input_size == 0 ||
        config.max_input_size > kMaximumInputSize ||
        config.max_seeds < 2 || config.max_seeds > kMaximumSeeds ||
        config.max_buffered_input_bytes < config.max_input_size ||
        config.max_buffered_input_bytes > kMaximumBufferedInputBytes) {
        throw Error("verifier configuration", MSPV_INVALID_CONFIG);
    }

    mspv_config defaults{};
    const mspv_status init_status = mspv_config_init(&defaults);
    if (init_status != MSPV_OK) {
        throw Error("mspv_config_init", init_status);
    }

    NativeConfigSnapshot result{};
    result.abi_version = MSPV_ABI_VERSION;
    result.worker_count = config.workers;
    result.seed_init_threads = config.seed_init_threads;
    result.pending_capacity = config.pending_capacity;
    result.max_outstanding = config.max_outstanding;
    result.max_input_size = config.max_input_size;
    result.max_seed_key_size = static_cast<std::uint32_t>(kSeedHashSize);
    result.max_seeds = config.max_seeds;
    result.max_buffered_input_bytes = config.max_buffered_input_bytes;
    result.memory_mode = map_memory_mode(config.memory_mode);
    result.large_pages = map_large_pages(config.large_pages);
    result.options = map_options(config.jit, config.aes);
    result.log_level = map_log_level(config.log_level);
    return result;
}

Verifier::Verifier(Config config)
    : config_(std::move(config))
{
    static_assert(MSPV_ABI_VERSION == 1U,
                  "this adapter must be reviewed for a new MSPV ABI");

    const NativeConfigSnapshot mapped = map_config(config_);
    const mspv_status init_status = mspv_config_init(&native_config_);
    if (init_status != MSPV_OK) {
        throw Error("mspv_config_init", init_status);
    }

    native_config_.worker_count = mapped.worker_count;
    native_config_.seed_init_threads = mapped.seed_init_threads;
    native_config_.pending_capacity = mapped.pending_capacity;
    native_config_.max_outstanding = mapped.max_outstanding;
    native_config_.max_input_size = mapped.max_input_size;
    native_config_.max_seed_key_size = mapped.max_seed_key_size;
    native_config_.max_seeds = mapped.max_seeds;
    native_config_.max_buffered_input_bytes = mapped.max_buffered_input_bytes;
    native_config_.memory_mode = mapped.memory_mode;
    native_config_.large_pages = mapped.large_pages;
    native_config_.options = mapped.options;
    native_config_.notify = &Verifier::native_notify;
    native_config_.notify_user_data = this;
    native_config_.log = &Verifier::native_log;
    native_config_.log_user_data = this;
    native_config_.log_level = mapped.log_level;

    /* Allocate all hot-path correlation slots before MSPV accepts any work. */
    submissions_.resize(submission_table_size(config_.max_outstanding));

    mspv_context *created = nullptr;
    const mspv_status create_status = mspv_create(&native_config_, &created);
    if (create_status != MSPV_OK) {
        throw Error("mspv_create", create_status);
    }
    context_ = created;

    const mspv_status start_status = mspv_start(context_);
    if (start_status != MSPV_OK) {
        /* Start may have partially acquired resources; always attempt cancel. */
        const mspv_status cleanup_status =
            mspv_shutdown(context_, MSPV_SHUTDOWN_CANCEL_PENDING);
        mspv_destroy(context_);
        context_ = nullptr;
        lifecycle_ = Lifecycle::failed;
        throw Error("mspv_start", start_status, cleanup_status);
    }
    lifecycle_ = Lifecycle::running;
}

Verifier::~Verifier() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == nullptr) {
        return;
    }

    if (lifecycle_ == Lifecycle::running) {
        mspv_status status = mspv_shutdown(context_, MSPV_SHUTDOWN_DRAIN);
        if (status != MSPV_OK) {
            status = mspv_shutdown(context_, MSPV_SHUTDOWN_CANCEL_PENDING);
        }
        if (status == MSPV_OK) {
            lifecycle_ = Lifecycle::shutdown;
        }
    }

    if (lifecycle_ == Lifecycle::shutdown) {
        discard_completions_locked();
    }
    mspv_destroy(context_);
    context_ = nullptr;
    lifecycle_ = Lifecycle::failed;
}

void Verifier::native_notify(void *const opaque) noexcept
{
    auto *const verifier = static_cast<Verifier *>(opaque);
    verifier->wake_pending_.store(true, std::memory_order_release);
    if (!verifier->config_.wake) {
        return;
    }
    try {
        verifier->config_.wake();
    }
    catch (...) {
        verifier->callback_exceptions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Verifier::native_log(void *const opaque,
                          const mspv_log_level level,
                          const char *const message) noexcept
{
    auto *const verifier = static_cast<Verifier *>(opaque);
    if (!verifier->config_.log) {
        return;
    }
    try {
        verifier->config_.log(unmap_log_level(level),
                              message == nullptr ? std::string_view{} :
                                                   std::string_view(message));
    }
    catch (...) {
        verifier->callback_exceptions_.fetch_add(1, std::memory_order_relaxed);
    }
}

Verifier::SeedRecord *Verifier::find_seed_locked(const mspv_seed_id seed_id)
{
    const auto found = std::find_if(
        seeds_.begin(), seeds_.end(), [seed_id](const SeedRecord &seed) {
            return seed.seed_id == seed_id && !seed.removed;
        });
    return found == seeds_.end() ? nullptr : &*found;
}

const Verifier::SeedRecord *Verifier::find_seed_locked(
    const mspv_seed_id seed_id) const
{
    const auto found = std::find_if(
        seeds_.begin(), seeds_.end(), [seed_id](const SeedRecord &seed) {
            return seed.seed_id == seed_id && !seed.removed;
        });
    return found == seeds_.end() ? nullptr : &*found;
}

Verifier::SeedRecord *Verifier::find_resident_seed_by_hash_locked(
    const SeedHash &seed_hash)
{
    const auto found = std::find_if(
        seeds_.rbegin(), seeds_.rend(), [&](const SeedRecord &seed) {
            return seed.seed_hash == seed_hash && !seed.removed;
        });
    return found == seeds_.rend() ? nullptr : &*found;
}

SeedQuery Verifier::seed_snapshot_locked(SeedRecord &record)
{
    SeedQuery result{};
    mspv_seed_info info{};
    result.status = mspv_seed_get_info(context_, record.seed_id, &info);
    if (result.status == MSPV_OK) {
        record.state = info.state;
        record.last_error = info.last_error;
        record.key_size = info.key_size;
        record.queued_jobs = info.queued_jobs;
        record.running_jobs = info.running_jobs;
        record.prepare_ns = info.prepare_ns;
        record.memory_uses_large_pages = info.memory_uses_large_pages != 0;
        record.all_vms_use_large_pages = info.all_vms_use_large_pages != 0;
    }
    else if (result.status == MSPV_SEED_NOT_FOUND && record.release_started) {
        record.removed = true;
    }

    result.seed.seed_id = record.seed_id;
    result.seed.seed_hash = record.seed_hash;
    result.seed.state = record.state;
    result.seed.last_error = record.last_error;
    result.seed.key_size = record.key_size;
    result.seed.queued_jobs = record.queued_jobs;
    result.seed.running_jobs = record.running_jobs;
    result.seed.prepare_ns = record.prepare_ns;
    result.seed.memory_uses_large_pages = record.memory_uses_large_pages;
    result.seed.all_vms_use_large_pages = record.all_vms_use_large_pages;
    result.seed.retained_job_references = record.retained_job_references;
    result.seed.tracked_verifications = record.tracked_verifications;
    result.seed.release_requested = record.release_requested;
    result.seed.release_started = record.release_started;
    return result;
}

SeedQuery Verifier::prepare_seed(const SeedHash &seed_hash)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return {MSPV_NOT_RUNNING, {}};
    }

    std::erase_if(seeds_, [](const SeedRecord &seed) { return seed.removed; });
    if (SeedRecord *const resident =
            find_resident_seed_by_hash_locked(seed_hash)) {
        if (resident->release_started) {
            SeedQuery result = seed_snapshot_locked(*resident);
            result.status = MSPV_SEED_RELEASING;
            return result;
        }
        // A daemon reorg can make a formerly obsolete seed current again while
        // retained old jobs have kept its requested release from starting.
        // Re-preparing that resident seed cancels only the pending intent; a
        // native release which already started remains irreversible above.
        resident->release_requested = false;
        return seed_snapshot_locked(*resident);
    }

    /* std::list keeps this key's address stable through insertions/erasures. */
    seeds_.emplace_back();
    SeedRecord &record = seeds_.back();
    record.seed_hash = seed_hash;
    mspv_seed_id seed_id = 0;
    const mspv_status status = mspv_seed_prepare(
        context_, record.seed_hash.data(), record.seed_hash.size(), &seed_id);
    if (status != MSPV_OK) {
        seeds_.pop_back();
        return {status, {}};
    }

    if (SeedRecord *const existing = find_seed_locked(seed_id)) {
        if (existing != &record) {
            seeds_.pop_back();
            return seed_snapshot_locked(*existing);
        }
    }
    record.seed_id = seed_id;
    SeedQuery result{};
    result.status = MSPV_OK;
    result.seed.seed_id = seed_id;
    result.seed.seed_hash = seed_hash;
    result.seed.state = MSPV_SEED_PREPARING;
    result.seed.key_size = static_cast<std::uint32_t>(kSeedHashSize);
    return result;
}

mspv_status Verifier::wait_seed_ready(const mspv_seed_id seed_id,
                                      const std::uint32_t timeout_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return MSPV_SEED_NOT_FOUND;
    }

    const mspv_status status =
        mspv_seed_wait_ready(context_, seed_id, timeout_ms);
    (void)seed_snapshot_locked(*record);
    return status;
}

SeedQuery Verifier::seed_snapshot(const mspv_seed_id seed_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return {MSPV_NOT_RUNNING, {}};
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return {MSPV_SEED_NOT_FOUND, {}};
    }
    return seed_snapshot_locked(*record);
}

std::vector<SeedSnapshot> Verifier::seed_snapshots()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SeedSnapshot> result;
    result.reserve(seeds_.size());
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return result;
    }
    for (SeedRecord &record : seeds_) {
        if (record.removed) {
            continue;
        }
        SeedQuery query = seed_snapshot_locked(record);
        if (!record.removed) {
            result.push_back(std::move(query.seed));
        }
    }
    std::sort(result.begin(), result.end(), [](const SeedSnapshot &left,
                                               const SeedSnapshot &right) {
        return left.seed_id < right.seed_id;
    });
    return result;
}

mspv_status Verifier::activate_seed(const mspv_seed_id seed_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return MSPV_SEED_NOT_FOUND;
    }
    if (record->release_requested || record->release_started) {
        return MSPV_SEED_RELEASING;
    }
    const SeedQuery query = seed_snapshot_locked(*record);
    if (query.status != MSPV_OK) {
        return query.status;
    }
    if (record->state != MSPV_SEED_READY &&
        record->state != MSPV_SEED_CURRENT) {
        return MSPV_SEED_NOT_READY;
    }

    const mspv_seed_id former_current = current_seed_id_;
    const mspv_status status = mspv_seed_activate(context_, seed_id);
    if (status != MSPV_OK) {
        return status;
    }
    current_seed_id_ = seed_id;
    record->state = MSPV_SEED_CURRENT;
    if (former_current != 0 && former_current != seed_id) {
        if (SeedRecord *const former = find_seed_locked(former_current)) {
            former->state = MSPV_SEED_READY;
            (void)maybe_begin_seed_release_locked(*former);
        }
    }
    return MSPV_OK;
}

mspv_status Verifier::deactivate_seed()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    if (current_seed_id_ == 0) {
        return MSPV_OK;
    }
    const mspv_seed_id former_current = current_seed_id_;
    const mspv_status status = mspv_seed_deactivate(context_);
    if (status != MSPV_OK) {
        return status;
    }
    current_seed_id_ = 0;
    if (SeedRecord *const former = find_seed_locked(former_current)) {
        former->state = MSPV_SEED_READY;
        (void)maybe_begin_seed_release_locked(*former);
    }
    return MSPV_OK;
}

mspv_status Verifier::retain_seed(const mspv_seed_id seed_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return MSPV_SEED_NOT_FOUND;
    }
    // A requested release is only intent while retained job references keep
    // the native seed resident. Old Stratum jobs may acquire another short
    // admission reference until release has actually started.
    if (record->release_started) {
        return MSPV_SEED_RELEASING;
    }
    if (record->retained_job_references ==
        std::numeric_limits<std::uint64_t>::max()) {
        return MSPV_INTERNAL_ERROR;
    }
    ++record->retained_job_references;
    return MSPV_OK;
}

mspv_status Verifier::drop_seed_reference(const mspv_seed_id seed_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return MSPV_SEED_NOT_FOUND;
    }
    if (record->retained_job_references == 0) {
        return MSPV_INVALID_ARGUMENT;
    }
    --record->retained_job_references;
    return maybe_begin_seed_release_locked(*record);
}

mspv_status Verifier::request_seed_release(const mspv_seed_id seed_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        const bool was_removed = std::any_of(
            seeds_.begin(), seeds_.end(), [seed_id](const SeedRecord &seed) {
                return seed.seed_id == seed_id && seed.removed;
            });
        if (was_removed) {
            std::erase_if(seeds_, [seed_id](const SeedRecord &seed) {
                return seed.seed_id == seed_id && seed.removed;
            });
            return MSPV_OK;
        }
        return MSPV_SEED_NOT_FOUND;
    }
    record->release_requested = true;
    return maybe_begin_seed_release_locked(*record);
}

mspv_status Verifier::maybe_begin_seed_release_locked(SeedRecord &record)
{
    if (!record.release_requested || record.release_started || record.removed) {
        return MSPV_OK;
    }
    if (record.seed_id == current_seed_id_ ||
        record.retained_job_references != 0 ||
        record.tracked_verifications != 0) {
        return MSPV_OK;
    }
    if (lifecycle_ != Lifecycle::running) {
        /* Successful shutdown owns final seed/resource teardown. */
        return MSPV_OK;
    }

    const mspv_status status = mspv_seed_release(context_, record.seed_id);
    if (status == MSPV_OK || status == MSPV_SEED_RELEASING) {
        record.release_started = true;
        record.state = MSPV_SEED_RELEASING_STATE;
        return MSPV_OK;
    }
    if (status == MSPV_SEED_NOT_FOUND) {
        record.release_started = true;
        record.removed = true;
        return MSPV_OK;
    }
    return status;
}

mspv_status Verifier::wait_seed_released(const mspv_seed_id seed_id,
                                         const std::uint32_t timeout_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    SeedRecord *const record = find_seed_locked(seed_id);
    if (record == nullptr) {
        return MSPV_SEED_NOT_FOUND;
    }
    record->release_requested = true;
    const mspv_status begin_status = maybe_begin_seed_release_locked(*record);
    if (begin_status != MSPV_OK) {
        return begin_status;
    }
    if (record->seed_id == current_seed_id_) {
        return MSPV_SEED_ACTIVE;
    }
    if (!record->release_started) {
        return MSPV_SEED_NOT_READY;
    }

    const mspv_status status =
        mspv_seed_wait_released(context_, seed_id, timeout_ms);
    if (status == MSPV_OK || status == MSPV_SEED_NOT_FOUND) {
        std::erase_if(seeds_, [seed_id](const SeedRecord &seed) {
            return seed.seed_id == seed_id;
        });
        return MSPV_OK;
    }
    return status;
}

std::size_t Verifier::find_submission_slot_locked(
    const std::uint64_t user_tag) const noexcept
{
    if (submissions_.empty()) {
        return submissions_.size();
    }
    const std::size_t mask = submissions_.size() - 1U;
    std::size_t index = static_cast<std::size_t>(mix64(user_tag)) & mask;
    for (std::size_t examined = 0; examined < submissions_.size(); ++examined) {
        const SubmissionSlot &slot = submissions_[index];
        if (slot.state == SubmissionSlot::State::empty) {
            return submissions_.size();
        }
        if (slot.state == SubmissionSlot::State::occupied &&
            slot.user_tag == user_tag) {
            return index;
        }
        index = (index + 1U) & mask;
    }
    return submissions_.size();
}

std::size_t Verifier::find_submission_insert_slot_locked(
    const std::uint64_t user_tag,
    bool &already_present) const noexcept
{
    already_present = false;
    const std::size_t mask = submissions_.size() - 1U;
    std::size_t index = static_cast<std::size_t>(mix64(user_tag)) & mask;
    std::size_t first_tombstone = submissions_.size();
    for (std::size_t examined = 0; examined < submissions_.size(); ++examined) {
        const SubmissionSlot &slot = submissions_[index];
        if (slot.state == SubmissionSlot::State::occupied) {
            if (slot.user_tag == user_tag) {
                already_present = true;
                return index;
            }
        }
        else if (slot.state == SubmissionSlot::State::tombstone) {
            if (first_tombstone == submissions_.size()) {
                first_tombstone = index;
            }
        }
        else {
            return first_tombstone == submissions_.size() ? index :
                                                            first_tombstone;
        }
        index = (index + 1U) & mask;
    }
    return first_tombstone;
}

void Verifier::erase_submission_slot_locked(const std::size_t slot_index) noexcept
{
    SubmissionSlot &slot = submissions_[slot_index];
    slot.state = SubmissionSlot::State::tombstone;
    slot.user_tag = 0;
    slot.ticket = 0;
    slot.seed_id = 0;
    slot.kind = SubmissionKind::verify_claim;
    if (tracked_submissions_ != 0) {
        --tracked_submissions_;
    }
}

SubmitResult Verifier::submit_locked(const SubmissionKind kind,
                                     const mspv_seed_id seed_id,
                                     const std::span<const std::uint8_t> input,
                                     const PowHash *const claimed_hash,
                                     const std::uint64_t user_tag)
{
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        return {MSPV_NOT_RUNNING, 0};
    }
    if (user_tag == 0 || input.empty() ||
        input.size() > config_.max_input_size ||
        (kind == SubmissionKind::verify_claim && claimed_hash == nullptr)) {
        return {MSPV_INVALID_ARGUMENT, 0};
    }
    SeedRecord *const seed = find_seed_locked(seed_id);
    if (seed == nullptr) {
        return {MSPV_SEED_NOT_FOUND, 0};
    }
    // See retain_seed(): retained old jobs remain verifiable until the native
    // release has actually begun.
    if (seed->release_started) {
        return {MSPV_SEED_RELEASING, 0};
    }
    const SeedQuery seed_query = seed_snapshot_locked(*seed);
    if (seed_query.status != MSPV_OK) {
        return {seed_query.status, 0};
    }
    if (seed->state != MSPV_SEED_READY && seed->state != MSPV_SEED_CURRENT) {
        return {MSPV_SEED_NOT_READY, 0};
    }
    if (tracked_submissions_ >= config_.max_outstanding) {
        return {MSPV_QUEUE_FULL, 0};
    }

    bool already_present = false;
    const std::size_t slot_index =
        find_submission_insert_slot_locked(user_tag, already_present);
    if (already_present) {
        return {MSPV_INVALID_ARGUMENT, 0};
    }
    if (slot_index == submissions_.size()) {
        return {MSPV_QUEUE_FULL, 0};
    }
    SubmissionSlot &slot = submissions_[slot_index];
    slot.state = SubmissionSlot::State::occupied;
    slot.user_tag = user_tag;
    slot.ticket = 0;
    slot.seed_id = seed_id;
    slot.kind = kind;
    ++tracked_submissions_;

    std::uint64_t ticket = 0;
    const mspv_status status =
        kind == SubmissionKind::verify_claim ?
            mspv_verify_submit(context_,
                               seed_id,
                               input.data(),
                               input.size(),
                               claimed_hash->data(),
                               user_tag,
                               &ticket) :
            mspv_hash_submit(context_,
                             seed_id,
                             input.data(),
                             input.size(),
                             user_tag,
                             &ticket);
    if (status != MSPV_OK) {
        erase_submission_slot_locked(slot_index);
        return {status, 0};
    }
    slot.ticket = ticket;
    ++seed->tracked_verifications;
    return {MSPV_OK, ticket};
}

SubmitResult Verifier::submit_verify(
    const mspv_seed_id seed_id,
    const std::span<const std::uint8_t> input,
    const PowHash &claimed_hash,
    const std::uint64_t user_tag)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_locked(SubmissionKind::verify_claim,
                         seed_id,
                         input,
                         &claimed_hash,
                         user_tag);
}

SubmitResult Verifier::submit_hash(const mspv_seed_id seed_id,
                                   const std::span<const std::uint8_t> input,
                                   const std::uint64_t user_tag)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_locked(
        SubmissionKind::hash_only, seed_id, input, nullptr, user_tag);
}

Completion Verifier::correlate_completion_locked(
    const mspv_completion &native_completion)
{
    Completion result{};
    result.result = native_completion.result;
    result.error = native_completion.error;
    result.ticket = native_completion.ticket;
    result.user_tag = native_completion.user_tag;
    result.seed_id = native_completion.seed_id;
    if (native_completion.result == MSPV_RESULT_OK &&
        native_completion.error == MSPV_OK) {
        result.comparison = native_completion.comparison;
        std::copy_n(native_completion.hash, result.hash.size(), result.hash.begin());
        result.queue_ns = native_completion.queue_ns;
        result.hash_ns = native_completion.hash_ns;
        result.total_ns = native_completion.total_ns;
    }

    const std::size_t slot_index =
        find_submission_slot_locked(native_completion.user_tag);
    if (slot_index == submissions_.size()) {
        result.correlation = Correlation::unknown_user_tag;
        return result;
    }

    const SubmissionSlot slot = submissions_[slot_index];
    result.submission_kind = slot.kind;
    result.expected_ticket = slot.ticket;
    result.expected_seed_id = slot.seed_id;
    const bool ticket_matches = slot.ticket == native_completion.ticket;
    const bool seed_matches = slot.seed_id == native_completion.seed_id;
    if (ticket_matches && seed_matches) {
        result.correlation = Correlation::matched;
    }
    else if (!ticket_matches && !seed_matches) {
        result.correlation = Correlation::ticket_and_seed_mismatch;
    }
    else if (!ticket_matches) {
        result.correlation = Correlation::ticket_mismatch;
    }
    else {
        result.correlation = Correlation::seed_mismatch;
    }

    erase_submission_slot_locked(slot_index);
    if (SeedRecord *const seed = find_seed_locked(slot.seed_id)) {
        if (seed->tracked_verifications != 0) {
            --seed->tracked_verifications;
        }
        (void)maybe_begin_seed_release_locked(*seed);
    }
    return result;
}

DrainResult Verifier::drain_completions(const std::size_t max_count)
{
    /* Clear before polling: a callback racing the drain leaves this set. */
    wake_pending_.exchange(false, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(mutex_);
    DrainResult result{};
    if (context_ == nullptr) {
        result.terminal_status = MSPV_NOT_RUNNING;
        return result;
    }
    result.completions.reserve(std::min<std::size_t>(max_count, 64));

    while (result.completions.size() < max_count) {
        /*
         * Allocate result storage before polling: after MSPV returns a
         * completion its outstanding reservation is gone, so an allocation
         * failure must not make the adapter lose the only copy.
         */
        result.completions.emplace_back();
        mspv_completion native_completion{};
        const mspv_status status =
            mspv_completion_poll(context_, &native_completion, 0);
        if (status != MSPV_OK) {
            result.completions.pop_back();
            result.terminal_status = status;
            return result;
        }
        result.completions.back() =
            correlate_completion_locked(native_completion);
    }
    result.terminal_status = MSPV_OK;
    return result;
}

StatsResult Verifier::stats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    StatsResult result{};
    if (lifecycle_ != Lifecycle::running || context_ == nullptr) {
        result.status = MSPV_NOT_RUNNING;
        return result;
    }
    result.status = mspv_get_stats(context_, &result.stats);
    return result;
}

mspv_status Verifier::shutdown(const ShutdownMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ == nullptr) {
        return MSPV_NOT_RUNNING;
    }
    if (lifecycle_ == Lifecycle::shutdown) {
        return MSPV_OK;
    }
    if (lifecycle_ != Lifecycle::running) {
        return MSPV_NOT_RUNNING;
    }

    const mspv_status status =
        mspv_shutdown(context_, map_shutdown_mode(mode));
    if (status == MSPV_OK) {
        lifecycle_ = Lifecycle::shutdown;
    }
    return status;
}

void Verifier::discard_completions_locked() noexcept
{
    for (;;) {
        mspv_completion completion{};
        const mspv_status status = mspv_completion_poll(context_, &completion, 0);
        if (status != MSPV_OK) {
            break;
        }
        (void)correlate_completion_locked(completion);
    }
}

bool Verifier::is_running() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lifecycle_ == Lifecycle::running;
}

bool Verifier::is_shutdown() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lifecycle_ == Lifecycle::shutdown;
}

bool Verifier::consume_wakeup() noexcept
{
    return wake_pending_.exchange(false, std::memory_order_acq_rel);
}

std::uint64_t Verifier::callback_exception_count() const noexcept
{
    return callback_exceptions_.load(std::memory_order_relaxed);
}

std::size_t Verifier::tracked_submission_count() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tracked_submissions_;
}

} // namespace monero_solo::verifier
