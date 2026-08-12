#include "monero_solo/defense.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace monero_solo {

PeerAddress PeerAddress::from_socket(const sockaddr_storage &storage) {
    PeerAddress result;
    if (storage.ss_family == AF_INET) {
        const auto *address = reinterpret_cast<const sockaddr_in *>(&storage);
        result.family = AF_INET;
        result.size = 4U;
        std::memcpy(result.bytes.data(), &address->sin_addr, result.size);
        return result;
    }
    if (storage.ss_family == AF_INET6) {
        const auto *address = reinterpret_cast<const sockaddr_in6 *>(&storage);
        if (IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)) {
            result.family = AF_INET;
            result.size = 4U;
            std::memcpy(result.bytes.data(), &address->sin6_addr.s6_addr[12], result.size);
        }
        else {
            result.family = AF_INET6;
            result.size = 16U;
            std::memcpy(result.bytes.data(), &address->sin6_addr, result.size);
        }
        return result;
    }
    throw std::invalid_argument("unsupported socket address family");
}

PeerAddress PeerAddress::parse(std::string_view text_value) {
    PeerAddress result;
    const std::string text(text_value);
    if (inet_pton(AF_INET, text.c_str(), result.bytes.data()) == 1) {
        result.family = AF_INET;
        result.size = 4U;
        return result;
    }
    if (inet_pton(AF_INET6, text.c_str(), result.bytes.data()) == 1) {
        in6_addr address{};
        std::memcpy(&address, result.bytes.data(), sizeof(address));
        if (IN6_IS_ADDR_V4MAPPED(&address)) {
            std::memmove(result.bytes.data(), result.bytes.data() + 12U, 4U);
            std::fill(result.bytes.begin() + 4, result.bytes.end(), 0U);
            result.family = AF_INET;
            result.size = 4U;
        }
        else {
            result.family = AF_INET6;
            result.size = 16U;
        }
        return result;
    }
    throw std::invalid_argument("invalid IP address");
}

std::string PeerAddress::text() const {
    std::array<char, INET6_ADDRSTRLEN> output{};
    if ((family != AF_INET && family != AF_INET6) ||
        inet_ntop(family, bytes.data(), output.data(), output.size()) == nullptr) {
        throw std::runtime_error("invalid normalized peer address");
    }
    return output.data();
}

std::string PeerAddress::key() const {
    std::string result;
    result.reserve(size + 1U);
    result.push_back(family == AF_INET ? '\x04' : '\x06');
    result.append(reinterpret_cast<const char *>(bytes.data()), size);
    return result;
}

struct DefenseEngine::TokenBucket {
    double tokens{};
    std::chrono::steady_clock::time_point updated{std::chrono::steady_clock::now()};
    bool initialized{};
};

struct DefenseEngine::HammerState {
    std::uint64_t bucket{};
    std::uint64_t attempts{};
    std::uint32_t consecutive_seconds{};
    std::chrono::system_clock::time_point evidence_start{};
    bool initialized{};
};

struct DefenseEngine::PeerState {
    TokenBucket connections;
    TokenBucket requests;
    TokenBucket submits;
    TokenBucket candidates;
    HammerState connection_hammer;
    HammerState request_hammer;
    HammerState submit_hammer;
    HammerState candidate_hammer;
    std::size_t candidate_inflight{};
    std::map<AbuseKind, std::deque<std::chrono::system_clock::time_point>> evidence;
    std::chrono::steady_clock::time_point last_activity{
        std::chrono::steady_clock::now()};
    std::list<PeerAddress>::iterator lru_position;
};

namespace {
constexpr auto kPeerStateTtl = std::chrono::hours(24);
}

DefenseEngine::DefenseEngine(DefensePolicyConfig config, BanCallback on_ban)
    : config_(std::move(config)), on_ban_(std::move(on_ban)) {
    if (config_.enabled &&
        (config_.hammer_rate_multiplier < 2U ||
         config_.hammer_sustain_seconds == 0U ||
         config_.max_peer_states == 0U ||
         config_.max_active_bans == 0U)) {
        throw std::invalid_argument("invalid sustained-hammer configuration");
    }
}

