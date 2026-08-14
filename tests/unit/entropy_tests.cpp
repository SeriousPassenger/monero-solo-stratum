#include "monero_solo/entropy.hpp"

#include "monero_solo/util.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void test_exact_construction_vector()
{
    unsigned reads = 0;
    const auto source = [&](std::span<std::uint8_t> output, bool) {
        require(output.size() == 32, "entropy request was not exactly 32 bytes");
        require(reads++ == 0, "unexpected reseed");
        for (std::size_t i = 0; i < output.size(); ++i) {
            output[i] = static_cast<std::uint8_t>(i);
        }
    };
    monero_solo::EntropyManager manager({}, source);
    require(monero_solo::hex_encode(manager.private_template_entropy()) ==
                "784a179fed4416542605a5ddb6d46f9d",
            "private entropy vector mismatch");
    require(monero_solo::hex_encode(manager.private_job_id()) ==
                "ab151a8861091de24d34e8b4221c4fe3",
            "job ID vector mismatch");
    require(manager.generate_calls() == 2, "generate counter mismatch");
}

void test_count_reseed_and_failure_atomicity()
{
    unsigned reads = 0;
    const auto source = [&](std::span<std::uint8_t> output, bool) {
        ++reads;
        std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(reads));
    };
    monero_solo::EntropyConfig config;
    config.max_generate_calls = 1;
    monero_solo::EntropyManager manager(config, source);
    (void)manager.private_job_id();
    (void)manager.private_job_id();
    require(reads == 2, "count reseed did not occur before output");

    unsigned failing_reads = 0;
    const auto failing = [&](std::span<std::uint8_t> output, bool) {
        if (failing_reads++ == 0) std::fill(output.begin(), output.end(), 7);
        else throw monero_solo::EntropyError("injected entropy failure");
    };
    monero_solo::EntropyManager failing_manager(config, failing);
    (void)failing_manager.private_job_id();
    try {
        (void)failing_manager.private_job_id();
        throw std::runtime_error("mandatory reseed failure returned output");
    } catch (const monero_solo::EntropyError &) {
    }
    require(failing_manager.generate_calls() == 1,
            "failed mandatory reseed changed generate count");
}

void test_timed_backoff()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    unsigned calls = 0;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        if (calls++ == 0) {
            std::fill(output.begin(), output.end(), 9);
            return;
        }
        require(nonblocking, "timed reseed was not bounded/nonblocking");
        throw monero_solo::EntropyError("injected timed failure");
    };
    monero_solo::EntropyConfig config;
    config.reseed_interval_seconds = 2;
    config.max_reseed_age_seconds = 20;
    monero_solo::EntropyManager manager(config, source, [&] { return now; });
    now += std::chrono::seconds(2);
    (void)manager.private_job_id();
    require(manager.degraded(), "timed failure did not degrade health");
    require(manager.timed_retry_delay_seconds() == 2, "retry did not double");
    (void)manager.private_job_id();
    require(calls == 2, "call before retry boundary touched entropy source");
    now += std::chrono::seconds(1);
    (void)manager.private_job_id();
    require(calls == 3, "retry boundary did not touch entropy source");
}

void test_default_timed_reseed_boundary()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    unsigned reads = 0;
    std::vector<bool> nonblocking_reads;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        require(output.size() == 32, "default reseed did not request 256 bits");
        ++reads;
        nonblocking_reads.push_back(nonblocking);
        std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(reads));
    };
    const monero_solo::EntropyConfig config;
    require(config.reseed_interval_seconds == 1200 &&
                config.max_reseed_age_seconds == 1260,
            "default entropy timing differs");
    monero_solo::EntropyManager manager(config, source, [&] { return now; });
    require(reads == 1 && !nonblocking_reads.front(),
            "startup entropy read was not one blocking 256-bit request");
    now += std::chrono::seconds(1199);
    (void)manager.private_template_entropy();
    require(reads == 1, "default timed reseed occurred before 20 minutes");
    now += std::chrono::seconds(1);
    (void)manager.private_job_id();
    require(reads == 2 && nonblocking_reads.back(),
            "default timed reseed did not occur at 20 minutes");
}

void test_default_max_reseed_age_fails_closed()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    unsigned reads = 0;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        require(output.size() == 32, "default reseed did not request 256 bits");
        if (reads++ == 0) {
            require(!nonblocking, "startup entropy read was nonblocking");
            std::fill(output.begin(), output.end(), 11);
            return;
        }
        require(nonblocking, "timed reseed failure was not bounded/nonblocking");
        throw monero_solo::EntropyError("injected timed failure");
    };
    const monero_solo::EntropyConfig config;
    monero_solo::EntropyManager manager(config, source, [&] { return now; });

    now += std::chrono::seconds(1200);
    (void)manager.private_job_id();
    require(manager.degraded(), "failed default timed reseed did not degrade health");
    require(manager.issuance_allowed(),
            "issuance stopped before the default maximum reseed age");

    now += std::chrono::seconds(59);
    (void)manager.private_template_entropy();
    require(manager.issuance_allowed(),
            "issuance stopped one second before the maximum reseed age");
    const auto generate_calls = manager.generate_calls();

    now += std::chrono::seconds(1);
    require(!manager.issuance_allowed(),
            "issuance remained allowed at the maximum reseed age");
    try {
        (void)manager.private_job_id();
        throw std::runtime_error("maximum reseed age returned output");
    } catch (const monero_solo::EntropyError &) {
    }
    require(manager.generate_calls() == generate_calls,
            "maximum reseed age failure changed generate count");
    require(reads == 3,
            "maximum reseed age bypassed the timed reseed retry schedule");
}

} // namespace

int main()
{
    try {
        test_exact_construction_vector();
        test_count_reseed_and_failure_atomicity();
        test_timed_backoff();
        test_default_timed_reseed_boundary();
        test_default_max_reseed_age_fails_closed();
        std::cout << "entropy tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "entropy tests failed: " << error.what() << '\n';
        return 1;
    }
}
