#include "monero_solo/monero.hpp"

#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace monero_solo {
namespace {

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::size_t kFullEncodedBlock = 11;
constexpr std::size_t kFullDecodedBlock = 8;

constexpr std::array<std::size_t, 12> kEncodedToDecoded = {
    0, 0, 1, 2, 3, 3, 4, 5, 5, 6, 7, 8,
};

[[nodiscard]] std::uint64_t prefix_for(Network network) noexcept
{
    switch (network) {
    case Network::mainnet:
    case Network::regtest: return 18;
    case Network::testnet: return 53;
    case Network::stagenet: return 24;
    }
    return 0;
}

[[nodiscard]] std::uint64_t decode_base58_block(std::string_view encoded,
                                                std::size_t decoded_size)
{
    std::uint64_t result = 0;
    std::uint64_t order = 1;
    for (auto it = encoded.rbegin(); it != encoded.rend(); ++it) {
        const std::size_t digit = kBase58Alphabet.find(*it);
        if (digit == std::string_view::npos) {
            throw ValidationError("Monero address contains a non-Base58 character");
        }
        if (digit != 0 && order > (std::numeric_limits<std::uint64_t>::max() - result) /
                                      digit) {
            throw ValidationError("Monero address Base58 block overflows");
        }
        result += order * digit;
        if (it + 1 != encoded.rend()) {
            if (order > std::numeric_limits<std::uint64_t>::max() / 58) {
                throw ValidationError("Monero address Base58 block overflows");
            }
            order *= 58;
        }
    }
    if (decoded_size < 8 && result >= (std::uint64_t{1} << (decoded_size * 8))) {
        throw ValidationError("Monero address Base58 block is noncanonical");
    }
    return result;
}

[[nodiscard]] Bytes base58_decode(std::string_view encoded)
{
    if (encoded.empty()) throw ValidationError("Monero address is empty");
    const std::size_t full_blocks = encoded.size() / kFullEncodedBlock;
    const std::size_t remainder = encoded.size() % kFullEncodedBlock;
    if (remainder >= kEncodedToDecoded.size() ||
        (remainder != 0 && kEncodedToDecoded[remainder] == 0)) {
        throw ValidationError("Monero address Base58 length is invalid");
    }
    const std::size_t last_decoded = kEncodedToDecoded[remainder];
    Bytes result(full_blocks * kFullDecodedBlock + last_decoded);
    for (std::size_t block = 0; block < full_blocks; ++block) {
        std::uint64_t value = decode_base58_block(
            encoded.substr(block * kFullEncodedBlock, kFullEncodedBlock), 8);
        for (std::size_t i = 0; i < 8; ++i) {
            result[block * 8 + 7 - i] = static_cast<std::uint8_t>(value);
            value >>= 8;
        }
    }
    if (remainder != 0) {
        std::uint64_t value = decode_base58_block(
            encoded.substr(full_blocks * kFullEncodedBlock), last_decoded);
        for (std::size_t i = 0; i < last_decoded; ++i) {
            result[full_blocks * 8 + last_decoded - 1 - i] =
                static_cast<std::uint8_t>(value);
            value >>= 8;
        }
    }
    return result;
}

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return data_.size() - position_;
    }
    [[nodiscard]] bool eof() const noexcept { return position_ == data_.size(); }

    [[nodiscard]] std::uint8_t byte()
    {
        require(1);
        return data_[position_++];
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t count)
    {
        require(count);
        auto result = data_.subspan(position_, count);
        position_ += count;
        return result;
    }

    void skip(std::size_t count) { (void)bytes(count); }

    [[nodiscard]] std::uint64_t varint()
    {
        const std::size_t start = position_;
        std::uint64_t value = 0;
        unsigned shift = 0;
        for (unsigned i = 0; i < 10; ++i) {
            const std::uint8_t current = byte();
            if (i == 9 && current > 1) {
                throw MoneroParseError("varint overflows uint64");
            }
            value |= static_cast<std::uint64_t>(current & 0x7fU) << shift;
            if ((current & 0x80U) == 0) {
                const auto canonical = encode_varint(value);
                if (canonical.size() != position_ - start) {
                    throw MoneroParseError("noncanonical varint");
                }
                return value;
            }
            shift += 7;
        }
        throw MoneroParseError("unterminated varint");
    }

    [[nodiscard]] std::size_t count(std::size_t element_minimum = 1)
    {
        const std::uint64_t count_value = varint();
        if (count_value > remaining() / element_minimum) {
            throw MoneroParseError("container count exceeds remaining block bytes");
        }
        return static_cast<std::size_t>(count_value);
    }

