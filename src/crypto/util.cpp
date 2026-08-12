#include "monero_solo/util.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>

namespace monero_solo {
namespace {

constexpr char kCandidateDomainData[] =
    "monero-solo-stratum/candidate/v1\0";
constexpr std::string_view kCandidateDomain{kCandidateDomainData,
                                             sizeof(kCandidateDomainData) - 1};

template <typename T, void (*Free)(T *)>
using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

[[nodiscard]] bool is_hex_char(char ch) noexcept
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

[[nodiscard]] std::uint8_t hex_value(char ch) noexcept
{
    if (ch >= '0' && ch <= '9') {
        return static_cast<std::uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<std::uint8_t>(ch - 'a' + 10);
    }
    return static_cast<std::uint8_t>(ch - 'A' + 10);
}

[[nodiscard]] bool is_ascii_space(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '\f' || ch == '\v';
}

[[nodiscard]] bool path_is_absolute_and_bounded(const std::string &path)
{
    return !path.empty() && path.size() <= 4096 && path.front() == '/';
}

void throw_path_error(const std::string &kind, const std::string &reason)
{
    throw ValidationError(kind + " path is invalid: " + reason);
}

// Keccak-f[1600], from the public-domain permutation specification. Monero's
// cn_fast_hash uses legacy Keccak padding (0x01), not SHA3 padding (0x06).
constexpr std::array<std::uint64_t, 24> kRoundConstants = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

constexpr std::array<unsigned, 24> kRotation = {
    1,  3,  6,  10, 15, 21, 28, 36,
    45, 55, 2,  14, 27, 41, 56, 8,
    25, 43, 62, 18, 39, 61, 20, 44,
};

constexpr std::array<unsigned, 24> kLane = {
    10, 7,  11, 17, 18, 3,  5,  16,
    8,  21, 24, 4,  15, 23, 19, 13,
    12, 2,  20, 14, 22, 9,  6,  1,
};

[[nodiscard]] constexpr std::uint64_t rotate_left(std::uint64_t value,
                                                  unsigned count) noexcept
{
    return (value << count) | (value >> (64U - count));
}

void keccak_f1600(std::array<std::uint64_t, 25> &state) noexcept
{
    for (const std::uint64_t round_constant : kRoundConstants) {
        std::array<std::uint64_t, 5> column{};
        for (std::size_t i = 0; i < 5; ++i) {
            column[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^
                        state[i + 15] ^ state[i + 20];
        }
        for (std::size_t i = 0; i < 5; ++i) {
            const std::uint64_t delta =
                column[(i + 4) % 5] ^ rotate_left(column[(i + 1) % 5], 1);
            for (std::size_t j = 0; j < 25; j += 5) {
                state[j + i] ^= delta;
            }
        }

        std::uint64_t current = state[1];
        for (std::size_t i = 0; i < 24; ++i) {
            const unsigned destination = kLane[i];
            const std::uint64_t displaced = state[destination];
            state[destination] = rotate_left(current, kRotation[i]);
            current = displaced;
        }

        for (std::size_t row = 0; row < 25; row += 5) {
            std::array<std::uint64_t, 5> original{};
            std::copy_n(state.begin() + static_cast<std::ptrdiff_t>(row), 5,
                        original.begin());
            for (std::size_t i = 0; i < 5; ++i) {
                state[row + i] =
                    original[i] ^ ((~original[(i + 1) % 5]) &
                                   original[(i + 2) % 5]);
            }
        }
        state[0] ^= round_constant;
    }
}

[[nodiscard]] std::uint64_t load64_le(const std::uint8_t *input) noexcept
{
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(input[i]) << (8U * i);
    }
    return result;
}

void store64_le(std::uint8_t *output, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i < 8; ++i) {
        output[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

} // namespace

const char *network_name(Network network) noexcept
{
    switch (network) {
    case Network::mainnet: return "mainnet";
    case Network::testnet: return "testnet";
    case Network::stagenet: return "stagenet";
    case Network::regtest: return "regtest";
    }
    return "unknown";
}

Network parse_network(const std::string &value)
{
    if (value == "mainnet") return Network::mainnet;
    if (value == "testnet") return Network::testnet;
    if (value == "stagenet") return Network::stagenet;
    if (value == "regtest") return Network::regtest;
    throw ValidationError("network must be mainnet, testnet, stagenet, or regtest");
}

std::string Endpoint::canonical() const
{
    return ipv6_literal ? "[" + host + "]:" + std::to_string(port)
                        : host + ":" + std::to_string(port);
}

bool Endpoint::is_definitely_loopback() const noexcept
{
    if (host == "localhost") return true;
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        return (ntohl(v4.s_addr) >> 24U) == 127U;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        return IN6_IS_ADDR_LOOPBACK(&v6) != 0;
    }
    return false;
}

Endpoint parse_endpoint(std::string_view text)
{
    if (text.empty() || text.size() > 4096) {
        throw ValidationError("endpoint must be 1..4096 bytes");
    }
    Endpoint result;
    std::string_view port_text;
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close == 1 ||
            close + 2 > text.size() || text[close + 1] != ':') {
            throw ValidationError("invalid bracketed IPv6 endpoint");
        }
        result.host.assign(text.substr(1, close - 1));
        port_text = text.substr(close + 2);
        in6_addr address{};
        if (inet_pton(AF_INET6, result.host.c_str(), &address) != 1) {
            throw ValidationError("bracketed endpoint is not a valid IPv6 literal");
        }
        char canonical[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, &address, canonical, sizeof(canonical)) == nullptr) {
            throw ValidationError("could not canonicalize IPv6 endpoint");
        }
        result.host = canonical;
        result.ipv6_literal = true;
        result.ip_literal = true;
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos || colon == 0 ||
            text.find(':') != colon) {
            throw ValidationError("endpoint must be hostname:port or [IPv6]:port");
        }
        result.host.assign(text.substr(0, colon));
        port_text = text.substr(colon + 1);
        if (result.host.size() > 253 || result.host.front() == '.' ||
            result.host.back() == '.') {
            throw ValidationError("invalid endpoint hostname");
        }
        in_addr address{};
        if (inet_pton(AF_INET, result.host.c_str(), &address) == 1) {
            char canonical[INET_ADDRSTRLEN]{};
            (void)inet_ntop(AF_INET, &address, canonical, sizeof(canonical));
            result.host = canonical;
            result.ip_literal = true;
        } else {
            const bool numeric_dotted = std::all_of(
                result.host.begin(), result.host.end(),
                [](char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; });
            if (numeric_dotted) {
                throw ValidationError("endpoint contains an invalid IPv4 literal");
            }
            for (const char ch : result.host) {
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')) {
                    throw ValidationError("endpoint hostname has invalid characters");
                }
            }
            std::size_t label_begin = 0;
            while (label_begin < result.host.size()) {
                const std::size_t dot = result.host.find('.', label_begin);
                const std::size_t label_end =
                    dot == std::string::npos ? result.host.size() : dot;
                const std::size_t label_size = label_end - label_begin;
                if (label_size == 0 || label_size > 63 ||
                    result.host[label_begin] == '-' ||
                    result.host[label_end - 1] == '-') {
                    throw ValidationError("endpoint hostname label is invalid");
                }
                label_begin = label_end + 1;
            }
            std::transform(result.host.begin(), result.host.end(), result.host.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
        }
    }
    if (port_text.empty() || port_text.size() > 5 || port_text.front() == '+' ||
        (port_text.size() > 1 && port_text.front() == '0')) {
        throw ValidationError("endpoint port must be canonical decimal 1..65535");
    }
    unsigned port = 0;
    const auto parsed = std::from_chars(port_text.data(),
                                        port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
        throw ValidationError("endpoint port must be 1..65535");
    }
    result.port = static_cast<std::uint16_t>(port);
    return result;
}

