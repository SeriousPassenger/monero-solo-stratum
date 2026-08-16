#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace monero_solo {

using PublicId = std::array<std::uint8_t, 16>;
using Hash32 = std::array<std::uint8_t, 32>;
using DuplicateKey = std::array<std::uint8_t, 48>;
using ByteVector = std::vector<std::uint8_t>;

class DatabaseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DatabaseOptions {
    std::string path;
    std::uint32_t busy_timeout_ms{5000};
    bool blocknotify_enabled{false};
    std::uint64_t max_writer_queue_items{100000};
    std::uint64_t max_writer_queue_bytes{67108864};
    std::uint64_t writer_priority_reserve_items{0};
    std::uint64_t min_persisted_share_difficulty{80000000000ULL};
    std::uint32_t accounting_flush_interval_ms{1000};
};

/*
 * A snapshot of commands admitted to, but not currently executing on, the
 * single SQLite writer connection. Envelope bytes are fixed at 512 bytes, so
 * the byte count is exact and never includes referenced immutable blobs.
 */
struct DatabaseWriterStats {
    std::uint64_t queued_items{};
    std::uint64_t queued_bytes{};
    std::uint64_t priority_items{};
    std::uint64_t pending_accounting_items{};
    std::uint64_t pending_transient_shares{};
};

struct DatabasePragmas {
    std::string journal_mode;
    std::string synchronous;
    bool foreign_keys{};
    std::uint32_t busy_timeout_ms{};
};

struct SessionStart {
    PublicId public_id{};
    std::int64_t started_unix_us{};
    std::string version;
    std::optional<std::string> verifier_commit;
};

struct EventInsert {
    std::int64_t session_id{};
    std::int64_t created_unix_us{};
    std::string type;
    std::optional<std::int64_t> connection_id;
    std::optional<std::int64_t> worker_id;
    std::optional<std::int64_t> template_generation;
    std::optional<PublicId> job_public_id;
    std::optional<std::int64_t> share_id;
    std::optional<std::int64_t> candidate_id;
    std::optional<std::int64_t> round_id;
    std::string payload_json{"{\"payload_schema_version\":1,\"data\":{}}"};
};

struct PersistedEvent {
    std::int64_t id{};
    PublicId session_public_id{};
    std::int64_t created_unix_us{};
    std::string type;
    std::optional<PublicId> connection_public_id;
    std::optional<std::int64_t> worker_id;
    std::optional<std::int64_t> template_generation;
    std::optional<PublicId> job_public_id;
    std::optional<std::int64_t> share_id;
    std::optional<std::int64_t> candidate_id;
    std::optional<std::int64_t> round_id;
    std::string payload_json;
};

struct WorkerInsert {
    std::string login;
    std::string rigid;
    std::int64_t seen_unix_us{};
};

struct ConnectionInsert {
    PublicId public_id{};
    std::int64_t session_id{};
    std::optional<std::int64_t> worker_id;
    int peer_family{};
    ByteVector peer_address;
    int peer_port{};
    std::string listen_address;
    std::string agent;
    std::int64_t opened_unix_us{};
};

enum class CandidateVerdictKind {
    false_candidate,
    candidate_mismatch,
};

// A value snapshot of durable security evidence. In particular, peer_address
// is copied from the persisted connection; callers never have to consult
// mutable live connection state in order to score the verdict.
struct ActionableCandidateVerdict {
    std::int64_t abuse_event_id{};
    std::int64_t share_id{};
    std::int64_t connection_id{};
    CandidateVerdictKind kind{CandidateVerdictKind::false_candidate};
    int peer_family{};
    ByteVector peer_address;
};

struct InterruptedRuntimeRecovery {
    std::uint64_t sessions_stopped{};
    std::uint64_t connections_closed{};
    std::vector<ActionableCandidateVerdict> actionable_verdicts;
};

struct ShareInsert {
    std::int64_t session_id{};
    std::optional<std::int64_t> connection_id;
    std::optional<std::int64_t> worker_id;
    std::optional<PublicId> job_public_id;
    std::optional<std::int64_t> template_generation;
    std::optional<std::uint64_t> height;
    std::uint64_t request_sequence{};
    std::optional<std::string> miner_request_id_type;
    std::optional<std::string> miner_request_id_text;
    std::int64_t received_unix_us{};
    std::optional<std::array<std::uint8_t, 4>> nonce;
    std::optional<std::string> assigned_difficulty_dec;
    std::optional<std::string> network_difficulty_dec;
    bool height_is_older{};
    bool claimed_candidate{};
    std::string candidate_admission{"not_candidate"};
    std::string status{"received"};
    std::string provenance{"pending"};
};

