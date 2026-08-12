#include "monero_solo/monero.hpp"

#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void test_keccak_and_address()
{
    const std::array<std::uint8_t, 0> empty{};
    require(monero_solo::hex_encode(monero_solo::keccak256(empty)) ==
                "c5d2460186f7233c927e7db2dcc703c0"
                "e500b653ca82273b7bfad8045d85a470",
            "legacy Keccak-256 vector mismatch");
    const std::string mainnet =
        "44AFFq5kSiGBoZ4NMDwYtN18obc8AemS33DBLWs3H7otXft3XjrpDtQGv7SqSsa"
        "BYBb98uNbr2VBBEt7f2wfn3RVGQBEP3A";
    const auto decoded = monero_solo::decode_primary_address(
        mainnet, monero_solo::Network::mainnet);
    require(decoded.prefix == 18, "mainnet prefix mismatch");
    monero_solo::validate_primary_address(mainnet, monero_solo::Network::regtest);
    try {
        monero_solo::validate_primary_address(mainnet,
                                              monero_solo::Network::testnet);
        throw std::runtime_error("network mismatch accepted");
    } catch (const monero_solo::ValidationError &) {
    }
}

void test_target_boundaries()
{
    require(monero_solo::share_target64(1) ==
                std::numeric_limits<std::uint64_t>::max(),
            "difficulty-one target mismatch");
    require(monero_solo::share_target_hex(2) == "ffffffffffffff7f",
            "little-endian share target mismatch");
    monero_solo::Hash32 hash{};
    const std::uint64_t target = 100;
    hash[24] = 99;
    require(monero_solo::meets_share_target(hash, target),
            "target-1 must pass");
    hash[24] = 100;
    require(!monero_solo::meets_share_target(hash, target),
            "target equality must fail");

    hash.fill(0xff);
    require(monero_solo::meets_network_target(hash, "1"),
            "maximum hash must pass difficulty one");
    require(!monero_solo::meets_network_target(hash, "2"),
            "maximum hash must fail difficulty two");
    hash.fill(0);
    require(monero_solo::meets_network_target(
                hash, "340282366920938463463374607431768211455"),
            "zero hash must pass maximum uint128 difficulty");

    struct BoundaryVector final {
        std::string_view difficulty;
        std::string_view largest_passing_hash_le;
        std::string_view smallest_failing_hash_le;
    };
    constexpr std::array vectors{
        BoundaryVector{
            "2",
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
            "0000000000000000000000000000000000000000000000000000000000000080"},
        BoundaryVector{
            "65537",
            "ffff0000ffff0000ffff0000ffff0000ffff0000ffff0000ffff0000ffff0000",
            "00000100ffff0000ffff0000ffff0000ffff0000ffff0000ffff0000ffff0000"},
        BoundaryVector{
            "18446744073709551615",
            "0100000000000000010000000000000001000000000000000100000000000000",
            "0200000000000000010000000000000001000000000000000100000000000000"},
        BoundaryVector{
            "18446744073709551616",
            "ffffffffffffffffffffffffffffffffffffffffffffffff0000000000000000",
            "0000000000000000000000000000000000000000000000000100000000000000"},
        BoundaryVector{
            "1208925819614629174706299",
            "ffff183b000000000000000085ffffffffffffffffff00000000000000000000",
            "0000193b000000000000000085ffffffffffffffffff00000000000000000000"},
        BoundaryVector{
            "340282366920938463463374607431768211455",
            "0100000000000000000000000000000001000000000000000000000000000000",
            "0200000000000000000000000000000001000000000000000000000000000000"},
    };
    for (const auto &vector : vectors) {
        const auto passing = monero_solo::hex_decode_array<32>(
            vector.largest_passing_hash_le);
        const auto failing = monero_solo::hex_decode_array<32>(
            vector.smallest_failing_hash_le);
        require(monero_solo::meets_network_target(passing, vector.difficulty),
                "Monero wide-difficulty maximum passing boundary failed");
        require(!monero_solo::meets_network_target(failing, vector.difficulty),
                "Monero wide-difficulty minimum failing boundary passed");
    }

    monero_solo::validate_network_difficulty("1");
    monero_solo::validate_network_difficulty(
        "340282366920938463463374607431768211455");
    const std::uint64_t unsigned_zero = 0;
    const std::string unsigned_zero_text = std::to_string(unsigned_zero);
    for (const std::string_view invalid : {
             std::string_view{},
             std::string_view{unsigned_zero_text},
             std::string_view{"00"}, std::string_view{"01"},
             std::string_view{"-1"}, std::string_view{"1.0"},
             std::string_view{
                 "340282366920938463463374607431768211456"}}) {
        try {
            monero_solo::validate_network_difficulty(invalid);
            throw std::runtime_error(
                "noncanonical or out-of-range network difficulty accepted");
        } catch (const monero_solo::ValidationError &) {
        }
    }
}

