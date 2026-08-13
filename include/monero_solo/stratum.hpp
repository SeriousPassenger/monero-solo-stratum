#pragma once

#include "monero_solo/defense.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace monero_solo {

using MinerRequestId = std::variant<std::int64_t, std::string>;

struct StratumJob {
    std::string blob;
    std::string job_id;
    std::string target;
    std::string seed_hash;
    std::uint64_t height{};
    /* Called synchronously only after the complete job frame is queued. */
    std::function<void(std::string_view wire_target,
                       std::string_view target_encoding)> on_queued;
    // Internal admission metadata; never emitted on the wire.
    std::string network_difficulty;
};

struct MinerConnection {
    std::string public_id;
    PeerAddress peer;
    std::uint16_t peer_port{};
    std::string listen_address;
    std::string login;
    std::string rigid;
    std::string agent;
    // Effective work difficulty. Compact-target clients may require this to
    // be quantized to the exact difficulty representable by their wire target.
    std::uint64_t assigned_difficulty{};
    std::uint64_t last_sent_height{};
    std::uint64_t request_sequence{};
};

struct StratumSubmission {
    MinerConnection connection;
    MinerRequestId request_id;
    std::uint64_t request_sequence{};
    std::int64_t received_unix_us{};
    std::string job_id;
    std::array<std::uint8_t, 4> nonce{};
    std::array<std::uint8_t, 32> claimed_hash{};
    std::string claimed_hash_hex;
    // Runtime-owned state acquired at the syntactic-admission boundary.  The
    // Stratum layer deliberately treats this as opaque, but keeps it alive
    // until the submit handler has finished with the exact admitted job.
    std::shared_ptr<void> job_lease;
    // A connection-local atomic view, updated only after a complete job frame
    // enters the bounded output queue.  This permits final stale
    // classification to observe jobs queued while verification was running.
    std::shared_ptr<const std::atomic<std::uint64_t>> latest_queued_height_view;

    [[nodiscard]] std::uint64_t latest_queued_height() const noexcept {
        return latest_queued_height_view
                   ? latest_queued_height_view->load(std::memory_order_acquire)
                   : connection.last_sent_height;
    }
};

struct StratumAdmission {
    std::shared_ptr<void> job_lease;
    // Exact network difficulty from the admitted job.  Empty means that the
    // job is unknown/not owned and therefore cannot use the candidate lane.
    std::string network_difficulty;
};

enum class ShareDisposition {
    accepted,
    stale,
    duplicate,
    low_difficulty,
    invalid_result,
    unknown_job,
    server_busy,
    verifier_failed,
    cancelled,
};

struct ShareResponse {
    ShareDisposition disposition{ShareDisposition::server_busy};
    std::string internal_code;
};

struct StratumServerConfig {
    std::vector<std::string> listen;
    std::string access_password;
    std::size_t max_connections{2048};
    std::size_t max_connections_per_ip{128};
    std::uint32_t login_timeout_ms{10000};
    std::uint32_t idle_timeout_ms{300000};
    std::size_t max_line_bytes{16384};
    std::size_t max_output_bytes_per_connection{1024U * 1024U};
    std::size_t max_json_depth{32};
    std::uint64_t difficulty_floor{1048576};
    bool minimum_difficulty{};
    std::size_t submit_workers{4};
    // Dedicated, prestarted claimed-candidate workers. Runtime derives this
    // from available hardware; direct test/embedded users default to one.
    std::size_t candidate_submit_workers{1};
    std::size_t max_pending_submits{4096};
    std::size_t candidate_submit_reserve{64};
    // Running plus queued submissions for one connection.  Eight matches the
    // normal per-connection verifier bound while remaining independent of the
    // number of miners sharing a rental-service source IP.
    std::size_t max_pending_submits_per_connection{8};
};

class StratumServer final {
public:
    using JobProvider = std::function<std::optional<StratumJob>(const MinerConnection &)>;
    using SubmitHandler = std::function<ShareResponse(const StratumSubmission &)>;
    using ConnectionObserver = std::function<void(const MinerConnection &, std::string_view)>;
    using AdmissionResolver = std::function<StratumAdmission(const StratumSubmission &)>;
    using FatalErrorHandler = std::function<void()>;

    StratumServer(StratumServerConfig config, JobProvider jobs,
                  SubmitHandler submits, DefenseEngine *defense = nullptr,
                  ConnectionObserver observer = {},
                  AdmissionResolver admission_resolver = {},
                  FatalErrorHandler fatal_error_handler = {});
    StratumServer(const StratumServer &) = delete;
    StratumServer &operator=(const StratumServer &) = delete;
    ~StratumServer();

    void start();
    void stop() noexcept;
    void refresh_jobs();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t connection_count() const;
    [[nodiscard]] std::vector<std::string> bound_endpoints() const;

private:
    struct Listener;
    struct Connection;
    struct PendingSubmit;
    struct CompletedSubmit;

    void event_loop() noexcept;
    void report_event_loop_failure() noexcept;
    void wake_event_loop() noexcept;
    void drain_event_loop_wakeup() noexcept;
    void submit_worker(std::stop_token token, bool candidate_lane) noexcept;
    void accept_ready(Listener &listener);
    void read_ready(const std::shared_ptr<Connection> &connection);
    void write_ready(const std::shared_ptr<Connection> &connection);
    void process_line(const std::shared_ptr<Connection> &connection, std::string line);
    void handle_login(const std::shared_ptr<Connection> &connection,
                      const nlohmann::json &document, const MinerRequestId &request_id,
                      std::string request_key);
    void handle_submit(const std::shared_ptr<Connection> &connection,
                       const nlohmann::json &document, const MinerRequestId &request_id,
                       std::string request_key);
    void handle_keepalive(const std::shared_ptr<Connection> &connection,
                          const nlohmann::json &document, const MinerRequestId &request_id,
                          std::string request_key);
    bool queue(const std::shared_ptr<Connection> &connection, std::string data,
               std::optional<std::string> release_key = std::nullopt,
               std::optional<std::uint64_t> sent_height = std::nullopt);
    void queue_error(const std::shared_ptr<Connection> &connection,
                     const MinerRequestId &id, int code, std::string_view message,
                     std::optional<std::string> release_key = std::nullopt);
    void close_connection(const std::shared_ptr<Connection> &connection,
                          std::string_view reason) noexcept;
    void drain_submit_results();
    void refresh_jobs_now();
    void remember_job(const std::shared_ptr<Connection> &connection,
                      const StratumJob &job);
    void close_expired();

    StratumServerConfig config_;
    JobProvider jobs_;
    SubmitHandler submits_;
    DefenseEngine *defense_{};
    ConnectionObserver observer_;
    AdmissionResolver admission_resolver_;
    FatalErrorHandler fatal_error_handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> jobs_refresh_requested_{false};
    std::jthread event_thread_;
    std::mutex wake_mutex_;
    int wake_read_descriptor_{-1};
    int wake_write_descriptor_{-1};
    std::vector<std::jthread> submit_threads_;
    std::vector<std::jthread> candidate_submit_threads_;
    mutable std::mutex state_mutex_;
    std::vector<Listener> listeners_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::map<PeerAddress, std::size_t> peer_connections_;
    std::mutex submit_mutex_;
    std::condition_variable submit_condition_;
    std::deque<PendingSubmit> pending_submits_;
    std::deque<PendingSubmit> pending_candidate_submits_;
    std::deque<CompletedSubmit> completed_submits_;
};

[[nodiscard]] std::string share_disposition_message(ShareDisposition disposition);

} // namespace monero_solo
