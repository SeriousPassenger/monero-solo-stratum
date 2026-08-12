#include "monero_solo/runtime.hpp"
#include "monero_solo/share_policy.hpp"

#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace monero_solo {
namespace {

std::string decimal_from_hex(std::string_view encoded) {
    if (encoded.empty()) throw ValidationError("empty hexadecimal difficulty");
    if (encoded.size() > 32U) throw ValidationError("hexadecimal difficulty exceeds uint128");
    std::string value = "0";
    for (const char character : encoded) {
        const auto byte = static_cast<unsigned char>(character);
        unsigned digit = 0;
        if (byte >= '0' && byte <= '9') digit = byte - '0';
        else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10U;
        else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10U;
        else throw ValidationError("invalid hexadecimal difficulty");
        unsigned carry = digit;
        for (auto iterator = value.rbegin(); iterator != value.rend(); ++iterator) {
            const unsigned current = static_cast<unsigned>(*iterator - '0') * 16U + carry;
            *iterator = static_cast<char>('0' + current % 10U);
            carry = current / 10U;
        }
        while (carry != 0U) {
            value.insert(value.begin(), static_cast<char>('0' + carry % 10U));
            carry /= 10U;
        }
    }
    if (value == "0") throw ValidationError("network difficulty is zero");
    return value;
}

std::string decimal_json(const nlohmann::json &value) {
    std::string result;
    if (value.is_number_unsigned()) {
        result = std::to_string(value.get<std::uint64_t>());
    }
    else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number <= 0) throw ValidationError("difficulty must be positive");
        result = std::to_string(number);
    }
    else if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.empty() || (text.size() > 1U && text.front() == '0') ||
            !std::all_of(text.begin(), text.end(), [](unsigned char byte) {
                return byte >= '0' && byte <= '9';
            })) {
            throw ValidationError("difficulty is not canonical decimal");
        }
        result = text;
    }
    else {
        throw ValidationError("difficulty has an invalid type");
    }
    validate_network_difficulty(result);
    return result;
}

std::uint64_t positive_u64(const nlohmann::json &object, const char *field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_number_unsigned()) {
        throw ValidationError(std::string("daemon template omitted ") + field);
    }
    const auto value = iterator->get<std::uint64_t>();
    if (value == 0U) throw ValidationError(std::string(field) + " must be positive");
    return value;
}

std::string required_string(const nlohmann::json &object, const char *field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_string()) {
        throw ValidationError(std::string("daemon response omitted ") + field);
    }
    std::string result = iterator->get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos) {
        throw ValidationError(std::string("daemon response has invalid ") + field);
    }
    return result;
}

const nlohmann::json &rpc_result(const RpcObservation &observation) {
    if (!observation.valid()) throw ValidationError("daemon RPC observation was invalid");
    const auto error = observation.document.find("error");
    if (error != observation.document.end() && !error->is_null()) {
        throw ValidationError("daemon returned a JSON-RPC error");
    }
    const auto result = observation.document.find("result");
    if (result == observation.document.end() || !result->is_object()) {
        throw ValidationError("daemon response has no result object");
    }
    const auto status = result->find("status");
    if (status == result->end() || !status->is_string() ||
        status->get_ref<const std::string &>().find('\0') != std::string::npos ||
        status->get_ref<const std::string &>() != "OK") {
        throw ValidationError("daemon returned non-OK status");
    }
    return *result;
}

std::uint64_t available_processor_count() noexcept
{
    const unsigned detected = std::thread::hardware_concurrency();
    return detected == 0U ? 1U : static_cast<std::uint64_t>(detected);
}

std::uint64_t auto_worker_count(const std::uint64_t processors) noexcept
{
    // Keep at least one logical processor available to the networking,
    // database, template, event and candidate control planes.
    return std::clamp<std::uint64_t>(processors > 1U ? processors - 1U : 1U,
                                     1U, 64U);
}

Config resolve_runtime_workers(Config config)
{
    const std::uint64_t automatic = auto_worker_count(
        available_processor_count());
    if (config.verifier.workers == 0U) {
        config.verifier.workers = automatic;
    }
    if (config.verifier.seed_init_threads == 0U) {
        config.verifier.seed_init_threads = automatic;
    }
    if (config.stratum.submit_workers == 0U) {
        config.stratum.submit_workers = automatic;
    }
    return config;
}

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) noexcept(
        std::is_nothrow_move_constructible_v<Callback>)
        : callback_(std::move(callback)) {}

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

    ~ScopeExit() noexcept
    {
        if (active_) callback_();
    }

    void release() noexcept { active_ = false; }

private:
    Callback callback_;
    bool active_{true};
};

template <typename Callback>
ScopeExit(Callback) -> ScopeExit<Callback>;

verifier::Config verifier_config(const Config &config) {
    verifier::Config result;
    result.workers = static_cast<std::uint32_t>(config.verifier.workers);
    result.seed_init_threads = static_cast<std::uint32_t>(config.verifier.seed_init_threads);
    result.pending_capacity = static_cast<std::uint32_t>(config.verifier.pending_capacity);
    result.max_outstanding = static_cast<std::uint32_t>(config.verifier.max_outstanding);
    result.max_input_size = static_cast<std::uint32_t>(config.verifier.max_input_size);
    result.max_seeds = static_cast<std::uint32_t>(config.verifier.max_seeds);
    result.max_buffered_input_bytes = config.verifier.max_buffered_input_bytes;
    result.memory_mode = config.verifier.memory_mode == "fast"
                             ? verifier::MemoryMode::fast : verifier::MemoryMode::light;
    if (config.verifier.large_pages == "disabled") result.large_pages = verifier::LargePages::disabled;
    else if (config.verifier.large_pages == "require") result.large_pages = verifier::LargePages::require;
    else result.large_pages = verifier::LargePages::try_enable;
    if (config.verifier.jit == "disabled") result.jit = verifier::JitMode::disabled;
    else if (config.verifier.jit == "enabled") result.jit = verifier::JitMode::enabled;
    else result.jit = verifier::JitMode::secure;
    result.aes = config.verifier.aes == "software"
                     ? verifier::AesMode::software : verifier::AesMode::automatic;
    const std::map<std::string, verifier::LogLevel> levels{
        {"error", verifier::LogLevel::error}, {"warning", verifier::LogLevel::warning},
        {"info", verifier::LogLevel::info}, {"debug", verifier::LogLevel::debug},
        {"trace", verifier::LogLevel::trace}};
    result.log_level = levels.at(config.verifier.log_level);
    return result;
}

DefensePolicyConfig defense_config(const Config &config) {
    DefensePolicyConfig result;
    result.enabled = config.defense.enabled;
    result.ban_seconds = static_cast<std::uint32_t>(config.defense.ban_seconds);
    result.connection_rate_per_minute = static_cast<std::uint32_t>(config.defense.connection_rate_per_minute);
    result.connection_burst = static_cast<std::uint32_t>(config.defense.connection_burst);
    result.request_rate_per_second = static_cast<std::uint32_t>(config.defense.request_rate_per_second);
    result.request_burst = static_cast<std::uint32_t>(config.defense.request_burst);
    result.submit_rate_per_second = static_cast<std::uint32_t>(config.defense.submit_rate_per_second);
    result.submit_burst = static_cast<std::uint32_t>(config.defense.submit_burst);
    result.malformed_limit = static_cast<std::uint32_t>(config.defense.malformed_limit);
    result.auth_failure_limit = static_cast<std::uint32_t>(config.defense.auth_failure_limit);
    result.unknown_job_limit = static_cast<std::uint32_t>(config.defense.unknown_job_limit);
    result.duplicate_limit = static_cast<std::uint32_t>(config.defense.duplicate_limit);
    result.abuse_window_seconds = static_cast<std::uint32_t>(config.defense.abuse_window_seconds);
    result.hammer_rate_multiplier = static_cast<std::uint32_t>(
        config.defense.hammer_rate_multiplier);
    result.hammer_sustain_seconds = static_cast<std::uint32_t>(
        config.defense.hammer_sustain_seconds);
    result.candidate_rate_per_minute = static_cast<std::uint32_t>(config.defense.candidate_rate_per_minute);
    result.candidate_burst = static_cast<std::uint32_t>(config.defense.candidate_burst);
    result.candidate_inflight_per_ip = static_cast<std::uint32_t>(config.defense.candidate_inflight_per_ip);
    result.candidate_global_inflight = static_cast<std::uint32_t>(config.defense.candidate_global_inflight);
    result.false_candidate_limit = static_cast<std::uint32_t>(config.defense.false_candidate_limit);
    result.false_candidate_window_seconds = static_cast<std::uint32_t>(config.defense.false_candidate_window_seconds);
    result.trusted_candidate_rejection_limit = static_cast<std::uint32_t>(config.defense.trusted_candidate_rejection_limit);
    result.trusted_candidate_rejection_window_seconds = static_cast<std::uint32_t>(config.defense.trusted_candidate_rejection_window_seconds);
    result.verification_mismatch_limit = static_cast<std::uint32_t>(config.defense.verification_mismatch_limit);
    result.verification_mismatch_window_seconds = static_cast<std::uint32_t>(config.defense.verification_mismatch_window_seconds);
    return result;
}

StratumServerConfig stratum_config(const Config &config) {
    StratumServerConfig result;
    result.listen = config.stratum.listen;
    result.access_password = config.stratum.access_password.value_or("");
    result.max_connections = static_cast<std::size_t>(config.stratum.max_connections);
    result.max_connections_per_ip = static_cast<std::size_t>(config.stratum.max_connections_per_ip);
    result.login_timeout_ms = static_cast<std::uint32_t>(config.stratum.login_timeout_ms);
    result.idle_timeout_ms = static_cast<std::uint32_t>(config.stratum.idle_timeout_ms);
    result.max_line_bytes = static_cast<std::size_t>(config.stratum.max_line_bytes);
    result.max_output_bytes_per_connection = static_cast<std::size_t>(config.stratum.max_output_bytes_per_connection);
    result.max_json_depth = static_cast<std::size_t>(config.stratum.max_json_depth);
    result.difficulty_floor = config.difficulty.value;
    result.minimum_difficulty = config.difficulty.mode == "minimum";
    result.submit_workers = static_cast<std::size_t>(
        config.stratum.submit_workers);
    result.candidate_submit_workers = std::min<std::size_t>(
        result.submit_workers, 16U);
    result.max_pending_submits = static_cast<std::size_t>(config.database.max_writer_queue_items / 4U);
    result.candidate_submit_reserve = std::min<std::size_t>(
        result.max_pending_submits,
        std::max<std::size_t>(1U, static_cast<std::size_t>(
            config.defense.candidate_global_inflight)));
    result.max_pending_submits_per_connection = static_cast<std::size_t>(
        config.stratum.max_pending_verifications_per_connection);
    return result;
}

std::string abuse_name(AbuseKind kind) {
    switch (kind) {
    case AbuseKind::malformed: return "malformed";
    case AbuseKind::authentication_failure: return "authentication_failure";
    case AbuseKind::unknown_job: return "unknown_job";
    case AbuseKind::duplicate: return "duplicate";
    case AbuseKind::oversized_line: return "oversized_line";
    case AbuseKind::verified_false_candidate: return "verified_false_candidate";
    case AbuseKind::candidate_mismatch: return "candidate_mismatch";
    case AbuseKind::trusted_candidate_rejection: return "trusted_candidate_rejection";
    case AbuseKind::rate_hammer: return "rate_hammer";
    }
    return "abuse";
}

std::string_view candidate_state_name(const CandidateState state) noexcept {
    switch (state) {
    case CandidateState::journaled: return "journaled";
    case CandidateState::dispatching: return "dispatching";
    case CandidateState::retry_wait: return "retry_wait";
    case CandidateState::accepted: return "accepted";
    case CandidateState::rejected: return "rejected";
    case CandidateState::ambiguous: return "ambiguous";
    case CandidateState::accepted_by_reconciliation:
        return "accepted_by_reconciliation";
    }
    return "unknown";
}

std::string_view attempt_classification_name(
    const CandidateAttemptClassification classification) noexcept {
    switch (classification) {
    case CandidateAttemptClassification::accepted: return "accepted";
    case CandidateAttemptClassification::explicit_rejection:
        return "explicit_rejection";
    case CandidateAttemptClassification::indeterminate: return "indeterminate";
    }
    return "unknown";
}

std::string_view reconciliation_classification_name(
    const ReconciliationClassification classification) noexcept {
    switch (classification) {
    case ReconciliationClassification::positive: return "positive";
    case ReconciliationClassification::inconclusive: return "inconclusive";
    case ReconciliationClassification::indeterminate: return "indeterminate";
    }
    return "unknown";
}

