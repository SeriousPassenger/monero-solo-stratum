#include "monero_solo/entropy.hpp"

#include "monero_solo/util.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unistd.h>

namespace monero_solo {
namespace {

constexpr std::string_view kInstantiateDomain =
    "monero-solo-stratum/HMAC-DRBG-SHA256/v1";
constexpr std::string_view kTimeReason = "reseed/time/v1";
constexpr std::string_view kCountReason = "reseed/count/v1";
constexpr std::string_view kForkReason = "reseed/fork/v1";

[[nodiscard]] Bytes combine(std::initializer_list<std::span<const std::uint8_t>> parts)
{
    std::size_t size = 0;
    for (const auto part : parts) {
        if (part.size() > std::numeric_limits<std::size_t>::max() - size) {
            throw EntropyError("HMAC-DRBG input is too large");
        }
        size += part.size();
    }
    Bytes result;
    result.reserve(size);
    for (const auto part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

[[nodiscard]] std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

void cleanse(Bytes &bytes) noexcept
{
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
}

} // namespace

EntropyManager::EntropyManager(EntropyConfig config,
                               EntropySource entropy_source,
                               NowSource now_source,
                               PidSource pid_source)
    : config_(config),
      entropy_source_(entropy_source ? std::move(entropy_source)
                                     : EntropySource{os_random_exact}),
      now_source_(now_source ? std::move(now_source)
                             : NowSource{[] { return Clock::now(); }}),
      pid_source_(pid_source ? std::move(pid_source)
                             : PidSource{[] { return getpid(); }})
{
    if (config_.reseed_interval_seconds == 0 ||
        config_.max_reseed_age_seconds < config_.reseed_interval_seconds ||
        config_.max_generate_calls == 0) {
        throw ValidationError("invalid entropy configuration");
    }
    value_.fill(0x01);
    std::array<std::uint8_t, 32> sample{};
    entropy_source_(sample, false);
    Bytes seed = combine({sample, bytes_of(kInstantiateDomain)});
    update_locked(seed);
    OPENSSL_cleanse(sample.data(), sample.size());
    cleanse(seed);
    last_successful_reseed_ = now();
    next_timed_reseed_retry_ =
        last_successful_reseed_ + std::chrono::seconds(config_.reseed_interval_seconds);
    creator_pid_ = pid();
}

EntropyManager::~EntropyManager()
{
    std::lock_guard lock(mutex_);
    OPENSSL_cleanse(key_.data(), key_.size());
    OPENSSL_cleanse(value_.data(), value_.size());
}

void EntropyManager::update_locked(std::span<const std::uint8_t> data)
{
    const std::uint8_t zero = 0;
    Bytes input = combine({value_, std::span{&zero, 1}, data});
    key_ = hmac_sha256(key_, input);
    value_ = hmac_sha256(key_, value_);
    cleanse(input);
    if (!data.empty()) {
        const std::uint8_t one = 1;
        input = combine({value_, std::span{&one, 1}, data});
        key_ = hmac_sha256(key_, input);
        value_ = hmac_sha256(key_, value_);
        cleanse(input);
    }
}

void EntropyManager::reseed_locked(std::array<std::uint8_t, 32> &sample,
                                   std::string_view reason)
{
    Bytes material = combine({sample, bytes_of(reason)});
    update_locked(material);
    OPENSSL_cleanse(sample.data(), sample.size());
    cleanse(material);
    last_successful_reseed_ = now();
    generate_calls_ = 0;
    creator_pid_ = pid();
    timed_retry_delay_seconds_ = 1;
    next_timed_reseed_retry_ =
        last_successful_reseed_ + std::chrono::seconds(config_.reseed_interval_seconds);
    degraded_ = false;
}

void EntropyManager::mandatory_reseed_locked(std::string_view reason)
{
    std::array<std::uint8_t, 32> sample{};
    try {
        entropy_source_(sample, false);
    } catch (...) {
        OPENSSL_cleanse(sample.data(), sample.size());
        throw;
    }
    reseed_locked(sample, reason);
}

void EntropyManager::try_timed_reseed_locked(Clock::time_point current)
{
    std::array<std::uint8_t, 32> sample{};
    try {
        entropy_source_(sample, true);
        reseed_locked(sample, kTimeReason);
    } catch (...) {
        OPENSSL_cleanse(sample.data(), sample.size());
        degraded_ = true;
        next_timed_reseed_retry_ =
            current + std::chrono::seconds(timed_retry_delay_seconds_);
        timed_retry_delay_seconds_ =
            std::min<std::uint32_t>(timed_retry_delay_seconds_ * 2, 60);
    }
}

Bytes EntropyManager::generate(std::size_t bytes,
                               std::string_view additional_domain)
{
    if (additional_domain.empty()) {
        throw ValidationError("HMAC-DRBG additional domain must be nonempty");
    }
    std::lock_guard lock(mutex_);
    if (pid() != creator_pid_) mandatory_reseed_locked(kForkReason);
    if (generate_calls_ >= config_.max_generate_calls) {
        mandatory_reseed_locked(kCountReason);
    }
    Clock::time_point current = now();
    const auto interval = std::chrono::seconds(config_.reseed_interval_seconds);
    if (current - last_successful_reseed_ >= interval &&
        current >= next_timed_reseed_retry_) {
        try_timed_reseed_locked(current);
        current = now();
    }
    if (current - last_successful_reseed_ >=
        std::chrono::seconds(config_.max_reseed_age_seconds)) {
        throw EntropyError("HMAC-DRBG maximum reseed age exceeded");
    }

    const auto domain = bytes_of(additional_domain);
    update_locked(domain);
    Bytes output;
    output.reserve(bytes);
    while (output.size() < bytes) {
        value_ = hmac_sha256(key_, value_);
        const std::size_t take = std::min(value_.size(), bytes - output.size());
        output.insert(output.end(), value_.begin(), value_.begin() + take);
    }
    update_locked(domain);
    ++generate_calls_;
    return output;
}

Id16 EntropyManager::generate_id(std::string_view additional_domain)
{
    Bytes data = generate(16, additional_domain);
    Id16 result{};
    std::copy(data.begin(), data.end(), result.begin());
    cleanse(data);
    return result;
}

Id16 EntropyManager::private_template_entropy()
{
    return generate_id("private-template-entropy/v1");
}

Id16 EntropyManager::private_job_id()
{
    return generate_id("private-job-id/v1");
}

bool EntropyManager::degraded() const
{
    std::lock_guard lock(mutex_);
    return degraded_;
}

bool EntropyManager::issuance_allowed() const
{
    std::lock_guard lock(mutex_);
    return now() - last_successful_reseed_ <
           std::chrono::seconds(config_.max_reseed_age_seconds);
}

std::uint64_t EntropyManager::generate_calls() const
{
    std::lock_guard lock(mutex_);
    return generate_calls_;
}

std::uint32_t EntropyManager::timed_retry_delay_seconds() const
{
    std::lock_guard lock(mutex_);
    return timed_retry_delay_seconds_;
}

EntropyManager::Clock::time_point EntropyManager::now() const
{
    return now_source_();
}

pid_t EntropyManager::pid() const
{
    return pid_source_();
}

} // namespace monero_solo
