#pragma once

#include "monero_solo/api.hpp"
#include "monero_solo/blocknotify.hpp"
#include "monero_solo/config.hpp"
#include "monero_solo/daemon.hpp"
#include "monero_solo/database.hpp"
#include "monero_solo/defense.hpp"
#include "monero_solo/duplicate_registry.hpp"
#include "monero_solo/entropy.hpp"
#include "monero_solo/event_stream.hpp"
#include "monero_solo/logger.hpp"
#include "monero_solo/monero.hpp"
#include "monero_solo/stratum.hpp"
#include "monero_solo/verifier.hpp"
#include "monero_solo/verifier_mailbox.hpp"
#include "monero_solo/zmq.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <semaphore>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace monero_solo {

class Runtime final {
public:
    explicit Runtime(Config config);
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    ~Runtime();

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool ready() const noexcept;

private:
    struct TemplateContext;
    struct JobContext;
    struct JobLease;
    struct CandidateTask;
    struct VerificationResult;

    void validate_daemon_network();
    [[nodiscard]] bool refresh_template(std::string reason);
    [[nodiscard]] std::shared_ptr<TemplateContext> parse_template(
        const RpcObservation &observation, std::string reason);
    [[nodiscard]] std::optional<StratumJob> make_job(const MinerConnection &connection);
    [[nodiscard]] StratumAdmission admit_submission(
        const StratumSubmission &submission);
    [[nodiscard]] ShareResponse process_share(const StratumSubmission &submission);
    void observe_connection(const MinerConnection &connection, std::string_view event);
    void retire_job_context(const std::shared_ptr<JobContext> &job,
                            std::int64_t retired_unix_us) noexcept;
    void persist_retired_duplicates(
        std::vector<DuplicateToken> tokens,
        std::int64_t retired_unix_us) noexcept;
    void apply_actionable_verdicts(
        const std::vector<ActionableCandidateVerdict> &verdicts) noexcept;
    void update_readiness() noexcept;
    void mark_database_unavailable() noexcept;
    void stop_locked() noexcept;

    void verifier_loop(std::stop_token token) noexcept;
    [[nodiscard]] std::optional<verifier::Completion> wait_verification(
        std::uint64_t share_id);
    void template_loop(std::stop_token token) noexcept;
    void entropy_loop(std::stop_token token) noexcept;
    [[nodiscard]] bool wait_for_zmq_tip_change(std::stop_token token);
    void candidate_loop(std::stop_token token) noexcept;
    void committed_event_loop(std::stop_token token) noexcept;
    [[nodiscard]] bool enqueue_candidate(CandidateTask task) noexcept;
    [[nodiscard]] CandidateJournalResult journal_candidate(
        std::int64_t share_id, const std::shared_ptr<JobContext> &job,
        const MinerConnection &connection, const ParsedBlock &frozen,
        bool claimed_path, bool bypass_admission,
        std::optional<bool> *admission_acquired = nullptr);
    [[nodiscard]] bool reconcile_candidate(const CandidateTask &task);
    [[nodiscard]] bool candidate_height_allowed_unlocked(
        std::uint64_t height) const noexcept;
    void register_candidate_boundary_unlocked(std::int64_t candidate_id,
                                              std::uint64_t height);
    void activate_accepted_candidate_fence_unlocked(std::uint64_t height);
    void release_candidate_boundary_unlocked(std::int64_t candidate_id) noexcept;
    void release_candidate_boundary(std::int64_t candidate_id) noexcept;
    [[nodiscard]] ApiDataSource api_data_source();
    void emit(std::string type, nlohmann::json data,
              std::optional<std::int64_t> connection_id = std::nullopt,
              std::optional<std::int64_t> job_id = std::nullopt,
              std::optional<std::int64_t> share_id = std::nullopt,
              std::optional<std::int64_t> candidate_id = std::nullopt,
              std::optional<std::int64_t> round_id = std::nullopt) noexcept;

    Config config_;
    logging::Logger logger_;
    Database database_;
    EntropyManager entropy_;
    DaemonRpcClient daemon_;
    DuplicateRegistry duplicates_;
    DefenseEngine defense_;
    std::unique_ptr<verifier::Verifier> verifier_;
    std::unique_ptr<EventStream> events_;
    std::unique_ptr<BlockNotifySupervisor> blocknotify_;
    std::unique_ptr<ZmqSubscriber> zmq_;
    std::unique_ptr<StratumServer> stratum_;
    std::unique_ptr<ApiService> api_;

    std::int64_t session_id_{};
    PublicId session_public_id_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_started_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> database_operational_{true};
    std::atomic<bool> template_operational_{false};
    std::atomic<bool> verifier_operational_{false};
    std::atomic<bool> stratum_operational_{false};
    std::atomic<bool> zmq_operational_{false};
    std::atomic<bool> startup_complete_{false};
    std::atomic<bool> internal_failure_{false};
    std::atomic<bool> job_issuance_allowed_{true};
    std::atomic<bool> template_refresh_requested_{false};
    std::atomic<std::uint64_t> template_generation_{0};
    std::atomic<std::uint64_t> current_height_{0};
    std::int64_t started_unix_us_{};
    std::int64_t event_stream_start_id_{};
    mspv_seed_id active_seed_id_{};

    mutable std::mutex state_mutex_;
    mutable std::shared_mutex job_issuance_mutex_;
    std::shared_ptr<TemplateContext> current_template_;
    std::unordered_map<std::string, std::shared_ptr<JobContext>> jobs_;
    std::unordered_map<std::string, std::int64_t> connection_ids_;
    std::unordered_map<std::string, std::int64_t> worker_ids_;
    std::unordered_map<std::string, std::deque<std::string>> connection_jobs_;
    std::unordered_map<std::string, std::size_t> pending_verifications_;
    std::unordered_map<std::string, std::size_t> accepted_submits_;
    std::unordered_set<std::string> disconnected_connections_;
    std::optional<std::string> last_error_;

    mutable std::mutex lifecycle_mutex_;

    verifier::CompletionMailbox verifier_mailbox_;
    std::jthread verifier_thread_;

    std::mutex candidate_mutex_;
    std::condition_variable candidate_condition_;
    std::deque<CandidateTask> candidate_queue_;
    std::unordered_set<std::int64_t> reconciling_candidates_;
    std::vector<std::jthread> candidate_threads_;
    std::counting_semaphore<4> reconciliation_slots_{4};
    mutable std::mutex candidate_boundary_mutex_;
    std::map<std::int64_t, std::uint64_t> unresolved_candidate_heights_;
    std::optional<std::uint64_t> accepted_candidate_height_fence_;
    std::jthread entropy_thread_;
    std::jthread template_thread_;
    std::jthread committed_event_thread_;
};

} // namespace monero_solo