DefenseEngine::~DefenseEngine() = default;

void DefenseEngine::touch_peer_locked(
    PeerState &state, const std::chrono::steady_clock::time_point now) {
    state.last_activity = now;
    peer_lru_.splice(peer_lru_.end(), peer_lru_, state.lru_position);
}

void DefenseEngine::erase_peer_locked(
    const std::map<PeerAddress, PeerState>::iterator peer) {
    peer_lru_.erase(peer->second.lru_position);
    peers_.erase(peer);
}

DefenseEngine::PeerState *DefenseEngine::find_or_create_peer_locked(
    const PeerAddress &peer, const std::chrono::steady_clock::time_point now) {
    if (auto found = peers_.find(peer); found != peers_.end()) {
        if (found->second.candidate_inflight == 0U &&
            found->second.last_activity < now - kPeerStateTtl) {
            erase_peer_locked(found);
        }
        else {
            touch_peer_locked(found->second, now);
            return &found->second;
        }
    }

    const auto cutoff = now - kPeerStateTtl;
    while (!peer_lru_.empty()) {
        const auto oldest = peers_.find(peer_lru_.front());
        if (oldest == peers_.end()) {
            peer_lru_.pop_front();
            continue;
        }
        if (oldest->second.candidate_inflight != 0U ||
            oldest->second.last_activity >= cutoff) {
            break;
        }
        erase_peer_locked(oldest);
    }

    std::size_t protected_examined = 0U;
    while (peers_.size() >= config_.max_peer_states &&
           protected_examined < peer_lru_.size()) {
        const auto victim = peers_.find(peer_lru_.front());
        if (victim == peers_.end()) {
            peer_lru_.pop_front();
            continue;
        }
        if (victim->second.candidate_inflight == 0U) {
            erase_peer_locked(victim);
            break;
        }
        peer_lru_.splice(peer_lru_.end(), peer_lru_,
                         victim->second.lru_position);
        ++protected_examined;
    }
    if (peers_.size() >= config_.max_peer_states) return nullptr;

    const auto [inserted, created] = peers_.try_emplace(peer);
    if (!created) {
        touch_peer_locked(inserted->second, now);
        return &inserted->second;
    }
    try {
        peer_lru_.push_back(peer);
    }
    catch (...) {
        peers_.erase(inserted);
        throw;
    }
    inserted->second.lru_position = std::prev(peer_lru_.end());
    inserted->second.last_activity = now;
    return &inserted->second;
}

