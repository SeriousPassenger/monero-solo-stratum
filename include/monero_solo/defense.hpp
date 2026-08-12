#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

namespace monero_solo {

struct PeerAddress {
    int family{};
    std::array<std::uint8_t, 16> bytes{};
    std::size_t size{};

    [[nodiscard]] static PeerAddress from_socket(const sockaddr_storage &storage);
    [[nodiscard]] static PeerAddress parse(std::string_view text);
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string key() const;
    auto operator<=>(const PeerAddress &) const = default;
};

enum class AbuseKind {
    malformed,
    authentication_failure,
    unknown_job,
    duplicate,
    oversized_line,
    verified_false_candidate,
    candidate_mismatch,
    trusted_candidate_rejection,
    rate_hammer,
};

struct DefensePolicyConfig {
    bool enabled{true};
    std::uint32_t ban_seconds{7200};
    std::uint32_t connection_rate_per_minute{60};
    std::uint32_t connection_burst{20};
    std::uint32_t request_rate_per_second{50};
    std::uint32_t request_burst{100};
    std::uint32_t submit_rate_per_second{20};
    std::uint32_t submit_burst{40};
    std::uint32_t malformed_limit{10};
    std::uint32_t auth_failure_limit{10};
    std::uint32_t unknown_job_limit{20};
    std::uint32_t duplicate_limit{20};
    std::uint32_t abuse_window_seconds{60};
    // Retained from the strict configuration contract. Sustained-rate
    // attribution is deliberately separate from one-shot token-bucket denial.
    std::uint32_t hammer_rate_multiplier{4};
    std::uint32_t hammer_sustain_seconds{5};
    std::uint32_t candidate_rate_per_minute{12};
    std::uint32_t candidate_burst{3};
    std::uint32_t candidate_inflight_per_ip{2};
    std::uint32_t candidate_global_inflight{64};
    std::uint32_t false_candidate_limit{3};
    std::uint32_t false_candidate_window_seconds{600};
    std::uint32_t trusted_candidate_rejection_limit{3};
    std::uint32_t trusted_candidate_rejection_window_seconds{600};
    std::uint32_t verification_mismatch_limit{10};
    std::uint32_t verification_mismatch_window_seconds{600};
    // Internal hard bounds. They are deliberately not part of the public JSON
    // contract, but are adjustable in focused tests.
    std::size_t max_peer_states{65536};
    std::size_t max_active_bans{65536};
};

struct BanRecord {
    PeerAddress peer;
    AbuseKind reason{};
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point expires;
    std::chrono::system_clock::time_point evidence_start;
    std::chrono::system_clock::time_point evidence_end;
};

class DefenseEngine final {
public:
    using BanCallback = std::function<void(const BanRecord &)>;

    explicit DefenseEngine(DefensePolicyConfig config, BanCallback on_ban = {});
    ~DefenseEngine();

    [[nodiscard]] bool admit_connection(const PeerAddress &peer);
    [[nodiscard]] bool admit_request(const PeerAddress &peer);
    [[nodiscard]] bool admit_submit(const PeerAddress &peer);
    [[nodiscard]] bool admit_candidate(const PeerAddress &peer);
    void candidate_finished(const PeerAddress &peer) noexcept;
    void record(const PeerAddress &peer, AbuseKind kind);
    [[nodiscard]] bool banned(const PeerAddress &peer);
    void restore_ban(const BanRecord &record);
    void expire();

    [[nodiscard]] std::size_t candidate_global_inflight() const;
    [[nodiscard]] std::size_t candidate_peer_inflight(const PeerAddress &peer) const;
    [[nodiscard]] std::size_t peer_state_count() const;
    [[nodiscard]] std::size_t active_ban_count() const;

private:
    struct TokenBucket;
    struct HammerState;
    struct PeerState;
    [[nodiscard]] bool consume(TokenBucket &bucket, double rate_per_second,
                               double burst,
                               std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::pair<std::uint32_t, std::chrono::seconds>
        threshold(AbuseKind kind) const;
    [[nodiscard]] std::optional<BanRecord> observe_hammer_locked(
        const PeerAddress &peer, HammerState &hammer, double rate_per_second,
        std::chrono::steady_clock::time_point steady_now,
        std::chrono::system_clock::time_point wall_now,
        bool count_current_attempt = true);
    void expire_locked(std::chrono::system_clock::time_point now);
    [[nodiscard]] PeerState *find_or_create_peer_locked(
        const PeerAddress &peer, std::chrono::steady_clock::time_point now);
    void touch_peer_locked(PeerState &state,
                           std::chrono::steady_clock::time_point now);
    void erase_peer_locked(std::map<PeerAddress, PeerState>::iterator peer);
    [[nodiscard]] bool insert_ban_locked(const BanRecord &record);
    void erase_ban_locked(std::map<PeerAddress, BanRecord>::iterator ban);

    DefensePolicyConfig config_;
    BanCallback on_ban_;
    mutable std::mutex mutex_;
    std::map<PeerAddress, PeerState> peers_;
    std::list<PeerAddress> peer_lru_;
    std::map<PeerAddress, BanRecord> bans_;
    std::map<std::chrono::system_clock::time_point, std::set<PeerAddress>>
        ban_expiries_;
    std::optional<std::chrono::system_clock::time_point> ban_overflow_until_;
    std::size_t candidate_global_inflight_{};
};

} // namespace monero_solo