enum class CandidateState {
    journaled,
    dispatching,
    retry_wait,
    accepted,
    rejected,
    ambiguous,
    accepted_by_reconciliation,
};

struct CandidateJournal {
    Hash32 candidate_key{};
    std::optional<std::int64_t> first_share_id;
    std::int64_t session_id{};
    std::int64_t round_id{};
    PublicId job_public_id{};
    std::int64_t template_generation{};
    std::int64_t connection_id{};
    std::uint64_t height{};
    int peer_family{};
    ByteVector peer_address;
    ByteVector frozen_block_blob;
    Hash32 miner_tx_hash{};
    std::optional<Hash32> expected_block_id;
    std::uint32_t max_attempts{4};
    std::int64_t created_unix_us{};
};

struct CandidateJournalResult {
    std::int64_t candidate_id{};
    bool inserted{};
    CandidateState state{CandidateState::journaled};
    // True means the origin round is no longer open or a higher-height share
    // was already admitted to it. No candidate row is inserted and
    // candidate_id remains zero.
    bool round_contaminated{};
};

enum class CandidateAttemptClassification {
    accepted,
    explicit_rejection,
    indeterminate,
};

struct CandidateAttemptCompletion {
    CandidateAttemptClassification classification{CandidateAttemptClassification::indeterminate};
    std::int64_t completed_unix_us{};
    bool trusted_mode{};
    std::optional<int> http_status;
    std::optional<int> rpc_error_code;
    std::optional<std::string> daemon_status;
    std::optional<Hash32> daemon_block_id;
    std::optional<std::string> response_excerpt;
};

struct CandidateAttemptResult {
    CandidateState state{CandidateState::journaled};
    bool terminal{};
    std::uint32_t attempt_count{};
    bool had_indeterminate{};
    std::uint32_t newly_actionable_false_candidates{};
    std::uint32_t newly_actionable_candidate_mismatches{};
    bool trusted_candidate_rejection_recorded{};
};

enum class ReconciliationLookupKind {
    expected_hash,
    height,
};

enum class ReconciliationClassification {
    positive,
    inconclusive,
    indeterminate,
};

struct CandidateReconciliationStart {
    std::int64_t candidate_id{};
    std::uint32_t cycle_number{};
    ReconciliationLookupKind lookup_kind{ReconciliationLookupKind::expected_hash};
    std::int64_t rpc_request_id{};
    std::optional<Hash32> requested_block_id;
    std::int64_t started_unix_us{};
};

struct CandidateReconciliationStartResult {
    std::int64_t reconciliation_id{};
    bool inserted{};
};

struct CandidateReconciliationCompletion {
    ReconciliationClassification classification{ReconciliationClassification::indeterminate};
    std::int64_t completed_unix_us{};
    std::optional<Hash32> observed_block_id;
    std::optional<std::uint64_t> observed_height;
    std::optional<Hash32> observed_miner_tx_hash;
    std::optional<bool> observed_orphan;
    std::optional<std::string> response_excerpt;
};

struct CandidateReconciliationResult {
    CandidateState candidate_state{CandidateState::journaled};
    bool candidate_accepted{};
    bool already_completed{};
};

struct CandidateRecovery {
    std::int64_t candidate_id{};
    Hash32 candidate_key{};
    std::optional<std::int64_t> first_share_id;
    std::int64_t round_id{};
    PublicId job_public_id{};
    std::int64_t template_generation{};
    std::int64_t connection_id{};
    std::uint64_t height{};
    int peer_family{};
    ByteVector peer_address;
    ByteVector frozen_block_blob;
    Hash32 miner_tx_hash{};
    std::optional<Hash32> expected_block_id;
    CandidateState state{CandidateState::journaled};
    std::uint32_t attempt_count{};
    std::uint32_t max_attempts{};
    bool had_indeterminate{};
    std::uint32_t reconciliation_cycle_count{};
    std::int64_t created_unix_us{};
    std::optional<std::int64_t> next_reconciliation_unix_us;
};

enum class HashrateSource {
    verified,
    claimed,
};

struct ShareAcceptanceResult {
    bool accepted{};
    std::int64_t round_id{};
    std::int64_t event_id{};
    // Zero for an ordinary transient-only share, otherwise the durable row ID.
    std::int64_t persisted_share_id{};
};

struct PersistedShareIdentity {
    std::int64_t share_id{};
    std::int64_t round_id{};
};

struct ShareAcceptance {
    std::int64_t share_id{};
    std::int64_t completed_unix_us{};
    std::string assigned_difficulty_dec;
    HashrateSource source{HashrateSource::verified};
    std::optional<std::string> actual_difficulty_dec;
    std::optional<std::string> verifier_ticket_dec;
    std::optional<std::string> verifier_seed_id_dec;
    std::optional<std::uint64_t> verifier_queue_ns;
    std::optional<std::uint64_t> verifier_hash_ns;
    std::optional<std::uint64_t> verifier_total_ns;
};

