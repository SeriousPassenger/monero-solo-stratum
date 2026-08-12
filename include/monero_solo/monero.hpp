#pragma once

#include "monero_solo/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace monero_solo {

struct PrimaryAddress {
    Network network{};
    std::uint64_t prefix{};
    Hash32 spend_public_key{};
    Hash32 view_public_key{};
};

[[nodiscard]] PrimaryAddress decode_primary_address(std::string_view address,
                                                    Network expected_network);
void validate_primary_address(std::string_view address,
                              Network expected_network);

[[nodiscard]] std::uint64_t share_target64(std::uint64_t difficulty);
[[nodiscard]] std::array<std::uint8_t, 8> share_target_le(
    std::uint64_t difficulty);
[[nodiscard]] std::string share_target_hex(std::uint64_t difficulty);
[[nodiscard]] std::uint64_t pow_word64(const Hash32 &hash) noexcept;
[[nodiscard]] bool meets_share_target(const Hash32 &hash,
                                      std::uint64_t target64) noexcept;
[[nodiscard]] std::uint64_t actual_difficulty(const Hash32 &hash) noexcept;
void validate_network_difficulty(std::string_view difficulty_decimal);
[[nodiscard]] bool meets_network_target(const Hash32 &hash,
                                        std::string_view difficulty_decimal);

struct ParsedMinerTransaction {
    std::size_t begin_offset{};
    std::size_t end_offset{};
    std::size_t prefix_end_offset{};
    std::size_t unprunable_end_offset{};
    std::size_t extra_begin_offset{};
    std::size_t extra_size{};
    std::uint64_t version{};
    std::uint64_t height{};
    std::uint64_t input_count{};
    std::uint64_t output_count{};
    bool is_coinbase{};
};

struct ParsedBlock {
    Bytes blob;
    std::uint64_t major_version{};
    std::uint64_t minor_version{};
    std::uint64_t timestamp{};
    Hash32 previous_hash{};
    std::size_t nonce_offset{};
    ParsedMinerTransaction miner_transaction;
    std::vector<Hash32> transaction_hashes;
};

[[nodiscard]] ParsedBlock parse_block(std::span<const std::uint8_t> blob);
[[nodiscard]] Hash32 miner_transaction_hash(const ParsedBlock &block);
[[nodiscard]] Hash32 transaction_tree_hash(const ParsedBlock &block);
[[nodiscard]] Bytes block_hashing_blob(const ParsedBlock &block);
[[nodiscard]] Hash32 block_hash(const ParsedBlock &block);
[[nodiscard]] ParsedBlock mutate_reserved_bytes(
    const ParsedBlock &block,
    std::size_t reserved_offset,
    std::span<const std::uint8_t> replacement);
[[nodiscard]] ParsedBlock insert_nonce(const ParsedBlock &block,
                                       const Nonce4 &nonce);
[[nodiscard]] bool reserved_offset_is_exact_extra_nonce(
    const ParsedBlock &block,
    std::size_t reserved_offset,
    std::size_t reserve_size = 16);

[[nodiscard]] std::vector<std::uint8_t> encode_varint(std::uint64_t value);

} // namespace monero_solo
