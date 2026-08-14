#pragma once

#include "monero_solo/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string_view>
#include <sys/types.h>

namespace monero_solo {

struct EntropyConfig {
    std::uint32_t reseed_interval_seconds{1200};
    std::uint32_t max_reseed_age_seconds{1260};
    std::uint32_t max_generate_calls{1048576};
};

class EntropyManager final {
public:
    using Clock = std::chrono::steady_clock;
    using EntropySource = std::function<void(std::span<std::uint8_t>, bool)>;
    using NowSource = std::function<Clock::time_point()>;
    using PidSource = std::function<pid_t()>;

    explicit EntropyManager(EntropyConfig config = {},
                            EntropySource entropy_source = {},
                            NowSource now_source = {},
                            PidSource pid_source = {});
    ~EntropyManager();

    EntropyManager(const EntropyManager &) = delete;
    EntropyManager &operator=(const EntropyManager &) = delete;

    [[nodiscard]] Bytes generate(std::size_t bytes,
                                 std::string_view additional_domain);
    [[nodiscard]] Id16 generate_id(std::string_view additional_domain);
    [[nodiscard]] Id16 private_template_entropy();
    [[nodiscard]] Id16 private_job_id();

    [[nodiscard]] bool degraded() const;
    [[nodiscard]] bool issuance_allowed() const;
    [[nodiscard]] std::uint64_t generate_calls() const;
    [[nodiscard]] std::uint32_t timed_retry_delay_seconds() const;

private:
    void update_locked(std::span<const std::uint8_t> data);
    void reseed_locked(std::array<std::uint8_t, 32> &sample,
                       std::string_view reason);
    void mandatory_reseed_locked(std::string_view reason);
    void try_timed_reseed_locked(Clock::time_point now);
    [[nodiscard]] Clock::time_point now() const;
    [[nodiscard]] pid_t pid() const;

    EntropyConfig config_;
    EntropySource entropy_source_;
    NowSource now_source_;
    PidSource pid_source_;
    mutable std::mutex mutex_;
    std::array<std::uint8_t, 32> key_{};
    std::array<std::uint8_t, 32> value_{};
    Clock::time_point last_successful_reseed_{};
    Clock::time_point next_timed_reseed_retry_{};
    std::uint64_t generate_calls_{};
    std::uint32_t timed_retry_delay_seconds_{1};
    pid_t creator_pid_{};
    bool degraded_{};
};

} // namespace monero_solo
