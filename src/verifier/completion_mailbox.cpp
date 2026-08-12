#include "monero_solo/verifier_mailbox.hpp"

#include <stdexcept>
#include <utility>

namespace monero_solo::verifier {

CompletionMailbox::CompletionMailbox(const std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0U) {
        throw std::invalid_argument("completion mailbox capacity is zero");
    }
    slots_.reserve(capacity_);
}

void CompletionMailbox::open()
{
    std::lock_guard lock(mutex_);
    if (!slots_.empty()) {
        throw std::logic_error("completion mailbox opened with live slots");
    }
    closed_ = false;
}

void CompletionMailbox::close() noexcept
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        slots_.clear();
    }
    condition_.notify_all();
}

bool CompletionMailbox::register_waiter(const std::uint64_t user_tag)
{
    std::lock_guard lock(mutex_);
    if (closed_ || slots_.size() >= capacity_) return false;
    return slots_.try_emplace(user_tag, std::nullopt).second;
}

void CompletionMailbox::cancel(const std::uint64_t user_tag) noexcept
{
    {
        std::lock_guard lock(mutex_);
        slots_.erase(user_tag);
    }
    condition_.notify_all();
}

bool CompletionMailbox::publish(Completion completion) noexcept
{
    bool delivered = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = slots_.find(completion.user_tag);
        if (!closed_ && found != slots_.end() && !found->second.has_value()) {
            try {
                found->second.emplace(std::move(completion));
                delivered = true;
            }
            catch (...) {
                // Allocation failure must not escape the verifier drain loop.
                slots_.erase(found);
            }
        }
    }
    if (delivered) condition_.notify_all();
    return delivered;
}

std::optional<Completion> CompletionMailbox::wait(
    const std::uint64_t user_tag, const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const bool finished = condition_.wait_for(lock, timeout, [&] {
        const auto found = slots_.find(user_tag);
        return closed_ || found == slots_.end() || found->second.has_value();
    });
    const auto found = slots_.find(user_tag);
    if (!finished || found == slots_.end() || !found->second.has_value()) {
        if (found != slots_.end()) slots_.erase(found);
        return std::nullopt;
    }
    std::optional<Completion> result(std::move(*found->second));
    slots_.erase(found);
    return result;
}

std::optional<Completion> CompletionMailbox::wait(const std::uint64_t user_tag)
{
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] {
        const auto found = slots_.find(user_tag);
        return closed_ || found == slots_.end() || found->second.has_value();
    });
    const auto found = slots_.find(user_tag);
    if (found == slots_.end() || !found->second.has_value()) {
        if (found != slots_.end()) slots_.erase(found);
        return std::nullopt;
    }
    std::optional<Completion> result(std::move(*found->second));
    slots_.erase(found);
    return result;
}

std::size_t CompletionMailbox::size() const noexcept
{
    std::lock_guard lock(mutex_);
    return slots_.size();
}

bool CompletionMailbox::closed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

} // namespace monero_solo::verifier