struct ShareFinalization {
    std::string status;
    std::string provenance;
    std::int64_t completed_unix_us{};
    std::optional<std::string> actual_difficulty_dec;
    std::optional<std::string> error_code;
    std::optional<std::string> error_message;
    std::optional<std::string> verifier_ticket_dec;
    std::optional<std::string> verifier_seed_id_dec;
    std::optional<std::uint64_t> verifier_queue_ns;
    std::optional<std::uint64_t> verifier_hash_ns;
    std::optional<std::uint64_t> verifier_total_ns;
};

struct ShareFinalizationResult {
    bool finalized{};
    // Zero for an ordinary transient-only share, otherwise the durable row ID.
    std::int64_t persisted_share_id{};

    [[nodiscard]] operator bool() const noexcept { return finalized; }
};

struct HashrateWindows {
    std::string one_minute{"0"};
    std::string five_minutes{"0"};
    std::string ten_minutes{"0"};
    std::string one_hour{"0"};
    std::string six_hours{"0"};
    std::string twenty_four_hours{"0"};
};

struct AbuseEventInsert {
    std::optional<std::int64_t> connection_id;
    std::optional<std::int64_t> share_id;
    std::optional<std::int64_t> candidate_id;
    int peer_family{};
    ByteVector peer_address;
    std::string kind;
    int weight{1};
    std::int64_t created_unix_us{};
    std::optional<std::string> detail;
};

struct BanInsert {
    int peer_family{};
    ByteVector peer_address;
    std::int64_t created_unix_us{};
    std::int64_t expires_unix_us{};
    std::int64_t evidence_window_started_unix_us{};
    std::int64_t evidence_window_ended_unix_us{};
    std::string reason;
    std::vector<std::int64_t> abuse_event_ids;
};

struct ActiveBan {
    std::int64_t id{};
    int peer_family{};
    ByteVector peer_address;
    std::int64_t expires_unix_us{};
    std::string reason;
};

enum class CandidateVerdictDisposition {
    pending,
    actionable,
    suppressed,
};

struct CandidateVerdictInsert {
    std::int64_t share_id{};
    CandidateVerdictKind kind{CandidateVerdictKind::false_candidate};
    Hash32 candidate_key{};
    std::optional<std::int64_t> candidate_id;
    std::int64_t created_unix_us{};
};

struct CandidateVerdictResult {
    CandidateVerdictDisposition disposition{CandidateVerdictDisposition::pending};
    std::optional<std::int64_t> abuse_event_id;
};

struct BlocknotifyDelivery {
    std::int64_t id{};
    std::int64_t candidate_id{};
    Hash32 miner_tx_hash{};
    std::uint32_t attempt_count{};
};

struct BlocknotifyCompletion {
    bool delivered{};
    std::int64_t completed_unix_us{};
    std::optional<int> exit_code;
    std::optional<int> term_signal;
    std::optional<std::string> stderr_excerpt;
    std::optional<std::string> last_error;
};

class Database final {
public:
    explicit Database(DatabaseOptions options);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;

    [[nodiscard]] const DatabaseOptions &options() const noexcept;
    [[nodiscard]] DatabaseWriterStats writer_stats() const noexcept;
    [[nodiscard]] DatabasePragmas pragmas() const;
    [[nodiscard]] std::uint32_t schema_version() const;

    [[nodiscard]] std::int64_t start_session(const SessionStart &session);
    // Idempotently closes process-local state that cannot survive a process
    // restart. Call this before creating the replacement server session.
    [[nodiscard]] InterruptedRuntimeRecovery recover_interrupted_runtime(
        std::int64_t recovered_unix_us);
    void finish_session(std::int64_t session_id, std::int64_t stopped_unix_us,
                        bool clean_shutdown);
    [[nodiscard]] std::int64_t ensure_open_round(std::int64_t opened_unix_us);
    [[nodiscard]] std::int64_t current_open_round_id() const;
    [[nodiscard]] std::optional<std::uint64_t> latest_accepted_height() const;

    [[nodiscard]] std::int64_t insert_event(const EventInsert &event);
    [[nodiscard]] std::int64_t event_high_water_mark() const;
    [[nodiscard]] std::vector<PersistedEvent> load_events_after(
        std::int64_t last_event_id, std::size_t limit) const;