std::uint64_t log_id(const std::int64_t value) noexcept {
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

class CandidateReconciliationGuard final {
public:
    CandidateReconciliationGuard(
        std::mutex &mutex,
        std::unordered_set<std::int64_t> &candidates,
        std::int64_t candidate_id)
        : mutex_(&mutex), candidates_(&candidates), candidate_id_(candidate_id)
    {
        std::lock_guard lock(*mutex_);
        acquired_ = candidates_->insert(candidate_id_).second;
    }

    CandidateReconciliationGuard(const CandidateReconciliationGuard &) = delete;
    CandidateReconciliationGuard &operator=(
        const CandidateReconciliationGuard &) = delete;

    ~CandidateReconciliationGuard()
    {
        if (!acquired_) return;
        try {
            std::lock_guard lock(*mutex_);
            candidates_->erase(candidate_id_);
        }
        catch (...) {
            // std::mutex lock failure indicates a process-level synchronization
            // failure.  Destruction must nevertheless remain noexcept.
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    std::mutex *mutex_;
    std::unordered_set<std::int64_t> *candidates_;
    std::int64_t candidate_id_;
    bool acquired_{};
};

} // namespace

namespace detail {

using JobPostCommitFaultHook = void (*)();
std::atomic<JobPostCommitFaultHook> job_post_commit_fault_hook{};

void set_job_post_commit_fault_hook_for_testing(
    const JobPostCommitFaultHook hook) noexcept
{
    job_post_commit_fault_hook.store(hook, std::memory_order_release);
}

void run_job_post_commit_fault_hook()
{
    JobPostCommitFaultHook hook = job_post_commit_fault_hook.load(
        std::memory_order_acquire);
    if (hook != nullptr && job_post_commit_fault_hook.compare_exchange_strong(
                               hook, nullptr, std::memory_order_acq_rel)) {
        hook();
    }
}

} // namespace detail

struct Runtime::TemplateContext {
    std::int64_t database_id{};
    std::uint64_t generation{};
    ParsedBlock block;
    Bytes hashing_blob;
    std::size_t reserved_offset{};
    std::uint64_t height{};
    Hash32 previous_hash{};
    Hash32 seed_hash{};
    std::optional<Hash32> next_seed_hash;
    std::string network_difficulty;
    std::optional<std::string> wide_difficulty_hex;
    mspv_seed_id seed_id{};
};

struct Runtime::JobContext {
    std::int64_t database_id{};
    Id16 public_id{};
    Id16 entropy{};
    std::string connection_public_id;
    std::int64_t connection_database_id{};
    std::uint64_t duplicate_source_id{};
    std::optional<std::int64_t> worker_database_id;
    std::int64_t template_database_id{};
    ParsedBlock block;
    Bytes hashing_blob;
    std::uint64_t height{};
    Hash32 seed_hash{};
    mspv_seed_id seed_id{};
    std::uint64_t assigned_difficulty{};
    std::uint64_t target64{};
    std::string network_difficulty;
    std::int64_t created_unix_us{};
    std::int64_t expires_unix_us{};
    bool duplicate_bucket_retained{};
};

struct Runtime::JobLease {
    Runtime *runtime{};
    std::shared_ptr<JobContext> job;
    std::int64_t connection_database_id{};
    std::optional<std::int64_t> worker_database_id;
    bool seed_retained{};
    bool duplicate_height_retained{};

    JobLease(Runtime &owner, std::shared_ptr<JobContext> admitted_job)
        : runtime(&owner), job(std::move(admitted_job)),
          connection_database_id(job->connection_database_id),
          worker_database_id(job->worker_database_id)
    {
        if (runtime->verifier_) {
            if (runtime->verifier_->retain_seed(job->seed_id) != MSPV_OK) {
                throw ValidationError(
                    "verifier could not retain the admitted job seed");
            }
            seed_retained = true;
        }
        try {
            runtime->duplicates_.retain_height(job->duplicate_source_id,
                                               job->height);
            duplicate_height_retained = true;
        }
        catch (...) {
            if (seed_retained) {
                (void)runtime->verifier_->drop_seed_reference(job->seed_id);
                seed_retained = false;
            }
            throw;
        }
    }

    JobLease(Runtime &owner, std::int64_t connection_id,
             std::optional<std::int64_t> worker_id) noexcept
        : runtime(&owner), connection_database_id(connection_id),
          worker_database_id(worker_id) {}

    JobLease(const JobLease &) = delete;
    JobLease &operator=(const JobLease &) = delete;

    ~JobLease()
    {
        if (duplicate_height_retained) {
            try {
                runtime->persist_retired_duplicates(
                    runtime->duplicates_.release_height(
                        job->duplicate_source_id, job->height),
                    unix_time_us());
            }
            catch (...) {
                runtime->mark_database_unavailable();
            }
        }
        if (seed_retained && runtime->verifier_) {
            try {
                if (runtime->verifier_->drop_seed_reference(job->seed_id) !=
                    MSPV_OK) {
                    runtime->ready_.store(false, std::memory_order_release);
                }
            }
            catch (...) {
                runtime->ready_.store(false, std::memory_order_release);
            }
        }
    }
};

struct Runtime::CandidateTask {
private:
    class InflightReference final {
    public:
        InflightReference() = default;
        InflightReference(Runtime &runtime, const PeerAddress &peer) noexcept
            : runtime_(&runtime), peer_(peer) {}

        InflightReference(const InflightReference &) = delete;
        InflightReference &operator=(const InflightReference &) = delete;

        InflightReference(InflightReference &&other) noexcept
            : runtime_(std::exchange(other.runtime_, nullptr)),
              peer_(other.peer_) {}

        InflightReference &operator=(InflightReference &&other) noexcept
        {
            if (this == &other) return *this;
            reset();
            runtime_ = std::exchange(other.runtime_, nullptr);
            peer_ = other.peer_;
            return *this;
        }

        ~InflightReference() { reset(); }

        void reset() noexcept
        {
            Runtime *runtime = std::exchange(runtime_, nullptr);
            if (runtime != nullptr) runtime->defense_.candidate_finished(peer_);
        }

        [[nodiscard]] bool held() const noexcept { return runtime_ != nullptr; }

    private:
        Runtime *runtime_{};
        PeerAddress peer_{};
    };

    class DuplicateHeightReference final {
    public:
        DuplicateHeightReference() = default;

        DuplicateHeightReference(Runtime &runtime, std::uint64_t source_id,
                                 std::uint64_t height)
            : source_id_(source_id), height_(height)
        {
            runtime.duplicates_.retain_height(source_id_, height_);
            runtime_ = &runtime;
        }

        DuplicateHeightReference(const DuplicateHeightReference &) = delete;
        DuplicateHeightReference &operator=(
            const DuplicateHeightReference &) = delete;

        DuplicateHeightReference(DuplicateHeightReference &&other) noexcept
            : runtime_(std::exchange(other.runtime_, nullptr)),
              source_id_(other.source_id_), height_(other.height_) {}

        DuplicateHeightReference &operator=(
            DuplicateHeightReference &&other) noexcept
        {
            if (this == &other) return *this;
            reset();
            runtime_ = std::exchange(other.runtime_, nullptr);
            source_id_ = other.source_id_;
            height_ = other.height_;
            return *this;
        }

        ~DuplicateHeightReference() { reset(); }

        void reset() noexcept
        {
            Runtime *runtime = std::exchange(runtime_, nullptr);
            if (runtime == nullptr) return;
            try {
                runtime->persist_retired_duplicates(
                    runtime->duplicates_.release_height(source_id_, height_),
                    unix_time_us());
            }
            catch (...) {
                // release_height is strongly exception-safe.  If its temporary
                // token allocation fails, leave the durable candidate for
                // startup recovery and stop this process from issuing work.
                runtime->mark_database_unavailable();
            }
        }

        [[nodiscard]] bool held() const noexcept { return runtime_ != nullptr; }

    private:
        Runtime *runtime_{};
        std::uint64_t source_id_{};
        std::uint64_t height_{};
    };

public:
    CandidateTask() = default;
    CandidateTask(const CandidateTask &) = delete;
    CandidateTask &operator=(const CandidateTask &) = delete;
    CandidateTask(CandidateTask &&) noexcept = default;
    CandidateTask &operator=(CandidateTask &&) noexcept = default;

    void adopt_candidate_inflight(Runtime &runtime,
                                  const PeerAddress &candidate_peer) noexcept
    {
        inflight_reference = InflightReference(runtime, candidate_peer);
    }

    void retain_duplicate_height(Runtime &runtime, std::uint64_t source_id,
                                 std::uint64_t candidate_height)
    {
        duplicate_height_reference = DuplicateHeightReference(
            runtime, source_id, candidate_height);
    }

    void release_candidate_inflight() noexcept { inflight_reference.reset(); }
    void release_duplicate_height() noexcept
    {
        duplicate_height_reference.reset();
    }

    [[nodiscard]] CandidateTask make_reference_free_lookup() const noexcept
    {
        CandidateTask lookup;
        lookup.candidate_id = candidate_id;
        lookup.share_id = share_id;
        lookup.job_id = job_id;
        lookup.connection_id = connection_id;
        lookup.duplicate_source_id = duplicate_source_id;
        lookup.height = height;
        lookup.peer = peer;
        lookup.candidate_key = candidate_key;
        lookup.miner_tx_hash = miner_tx_hash;
        lookup.expected_block_id = expected_block_id;
        lookup.max_attempts = max_attempts;
        lookup.attempt_count = attempt_count;
        lookup.reconciliation_cycle = reconciliation_cycle;
        lookup.terminal_reconciliation_cycle = terminal_reconciliation_cycle;
        lookup.created_unix_us = created_unix_us;
        lookup.reconciliation_only = true;
        lookup.quick_reconciliation = true;
        lookup.resume_after_reconciliation = false;
        return lookup;
    }

    std::int64_t candidate_id{};
    std::int64_t share_id{};
    std::int64_t job_id{};
    std::int64_t connection_id{};
    std::uint64_t duplicate_source_id{};
    std::uint64_t height{};
    PeerAddress peer;
    Bytes frozen_block;
    Hash32 candidate_key{};
    Hash32 miner_tx_hash{};
    std::optional<Hash32> expected_block_id;
    std::uint32_t max_attempts{4};
    std::uint32_t attempt_count{};
    std::uint32_t reconciliation_cycle{};
    std::uint32_t terminal_reconciliation_cycle{};
    std::int64_t created_unix_us{};
    bool reconciliation_only{};
    bool quick_reconciliation{};
    bool resume_after_reconciliation{};
    std::chrono::steady_clock::time_point not_before{};

private:
    // Declare the duplicate reference first so ordinary reverse destruction
    // releases admission before it potentially performs durable retirement.
    DuplicateHeightReference duplicate_height_reference;
    InflightReference inflight_reference;
};

Runtime::Runtime(Config config)
    : config_(resolve_runtime_workers(std::move(config))),
      logger_(logging::parse_severity(config_.logging.level),
              config_.logging.file),
      database_({config_.database.path,
                 static_cast<std::uint32_t>(config_.database.busy_timeout_ms),
                 config_.blocknotify.has_value() && !config_.blocknotify->empty(),
                 config_.database.max_writer_queue_items,
                 config_.database.max_writer_queue_bytes,
                 config_.defense.candidate_global_inflight +
                     (config_.verifier.enabled
                          ? config_.verifier.max_outstanding
                          : 0U)}),
      entropy_({static_cast<std::uint32_t>(config_.entropy.reseed_interval_seconds),
                static_cast<std::uint32_t>(config_.entropy.max_reseed_age_seconds),
                static_cast<std::uint32_t>(config_.entropy.max_generate_calls)}),
      daemon_(config_.daemon.rpc_url,
              config_.daemon.rpc_username.value_or(""),
              config_.daemon.rpc_password.value_or(""),
              static_cast<std::uint32_t>(config_.daemon.request_timeout_ms),
              static_cast<std::size_t>(config_.daemon.max_response_bytes),
              static_cast<std::uint32_t>(config_.daemon.max_concurrent_requests),
              static_cast<std::uint32_t>(config_.daemon.max_pending_requests)),
      duplicates_(),
      defense_(defense_config(config_), [this](const BanRecord &record) {
          if (session_id_ == 0) return;
          try {
              const auto created = std::chrono::duration_cast<std::chrono::microseconds>(
                  record.created.time_since_epoch()).count();
              const auto expires = std::chrono::duration_cast<std::chrono::microseconds>(
                  record.expires.time_since_epoch()).count();
              const auto start = std::chrono::duration_cast<std::chrono::microseconds>(
                  record.evidence_start.time_since_epoch()).count();
              const auto end = std::chrono::duration_cast<std::chrono::microseconds>(
                  record.evidence_end.time_since_epoch()).count();
              const std::string reason = abuse_name(record.reason);
              ByteVector address(record.peer.bytes.begin(), record.peer.bytes.begin() +
                                  static_cast<std::ptrdiff_t>(record.peer.size));
              const auto event = database_.insert_abuse_event({
                  .connection_id = std::nullopt,
                  .share_id = std::nullopt,
                  .candidate_id = std::nullopt,
                  .peer_family = record.peer.family, .peer_address = address,
                  .kind = reason, .weight = 1,
                  .created_unix_us = created,
                  .detail = std::nullopt});
              (void)database_.create_ban({
                  .peer_family = record.peer.family, .peer_address = address,
                  .created_unix_us = created, .expires_unix_us = expires,
                  .evidence_window_started_unix_us = start,
                  .evidence_window_ended_unix_us = end,
                  .reason = reason, .abuse_event_ids = {event}});
              logger_.log(
                  logging::Severity::warning, "defense.ban_created",
                  {{logging::PublicStringKey::peer, record.peer.text()},
                   {logging::PublicStringKey::reason_code, reason}});
          }
          catch (...) {
              logger_.log(logging::Severity::error,
                          "defense.ban_persist_failed",
                          {{logging::PublicStringKey::reason_code,
                            "database_write_failed"}});
              mark_database_unavailable();
          }
      }),
      verifier_mailbox_(static_cast<std::size_t>(config_.verifier.max_outstanding)) {}

Runtime::~Runtime() { stop(); }

bool Runtime::running() const noexcept { return running_.load(std::memory_order_acquire); }
bool Runtime::ready() const noexcept { return ready_.load(std::memory_order_acquire); }

void Runtime::update_readiness() noexcept {
    const bool stratum_ready =
        stratum_operational_.load(std::memory_order_acquire);
    ready_.store(
        running() && startup_complete_.load(std::memory_order_acquire) &&
            database_operational_.load(std::memory_order_acquire) &&
            template_operational_.load(std::memory_order_acquire) &&
            entropy_.issuance_allowed() &&
            job_issuance_allowed_.load(std::memory_order_acquire) &&
            stratum_ready &&
            (!config_.verifier.enabled ||
             verifier_operational_.load(std::memory_order_acquire)),
        std::memory_order_release);
}

void Runtime::mark_database_unavailable() noexcept {
    database_operational_.store(false, std::memory_order_release);
    internal_failure_.store(true, std::memory_order_release);
    // A durable-writer/candidate-commit failure is process-fatal for mining.
    // The main control loop observes this and performs the normal ordered stop;
    // candidate workers are independently gated below in the meantime.
    running_.store(false, std::memory_order_release);
    update_readiness();
}

void Runtime::validate_daemon_network() {
    const RpcObservation observation = daemon_.get_info();
    const nlohmann::json &result = rpc_result(observation);
    const std::string observed = required_string(result, "nettype");
    const std::string expected = config_.network == Network::regtest
                                     ? "fakechain" : network_name(config_.network);
    if (observed != expected) {
        throw ValidationError("daemon network is " + observed +
                              ", configured network requires " + expected);
    }
}

std::shared_ptr<Runtime::TemplateContext> Runtime::parse_template(
    const RpcObservation &observation, std::string reason) {
    const nlohmann::json &result = rpc_result(observation);
    const Bytes block_bytes = hex_decode(required_string(result, "blocktemplate_blob"));
    const Bytes daemon_hashing = hex_decode(required_string(result, "blockhashing_blob"));
    if (block_bytes.empty() || daemon_hashing.empty()) {
        throw ValidationError("daemon returned an empty block template blob");
    }
    ParsedBlock block = parse_block(block_bytes);
    const std::uint64_t height = positive_u64(result, "height");
    if (block.miner_transaction.height != height) {
        throw ValidationError("template height differs from coinbase height");
    }
    const Hash32 previous = hex_decode_array<32>(required_string(result, "prev_hash"));
    if (block.previous_hash != previous) {
        throw ValidationError("template previous hash differs from parsed block");
    }
    const Hash32 seed = hex_decode_array<32>(required_string(result, "seed_hash"));
    std::optional<Hash32> next_seed;
    if (const auto next = result.find("next_seed_hash");
        next != result.end() && !next->is_null()) {
        if (!next->is_string()) throw ValidationError("next_seed_hash has an invalid type");
        const std::string encoded = next->get<std::string>();
        // monerod serializes this field as an empty string when there is no
        // distinct next RandomX seed.  Null and empty therefore both mean
        // absent; a nonempty value remains strict 32-byte hexadecimal.
        if (!encoded.empty()) next_seed = hex_decode_array<32>(encoded);
    }
    const std::uint64_t reserved = positive_u64(result, "reserved_offset");
    if (reserved > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        !reserved_offset_is_exact_extra_nonce(
            block, static_cast<std::size_t>(reserved), 16U)) {
        throw ValidationError("reserved_offset is not the exact 16-byte extra nonce");
    }
    const Bytes local_hashing = block_hashing_blob(block);
    if (local_hashing != daemon_hashing) {
        throw ValidationError("locally regenerated hashing blob differs from daemon");
    }

    std::optional<std::string> wide_hex;
    std::string difficulty;
    if (const auto wide = result.find("wide_difficulty");
        wide != result.end() && !wide->is_null()) {
        if (!wide->is_string()) throw ValidationError("wide_difficulty has an invalid type");
        std::string encoded = wide->get<std::string>();
        if (encoded.starts_with("0x") || encoded.starts_with("0X")) encoded.erase(0, 2);
        if (encoded.empty() || encoded.size() > 32U) {
            throw ValidationError("wide_difficulty is outside uint128 range");
        }
        std::transform(encoded.begin(), encoded.end(), encoded.begin(),
                       [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
        const auto first_nonzero = encoded.find_first_not_of('0');
        encoded = first_nonzero == std::string::npos ? "0" : encoded.substr(first_nonzero);
        difficulty = decimal_from_hex(encoded);
        wide_hex = std::move(encoded);
    }
    else {
        const auto value = result.find("difficulty");
        if (value == result.end()) throw ValidationError("daemon template omitted difficulty");
        difficulty = decimal_json(*value);
    }

    mspv_seed_id seed_id = 0;
    if (verifier_) {
        // The daemon's advertised next seed is speculative and may change
        // across an inconsistent response or reorg. Reclaim any abandoned
        // zero-reference prefetch before admitting the actual current seed;
        // otherwise max_seeds=2 can be wedged by current + stale future.
        mspv_seed_id active_seed = 0;
        {
            std::lock_guard lock(state_mutex_);
            active_seed = active_seed_id_;
        }
        const auto resident_seeds = verifier_->seed_snapshots();
        bool actual_seed_differs = active_seed == 0;
        bool actual_seed_ready = false;
        for (const auto &resident : resident_seeds) {
            if (resident.seed_id == active_seed &&
                resident.seed_hash == seed) {
                actual_seed_differs = false;
            }
            if (resident.seed_hash == seed &&
                (resident.state == MSPV_SEED_READY ||
                 resident.state == MSPV_SEED_CURRENT)) {
                actual_seed_ready = true;
            }
        }
        if (actual_seed_differs && !actual_seed_ready) {
            // Synchronize with complete job derivations: once this exclusive
            // section begins, no new job can retain the old seed until the new
            // template and its ready seed are installed together below.
            std::unique_lock issuance_lock(job_issuance_mutex_);
            job_issuance_allowed_.store(false, std::memory_order_release);
            update_readiness();
        }
        for (const auto &resident : resident_seeds) {
            const bool still_relevant = resident.seed_hash == seed ||
                (next_seed.has_value() && resident.seed_hash == *next_seed);
            if (resident.seed_id == active_seed || still_relevant ||
                resident.retained_job_references != 0U ||
                resident.tracked_verifications != 0U) {
                continue;
            }
            const auto release = verifier_->request_seed_release(resident.seed_id);
            if (release != MSPV_OK && release != MSPV_SEED_NOT_FOUND) {
                throw ValidationError("verifier could not release an obsolete seed");
            }
            const auto released = verifier_->wait_seed_released(
                resident.seed_id,
                static_cast<std::uint32_t>(config_.daemon.request_timeout_ms));
            if (released != MSPV_OK && released != MSPV_SEED_NOT_FOUND) {
                throw ValidationError("verifier obsolete seed release timed out");
            }
        }
        const auto prepared = verifier_->prepare_seed(seed);
        if (prepared.status != MSPV_OK || prepared.seed.seed_id == 0) {
            throw ValidationError("verifier could not prepare the template seed");
        }
        seed_id = prepared.seed.seed_id;
        const auto status = verifier_->wait_seed_ready(
            seed_id, static_cast<std::uint32_t>(config_.daemon.request_timeout_ms));
        if (status != MSPV_OK || verifier_->activate_seed(seed_id) != MSPV_OK) {
            throw ValidationError("verifier seed preparation failed");
        }
        if (next_seed.has_value() && *next_seed != seed) {
            try { (void)verifier_->prepare_seed(*next_seed); } catch (...) {}
        }
    }

    const std::uint64_t generation =
        template_generation_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (generation > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw ValidationError("template generation overflow");
    }
    const std::int64_t fetched = unix_time_us();
    std::int64_t template_id = 0;
    try {
        template_id = database_.insert_public_template({
            .session_id = session_id_,
            .generation = static_cast<std::int64_t>(generation),
            .height = height,
            .prev_hash = previous,
            .seed_hash = seed,
            .next_seed_hash = next_seed,
            .difficulty_dec = difficulty,
            .wide_difficulty_hex = wide_hex,
            .reserved_offset = reserved,
            .blocktemplate_blob = block_bytes,
            .blockhashing_blob = daemon_hashing,
            .fetched_unix_us = fetched,
            .fetch_reason = std::move(reason),
        });
    }
    catch (const DatabaseError &) {
        mark_database_unavailable();
        throw;
    }

    auto context = std::make_shared<TemplateContext>();
    context->database_id = template_id;
    context->generation = generation;
    context->block = std::move(block);
    context->hashing_blob = daemon_hashing;
    context->reserved_offset = static_cast<std::size_t>(reserved);
    context->height = height;
    context->previous_hash = previous;
    context->seed_hash = seed;
    context->next_seed_hash = next_seed;
    context->network_difficulty = std::move(difficulty);
    context->wide_difficulty_hex = std::move(wide_hex);
    context->seed_id = seed_id;
    return context;
}

void Runtime::refresh_template(std::string reason) {
    const RpcObservation observation = daemon_.get_block_template(config_.wallet_address);
    auto next = parse_template(observation, std::move(reason));
    mspv_seed_id old_seed = 0;
    {
        std::lock_guard lock(state_mutex_);
        old_seed = active_seed_id_;
        active_seed_id_ = next->seed_id;
        current_template_ = next;
        current_height_.store(next->height, std::memory_order_release);
        last_error_.reset();
    }
    {
        std::unique_lock issuance_lock(job_issuance_mutex_);
        job_issuance_allowed_.store(true, std::memory_order_release);
    }
    if (verifier_ && old_seed != 0 && old_seed != next->seed_id) {
        (void)verifier_->request_seed_release(old_seed);
    }
    if (stratum_) stratum_->refresh_jobs();
    emit("template_refresh", {{"height", next->height},
                               {"generation", std::to_string(next->generation)}});
    logger_.log(logging::Severity::info, "template.refreshed", {},
                {{logging::IntegerKey::template_id,
                  log_id(next->database_id)},
                 {logging::IntegerKey::height, next->height},
                 {logging::IntegerKey::generation, next->generation},
                 {logging::IntegerKey::seed_id,
                  static_cast<std::uint64_t>(next->seed_id)}});
    template_operational_.store(true, std::memory_order_release);
    update_readiness();
}

std::optional<StratumJob> Runtime::make_job(const MinerConnection &connection) {
    std::shared_lock issuance_lock(job_issuance_mutex_);
    if (!job_issuance_allowed_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    try {
        std::shared_ptr<TemplateContext> snapshot;
        std::int64_t connection_id = 0;
        std::optional<std::int64_t> worker_id;
        {
            std::lock_guard lock(state_mutex_);
            snapshot = current_template_;
            const auto found = connection_ids_.find(connection.public_id);
            if (found != connection_ids_.end()) connection_id = found->second;
            const auto worker = worker_ids_.find(connection.public_id);
            if (worker != worker_ids_.end()) worker_id = worker->second;
        }
        if (!snapshot || connection_id <= 0 || !entropy_.issuance_allowed()) {
            return std::nullopt;
        }
        const std::int64_t created = unix_time_us();
        const auto ttl_us = static_cast<std::int64_t>(config_.stratum.job_ttl_ms) * 1000;
        for (unsigned attempt = 0; attempt < 8U; ++attempt) {
            const Id16 entropy = entropy_.private_template_entropy();
            const Id16 job_id = entropy_.private_job_id();
            ParsedBlock private_block = mutate_reserved_bytes(
                snapshot->block, snapshot->reserved_offset, entropy);
            Bytes hashing = block_hashing_blob(private_block);
            bool seed_retained = false;
            bool durable_committed = false;
            std::int64_t database_id = 0;
            std::shared_ptr<JobContext> context;
            if (verifier_) {
                if (verifier_->retain_seed(snapshot->seed_id) != MSPV_OK) return std::nullopt;
                seed_retained = true;
            }
            ScopeExit rollback([&]() noexcept {
                bool cleanup_failed = false;
                if (context && context->duplicate_bucket_retained) {
                    try {
                        persist_retired_duplicates(
                            duplicates_.retire_height(
                                context->duplicate_source_id, context->height),
                            unix_time_us());
                        persist_retired_duplicates(
                            duplicates_.release_height(
                                context->duplicate_source_id, context->height),
                            unix_time_us());
                        context->duplicate_bucket_retained = false;
                    }
                    catch (...) {
                        cleanup_failed = true;
                    }
                }
                if (database_id > 0) {
                    try {
                        const JobRetirementResult retirement = database_.retire_job(
                            database_id, unix_time_us());
                        apply_actionable_verdicts(retirement.actionable_verdicts);
                    }
                    catch (...) {
                        cleanup_failed = true;
                    }
                }
                if (seed_retained && verifier_) {
                    try {
                        if (verifier_->drop_seed_reference(snapshot->seed_id) !=
                            MSPV_OK) {
                            cleanup_failed = true;
                        }
                    }
                    catch (...) {
                        cleanup_failed = true;
                    }
                    seed_retained = false;
                }
                if (cleanup_failed) mark_database_unavailable();
            });
            try {
                database_id = database_.insert_private_job({
                    .public_job_id = job_id,
                    .connection_id = connection_id,
                    .template_id = snapshot->database_id,
                    .height = snapshot->height,
                    .entropy = entropy,
                    .seed_hash = snapshot->seed_hash,
                    .mspv_seed_id_dec = verifier_
                        ? std::optional<std::string>(std::to_string(snapshot->seed_id))
                        : std::nullopt,
                    .assigned_difficulty_dec = std::to_string(connection.assigned_difficulty),
                    .target64_le = share_target_le(connection.assigned_difficulty),
                    .network_difficulty_dec = snapshot->network_difficulty,
                    .nonce_offset = private_block.nonce_offset,
                    .reserved_offset = snapshot->reserved_offset,
                    .private_block_blob = private_block.blob,
                    .hashing_blob = hashing,
                    .created_unix_us = created,
                    .queued_unix_us = std::nullopt,
                    .expires_unix_us = created + ttl_us,
                });
                durable_committed = true;
                context = std::make_shared<JobContext>();
                context->database_id = database_id;
                context->public_id = job_id;
                context->entropy = entropy;
                context->connection_public_id = connection.public_id;
                context->connection_database_id = connection_id;
                context->duplicate_source_id = static_cast<std::uint64_t>(
                    connection_id);
                context->worker_database_id = worker_id;
                context->template_database_id = snapshot->database_id;
                context->block = std::move(private_block);
                context->hashing_blob = std::move(hashing);
                context->height = snapshot->height;
                context->seed_hash = snapshot->seed_hash;
                context->seed_id = snapshot->seed_id;
                context->assigned_difficulty = connection.assigned_difficulty;
                context->target64 = share_target64(connection.assigned_difficulty);
                context->network_difficulty = snapshot->network_difficulty;
                context->created_unix_us = created;
                context->expires_unix_us = created + ttl_us;
                duplicates_.retain_height(context->duplicate_source_id,
                                          context->height);
                context->duplicate_bucket_retained = true;
                const std::string encoded_id = hex_encode(job_id);
                StratumJob job;
                job.blob = hex_encode(context->hashing_blob);
                job.job_id = encoded_id;
                job.target = share_target_hex(connection.assigned_difficulty);
                job.seed_hash = hex_encode(snapshot->seed_hash);
                job.height = snapshot->height;
                job.network_difficulty = context->network_difficulty;
                job.on_queued = [this, database_id] {
                    try {
                        database_.mark_job_queued(database_id, unix_time_us());
                        emit("job_sent", {}, std::nullopt, database_id);
                    }
                    catch (...) { mark_database_unavailable(); throw; }
                };
                detail::run_job_post_commit_fault_hook();

                std::shared_ptr<JobContext> retired_job;
                {
                    std::lock_guard lock(state_mutex_);
                    const auto [job_iterator, inserted] = jobs_.emplace(
                        encoded_id, context);
                    if (!inserted) {
                        throw DatabaseError(
                            "private job identity already exists in runtime state");
                    }
                    try {
                        const auto [history_iterator, history_created] =
                            connection_jobs_.try_emplace(connection.public_id);
                        try {
                            auto &history = history_iterator->second;
                            history.push_front(encoded_id);
                            if (history.size() > config_.stratum.job_history) {
                                std::string retired = std::move(history.back());
                                history.pop_back();
                                const auto old = jobs_.find(retired);
                                if (old != jobs_.end()) {
                                    retired_job = old->second;
                                    jobs_.erase(old);
                                }
                            }
                        }
                        catch (...) {
                            if (history_created &&
                                history_iterator->second.empty()) {
                                connection_jobs_.erase(history_iterator);
                            }
                            throw;
                        }
                    }
                    catch (...) {
                        jobs_.erase(job_iterator);
                        throw;
                    }
                }
                rollback.release();
                if (retired_job) retire_job_context(retired_job, created);
                emit("job_derived", {{"height", context->height}},
                     connection_id, database_id);
                return job;
            }
            catch (const DatabaseError &) {
                if (attempt + 1U == 8U) throw;
            }
            catch (...) {
                if (durable_committed) {
                    internal_failure_.store(true, std::memory_order_release);
                    running_.store(false, std::memory_order_release);
                    ready_.store(false, std::memory_order_release);
                }
                throw;
            }
        }
    }
    catch (const std::exception &error) {
        if (dynamic_cast<const DatabaseError *>(&error) != nullptr) {
            mark_database_unavailable();
        }
        std::lock_guard lock(state_mutex_);
        last_error_ = error.what();
        ready_.store(false, std::memory_order_release);
    }
    return std::nullopt;
}

StratumAdmission Runtime::admit_submission(
    const StratumSubmission &submission)
{
    std::lock_guard lock(state_mutex_);
    const auto connection = connection_ids_.find(
        submission.connection.public_id);
    if (connection == connection_ids_.end()) {
        mark_database_unavailable();
        throw DatabaseError(
            "submission admission has no durable connection");
    }
    const auto unknown_job = [&] {
        std::optional<std::int64_t> worker_id;
        if (const auto worker = worker_ids_.find(
                submission.connection.public_id);
            worker != worker_ids_.end()) {
            worker_id = worker->second;
        }
        auto lease = std::make_shared<JobLease>(
            *this, connection->second, worker_id);
        return StratumAdmission{
            .job_lease = std::static_pointer_cast<void>(lease),
            .network_difficulty = {},
        };
    };
    const auto found = jobs_.find(submission.job_id);
    if (found == jobs_.end() ||
        found->second->connection_public_id !=
            submission.connection.public_id) {
        return unknown_job();
    }
    {
        const auto history = connection_jobs_.find(
            submission.connection.public_id);
        const bool current = connection_ids_.contains(
                                 submission.connection.public_id) &&
                             history != connection_jobs_.end() &&
                             !history->second.empty() &&
                             history->second.front() == submission.job_id;
        if (!current && submission.received_unix_us >=
                            found->second->expires_unix_us) {
            return unknown_job();
        }
    }
    // Construct while state_mutex_ still excludes job retirement.  The extra
    // seed and duplicate-height references therefore become live before the
    // job's process-local references can be dropped.
    auto lease = std::make_shared<JobLease>(*this, found->second);
    return StratumAdmission{
        .job_lease = std::static_pointer_cast<void>(lease),
        .network_difficulty = lease->job->network_difficulty,
    };
}

void Runtime::persist_retired_duplicates(
    std::vector<DuplicateToken> tokens,
    std::int64_t retired_unix_us) noexcept
{
    for (const DuplicateToken &token : tokens) {
        try {
            if (!database_.retire_duplicate(
                    token.key, static_cast<std::int64_t>(token.generation),
                    retired_unix_us)) {
                // A false result is safe when a provisional reservation was
                // already retired or another process generation reused its
                // key. The generation predicate prevents retiring that newer
                // durable reservation.
            }
        }
        catch (const std::exception &error) {
            mark_database_unavailable();
            std::lock_guard lock(state_mutex_);
            last_error_ = error.what();
            ready_.store(false, std::memory_order_release);
        }
    }
}

void Runtime::apply_actionable_verdicts(
    const std::vector<ActionableCandidateVerdict> &verdicts) noexcept
{
    try {
        for (const ActionableCandidateVerdict &verdict : verdicts) {
            const std::size_t expected_size = verdict.peer_family == AF_INET
                                                  ? 4U
                                                  : verdict.peer_family == AF_INET6
                                                        ? 16U
                                                        : 0U;
            if (expected_size == 0U ||
                verdict.peer_address.size() != expected_size) {
                throw DatabaseError(
                    "retired candidate verdict has an invalid peer address");
            }
            PeerAddress peer;
            peer.family = verdict.peer_family;
            peer.size = expected_size;
            std::copy(verdict.peer_address.begin(), verdict.peer_address.end(),
                      peer.bytes.begin());
            defense_.record(
                peer, verdict.kind == CandidateVerdictKind::false_candidate
                          ? AbuseKind::verified_false_candidate
                          : AbuseKind::candidate_mismatch);
        }
    }
    catch (const std::exception &error) {
        mark_database_unavailable();
        std::lock_guard lock(state_mutex_);
        last_error_ = error.what();
        ready_.store(false, std::memory_order_release);
    }
}

void Runtime::retire_job_context(const std::shared_ptr<JobContext> &job,
                                 std::int64_t retired_unix_us) noexcept {
    if (!job) return;
    if (job->duplicate_bucket_retained) {
        try {
            persist_retired_duplicates(
                duplicates_.retire_height(job->duplicate_source_id, job->height),
                retired_unix_us);
            persist_retired_duplicates(
                duplicates_.release_height(job->duplicate_source_id, job->height),
                retired_unix_us);
            job->duplicate_bucket_retained = false;
        }
        catch (...) {
            internal_failure_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            ready_.store(false, std::memory_order_release);
            return;
        }
    }
    try {
        const JobRetirementResult retirement = database_.retire_job(
            job->database_id, retired_unix_us);
        apply_actionable_verdicts(retirement.actionable_verdicts);
    }
    catch (const std::exception &error) {
        mark_database_unavailable();
        std::lock_guard lock(state_mutex_);
        last_error_ = error.what();
        ready_.store(false, std::memory_order_release);
    }
    if (verifier_) {
        try {
            const mspv_status status = verifier_->drop_seed_reference(job->seed_id);
            if (status != MSPV_OK) ready_.store(false, std::memory_order_release);
        }
        catch (...) { ready_.store(false, std::memory_order_release); }
    }
}

void Runtime::observe_connection(const MinerConnection &connection,
                                 std::string_view event) {
    try {
        const std::int64_t now = unix_time_us();
        if (event == "opened") {
            const PublicId public_id = hex_decode_array<16>(connection.public_id);
            ByteVector address(connection.peer.bytes.begin(),
                               connection.peer.bytes.begin() +
                                   static_cast<std::ptrdiff_t>(connection.peer.size));
            const auto id = database_.insert_connection({
                .public_id = public_id,
                .session_id = session_id_,
                .worker_id = std::nullopt,
                .peer_family = connection.peer.family,
                .peer_address = std::move(address),
                .peer_port = connection.peer_port,
                .listen_address = connection.listen_address,
                .agent = "",
                .opened_unix_us = now,
            });
            std::lock_guard lock(state_mutex_);
            disconnected_connections_.erase(connection.public_id);
            connection_ids_[connection.public_id] = id;
            emit("connection_opened", {}, id);
            return;
        }
        if (event == "submit_queued") {
            std::lock_guard lock(state_mutex_);
            ++accepted_submits_[connection.public_id];
            return;
        }
        if (event == "submit_completed") {
            std::deque<std::string> retired;
            {
                std::lock_guard lock(state_mutex_);
                const auto pending = accepted_submits_.find(connection.public_id);
                if (pending != accepted_submits_.end()) {
                    if (pending->second > 1U) {
                        --pending->second;
                    }
                    else {
                        accepted_submits_.erase(pending);
                        if (disconnected_connections_.erase(
                                connection.public_id) != 0U) {
                            connection_ids_.erase(connection.public_id);
                            worker_ids_.erase(connection.public_id);
                            pending_verifications_.erase(connection.public_id);
                            const auto history = connection_jobs_.find(
                                connection.public_id);
                            if (history != connection_jobs_.end()) {
                                retired.swap(history->second);
                                connection_jobs_.erase(history);
                            }
                        }
                    }
                }
            }
            for (const auto &id : retired) {
                std::shared_ptr<JobContext> job;
                {
                    std::lock_guard lock(state_mutex_);
                    const auto found = jobs_.find(id);
                    if (found != jobs_.end()) {
                        job = found->second;
                        jobs_.erase(found);
                    }
                }
                retire_job_context(job, now);
            }
            return;
        }
        std::int64_t connection_id = 0;
        {
            std::lock_guard lock(state_mutex_);
            const auto found = connection_ids_.find(connection.public_id);
            if (found != connection_ids_.end()) connection_id = found->second;
        }
        if (connection_id <= 0) return;
        if (event == "authenticated") {
            const auto worker_id = database_.upsert_worker({
                .login = connection.login, .rigid = connection.rigid,
                .seen_unix_us = now});
            database_.authenticate_connection(connection_id, worker_id,
                                               connection.agent, now);
            emit("login_succeeded", {}, connection_id);
            std::lock_guard lock(state_mutex_);
            worker_ids_[connection.public_id] = worker_id;
            if (const auto history = connection_jobs_.find(connection.public_id);
                history != connection_jobs_.end()) {
                for (const auto &job_id : history->second) {
                    if (const auto job = jobs_.find(job_id); job != jobs_.end()) {
                        job->second->worker_database_id = worker_id;
                    }
                }
            }
            return;
        }
        (void)database_.close_connection(connection_id, now, event);
        emit("connection_closed", {{"reason", event}}, connection_id);
        std::deque<std::string> retired;
        {
            std::lock_guard lock(state_mutex_);
            if (accepted_submits_.contains(connection.public_id)) {
                disconnected_connections_.insert(connection.public_id);
            }
            else {
                connection_ids_.erase(connection.public_id);
                worker_ids_.erase(connection.public_id);
                pending_verifications_.erase(connection.public_id);
                const auto history = connection_jobs_.find(connection.public_id);
                if (history != connection_jobs_.end()) {
                    retired.swap(history->second);
                    connection_jobs_.erase(history);
                }
            }
        }
        for (const auto &id : retired) {
            std::shared_ptr<JobContext> job;
            {
                std::lock_guard lock(state_mutex_);
                const auto found = jobs_.find(id);
                if (found != jobs_.end()) {
                    job = found->second;
                    jobs_.erase(found);
                }
            }
            retire_job_context(job, now);
        }
    }
    catch (const std::exception &error) {
        mark_database_unavailable();
        std::lock_guard lock(state_mutex_);
        last_error_ = error.what();
        ready_.store(false, std::memory_order_release);
    }
}

CandidateJournalResult Runtime::journal_candidate(
    std::int64_t share_id, const std::shared_ptr<JobContext> &job,
    const MinerConnection &connection, const ParsedBlock &frozen,
    bool claimed_path, bool bypass_admission,
    std::optional<bool> *admission_acquired) {
    const Hash32 key = make_candidate_key(frozen.blob);
    if (const auto existing = database_.find_candidate_by_key(key); existing.has_value()) {
        database_.attach_share_to_candidate(share_id, existing->candidate_id, "existing");
        logger_.log(
            logging::Severity::debug, "candidate.journal_reused",
            {{logging::PublicStringKey::candidate_key, hex_encode(key)},
             {logging::PublicStringKey::state,
              candidate_state_name(existing->state)}},
            {{logging::IntegerKey::candidate_id,
              log_id(existing->candidate_id)},
             {logging::IntegerKey::share_id, log_id(share_id)},
             {logging::IntegerKey::job_id, log_id(job->database_id)},
             {logging::IntegerKey::height, job->height}});
        if (admission_acquired != nullptr) *admission_acquired = false;
        return *existing;
    }

    bool acquired = false;
    CandidateTask task;
    if (claimed_path && !bypass_admission) {
        acquired = defense_.admit_candidate(connection.peer);
        if (!acquired) {
            database_.set_candidate_admission(
                share_id, verifier_ ? "deferred" : "trusted_rate_limited");
            if (admission_acquired != nullptr) *admission_acquired = false;
            return {};
        }
        task.adopt_candidate_inflight(*this, connection.peer);
    }

    const Hash32 miner_hash = miner_transaction_hash(frozen);
    const Hash32 expected = block_hash(frozen);
    ByteVector address(connection.peer.bytes.begin(),
                       connection.peer.bytes.begin() +
                           static_cast<std::ptrdiff_t>(connection.peer.size));
    task.share_id = share_id;
    task.job_id = job->database_id;
    task.connection_id = job->connection_database_id;
    task.duplicate_source_id = job->duplicate_source_id;
    task.height = job->height;
    task.peer = connection.peer;
    task.frozen_block = frozen.blob;
    task.candidate_key = key;
    task.miner_tx_hash = miner_hash;
    task.expected_block_id = expected;
    task.max_attempts = static_cast<std::uint32_t>(config_.daemon.submit_attempts);
    task.created_unix_us = unix_time_us();

    CandidateJournalResult result = database_.journal_candidate({
            .candidate_key = key,
            .first_share_id = share_id,
            .job_id = job->database_id,
            .connection_id = job->connection_database_id,
            .height = job->height,
            .peer_family = connection.peer.family,
            .peer_address = address,
            .frozen_block_blob = frozen.blob,
            .miner_tx_hash = miner_hash,
            .expected_block_id = expected,
            .max_attempts = static_cast<std::uint32_t>(config_.daemon.submit_attempts),
            .created_unix_us = task.created_unix_us,
        });
    if (!result.inserted) acquired = false;
    logger_.log(
        result.inserted ? logging::Severity::info : logging::Severity::debug,
        result.inserted ? "candidate.journaled" : "candidate.journal_reused",
        {{logging::PublicStringKey::candidate_key, hex_encode(key)},
         {logging::PublicStringKey::state,
          candidate_state_name(result.state)},
         {logging::PublicStringKey::mode,
          claimed_path ? "claimed" : "computed"}},
        {{logging::IntegerKey::candidate_id, log_id(result.candidate_id)},
         {logging::IntegerKey::share_id, log_id(share_id)},
         {logging::IntegerKey::job_id, log_id(job->database_id)},
         {logging::IntegerKey::height, job->height}});
    if (result.inserted) {
        task.candidate_id = result.candidate_id;
        try {
            task.retain_duplicate_height(
                *this, task.duplicate_source_id, task.height);
            task.not_before = std::chrono::steady_clock::now();
            if (!enqueue_candidate(std::move(task))) {
                throw DatabaseError(
                    "candidate queue allocation failed after durable journal commit");
            }
        }
        catch (...) {
            // The immutable row is committed and therefore recoverable.  Never
            // keep mining if its sole in-process owner could not be handed to
            // the worker queue.
            mark_database_unavailable();
            throw;
        }
    }
    if (admission_acquired != nullptr) *admission_acquired = acquired;
    return result;
}

ShareResponse Runtime::process_share(const StratumSubmission &submission) {
    const std::int64_t received = submission.received_unix_us > 0
                                      ? submission.received_unix_us
                                      : unix_time_us();
    const auto lease = std::static_pointer_cast<JobLease>(
        submission.job_lease);
    const std::shared_ptr<JobContext> job = lease ? lease->job : nullptr;
    const std::int64_t connection_id = lease
        ? lease->connection_database_id : 0;
    const std::optional<std::int64_t> worker_id = lease
        ? lease->worker_database_id : std::nullopt;
    if (connection_id <= 0) return {ShareDisposition::unknown_job, "connection_missing"};

    std::optional<std::string> request_type;
    std::optional<std::string> request_text;
    if (std::holds_alternative<std::int64_t>(submission.request_id)) {
        request_type = "integer";
        request_text = std::to_string(std::get<std::int64_t>(submission.request_id));
    }
    else {
        request_type = "string";
        request_text = std::get<std::string>(submission.request_id);
    }
    if (!job) {
        try {
            const auto share_id = database_.insert_share({
                .connection_id = connection_id,
                .worker_id = worker_id,
                .job_id = std::nullopt,
                .request_sequence = submission.request_sequence,
                .miner_request_id_type = request_type,
                .miner_request_id_text = request_text,
                .received_unix_us = received,
                .nonce = submission.nonce,
                .assigned_difficulty_dec = std::nullopt,
                .network_difficulty_dec = std::nullopt,
                .height_is_older = false,
                .claimed_candidate = false,
                .candidate_admission = "not_candidate",
                .status = "received",
                .provenance = "pending",
            });
            (void)database_.finalize_share(share_id, {
                .status = "unknown_job", .provenance = "pending",
                .completed_unix_us = unix_time_us(),
                .actual_difficulty_dec = std::nullopt,
                .error_code = "unknown_job", .error_message = "Unknown job",
                .verifier_ticket_dec = std::nullopt,
                .verifier_seed_id_dec = std::nullopt,
                .verifier_queue_ns = std::nullopt,
                .verifier_hash_ns = std::nullopt,
                .verifier_total_ns = std::nullopt});
        }
        catch (...) { mark_database_unavailable(); }
        return {ShareDisposition::unknown_job, "unknown_job"};
    }

    const bool height_is_older_at_admission = job->height != 0U &&
        submission.latest_queued_height() > job->height;
    const bool claimed_share_target = meets_share_target(
        submission.claimed_hash, job->target64);
    const bool claimed_network_target = meets_network_target(
        submission.claimed_hash, job->network_difficulty);

    const auto share_id = database_.insert_share({
        .connection_id = connection_id,
        .worker_id = worker_id,
        .job_id = job->database_id,
        .request_sequence = submission.request_sequence,
        .miner_request_id_type = request_type,
        .miner_request_id_text = request_text,
        .received_unix_us = received,
        .nonce = submission.nonce,
        .assigned_difficulty_dec = std::to_string(job->assigned_difficulty),
        .network_difficulty_dec = job->network_difficulty,
        .height_is_older = height_is_older_at_admission,
        .claimed_candidate = claimed_network_target,
        .candidate_admission = "not_candidate",
        .status = "received",
        .provenance = verifier_ ? "pending" : "claimed",
    });
    auto finalize = [&](std::string status, std::string provenance,
                        ShareDisposition disposition, std::string code,
                        std::optional<std::string> actual = std::nullopt,
                        const verifier::Completion *completion = nullptr) {
        ShareFinalization value;
        value.status = std::move(status);
        value.provenance = std::move(provenance);
        value.completed_unix_us = unix_time_us();
        value.actual_difficulty_dec = std::move(actual);
        value.error_code = code;
        value.error_message = share_disposition_message(disposition);
        if (completion != nullptr) {
            value.verifier_ticket_dec = std::to_string(completion->ticket);
            value.verifier_seed_id_dec = std::to_string(completion->seed_id);
            value.verifier_queue_ns = completion->queue_ns;
            value.verifier_hash_ns = completion->hash_ns;
            value.verifier_total_ns = completion->total_ns;
        }
        (void)database_.finalize_share(share_id, value);
        return ShareResponse{disposition, std::move(code)};
    };
    const auto final_height_is_older = [&] {
        const bool final_value = job->height != 0U &&
            submission.latest_queued_height() > job->height;
        if (final_value != height_is_older_at_admission) {
            database_.set_share_height_is_older(share_id, final_value);
        }
        return final_value;
    };

    ParsedBlock frozen;
    Bytes hashing_blob;
    try {
        frozen = insert_nonce(job->block, submission.nonce);
        hashing_blob = block_hashing_blob(frozen);
    }
    catch (...) {
        return finalize("invalid_result", verifier_ ? "pending" : "claimed",
                        ShareDisposition::invalid_result,
                        "job_reconstruction_failed");
    }
    database_.insert_share_hash(share_id, "claimed", submission.claimed_hash,
                                claimed_share_target, claimed_network_target);

    DuplicateToken claimed_token;
    const DuplicateKey claimed_key = make_duplicate_key(job->entropy,
                                                         submission.claimed_hash);
    const DuplicateReserveResult claimed_local = duplicates_.reserve(
        claimed_key, job->duplicate_source_id, job->height,
        claimed_token);
    const bool claimed_capacity =
        claimed_local == DuplicateReserveResult::capacity;
    bool claimed_duplicate = claimed_local == DuplicateReserveResult::duplicate;
    bool claimed_durable = false;
    if (!claimed_capacity && !claimed_duplicate) {
        const auto durable = database_.reserve_duplicate(
            claimed_key, job->height, share_id, DuplicateRole::claimed, received,
            static_cast<std::int64_t>(claimed_token.generation));
        claimed_durable = durable.reserved;
        if (!durable.reserved) {
            claimed_duplicate = true;
            (void)duplicates_.release(claimed_token);
            claimed_token = {};
        }
    }

    CandidateJournalResult candidate;
    auto release_claimed_if_safe = [&] {
        // This is the provisional replay reservation, not the durable
        // candidate identity. Infrastructure/admission failure must permit a
        // genuine retry even after the immutable candidate was journaled.
        const auto generation = claimed_token.generation;
        if (claimed_token) (void)duplicates_.release(claimed_token);
        if (claimed_durable) {
            (void)database_.retire_duplicate(
                claimed_key, static_cast<std::int64_t>(generation),
                unix_time_us());
        }
        claimed_token = {};
        claimed_durable = false;
    };

    const Hash32 frozen_key = make_candidate_key(frozen.blob);
    if (claimed_network_target) {
        candidate = journal_candidate(share_id, job, submission.connection,
                                      frozen, true, false);
        if (candidate.candidate_id == 0 && !verifier_) {
            release_claimed_if_safe();
            return finalize("server_busy", "claimed", ShareDisposition::server_busy,
                            "candidate_rate_limited",
                            std::to_string(actual_difficulty(submission.claimed_hash)));
        }
    }
    const bool frozen_candidate_duplicate =
        claimed_network_target && candidate.candidate_id != 0 &&
        !candidate.inserted;

    if (!verifier_) {
        const auto actual = std::to_string(actual_difficulty(submission.claimed_hash));
        if (claimed_duplicate || frozen_candidate_duplicate) {
            return finalize("duplicate", "claimed", ShareDisposition::duplicate,
                            "duplicate", actual);
        }
        if (claimed_capacity) {
            return finalize("server_busy", "claimed",
                            ShareDisposition::server_busy,
                            "duplicate_registry_full", actual);
        }
        if (!claimed_share_target) {
            return finalize("low_difficulty", "claimed",
                            ShareDisposition::low_difficulty,
                            "low_difficulty", actual);
        }
        if (final_height_is_older()) {
            return finalize("stale", "claimed", ShareDisposition::stale,
                            "stale", actual);
        }
        const auto accepted = database_.accept_share({
            .share_id = share_id,
            .completed_unix_us = unix_time_us(),
            .assigned_difficulty_dec = std::to_string(job->assigned_difficulty),
            .source = HashrateSource::claimed,
            .actual_difficulty_dec = actual,
            .verifier_ticket_dec = std::nullopt,
            .verifier_seed_id_dec = std::nullopt,
            .verifier_queue_ns = std::nullopt,
            .verifier_hash_ns = std::nullopt,
            .verifier_total_ns = std::nullopt,
        });
        if (!accepted.accepted) {
            return {ShareDisposition::server_busy, "share_already_final"};
        }
        return {ShareDisposition::accepted, "accepted"};
    }

    // A claimed candidate must be durably journaled before duplicate-cache
    // backpressure is reported. In verified mode it must also reach MSPV so a
    // deferred or admitted claim can be rescued or produce candidate evidence.
    if (!detail::continue_verification_after_claimed_capacity(
            claimed_capacity, claimed_network_target)) {
        return finalize("server_busy", "pending", ShareDisposition::server_busy,
                        "duplicate_registry_full");
    }

    {
        std::lock_guard lock(state_mutex_);
        auto &pending = pending_verifications_[submission.connection.public_id];
        if (pending >= config_.stratum.max_pending_verifications_per_connection) {
            release_claimed_if_safe();
            return finalize("server_busy", "pending", ShareDisposition::server_busy,
                            "verification_connection_limit");
        }
        ++pending;
    }
    auto decrement_pending = [&] {
        std::lock_guard lock(state_mutex_);
        auto found = pending_verifications_.find(submission.connection.public_id);
        if (found != pending_verifications_.end() && found->second != 0U) --found->second;
    };

    bool waiter_registered = false;
    try {
        waiter_registered = verifier_mailbox_.register_waiter(
            static_cast<std::uint64_t>(share_id));
    }
    catch (...) {
        decrement_pending();
        release_claimed_if_safe();
        return finalize("server_busy", "pending", ShareDisposition::server_busy,
                        "verifier_completion_allocation_failed");
    }
    if (!waiter_registered) {
        decrement_pending();
        release_claimed_if_safe();
        return finalize("server_busy", "pending", ShareDisposition::server_busy,
                        "verifier_completion_capacity");
    }
    verifier::SubmitResult submitted;
    try {
        submitted = verifier_->submit_verify(
            job->seed_id, hashing_blob, submission.claimed_hash,
            static_cast<std::uint64_t>(share_id));
    }
    catch (...) {
        verifier_mailbox_.cancel(static_cast<std::uint64_t>(share_id));
        decrement_pending();
        release_claimed_if_safe();
        return finalize("verifier_failed", "pending",
                        ShareDisposition::verifier_failed,
                        "verifier_submit_exception");
    }
    if (submitted.status != MSPV_OK) {
        verifier_mailbox_.cancel(static_cast<std::uint64_t>(share_id));
        decrement_pending();
        release_claimed_if_safe();
        return finalize(submitted.status == MSPV_QUEUE_FULL ? "server_busy" : "verifier_failed",
                        "pending",
                        submitted.status == MSPV_QUEUE_FULL
                            ? ShareDisposition::server_busy
                            : ShareDisposition::verifier_failed,
                        submitted.status == MSPV_QUEUE_FULL
                            ? "verifier_queue_full" : "verifier_submit_failed");
    }
    std::optional<verifier::Completion> completion;
    try {
        database_.mark_share_verifying(share_id, std::to_string(submitted.ticket),
                                       std::to_string(job->seed_id));
        completion = wait_verification(static_cast<std::uint64_t>(share_id));
    }
    catch (...) {
        verifier_mailbox_.cancel(static_cast<std::uint64_t>(share_id));
        decrement_pending();
        throw;
    }
    decrement_pending();
    if (!completion.has_value() || completion->result != MSPV_RESULT_OK ||
        completion->error != MSPV_OK ||
        completion->correlation != verifier::Correlation::matched ||
        completion->ticket != submitted.ticket || completion->seed_id != job->seed_id) {
        release_claimed_if_safe();
        return finalize("verifier_failed", "pending",
                        ShareDisposition::verifier_failed,
                        "verifier_completion_failed");
    }

    const Hash32 computed_hash = completion->hash;
    const bool match = constant_time_equal(computed_hash,
                                           submission.claimed_hash);
    const bool computed_share_target = meets_share_target(computed_hash, job->target64);
    const bool computed_network_target = meets_network_target(
        computed_hash, job->network_difficulty);
    database_.insert_share_hash(share_id, "computed", computed_hash,
                                computed_share_target, computed_network_target);

    if (match && claimed_durable) {
        /* Upgrade the durable replay identity now that native proof agrees. */
        (void)database_.reserve_duplicate(
            claimed_key, job->height, share_id, DuplicateRole::computed,
            unix_time_us(), static_cast<std::int64_t>(claimed_token.generation));
    }

    bool computed_duplicate = claimed_duplicate && match;
    bool computed_duplicate_capacity = false;
    if (!match) {
        DuplicateToken computed_token;
        const DuplicateKey computed_key = make_duplicate_key(job->entropy, computed_hash);
        const auto local = duplicates_.reserve(
            computed_key, job->duplicate_source_id, job->height,
            computed_token);
        if (local == DuplicateReserveResult::capacity) {
            computed_duplicate_capacity = true;
        }
        else {
            computed_duplicate = local == DuplicateReserveResult::duplicate;
        }
        if (!computed_duplicate_capacity && !computed_duplicate) {
            const auto durable = database_.reserve_duplicate(
                computed_key, job->height, share_id, DuplicateRole::computed,
                unix_time_us(), static_cast<std::int64_t>(computed_token.generation));
            if (!durable.reserved) {
                computed_duplicate = true;
                (void)duplicates_.release(computed_token);
            }
        }
    }

    const detail::VerifiedPostHashPlan post_hash_plan =
        detail::verified_post_hash_plan(
            computed_network_target, computed_duplicate,
            claimed_capacity, computed_duplicate_capacity);
    if (post_hash_plan.journal_computed_candidate && candidate.candidate_id == 0) {
        candidate = journal_candidate(share_id, job, submission.connection,
                                      frozen, false, true);
    }
    if (claimed_network_target && !computed_network_target) {
        const auto verdict = database_.record_candidate_verdict({
            .share_id = share_id,
            .kind = CandidateVerdictKind::false_candidate,
            .candidate_key = frozen_key,
            .candidate_id = candidate.candidate_id == 0
                                ? std::nullopt
                                : std::optional<std::int64_t>(candidate.candidate_id),
            .created_unix_us = unix_time_us(),
        });
        if (verdict.disposition == CandidateVerdictDisposition::actionable) {
            defense_.record(submission.connection.peer,
                            AbuseKind::verified_false_candidate);
        }
    }
    if (!match && (claimed_network_target || computed_network_target)) {
        const auto verdict = database_.record_candidate_verdict({
            .share_id = share_id,
            .kind = CandidateVerdictKind::candidate_mismatch,
            .candidate_key = frozen_key,
            .candidate_id = candidate.candidate_id == 0
                                ? std::nullopt
                                : std::optional<std::int64_t>(candidate.candidate_id),
            .created_unix_us = unix_time_us(),
        });
        if (verdict.disposition == CandidateVerdictDisposition::actionable) {
            defense_.record(submission.connection.peer, AbuseKind::candidate_mismatch);
        }
    }
    else if (!match) {
        // A completed non-candidate mismatch is definitive miner evidence.
        // Candidate mismatches above remain deferred until daemon authority
        // resolves the immutable candidate.
        defense_.record(submission.connection.peer,
                        AbuseKind::candidate_mismatch);
    }

    const std::string actual = std::to_string(actual_difficulty(computed_hash));
    if (post_hash_plan.duplicate_terminal == detail::DuplicateTerminal::duplicate) {
        return finalize("duplicate", "verified", ShareDisposition::duplicate,
                        "duplicate", actual, &*completion);
    }
    if (post_hash_plan.duplicate_terminal == detail::DuplicateTerminal::capacity) {
        return finalize("server_busy", "verified", ShareDisposition::server_busy,
                        "duplicate_registry_full", actual, &*completion);
    }
    if (!match) {
        return finalize("invalid_result", "verified",
                        ShareDisposition::invalid_result, "hash_mismatch",
                        actual, &*completion);
    }
    if (!computed_share_target) {
        return finalize("low_difficulty", "verified",
                        ShareDisposition::low_difficulty, "low_difficulty",
                        actual, &*completion);
    }
    if (final_height_is_older()) {
        return finalize("stale", "verified", ShareDisposition::stale,
                        "stale", actual, &*completion);
    }
    const auto accepted = database_.accept_share({
        .share_id = share_id,
        .completed_unix_us = unix_time_us(),
        .assigned_difficulty_dec = std::to_string(job->assigned_difficulty),
        .source = HashrateSource::verified,
        .actual_difficulty_dec = actual,
        .verifier_ticket_dec = std::to_string(completion->ticket),
        .verifier_seed_id_dec = std::to_string(completion->seed_id),
        .verifier_queue_ns = completion->queue_ns,
        .verifier_hash_ns = completion->hash_ns,
        .verifier_total_ns = completion->total_ns,
    });
    if (!accepted.accepted) return {ShareDisposition::server_busy, "share_already_final"};
    return {ShareDisposition::accepted, "accepted"};
}

bool Runtime::enqueue_candidate(CandidateTask task) noexcept {
    try {
        {
            std::lock_guard lock(candidate_mutex_);
            candidate_queue_.push_back(std::move(task));
        }
        candidate_condition_.notify_one();
        return true;
    }
    catch (...) {
        // CandidateTask owns all process-local accounting references.  If the
        // bounded handoff itself cannot allocate, its destructor releases those
        // references and the durable row remains eligible for startup recovery.
        mark_database_unavailable();
        return false;
    }
}

std::optional<verifier::Completion> Runtime::wait_verification(
    std::uint64_t share_id) {
    return verifier_mailbox_.wait(share_id);
}

void Runtime::verifier_loop(std::stop_token token) noexcept {
    bool fatal = false;
    try {
        while (!token.stop_requested() && verifier_) {
            (void)verifier_->consume_wakeup();
            auto drained = verifier_->drain_completions(256U);
            if (!drained.completions.empty()) {
                bool correlation_failure = false;
                for (auto &completion : drained.completions) {
                    if (completion.correlation !=
                        verifier::Correlation::matched) {
                        correlation_failure = true;
                        break;
                    }
                    (void)verifier_mailbox_.publish(std::move(completion));
                }
                if (correlation_failure) {
                    fatal = true;
                    verifier_operational_.store(false,
                                                std::memory_order_release);
                    verifier_mailbox_.close();
                    ready_.store(false, std::memory_order_release);
                    break;
                }
            }
            if (drained.terminal_status == MSPV_CLOSED) {
                if (running() && !token.stop_requested()) fatal = true;
                break;
            }
            if (drained.terminal_status != MSPV_OK &&
                drained.terminal_status != MSPV_TIMEOUT) {
                fatal = true;
                ready_.store(false, std::memory_order_release);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    catch (...) {
        fatal = true;
        ready_.store(false, std::memory_order_release);
    }
    if (fatal && !token.stop_requested()) {
        internal_failure_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }
    verifier_operational_.store(false, std::memory_order_release);
    verifier_mailbox_.close();
    update_readiness();
}

bool Runtime::reconcile_candidate(const CandidateTask &task) {
    const std::uint32_t cycle = task.reconciliation_cycle + 1U;
    const auto lookup = [&](ReconciliationLookupKind kind,
                            std::optional<Hash32> requested) -> bool {
        std::int64_t reconciliation_id = 0;
        const std::int64_t started = unix_time_us();
        auto before = [&](std::int64_t rpc_id) {
            const auto row = database_.start_candidate_reconciliation({
                .candidate_id = task.candidate_id,
                .cycle_number = cycle,
                .lookup_kind = kind,
                .rpc_request_id = rpc_id,
                .requested_block_id = requested,
                .started_unix_us = started,
            });
            reconciliation_id = row.reconciliation_id;
            logger_.log(
                logging::Severity::debug, "candidate.reconcile_started",
                {{logging::PublicStringKey::state, "reconciling"},
                 {logging::PublicStringKey::mode,
                  kind == ReconciliationLookupKind::expected_hash
                      ? "expected_hash"
                      : "height"}},
                {{logging::IntegerKey::candidate_id,
                  log_id(task.candidate_id)},
                 {logging::IntegerKey::reconciliation_id,
                  log_id(reconciliation_id)},
                 {logging::IntegerKey::cycle, cycle},
                 {logging::IntegerKey::height, task.height}});
        };
        RpcObservation observation;
        if (kind == ReconciliationLookupKind::expected_hash) {
            observation = daemon_.get_block_by_hash(hex_encode(*requested), before);
        }
        else {
            observation = daemon_.get_block_by_height(task.height, before);
        }
        if (!task.expected_block_id.has_value()) {
            throw ValidationError("candidate has no expected block ID");
        }
        const std::string expected_block_id =
            hex_encode(*task.expected_block_id);
        const std::optional<std::string> requested_block_id =
            requested.has_value()
                ? std::optional<std::string>(hex_encode(*requested))
                : std::nullopt;
        const auto evidence = DaemonRpcClient::classify_reconciliation(
            observation, task.height, hex_encode(task.miner_tx_hash),
            expected_block_id,
            requested_block_id.has_value()
                ? std::optional<std::string_view>(*requested_block_id)
                : std::nullopt);
        bool positive = evidence.positive;
        if (positive) {
            try {
                const ParsedBlock observed = parse_block(hex_decode(*evidence.blob_hex));
                const Hash32 observed_block_id = block_hash(observed);
                const Hash32 header_block_id =
                    hex_decode_array<32>(*evidence.block_id);
                positive = constant_time_equal(miner_transaction_hash(observed),
                                               task.miner_tx_hash) &&
                           constant_time_equal(observed_block_id,
                                               header_block_id) &&
                           constant_time_equal(observed_block_id,
                                               *task.expected_block_id);
            }
            catch (...) { positive = false; }
        }
        CandidateReconciliationCompletion completion;
        completion.classification = positive
            ? ReconciliationClassification::positive
            : evidence.indeterminate
                  ? ReconciliationClassification::indeterminate
                  : ReconciliationClassification::inconclusive;
        completion.completed_unix_us = unix_time_us();
        if (evidence.block_id.has_value()) {
            try { completion.observed_block_id = hex_decode_array<32>(*evidence.block_id); }
            catch (...) {}
        }
        completion.observed_height = evidence.height;
        if (evidence.miner_tx_hash.has_value()) {
            try {
                completion.observed_miner_tx_hash =
                    hex_decode_array<32>(*evidence.miner_tx_hash);
            }
            catch (...) {}
        }
        completion.observed_orphan = evidence.orphan;
        completion.response_excerpt = evidence.response_excerpt;
        const auto finished = database_.finish_candidate_reconciliation(
            reconciliation_id, completion);
        logger_.log(
            finished.candidate_accepted ? logging::Severity::info
                                        : logging::Severity::debug,
            "candidate.reconcile_completed",
            {{logging::PublicStringKey::state,
              candidate_state_name(finished.candidate_state)},
             {logging::PublicStringKey::status,
              reconciliation_classification_name(completion.classification)}},
            {{logging::IntegerKey::candidate_id,
              log_id(task.candidate_id)},
             {logging::IntegerKey::reconciliation_id,
              log_id(reconciliation_id)},
             {logging::IntegerKey::cycle, cycle},
             {logging::IntegerKey::height, task.height}});
        if (finished.candidate_accepted) {
            logger_.log(
                logging::Severity::info, "candidate.terminal",
                {{logging::PublicStringKey::state,
                  candidate_state_name(finished.candidate_state)}},
                {{logging::IntegerKey::candidate_id,
                  log_id(task.candidate_id)},
                 {logging::IntegerKey::height, task.height}});
            if (blocknotify_) blocknotify_->wake();
            return true;
        }
        return false;
    };

    try {
        if (task.expected_block_id.has_value() &&
            lookup(ReconciliationLookupKind::expected_hash,
                   task.expected_block_id)) {
            return true;
        }
        if (lookup(ReconciliationLookupKind::height, std::nullopt)) return true;
    }
    catch (const std::exception &error) {
        if (dynamic_cast<const DatabaseError *>(&error) != nullptr) {
            mark_database_unavailable();
        }
        logger_.log(
            logging::Severity::warning, "candidate.reconcile_failed",
            {{logging::PublicStringKey::reason_code, "lookup_failed"}},
            {{logging::IntegerKey::candidate_id,
              log_id(task.candidate_id)},
             {logging::IntegerKey::cycle, cycle},
             {logging::IntegerKey::height, task.height}});
        std::lock_guard lock(state_mutex_);
        last_error_ = error.what();
    }
    return false;
}

void Runtime::candidate_loop(std::stop_token token) noexcept {
    static_assert(!std::is_copy_constructible_v<CandidateTask>);
    static_assert(!std::is_copy_assignable_v<CandidateTask>);
    static_assert(std::is_nothrow_move_constructible_v<CandidateTask>);
    static_assert(std::is_nothrow_move_assignable_v<CandidateTask>);

    while (!token.stop_requested()) {
        CandidateTask task;
        {
            std::unique_lock lock(candidate_mutex_);
            candidate_condition_.wait(lock, [this, &token] {
                return token.stop_requested() || !candidate_queue_.empty();
            });
            if (token.stop_requested()) break;
            const auto selected = std::min_element(
                candidate_queue_.begin(), candidate_queue_.end(),
                [](const CandidateTask &left, const CandidateTask &right) {
                    return left.not_before < right.not_before;
                });
            if (selected->not_before > std::chrono::steady_clock::now()) {
                candidate_condition_.wait_until(lock, selected->not_before);
                continue;
            }
            task = std::move(*selected);
            candidate_queue_.erase(selected);
        }
        auto release_duplicate_bucket = [&] {
            task.release_duplicate_height();
        };
        auto release_candidate_inflight = [&] {
            task.release_candidate_inflight();
        };
        if (!database_operational_.load(std::memory_order_acquire)) {
            release_candidate_inflight();
            release_duplicate_bucket();
            break;
        }
        try {
            const auto durable = database_.find_candidate_by_key(task.candidate_key);
            if (!durable.has_value() ||
                durable->state == CandidateState::accepted ||
                durable->state == CandidateState::accepted_by_reconciliation ||
                durable->state == CandidateState::rejected) {
                release_candidate_inflight();
                release_duplicate_bucket();
                continue;
            }
            if (!task.reconciliation_only &&
                durable->state == CandidateState::ambiguous) {
                release_candidate_inflight();
                task.reconciliation_only = true;
                task.resume_after_reconciliation = false;
                task.not_before = std::chrono::steady_clock::now();
                (void)enqueue_candidate(std::move(task));
                continue;
            }
        }
        catch (const std::exception &error) {
            if (dynamic_cast<const DatabaseError *>(&error) != nullptr) {
                mark_database_unavailable();
                release_candidate_inflight();
                release_duplicate_bucket();
                break;
            }
            std::lock_guard lock(state_mutex_);
            last_error_ = error.what();
            ready_.store(false, std::memory_order_release);
            task.not_before = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
            (void)enqueue_candidate(std::move(task));
            continue;
        }
        std::optional<std::uint32_t> dispatch_intent_attempt;
        try {
            if (task.reconciliation_only) {
                CandidateReconciliationGuard candidate_guard(
                    candidate_mutex_, reconciling_candidates_,
                    task.candidate_id);
                if (!candidate_guard.acquired()) {
                    task.not_before = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(10);
                    (void)enqueue_candidate(std::move(task));
                    continue;
                }
                if (!reconciliation_slots_.try_acquire()) {
                    task.not_before = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(10);
                    (void)enqueue_candidate(std::move(task));
                    continue;
                }
                bool reconciled = false;
                try {
                    reconciled = reconcile_candidate(task);
                }
                catch (...) {
                    reconciliation_slots_.release();
                    throw;
                }
                reconciliation_slots_.release();
                if (reconciled) {
                    release_candidate_inflight();
                    release_duplicate_bucket();
                    continue;
                }
                if (task.quick_reconciliation) {
                    continue;
                }
                ++task.reconciliation_cycle;
                if (task.resume_after_reconciliation) {
                    task.reconciliation_only = false;
                    task.resume_after_reconciliation = false;
                    task.not_before = std::chrono::steady_clock::now();
                    (void)enqueue_candidate(std::move(task));
                    continue;
                }
                ++task.terminal_reconciliation_cycle;
                const std::int64_t now = unix_time_us();
                const bool old_enough = now - task.created_unix_us >=
                                        24LL * 60LL * 60LL * 1'000'000LL;
                const std::uint64_t height = current_height_.load(std::memory_order_acquire);
                const bool deep_enough = task.height <=
                    std::numeric_limits<std::uint64_t>::max() - 60U &&
                    height >= task.height + 60U;
                if (old_enough && deep_enough) {
                    (void)database_.exhaust_candidate_reconciliation(
                        task.candidate_id, now);
                    logger_.log(
                        logging::Severity::warning,
                        "candidate.reconciliation_exhausted",
                        {{logging::PublicStringKey::state, "ambiguous"}},
                        {{logging::IntegerKey::candidate_id,
                          log_id(task.candidate_id)},
                         {logging::IntegerKey::cycle,
                          task.reconciliation_cycle},
                         {logging::IntegerKey::height, task.height}});
                    release_duplicate_bucket();
                    continue;
                }
                static constexpr std::array<std::uint32_t, 6> delays{
                    0U, 5U, 30U, 120U, 600U, 3600U};
                const std::size_t index = std::min<std::size_t>(
                    task.terminal_reconciliation_cycle,
                    delays.size() - 1U);
                const auto delay = std::chrono::seconds(delays[index]);
                database_.schedule_candidate_reconciliation(
                    task.candidate_id,
                    now + static_cast<std::int64_t>(delays[index]) * 1'000'000LL);
                task.not_before = std::chrono::steady_clock::now() + delay;
                (void)enqueue_candidate(std::move(task));
                continue;
            }

            const std::uint32_t attempt = task.attempt_count + 1U;
            if (attempt > task.max_attempts) continue;
            const std::int64_t started = unix_time_us();
            const SubmitObservation observation = daemon_.submit_block(
                task.frozen_block, [&](std::int64_t rpc_id) {
                    (void)database_.start_candidate_attempt(
                        task.candidate_id, attempt, rpc_id, started);
                    task.attempt_count = attempt;
                    dispatch_intent_attempt = attempt;
                    logger_.log(
                        logging::Severity::info,
                        "candidate.attempt_started",
                        {{logging::PublicStringKey::state, "dispatching"}},
                        {{logging::IntegerKey::candidate_id,
                          log_id(task.candidate_id)},
                         {logging::IntegerKey::attempt, attempt},
                         {logging::IntegerKey::height, task.height}});
                });
            if (!dispatch_intent_attempt.has_value()) {
                // The bounded daemon scheduler rejected admission before the
                // durable dispatch barrier. No request could be on wire and
                // no candidate attempt is consumed.
                task.not_before = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(10);
                (void)enqueue_candidate(std::move(task));
                continue;
            }
            CandidateAttemptCompletion completion;
            completion.completed_unix_us = unix_time_us();
            completion.trusted_mode = !config_.verifier.enabled;
            if (observation.http_status >= std::numeric_limits<int>::min() &&
                observation.http_status <= std::numeric_limits<int>::max()) {
                completion.http_status = static_cast<int>(observation.http_status);
            }
            if (observation.rpc_error_code.has_value() &&
                *observation.rpc_error_code >= std::numeric_limits<int>::min() &&
                *observation.rpc_error_code <= std::numeric_limits<int>::max()) {
                completion.rpc_error_code = static_cast<int>(*observation.rpc_error_code);
            }
            completion.daemon_status = observation.daemon_status;
            if (observation.daemon_block_id.has_value()) {
                try {
                    completion.daemon_block_id =
                        hex_decode_array<32>(*observation.daemon_block_id);
                }
                catch (...) {}
            }
            completion.response_excerpt = observation.response_excerpt;
            if (observation.classification == SubmitClassification::accepted) {
                completion.classification = CandidateAttemptClassification::accepted;
            }
            else if (observation.classification == SubmitClassification::explicit_rejection) {
                completion.classification = CandidateAttemptClassification::explicit_rejection;
            }
            else {
                completion.classification = CandidateAttemptClassification::indeterminate;
            }
            const auto result = database_.finish_candidate_attempt(
                task.candidate_id, attempt, completion);
            task.attempt_count = result.attempt_count;
            logger_.log(
                result.terminal ? logging::Severity::info
                                : logging::Severity::debug,
                "candidate.attempt_completed",
                {{logging::PublicStringKey::state,
                  candidate_state_name(result.state)},
                 {logging::PublicStringKey::status,
                  attempt_classification_name(completion.classification)}},
                {{logging::IntegerKey::candidate_id,
                  log_id(task.candidate_id)},
                 {logging::IntegerKey::attempt, attempt},
                 {logging::IntegerKey::height, task.height}});
            if (result.terminal) {
                logger_.log(
                    logging::Severity::info, "candidate.terminal",
                    {{logging::PublicStringKey::state,
                      candidate_state_name(result.state)}},
                    {{logging::IntegerKey::candidate_id,
                      log_id(task.candidate_id)},
                     {logging::IntegerKey::attempt, attempt},
                     {logging::IntegerKey::height, task.height}});
            }
            if (result.state == CandidateState::accepted ||
                result.state == CandidateState::accepted_by_reconciliation) {
                release_candidate_inflight();
                if (blocknotify_) blocknotify_->wake();
                release_duplicate_bucket();
                continue;
            }
            if (completion.classification == CandidateAttemptClassification::indeterminate &&
                !result.terminal) {
                // Reconciliation is independent of the already scheduled
                // identical-block retry. Build the metadata-only lookup before
                // moving the original (and sole owning) retry task. Queueing the
                // lookup first is safe: a retry-handoff allocation failure stops
                // mining and leaves the committed row for startup recovery.
                CandidateTask lookup = task.make_reference_free_lookup();
                lookup.not_before = std::chrono::steady_clock::now();
                if (!enqueue_candidate(std::move(lookup))) continue;

                ++task.reconciliation_cycle;
                task.not_before = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(config_.daemon.submit_retry_ms);
                (void)enqueue_candidate(std::move(task));
                continue;
            }
            if (result.terminal) {
                release_candidate_inflight();
                for (std::uint32_t index = 0;
                     index < result.newly_actionable_false_candidates; ++index) {
                    defense_.record(task.peer, AbuseKind::verified_false_candidate);
                }
                for (std::uint32_t index = 0;
                     index < result.newly_actionable_candidate_mismatches; ++index) {
                    defense_.record(task.peer, AbuseKind::candidate_mismatch);
                }
                if (result.trusted_candidate_rejection_recorded) {
                    defense_.record(task.peer, AbuseKind::trusted_candidate_rejection);
                }
                if (result.state == CandidateState::ambiguous) {
                    task.reconciliation_only = true;
                    task.not_before = std::chrono::steady_clock::now();
                    (void)enqueue_candidate(std::move(task));
                }
                else {
                    release_duplicate_bucket();
                }
                continue;
            }
            task.not_before = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(config_.daemon.submit_retry_ms);
            (void)enqueue_candidate(std::move(task));
        }
        catch (const std::exception &error) {
            logger_.log(
                logging::Severity::error, "candidate.worker_failed",
                {{logging::PublicStringKey::reason_code,
                  dispatch_intent_attempt.has_value()
                      ? "post_dispatch_failure"
                      : "pre_dispatch_failure"}},
                {{logging::IntegerKey::candidate_id,
                  log_id(task.candidate_id)},
                 {logging::IntegerKey::attempt,
                  dispatch_intent_attempt.has_value()
                      ? static_cast<std::uint64_t>(*dispatch_intent_attempt)
                      : static_cast<std::uint64_t>(task.attempt_count)},
                 {logging::IntegerKey::height, task.height}});
            {
                std::lock_guard lock(state_mutex_);
                last_error_ = error.what();
            }
            ready_.store(false, std::memory_order_release);

            bool retain_task = true;
            if (dispatch_intent_attempt.has_value()) {
                try {
                    CandidateAttemptCompletion uncertain;
                    uncertain.classification =
                        CandidateAttemptClassification::indeterminate;
                    uncertain.completed_unix_us = unix_time_us();
                    uncertain.trusted_mode = !config_.verifier.enabled;
                    uncertain.response_excerpt =
                        "candidate worker failed after durable dispatch intent";
                    const auto recovered = database_.finish_candidate_attempt(
                        task.candidate_id, *dispatch_intent_attempt, uncertain);
                    task.attempt_count = recovered.attempt_count;

                    if (recovered.state == CandidateState::accepted ||
                        recovered.state == CandidateState::accepted_by_reconciliation) {
                        release_candidate_inflight();
                        if (blocknotify_) blocknotify_->wake();
                        release_duplicate_bucket();
                        retain_task = false;
                    }
                    else if (recovered.state == CandidateState::rejected) {
                        release_candidate_inflight();
                        release_duplicate_bucket();
                        retain_task = false;
                        try {
                            for (std::uint32_t index = 0;
                                 index < recovered.newly_actionable_false_candidates;
                                 ++index) {
                                defense_.record(task.peer,
                                                AbuseKind::verified_false_candidate);
                            }
                            for (std::uint32_t index = 0;
                                 index < recovered.newly_actionable_candidate_mismatches;
                                 ++index) {
                                defense_.record(task.peer,
                                                AbuseKind::candidate_mismatch);
                            }
                            if (recovered.trusted_candidate_rejection_recorded) {
                                defense_.record(task.peer,
                                                AbuseKind::trusted_candidate_rejection);
                            }
                        }
                        catch (...) {
                            ready_.store(false, std::memory_order_release);
                        }
                    }
                    else {
                        task.reconciliation_only = true;
                        task.resume_after_reconciliation =
                            recovered.state != CandidateState::ambiguous &&
                            task.attempt_count < task.max_attempts;
                        if (recovered.state == CandidateState::ambiguous) {
                            release_candidate_inflight();
                        }
                    }
                }
                catch (...) {
                    // The durable attempt is still dispatching and cannot be
                    // safely repaired in-process. Stop mining; startup
                    // recovery owns the transition before any later RPC.
                    mark_database_unavailable();
                    release_candidate_inflight();
                    release_duplicate_bucket();
                    retain_task = false;
                }
            }

            if (retain_task && !token.stop_requested()) {
                task.not_before = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(1);
                (void)enqueue_candidate(std::move(task));
            }
        }
    }
}

bool Runtime::wait_for_zmq_tip_change(const std::stop_token token)
{
    Hash32 cached_parent{};
    {
        std::lock_guard lock(state_mutex_);
        if (!current_template_) return true;
        cached_parent = current_template_->previous_hash;
    }

    // Monero may publish the ZMQ message just before /getheight reflects the
    // new chain tip. Ten 100 ms observations match the historical daemon-
    // mining behavior while remaining stop-aware.
    for (unsigned attempt = 0; attempt < 10U; ++attempt) {
        if (token.stop_requested()) return false;
        const RpcObservation observation = daemon_.get_height();
        if (!observation.valid() || !observation.document.is_object()) {
            throw ValidationError("daemon /getheight observation was invalid");
        }
        const auto status = observation.document.find("status");
        const auto height = observation.document.find("height");
        const auto hash = observation.document.find("hash");
        if (status == observation.document.end() ||
            height == observation.document.end() ||
            hash == observation.document.end()) {
            throw ValidationError("daemon /getheight response was malformed");
        }
        if (!status->is_string() || !height->is_number_unsigned() ||
            !hash->is_string()) {
            throw ValidationError("daemon /getheight response was malformed");
        }
        const std::string &status_text = status->get_ref<const std::string &>();
        const std::string &hash_text = hash->get_ref<const std::string &>();
        if (status_text != "OK" || !is_hex_64(hash_text)) {
            throw ValidationError("daemon /getheight response was malformed");
        }
        const Hash32 observed = hex_decode_array<32>(hash_text);
        if (!constant_time_equal(observed, cached_parent)) return true;
        for (unsigned tick = 0; tick < 10U && !token.stop_requested(); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return false;
}

void Runtime::template_loop(std::stop_token token) noexcept {
    auto next_poll = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(config_.daemon.poll_interval_ms);
    while (!token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        const bool hinted = template_refresh_requested_.exchange(
            false, std::memory_order_acq_rel);
        if (hinted || now >= next_poll) {
            try {
                if (hinted && !wait_for_zmq_tip_change(token)) {
                    // A duplicate/stale hint, or a hint already satisfied by
                    // a concurrent poll, must not spin and suppress polling.
                    // Shorten the ordinary poll deadline in case daemon tip
                    // propagation merely exceeded the bounded ZMQ barrier.
                    next_poll = std::min(
                        next_poll,
                        std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(
                                config_.daemon.refresh_retry_ms));
                }
                else {
                    refresh_template(hinted ? "zmq" : "poll");
                    next_poll = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(
                                    config_.daemon.poll_interval_ms);
                    update_readiness();
                }
            }
            catch (const std::exception &error) {
                logger_.log(
                    logging::Severity::warning, "template.refresh_failed",
                    {{logging::PublicStringKey::reason_code,
                      "template_refresh_failed"},
                     {logging::PublicStringKey::mode,
                      hinted ? "zmq" : "poll"}});
                {
                    std::lock_guard lock(state_mutex_);
                    last_error_ = error.what();
                }
                template_operational_.store(false, std::memory_order_release);
                if (dynamic_cast<const DatabaseError *>(&error) != nullptr) {
                    mark_database_unavailable();
                }
                update_readiness();
                next_poll = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(config_.daemon.refresh_retry_ms);
                emit("template_error", {{"message", error.what()}});
            }
            try {
                defense_.expire();
                (void)database_.expire_bans(unix_time_us(), session_id_);
            }
            catch (...) { mark_database_unavailable(); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Runtime::committed_event_loop(std::stop_token token) noexcept {
    std::int64_t last = event_stream_start_id_;
    for (;;) {
        try {
            const auto rows = database_.load_events_after(last, 256U);
            for (const auto &row : rows) {
                nlohmann::ordered_json frame;
                frame["schema_version"] = 1;
                frame["event_id"] = std::to_string(row.id);
                frame["session_id"] = hex_encode(row.session_public_id);
                frame["time_utc"] = format_rfc3339_utc_us(row.created_unix_us);
                frame["type"] = row.type;
                frame["connection_id"] = row.connection_public_id.has_value()
                    ? nlohmann::ordered_json(hex_encode(*row.connection_public_id))
                    : nlohmann::ordered_json(nullptr);
                frame["worker_id"] = row.worker_id.has_value()
                    ? nlohmann::ordered_json(std::to_string(*row.worker_id))
                    : nlohmann::ordered_json(nullptr);
                frame["template_id"] = row.template_id.has_value()
                    ? nlohmann::ordered_json(std::to_string(*row.template_id))
                    : nlohmann::ordered_json(nullptr);
                frame["job_id"] = row.job_public_id.has_value()
                    ? nlohmann::ordered_json(hex_encode(*row.job_public_id))
                    : nlohmann::ordered_json(nullptr);
                frame["share_id"] = row.share_id.has_value()
                    ? nlohmann::ordered_json(std::to_string(*row.share_id))
                    : nlohmann::ordered_json(nullptr);
                frame["candidate_id"] = row.candidate_id.has_value()
                    ? nlohmann::ordered_json(std::to_string(*row.candidate_id))
                    : nlohmann::ordered_json(nullptr);
                frame["round_id"] = row.round_id.has_value()
                    ? nlohmann::ordered_json(std::to_string(*row.round_id))
                    : nlohmann::ordered_json(nullptr);
                frame["payload"] = nlohmann::ordered_json::parse(row.payload_json);
                if (events_) events_->publish_committed(
                    static_cast<std::uint64_t>(row.id), frame.dump());
                last = row.id;
            }
            if (token.stop_requested() && rows.empty()) break;
            if (rows.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        catch (...) {
            if (token.stop_requested()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Runtime::emit(std::string type, nlohmann::json data,
                   std::optional<std::int64_t> connection_id,
                   std::optional<std::int64_t> job_id,
                   std::optional<std::int64_t> share_id,
                   std::optional<std::int64_t> candidate_id,
                   std::optional<std::int64_t> round_id) noexcept {
    try {
        if (!data.is_object()) data = nlohmann::json::object();
        const nlohmann::json payload{{"payload_schema_version", 1},
                                     {"data", std::move(data)}};
        (void)database_.insert_event({
            .session_id = session_id_,
            .created_unix_us = unix_time_us(),
            .type = std::move(type),
            .connection_id = connection_id,
            .worker_id = std::nullopt,
            .template_id = std::nullopt,
            .job_id = job_id,
            .share_id = share_id,
            .candidate_id = candidate_id,
            .round_id = round_id,
            .payload_json = payload.dump(),
        });
    }
    catch (...) { mark_database_unavailable(); }
}

ApiDataSource Runtime::api_data_source() {
    ApiDataSource live;
    live.readiness = [this] {
        ApiReadinessSnapshot snapshot;
        snapshot.height = current_height_.load(std::memory_order_acquire) == 0
            ? std::nullopt
            : std::optional<std::uint64_t>(current_height_.load(std::memory_order_acquire));
        const bool database_ready =
            database_operational_.load(std::memory_order_acquire);
        snapshot.database = {
            database_ready, !database_ready,
            database_ready
                ? std::nullopt
                : std::optional<std::string>("SQLite durable writer unavailable")};
        snapshot.entropy = {entropy_.issuance_allowed(), entropy_.degraded(),
                            entropy_.issuance_allowed()
                                ? std::nullopt
                                : std::optional<std::string>("entropy reseed unavailable")};
        const bool has_template = [&] {
            std::lock_guard lock(state_mutex_);
            return static_cast<bool>(current_template_);
        }();
        const bool template_ready = has_template &&
            template_operational_.load(std::memory_order_acquire);
        snapshot.daemon_rpc = {
            template_ready, !template_ready,
            template_ready
                ? std::nullopt
                : std::optional<std::string>("no recent valid daemon template")};
        snapshot.template_state = snapshot.daemon_rpc;
        const bool verifier_ready = !config_.verifier.enabled ||
            verifier_operational_.load(std::memory_order_acquire);
        snapshot.verifier = {
            verifier_ready, config_.verifier.enabled && !verifier_ready,
            verifier_ready
                ? std::nullopt
                : std::optional<std::string>(
                      "verifier completion loop unavailable")};
        const bool stratum_ready =
            stratum_operational_.load(std::memory_order_acquire);
        snapshot.stratum = {stratum_ready, false, std::nullopt};
        return snapshot;
    };
    live.singleton = [this](ApiSingleton resource) -> std::optional<nlohmann::json> {
        if (resource == ApiSingleton::daemon) {
            std::shared_ptr<TemplateContext> current;
            std::optional<std::string> error;
            const DaemonRequestCounts request_counts = daemon_.request_counts();
            {
                std::lock_guard lock(state_mutex_);
                current = current_template_;
                error = last_error_;
            }
            const bool healthy = static_cast<bool>(current) &&
                template_operational_.load(std::memory_order_acquire);
            return nlohmann::json{
                {"ready", healthy},
                {"rpc_url", daemon_.redacted_url()},
                {"rpc_state", healthy ? "healthy" : "unavailable"},
                {"zmq_configured", config_.daemon.zmq_address.has_value()},
                {"zmq_address", config_.daemon.zmq_address.has_value()
                    ? nlohmann::json(*config_.daemon.zmq_address) : nlohmann::json(nullptr)},
                {"zmq_state", !config_.daemon.zmq_address.has_value()
                    ? "disabled"
                    : (zmq_operational_.load(std::memory_order_acquire)
                           ? "healthy" : "degraded")},
                {"network", config_.network == Network::regtest
                    ? "fakechain" : network_name(config_.network)},
                {"height", current ? nlohmann::json(current->height) : nlohmann::json(nullptr)},
                {"target_height", nullptr}, {"synchronized", nullptr},
                {"template_id", current ? nlohmann::json(std::to_string(current->database_id))
                                          : nlohmann::json(nullptr)},
                {"template_generation", current
                    ? nlohmann::json(std::to_string(current->generation)) : nlohmann::json(nullptr)},
                {"template_height", current ? nlohmann::json(current->height) : nlohmann::json(nullptr)},
                {"template_fetched_at", nullptr}, {"last_rpc_success_at", nullptr},
                {"last_template_success_at", nullptr}, {"last_error_at", nullptr},
                {"last_error_code", error ? nlohmann::json("runtime_error") : nlohmann::json(nullptr)},
                {"last_error_message", error ? nlohmann::json(*error) : nlohmann::json(nullptr)},
                {"refresh_inflight", false},
                {"http_inflight", request_counts.inflight},
                {"http_pending", request_counts.pending},
            };
        }
        if (resource == ApiSingleton::verifier) {
            nlohmann::json configuration = nullptr;
            nlohmann::json stats = nullptr;
            nlohmann::json seeds = nlohmann::json::array();
            if (verifier_) {
                const auto mapped = verifier::Verifier::map_config(verifier_config(config_));
                configuration = {
                    {"memory_mode", config_.verifier.memory_mode},
                    {"workers", mapped.worker_count},
                    {"seed_init_threads", mapped.seed_init_threads},
                    {"pending_capacity", mapped.pending_capacity},
                    {"max_outstanding", mapped.max_outstanding},
                    {"max_input_size", mapped.max_input_size},
                    {"max_seed_key_size", mapped.max_seed_key_size},
                    {"max_seeds", mapped.max_seeds},
                    {"max_buffered_input_bytes", std::to_string(mapped.max_buffered_input_bytes)},
                    {"large_pages", config_.verifier.large_pages},
                    {"jit", config_.verifier.jit}, {"aes", config_.verifier.aes},
                    {"log_level", config_.verifier.log_level},
                };
                const auto native_stats = verifier_->stats();
                if (native_stats.status == MSPV_OK) {
                    const auto &value = native_stats.stats;
                    stats = {
                        {"workers", value.workers}, {"seeds", value.seeds},
                        {"seeds_preparing", value.seeds_preparing},
                        {"seeds_ready", value.seeds_ready}, {"pending", value.pending},
                        {"running", value.running}, {"completions", value.completions},
                        {"outstanding", value.outstanding},
                        {"buffered_input_bytes", std::to_string(value.buffered_input_bytes)},
                        {"active_seed_id", value.active_seed_id == 0
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(std::to_string(value.active_seed_id))},
                        {"submitted", std::to_string(value.submitted)},
                        {"completed", std::to_string(value.completed)},
                        {"cancelled", std::to_string(value.cancelled)},
                        {"failed", std::to_string(value.failed)},
                    };
                }
                for (const auto &seed : verifier_->seed_snapshots()) {
                    std::string state = "preparing";
                    if (seed.state == MSPV_SEED_READY) state = "ready";
                    else if (seed.state == MSPV_SEED_CURRENT) state = "current";
                    else if (seed.state == MSPV_SEED_RELEASING) state = "releasing";
                    else if (seed.state == MSPV_SEED_FAILED) state = "failed";
                    seeds.push_back({
                        {"seed_id", std::to_string(seed.seed_id)},
                        {"seed_hash", hex_encode(seed.seed_hash)}, {"state", state},
                        {"last_error_code", seed.last_error},
                        {"last_error_message", mspv_status_string(seed.last_error)},
                        {"key_size", seed.key_size}, {"queued_jobs", seed.queued_jobs},
                        {"running_jobs", seed.running_jobs},
                        {"prepare_ns", std::to_string(seed.prepare_ns)},
                        {"memory_uses_large_pages", seed.memory_uses_large_pages},
                        {"all_vms_use_large_pages", seed.all_vms_use_large_pages},
                    });
                }
            }
            return nlohmann::json{
                {"enabled", static_cast<bool>(verifier_)},
                {"provenance", verifier_ ? "verified" : "claimed"},
                {"package_version", verifier_ ? nlohmann::json("0.1.0") : nlohmann::json(nullptr)},
                {"commit", verifier_ ? nlohmann::json(MSS_MSPV_COMMIT) : nlohmann::json(nullptr)},
                {"abi_version", verifier_ ? nlohmann::json(MSPV_ABI_VERSION) : nlohmann::json(nullptr)},
                {"configuration", configuration}, {"stats", stats}, {"seeds", seeds},
                {"last_error_at", nullptr}, {"last_error_code", nullptr},
                {"last_error_message", nullptr},
            };
        }
        if (resource == ApiSingleton::summary) {
            const auto uptime = std::max<std::int64_t>(
                0, (unix_time_us() - started_unix_us_) / 1'000'000);
            return nlohmann::json{{"server", {
                {"version", MSS_VERSION}, {"git_commit", MSS_GIT_COMMIT},
                {"session_id", hex_encode(session_public_id_)},
                {"started_at", format_rfc3339_utc_us(started_unix_us_)},
                {"uptime_seconds", uptime},
                {"network", config_.network == Network::regtest
                    ? "fakechain" : network_name(config_.network)},
                {"verification", verifier_ ? "verified" : "trusted"},
                {"stratum_authentication", config_.stratum.access_password.has_value()
                    && !config_.stratum.access_password->empty() ? "enabled" : "disabled"},
                {"api_authentication", config_.api.access_token.has_value()
                    && !config_.api.access_token->empty() ? "enabled" : "disabled"}}}};
        }
        return std::nullopt;
    };
    return make_sqlite_api_data_source({
        .database = database_.options(),
        .active_hashrate_source = verifier_ ? HashrateSource::verified
                                             : HashrateSource::claimed,
        .live = std::move(live),
        .clock = [] { return unix_time_us(); },
        .writer_stats = [this] { return database_.writer_stats(); },
    });
}

void Runtime::start() {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (stop_started_.load(std::memory_order_acquire)) {
        throw std::logic_error("Runtime instances cannot be restarted after stop");
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (stop_started_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        throw std::logic_error("Runtime instances cannot be restarted after stop");
    }
    logger_.log(
        logging::Severity::info, "runtime.starting",
        {{logging::PublicStringKey::component, "runtime"},
         {logging::PublicStringKey::network, network_name(config_.network)},
         {logging::PublicStringKey::wallet_address, config_.wallet_address},
         {logging::PublicStringKey::daemon_endpoint, daemon_.redacted_url()},
         {logging::PublicStringKey::mode,
          config_.verifier.enabled ? "verified" : "trusted"}},
        {{logging::IntegerKey::listener_count,
          static_cast<std::uint64_t>(config_.stratum.listen.size())}});
    try {
        started_unix_us_ = unix_time_us();
        session_public_id_ = entropy_.generate_id("server-session-public-id/v1");
        const InterruptedRuntimeRecovery interrupted =
            database_.recover_interrupted_runtime(started_unix_us_);
        session_id_ = database_.start_session({
            .public_id = session_public_id_,
            .started_unix_us = started_unix_us_,
            .version = MSS_VERSION,
            .verifier_commit = config_.verifier.enabled
                ? std::optional<std::string>(MSS_MSPV_COMMIT) : std::nullopt,
        });
        apply_actionable_verdicts(interrupted.actionable_verdicts);
        (void)database_.ensure_open_round(started_unix_us_);

        const auto active_bans = database_.load_active_bans(started_unix_us_);
        for (const auto &ban : active_bans) {
            PeerAddress peer;
            peer.family = ban.peer_family;
            peer.size = ban.peer_address.size();
            if (peer.size > peer.bytes.size()) throw DatabaseError("invalid persisted ban address");
            std::copy(ban.peer_address.begin(), ban.peer_address.end(), peer.bytes.begin());
            BanRecord record;
            record.peer = peer;
            record.reason = AbuseKind::malformed;
            record.created = std::chrono::system_clock::time_point(
                std::chrono::microseconds(started_unix_us_));
            record.expires = std::chrono::system_clock::time_point(
                std::chrono::microseconds(ban.expires_unix_us));
            record.evidence_start = record.created;
            record.evidence_end = record.created;
            defense_.restore_ban(record);
        }
        std::set<std::pair<std::uint64_t, std::uint64_t>>
            restored_duplicate_buckets;
        for (const auto &duplicate : database_.load_active_duplicates()) {
            duplicates_.restore(
                duplicate.key, duplicate.source_id, duplicate.height,
                static_cast<std::uint64_t>(duplicate.generation_token));
            restored_duplicate_buckets.emplace(duplicate.source_id,
                                               duplicate.height);
        }

        if (config_.verifier.enabled) {
            verifier_mailbox_.open();
            verifier_ = std::make_unique<verifier::Verifier>(verifier_config(config_));
            verifier_operational_.store(true, std::memory_order_release);
            logger_.log(
                logging::Severity::info, "verifier.enabled",
                {{logging::PublicStringKey::mode,
                  config_.verifier.memory_mode},
                 {logging::PublicStringKey::state, "running"}},
                {{logging::IntegerKey::count, config_.verifier.workers}});
            verifier_thread_ = std::jthread(
                [this](std::stop_token token) { verifier_loop(token); });
        }
        else {
            logger_.log(
                logging::Severity::warning, "verifier.disabled",
                {{logging::PublicStringKey::mode, "trusted"},
                 {logging::PublicStringKey::state, "disabled"}});
        }

        if (config_.api.enabled) {
            api_ = std::make_unique<ApiService>(ApiServiceOptions{
                .api = config_.api,
                .identity = {MSS_VERSION, MSS_GIT_COMMIT,
                             hex_encode(session_public_id_), started_unix_us_},
                .active_hashrate_source = verifier_ ? HashrateSource::verified
                                                     : HashrateSource::claimed,
                .worker_threads = std::min<std::size_t>(4U,
                    static_cast<std::size_t>(config_.api.max_connections)),
            }, &database_, api_data_source());
            api_->start();
        }
        if (config_.events.enabled) {
            unsigned permissions = 0;
            for (const char byte : config_.events.permissions) {
                if (byte < '0' || byte > '7') throw ValidationError("invalid event permissions");
                permissions = permissions * 8U + static_cast<unsigned>(byte - '0');
            }
            event_stream_start_id_ = database_.event_high_water_mark();
            events_ = std::make_unique<EventStream>(EventStreamConfig{
                .unix_socket = config_.events.unix_socket,
                .permissions = permissions,
                .max_clients = static_cast<std::size_t>(config_.events.max_clients),
                .max_pending_bytes_per_client = static_cast<std::size_t>(
                    config_.events.max_pending_bytes_per_client),
            }, hex_encode(session_public_id_));
            events_->start(static_cast<std::uint64_t>(event_stream_start_id_));
            committed_event_thread_ = std::jthread(
                [this](std::stop_token token) { committed_event_loop(token); });
        }

        if (config_.blocknotify.has_value() && !config_.blocknotify->empty()) {
            database_.recover_blocknotify_deliveries();
            BlockNotifyCommand command;
            command.arguments = config_.blocknotify_argv;
            blocknotify_ = std::make_unique<BlockNotifySupervisor>(
                std::move(command),
                [this]() -> std::optional<monero_solo::BlockNotifyDelivery> {
                    std::optional<BlocknotifyDelivery> row;
                    try {
                        row = database_.claim_next_blocknotify(unix_time_us());
                    }
                    catch (...) {
                        mark_database_unavailable();
                        throw;
                    }
                    if (!row.has_value()) return std::nullopt;
                    return monero_solo::BlockNotifyDelivery{
                        .id = row->id, .candidate_id = row->candidate_id,
                        .miner_tx_hash = hex_encode(row->miner_tx_hash),
                        .attempt_count = row->attempt_count,
                    };
                },
                [this](const monero_solo::BlockNotifyDelivery &delivery,
                       const BlockNotifyResult &result, std::chrono::seconds) {
                    try {
                        database_.finish_blocknotify(delivery.id, {
                            .delivered = result.delivered,
                            .completed_unix_us = unix_time_us(),
                            .exit_code = result.exit_code,
                            .term_signal = result.term_signal,
                            .stderr_excerpt = result.stderr_excerpt.empty()
                                ? std::nullopt
                                : std::optional<std::string>(result.stderr_excerpt),
                            .last_error = result.error.empty()
                                ? std::nullopt
                                : std::optional<std::string>(result.error),
                        });
                    }
                    catch (...) {
                        mark_database_unavailable();
                        throw;
                    }
                    const std::string_view status = result.delivered
                        ? "delivered"
                        : result.timed_out ? "timed_out" : "failed";
                    const std::string_view reason = result.delivered
                        ? "success"
                        : result.timed_out
                              ? "timeout"
                              : result.exit_code.has_value()
                                    ? "nonzero_exit"
                                    : result.term_signal.has_value()
                                          ? "terminated_by_signal"
                                          : "execution_failed";
                    logger_.log(
                        result.delivered ? logging::Severity::info
                                         : logging::Severity::warning,
                        "blocknotify.completed",
                        {{logging::PublicStringKey::status, status},
                         {logging::PublicStringKey::reason_code, reason}},
                        {{logging::IntegerKey::delivery_id,
                          log_id(delivery.id)},
                         {logging::IntegerKey::candidate_id,
                          log_id(delivery.candidate_id)},
                         {logging::IntegerKey::attempt,
                          static_cast<std::uint64_t>(delivery.attempt_count)},
                         {logging::IntegerKey::exit_code,
                          result.exit_code.has_value() && *result.exit_code >= 0
                              ? static_cast<std::uint64_t>(*result.exit_code)
                              : 0U},
                         {logging::IntegerKey::signal_number,
                          result.term_signal.has_value() &&
                                  *result.term_signal >= 0
                              ? static_cast<std::uint64_t>(*result.term_signal)
                              : 0U}});
                });
            blocknotify_->start();
        }

        validate_daemon_network();
        logger_.log(
            logging::Severity::info, "daemon.ready",
            {{logging::PublicStringKey::daemon_endpoint,
              daemon_.redacted_url()},
             {logging::PublicStringKey::network,
              network_name(config_.network)},
             {logging::PublicStringKey::state, "ready"}});

        const auto recoverable_candidates = database_.recoverable_candidates();
        std::vector<CandidateTask> recovered_tasks;
        recovered_tasks.reserve(recoverable_candidates.size());
        for (const auto &recovery : recoverable_candidates) {
            CandidateTask task;
            task.candidate_id = recovery.candidate_id;
            task.share_id = recovery.first_share_id;
            task.job_id = recovery.job_id;
            task.connection_id = recovery.connection_id;
            if (recovery.connection_id <= 0) {
                throw DatabaseError(
                    "recovery candidate has an invalid duplicate source");
            }
            task.duplicate_source_id = static_cast<std::uint64_t>(
                recovery.connection_id);
            task.height = recovery.height;
            task.peer.family = recovery.peer_family;
            task.peer.size = recovery.peer_address.size();
            if (task.peer.size > task.peer.bytes.size()) {
                throw DatabaseError("recovery candidate peer address is invalid");
            }
            std::copy(recovery.peer_address.begin(), recovery.peer_address.end(),
                      task.peer.bytes.begin());
            task.frozen_block = recovery.frozen_block_blob;
            task.candidate_key = recovery.candidate_key;
            task.miner_tx_hash = recovery.miner_tx_hash;
            task.expected_block_id = recovery.expected_block_id;
            task.max_attempts = recovery.max_attempts;
            task.attempt_count = recovery.attempt_count;
            task.reconciliation_cycle = recovery.reconciliation_cycle_count;
            task.created_unix_us = recovery.created_unix_us;
            task.reconciliation_only = true;
            // Durable cycle_count also includes speculative/quick lookups.
            // Terminal backoff is process-local and starts independently.
            task.terminal_reconciliation_cycle = 0U;
            task.resume_after_reconciliation =
                recovery.state != CandidateState::ambiguous &&
                recovery.attempt_count < recovery.max_attempts;
            task.retain_duplicate_height(
                *this, task.duplicate_source_id, task.height);
            persist_retired_duplicates(
                duplicates_.retire_height(task.duplicate_source_id,
                                          task.height),
                started_unix_us_);
            recovered_tasks.push_back(std::move(task));
        }

        // Every restored row belonged to a job from a prior process and is
        // therefore logically retired. Candidate tasks above hold the only
        // cross-restart references that may keep a bucket alive. Ordinary
        // keys with no recoverable candidate are retired here immediately,
        // preventing a permanent active-row/capacity leak across restarts.
        for (const auto &[source_id, height] : restored_duplicate_buckets) {
            persist_retired_duplicates(
                duplicates_.retire_height(source_id, height),
                started_unix_us_);
        }

        // Complete one authoritative recovery lookup for every durable
        // candidate before any new template or listener is exposed.  This
        // closes the restart window in which a previously accepted block
        // could otherwise be submitted again before its daemon state was
        // checked.  Candidate workers are intentionally not running yet.
        for (auto &task : recovered_tasks) {
            if (reconcile_candidate(task)) {
                task.release_duplicate_height();
                continue;
            }
            if (!database_operational_.load(std::memory_order_acquire)) {
                throw DatabaseError(
                    "candidate recovery reconciliation persistence failed");
            }
            ++task.reconciliation_cycle;
            if (task.resume_after_reconciliation) {
                task.reconciliation_only = false;
                task.resume_after_reconciliation = false;
                task.not_before = std::chrono::steady_clock::now();
            }
            else {
                static constexpr std::array<std::uint32_t, 6> delays{
                    0U, 5U, 30U, 120U, 600U, 3600U};
                ++task.terminal_reconciliation_cycle;
                const std::size_t delay_index = std::min<std::size_t>(
                    task.terminal_reconciliation_cycle,
                    delays.size() - 1U);
                const std::uint32_t delay_seconds = delays[delay_index];
                database_.schedule_candidate_reconciliation(
                    task.candidate_id,
                    unix_time_us() +
                        static_cast<std::int64_t>(delay_seconds) * 1'000'000LL);
                task.not_before = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(delay_seconds);
            }
            if (!enqueue_candidate(std::move(task))) {
                throw DatabaseError(
                    "candidate recovery queue allocation failed");
            }
        }

        refresh_template("startup");

        // Four workers may be occupied by the globally bounded reconciliation
        // class while two remain available for due candidate submissions.
        candidate_threads_.reserve(6U);
        for (std::size_t index = 0; index < 6U; ++index) {
            candidate_threads_.emplace_back(
                [this](std::stop_token token) { candidate_loop(token); });
        }
        stratum_ = std::make_unique<StratumServer>(
            stratum_config(config_),
            [this](const MinerConnection &connection) { return make_job(connection); },
            [this](const StratumSubmission &submission) {
                try {
                    return process_share(submission);
                }
                catch (const DatabaseError &) {
                    mark_database_unavailable();
                    throw;
                }
            },
            &defense_,
            [this](const MinerConnection &connection, std::string_view event) {
                observe_connection(connection, event);
            },
            [this](const StratumSubmission &submission) {
                return admit_submission(submission);
            },
            [this] {
                internal_failure_.store(true, std::memory_order_release);
                stratum_operational_.store(false, std::memory_order_release);
                ready_.store(false, std::memory_order_release);
                running_.store(false, std::memory_order_release);
            });
        stratum_->start();
        stratum_operational_.store(true, std::memory_order_release);

        if (config_.daemon.zmq_address.has_value()) {
            try {
                zmq_ = std::make_unique<ZmqSubscriber>(
                    *config_.daemon.zmq_address,
                    [this](std::string_view) {
                        template_refresh_requested_.store(true, std::memory_order_release);
                    },
                    [this](std::string_view message) {
                        (void)message;
                        zmq_operational_.store(false,
                                               std::memory_order_release);
                        logger_.log(
                            logging::Severity::warning, "zmq.failed",
                            {{logging::PublicStringKey::reason_code,
                              "subscriber_error"},
                             {logging::PublicStringKey::state, "degraded"}});
                        emit("template_error", {{"component", "zmq"},
                                                {"message", message}});
                    });
                // Publish the optimistic starting state before launching the
                // subscriber so an immediate asynchronous failure cannot be
                // overwritten back to healthy by this thread.
                zmq_operational_.store(true, std::memory_order_release);
                zmq_->start();
                logger_.log(
                    logging::Severity::info, "zmq.ready",
                    {{logging::PublicStringKey::zmq_endpoint,
                      *config_.daemon.zmq_address},
                     {logging::PublicStringKey::state, "running"}});
            }
            catch (const std::exception &error) {
                zmq_operational_.store(false, std::memory_order_release);
                logger_.log(
                    logging::Severity::warning, "zmq.failed",
                    {{logging::PublicStringKey::reason_code,
                      "subscriber_start_failed"},
                     {logging::PublicStringKey::state, "degraded"}});
                emit("template_error", {{"component", "zmq"},
                                        {"message", error.what()}});
            }
        }
        template_thread_ = std::jthread(
            [this](std::stop_token token) { template_loop(token); });
        startup_complete_.store(true, std::memory_order_release);
        update_readiness();
        emit("server_ready", {{"height", current_height_.load()}});
        logger_.log(
            logging::Severity::info, "runtime.ready",
            {{logging::PublicStringKey::session_id,
              hex_encode(session_public_id_)},
             {logging::PublicStringKey::state, "ready"}},
            {{logging::IntegerKey::height,
              current_height_.load(std::memory_order_acquire)}});
    }
    catch (...) {
        logger_.log(
            logging::Severity::error, "runtime.start_failed",
            {{logging::PublicStringKey::reason_code, "startup_failed"},
             {logging::PublicStringKey::state, "failed"}});
        stop_locked();
        throw;
    }
}

void Runtime::stop() noexcept {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    stop_locked();
}

void Runtime::stop_locked() noexcept {
    bool expected_stop = false;
    if (!stop_started_.compare_exchange_strong(
            expected_stop, true, std::memory_order_acq_rel)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    bool clean_shutdown = startup_complete_.exchange(
        false, std::memory_order_acq_rel);
    if (internal_failure_.load(std::memory_order_acquire)) {
        clean_shutdown = false;
    }
    ready_.store(false, std::memory_order_release);
    logger_.log(
        logging::Severity::info, "runtime.stopping",
        {{logging::PublicStringKey::state, "stopping"}});
    try { emit("server_stopping", {}); } catch (...) {}

    // Stop accepting sockets and drain every syntactically admitted submit
    // while the completion mailbox, verifier, and daemon are still live.
    if (stratum_) stratum_->stop();
    stratum_operational_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(state_mutex_);
        const bool verification_waiters_drained = std::all_of(
            pending_verifications_.begin(), pending_verifications_.end(),
            [](const auto &entry) { return entry.second == 0U; });
        if (!accepted_submits_.empty() || !verification_waiters_drained) {
            clean_shutdown = false;
        }
    }
    if (!database_operational_.load(std::memory_order_acquire)) {
        clean_shutdown = false;
    }

    if (zmq_) zmq_->stop();
    zmq_operational_.store(false, std::memory_order_release);
    for (auto &thread : candidate_threads_) thread.request_stop();
    candidate_condition_.notify_all();
    if (template_thread_.joinable()) {
        template_thread_.request_stop(); template_thread_.join();
    }
    for (auto &thread : candidate_threads_) if (thread.joinable()) thread.join();
    candidate_threads_.clear();
    {
        std::deque<CandidateTask> abandoned;
        std::lock_guard lock(candidate_mutex_);
        abandoned.swap(candidate_queue_);
        reconciling_candidates_.clear();
    }
    daemon_.stop();
    if (verifier_) {
        try {
            if (verifier_->shutdown(verifier::ShutdownMode::drain) != MSPV_OK) {
                clean_shutdown = false;
            }
        }
        catch (...) { clean_shutdown = false; }
        if (verifier_thread_.joinable()) {
            verifier_thread_.request_stop(); verifier_thread_.join();
        }
        try {
            const auto drain_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(5);
            for (;;) {
                auto drained = verifier_->drain_completions();
                if (drained.terminal_status == MSPV_CLOSED) break;
                if (drained.terminal_status != MSPV_OK &&
                    drained.terminal_status != MSPV_TIMEOUT) {
                    clean_shutdown = false;
                    break;
                }
                if (std::chrono::steady_clock::now() >= drain_deadline) {
                    clean_shutdown = false;
                    break;
                }
                if (drained.completions.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        catch (...) { clean_shutdown = false; }
    }
    verifier_mailbox_.close();
    verifier_operational_.store(false, std::memory_order_release);
    if (blocknotify_) blocknotify_->stop();
    if (committed_event_thread_.joinable()) {
        committed_event_thread_.request_stop(); committed_event_thread_.join();
    }
    if (events_) events_->stop();
    if (api_) api_->stop();
    if (!database_operational_.load(std::memory_order_acquire)) {
        clean_shutdown = false;
    }
    if (session_id_ != 0) {
        try {
            database_.finish_session(session_id_, unix_time_us(), clean_shutdown);
        }
        catch (...) {}
    }
    logger_.log(
        clean_shutdown ? logging::Severity::info : logging::Severity::warning,
        "runtime.stopped",
        {{logging::PublicStringKey::state,
          clean_shutdown ? "clean" : "unclean"}});
}

} // namespace monero_solo