bool DefenseEngine::consume(TokenBucket &bucket, double rate_per_second,
                            double burst,
                            std::chrono::steady_clock::time_point now) {
    if (!bucket.initialized) {
        bucket.initialized = true;
        bucket.tokens = burst;
        bucket.updated = now;
    }
    const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
    bucket.tokens = std::min(burst, bucket.tokens + elapsed * rate_per_second);
    bucket.updated = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

std::optional<BanRecord> DefenseEngine::observe_hammer_locked(
    const PeerAddress &peer, HammerState &hammer, double rate_per_second,
    std::chrono::steady_clock::time_point steady_now,
    std::chrono::system_clock::time_point wall_now,
    bool count_current_attempt) {
    const auto raw_bucket = std::chrono::duration_cast<std::chrono::seconds>(
                                steady_now.time_since_epoch()).count();
    const auto bucket = static_cast<std::uint64_t>(std::max<std::int64_t>(0, raw_bucket));
    if (!hammer.initialized) {
        hammer.initialized = true;
        hammer.bucket = bucket;
        hammer.attempts = count_current_attempt ? 1U : 0U;
        return std::nullopt;
    }
    if (bucket == hammer.bucket) {
        if (count_current_attempt &&
            hammer.attempts != std::numeric_limits<std::uint64_t>::max()) {
            ++hammer.attempts;
        }
        return std::nullopt;
    }

    const double scaled = rate_per_second *
                          static_cast<double>(config_.hammer_rate_multiplier);
    const auto threshold_count = static_cast<std::uint64_t>(
        std::max(1.0, std::ceil(scaled)));
    const bool adjacent = bucket == hammer.bucket + 1U;
    const bool sustained = adjacent && hammer.attempts >= threshold_count;
    if (sustained) {
        if (hammer.consecutive_seconds == 0U) {
            hammer.evidence_start = wall_now - std::chrono::seconds(1);
        }
        ++hammer.consecutive_seconds;
    }
    else {
        hammer.consecutive_seconds = 0U;
        hammer.evidence_start = {};
    }
    hammer.bucket = bucket;
    hammer.attempts = count_current_attempt ? 1U : 0U;
    if (hammer.consecutive_seconds < config_.hammer_sustain_seconds ||
        bans_.contains(peer)) {
        return std::nullopt;
    }
    BanRecord record{peer, AbuseKind::rate_hammer, wall_now,
                     wall_now + std::chrono::seconds(config_.ban_seconds),
                     hammer.evidence_start, wall_now};
    return insert_ban_locked(record) ? std::optional<BanRecord>(record)
                                     : std::nullopt;
}

bool DefenseEngine::banned(const PeerAddress &peer) {
    if (!config_.enabled) {
        return false;
    }
    std::lock_guard lock(mutex_);
    expire_locked(std::chrono::system_clock::now());
    return bans_.contains(peer) || ban_overflow_until_.has_value();
}

bool DefenseEngine::admit_connection(const PeerAddress &peer) {
    if (!config_.enabled) {
        return true;
    }
    std::optional<BanRecord> created;
    bool allowed{};
    {
        std::lock_guard lock(mutex_);
        const auto wall_now = std::chrono::system_clock::now();
        const auto steady_now = std::chrono::steady_clock::now();
        expire_locked(wall_now);
        if (bans_.contains(peer) || ban_overflow_until_.has_value()) return false;
        PeerState *const state = find_or_create_peer_locked(peer, steady_now);
        if (state == nullptr) return false;
        const double rate = static_cast<double>(config_.connection_rate_per_minute) / 60.0;
        created = observe_hammer_locked(peer, state->connection_hammer, rate,
                                        steady_now, wall_now);
        allowed = !created && consume(state->connections, rate,
                                      static_cast<double>(config_.connection_burst),
                                      steady_now);
    }
    if (created && on_ban_) on_ban_(*created);
    return allowed;
}

bool DefenseEngine::admit_request(const PeerAddress &peer) {
    if (!config_.enabled) {
        return true;
    }
    std::optional<BanRecord> created;
    bool allowed{};
    {
        std::lock_guard lock(mutex_);
        const auto wall_now = std::chrono::system_clock::now();
        const auto steady_now = std::chrono::steady_clock::now();
        expire_locked(wall_now);
        if (bans_.contains(peer) || ban_overflow_until_.has_value()) return false;
        PeerState *const state = find_or_create_peer_locked(peer, steady_now);
        if (state == nullptr) return false;
        const double rate = static_cast<double>(config_.request_rate_per_second);
        created = observe_hammer_locked(peer, state->request_hammer, rate,
                                        steady_now, wall_now);
        allowed = !created && consume(state->requests, rate,
                                      static_cast<double>(config_.request_burst),
                                      steady_now);
    }
    if (created && on_ban_) on_ban_(*created);
    return allowed;
}

bool DefenseEngine::admit_submit(const PeerAddress &peer) {
    if (!config_.enabled) {
        return true;
    }
    std::optional<BanRecord> created;
    bool allowed{};
    {
        std::lock_guard lock(mutex_);
        const auto wall_now = std::chrono::system_clock::now();
        const auto steady_now = std::chrono::steady_clock::now();
        expire_locked(wall_now);
        if (bans_.contains(peer) || ban_overflow_until_.has_value()) return false;
        PeerState *const state = find_or_create_peer_locked(peer, steady_now);
        if (state == nullptr) return false;
        const double rate = static_cast<double>(config_.submit_rate_per_second);
        created = observe_hammer_locked(peer, state->submit_hammer, rate,
                                        steady_now, wall_now);
        allowed = !created && consume(state->submits, rate,
                                      static_cast<double>(config_.submit_burst),
                                      steady_now);
    }
    if (created && on_ban_) on_ban_(*created);
    return allowed;
}

bool DefenseEngine::admit_candidate(const PeerAddress &peer) {
    if (!config_.enabled) {
        return true;
    }
    std::optional<BanRecord> created;
    bool allowed{};
    {
        std::lock_guard lock(mutex_);
        const auto wall_now = std::chrono::system_clock::now();
        const auto steady_now = std::chrono::steady_clock::now();
        expire_locked(wall_now);
        if (bans_.contains(peer) || ban_overflow_until_.has_value()) return false;
        PeerState *const state = find_or_create_peer_locked(peer, steady_now);
        if (state == nullptr) return false;
        const double rate = static_cast<double>(config_.candidate_rate_per_minute) / 60.0;
        if (config_.candidate_rate_per_minute != 0U) {
            created = observe_hammer_locked(peer, state->candidate_hammer, rate,
                                            steady_now, wall_now, false);
        }
        const bool capped =
            (config_.candidate_global_inflight != 0U &&
             candidate_global_inflight_ >= config_.candidate_global_inflight) ||
            (config_.candidate_inflight_per_ip != 0U &&
             state->candidate_inflight >= config_.candidate_inflight_per_ip);
        const bool rate_allowed = config_.candidate_rate_per_minute == 0U ||
            consume(state->candidates, rate,
                    static_cast<double>(config_.candidate_burst), steady_now);
        if (!created && (capped || !rate_allowed) &&
            config_.candidate_rate_per_minute != 0U) {
            // Candidate hammering measures evasion attempts after a rate or
            // in-flight denial, not legitimate admitted candidate traffic.
            created = observe_hammer_locked(peer, state->candidate_hammer, rate,
                                            steady_now, wall_now, true);
        }
        allowed = !created && !capped && rate_allowed;
        if (allowed) {
            ++state->candidate_inflight;
            ++candidate_global_inflight_;
        }
    }
    if (created && on_ban_) on_ban_(*created);
    return allowed;
}

void DefenseEngine::candidate_finished(const PeerAddress &peer) noexcept {
    if (!config_.enabled) {
        return;
    }
    std::lock_guard lock(mutex_);
    const auto iterator = peers_.find(peer);
    if (iterator != peers_.end() && iterator->second.candidate_inflight != 0U) {
        touch_peer_locked(iterator->second, std::chrono::steady_clock::now());
        --iterator->second.candidate_inflight;
        if (candidate_global_inflight_ != 0U) {
            --candidate_global_inflight_;
        }
    }
}

std::pair<std::uint32_t, std::chrono::seconds>
DefenseEngine::threshold(AbuseKind kind) const {
    switch (kind) {
    case AbuseKind::malformed:
        return {config_.malformed_limit, std::chrono::seconds(config_.abuse_window_seconds)};
    case AbuseKind::authentication_failure:
        return {config_.auth_failure_limit, std::chrono::seconds(config_.abuse_window_seconds)};
    case AbuseKind::unknown_job:
        return {config_.unknown_job_limit, std::chrono::seconds(config_.abuse_window_seconds)};
    case AbuseKind::duplicate:
        return {config_.duplicate_limit, std::chrono::seconds(config_.abuse_window_seconds)};
    case AbuseKind::oversized_line:
        return {2U, std::chrono::seconds(config_.abuse_window_seconds)};
    case AbuseKind::verified_false_candidate:
        return {config_.false_candidate_limit,
                std::chrono::seconds(config_.false_candidate_window_seconds)};
    case AbuseKind::candidate_mismatch:
        return {config_.verification_mismatch_limit,
                std::chrono::seconds(config_.verification_mismatch_window_seconds)};
    case AbuseKind::trusted_candidate_rejection:
        return {config_.trusted_candidate_rejection_limit,
                std::chrono::seconds(config_.trusted_candidate_rejection_window_seconds)};
    case AbuseKind::rate_hammer:
        return {std::numeric_limits<std::uint32_t>::max(),
                std::chrono::seconds(config_.hammer_sustain_seconds)};
    }
    return {std::numeric_limits<std::uint32_t>::max(), std::chrono::seconds(1)};
}

void DefenseEngine::record(const PeerAddress &peer, AbuseKind kind) {
    if (!config_.enabled) {
        return;
    }
    std::optional<BanRecord> created;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::system_clock::now();
        expire_locked(now);
        if (bans_.contains(peer) || ban_overflow_until_.has_value()) {
            return;
        }
        PeerState *const state = find_or_create_peer_locked(
            peer, std::chrono::steady_clock::now());
        if (state == nullptr) return;
        auto &events = state->evidence[kind];
        const auto [limit, window] = threshold(kind);
        const auto cutoff = now - window;
        while (!events.empty() && events.front() < cutoff) {
            events.pop_front();
        }
        events.push_back(now);
        if (events.size() >= limit) {
            BanRecord record{peer, kind, now,
                             now + std::chrono::seconds(config_.ban_seconds),
                             events.front(), events.back()};
            if (insert_ban_locked(record)) created = record;
        }
    }
    if (created.has_value() && on_ban_) {
        on_ban_(*created);
    }
}