Bytes hex_decode(std::string_view text)
{
    if ((text.size() & 1U) != 0U) {
        throw ValidationError("hex text must contain an even number of characters");
    }
    Bytes result(text.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const char high = text[i * 2];
        const char low = text[i * 2 + 1];
        if (!is_hex_char(high) || !is_hex_char(low)) {
            throw ValidationError("hex text contains a non-hexadecimal character");
        }
        result[i] = static_cast<std::uint8_t>((hex_value(high) << 4U) |
                                              hex_value(low));
    }
    return result;
}

Bytes hex_decode_exact(std::string_view text, std::size_t bytes)
{
    if (bytes > std::numeric_limits<std::size_t>::max() / 2 ||
        text.size() != bytes * 2) {
        throw ValidationError("hex text has an unexpected length");
    }
    return hex_decode(text);
}

std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[2 * i] = kHex[bytes[i] >> 4U];
        result[2 * i + 1] = kHex[bytes[i] & 0x0fU];
    }
    return result;
}

bool constant_time_equal(std::span<const std::uint8_t> left,
                         std::span<const std::uint8_t> right) noexcept
{
    if (left.size() != right.size()) return false;
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept
{
    return constant_time_equal(
        {reinterpret_cast<const std::uint8_t *>(left.data()), left.size()},
        {reinterpret_cast<const std::uint8_t *>(right.data()), right.size()});
}

Hash32 sha256(std::span<const std::uint8_t> input)
{
    Hash32 result{};
    unsigned result_size = 0;
    OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(),
                                                   EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), result.data(), &result_size) != 1 ||
        result_size != result.size()) {
        throw std::runtime_error("OpenSSL SHA-256 failed");
    }
    return result;
}