private:
    void require(std::size_t count)
    {
        if (count > remaining()) throw MoneroParseError("truncated Monero blob");
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_{};
};

void parse_vector_bytes(Reader &reader)
{
    const std::size_t size = reader.count();
    reader.skip(size);
}

void parse_public_key_vector(Reader &reader)
{
    const std::size_t count = reader.count(32);
    reader.skip(count * 32);
}

void parse_txout_to_script(Reader &reader)
{
    parse_public_key_vector(reader);
    parse_vector_bytes(reader);
}

void parse_input(Reader &reader, std::uint64_t &coinbase_height,
                 bool &coinbase, std::size_t &signature_count)
{
    const std::uint8_t tag = reader.byte();
    switch (tag) {
    case 0xff:
        coinbase_height = reader.varint();
        coinbase = true;
        signature_count = 0;
        return;
    case 0x00:
        reader.skip(32);
        (void)reader.varint();
        parse_vector_bytes(reader);
        signature_count = 0;
        return;
    case 0x01:
        reader.skip(32);
        (void)reader.varint();
        parse_txout_to_script(reader);
        parse_vector_bytes(reader);
        signature_count = 0;
        return;
    case 0x02: {
        (void)reader.varint();
        const std::size_t offsets = reader.count();
        signature_count = offsets;
        for (std::size_t i = 0; i < offsets; ++i) (void)reader.varint();
        reader.skip(32);
        return;
    }
    default: throw MoneroParseError("unsupported transaction input variant");
    }
}

void parse_output(Reader &reader)
{
    (void)reader.varint();
    switch (reader.byte()) {
    case 0x00: parse_txout_to_script(reader); break;
    case 0x01: reader.skip(32); break;
    case 0x02: reader.skip(32); break;
    case 0x03: reader.skip(33); break;
    default: throw MoneroParseError("unsupported transaction output variant");
    }
}

// A block template's miner transaction is always coinbase. Its v2 RingCT type
// is Null and therefore consists of exactly the one-byte type after the prefix.
ParsedMinerTransaction parse_miner_transaction(Reader &reader)
{
    ParsedMinerTransaction tx;
    tx.begin_offset = reader.position();
    tx.version = reader.varint();
    if (tx.version == 0 || tx.version > 2) {
        throw MoneroParseError("unsupported miner transaction version");
    }
    (void)reader.varint(); // unlock time
    tx.input_count = reader.varint();
    if (tx.input_count == 0 || tx.input_count > reader.remaining()) {
        throw MoneroParseError("invalid miner transaction input count");
    }
    std::vector<std::size_t> signature_counts;
    signature_counts.reserve(static_cast<std::size_t>(tx.input_count));
    for (std::uint64_t i = 0; i < tx.input_count; ++i) {
        std::size_t signatures = 0;
        bool coinbase = false;
        std::uint64_t height = 0;
        parse_input(reader, height, coinbase, signatures);
        signature_counts.push_back(signatures);
        if (i == 0) {
            tx.is_coinbase = coinbase;
            tx.height = height;
        }
    }
    if (!tx.is_coinbase || tx.input_count != 1) {
        throw MoneroParseError("miner transaction must have exactly one generation input");
    }
    tx.output_count = reader.varint();
    if (tx.output_count > reader.remaining()) {
        throw MoneroParseError("invalid miner transaction output count");
    }
    for (std::uint64_t i = 0; i < tx.output_count; ++i) parse_output(reader);
    tx.extra_size = reader.count();
    tx.extra_begin_offset = reader.position();
    reader.skip(tx.extra_size);
    tx.prefix_end_offset = reader.position();

    if (tx.version == 1) {
        for (const std::size_t count : signature_counts) {
            if (count > reader.remaining() / 64) {
                throw MoneroParseError("truncated transaction signatures");
            }
            reader.skip(count * 64);
        }
        tx.unprunable_end_offset = reader.position();
    } else {
        const std::uint8_t rct_type = reader.byte();
        if (rct_type != 0) {
            throw MoneroParseError(
                "unsupported miner transaction with non-null RingCT data");
        }
        tx.unprunable_end_offset = reader.position();
    }
    tx.end_offset = reader.position();
    return tx;
}