void DefenseEngine::restore_ban(const BanRecord &record) {
    if (!config_.enabled || record.expires <= std::chrono::system_clock::now()) {
        return;
    }
    std::lock_guard lock(mutex_);
    (void)insert_ban_locked(record);
}

void DefenseEngine::erase_ban_locked(
    const std::map<PeerAddress, BanRecord>::iterator ban) {
    const auto expiry = ban_expiries_.find(ban->second.expires);
    if (expiry != ban_expiries_.end()) {
        expiry->second.erase(ban->first);
        if (expiry->second.empty()) ban_expiries_.erase(expiry);
    }
    bans_.erase(ban);
}

bool DefenseEngine::insert_ban_locked(const BanRecord &record) {
    if (const auto existing = bans_.find(record.peer); existing != bans_.end()) {
        erase_ban_locked(existing);
    }
    // Preserve exact active bans up to the hard bound. Overflow never evicts
    // an authoritative ban: it enters a bounded fail-closed interval until
    // the would-be ban would have expired. Durable persistence still receives
    // the record through the caller's callback.
    if (bans_.size() >= config_.max_active_bans) {
        if (!ban_overflow_until_.has_value() ||
            *ban_overflow_until_ < record.expires) {
            ban_overflow_until_ = record.expires;
        }
        return true;
    }
    const auto [inserted, created] = bans_.emplace(record.peer, record);
    if (created) {
        try {
            ban_expiries_[record.expires].insert(record.peer);
        }
        catch (...) {
            bans_.erase(inserted);
            if (const auto empty = ban_expiries_.find(record.expires);
                empty != ban_expiries_.end() && empty->second.empty()) {
                ban_expiries_.erase(empty);
            }
            throw;
        }
    }
    return created;
}