    [[nodiscard]] std::int64_t upsert_worker(const WorkerInsert &worker);
    [[nodiscard]] std::int64_t insert_connection(const ConnectionInsert &connection);
    [[nodiscard]] std::int64_t upsert_worker_and_authenticate_connection(
        std::int64_t connection_id, const WorkerInsert &worker,
        std::string_view agent, std::int64_t authenticated_unix_us);
    void authenticate_connection(std::int64_t connection_id,
                                 std::int64_t worker_id,
                                 std::string_view agent,
                                 std::int64_t authenticated_unix_us);
    [[nodiscard]] bool update_connection_last_sent_height(
        std::int64_t connection_id, std::uint64_t height);
    [[nodiscard]] bool close_connection(std::int64_t connection_id,
                                        std::int64_t closed_unix_us,
                                        std::string_view reason);
    [[nodiscard]] std::int64_t insert_share(const ShareInsert &share);
    [[nodiscard]] PersistedShareIdentity ensure_share_persisted(
        std::int64_t share_id, std::string_view retention_reason);
    void flush_accounting();

    [[nodiscard]] CandidateJournalResult journal_candidate(const CandidateJournal &candidate);
    [[nodiscard]] std::optional<CandidateJournalResult> find_candidate_by_key(
        const Hash32 &candidate_key) const;
    void attach_share_to_candidate(std::int64_t share_id,
                                   std::int64_t candidate_id,
                                   std::string_view admission = "existing");
    [[nodiscard]] std::int64_t start_candidate_attempt(
        std::int64_t candidate_id, std::uint32_t attempt_number,
        std::int64_t rpc_request_id, std::int64_t started_unix_us);
    [[nodiscard]] CandidateAttemptResult finish_candidate_attempt(
        std::int64_t candidate_id, std::uint32_t attempt_number,
        const CandidateAttemptCompletion &completion);
    [[nodiscard]] CandidateReconciliationStartResult start_candidate_reconciliation(
        const CandidateReconciliationStart &reconciliation);
    [[nodiscard]] CandidateReconciliationResult finish_candidate_reconciliation(
        std::int64_t reconciliation_id,
        const CandidateReconciliationCompletion &completion);
    void schedule_candidate_reconciliation(std::int64_t candidate_id,
                                           std::int64_t next_unix_us);
    [[nodiscard]] bool exhaust_candidate_reconciliation(
        std::int64_t candidate_id, std::int64_t exhausted_unix_us);
    [[nodiscard]] bool accept_candidate(
        std::int64_t candidate_id, std::int64_t accepted_unix_us,
        std::optional<Hash32> canonical_block_id = std::nullopt,
        bool by_reconciliation = false);
    [[nodiscard]] std::vector<CandidateRecovery> recoverable_candidates() const;

    [[nodiscard]] ShareAcceptanceResult accept_share(
        const ShareAcceptance &acceptance);
    [[nodiscard]] ShareAcceptanceResult accept_share(
        std::int64_t share_id, std::int64_t completed_unix_us,
        std::string_view assigned_difficulty_dec, HashrateSource source);
    void mark_share_verifying(std::int64_t share_id,
                              std::string_view verifier_ticket_dec,
                              std::string_view verifier_seed_id_dec);
    void set_share_height_is_older(std::int64_t share_id,
                                   bool height_is_older);
    void insert_share_hash(std::int64_t share_id, std::string_view role,
                           const Hash32 &hash,
                           std::optional<bool> meets_share_target,
                           std::optional<bool> meets_network_target);
    [[nodiscard]] ShareFinalizationResult finalize_share(
        std::int64_t share_id, const ShareFinalization &finalization);
    void set_candidate_admission(std::int64_t share_id,
                                 std::string_view admission);
    [[nodiscard]] HashrateWindows hashrate(
        std::string_view scope_type, std::int64_t scope_id,
        HashrateSource source, std::int64_t now_second_utc) const;

    [[nodiscard]] std::int64_t insert_abuse_event(const AbuseEventInsert &event);
    [[nodiscard]] std::int64_t create_ban(const BanInsert &ban);
    [[nodiscard]] std::vector<ActiveBan> load_active_bans(std::int64_t now_unix_us);
    [[nodiscard]] std::uint64_t expire_bans(std::int64_t now_unix_us,
                                            std::int64_t event_session_id = 0);
    [[nodiscard]] CandidateVerdictResult record_candidate_verdict(
        const CandidateVerdictInsert &verdict);

    void recover_blocknotify_deliveries();
    [[nodiscard]] std::optional<BlocknotifyDelivery> claim_next_blocknotify(
        std::int64_t now_unix_us);
    void finish_blocknotify(std::int64_t delivery_id,
                            const BlocknotifyCompletion &completion);
    [[nodiscard]] std::uint64_t pending_blocknotify_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view to_string(CandidateState value) noexcept;
[[nodiscard]] std::string_view to_string(HashrateSource value) noexcept;

} // namespace monero_solo