Hash32 hmac_sha256(std::span<const std::uint8_t> key,
                   std::span<const std::uint8_t> input)
{
    Hash32 result{};
    unsigned result_size = 0;
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), input.data(),
             input.size(), result.data(), &result_size) == nullptr ||
        result_size != result.size()) {
        throw std::runtime_error("OpenSSL HMAC-SHA-256 failed");
    }
    return result;
}

Hash32 keccak256(std::span<const std::uint8_t> input) noexcept
{
    constexpr std::size_t rate = 136;
    std::array<std::uint64_t, 25> state{};
    while (input.size() >= rate) {
        for (std::size_t i = 0; i < rate / 8; ++i) {
            state[i] ^= load64_le(input.data() + i * 8);
        }
        keccak_f1600(state);
        input = input.subspan(rate);
    }
    std::array<std::uint8_t, rate> tail{};
    std::copy(input.begin(), input.end(), tail.begin());
    tail[input.size()] = 0x01;
    tail[rate - 1] |= 0x80;
    for (std::size_t i = 0; i < rate / 8; ++i) {
        state[i] ^= load64_le(tail.data() + i * 8);
    }
    keccak_f1600(state);
    Hash32 result{};
    for (std::size_t i = 0; i < result.size() / 8; ++i) {
        store64_le(result.data() + i * 8, state[i]);
    }
    return result;
}

DuplicateKey make_duplicate_key(const Id16 &private_entropy,
                                const Hash32 &pow_hash) noexcept
{
    DuplicateKey result{};
    std::copy(private_entropy.begin(), private_entropy.end(), result.begin());
    std::copy(pow_hash.begin(), pow_hash.end(), result.begin() + 16);
    return result;
}

Hash32 make_candidate_key(std::span<const std::uint8_t> frozen_full_block)
{
    Bytes material;
    material.reserve(kCandidateDomain.size() + frozen_full_block.size());
    material.insert(material.end(), kCandidateDomain.begin(), kCandidateDomain.end());
    material.insert(material.end(), frozen_full_block.begin(), frozen_full_block.end());
    return sha256(material);
}

void os_random_exact(std::span<std::uint8_t> output, bool nonblocking)
{
    std::size_t completed = 0;
    const unsigned flags = nonblocking ? GRND_NONBLOCK : 0;
    while (completed < output.size()) {
        const ssize_t count =
            getrandom(output.data() + completed, output.size() - completed, flags);
        if (count > 0) {
            completed += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        throw EntropyError(std::string("getrandom failed: ") + std::strerror(errno));
    }
}

Id16 random_id16()
{
    Id16 result{};
    os_random_exact(result);
    return result;
}

std::string format_rfc3339_utc_us(std::int64_t unix_microseconds)
{
    std::int64_t seconds = unix_microseconds / 1000000;
    std::int64_t micros = unix_microseconds % 1000000;
    if (micros < 0) {
        micros += 1000000;
        --seconds;
    }
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm tm{};
    if (gmtime_r(&value, &tm) == nullptr) {
        throw ValidationError("timestamp is outside the supported UTC range");
    }
    char base[32]{};
    if (std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm) == 0) {
        throw ValidationError("could not format timestamp");
    }
    std::ostringstream result;
    result << base << '.' << std::setfill('0') << std::setw(6) << micros << 'Z';
    return result.str();
}