void DefenseEngine::expire_locked(std::chrono::system_clock::time_point now) {
    if (ban_overflow_until_.has_value() && *ban_overflow_until_ <= now) {
        ban_overflow_until_.reset();
    }
    while (!ban_expiries_.empty() && ban_expiries_.begin()->first <= now) {
        auto expired = ban_expiries_.begin();
        for (const PeerAddress &peer : expired->second) {
            bans_.erase(peer);
        }
        ban_expiries_.erase(expired);
    }
}

void DefenseEngine::expire() {
    std::lock_guard lock(mutex_);
    expire_locked(std::chrono::system_clock::now());
}

std::size_t DefenseEngine::candidate_global_inflight() const {
    std::lock_guard lock(mutex_);
    return candidate_global_inflight_;
}

std::size_t DefenseEngine::candidate_peer_inflight(const PeerAddress &peer) const {
    std::lock_guard lock(mutex_);
    const auto iterator = peers_.find(peer);
    return iterator == peers_.end() ? 0U : iterator->second.candidate_inflight;
}

std::size_t DefenseEngine::peer_state_count() const {
    std::lock_guard lock(mutex_);
    return peers_.size();
}

std::size_t DefenseEngine::active_ban_count() const {
    std::lock_guard lock(mutex_);
    return bans_.size();
}

} // namespace monero_solo