void test_constant_time_hash_comparison()
{
    monero_solo::Hash32 expected{};
    monero_solo::Hash32 actual{};
    expected.fill(0xa5);
    actual = expected;
    require(monero_solo::constant_time_equal(expected, actual),
            "equal PoW hashes did not compare equal");
    actual.front() ^= 0x01;
    require(!monero_solo::constant_time_equal(expected, actual),
            "first-byte PoW mismatch compared equal");
    actual = expected;
    actual.back() ^= 0x01;
    require(!monero_solo::constant_time_equal(expected, actual),
            "last-byte PoW mismatch compared equal");
}

monero_solo::Bytes synthetic_block()
{
    monero_solo::Bytes block;
    block.push_back(14); // major
    block.push_back(14); // minor
    block.push_back(0);  // timestamp
    block.insert(block.end(), 32, 0); // prev id
    block.insert(block.end(), 4, 0);  // nonce
    block.push_back(2);  // miner tx version
    block.push_back(0);  // unlock time
    block.push_back(1);  // input count
    block.push_back(0xff);
    block.push_back(1);  // height
    block.push_back(1);  // output count
    block.push_back(1);  // amount
    block.push_back(2);  // output to key
    block.insert(block.end(), 32, 0x11);
    block.push_back(51); // extra size
    block.push_back(1);  // pubkey tag
    block.insert(block.end(), 32, 0x22);
    block.push_back(2);  // nonce tag
    block.push_back(16); // nonce length
    block.insert(block.end(), 16, 0);
    block.push_back(0);  // null RingCT type
    block.push_back(0);  // tx hash count
    return block;
}

void test_block_parser_and_mutation()
{
    const auto parsed = monero_solo::parse_block(synthetic_block());
    require(parsed.major_version == 14 && parsed.miner_transaction.height == 1,
            "synthetic block metadata mismatch");
    const std::size_t reserved = parsed.miner_transaction.extra_begin_offset + 35;
    require(monero_solo::reserved_offset_is_exact_extra_nonce(parsed, reserved),
            "reserved extra nonce was not found exactly");
    monero_solo::Id16 entropy{};
    entropy.fill(0xab);
    const auto mutated = monero_solo::mutate_reserved_bytes(parsed, reserved, entropy);
    require(std::equal(entropy.begin(), entropy.end(),
                       mutated.blob.begin() + static_cast<std::ptrdiff_t>(reserved)),
            "reserved mutation did not persist");
    require(monero_solo::miner_transaction_hash(parsed) !=
                monero_solo::miner_transaction_hash(mutated),
            "coinbase hash did not change after private entropy mutation");
    require(monero_solo::block_hashing_blob(parsed).size() ==
                parsed.nonce_offset + 4 + 32 + 1,
            "block hashing blob layout mismatch");
    monero_solo::Nonce4 nonce{1, 2, 3, 4};
    const auto finalized = monero_solo::insert_nonce(mutated, nonce);
    require(std::equal(nonce.begin(), nonce.end(),
                       finalized.blob.begin() +
                           static_cast<std::ptrdiff_t>(finalized.nonce_offset)),
            "raw nonce byte insertion changed order");

    auto malformed = synthetic_block();
    malformed.push_back(0);
    try {
        (void)monero_solo::parse_block(malformed);
        throw std::runtime_error("trailing block byte accepted");
    } catch (const monero_solo::MoneroParseError &) {
    }
}

void test_duplicate_and_candidate_keys()
{
    monero_solo::Id16 entropy{};
    monero_solo::Hash32 hash{};
    entropy[0] = 1;
    hash[0] = 2;
    const auto duplicate = monero_solo::make_duplicate_key(entropy, hash);
    require(duplicate.size() == 48 && duplicate[0] == 1 && duplicate[16] == 2,
            "duplicate key layout mismatch");
    const std::array<std::uint8_t, 3> block{1, 2, 3};
    require(monero_solo::hex_encode(monero_solo::make_candidate_key(block)) ==
                "ce2c9065142529fca51f8c0055d9956c"
                "660db2d90ceea570c7f153b45f8405df",
            "candidate-key domain vector mismatch");
}

} // namespace

int main()
{
    try {
        test_keccak_and_address();
        test_target_boundaries();
        test_constant_time_hash_comparison();
        test_block_parser_and_mutation();
        test_duplicate_and_candidate_keys();
        std::cout << "Monero primitive tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Monero primitive tests failed: " << error.what() << '\n';
        return 1;
    }
}