[[nodiscard]] Hash32 tree_hash(std::span<const Hash32> hashes)
{
    if (hashes.empty()) throw MoneroParseError("transaction tree cannot be empty");
    if (hashes.size() == 1) return hashes.front();
    const auto hash_pair = [](const Hash32 &left, const Hash32 &right) {
        std::array<std::uint8_t, 64> material{};
        std::copy(left.begin(), left.end(), material.begin());
        std::copy(right.begin(), right.end(), material.begin() + 32);
        return keccak256(material);
    };
    if (hashes.size() == 2) return hash_pair(hashes[0], hashes[1]);
    std::size_t count = 2;
    while (count < hashes.size()) count <<= 1U;
    count >>= 1U;
    std::vector<Hash32> intermediates(count);
    const std::size_t copied = 2 * count - hashes.size();
    std::copy_n(hashes.begin(), copied, intermediates.begin());
    std::size_t source = copied;
    for (std::size_t destination = copied; destination < count; ++destination) {
        intermediates[destination] = hash_pair(hashes[source], hashes[source + 1]);
        source += 2;
    }
    while (count > 2) {
        count >>= 1U;
        for (std::size_t i = 0; i < count; ++i) {
            intermediates[i] = hash_pair(intermediates[2 * i],
                                         intermediates[2 * i + 1]);
        }
    }
    return hash_pair(intermediates[0], intermediates[1]);
}

using Limbs128 = std::array<std::uint32_t, 4>;

[[nodiscard]] Limbs128 parse_decimal_uint128(std::string_view text)
{
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        throw ValidationError("difficulty must be canonical unsigned decimal");
    }
    Limbs128 result{};
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw ValidationError("difficulty must be canonical unsigned decimal");
        }
        std::uint64_t carry = static_cast<unsigned>(ch - '0');
        for (std::uint32_t &limb : result) {
            const std::uint64_t value = static_cast<std::uint64_t>(limb) * 10 + carry;
            limb = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        if (carry != 0) {
            throw ValidationError("network difficulty exceeds 2^128-1");
        }
    }
    if (std::all_of(result.begin(), result.end(),
                    [](std::uint32_t limb) { return limb == 0; }))
        throw ValidationError("network difficulty must be positive");
    return result;
}

} // namespace

PrimaryAddress decode_primary_address(std::string_view address,
                                      Network expected_network)
{
    const Bytes decoded = base58_decode(address);
    // One-byte v1 network prefixes plus two public keys and four checksum bytes.
    if (decoded.size() != 69) {
        throw ValidationError("wallet_address is not a primary Monero address");
    }
    const std::span payload(decoded.data(), decoded.size() - 4);
    const Hash32 digest = keccak256(payload);
    if (!constant_time_equal(std::span(decoded).last(4),
                             std::span(digest).first(4))) {
        throw ValidationError("wallet_address checksum is invalid");
    }
    Reader reader(payload);
    const std::uint64_t prefix = reader.varint();
    if (prefix != prefix_for(expected_network)) {
        throw ValidationError("wallet_address network or address type does not match");
    }
    PrimaryAddress result;
    result.network = expected_network;
    result.prefix = prefix;
    const auto spend = reader.bytes(32);
    const auto view = reader.bytes(32);
    if (!reader.eof()) throw ValidationError("wallet_address payload has trailing data");
    std::copy(spend.begin(), spend.end(), result.spend_public_key.begin());
    std::copy(view.begin(), view.end(), result.view_public_key.begin());
    return result;
}

void validate_primary_address(std::string_view address, Network expected_network)
{
    (void)decode_primary_address(address, expected_network);
}

std::uint64_t share_target64(std::uint64_t difficulty)
{
    if (difficulty == 0) throw ValidationError("share difficulty must be positive");
    return std::numeric_limits<std::uint64_t>::max() / difficulty;
}

std::array<std::uint8_t, 8> share_target_le(std::uint64_t difficulty)
{
    std::array<std::uint8_t, 8> result{};
    std::uint64_t target = share_target64(difficulty);
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(target >> (8U * i));
    }
    return result;
}

std::string share_target_hex(std::uint64_t difficulty)
{
    return hex_encode(share_target_le(difficulty));
}

std::uint64_t pow_word64(const Hash32 &hash) noexcept
{
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(hash[24 + i]) << (8U * i);
    }
    return result;
}

bool meets_share_target(const Hash32 &hash, std::uint64_t target64) noexcept
{
    return pow_word64(hash) < target64;
}

