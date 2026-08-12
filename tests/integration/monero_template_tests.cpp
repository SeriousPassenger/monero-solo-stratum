#include "monero_solo/monero.hpp"
#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using monero_solo::Bytes;

struct DaemonTemplateFixture final {
    std::string_view blocktemplate_blob;
    std::string_view blockhashing_blob;
    std::size_t reserved_offset;
    std::uint64_t height;
    std::string_view prev_hash;
    std::string_view miner_tx_hash;
    std::string_view block_id;
};

// The expected hashes below were generated independently with the pinned
// Monero v0.18.5.1 src/crypto/keccak.c implementation and Monero's canonical
// transaction-tree construction.  The fixture intentionally resembles a
// current daemon response: v16 header, v2/null-RingCT coinbase, tagged output,
// exact 16-byte tx-extra nonce, and a nonempty transaction-hash vector.
constexpr DaemonTemplateFixture kDaemonFixture{
    .blocktemplate_blob =
        "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
        "1a1b1c1d1e1fa1b2c3d402f685e40101ffba85e4010180e0a596bb1103404142"
        "434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f7e3301"
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
        "0210000000000000000000000000000000000004202122232425262728292a2b2c"
        "2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c"
        "4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c"
        "6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c"
        "8d8e8f909192939495969798999a9b9c9d9e9f",
    .blockhashing_blob =
        "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
        "1a1b1c1d1e1fa1b2c3d45ec8d04ebe937c6760b1e165cbc49a9a5b52d9c5308b"
        "56a090ddb029c8c719a905",
    .reserved_offset = 131,
    .height = 3'736'250,
    .prev_hash =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
    .miner_tx_hash =
        "94045406ace809d0830c5f734f170e396a430e4a80f3a835d41ce74c610adc86",
    .block_id =
        "442e1870d882e629cc78b43e829bfdea29f62d67ab1bd41b6f7cf0dc290c5413",
};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void append(Bytes &destination, std::span<const std::uint8_t> source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

void append_varint(Bytes &destination, std::uint64_t value)
{
    append(destination, monero_solo::encode_varint(value));
}

template <std::size_t Size>
std::array<std::uint8_t, Size> sequence(std::uint8_t first)
{
    std::array<std::uint8_t, Size> result{};
    for (std::size_t index = 0; index < Size; ++index) {
        result[index] = static_cast<std::uint8_t>(first + index);
    }
    return result;
}

Bytes construct_daemon_shaped_template()
{
    constexpr std::uint64_t timestamp = 1'723'456'789;
    constexpr std::uint64_t unlock_height = kDaemonFixture.height + 60;
    constexpr std::uint64_t reward = 600'000'000'000;

    Bytes block;
    append_varint(block, 16); // current-style major version
    append_varint(block, 16); // current-style minor version
    append_varint(block, timestamp);
    append(block, sequence<32>(0x00)); // previous block ID
    append(block, std::array<std::uint8_t, 4>{0xa1, 0xb2, 0xc3, 0xd4});

    append_varint(block, 2); // miner transaction version
    append_varint(block, unlock_height);
    append_varint(block, 1); // one generation input
    block.push_back(0xff);
    append_varint(block, kDaemonFixture.height);
    append_varint(block, 1); // one miner output
    append_varint(block, reward);
    block.push_back(0x03); // txout_to_tagged_key
    append(block, sequence<32>(0x40));
    block.push_back(0x7e); // view tag

    Bytes extra;
    extra.push_back(0x01); // primary tx public key
    append(extra, sequence<32>(0x80));
    extra.push_back(0x02); // tx extra nonce
    append_varint(extra, 16);
    extra.insert(extra.end(), 16, 0);
    append_varint(block, extra.size());
    append(block, extra);
    block.push_back(0x00); // null RingCT type

    append_varint(block, 4); // four non-miner transaction hashes
    append(block, sequence<32>(0x20));
    append(block, sequence<32>(0x40));
    append(block, sequence<32>(0x60));
    append(block, sequence<32>(0x80));
    return block;
}

std::size_t changed_byte_count(std::span<const std::uint8_t> before,
                               std::span<const std::uint8_t> after)
{
    require(before.size() == after.size(), "mutation changed block length");
    return static_cast<std::size_t>(std::count_if(
        before.begin(), before.end(),
        [after, index = std::size_t{0}](std::uint8_t byte) mutable {
            return byte != after[index++];
        }));
}

void test_public_daemon_template()
{
    const Bytes constructed = construct_daemon_shaped_template();
    require(monero_solo::hex_encode(constructed) ==
                kDaemonFixture.blocktemplate_blob,
            "constructed daemon block fixture changed");

    const Bytes daemon_blob = monero_solo::hex_decode(
        kDaemonFixture.blocktemplate_blob);
    const monero_solo::ParsedBlock block = monero_solo::parse_block(daemon_blob);

    require(block.blob == constructed, "daemon blob did not round-trip");
    require(block.major_version == 16 && block.minor_version == 16,
            "daemon block version mismatch");
    require(block.timestamp == 1'723'456'789,
            "daemon block timestamp mismatch");
    require(block.nonce_offset == 39, "daemon nonce offset mismatch");
    require(block.miner_transaction.height == kDaemonFixture.height,
            "coinbase height mismatch");
    require(block.miner_transaction.version == 2 &&
                block.miner_transaction.is_coinbase,
            "daemon miner transaction shape mismatch");
    require(block.miner_transaction.extra_begin_offset == 96,
            "daemon tx-extra offset mismatch");
    require(block.transaction_hashes.size() == 4,
            "daemon transaction-hash count mismatch");
    require(monero_solo::hex_encode(block.previous_hash) ==
                kDaemonFixture.prev_hash,
            "daemon prev_hash does not match serialized header");

    require(monero_solo::reserved_offset_is_exact_extra_nonce(
                block, kDaemonFixture.reserved_offset, 16),
            "daemon reserved_offset did not identify the exact nonce field");
    require(!monero_solo::reserved_offset_is_exact_extra_nonce(
                block, kDaemonFixture.reserved_offset - 1, 16),
            "reserved offset immediately before nonce was accepted");
    require(!monero_solo::reserved_offset_is_exact_extra_nonce(
                block, kDaemonFixture.reserved_offset + 1, 16),
            "reserved offset inside nonce was accepted");
    require(!monero_solo::reserved_offset_is_exact_extra_nonce(
                block, block.blob.size(), 16),
            "out-of-range reserved offset was accepted");
    require(!monero_solo::reserved_offset_is_exact_extra_nonce(
                block, kDaemonFixture.reserved_offset, 15),
            "non-16-byte reserve was accepted");

    // A block's serialized transaction prefix treats extra as opaque bytes;
    // reserve validation must nevertheless parse the complete extra rather
    // than accepting a matching nonce followed by a malformed field.
    Bytes malformed_extra = block.blob;
    constexpr std::size_t extra_size_offset = 95;
    const std::size_t extra_end = block.miner_transaction.extra_begin_offset +
                                  block.miner_transaction.extra_size;
    require(malformed_extra[extra_size_offset] == 51,
            "fixture extra-size offset changed");
    malformed_extra[extra_size_offset] = 52;
    malformed_extra.insert(malformed_extra.begin() +
                               static_cast<std::ptrdiff_t>(extra_end),
                           0x7f);
    const auto malformed_block = monero_solo::parse_block(malformed_extra);
    require(!monero_solo::reserved_offset_is_exact_extra_nonce(
                malformed_block, kDaemonFixture.reserved_offset),
            "matching nonce hid malformed trailing tx-extra data");

    require(monero_solo::hex_encode(monero_solo::block_hashing_blob(block)) ==
                kDaemonFixture.blockhashing_blob,
            "locally regenerated hashing blob differs from daemon fixture");
    require(monero_solo::hex_encode(
                monero_solo::miner_transaction_hash(block)) ==
                kDaemonFixture.miner_tx_hash,
            "public miner transaction hash differs from Monero ground truth");
    require(monero_solo::hex_encode(monero_solo::block_hash(block)) ==
                kDaemonFixture.block_id,
            "public block ID differs from Monero ground truth");
}

void test_private_template_and_frozen_candidate()
{
    const auto public_block = monero_solo::parse_block(
        monero_solo::hex_decode(kDaemonFixture.blocktemplate_blob));
    const auto public_miner_hash =
        monero_solo::miner_transaction_hash(public_block);
    const auto public_tree_hash = monero_solo::transaction_tree_hash(public_block);
    const auto public_block_id = monero_solo::block_hash(public_block);

    const monero_solo::Id16 private_entropy = sequence<16>(0xd0);
    const auto private_block = monero_solo::mutate_reserved_bytes(
        public_block, kDaemonFixture.reserved_offset, private_entropy);

    require(private_block.blob.size() == public_block.blob.size(),
            "private entropy mutation changed serialized length");
    require(changed_byte_count(public_block.blob, private_block.blob) == 16,
            "private entropy mutation changed bytes outside the reserve");
    require(std::equal(private_entropy.begin(), private_entropy.end(),
                       private_block.blob.begin() +
                           static_cast<std::ptrdiff_t>(
                               kDaemonFixture.reserved_offset)),
            "private entropy was not inserted byte-for-byte");
    require(monero_solo::reserved_offset_is_exact_extra_nonce(
                private_block, kDaemonFixture.reserved_offset),
            "private block no longer contains the exact reserved field");
    require(private_block.nonce_offset == public_block.nonce_offset &&
                private_block.previous_hash == public_block.previous_hash &&
                private_block.transaction_hashes == public_block.transaction_hashes,
            "private entropy altered immutable template metadata");

    const auto private_miner_hash =
        monero_solo::miner_transaction_hash(private_block);
    const auto private_tree_hash =
        monero_solo::transaction_tree_hash(private_block);
    const auto private_block_id = monero_solo::block_hash(private_block);
    require(private_miner_hash != public_miner_hash,
            "private entropy did not alter miner transaction hash");
    require(private_tree_hash != public_tree_hash,
            "private entropy did not alter transaction tree root");
    require(private_block_id != public_block_id,
            "private entropy did not alter block ID");
    require(monero_solo::hex_encode(private_miner_hash) ==
                "268bcf5a5604f47f9e2ed341f51a4b61"
                "b2a1b1c8fbbb0e6a7da56621e409390e",
            "private miner transaction hash differs from Monero ground truth");
    require(monero_solo::hex_encode(private_tree_hash) ==
                "c93146f764d8a760a65dd4dc5d32c046"
                "9a847976dfbeda7a7c80daf2bb2fba47",
            "private transaction tree differs from Monero ground truth");
    require(monero_solo::hex_encode(
                monero_solo::block_hashing_blob(private_block)) ==
                "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
                "1a1b1c1d1e1fa1b2c3d4c93146f764d8a760a65dd4dc5d32c0469a847976dfbe"
                "da7a7c80daf2bb2fba4705",
            "private hashing blob differs from Monero ground truth");
    require(monero_solo::hex_encode(private_block_id) ==
                "ef1e2a752ed828bfd8b5955d2c8bd671"
                "4f0f221d3730c1972e6b5b6935442c7d",
            "private block ID differs from Monero ground truth");

    const monero_solo::Nonce4 submitted_nonce{0xde, 0xad, 0xbe, 0xef};
    const auto frozen = monero_solo::insert_nonce(private_block, submitted_nonce);
    require(changed_byte_count(private_block.blob, frozen.blob) == 4,
            "nonce insertion changed bytes outside the header nonce");
    require(std::equal(submitted_nonce.begin(), submitted_nonce.end(),
                       frozen.blob.begin() + static_cast<std::ptrdiff_t>(
                           frozen.nonce_offset)),
            "submitted nonce bytes were endian-transformed");
    require(std::equal(private_entropy.begin(), private_entropy.end(),
                       frozen.blob.begin() + static_cast<std::ptrdiff_t>(
                           kDaemonFixture.reserved_offset)),
            "nonce insertion altered private entropy");
    require(monero_solo::miner_transaction_hash(frozen) == private_miner_hash,
            "header nonce changed miner transaction hash");
    require(monero_solo::transaction_tree_hash(frozen) == private_tree_hash,
            "header nonce changed transaction tree root");
    require(monero_solo::block_hash(frozen) != private_block_id,
            "header nonce did not change block ID");
    require(monero_solo::hex_encode(monero_solo::block_hashing_blob(frozen)) ==
                "101095bae7b506000102030405060708090a0b0c0d0e0f10111213141516171819"
                "1a1b1c1d1e1fdeadbeefc93146f764d8a760a65dd4dc5d32c0469a847976dfbe"
                "da7a7c80daf2bb2fba4705",
            "frozen hashing blob differs from Monero ground truth");
    require(monero_solo::hex_encode(monero_solo::block_hash(frozen)) ==
                "3d4b7aec80c5a23132811b9fd8608c1a"
                "1ae2343b748fddc49a7135dd37071eba",
            "frozen block ID differs from Monero ground truth");
    require(monero_solo::hex_encode(
                monero_solo::make_candidate_key(frozen.blob)) ==
                "3b766fd04735f41d1a54356465437a13"
                "58014b3b89bcbe68912cba1000b79c2e",
            "frozen candidate key vector mismatch");

    const auto reparsed = monero_solo::parse_block(frozen.blob);
    require(reparsed.blob == frozen.blob &&
                monero_solo::block_hash(reparsed) ==
                    monero_solo::block_hash(frozen),
            "frozen candidate did not survive final reparse");
}

} // namespace

int main()
{
    try {
        test_public_daemon_template();
        test_private_template_and_frozen_candidate();
        std::cout << "Monero template integration tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Monero template integration tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
