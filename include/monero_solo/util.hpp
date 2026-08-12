#pragma once

#include "monero_solo/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace monero_solo {

struct Endpoint {
    std::string host;
    std::uint16_t port{};
    bool ipv6_literal{};
    bool ip_literal{};

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool is_definitely_loopback() const noexcept;
};

[[nodiscard]] Endpoint parse_endpoint(std::string_view text);

[[nodiscard]] Bytes hex_decode(std::string_view text);
[[nodiscard]] Bytes hex_decode_exact(std::string_view text, std::size_t bytes);
[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> bytes);

template <std::size_t N>
[[nodiscard]] std::array<std::uint8_t, N> hex_decode_array(
    std::string_view text)
{
    const Bytes decoded = hex_decode_exact(text, N);
    std::array<std::uint8_t, N> out{};
    std::copy(decoded.begin(), decoded.end(), out.begin());
    return out;
}

[[nodiscard]] bool constant_time_equal(std::span<const std::uint8_t> left,
                                       std::span<const std::uint8_t> right) noexcept;
[[nodiscard]] bool constant_time_equal(std::string_view left,
                                       std::string_view right) noexcept;

[[nodiscard]] Hash32 sha256(std::span<const std::uint8_t> input);
[[nodiscard]] Hash32 hmac_sha256(std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> input);
[[nodiscard]] Hash32 keccak256(std::span<const std::uint8_t> input) noexcept;

[[nodiscard]] DuplicateKey make_duplicate_key(const Id16 &private_entropy,
                                              const Hash32 &pow_hash) noexcept;
[[nodiscard]] Hash32 make_candidate_key(
    std::span<const std::uint8_t> frozen_full_block);

void os_random_exact(std::span<std::uint8_t> output, bool nonblocking = false);
[[nodiscard]] Id16 random_id16();

[[nodiscard]] std::string format_rfc3339_utc_us(std::int64_t unix_microseconds);
[[nodiscard]] std::int64_t unix_time_us();

[[nodiscard]] std::vector<std::string> parse_blocknotify_argv(
    std::string_view command_template,
    bool validate_executable = true);
[[nodiscard]] std::vector<std::string> expand_blocknotify_argv(
    const std::vector<std::string> &template_argv,
    const Hash32 &miner_tx_hash);

[[nodiscard]] bool path_parent_is_safe(const std::string &absolute_path,
                                       std::string *reason = nullptr);
void validate_database_path(const std::string &absolute_path);
void validate_log_path(const std::string &absolute_path);
void validate_unix_socket_path(const std::string &absolute_path);

} // namespace monero_solo
