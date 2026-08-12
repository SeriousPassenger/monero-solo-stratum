#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace monero_solo {

enum class RpcObservationKind {
    valid,
    transport_error,
    http_error,
    response_too_large,
    malformed_json,
    mismatched_id,
    invalid_envelope,
};

struct RpcObservation {
    RpcObservationKind kind{RpcObservationKind::transport_error};
    std::int64_t request_id{};
    long http_status{};
    nlohmann::json document;
    std::string diagnostic;
    std::string response_excerpt;

    [[nodiscard]] bool valid() const noexcept {
        return kind == RpcObservationKind::valid;
    }
};

enum class SubmitClassification {
    accepted,
    explicit_rejection,
    indeterminate,
};

struct SubmitObservation {
    SubmitClassification classification{SubmitClassification::indeterminate};
    std::int64_t request_id{};
    long http_status{};
    std::optional<std::int64_t> rpc_error_code;
    std::optional<std::string> daemon_status;
    std::optional<std::string> daemon_block_id;
    std::string response_excerpt;
    std::string diagnostic;
};

struct ReconciliationEvidence {
    bool positive{};
    bool indeterminate{};
    std::optional<std::string> block_id;
    std::optional<std::uint64_t> height;
    std::optional<std::string> miner_tx_hash;
    std::optional<bool> orphan;
    std::optional<std::string> blob_hex;
    std::string response_excerpt;
    std::string diagnostic;
};

struct DaemonRequestCounts {
    std::uint32_t inflight{};
    std::uint32_t pending{};
};

class DaemonRpcClient final {
public:
    DaemonRpcClient(std::string rpc_url,
                    std::string username,
                    std::string password,
                    std::uint32_t timeout_ms,
                    std::size_t max_response_bytes,
                    std::uint32_t max_concurrent_requests = 8,
                    std::uint32_t max_pending_requests = 256);

    DaemonRpcClient(const DaemonRpcClient &) = delete;
    DaemonRpcClient &operator=(const DaemonRpcClient &) = delete;
    DaemonRpcClient(DaemonRpcClient &&) = delete;
    DaemonRpcClient &operator=(DaemonRpcClient &&) = delete;
    ~DaemonRpcClient() = default;

    [[nodiscard]] RpcObservation json_rpc(
        std::string_view method, const nlohmann::json &params,
        const std::function<void(std::int64_t)> &before_dispatch = {});
    [[nodiscard]] RpcObservation endpoint(std::string_view path,
                                          const nlohmann::json &body);
    [[nodiscard]] RpcObservation get_info();
    [[nodiscard]] RpcObservation get_height();
    [[nodiscard]] RpcObservation get_block_template(std::string_view wallet_address);
    /*
     * before_dispatch is invoked with the exact JSON-RPC request ID after the
     * request has been fully encoded but before any network I/O begins.  The
     * candidate journal uses this barrier to durably record the attempt first.
     */
    [[nodiscard]] SubmitObservation submit_block(
        std::span<const std::uint8_t> frozen_block,
        const std::function<void(std::int64_t)> &before_dispatch = {});
    [[nodiscard]] RpcObservation get_block_by_hash(
        std::string_view expected_hash,
        const std::function<void(std::int64_t)> &before_dispatch = {});
    [[nodiscard]] RpcObservation get_block_by_height(
        std::uint64_t height,
        const std::function<void(std::int64_t)> &before_dispatch = {});

    [[nodiscard]] const std::string &redacted_url() const noexcept { return rpc_url_; }
    [[nodiscard]] DaemonRequestCounts request_counts() const noexcept;
    void stop() noexcept;

    static SubmitObservation classify_submit(const RpcObservation &observation);
    static ReconciliationEvidence classify_reconciliation(
        const RpcObservation &observation,
        std::uint64_t expected_height,
        std::string_view expected_miner_tx_hash,
        std::string_view expected_block_id,
        std::optional<std::string_view> requested_block_id = std::nullopt);

private:
    [[nodiscard]] std::int64_t next_request_id() noexcept;
    [[nodiscard]] RpcObservation post(std::string_view absolute_url,
                                      std::string_view encoded_body,
                                      std::optional<std::int64_t> expected_id,
                                      bool priority = false,
                                      const std::function<void(std::int64_t)> &
                                          before_dispatch = {});
    [[nodiscard]] bool acquire_request_slot(bool priority);
    void release_request_slot() noexcept;

    std::string rpc_url_;
    std::string username_;
    std::string password_;
    std::uint32_t timeout_ms_;
    std::size_t max_response_bytes_;
    std::uint32_t max_concurrent_requests_;
    std::uint32_t max_pending_requests_;
    std::atomic<std::int64_t> next_id_{1};
    mutable std::mutex scheduler_mutex_;
    std::condition_variable scheduler_condition_;
    std::uint32_t scheduler_inflight_{};
    std::uint32_t scheduler_waiting_{};
    std::uint32_t scheduler_priority_waiting_{};
    bool scheduler_stopping_{};
};

[[nodiscard]] bool is_hex_64(std::string_view value) noexcept;

} // namespace monero_solo
