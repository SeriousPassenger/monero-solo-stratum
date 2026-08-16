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

void test_idle_maintenance_reseeds_without_output()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    unsigned reads = 0;
    std::vector<bool> nonblocking_reads;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        ++reads;
        nonblocking_reads.push_back(nonblocking);
        std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(reads));
    };
    monero_solo::EntropyConfig config;
    config.reseed_interval_seconds = 2;
    config.max_reseed_age_seconds = 3;
    monero_solo::EntropyManager manager(config, source, [&] { return now; });

    now += std::chrono::seconds(3);
    require(!manager.issuance_allowed(),
            "idle entropy remained available at the maximum age");
    const auto result = manager.maintain();
    require(result.attempted && result.succeeded && result.issuance_allowed &&
                !result.degraded &&
                result.reason == monero_solo::EntropyReseedReason::timed,
            "idle maintenance did not recover timed entropy");
    require(reads == 2 && !nonblocking_reads.front() &&
                nonblocking_reads.back(),
            "idle maintenance used the wrong entropy source mode");
    require(manager.generate_calls() == 0,
            "idle maintenance generated output or changed the call counter");
}

void test_idle_maintenance_retries_after_expiry()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    unsigned reads = 0;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        ++reads;
        if (reads == 1 || reads == 9) {
            require(nonblocking == (reads != 1),
                    "maintenance entropy source mode mismatch");
            std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(reads));
            return;
        }
        require(nonblocking, "timed maintenance retry was blocking");
        throw monero_solo::EntropyError("injected maintenance failure");
    };
    monero_solo::EntropyConfig config;
    config.reseed_interval_seconds = 2;
    config.max_reseed_age_seconds = 4;
    monero_solo::EntropyManager manager(config, source, [&] { return now; });

    const std::array<unsigned, 8> retry_deadlines{2, 3, 5, 9, 17, 33, 65, 125};
    const std::array<std::uint32_t, 7> retry_delays{2, 4, 8, 16, 32, 60, 60};
    for (std::size_t index = 0; index < retry_deadlines.size(); ++index) {
        if (index != 0) {
            now = Clock::time_point{} +
                  std::chrono::seconds(retry_deadlines[index] - 1U);
            const auto early = manager.maintain();
            require(!early.attempted,
                    "maintenance touched the source before its retry deadline");
        }
        now = Clock::time_point{} +
              std::chrono::seconds(retry_deadlines[index]);
        const auto result = manager.maintain();
        require(result.attempted &&
                    result.reason == monero_solo::EntropyReseedReason::timed,
                "maintenance skipped a retry deadline");
        if (index + 1U < retry_deadlines.size()) {
            require(!result.succeeded && result.degraded &&
                        manager.timed_retry_delay_seconds() ==
                            retry_delays[index],
                    "maintenance retry backoff mismatch");
            if (retry_deadlines[index] >= config.max_reseed_age_seconds) {
                require(!result.issuance_allowed,
                        "expired entropy remained available after retry failure");
            }
        }
        else {
            require(result.succeeded && result.issuance_allowed &&
                        !result.degraded &&
                        manager.timed_retry_delay_seconds() == 1,
                    "maintenance did not recover after maximum-age expiry");
        }
        require(manager.generate_calls() == 0,
                "maintenance retry generated DRBG output");
    }
    require(reads == 9, "maintenance retry touched the entropy source unexpectedly");
}

void test_maintenance_preserves_count_and_fork_semantics()
{
    using Clock = monero_solo::EntropyManager::Clock;
    Clock::time_point now{};
    pid_t process_id = 10;
    unsigned reads = 0;
    std::vector<bool> nonblocking_reads;
    const auto source = [&](std::span<std::uint8_t> output, bool nonblocking) {
        ++reads;
        nonblocking_reads.push_back(nonblocking);
        std::fill(output.begin(), output.end(), static_cast<std::uint8_t>(reads));
    };
    monero_solo::EntropyConfig config;
    config.reseed_interval_seconds = 100;
    config.max_reseed_age_seconds = 200;
    config.max_generate_calls = 1;
    monero_solo::EntropyManager manager(
        config, source, [&] { return now; }, [&] { return process_id; });
    (void)manager.private_job_id();
    require(manager.generate_calls() == 1, "count fixture did not generate output");
    require(!manager.maintain().attempted && manager.generate_calls() == 1 &&
                reads == 1,
            "maintenance consumed the count-reseed budget");
    (void)manager.private_job_id();
    require(reads == 2 && !nonblocking_reads.back(),
            "count reseed was not left to output generation");

    process_id = 11;
    now += std::chrono::seconds(100);
    const auto fork = manager.maintain();
    require(fork.attempted && fork.succeeded && fork.issuance_allowed &&
                fork.reason == monero_solo::EntropyReseedReason::fork &&
                reads == 3 && !nonblocking_reads.back(),
            "maintenance did not perform a mandatory fork reseed first");
    require(manager.generate_calls() == 0,
            "fork maintenance generated output");
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
        test_idle_maintenance_reseeds_without_output();
        test_idle_maintenance_retries_after_expiry();
        test_maintenance_preserves_count_and_fork_semantics();
        std::cout << "entropy tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "entropy tests failed: " << error.what() << '\n';
        return 1;
    }
}
