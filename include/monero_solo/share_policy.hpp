#pragma once

namespace monero_solo::detail {

enum class DuplicateTerminal {
    none,
    duplicate,
    capacity,
};

struct VerifiedPostHashPlan {
    bool journal_computed_candidate{};
    DuplicateTerminal duplicate_terminal{DuplicateTerminal::none};
};

[[nodiscard]] constexpr bool continue_verification_after_claimed_capacity(
    bool claimed_capacity, bool claimed_network_candidate) noexcept
{
    return !claimed_capacity || claimed_network_candidate;
}

[[nodiscard]] constexpr VerifiedPostHashPlan verified_post_hash_plan(
    bool computed_network_candidate, bool computed_duplicate,
    bool claimed_capacity, bool computed_capacity) noexcept
{
    return {
        .journal_computed_candidate = computed_network_candidate,
        .duplicate_terminal = computed_duplicate
                                  ? DuplicateTerminal::duplicate
                                  : (claimed_capacity || computed_capacity)
                                        ? DuplicateTerminal::capacity
                                        : DuplicateTerminal::none,
    };
}

} // namespace monero_solo::detail