std::uint64_t actual_difficulty(const Hash32 &hash) noexcept
{
    const std::uint64_t word = pow_word64(hash);
    return word == 0 ? std::numeric_limits<std::uint64_t>::max()
                     : std::numeric_limits<std::uint64_t>::max() / word;
}

void validate_network_difficulty(std::string_view difficulty_decimal)
{
    (void)parse_decimal_uint128(difficulty_decimal);
}

bool meets_network_target(const Hash32 &hash, std::string_view difficulty_decimal)
{
    std::array<std::uint32_t, 8> hash_limbs{};
    for (std::size_t i = 0; i < hash_limbs.size(); ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            hash_limbs[i] |= static_cast<std::uint32_t>(hash[i * 4 + j]) << (8U * j);
        }
    }
    const Limbs128 difficulty = parse_decimal_uint128(difficulty_decimal);
    std::array<std::uint32_t, 12> product{};
    for (std::size_t i = 0; i < hash_limbs.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < difficulty.size(); ++j) {
            const std::uint64_t value =
                static_cast<std::uint64_t>(hash_limbs[i]) * difficulty[j] +
                product[i + j] + carry;
            product[i + j] = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        std::size_t position = i + difficulty.size();
        while (carry != 0 && position < product.size()) {
            const std::uint64_t value = product[position] + carry;
            product[position] = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
            ++position;
        }
    }
    return std::all_of(product.begin() + 8, product.end(),
                       [](std::uint32_t limb) { return limb == 0; });
}

std::vector<std::uint8_t> encode_varint(std::uint64_t value)
{
    std::vector<std::uint8_t> result;
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) byte |= 0x80U;
        result.push_back(byte);
    } while (value != 0);
    return result;
}

ParsedBlock parse_block(std::span<const std::uint8_t> blob)
{
    Reader reader(blob);
    ParsedBlock result;
    result.blob.assign(blob.begin(), blob.end());
    result.major_version = reader.varint();
    result.minor_version = reader.varint();
    result.timestamp = reader.varint();
    if (result.major_version == 0 || result.major_version > 255 ||
        result.minor_version > 255) {
        throw MoneroParseError("unsupported block version");
    }
    const auto previous = reader.bytes(32);
    std::copy(previous.begin(), previous.end(), result.previous_hash.begin());
    result.nonce_offset = reader.position();
    reader.skip(4);
    result.miner_transaction = parse_miner_transaction(reader);
    const std::size_t tx_count = reader.count(32);
    result.transaction_hashes.resize(tx_count);
    for (Hash32 &hash : result.transaction_hashes) {
        const auto bytes = reader.bytes(32);
        std::copy(bytes.begin(), bytes.end(), hash.begin());
    }
    if (!reader.eof()) throw MoneroParseError("block has trailing bytes");
    return result;
}

Hash32 miner_transaction_hash(const ParsedBlock &block)
{
    const auto &tx = block.miner_transaction;
    if (tx.version == 1) {
        return keccak256(std::span(block.blob).subspan(
            tx.begin_offset, tx.end_offset - tx.begin_offset));
    }
    const Hash32 prefix = keccak256(std::span(block.blob).subspan(
        tx.begin_offset, tx.prefix_end_offset - tx.begin_offset));
    const Hash32 base = keccak256(std::span(block.blob).subspan(
        tx.prefix_end_offset, tx.unprunable_end_offset - tx.prefix_end_offset));
    const Hash32 null_hash{};
    std::array<std::uint8_t, 96> material{};
    std::copy(prefix.begin(), prefix.end(), material.begin());
    std::copy(base.begin(), base.end(), material.begin() + 32);
    std::copy(null_hash.begin(), null_hash.end(), material.begin() + 64);
    return keccak256(material);
}

Hash32 transaction_tree_hash(const ParsedBlock &block)
{
    std::vector<Hash32> hashes;
    hashes.reserve(block.transaction_hashes.size() + 1);
    hashes.push_back(miner_transaction_hash(block));
    hashes.insert(hashes.end(), block.transaction_hashes.begin(),
                  block.transaction_hashes.end());
    return tree_hash(hashes);
}

Bytes block_hashing_blob(const ParsedBlock &block)
{
    const std::size_t header_end = block.nonce_offset + 4;
    if (header_end > block.blob.size()) throw MoneroParseError("invalid block header range");
    Bytes result(block.blob.begin(),
                 block.blob.begin() + static_cast<std::ptrdiff_t>(header_end));
    const Hash32 root = transaction_tree_hash(block);
    result.insert(result.end(), root.begin(), root.end());
    const auto count = encode_varint(block.transaction_hashes.size() + 1);
    result.insert(result.end(), count.begin(), count.end());
    return result;
}

