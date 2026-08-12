#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace monero_solo {

struct BlockNotifyCommand {
    std::vector<std::string> arguments;

    [[nodiscard]] static BlockNotifyCommand parse(std::string_view command_template,
                                                  bool validate_executable = true);
    [[nodiscard]] std::vector<std::string> instantiate(std::string_view miner_tx_hash) const;
};

struct BlockNotifyDelivery {
    std::int64_t id{};
    std::int64_t candidate_id{};
    std::string miner_tx_hash;
    // The durable counter is incremented by the atomic database claim.
    std::uint32_t attempt_count{};
};

struct BlockNotifyResult {
    bool delivered{};
    bool timed_out{};
    std::optional<int> exit_code;
    std::optional<int> term_signal;
    std::string stderr_excerpt;
    std::string error;
};

class BlockNotifySupervisor final {
public:
    using Claim = std::function<std::optional<BlockNotifyDelivery>()>;
    using Complete = std::function<void(const BlockNotifyDelivery &,
                                        const BlockNotifyResult &,
                                        std::chrono::seconds)>;

    BlockNotifySupervisor(BlockNotifyCommand command, Claim claim, Complete complete);
    BlockNotifySupervisor(const BlockNotifySupervisor &) = delete;
    BlockNotifySupervisor &operator=(const BlockNotifySupervisor &) = delete;
    ~BlockNotifySupervisor();

    void start();
    void wake() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] static BlockNotifyResult execute(
        const BlockNotifyCommand &command,
        std::string_view miner_tx_hash,
        std::chrono::seconds timeout = std::chrono::seconds(60));
    [[nodiscard]] static std::chrono::seconds retry_delay(std::uint32_t attempt_count);

private:
    void loop(std::stop_token token) noexcept;

    BlockNotifyCommand command_;
    Claim claim_;
    Complete complete_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_requested_{false};
    std::jthread thread_;
};

} // namespace monero_solo
