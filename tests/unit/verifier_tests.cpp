/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/verifier.hpp"
#include "monero_solo/verifier_mailbox.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using monero_solo::verifier::AesMode;
using monero_solo::verifier::Completion;
using monero_solo::verifier::CompletionMailbox;
using monero_solo::verifier::Config;
using monero_solo::verifier::Correlation;
using monero_solo::verifier::Error;
using monero_solo::verifier::JitMode;
using monero_solo::verifier::LargePages;
using monero_solo::verifier::MemoryMode;
using monero_solo::verifier::PowHash;
using monero_solo::verifier::SeedHash;
using monero_solo::verifier::ShutdownMode;
using monero_solo::verifier::SubmissionKind;
using monero_solo::verifier::Verifier;

void require(const bool condition, const char *const message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint8_t hex_digit(const char value)
{
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::runtime_error("invalid test fixture hex");
}

std::vector<std::uint8_t> hex(const std::string &text)
{
    require((text.size() % 2U) == 0, "odd test fixture hex");
    std::vector<std::uint8_t> bytes(text.size() / 2U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            static_cast<unsigned>(hex_digit(text[index * 2U])) << 4U |
            hex_digit(text[index * 2U + 1U]));
    }
    return bytes;
}

template<std::size_t Size>
std::array<std::uint8_t, Size> hex_array(const std::string &text)
{
    const std::vector<std::uint8_t> bytes = hex(text);
    require(bytes.size() == Size, "wrong fixed test fixture size");
    std::array<std::uint8_t, Size> result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

const SeedHash &monero_seed()
{
    static const SeedHash value = hex_array<32>(
        "d432f499205150873b2572b5f033c9c6"
        "e4b7c6f3394bd2dd93822cd7085e7307");
    return value;
}

const std::vector<std::uint8_t> &monero_blob()
{
    /* Published MSPV/RandomX v1.2.2 light/fast known-answer fixture. */
    static const std::vector<std::uint8_t> value = hex(
        "0e0ed286da8006ecdc1aab3033cf1716c52f13f9d8ae0051615a2453643de946"
        "43b550d543becd0000000002abc78b0101ffefc68b0101fcfcf0d4b422025014"
        "bb4a1eade6622fd781cb1063381cad396efa69719b41aa28b4fce8c7ad4b5f01"
        "9ce1dc670456b24a5e03c2d9058a2df10fec779e2579753b1847b74ee644f16b"
        "023c000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000005"
        "1399a1bc46a846474f5b33db24eae173a26393b976054ee14f9feefe999252338"
        "02867097564c9db7a36af5bb5ed33ab46e63092bd8d32cef121608c3258edd555"
        "62812e21cc7e3ac73045745a72f7d74581d9a0849d6f30e8b2923171253e864f"
        "4e9ddea3acb5bc755f1c4a878130a70c26297540bc0b7a57affb6b35c1f03d8d"
        "bd54ece8457531f8cba15bb74516779c01193e212050423020e45aa2c15dcb");
    return value;
}

const PowHash &monero_expected()
{
    static const PowHash value = hex_array<32>(
        "d0402d6834e26fb94a9ce38c6424d27d"
        "2069896a9b8b1ce685d79936bca6e0a8");
    return value;
}

Config light_config()
{
    Config config{};
    config.workers = 1;
    config.seed_init_threads = 1;
    config.pending_capacity = 8;
    config.max_outstanding = 8;
    config.max_input_size = 4096;
    config.max_seeds = 2;
    config.max_buffered_input_bytes = 1024U * 1024U;
    config.memory_mode = MemoryMode::light;
    config.large_pages = LargePages::disabled;
    config.jit = JitMode::disabled;
    config.aes = AesMode::software;
    return config;
}

void test_exact_config_mapping()
{
    Config config{};
    const auto defaults = Verifier::map_config(config);
    require(defaults.abi_version == MSPV_ABI_VERSION, "wrong ABI mapping");
    require(defaults.worker_count == 4, "wrong worker mapping");
    require(defaults.seed_init_threads == 4, "wrong init-thread mapping");
    require(defaults.pending_capacity == 256, "wrong pending mapping");
    require(defaults.max_outstanding == 512, "wrong outstanding mapping");
    require(defaults.max_input_size == 4096, "wrong input mapping");
    require(defaults.max_seed_key_size == 32, "Monero key size not fixed");
    require(defaults.max_seeds == 2, "wrong seed-capacity mapping");
    require(defaults.max_buffered_input_bytes == 16U * 1024U * 1024U,
            "wrong byte-cap mapping");
    require(defaults.memory_mode == MSPV_MEMORY_FAST, "wrong memory mapping");
    require(defaults.large_pages == MSPV_LARGE_PAGES_TRY,
            "wrong large-page mapping");
    require(defaults.options == MSPV_OPTION_SECURE_JIT,
            "secure JIT mapping must replace, not OR with stale defaults");
    require(defaults.log_level == MSPV_LOG_INFO, "wrong log mapping");

    config.jit = JitMode::enabled;
    config.aes = AesMode::software;
    const auto portable_aes = Verifier::map_config(config);
    require(portable_aes.options == MSPV_OPTION_DISABLE_HARD_AES,
            "enabled JIT/software AES option mapping is wrong");

    config.jit = JitMode::disabled;
    const auto no_jit = Verifier::map_config(config);
    require(no_jit.options ==
                (MSPV_OPTION_DISABLE_JIT | MSPV_OPTION_DISABLE_HARD_AES),
            "disabled JIT option mapping is wrong");

    config.max_seeds = 1;
    bool rejected = false;
    try {
        (void)Verifier::map_config(config);
    }
    catch (const Error &error) {
        rejected = error.status() == MSPV_INVALID_CONFIG;
    }
    require(rejected, "server adapter accepted max_seeds < 2");
}

void test_completion_mailbox()
{
    CompletionMailbox mailbox(2);
    require(mailbox.closed(), "new completion mailbox was open");
    mailbox.open();
    require(mailbox.register_waiter(10), "mailbox waiter registration failed");
    Completion completion;
    completion.user_tag = 10;
    completion.ticket = 77;
    require(mailbox.publish(completion), "registered completion was discarded");
    completion.ticket = 88;
    require(!mailbox.publish(completion), "duplicate completion overwrote first result");
    const auto delivered = mailbox.wait(10, std::chrono::milliseconds(1));
    require(delivered.has_value() && delivered->ticket == 77 && mailbox.size() == 0,
            "mailbox did not consume the first published completion");

    require(mailbox.register_waiter(20), "mailbox cancel fixture failed");
    mailbox.cancel(20);
    completion.user_tag = 20;
    require(!mailbox.publish(completion) && mailbox.size() == 0,
            "late completion created an unowned mailbox entry");
    require(mailbox.register_waiter(21), "mailbox timeout fixture failed");
    require(!mailbox.wait(21, std::chrono::milliseconds(0)).has_value() &&
                mailbox.size() == 0,
            "zero-timeout mailbox wait retained its slot");
    completion.user_tag = 21;
    require(!mailbox.publish(completion),
            "completion published after deterministic timeout was retained");
    require(mailbox.register_waiter(30) && mailbox.register_waiter(31) &&
                !mailbox.register_waiter(32) && mailbox.size() == 2,
            "mailbox capacity was not enforced exactly");
    mailbox.cancel(30);
    mailbox.cancel(31);

    for (std::uint64_t tag = 100; tag < 1100; ++tag) {
        require(mailbox.register_waiter(tag), "mailbox race registration failed");
        completion.user_tag = tag;
        std::jthread publisher([&mailbox, completion] {
            (void)mailbox.publish(completion);
        });
        (void)mailbox.wait(tag, std::chrono::milliseconds(0));
        publisher.join();
        require(mailbox.size() == 0, "publish/timeout race leaked a slot");
    }

    require(mailbox.register_waiter(2000), "mailbox close fixture failed");
    std::atomic<bool> waiter_returned{false};
    const auto started = std::chrono::steady_clock::now();
    std::jthread waiter([&] {
        require(!mailbox.wait(2000, std::chrono::seconds(10)).has_value(),
                "closed mailbox returned a completion");
        waiter_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mailbox.close();
    waiter.join();
    require(waiter_returned.load(std::memory_order_acquire) &&
                std::chrono::steady_clock::now() - started <
                    std::chrono::seconds(1) &&
                mailbox.size() == 0,
            "mailbox close did not promptly wake and clear a waiter");
}

std::vector<Completion> wait_for_completions(Verifier &verifier,
                                             const std::size_t wanted)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    std::vector<Completion> result;
    while (result.size() < wanted &&
           std::chrono::steady_clock::now() < deadline) {
        auto drained = verifier.drain_completions();
        require(drained.terminal_status == MSPV_TIMEOUT ||
                    drained.terminal_status == MSPV_OK,
                "unexpected completion-poll status while running");
        result.insert(result.end(),
                      drained.completions.begin(),
                      drained.completions.end());
        if (result.size() < wanted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    require(result.size() == wanted, "timed out waiting for MSPV completion");
    return result;
}

void test_known_answer_tracking_rotation_and_cancel_shutdown()
{
    std::atomic<std::uint64_t> wakes{0};
    Config config = light_config();
    config.wake = [&]() { wakes.fetch_add(1, std::memory_order_relaxed); };
    Verifier verifier(std::move(config));
    require(verifier.is_running(), "adapter did not start");

    const auto prepared = verifier.prepare_seed(monero_seed());
    require(prepared.status == MSPV_OK && prepared.seed.seed_id != 0,
            "seed prepare failed");
    const mspv_seed_id seed_a = prepared.seed.seed_id;
    require(verifier.wait_seed_ready(seed_a, 120'000) == MSPV_OK,
            "seed did not become ready");
    const auto ready = verifier.seed_snapshot(seed_a);
    require(ready.status == MSPV_OK, "seed snapshot failed");
    require(ready.seed.key_size == 32, "adapter did not retain a Monero key");
    require(ready.seed.prepare_ns > 0, "seed preparation timing missing");
    require(!ready.seed.memory_uses_large_pages,
            "disabled large pages reported as enabled");
    require(verifier.activate_seed(seed_a) == MSPV_OK, "seed activation failed");
    require(verifier.retain_seed(seed_a) == MSPV_OK,
            "retained-job reference failed");

    std::vector<std::uint8_t> copied_input = monero_blob();
    PowHash copied_claim = monero_expected();
    const auto match = verifier.submit_verify(
        seed_a, copied_input, copied_claim, 1001);
    require(match.status == MSPV_OK && match.ticket != 0,
            "known-answer submit failed");

    /* MSPV promises to copy both buffers before submit returns. */
    std::fill(copied_input.begin(), copied_input.end(), 0);
    copied_claim.fill(0);

    PowHash wrong{};
    const auto mismatch = verifier.submit_verify(
        seed_a, monero_blob(), wrong, 1002);
    require(mismatch.status == MSPV_OK, "mismatch submit failed");
    const auto hash_only = verifier.submit_hash(seed_a, monero_blob(), 1003);
    require(hash_only.status == MSPV_OK, "hash-only submit failed");
    require(verifier.submit_hash(seed_a, monero_blob(), 1001).status ==
                MSPV_INVALID_ARGUMENT,
            "duplicate live durable user_tag was accepted");
    require(verifier.tracked_submission_count() == 3,
            "adapter tracking count is wrong before drain");

    const std::vector<Completion> completions =
        wait_for_completions(verifier, 3);
    require(verifier.tracked_submission_count() == 0,
            "adapter did not release completion correlations");
    for (const Completion &completion : completions) {
        require(completion.correlation == Correlation::matched,
                "ticket/user-tag/seed correlation failed");
        require(completion.result == MSPV_RESULT_OK &&
                    completion.error == MSPV_OK,
                "known-answer RandomX execution failed");
        require(completion.hash == monero_expected(),
                "authoritative computed hash differs from known answer");
    }

    const auto by_tag = [&](const std::uint64_t tag) -> const Completion & {
        const auto found = std::find_if(
            completions.begin(), completions.end(), [tag](const Completion &item) {
                return item.user_tag == tag;
            });
        require(found != completions.end(), "completion user_tag missing");
        return *found;
    };
    require(by_tag(1001).ticket == match.ticket &&
                by_tag(1001).comparison == MSPV_COMPARISON_MATCH &&
                by_tag(1001).submission_kind == SubmissionKind::verify_claim,
            "MATCH completion metadata is wrong");
    require(by_tag(1002).ticket == mismatch.ticket &&
                by_tag(1002).comparison == MSPV_COMPARISON_MISMATCH,
            "MISMATCH completion metadata is wrong");
    require(by_tag(1003).ticket == hash_only.ticket &&
                by_tag(1003).comparison == MSPV_COMPARISON_NOT_REQUESTED &&
                by_tag(1003).submission_kind == SubmissionKind::hash_only,
            "hash-only completion metadata is wrong");

    const auto stats = verifier.stats();
    require(stats.status == MSPV_OK && stats.stats.outstanding == 0 &&
                stats.stats.pending == 0 && stats.stats.running == 0 &&
                stats.stats.buffered_input_bytes == 0,
            "MSPV resources did not return to zero after drain");
    require(wakes.load(std::memory_order_relaxed) > 0,
            "native notifier never signalled the adapter");

    /* A requested release waits for retained jobs and current designation. */
    require(verifier.request_seed_release(seed_a) == MSPV_OK,
            "release request failed");
    require(verifier.retain_seed(seed_a) == MSPV_OK,
            "retained old job could not extend a pending seed release");
    const auto retained_after_release_request = verifier.submit_hash(
        seed_a, monero_blob(), 1004);
    require(retained_after_release_request.status == MSPV_OK,
            "retained old job could not verify during pending seed release");
    const auto retained_completion = wait_for_completions(verifier, 1);
    require(retained_completion.front().user_tag == 1004 &&
                retained_completion.front().hash == monero_expected(),
            "pending-release old-job verification was incorrect");
    require(verifier.drop_seed_reference(seed_a) == MSPV_OK,
            "dropping extended old-job reference failed");
    auto pending_release = verifier.seed_snapshot(seed_a);
    require(pending_release.status == MSPV_OK &&
                pending_release.seed.release_requested &&
                !pending_release.seed.release_started,
            "current seed release began too early");

    SeedHash second_seed = monero_seed();
    second_seed[0] ^= 0x80U;
    const auto second = verifier.prepare_seed(second_seed);
    require(second.status == MSPV_OK && second.seed.seed_id != seed_a,
            "second seed preparation failed");
    require(verifier.wait_seed_ready(second.seed.seed_id, 120'000) == MSPV_OK,
            "second seed did not become ready");
    require(verifier.activate_seed(second.seed.seed_id) == MSPV_OK,
            "seed rotation failed");

    // A downward reorg may make the former seed current before retained old
    // jobs have drained. Re-prepare must cancel the not-yet-started release.
    const auto reorg_seed = verifier.prepare_seed(monero_seed());
    require(reorg_seed.status == MSPV_OK && reorg_seed.seed.seed_id == seed_a &&
                !reorg_seed.seed.release_requested &&
                verifier.activate_seed(seed_a) == MSPV_OK,
            "pending seed release could not be cancelled for a reorg");
    require(verifier.activate_seed(second.seed.seed_id) == MSPV_OK,
            "seed could not rotate again after reorg cancellation");
    require(verifier.request_seed_release(seed_a) == MSPV_OK,
            "former seed release could not be requested again");
    require(verifier.drop_seed_reference(seed_a) == MSPV_OK,
            "dropping retained-job reference failed");
    require(verifier.wait_seed_released(seed_a, 120'000) == MSPV_OK,
            "former seed was not released after references drained");

    const std::vector auto_snapshots = verifier.seed_snapshots();
    require(auto_snapshots.size() == 1 &&
                auto_snapshots.front().seed_id == second.seed.seed_id,
            "released seed remained in resident snapshots");

    // Released adapter records must not accumulate outside native max_seeds.
    SeedHash rotating_seed = second_seed;
    mspv_seed_id rotating_id = second.seed.seed_id;
    for (unsigned cycle = 0; cycle < 8U; ++cycle) {
        rotating_seed[cycle % rotating_seed.size()] ^= 0x01U;
        const auto next = verifier.prepare_seed(rotating_seed);
        require(next.status == MSPV_OK,
                "seed churn could not prepare within native capacity");
        require(verifier.wait_seed_ready(next.seed.seed_id, 120'000) == MSPV_OK &&
                    verifier.activate_seed(next.seed.seed_id) == MSPV_OK,
                "seed churn activation failed");
        require(verifier.request_seed_release(rotating_id) == MSPV_OK &&
                    verifier.wait_seed_released(rotating_id, 120'000) == MSPV_OK,
                "seed churn release failed");
        rotating_id = next.seed.seed_id;
        require(verifier.seed_snapshots().size() == 1U,
                "released adapter seed records accumulated");
    }

    require(verifier.shutdown(ShutdownMode::cancel_pending) == MSPV_OK,
            "CANCEL_PENDING shutdown failed");
    require(verifier.is_shutdown(), "adapter did not enter shutdown state");
    const auto closed = verifier.drain_completions();
    require(closed.completions.empty() &&
                closed.terminal_status == MSPV_CLOSED,
            "completion stream did not close after cancel shutdown");
}

void test_drain_shutdown_without_work()
{
    Verifier verifier(light_config());
    require(verifier.shutdown(ShutdownMode::drain) == MSPV_OK,
            "DRAIN shutdown failed");
    const auto closed = verifier.drain_completions();
    require(closed.terminal_status == MSPV_CLOSED,
            "DRAIN completion stream did not close");
    require(verifier.shutdown(ShutdownMode::drain) == MSPV_OK,
            "shutdown was not idempotent");
}

} // namespace

int main()
{
    try {
        test_exact_config_mapping();
        test_completion_mailbox();
        test_known_answer_tracking_rotation_and_cancel_shutdown();
        test_drain_shutdown_without_work();
        std::cout << "verifier adapter tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "verifier adapter test failure: " << error.what() << '\n';
        return 1;
    }
}