std::int64_t unix_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<std::string> parse_blocknotify_argv(std::string_view command_template,
                                                bool validate_executable)
{
    if (command_template.empty()) return {};
    enum class Quote { none, single, double_quote };
    Quote quote = Quote::none;
    bool escaped = false;
    bool token_started = false;
    std::string token;
    std::vector<std::string> argv;
    const auto finish = [&]() {
        if (token_started) {
            argv.push_back(token);
            token.clear();
            token_started = false;
        }
    };
    for (const char ch : command_template) {
        if (escaped) {
            if (quote == Quote::double_quote && ch != '"' && ch != '\\') {
                throw ValidationError(
                    "inside blocknotify double quotes, backslash may escape only quote or backslash");
            }
            token.push_back(ch);
            escaped = false;
            token_started = true;
            continue;
        }
        if (ch == '\\' && quote != Quote::single) {
            escaped = true;
            token_started = true;
            continue;
        }
        if (quote == Quote::single) {
            if (ch == '\'') quote = Quote::none;
            else token.push_back(ch);
            token_started = true;
            continue;
        }
        if (quote == Quote::double_quote) {
            if (ch == '"') quote = Quote::none;
            else token.push_back(ch);
            token_started = true;
            continue;
        }
        if (ch == '\'') {
            quote = Quote::single;
            token_started = true;
        } else if (ch == '"') {
            quote = Quote::double_quote;
            token_started = true;
        } else if (is_ascii_space(ch)) {
            finish();
        } else {
            token.push_back(ch);
            token_started = true;
        }
    }
    if (escaped || quote != Quote::none) {
        throw ValidationError("blocknotify contains an unmatched quote or terminal backslash");
    }
    finish();
    if (argv.empty() || argv.front().empty()) {
        throw ValidationError("blocknotify must contain a nonempty argv[0]");
    }
    bool placeholder = false;
    for (const auto &argument : argv) {
        if (argument.find('\0') != std::string::npos) {
            throw ValidationError("blocknotify argument contains NUL");
        }
        placeholder = placeholder || argument.find("%s") != std::string::npos;
    }
    if (!placeholder) {
        throw ValidationError("blocknotify must contain at least one literal %s");
    }
    if (argv.front().front() != '/') {
        throw ValidationError("blocknotify argv[0] must be absolute");
    }
    if (validate_executable) {
        struct stat status{};
        if (lstat(argv.front().c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
            S_ISLNK(status.st_mode) ||
            access(argv.front().c_str(), X_OK) != 0) {
            throw ValidationError("blocknotify argv[0] must name a regular executable");
        }
    }
    return argv;
}

std::vector<std::string> expand_blocknotify_argv(
    const std::vector<std::string> &template_argv,
    const Hash32 &miner_tx_hash)
{
    const std::string replacement = hex_encode(miner_tx_hash);
    std::vector<std::string> result = template_argv;
    for (std::string &argument : result) {
        std::size_t position = 0;
        while ((position = argument.find("%s", position)) != std::string::npos) {
            argument.replace(position, 2, replacement);
            position += replacement.size();
        }
    }
    return result;
}

bool path_parent_is_safe(const std::string &absolute_path, std::string *reason)
{
    if (!path_is_absolute_and_bounded(absolute_path)) {
        if (reason) *reason = "must be a nonempty absolute path of at most 4096 bytes";
        return false;
    }
    const std::filesystem::path parent = std::filesystem::path(absolute_path).parent_path();
    struct stat status{};
    if (lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        if (reason) *reason = "parent does not exist or is not a directory";
        return false;
    }
    if ((status.st_mode & S_IWOTH) != 0 &&
        ((status.st_mode & S_ISVTX) == 0 || status.st_uid != 0)) {
        if (reason) *reason = "parent is an unsafe world-writable directory";
        return false;
    }
    return true;
}

void validate_database_path(const std::string &absolute_path)
{
    std::string reason;
    if (!path_parent_is_safe(absolute_path, &reason)) {
        throw_path_error("database", reason);
    }
    struct stat status{};
    if (lstat(absolute_path.c_str(), &status) == 0 && S_ISLNK(status.st_mode)) {
        throw_path_error("database", "database must not be a symlink");
    }
}

void validate_log_path(const std::string &absolute_path)
{
    if (absolute_path.empty()) return;
    std::string reason;
    if (!path_parent_is_safe(absolute_path, &reason)) throw_path_error("log", reason);
    struct stat status{};
    if (lstat(absolute_path.c_str(), &status) == 0 && S_ISLNK(status.st_mode)) {
        throw_path_error("log", "log file must not be a symlink");
    }
}

void validate_unix_socket_path(const std::string &absolute_path)
{
    std::string reason;
    if (!path_parent_is_safe(absolute_path, &reason)) {
        throw_path_error("event socket", reason);
    }
    struct stat status{};
    if (lstat(absolute_path.c_str(), &status) == 0 && !S_ISSOCK(status.st_mode)) {
        throw_path_error("event socket", "existing path is not a Unix socket");
    }
}

} // namespace monero_solo
