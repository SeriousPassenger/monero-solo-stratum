#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace monero_solo {

using Bytes = std::vector<std::uint8_t>;
using Hash32 = std::array<std::uint8_t, 32>;
using Id16 = std::array<std::uint8_t, 16>;
using Nonce4 = std::array<std::uint8_t, 4>;
using DuplicateKey = std::array<std::uint8_t, 48>;

enum class Network {
    mainnet,
    testnet,
    stagenet,
    regtest,
};

class ValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class EntropyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MoneroParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] const char *network_name(Network network) noexcept;
[[nodiscard]] Network parse_network(const std::string &value);

} // namespace monero_solo