Hash32 block_hash(const ParsedBlock &block)
{
    const Hash32 result = keccak256(block_hashing_blob(block));
    if (block.miner_transaction.height != 202612) return result;

    // Preserve Monero's historical height-202612 tree-hash exception exactly.
    static const Hash32 correct_blob_hash = hex_decode_array<32>(
        "3a8a2b3a29b50fc86ff73dd087ea43c6"
        "f0d6b8f936c849194d5c84c737903966");
    static const Hash32 existing_block_id = hex_decode_array<32>(
        "bbd604d2ba11ba27935e006ed39c9bfdd"
        "99b76bf4a50654bc1e1e61217962698");
    if (keccak256(block.blob) == correct_blob_hash) return existing_block_id;
    if (result == existing_block_id) {
        throw MoneroParseError(
            "nonhistorical block collides with the height-202612 exception ID");
    }
    return result;
}

ParsedBlock mutate_reserved_bytes(const ParsedBlock &block,
                                  std::size_t reserved_offset,
                                  std::span<const std::uint8_t> replacement)
{
    if (replacement.size() != 16 ||
        !reserved_offset_is_exact_extra_nonce(block, reserved_offset,
                                               replacement.size())) {
        throw MoneroParseError("reserved mutation is not the exact 16-byte extra nonce");
    }
    Bytes mutated = block.blob;
    std::copy(replacement.begin(), replacement.end(),
              mutated.begin() + static_cast<std::ptrdiff_t>(reserved_offset));
    return parse_block(mutated);
}

ParsedBlock insert_nonce(const ParsedBlock &block, const Nonce4 &nonce)
{
    if (block.nonce_offset + nonce.size() > block.blob.size()) {
        throw MoneroParseError("nonce offset is outside block");
    }
    Bytes mutated = block.blob;
    std::copy(nonce.begin(), nonce.end(),
              mutated.begin() + static_cast<std::ptrdiff_t>(block.nonce_offset));
    return parse_block(mutated);
}

bool reserved_offset_is_exact_extra_nonce(const ParsedBlock &block,
                                          std::size_t reserved_offset,
                                          std::size_t reserve_size)
{
    if (reserve_size != 16) return false;
    const auto &tx = block.miner_transaction;
    if (tx.extra_begin_offset > block.blob.size() ||
        tx.extra_size > block.blob.size() - tx.extra_begin_offset ||
        reserved_offset > block.blob.size() ||
        reserve_size > block.blob.size() - reserved_offset) {
        return false;
    }
    Reader extra(std::span(block.blob).subspan(tx.extra_begin_offset,
                                               tx.extra_size));
    bool found = false;
    try {
        while (!extra.eof()) {
            // Binary-archive variant tags are varints.  All common tags fit in
            // one byte, but the historical 0xde tag canonically occupies two.
            const std::uint64_t tag = extra.varint();
            if (tag == 0x00) {
                // Monero's tx_extra_padding consumes the rest of the extra,
                // requires every byte to be zero, and caps total padding at
                // 255 bytes including its tag.
                std::size_t padding_size = 1;
                while (!extra.eof()) {
                    if (extra.byte() != 0 || ++padding_size > 255) return false;
                }
            } else if (tag == 0x01) {
                extra.skip(32);
            } else if (tag == 0x02) {
                const std::uint64_t size = extra.varint();
                if (size > extra.remaining()) return false;
                const std::size_t payload_offset =
                    tx.extra_begin_offset + extra.position();
                if (size == reserve_size && payload_offset == reserved_offset) {
                    found = true;
                }
                extra.skip(static_cast<std::size_t>(size));
            } else if (tag == 0x03 || tag == 0xde) {
                const std::uint64_t size = extra.varint();
                if (size > extra.remaining()) return false;
                extra.skip(static_cast<std::size_t>(size));
            } else if (tag == 0x04) {
                const std::uint64_t count = extra.varint();
                if (count > extra.remaining() / 32) return false;
                extra.skip(static_cast<std::size_t>(count) * 32);
            } else {
                return false;
            }
        }
    } catch (const MoneroParseError &) {
        return false;
    }
    return found;
}

} // namespace monero_solo
