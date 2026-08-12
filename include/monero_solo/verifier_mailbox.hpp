#pragma once

#include "monero_solo/verifier.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace monero_solo::verifier {

// A bounded one-result mailbox. Waiter slots are registered before native
// submission, which closes the fast-completion race. Unknown, duplicate, and
// late completions are discarded instead of becoming unowned process state.
class CompletionMailbox final {
public:
    explicit CompletionMailbox(std::size_t capacity);

    CompletionMailbox(const CompletionMailbox &) = delete;
    CompletionMailbox &operator=(const CompletionMailbox &) = delete;

    void open();
    void close() noexcept;
    [[nodiscard]] bool register_waiter(std::uint64_t user_tag);
    void cancel(std::uint64_t user_tag) noexcept;
    [[nodiscard]] bool publish(Completion completion) noexcept;
    [[nodiscard]] std::optional<Completion> wait(
        std::uint64_t user_tag, std::chrono::milliseconds timeout);
    [[nodiscard]] std::optional<Completion> wait(std::uint64_t user_tag);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    std::size_t capacity_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::uint64_t, std::optional<Completion>> slots_;
    bool closed_{true};
};

} // namespace monero_solo::verifier
