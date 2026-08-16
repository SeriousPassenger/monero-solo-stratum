/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/api.hpp"

#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <openssl/bn.h>
#include <sqlite3.h>

namespace monero_solo {
namespace {

using Json = nlohmann::json;
using QueryMap = std::map<std::string, std::string, std::less<>>;

constexpr std::size_t kDefaultPageSize = 100;
constexpr std::size_t kMaximumQueryBytes = 16U * 1024U;
constexpr std::size_t kMaximumEncodedBlobBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumResponseBytes = 32U * 1024U * 1024U;
constexpr std::size_t kCursorBytes = 27;

class ApiRequestError final : public std::runtime_error {
public:
    ApiRequestError(const int response_status,
                    std::string error_code,
                    std::string message)
        : std::runtime_error(std::move(message)),
          status(response_status),
          code(std::move(error_code))
    {
    }

    int status;
    std::string code;
};

std::string percent_decode(const std::string_view input)
{
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const char byte = input[index];
        if (byte == '%') {
            if (index + 2U >= input.size()) {
                throw ApiRequestError(400, "invalid_query",
                                      "Query contains invalid percent encoding");
            }
            const auto value = [](const char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = value(input[index + 1U]);
            const int low = value(input[index + 2U]);
            if (high < 0 || low < 0) {
                throw ApiRequestError(400, "invalid_query",
                                      "Query contains invalid percent encoding");
            }
            output.push_back(static_cast<char>((high << 4) | low));
            index += 2U;
        }
        else if (byte == '+') {
            output.push_back(' ');
        }
        else {
            output.push_back(byte);
        }
    }
    if (output.find('\0') != std::string::npos) {
        throw ApiRequestError(400, "invalid_query", "Query contains NUL");
    }
    return output;
}

QueryMap parse_query(const std::string_view encoded)
{
    if (encoded.size() > kMaximumQueryBytes) {
        throw ApiRequestError(400, "invalid_query", "Query is too long");
    }
    QueryMap result;
    if (encoded.empty()) {
        return result;
    }
    std::size_t offset = 0;
    while (offset <= encoded.size()) {
        const std::size_t ampersand = encoded.find('&', offset);
        const std::string_view pair = encoded.substr(
            offset, ampersand == std::string_view::npos ? encoded.size() - offset :
                                                          ampersand - offset);
        const std::size_t equals = pair.find('=');
        if (pair.empty() || equals == std::string_view::npos || equals == 0) {
            throw ApiRequestError(400, "invalid_query", "Malformed query parameter");
        }
        const std::string name = percent_decode(pair.substr(0, equals));
        const std::string value = percent_decode(pair.substr(equals + 1U));
        if (name.empty() || value.empty() ||
            !std::all_of(name.begin(), name.end(), [](const unsigned char byte) {
                return std::isalnum(byte) != 0 || byte == '_';
            })) {
            throw ApiRequestError(400, "invalid_query", "Invalid query parameter");
        }
        if (!result.emplace(name, value).second) {
            throw ApiRequestError(400, "invalid_query", "Duplicate query parameter");
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        offset = ampersand + 1U;
    }
    return result;
}

bool canonical_unsigned(const std::string_view text,
                        const bool allow_zero = false,
                        std::uint64_t *const parsed = nullptr)
{
    if (text.empty() || text.front() == '+' || text.front() == '-' ||
        (text.size() > 1U && text.front() == '0')) {
        return false;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        (!allow_zero && value == 0)) {
        return false;
    }
    if (parsed != nullptr) {
        *parsed = value;
    }
    return true;
}

bool decimal_string(const std::string_view text, const bool allow_zero = true)
{
    return !text.empty() && (allow_zero || text != "0") &&
           (text.size() == 1U || text.front() != '0') &&
           std::all_of(text.begin(), text.end(), [](const unsigned char byte) {
               return std::isdigit(byte) != 0;
           });
}

bool lowercase_hex(const std::string_view text, const std::size_t size)
{
    return text.size() == size &&
           std::all_of(text.begin(), text.end(), [](const char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

bool valid_timestamp(const std::string_view value)
{
    /* API filters use the same exact UTC/microsecond representation as output. */
    if (value.size() != 27U || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != '.' || value[26] != 'Z') {
        return false;
    }
    for (const std::size_t index :
         {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U,
          18U, 20U, 21U, 22U, 23U, 24U, 25U}) {
        if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    const auto number = [&](const std::size_t offset,
                            const std::size_t length) -> unsigned {
        unsigned result = 0;
        const auto parsed = std::from_chars(value.data() + offset,
                                            value.data() + offset + length,
                                            result);
        return parsed.ec == std::errc{} ? result : 9999U;
    };
    const unsigned year = number(0, 4);
    const unsigned month = number(5, 2);
    const unsigned day = number(8, 2);
    const unsigned hour = number(11, 2);
    const unsigned minute = number(14, 2);
    const unsigned second = number(17, 2);
    if (year == 0 || month == 0 || month > 12 || day == 0 || hour > 23 ||
        minute > 59 || second > 59) {
        return false;
    }
    constexpr std::array<unsigned, 12> days = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    unsigned maximum = days[month - 1U];
    const bool leap = (year % 4U == 0U && year % 100U != 0U) ||
                      year % 400U == 0U;
    if (month == 2U && leap) ++maximum;
    return day <= maximum;
}

bool canonical_peer(const std::string &value)
{
    std::array<char, INET6_ADDRSTRLEN> encoded{};
    in_addr ipv4{};
    if (inet_pton(AF_INET, value.c_str(), &ipv4) == 1) {
        return inet_ntop(AF_INET, &ipv4, encoded.data(), encoded.size()) != nullptr &&
               value == encoded.data();
    }
    in6_addr ipv6{};
    if (inet_pton(AF_INET6, value.c_str(), &ipv6) != 1 ||
        IN6_IS_ADDR_V4MAPPED(&ipv6)) {
        return false;
    }
    return inet_ntop(AF_INET6, &ipv6, encoded.data(), encoded.size()) != nullptr &&
           value == encoded.data();
}

bool comma_enum(const std::string_view value,
                const std::set<std::string_view> &allowed)
{
    std::set<std::string_view> observed;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t comma = value.find(',', begin);
        const std::string_view item = value.substr(
            begin, comma == std::string_view::npos ? value.size() - begin :
                                                     comma - begin);
        if (item.empty() || item.find_first_of(" \t\r\n") != std::string_view::npos ||
            !allowed.contains(item) || !observed.insert(item).second) {
            return false;
        }
        if (comma == std::string_view::npos) return true;
        begin = comma + 1U;
    }
}

bool event_type_list(const std::string_view value)
{
    std::size_t begin = 0;
    std::set<std::string_view> observed;
    for (;;) {
        const std::size_t comma = value.find(',', begin);
        const std::string_view item = value.substr(
            begin, comma == std::string_view::npos ? value.size() - begin :
                                                     comma - begin);
        if (item.empty() || item.size() > 128U ||
            !std::all_of(item.begin(), item.end(), [](const unsigned char byte) {
                return std::islower(byte) != 0 || std::isdigit(byte) != 0 ||
                       byte == '_';
            }) ||
            !observed.insert(item).second) {
            return false;
        }
        if (comma == std::string_view::npos) return true;
        begin = comma + 1U;
    }
}

std::string base64url_encode(const std::span<const std::uint8_t> input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((input.size() * 4U + 2U) / 3U);
    std::size_t offset = 0;
    while (offset + 3U <= input.size()) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(input[offset]) << 16U |
            static_cast<std::uint32_t>(input[offset + 1U]) << 8U |
            input[offset + 2U];
        result.push_back(alphabet[(value >> 18U) & 63U]);
        result.push_back(alphabet[(value >> 12U) & 63U]);
        result.push_back(alphabet[(value >> 6U) & 63U]);
        result.push_back(alphabet[value & 63U]);
        offset += 3U;
    }
    if (offset < input.size()) {
        std::uint32_t value = static_cast<std::uint32_t>(input[offset]) << 16U;
        result.push_back(alphabet[(value >> 18U) & 63U]);
        if (offset + 1U < input.size()) {
            value |= static_cast<std::uint32_t>(input[offset + 1U]) << 8U;
            result.push_back(alphabet[(value >> 12U) & 63U]);
            result.push_back(alphabet[(value >> 6U) & 63U]);
        }
        else {
            result.push_back(alphabet[(value >> 12U) & 63U]);
        }
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> base64url_decode(
    const std::string_view encoded)
{
    if (encoded.find('=') != std::string_view::npos || encoded.size() % 4U == 1U) {
        return std::nullopt;
    }
    const auto value = [](const char byte) -> int {
        if (byte >= 'A' && byte <= 'Z') return byte - 'A';
        if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
        if (byte >= '0' && byte <= '9') return byte - '0' + 52;
        if (byte == '-') return 62;
        if (byte == '_') return 63;
        return -1;
    };
    std::vector<std::uint8_t> result;
    result.reserve(encoded.size() * 3U / 4U);
    std::size_t offset = 0;
    while (offset + 4U <= encoded.size()) {
        std::uint32_t packed = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            const int digit = value(encoded[offset + index]);
            if (digit < 0) return std::nullopt;
            packed = (packed << 6U) | static_cast<std::uint32_t>(digit);
        }
        result.push_back(static_cast<std::uint8_t>(packed >> 16U));
        result.push_back(static_cast<std::uint8_t>(packed >> 8U));
        result.push_back(static_cast<std::uint8_t>(packed));
        offset += 4U;
    }
    const std::size_t remaining = encoded.size() - offset;
    if (remaining == 2U || remaining == 3U) {
        const int first = value(encoded[offset]);
        const int second = value(encoded[offset + 1U]);
        if (first < 0 || second < 0) return std::nullopt;
        std::uint32_t packed = static_cast<std::uint32_t>(first) << 18U |
                               static_cast<std::uint32_t>(second) << 12U;
        if (remaining == 3U) {
            const int third = value(encoded[offset + 2U]);
            if (third < 0) return std::nullopt;
            packed |= static_cast<std::uint32_t>(third) << 6U;
        }
        result.push_back(static_cast<std::uint8_t>(packed >> 16U));
        if (remaining == 3U) {
            result.push_back(static_cast<std::uint8_t>(packed >> 8U));
        }
    }
    if (base64url_encode(result) != encoded) return std::nullopt;
    return result;
}

std::array<std::uint8_t, 16> filter_digest(const std::string_view path,
                                           const QueryMap &filters)
{
    Bytes material(path.begin(), path.end());
    for (const auto &[name, value] : filters) {
        material.insert(material.end(), name.begin(), name.end());
        material.push_back(0);
        material.insert(material.end(), value.begin(), value.end());
        material.push_back(0);
    }
    const Hash32 digest = sha256(material);
    std::array<std::uint8_t, 16> result{};
    std::copy_n(digest.begin(), result.size(), result.begin());
    return result;
}

std::string make_cursor(const std::uint16_t resource_tag,
                        const std::uint64_t database_id,
                        const std::string_view path,
                        const QueryMap &filters)
{
    std::array<std::uint8_t, kCursorBytes> cursor{};
    cursor[0] = 1;
    cursor[1] = static_cast<std::uint8_t>(resource_tag >> 8U);
    cursor[2] = static_cast<std::uint8_t>(resource_tag);
    for (unsigned index = 0; index < 8U; ++index) {
        cursor[3U + index] = static_cast<std::uint8_t>(
            database_id >> (56U - 8U * index));
    }
    const auto digest = filter_digest(path, filters);
    std::copy(digest.begin(), digest.end(), cursor.begin() + 11);
    return base64url_encode(cursor);
}

std::optional<std::uint64_t> read_cursor(const std::string_view encoded,
                                         const std::uint16_t resource_tag,
                                         const std::string_view path,
                                         const QueryMap &filters)
{
    const auto decoded = base64url_decode(encoded);
    if (!decoded.has_value() || decoded->size() != kCursorBytes ||
        (*decoded)[0] != 1 ||
        (static_cast<std::uint16_t>((*decoded)[1]) << 8U | (*decoded)[2]) !=
            resource_tag) {
        return std::nullopt;
    }
    const auto digest = filter_digest(path, filters);
    if (!constant_time_equal(
            std::span<const std::uint8_t>(decoded->data() + 11, digest.size()),
            digest)) {
        return std::nullopt;
    }
    std::uint64_t database_id = 0;
    for (unsigned index = 0; index < 8U; ++index) {
        database_id = (database_id << 8U) | (*decoded)[3U + index];
    }
    return database_id;
}

std::string add_decimal(const std::string_view left,
                        const std::string_view right)
{
    if (!decimal_string(left) || !decimal_string(right)) {
        throw std::runtime_error("invalid decimal hashrate from database");
    }
    std::string result;
    result.reserve(std::max(left.size(), right.size()) + 1U);
    std::size_t left_index = left.size();
    std::size_t right_index = right.size();
    unsigned carry = 0;
    while (left_index != 0 || right_index != 0 || carry != 0) {
        unsigned digit = carry;
        if (left_index != 0) digit += static_cast<unsigned>(left[--left_index] - '0');
        if (right_index != 0) digit += static_cast<unsigned>(right[--right_index] - '0');
        result.push_back(static_cast<char>('0' + digit % 10U));
        carry = digit / 10U;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

bool has_work(const HashrateWindows &windows)
{
    return windows.one_minute != "0" || windows.five_minutes != "0" ||
           windows.ten_minutes != "0" || windows.one_hour != "0" ||
           windows.six_hours != "0" || windows.twenty_four_hours != "0";
}

HashrateWindows add_windows(const HashrateWindows &left,
                            const HashrateWindows &right)
{
    return {
        add_decimal(left.one_minute, right.one_minute),
        add_decimal(left.five_minutes, right.five_minutes),
        add_decimal(left.ten_minutes, right.ten_minutes),
        add_decimal(left.one_hour, right.one_hour),
        add_decimal(left.six_hours, right.six_hours),
        add_decimal(left.twenty_four_hours, right.twenty_four_hours),
    };
}

Json hashrate_json(const HashrateWindows &windows, const std::string &source)
{
    return Json{
        {"unit", "H/s"},
        {"source", source},
        {"1m", windows.one_minute},
        {"5m", windows.five_minutes},
        {"10m", windows.ten_minutes},
        {"1h", windows.one_hour},
        {"6h", windows.six_hours},
        {"24h", windows.twenty_four_hours},
    };
}

using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using BnCtxPtr = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

BnPtr make_bn()
{
    BIGNUM *value = BN_new();
    if (value == nullptr) throw DatabaseError("allocate exact effort integer");
    return BnPtr(value, &BN_free);
}

BnPtr decimal_bn(const std::string_view value)
{
    if (!decimal_string(value)) {
        throw DatabaseError("invalid decimal round effort value");
    }
    BIGNUM *parsed = nullptr;
    const std::string owned(value);
    if (BN_dec2bn(&parsed, owned.c_str()) !=
            static_cast<int>(owned.size()) ||
        parsed == nullptr) {
        if (parsed != nullptr) BN_free(parsed);
        throw DatabaseError("parse exact round effort integer");
    }
    return BnPtr(parsed, &BN_free);
}

std::string bn_decimal(const BIGNUM *const value)
{
    char *encoded = BN_bn2dec(value);
    if (encoded == nullptr) {
        throw DatabaseError("encode exact round effort integer");
    }
    std::string result(encoded);
    OPENSSL_free(encoded);
    return result;
}

class ExactEffort final {
public:
    ExactEffort()
        : numerator_(make_bn()), denominator_(make_bn()),
          context_(BN_CTX_new(), &BN_CTX_free)
    {
        BN_zero(numerator_.get());
        if (!context_ || BN_one(denominator_.get()) != 1) {
            throw DatabaseError("initialize exact round effort arithmetic");
        }
    }

    void add(const std::string_view credited,
             const std::string_view network_difficulty)
    {
        if (!decimal_string(network_difficulty, false)) {
            throw DatabaseError("invalid round effort network difficulty");
        }
        BnPtr work = decimal_bn(credited);
        BnPtr difficulty = decimal_bn(network_difficulty);
        BnPtr left = make_bn();
        BnPtr right = make_bn();
        BnPtr sum = make_bn();
        BnPtr product = make_bn();
        BnPtr divisor = make_bn();
        BnPtr reduced_numerator = make_bn();
        BnPtr reduced_denominator = make_bn();
        if (BN_mul(left.get(), numerator_.get(), difficulty.get(),
                   context_.get()) != 1 ||
            BN_mul(right.get(), work.get(), denominator_.get(),
                   context_.get()) != 1 ||
            BN_add(sum.get(), left.get(), right.get()) != 1 ||
            BN_mul(product.get(), denominator_.get(), difficulty.get(),
                   context_.get()) != 1 ||
            BN_gcd(divisor.get(), sum.get(), product.get(), context_.get()) != 1 ||
            BN_div(reduced_numerator.get(), nullptr, sum.get(), divisor.get(),
                   context_.get()) != 1 ||
            BN_div(reduced_denominator.get(), nullptr, product.get(),
                   divisor.get(), context_.get()) != 1 ||
            BN_copy(numerator_.get(), reduced_numerator.get()) == nullptr ||
            BN_copy(denominator_.get(), reduced_denominator.get()) == nullptr) {
            throw DatabaseError("accumulate exact round effort");
        }
    }

    [[nodiscard]] std::string micro_percent() const
    {
        BnPtr scale = decimal_bn("100000000");
        BnPtr scaled = make_bn();
        BnPtr quotient = make_bn();
        if (BN_mul(scaled.get(), numerator_.get(), scale.get(),
                   context_.get()) != 1 ||
            BN_div(quotient.get(), nullptr, scaled.get(), denominator_.get(),
                   context_.get()) != 1) {
            throw DatabaseError("divide exact round effort");
        }
        return bn_decimal(quotient.get());
    }

private:
    BnPtr numerator_;
    BnPtr denominator_;
    mutable BnCtxPtr context_;
};

std::string effort_micro_percent(const std::string_view credited,
                                 const std::string_view network_difficulty)
{
    ExactEffort effort;
    effort.add(credited, network_difficulty);
    return effort.micro_percent();
}

std::string format_micro_percent(std::string value)
{
    if (!decimal_string(value)) {
        throw DatabaseError("invalid effort percentage");
    }
    if (value.size() <= 6U) value.insert(0, 7U - value.size(), '0');
    value.insert(value.size() - 6U, 1, '.');
    return value;
}

bool contains_oversize_blob(const Json &value)
{
    static const std::set<std::string_view> blob_fields = {
        "blocktemplate_blob", "blockhashing_blob", "private_block_blob",
        "hashing_blob", "frozen_block_blob",
    };
    if (value.is_object()) {
        for (const auto &[key, item] : value.items()) {
            if (blob_fields.contains(key) && item.is_string() &&
                item.get_ref<const std::string &>().size() >
                    kMaximumEncodedBlobBytes) {
                return true;
            }
            if (contains_oversize_blob(item)) return true;
        }
    }
    else if (value.is_array()) {
        for (const Json &item : value) {
            if (contains_oversize_blob(item)) return true;
        }
    }
    return false;
}

std::uint64_t file_bytes(const std::string &path)
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(bytes);
}

} // namespace

struct ApiService::ParsedQuery {
    QueryMap values;
};

struct ApiService::CollectionRoute {
    ApiCollection resource;
    std::string_view path;
    std::uint16_t tag;
    std::set<std::string_view> parameters;
};

bool ApiReadinessSnapshot::ready() const noexcept
{
    return database.ready && entropy.ready && daemon_rpc.ready &&
           template_state.ready && verifier.ready && stratum.ready;
}

std::string_view api_collection_path(const ApiCollection resource) noexcept
{
    switch (resource) {
    case ApiCollection::connections: return "/v1/connections";
    case ApiCollection::workers: return "/v1/workers";
    case ApiCollection::shares: return "/v1/shares";
    case ApiCollection::top_shares: return "/v1/shares/top";
    case ApiCollection::recent_high_shares: return "/v1/shares/recent-high";
    case ApiCollection::hashes: return "/v1/hashes";
    case ApiCollection::submissions: return "/v1/submissions";
    case ApiCollection::rounds: return "/v1/rounds";
    case ApiCollection::bans: return "/v1/bans";
    case ApiCollection::events: return "/v1/events";
    }
    return {};
}

ApiService::ApiService(ApiServiceOptions options,
                       Database *const database,
                       ApiDataSource data_source,
                       Clock clock)
    : options_(std::move(options)),
      database_(database),
      data_source_(std::move(data_source)),
      clock_(std::move(clock))
{
    if (!clock_) {
        clock_ = [] { return unix_time_us(); };
    }
    if (options_.api.access_token.has_value() &&
        !options_.api.access_token->empty()) {
        expected_authorization_ = "Bearer " + *options_.api.access_token;
    }
    if (options_.api.max_page_size == 0 || options_.api.max_page_size > 10'000 ||
        options_.api.top_shares_limit == 0 ||
        options_.api.top_shares_limit > 100 ||
        options_.api.recent_high_shares_limit == 0 ||
        options_.api.recent_high_shares_limit > 100 ||
        options_.api.recent_high_share_min_difficulty == 0 ||
        options_.api.max_connections == 0 ||
        options_.api.request_rate_per_second == 0 ||
        options_.api.request_burst == 0 ||
        options_.api.max_pending_bytes_per_connection < 4096U ||
        options_.worker_threads == 0 || options_.worker_threads > 64U ||
        !lowercase_hex(options_.identity.git_commit, 40) ||
        !lowercase_hex(options_.identity.session_id, 32) ||
        options_.identity.version.empty()) {
        throw std::invalid_argument("invalid API service configuration");
    }
    if (options_.api.enabled) {
        HttpServerConfig server_config{
            .listen = options_.api.listen,
            .max_connections = static_cast<std::size_t>(options_.api.max_connections),
            .request_rate_per_second = static_cast<std::uint32_t>(
                options_.api.request_rate_per_second),
            .request_burst = static_cast<std::uint32_t>(options_.api.request_burst),
            .max_pending_bytes_per_connection = static_cast<std::size_t>(
                options_.api.max_pending_bytes_per_connection),
            .worker_threads = options_.worker_threads,
        };
        server_ = std::make_unique<HttpServer>(
            std::move(server_config),
            [this](const HttpRequest &request) { return handle(request); });
    }
}

ApiService::~ApiService() = default;

void ApiService::start()
{
    if (server_) server_->start();
}

void ApiService::stop() noexcept
{
    if (server_) server_->stop();
}

bool ApiService::running() const noexcept
{
    return server_ && server_->running();
}

std::string ApiService::bound_endpoint() const
{
    return server_ ? server_->bound_endpoint() : std::string{};
}

bool ApiService::authenticate(const HttpRequest &request) const
{
    if (!expected_authorization_.has_value()) return true;
    const auto authorization = request.headers.find("authorization");
    if (authorization == request.headers.end()) return false;
    return constant_time_equal(authorization->second,
                               *expected_authorization_);
}

std::string ApiService::generated_at(const std::int64_t now_unix_us) const
{
    return format_rfc3339_utc_us(now_unix_us);
}

HttpResponse ApiService::error(
    const int status,
    const std::string_view code,
    const std::string_view message,
    const std::int64_t now_unix_us,
    std::vector<std::pair<std::string, std::string>> headers) const
{
    Json document{
        {"schema_version", 1},
        {"generated_at", generated_at(now_unix_us)},
        {"error", {{"code", code}, {"message", message}}},
    };
    return {status, document.dump(), std::move(headers)};
}

HttpResponse ApiService::success(Json data,
                                 const std::int64_t now_unix_us,
                                 const int status) const
{
    if (!data.is_object() && !data.is_array()) {
        return error(500, "query_failed", "Reader returned invalid data",
                     now_unix_us);
    }
    if (contains_oversize_blob(data)) {
        return error(413, "response_too_large",
                     "Requested blob exceeds the response bound", now_unix_us);
    }
    Json document{
        {"schema_version", 1},
        {"generated_at", generated_at(now_unix_us)},
        {"data", std::move(data)},
    };
    std::string body = document.dump();
    if (body.size() > kMaximumResponseBytes) {
        return error(413, "response_too_large",
                     "Requested response exceeds the response bound", now_unix_us);
    }
    return {status, std::move(body), {}};
}

HttpResponse ApiService::handle(const HttpRequest &request) const
{
    std::int64_t now = 0;
    try {
        now = clock_();
        if (request.method != "GET") {
            return error(405, "method_not_allowed", "Only GET is allowed", now,
                         {{"Allow", "GET"}});
        }
        const bool authorized = authenticate(request);
        if (!authorized) {
            return error(401, "authentication_required",
                         "A valid Bearer token is required", now,
                         {{"WWW-Authenticate", "Bearer"}});
        }
        const bool authenticated = options_.api.access_token.has_value() &&
                                   !options_.api.access_token->empty();
        return handle_authenticated(request, authenticated, now);
    }
    catch (const ApiRequestError &failure) {
        return error(failure.status, failure.code, failure.what(), now);
    }
    catch (...) {
        return error(500, "query_failed", "The API query failed", now);
    }
}

HttpResponse ApiService::live_response(const std::int64_t now_unix_us) const
{
    const std::int64_t elapsed = std::max<std::int64_t>(
        0, now_unix_us - options_.identity.started_unix_us);
    return success(Json{
                       {"alive", true},
                       {"version", options_.identity.version},
                       {"uptime_seconds", elapsed / 1'000'000},
                   },
                   now_unix_us);
}

HttpResponse ApiService::ready_response(const std::int64_t now_unix_us) const
{
    ApiReadinessSnapshot snapshot{};
    if (data_source_.readiness) {
        snapshot = data_source_.readiness();
    }
    else {
        snapshot.database = {
            database_ != nullptr,
            false,
            database_ == nullptr ? std::optional<std::string>("database unavailable") :
                                   std::nullopt,
        };
        snapshot.entropy.reason = "runtime readiness unavailable";
        snapshot.daemon_rpc.reason = "runtime readiness unavailable";
        snapshot.template_state.reason = "runtime readiness unavailable";
        snapshot.verifier.reason = "runtime readiness unavailable";
        snapshot.stratum.reason = "runtime readiness unavailable";
    }
    const auto component = [](const ApiComponentState &state) {
        return Json{{"ready", state.ready},
                    {"degraded", state.degraded},
                    {"reason", state.reason.has_value() ? Json(*state.reason) :
                                                          Json(nullptr)}};
    };
    Json data{
        {"ready", snapshot.ready()},
        {"height", snapshot.height.has_value() ? Json(*snapshot.height) :
                                                 Json(nullptr)},
        {"components",
         {{"database", component(snapshot.database)},
          {"entropy", component(snapshot.entropy)},
          {"daemon_rpc", component(snapshot.daemon_rpc)},
          {"template", component(snapshot.template_state)},
          {"verifier", component(snapshot.verifier)},
          {"stratum", component(snapshot.stratum)}}},
    };
    return success(std::move(data), now_unix_us, snapshot.ready() ? 200 : 503);
}

HttpResponse ApiService::singleton_response(const ApiSingleton resource,
                                            const std::int64_t now_unix_us) const
{
    if (resource == ApiSingleton::persistence) {
        return persistence_response(now_unix_us);
    }
    if (!data_source_.singleton) {
        const bool round = resource == ApiSingleton::current_round;
        return error(503, round ? "round_unavailable" : "not_ready",
                     round ? "The open round is unavailable" :
                             "The requested snapshot is unavailable",
                     now_unix_us);
    }
    const std::optional<Json> result = data_source_.singleton(resource);
    if (!result.has_value()) {
        const bool round = resource == ApiSingleton::current_round;
        return error(round ? 503 : 404,
                     round ? "round_unavailable" : "not_found",
                     round ? "The open round is unavailable" :
                             "The resource was not found",
                     now_unix_us);
    }
    if (!result->is_object()) {
        return error(500, "query_failed", "Reader returned invalid data",
                     now_unix_us);
    }
    return success(*result, now_unix_us);
}

HttpResponse ApiService::persistence_response(
    const std::int64_t now_unix_us) const
{
    if (data_source_.singleton) {
        const std::optional<Json> supplied =
            data_source_.singleton(ApiSingleton::persistence);
        if (supplied.has_value()) {
            if (!supplied->is_object()) {
                return error(500, "query_failed", "Reader returned invalid data",
                             now_unix_us);
            }
            return success(*supplied, now_unix_us);
        }
    }
    if (database_ == nullptr) {
        return error(500, "query_failed", "Persistence reader is unavailable",
                     now_unix_us);
    }
    const DatabasePragmas pragmas = database_->pragmas();
    const std::uint64_t unresolved = database_->recoverable_candidates().size();
    const std::uint64_t pending_notify = database_->pending_blocknotify_count();
    const DatabaseWriterStats writer = database_->writer_stats();
    const std::string &path = database_->options().path;
    return success(Json{
                       {"schema_version", database_->schema_version()},
                       {"journal_mode", pragmas.journal_mode},
                       {"synchronous", pragmas.synchronous},
                       {"foreign_keys", pragmas.foreign_keys},
                       {"database_bytes", std::to_string(file_bytes(path))},
                       {"wal_bytes", std::to_string(file_bytes(path + "-wal"))},
                       {"writer_queue_items", writer.queued_items},
                       {"writer_queue_bytes", writer.queued_bytes},
                       {"writer_priority_items", writer.priority_items},
                       {"pending_accounting_items",
                        writer.pending_accounting_items},
                       {"pending_transient_shares",
                        writer.pending_transient_shares},
                       {"last_commit_at", nullptr},
                       {"last_writer_error_at", nullptr},
                       {"last_writer_error_code", nullptr},
                       {"last_writer_error_message", nullptr},
                       {"unresolved_candidates", std::to_string(unresolved)},
                       {"pending_blocknotify", std::to_string(pending_notify)},
                   },
                   now_unix_us);
}

HttpResponse ApiService::hashrate_response(const ParsedQuery &query,
                                           const std::int64_t now_unix_us) const
{
    if (database_ == nullptr) {
        return error(500, "query_failed", "Hashrate reader is unavailable",
                     now_unix_us);
    }
    std::string requested =
        options_.active_hashrate_source == HashrateSource::verified ? "verified" :
                                                                     "claimed";
    if (const auto source = query.values.find("source");
        source != query.values.end()) {
        requested = source->second;
    }
    if (requested != "verified" && requested != "claimed" && requested != "all") {
        throw ApiRequestError(400, "invalid_query", "Invalid hashrate source");
    }
    const std::int64_t now_second = now_unix_us / 1'000'000;
    if (requested == "verified" || requested == "claimed") {
        const HashrateSource source = requested == "verified" ?
                                          HashrateSource::verified :
                                          HashrateSource::claimed;
        return success(hashrate_json(
                           database_->hashrate("global", 0, source, now_second),
                           requested),
                       now_unix_us);
    }
    const HashrateWindows verified = database_->hashrate(
        "global", 0, HashrateSource::verified, now_second);
    const HashrateWindows claimed = database_->hashrate(
        "global", 0, HashrateSource::claimed, now_second);
    std::string source = "mixed";
    if (!has_work(claimed)) source = "verified";
    else if (!has_work(verified)) source = "claimed";
    return success(hashrate_json(add_windows(verified, claimed), source),
                   now_unix_us);
}

HttpResponse ApiService::handle_detail(const HttpRequest &,
                                       const ApiDetail resource,
                                       std::string id,
                                       const bool include_blobs,
                                       const bool authenticated,
                                       const std::int64_t now_unix_us) const
{
    if (include_blobs &&
        (!options_.api.access_token.has_value() ||
         options_.api.access_token->empty())) {
        return error(403, "sensitive_view_disabled",
                     "Sensitive blob views require authenticated API mode",
                     now_unix_us);
    }
    if (!data_source_.detail) {
        return error(500, "query_failed", "Detail reader is unavailable",
                     now_unix_us);
    }
    ApiDetailRequest query{resource, std::move(id), include_blobs, authenticated};
    const std::optional<Json> result = data_source_.detail(query);
    if (!result.has_value()) {
        return error(404, "not_found", "The resource was not found", now_unix_us);
    }
    if (!result->is_object()) {
        return error(500, "query_failed", "Reader returned invalid data",
                     now_unix_us);
    }
    return success(*result, now_unix_us);
}

HttpResponse ApiService::handle_collection(
    const HttpRequest &request,
    const CollectionRoute &route,
    const bool authenticated,
    const std::int64_t now_unix_us) const
{
    const bool top_shares = route.resource == ApiCollection::top_shares;
    const bool recent_high_shares =
        route.resource == ApiCollection::recent_high_shares;
    const bool bounded_ranking = top_shares || recent_high_shares;
    QueryMap parameters = parse_query(request.query);
    for (const auto &[name, unused] : parameters) {
        (void)unused;
        if (name != "cursor" && name != "limit" &&
            !route.parameters.contains(name)) {
            throw ApiRequestError(400, "invalid_query", "Unknown query parameter");
        }
    }

    const std::uint64_t ranking_limit = top_shares ?
        options_.api.top_shares_limit : options_.api.recent_high_shares_limit;
    std::uint64_t limit_value = bounded_ranking ? ranking_limit :
                                                       kDefaultPageSize;
    if (const auto limit = parameters.find("limit"); limit != parameters.end()) {
        if (!canonical_unsigned(limit->second, false, &limit_value) ||
            limit_value > options_.api.max_page_size ||
            (bounded_ranking && limit_value > ranking_limit)) {
            throw ApiRequestError(400, "invalid_query", "Invalid page limit");
        }
    }
    if (bounded_ranking && parameters.contains("cursor")) {
        throw ApiRequestError(400, "invalid_query",
                              "Ranked share views are bounded snapshots");
    }

    QueryMap filters = parameters;
    filters.erase("cursor");
    filters.erase("limit");
    if (recent_high_shares) {
        filters.emplace("min_actual_difficulty", std::to_string(
            options_.api.recent_high_share_min_difficulty));
    }
    const auto invalid = [](const std::string &message) {
        throw ApiRequestError(400, "invalid_query", message);
    };
    const auto validate_bool = [&](const char *name) {
        if (const auto item = filters.find(name); item != filters.end() &&
            item->second != "true" && item->second != "false") {
            invalid("Invalid boolean filter");
        }
    };
    const auto validate_decimal = [&](const char *name) {
        if (const auto item = filters.find(name); item != filters.end()) {
            std::uint64_t value = 0;
            if (!canonical_unsigned(item->second, false, &value) ||
                value > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                invalid("Invalid decimal identifier");
            }
        }
    };
    const auto validate_hex_id = [&](const char *name) {
        if (const auto item = filters.find(name); item != filters.end() &&
            !lowercase_hex(item->second, 32)) {
            invalid("Invalid public identifier");
        }
    };
    const auto validate_time = [&](const char *name) {
        if (const auto item = filters.find(name); item != filters.end() &&
            !valid_timestamp(item->second)) {
            invalid("Invalid UTC time filter");
        }
    };
    validate_bool("active");
    validate_bool("include_blobs");
    for (const char *const name : {"worker_id", "template_generation",
                                   "candidate_id", "share_id", "round_id",
                                   "height"}) {
        validate_decimal(name);
    }
    for (const char *const name : {"connection_id", "job_id"}) {
        validate_hex_id(name);
    }
    validate_time("after_time");
    validate_time("before_time");
    const auto after_time = filters.find("after_time");
    const auto before_time = filters.find("before_time");
    if (after_time != filters.end() && before_time != filters.end() &&
        after_time->second > before_time->second) {
        invalid("Time filter bounds are reversed");
    }
    for (const char *const name : {"login", "rigid"}) {
        if (const auto text = filters.find(name);
            text != filters.end() && text->second.size() > 256U) {
            invalid("Worker text filter is too long");
        }
    }
    if (const auto peer = filters.find("peer");
        peer != filters.end() && !canonical_peer(peer->second)) {
        invalid("Peer must be one canonical IP address");
    }
    if (const auto minimum = filters.find("min_difficulty");
        minimum != filters.end() && !decimal_string(minimum->second, false)) {
        invalid("Invalid minimum difficulty");
    }
    if (const auto minimum = filters.find("min_actual_difficulty");
        minimum != filters.end() && !decimal_string(minimum->second, false)) {
        invalid("Invalid minimum actual difficulty");
    }
    if (const auto blobs = filters.find("include_blobs");
        blobs != filters.end() && blobs->second == "true" &&
        (!options_.api.access_token.has_value() ||
         options_.api.access_token->empty())) {
        return error(403, "sensitive_view_disabled",
                     "Sensitive blob views require authenticated API mode",
                     now_unix_us);
    }
    if (const auto role_filter = filters.find("role");
        role_filter != filters.end() && role_filter->second != "claimed" &&
        role_filter->second != "computed") {
        invalid("Invalid hash role");
    }
    static const std::set<std::string_view> share_statuses = {
        "received", "verifying", "accepted", "stale", "duplicate",
        "low_difficulty", "invalid_result", "unknown_job", "malformed",
        "unauthenticated", "server_busy", "verifier_failed", "cancelled",
    };
    static const std::set<std::string_view> candidate_states = {
        "journaled", "dispatching", "retry_wait", "accepted", "rejected",
        "ambiguous", "accepted_by_reconciliation",
    };
    if (const auto status = filters.find("status"); status != filters.end()) {
        const bool valid = route.resource == ApiCollection::shares ?
                               comma_enum(status->second, share_statuses) :
                           route.resource == ApiCollection::submissions ?
                               comma_enum(status->second, candidate_states) :
                           route.resource == ApiCollection::rounds ?
                               comma_enum(status->second, {"open", "closed"}) :
                               false;
        if (!valid) invalid("Invalid status filter");
    }
    if (const auto status = filters.find("state"); status != filters.end()) {
        const bool valid = route.resource == ApiCollection::submissions ?
                               comma_enum(status->second, candidate_states) :
                           route.resource == ApiCollection::rounds ?
                               comma_enum(status->second, {"open", "closed"}) :
                               false;
        if (!valid) invalid("Invalid state filter");
    }
    if (const auto statuses = filters.find("share_status");
        statuses != filters.end() &&
        !comma_enum(statuses->second, share_statuses)) {
        invalid("Invalid share status filter");
    }
    if (const auto types = filters.find("type");
        types != filters.end() && !event_type_list(types->second)) {
        invalid("Invalid event type filter");
    }

    std::uint64_t after_id = 0;
    if (const auto cursor = parameters.find("cursor"); cursor != parameters.end()) {
        const std::optional<std::uint64_t> decoded =
            read_cursor(cursor->second, route.tag, route.path, filters);
        if (!decoded.has_value()) {
            throw ApiRequestError(400, "invalid_cursor",
                                  "The cursor is not valid for this resource");
        }
        after_id = *decoded;
    }
    if (!data_source_.collection) {
        return error(500, "query_failed", "Collection reader is unavailable",
                     now_unix_us);
    }
    ApiCollectionRequest query{
        route.resource,
        std::string(route.path),
        after_id,
        static_cast<std::uint32_t>(limit_value),
        filters,
        authenticated,
    };
    ApiCollectionResult result = data_source_.collection(query);
    if (!result.rows.is_array() || result.rows.size() > limit_value ||
        (result.next_database_id.has_value() &&
         (*result.next_database_id <= after_id || result.rows.empty()))) {
        return error(500, "query_failed", "Reader returned invalid pagination",
                     now_unix_us);
    }
    const Json next_cursor = result.next_database_id.has_value() ?
                                 Json(make_cursor(route.tag,
                                                  *result.next_database_id,
                                                  route.path,
                                                  filters)) :
                                 Json(nullptr);
    Json document{
        {"schema_version", 1},
        {"generated_at", generated_at(now_unix_us)},
        {"data", std::move(result.rows)},
        {"page", {{"limit", limit_value}, {"next_cursor", next_cursor}}},
    };
    if (top_shares) {
        const auto round = filters.find("round_id");
        document["selection"] = {
            {"kind", "top_actual_difficulty"},
            {"round_id", round == filters.end() ? Json(nullptr) :
                                                  Json(round->second)},
            {"configured_limit", ranking_limit},
            {"retained_only", true},
        };
    }
    else if (recent_high_shares) {
        document["selection"] = {
            {"kind", "recent_high_actual_difficulty"},
            {"min_actual_difficulty", filters.at("min_actual_difficulty")},
            {"configured_limit", ranking_limit},
            {"retained_only", true},
        };
    }
    if (contains_oversize_blob(document)) {
        return error(413, "response_too_large",
                     "Requested blob exceeds the response bound", now_unix_us);
    }
    std::string body = document.dump();
    if (body.size() > kMaximumResponseBytes) {
        return error(413, "response_too_large",
                     "Requested response exceeds the response bound", now_unix_us);
    }
    return {200, std::move(body), {}};
}

HttpResponse ApiService::handle_authenticated(const HttpRequest &request,
                                              const bool authenticated,
                                              const std::int64_t now_unix_us) const
{
    const auto require_no_query = [&]() {
        if (!request.query.empty()) {
            throw ApiRequestError(400, "invalid_query",
                                  "This endpoint accepts no query parameters");
        }
    };
    if (request.path == "/v1/health/live") {
        require_no_query();
        return live_response(now_unix_us);
    }
    if (request.path == "/v1/health/ready") {
        require_no_query();
        return ready_response(now_unix_us);
    }
    if (request.path == "/v1/summary") {
        require_no_query();
        return singleton_response(ApiSingleton::summary, now_unix_us);
    }
    if (request.path == "/v1/daemon") {
        require_no_query();
        return singleton_response(ApiSingleton::daemon, now_unix_us);
    }
    if (request.path == "/v1/verifier") {
        require_no_query();
        return singleton_response(ApiSingleton::verifier, now_unix_us);
    }
    if (request.path == "/v1/persistence") {
        require_no_query();
        return singleton_response(ApiSingleton::persistence, now_unix_us);
    }
    if (request.path == "/v1/hashrate") {
        ParsedQuery query{parse_query(request.query)};
        for (const auto &[name, unused] : query.values) {
            (void)unused;
            if (name != "source") {
                throw ApiRequestError(400, "invalid_query",
                                      "Unknown query parameter");
            }
        }
        return hashrate_response(query, now_unix_us);
    }
    if (request.path == "/v1/rounds/current") {
        require_no_query();
        return singleton_response(ApiSingleton::current_round, now_unix_us);
    }

    static const std::array<CollectionRoute, 10> collections = {{
        {ApiCollection::connections, "/v1/connections", 1,
         {"active", "worker_id", "peer", "after_time", "before_time"}},
        {ApiCollection::workers, "/v1/workers", 2,
         {"active", "login", "rigid", "after_time", "before_time"}},
        {ApiCollection::shares, "/v1/shares", 5,
         {"status", "connection_id", "worker_id", "job_id", "candidate_id",
          "height", "min_difficulty", "after_time", "before_time"}},
        {ApiCollection::top_shares, "/v1/shares/top", 11,
         {"round_id"}},
        {ApiCollection::recent_high_shares, "/v1/shares/recent-high", 12,
         {}},
        {ApiCollection::hashes, "/v1/hashes", 6,
         {"role", "share_status", "connection_id", "worker_id", "job_id",
          "after_time", "before_time"}},
        {ApiCollection::submissions, "/v1/submissions", 7,
         {"state", "connection_id", "job_id", "height", "peer", "after_time",
          "before_time"}},
        {ApiCollection::rounds, "/v1/rounds", 8,
         {"state", "after_time", "before_time"}},
        {ApiCollection::bans, "/v1/bans", 9,
         {"active", "peer", "after_time", "before_time"}},
        {ApiCollection::events, "/v1/events", 10,
         {"type", "connection_id", "worker_id", "template_generation",
          "job_id", "share_id", "candidate_id", "round_id", "after_time",
          "before_time"}},
    }};
    for (const CollectionRoute &route : collections) {
        if (request.path == route.path) {
            return handle_collection(request, route, authenticated, now_unix_us);
        }
    }

    const auto detail = [&](const std::string_view prefix,
                            const ApiDetail resource,
                            const bool hex_id,
                            const bool blobs) -> std::optional<HttpResponse> {
        if (!request.path.starts_with(prefix)) return std::nullopt;
        const std::string id = request.path.substr(prefix.size());
        std::uint64_t numeric_id = 0;
        const bool valid_id = hex_id ? lowercase_hex(id, 32) :
            (canonical_unsigned(id, false, &numeric_id) &&
             numeric_id <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()));
        if (id.empty() || id.find('/') != std::string::npos ||
            !valid_id) {
            return error(400, "invalid_id", "The resource ID is invalid",
                         now_unix_us);
        }
        QueryMap query = parse_query(request.query);
        bool include_blobs = false;
        if (blobs) {
            if (query.size() > 1U ||
                (!query.empty() && !query.contains("include_blobs"))) {
                throw ApiRequestError(400, "invalid_query",
                                      "Unknown query parameter");
            }
            if (const auto include = query.find("include_blobs");
                include != query.end()) {
                if (include->second != "true" && include->second != "false") {
                    throw ApiRequestError(400, "invalid_query",
                                          "Invalid boolean filter");
                }
                include_blobs = include->second == "true";
            }
        }
        else if (!query.empty()) {
            throw ApiRequestError(400, "invalid_query",
                                  "This detail endpoint accepts no query parameters");
        }
        return handle_detail(request, resource, id, include_blobs, authenticated,
                             now_unix_us);
    };
    if (auto response = detail("/v1/connections/", ApiDetail::connection, true,
                               false)) {
        return *response;
    }
    if (auto response = detail("/v1/shares/", ApiDetail::share, false, false)) {
        return *response;
    }
    if (auto response = detail("/v1/submissions/", ApiDetail::submission, false,
                               true)) {
        return *response;
    }
    return error(404, "not_found", "The API route was not found", now_unix_us);
}

namespace {

[[noreturn]] void throw_sqlite(sqlite3 *const database,
                               const std::string_view operation)
{
    throw DatabaseError(std::string(operation) + ": " +
                        (database == nullptr ? "SQLite unavailable" :
                                               sqlite3_errmsg(database)));
}

class ReadStatement final {
public:
    ReadStatement(sqlite3 *const database, const std::string_view sql)
        : database_(database)
    {
        if (sqlite3_prepare_v3(database,
                              sql.data(),
                              static_cast<int>(sql.size()),
                              SQLITE_PREPARE_PERSISTENT,
                              &statement_,
                              nullptr) != SQLITE_OK) {
            throw_sqlite(database, "prepare API read query");
        }
    }

    ~ReadStatement()
    {
        if (statement_ != nullptr) (void)sqlite3_finalize(statement_);
    }

    ReadStatement(const ReadStatement &) = delete;
    ReadStatement &operator=(const ReadStatement &) = delete;

    void bind(const int index, const std::int64_t value)
    {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw_sqlite(database_, "bind API integer");
        }
    }

    void bind(const int index, const std::string_view value)
    {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(statement_,
                              index,
                              value.data(),
                              static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(database_, "bind API text");
        }
    }

    void bind_blob(const int index, const std::span<const std::uint8_t> value)
    {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_blob(statement_,
                              index,
                              value.data(),
                              static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(database_, "bind API blob");
        }
    }

    [[nodiscard]] bool row()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(database_, "execute API read query");
    }

    [[nodiscard]] bool is_null(const int column) const noexcept
    {
        return sqlite3_column_type(statement_, column) == SQLITE_NULL;
    }

    [[nodiscard]] std::int64_t integer(const int column) const
    {
        if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
            throw DatabaseError("API query returned a noninteger column");
        }
        return sqlite3_column_int64(statement_, column);
    }

    [[nodiscard]] std::string text(const int column) const
    {
        if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
            throw DatabaseError("API query returned a nontext column");
        }
        const unsigned char *const value = sqlite3_column_text(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (bytes == 0) return {};
        if (bytes < 0 || value == nullptr) {
            throw DatabaseError("API query returned invalid text");
        }
        return std::string(reinterpret_cast<const char *>(value),
                           static_cast<std::size_t>(bytes));
    }

    [[nodiscard]] Bytes blob(const int column) const
    {
        if (sqlite3_column_type(statement_, column) != SQLITE_BLOB) {
            throw DatabaseError("API query returned a nonblob column");
        }
        const auto *const value = static_cast<const std::uint8_t *>(
            sqlite3_column_blob(statement_, column));
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (bytes < 0 || (bytes != 0 && value == nullptr)) {
            throw DatabaseError("API query returned an invalid blob");
        }
        if (bytes == 0) return {};
        return Bytes(value, value + bytes);
    }

private:
    sqlite3 *database_{};
    sqlite3_stmt *statement_{};
};

struct QueryBinding {
    enum class Type { integer, text, blob } type{Type::integer};
    std::int64_t integer{};
    std::string text;
    Bytes blob;
};

void apply_bindings(ReadStatement &statement,
                    const std::vector<QueryBinding> &bindings)
{
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const int parameter = static_cast<int>(index + 1U);
        switch (bindings[index].type) {
        case QueryBinding::Type::integer:
            statement.bind(parameter, bindings[index].integer);
            break;
        case QueryBinding::Type::text:
            statement.bind(parameter, bindings[index].text);
            break;
        case QueryBinding::Type::blob:
            statement.bind_blob(parameter, bindings[index].blob);
            break;
        }
    }
}

QueryBinding integer_binding(const std::int64_t value)
{
    QueryBinding result;
    result.type = QueryBinding::Type::integer;
    result.integer = value;
    return result;
}

QueryBinding text_binding(std::string value)
{
    QueryBinding result;
    result.type = QueryBinding::Type::text;
    result.text = std::move(value);
    return result;
}

QueryBinding blob_binding(Bytes value)
{
    QueryBinding result;
    result.type = QueryBinding::Type::blob;
    result.blob = std::move(value);
    return result;
}

std::int64_t parse_timestamp_us(const std::string &value)
{
    if (!valid_timestamp(value)) {
        throw DatabaseError("invalid validated API timestamp");
    }
    std::tm broken{};
    broken.tm_year = std::stoi(value.substr(0, 4)) - 1900;
    broken.tm_mon = std::stoi(value.substr(5, 2)) - 1;
    broken.tm_mday = std::stoi(value.substr(8, 2));
    broken.tm_hour = std::stoi(value.substr(11, 2));
    broken.tm_min = std::stoi(value.substr(14, 2));
    broken.tm_sec = std::stoi(value.substr(17, 2));
    const std::time_t seconds = timegm(&broken);
    if (seconds < 0) {
        throw DatabaseError("API timestamp is outside the supported range");
    }
    const std::int64_t micros = std::stoll(value.substr(20, 6));
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1'000'000) {
        throw DatabaseError("API timestamp overflows Unix microseconds");
    }
    return static_cast<std::int64_t>(seconds) * 1'000'000 + micros;
}

std::pair<int, Bytes> parse_peer_filter(const std::string &peer)
{
    in_addr ipv4{};
    if (inet_pton(AF_INET, peer.c_str(), &ipv4) == 1) {
        const auto *const first = reinterpret_cast<const std::uint8_t *>(&ipv4);
        return {AF_INET, Bytes(first, first + sizeof(ipv4))};
    }
    in6_addr ipv6{};
    if (inet_pton(AF_INET6, peer.c_str(), &ipv6) == 1) {
        const auto *const first = reinterpret_cast<const std::uint8_t *>(&ipv6);
        return {AF_INET6, Bytes(first, first + sizeof(ipv6))};
    }
    throw DatabaseError("invalid validated API peer");
}

std::string peer_text(const int family, const Bytes &address)
{
    std::array<char, INET6_ADDRSTRLEN> encoded{};
    if (family == AF_INET && address.size() == sizeof(in_addr) &&
        inet_ntop(AF_INET, address.data(), encoded.data(), encoded.size()) !=
            nullptr) {
        return encoded.data();
    }
    if (family == AF_INET6 && address.size() == sizeof(in6_addr)) {
        const auto *const ipv6 = reinterpret_cast<const in6_addr *>(address.data());
        if (IN6_IS_ADDR_V4MAPPED(ipv6)) {
            if (inet_ntop(AF_INET,
                          address.data() + 12,
                          encoded.data(),
                          encoded.size()) != nullptr) {
                return encoded.data();
            }
        }
        if (inet_ntop(AF_INET6,
                      address.data(),
                      encoded.data(),
                      encoded.size()) != nullptr) {
            return encoded.data();
        }
    }
    throw DatabaseError("database contains an invalid peer address");
}

Json nullable_text(ReadStatement &statement, const int column)
{
    return statement.is_null(column) ? Json(nullptr) : Json(statement.text(column));
}

Json nullable_decimal(ReadStatement &statement, const int column)
{
    return statement.is_null(column) ? Json(nullptr) :
                                       Json(std::to_string(statement.integer(column)));
}

Json nullable_timestamp(ReadStatement &statement, const int column)
{
    return statement.is_null(column) ? Json(nullptr) :
                                       Json(format_rfc3339_utc_us(
                                           statement.integer(column)));
}

Json nullable_blob_hex(ReadStatement &statement, const int column)
{
    return statement.is_null(column) ? Json(nullptr) :
                                       Json(hex_encode(statement.blob(column)));
}

std::string divide_decimal_string(const std::string &value,
                                  const std::uint32_t divisor)
{
    if (!decimal_string(value) || divisor == 0) {
        throw DatabaseError("invalid decimal division in API hashrate");
    }
    std::string quotient;
    quotient.reserve(value.size());
    std::uint64_t remainder = 0;
    for (const char digit : value) {
        const std::uint64_t current = remainder * 10U +
                                      static_cast<unsigned>(digit - '0');
        const char result_digit = static_cast<char>('0' + current / divisor);
        if (!quotient.empty() || result_digit != '0') quotient.push_back(result_digit);
        remainder = current % divisor;
    }
    return quotient.empty() ? "0" : quotient;
}

class SqliteApiReader final {
public:
    SqliteApiReader(DatabaseOptions options,
                    ApiConfig api,
                    const HashrateSource source,
                    std::function<std::int64_t()> clock,
                    std::function<DatabaseWriterStats()> writer_stats)
        : options_(std::move(options)), api_(std::move(api)), source_(source), clock_(std::move(clock)),
          writer_stats_(std::move(writer_stats))
    {
        if (options_.path.empty()) {
            throw std::invalid_argument("SQLite API reader path is empty");
        }
        if (api_.top_shares_limit == 0 || api_.top_shares_limit > 100 ||
            api_.recent_high_shares_limit == 0 ||
            api_.recent_high_shares_limit > 100 ||
            api_.recent_high_share_min_difficulty == 0) {
            throw std::invalid_argument(
                "invalid API share-statistics configuration");
        }
        sqlite3 *opened = nullptr;
        const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                          SQLITE_OPEN_NOFOLLOW;
        const int status = sqlite3_open_v2(options_.path.c_str(), &opened, flags, nullptr);
        database_ = opened;
        if (status != SQLITE_OK) {
            const std::string message = database_ == nullptr ?
                                            sqlite3_errstr(status) :
                                            sqlite3_errmsg(database_);
            if (database_ != nullptr) (void)sqlite3_close_v2(database_);
            database_ = nullptr;
            throw DatabaseError("open read-only API database: " + message);
        }
        if (sqlite3_busy_timeout(database_,
                                 static_cast<int>(options_.busy_timeout_ms)) !=
            SQLITE_OK) {
            const std::string message = sqlite3_errmsg(database_);
            (void)sqlite3_close_v2(database_);
            database_ = nullptr;
            throw DatabaseError("configure read-only API database: " + message);
        }
        try {
            char *error = nullptr;
            if (sqlite3_exec(database_, "PRAGMA foreign_keys=ON", nullptr,
                             nullptr, &error) != SQLITE_OK) {
                const std::string message = error == nullptr ?
                                                sqlite3_errmsg(database_) : error;
                sqlite3_free(error);
                throw DatabaseError(
                    "configure API reader foreign keys: " + message);
            }
            ReadStatement schema(database_,
                                 "SELECT value FROM schema_meta "
                                 "WHERE key='schema_version'");
            if (!schema.row() || schema.text(0) != "3" || schema.row()) {
                throw DatabaseError(
                    "read-only API database schema is not version 3");
            }
        }
        catch (...) {
            (void)sqlite3_close_v2(database_);
            database_ = nullptr;
            throw;
        }
    }

    ~SqliteApiReader()
    {
        if (database_ != nullptr) (void)sqlite3_close_v2(database_);
    }

    SqliteApiReader(const SqliteApiReader &) = delete;
    SqliteApiReader &operator=(const SqliteApiReader &) = delete;

    [[nodiscard]] ApiCollectionResult collection(const ApiCollectionRequest &request);
    [[nodiscard]] std::optional<Json> detail(const ApiDetailRequest &request);
    [[nodiscard]] std::optional<Json> singleton(ApiSingleton resource);
    [[nodiscard]] Json summary(Json live_summary,
                               std::optional<Json> live_daemon);

private:
    using RowFormatter = std::function<Json(ReadStatement &)>;

    [[nodiscard]] ApiCollectionResult run_collection(
        std::string sql,
        std::vector<QueryBinding> bindings,
        const ApiCollectionRequest &request,
        const RowFormatter &formatter,
        bool synthetic_hash_cursor = false);
    [[nodiscard]] ApiCollectionResult run_bounded_collection(
        std::string sql,
        std::vector<QueryBinding> bindings,
        const ApiCollectionRequest &request,
        const RowFormatter &formatter);
    [[nodiscard]] Json connection_resource(ReadStatement &statement);
    [[nodiscard]] Json worker_resource(ReadStatement &statement);
    [[nodiscard]] Json share_resource(ReadStatement &statement);
    [[nodiscard]] Json hash_resource(ReadStatement &statement);
    [[nodiscard]] Json submission_resource(ReadStatement &statement);
    [[nodiscard]] Json round_resource(ReadStatement &statement);
    [[nodiscard]] Json round_effort(
        std::int64_t round_id,
        bool finalized,
        std::optional<std::uint64_t> finalized_segment_count);
    [[nodiscard]] Json ban_resource(ReadStatement &statement);
    [[nodiscard]] Json event_resource(ReadStatement &statement);
    [[nodiscard]] HashrateWindows hashrate(std::string_view scope,
                                           std::int64_t scope_id,
                                           std::int64_t now_second);
    [[nodiscard]] Json hashrate_resource(std::string_view scope,
                                         std::int64_t scope_id);
    [[nodiscard]] std::optional<Json> connection_detail(std::string_view id);
    [[nodiscard]] std::optional<Json> share_detail(std::string_view id);
    [[nodiscard]] std::optional<Json> submission_detail(std::string_view id,
                                                        bool include_blobs);
    [[nodiscard]] std::optional<Json> current_round();
    [[nodiscard]] Json persistence();

    DatabaseOptions options_;
    ApiConfig api_;
    HashrateSource source_;
    std::function<std::int64_t()> clock_;
    std::function<DatabaseWriterStats()> writer_stats_;
    sqlite3 *database_{};
    std::mutex mutex_;
};

HashrateWindows SqliteApiReader::hashrate(const std::string_view scope,
                                          const std::int64_t scope_id,
                                          const std::int64_t now_second)
{
    constexpr std::array<std::uint32_t, 6> windows = {
        60, 300, 600, 3600, 21600, 86400,
    };
    std::array<std::string, 6> work = {"0", "0", "0", "0", "0", "0"};
    ReadStatement statement(
        database_,
        "SELECT second_utc,credited_difficulty_dec FROM hashrate_buckets "
        "WHERE scope_type=?1 AND scope_id=?2 AND source=?3 "
        "AND second_utc>?4 AND second_utc<=?5 ORDER BY second_utc");
    statement.bind(1, scope);
    statement.bind(2, scope_id);
    statement.bind(3, to_string(source_));
    statement.bind(4, now_second - static_cast<std::int64_t>(windows.back()));
    statement.bind(5, now_second);
    while (statement.row()) {
        const std::int64_t second = statement.integer(0);
        const std::string difficulty = statement.text(1);
        for (std::size_t index = 0; index < windows.size(); ++index) {
            if (second > now_second - static_cast<std::int64_t>(windows[index])) {
                work[index] = add_decimal(work[index], difficulty);
            }
        }
    }
    return {
        divide_decimal_string(work[0], windows[0]),
        divide_decimal_string(work[1], windows[1]),
        divide_decimal_string(work[2], windows[2]),
        divide_decimal_string(work[3], windows[3]),
        divide_decimal_string(work[4], windows[4]),
        divide_decimal_string(work[5], windows[5]),
    };
}

Json SqliteApiReader::hashrate_resource(const std::string_view scope,
                                        const std::int64_t scope_id)
{
    const std::int64_t now_second = clock_() / 1'000'000;
    return hashrate_json(hashrate(scope, scope_id, now_second),
                         std::string(to_string(source_)));
}

ApiCollectionResult SqliteApiReader::run_collection(
    std::string sql,
    std::vector<QueryBinding> bindings,
    const ApiCollectionRequest &request,
    const RowFormatter &formatter,
    const bool synthetic_hash_cursor)
{
    sql += " LIMIT ?" + std::to_string(bindings.size() + 1U);
    bindings.push_back(integer_binding(
        static_cast<std::int64_t>(request.limit) + 1));
    ReadStatement statement(database_, sql);
    apply_bindings(statement, bindings);
    ApiCollectionResult result;
    std::vector<std::uint64_t> cursor_ids;
    while (statement.row()) {
        const std::int64_t raw_id = statement.integer(0);
        if (raw_id <= 0) throw DatabaseError("API row has invalid database ID");
        std::uint64_t cursor_id = static_cast<std::uint64_t>(raw_id);
        if (synthetic_hash_cursor) {
            const std::string role = statement.text(1);
            cursor_id = cursor_id * 2U + (role == "computed" ? 1U : 0U);
        }
        result.rows.push_back(formatter(statement));
        cursor_ids.push_back(cursor_id);
        if (result.rows.size() > request.limit) break;
    }
    if (result.rows.size() > request.limit) {
        result.rows.erase(result.rows.end() - 1);
        result.next_database_id = cursor_ids[request.limit - 1U];
    }
    return result;
}

ApiCollectionResult SqliteApiReader::run_bounded_collection(
    std::string sql,
    std::vector<QueryBinding> bindings,
    const ApiCollectionRequest &request,
    const RowFormatter &formatter)
{
    if (request.after_database_id != 0) {
        throw DatabaseError("bounded API ranking received a cursor");
    }
    sql += " LIMIT ?" + std::to_string(bindings.size() + 1U);
    bindings.push_back(integer_binding(static_cast<std::int64_t>(request.limit)));
    ReadStatement statement(database_, sql);
    apply_bindings(statement, bindings);
    ApiCollectionResult result;
    while (statement.row()) {
        if (statement.integer(0) <= 0) {
            throw DatabaseError("API row has invalid database ID");
        }
        result.rows.push_back(formatter(statement));
    }
    return result;
}

Json SqliteApiReader::connection_resource(ReadStatement &row)
{
    return Json{
        {"id", hex_encode(row.blob(1))},
        {"session_id", hex_encode(row.blob(2))},
        {"worker_id", nullable_decimal(row, 3)},
        {"peer", peer_text(static_cast<int>(row.integer(4)), row.blob(5))},
        {"peer_port", row.integer(6)},
        {"listen_address", row.text(7)},
        {"agent", row.text(8)},
        {"opened_at", format_rfc3339_utc_us(row.integer(9))},
        {"authenticated_at", nullable_timestamp(row, 10)},
        {"closed_at", nullable_timestamp(row, 11)},
        {"close_reason", nullable_text(row, 12)},
        {"last_sent_height", row.integer(13)},
        {"rx_bytes", std::to_string(row.integer(14))},
        {"tx_bytes", std::to_string(row.integer(15))},
        {"active", row.is_null(11)},
        {"hashrate", hashrate_resource("connection", row.integer(0))},
    };
}

Json SqliteApiReader::worker_resource(ReadStatement &row)
{
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"login", row.text(1)},
        {"rigid", row.text(2)},
        {"first_seen_at", format_rfc3339_utc_us(row.integer(3))},
        {"last_seen_at", format_rfc3339_utc_us(row.integer(4))},
        {"active_connections", row.integer(5)},
        {"accepted_shares", std::to_string(row.integer(6))},
        {"rejected_shares", std::to_string(row.integer(7))},
        {"share_counts_retained_only", true},
        {"hashrate", hashrate_resource("worker", row.integer(0))},
    };
}

Json SqliteApiReader::share_resource(ReadStatement &row)
{
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"connection_id", nullable_blob_hex(row, 1)},
        {"worker_id", nullable_decimal(row, 2)},
        {"job_id", nullable_blob_hex(row, 3)},
        {"template_generation", nullable_decimal(row, 35)},
        {"request_sequence", std::to_string(row.integer(4))},
        {"miner_request_id_type", nullable_text(row, 5)},
        {"miner_request_id", nullable_text(row, 6)},
        {"received_at", format_rfc3339_utc_us(row.integer(7))},
        {"completed_at", nullable_timestamp(row, 8)},
        {"nonce", nullable_blob_hex(row, 9)},
        {"height", row.is_null(10) ? Json(nullptr) : Json(row.integer(10))},
        {"assigned_difficulty", nullable_text(row, 11)},
        {"actual_difficulty", nullable_text(row, 12)},
        {"network_difficulty", nullable_text(row, 13)},
        {"height_is_older", row.integer(14) != 0},
        {"claimed_candidate", row.integer(15) != 0},
        {"candidate_admission", row.text(16)},
        {"retention_reason", nullable_text(row, 36)},
        {"status", row.text(17)},
        {"error_code", nullable_text(row, 18)},
        {"error_message", nullable_text(row, 19)},
        {"provenance", row.text(20)},
        {"credited_difficulty", nullable_text(row, 21)},
        {"verifier_ticket", nullable_text(row, 22)},
        {"verifier_seed_id", nullable_text(row, 23)},
        {"verifier_queue_ns", nullable_decimal(row, 24)},
        {"verifier_hash_ns", nullable_decimal(row, 25)},
        {"verifier_total_ns", nullable_decimal(row, 26)},
        {"claimed_hash", nullable_blob_hex(row, 27)},
        {"computed_hash", nullable_blob_hex(row, 28)},
        {"claimed_meets_share_target",
         row.is_null(29) ? Json(nullptr) : Json(row.integer(29) != 0)},
        {"computed_meets_share_target",
         row.is_null(30) ? Json(nullptr) : Json(row.integer(30) != 0)},
        {"claimed_meets_network_target",
         row.is_null(31) ? Json(nullptr) : Json(row.integer(31) != 0)},
        {"computed_meets_network_target",
         row.is_null(32) ? Json(nullptr) : Json(row.integer(32) != 0)},
        {"candidate_id", nullable_decimal(row, 33)},
        {"round_id", nullable_decimal(row, 34)},
    };
}

Json SqliteApiReader::hash_resource(ReadStatement &row)
{
    return Json{
        {"share_id", std::to_string(row.integer(0))},
        {"role", row.text(1)},
        {"hash", hex_encode(row.blob(2))},
        {"meets_share_target",
         row.is_null(3) ? Json(nullptr) : Json(row.integer(3) != 0)},
        {"meets_network_target",
         row.is_null(4) ? Json(nullptr) : Json(row.integer(4) != 0)},
        {"received_at", format_rfc3339_utc_us(row.integer(5))},
        {"share_status", row.text(6)},
        {"connection_id", nullable_blob_hex(row, 7)},
        {"worker_id", nullable_decimal(row, 8)},
        {"job_id", nullable_blob_hex(row, 9)},
        {"assigned_difficulty", nullable_text(row, 10)},
        {"actual_difficulty", nullable_text(row, 11)},
        {"network_difficulty", nullable_text(row, 12)},
        {"credited_difficulty", nullable_text(row, 13)},
        {"provenance", row.text(14)},
        {"round_id", nullable_decimal(row, 15)},
    };
}

Json SqliteApiReader::submission_resource(ReadStatement &row)
{
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"candidate_key", hex_encode(row.blob(1))},
        {"first_share_id", nullable_decimal(row, 2)},
        {"job_id", hex_encode(row.blob(3))},
        {"template_generation", std::to_string(row.integer(22))},
        {"round_id", std::to_string(row.integer(23))},
        {"connection_id", hex_encode(row.blob(4))},
        {"height", row.integer(5)},
        {"peer", peer_text(static_cast<int>(row.integer(6)), row.blob(7))},
        {"miner_tx_hash", hex_encode(row.blob(8))},
        {"expected_block_id", nullable_blob_hex(row, 9)},
        {"canonical_block_id", nullable_blob_hex(row, 10)},
        {"state", row.text(11)},
        {"attempt_count", row.integer(12)},
        {"max_attempts", row.integer(13)},
        {"had_indeterminate", row.integer(14) != 0},
        {"reconciliation_cycle_count", row.integer(15)},
        {"next_reconciliation_at", nullable_timestamp(row, 16)},
        {"reconciliation_exhausted_at", nullable_timestamp(row, 17)},
        {"created_at", format_rfc3339_utc_us(row.integer(18))},
        {"updated_at", format_rfc3339_utc_us(row.integer(19))},
        {"accepted_at", nullable_timestamp(row, 20)},
        {"terminal_reason", nullable_text(row, 21)},
    };
}

Json SqliteApiReader::round_resource(ReadStatement &row)
{
    const bool finalized = !row.is_null(10);
    std::optional<std::uint64_t> finalized_segment_count;
    if (!row.is_null(11)) {
        finalized_segment_count =
            static_cast<std::uint64_t>(row.integer(11));
    }
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"opened_at", format_rfc3339_utc_us(row.integer(1))},
        {"closed_at", nullable_timestamp(row, 2)},
        {"state", row.text(3)},
        {"accepted_candidate_id", nullable_decimal(row, 4)},
        {"accepted_height", row.is_null(5) ? Json(nullptr) : Json(row.integer(5))},
        {"miner_tx_hash", nullable_blob_hex(row, 6)},
        {"block_id", nullable_blob_hex(row, 7)},
        {"credited_difficulty", row.text(8)},
        {"estimated_hashes", row.text(8)},
        {"accepted_share_count", std::to_string(row.integer(9))},
        {"max_share_height", row.integer(12)},
        {"effort_finalized_at", nullable_timestamp(row, 10)},
        {"effort", round_effort(row.integer(0), finalized,
                                finalized_segment_count)},
    };
}

Json SqliteApiReader::round_effort(
    const std::int64_t round_id,
    const bool finalized,
    const std::optional<std::uint64_t> finalized_segment_count)
{
    Json segments = Json::array();
    ExactEffort total;
    ReadStatement rows(
        database_,
        "SELECT source,network_difficulty_dec,credited_difficulty_dec,"
        "accepted_share_count FROM round_work_segments WHERE round_id=?1 "
        "ORDER BY source,length(network_difficulty_dec),network_difficulty_dec");
    rows.bind(1, round_id);
    while (rows.row()) {
        const std::string source = rows.text(0);
        const std::string network_difficulty = rows.text(1);
        const std::string estimated_hashes = rows.text(2);
        total.add(estimated_hashes, network_difficulty);
        segments.push_back({
            {"source", source},
            {"network_difficulty", network_difficulty},
            {"estimated_hashes", estimated_hashes},
            {"accepted_share_count", std::to_string(rows.integer(3))},
            {"effort_percent", format_micro_percent(
                effort_micro_percent(estimated_hashes,
                                     network_difficulty))},
        });
    }
    if (finalized_segment_count.has_value() &&
        *finalized_segment_count != segments.size()) {
        throw DatabaseError("finalized round effort segment count changed");
    }
    return {
        {"unit", "percent"},
        {"value", format_micro_percent(total.micro_percent())},
        {"precision", "0.000001"},
        {"rounding", "down"},
        {"basis", "credited_assigned_difficulty/network_difficulty"},
        {"finalized", finalized},
        {"segments", std::move(segments)},
    };
}

Json SqliteApiReader::ban_resource(ReadStatement &row)
{
    Json evidence = Json::array();
    ReadStatement links(
        database_,
        "SELECT abuse_event_id FROM ban_abuse_events WHERE ban_id=?1 "
        "ORDER BY abuse_event_id");
    links.bind(1, row.integer(0));
    while (links.row()) evidence.push_back(std::to_string(links.integer(0)));
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"peer", peer_text(static_cast<int>(row.integer(1)), row.blob(2))},
        {"created_at", format_rfc3339_utc_us(row.integer(3))},
        {"expires_at", format_rfc3339_utc_us(row.integer(4))},
        {"evidence_window_started_at", format_rfc3339_utc_us(row.integer(5))},
        {"evidence_window_ended_at", format_rfc3339_utc_us(row.integer(6))},
        {"reason", row.text(7)},
        {"active", row.integer(8) != 0},
        {"abuse_event_ids", std::move(evidence)},
    };
}

Json SqliteApiReader::event_resource(ReadStatement &row)
{
    Json payload = Json::parse(row.text(11), nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        throw DatabaseError("persisted API event payload is invalid JSON");
    }
    return Json{
        {"id", std::to_string(row.integer(0))},
        {"session_id", hex_encode(row.blob(1))},
        {"created_at", format_rfc3339_utc_us(row.integer(2))},
        {"type", row.text(3)},
        {"connection_id", nullable_blob_hex(row, 4)},
        {"worker_id", nullable_decimal(row, 5)},
        {"template_generation", nullable_decimal(row, 6)},
        {"job_id", nullable_blob_hex(row, 7)},
        {"share_id", nullable_decimal(row, 8)},
        {"candidate_id", nullable_decimal(row, 9)},
        {"round_id", nullable_decimal(row, 10)},
        {"payload", std::move(payload)},
    };
}

ApiCollectionResult SqliteApiReader::collection(
    const ApiCollectionRequest &request)
{
    std::scoped_lock lock(mutex_);
    if (request.resource != ApiCollection::hashes &&
        request.after_database_id >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return {};
    }
    std::vector<QueryBinding> bindings;
    std::string sql;
    const auto placeholder = [&]() {
        return "?" + std::to_string(bindings.size() + 1U);
    };
    const auto add_integer = [&](const std::string_view column,
                                 const std::int64_t value) {
        sql += " AND " + std::string(column) + "=" + placeholder();
        bindings.push_back(integer_binding(value));
    };
    const auto add_text = [&](const std::string_view column,
                              const std::string &value) {
        sql += " AND " + std::string(column) + "=" + placeholder();
        bindings.push_back(text_binding(value));
    };
    const auto add_blob = [&](const std::string_view column,
                              const std::string &hex) {
        sql += " AND " + std::string(column) + "=" + placeholder();
        bindings.push_back(blob_binding(hex_decode(hex)));
    };
    const auto add_time = [&](const std::string_view column,
                              const char *const name,
                              const bool after) {
        if (const auto value = request.filters.find(name);
            value != request.filters.end()) {
            sql += " AND " + std::string(column) + (after ? ">=" : "<=") +
                   placeholder();
            bindings.push_back(integer_binding(parse_timestamp_us(value->second)));
        }
    };
    const auto add_peer = [&](const std::string_view family_column,
                              const std::string_view address_column) {
        if (const auto peer = request.filters.find("peer");
            peer != request.filters.end()) {
            const auto [family, address] = parse_peer_filter(peer->second);
            sql += " AND " + std::string(family_column) + "=" + placeholder();
            bindings.push_back(integer_binding(family));
            sql += " AND " + std::string(address_column) + "=" + placeholder();
            bindings.push_back(blob_binding(address));
        }
    };
    const auto add_list = [&](const std::string_view column,
                              const std::string &value) {
        sql += " AND " + std::string(column) + " IN (";
        std::size_t begin = 0;
        bool first = true;
        for (;;) {
            const std::size_t comma = value.find(',', begin);
            const std::string item = value.substr(
                begin, comma == std::string::npos ? value.size() - begin :
                                                    comma - begin);
            if (!first) sql += ',';
            first = false;
            sql += placeholder();
            bindings.push_back(text_binding(item));
            if (comma == std::string::npos) break;
            begin = comma + 1U;
        }
        sql += ')';
    };
    const auto add_after_id = [&](const std::string_view column) {
        sql += " AND " + std::string(column) + ">" + placeholder();
        bindings.push_back(integer_binding(
            static_cast<std::int64_t>(request.after_database_id)));
    };

    switch (request.resource) {
    case ApiCollection::connections: {
        sql =
            "SELECT c.id,c.public_id,ss.public_id,c.worker_id,c.peer_family,"
            "c.peer_address,c.peer_port,c.listen_address,c.agent,c.opened_unix_us,"
            "c.authenticated_unix_us,c.closed_unix_us,c.close_reason,"
            "c.last_sent_height,c.rx_bytes,c.tx_bytes FROM connections c "
            "JOIN server_sessions ss ON ss.id=c.session_id WHERE 1=1";
        add_after_id("c.id");
        if (const auto active = request.filters.find("active");
            active != request.filters.end()) {
            sql += active->second == "true" ? " AND c.closed_unix_us IS NULL" :
                                              " AND c.closed_unix_us IS NOT NULL";
        }
        if (const auto worker = request.filters.find("worker_id");
            worker != request.filters.end()) {
            add_integer("c.worker_id", std::stoll(worker->second));
        }
        add_peer("c.peer_family", "c.peer_address");
        add_time("c.opened_unix_us", "after_time", true);
        add_time("c.opened_unix_us", "before_time", false);
        sql += " ORDER BY c.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return connection_resource(row); });
    }
    case ApiCollection::workers: {
        sql =
            "SELECT w.id,w.login,w.rigid,w.first_seen_unix_us,w.last_seen_unix_us,"
            "(SELECT count(*) FROM connections c WHERE c.worker_id=w.id AND "
            "c.closed_unix_us IS NULL),"
            "(SELECT count(*) FROM shares s WHERE s.worker_id=w.id AND "
            "s.status='accepted'),"
            "(SELECT count(*) FROM shares s WHERE s.worker_id=w.id AND s.status "
            "NOT IN ('received','verifying','accepted')) FROM workers w WHERE 1=1";
        add_after_id("w.id");
        if (const auto active = request.filters.find("active");
            active != request.filters.end()) {
            sql += active->second == "true" ?
                       " AND EXISTS(SELECT 1 FROM connections ac WHERE "
                       "ac.worker_id=w.id AND ac.closed_unix_us IS NULL)" :
                       " AND NOT EXISTS(SELECT 1 FROM connections ac WHERE "
                       "ac.worker_id=w.id AND ac.closed_unix_us IS NULL)";
        }
        if (const auto login = request.filters.find("login");
            login != request.filters.end()) add_text("w.login", login->second);
        if (const auto rigid = request.filters.find("rigid");
            rigid != request.filters.end()) add_text("w.rigid", rigid->second);
        add_time("w.first_seen_unix_us", "after_time", true);
        add_time("w.first_seen_unix_us", "before_time", false);
        sql += " ORDER BY w.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return worker_resource(row); });
    }
    case ApiCollection::shares: {
        sql =
            "SELECT s.id,c.public_id,s.worker_id,s.job_public_id,"
            "s.request_sequence,s.miner_request_id_type,s.miner_request_id_text,"
            "s.received_unix_us,s.completed_unix_us,s.nonce,s.height,"
            "s.assigned_difficulty_dec,s.actual_difficulty_dec,"
            "s.network_difficulty_dec,s.height_is_older,s.claimed_candidate,"
            "s.candidate_admission,s.status,s.error_code,s.error_message,"
            "s.provenance,s.credited_difficulty_dec,s.verifier_ticket_dec,"
            "s.verifier_seed_id_dec,s.verifier_queue_ns,s.verifier_hash_ns,"
            "s.verifier_total_ns,ch.hash,co.hash,ch.meets_share_target,"
            "co.meets_share_target,ch.meets_network_target,co.meets_network_target,"
            "s.candidate_id,s.round_id,s.template_generation,s.retention_reason "
            "FROM shares s LEFT JOIN connections c ON c.id=s.connection_id "
            "LEFT JOIN share_hashes ch ON ch.share_id=s.id AND ch.role='claimed' "
            "LEFT JOIN share_hashes co ON co.share_id=s.id AND co.role='computed' "
            "WHERE 1=1";
        add_after_id("s.id");
        if (const auto status = request.filters.find("status");
            status != request.filters.end()) add_list("s.status", status->second);
        if (const auto connection = request.filters.find("connection_id");
            connection != request.filters.end()) add_blob("c.public_id", connection->second);
        if (const auto worker = request.filters.find("worker_id");
            worker != request.filters.end()) add_integer("s.worker_id", std::stoll(worker->second));
        if (const auto job = request.filters.find("job_id");
            job != request.filters.end()) add_blob("s.job_public_id", job->second);
        if (const auto candidate = request.filters.find("candidate_id");
            candidate != request.filters.end()) add_integer("s.candidate_id", std::stoll(candidate->second));
        if (const auto height = request.filters.find("height");
            height != request.filters.end()) add_integer("s.height", std::stoll(height->second));
        if (const auto minimum = request.filters.find("min_difficulty");
            minimum != request.filters.end()) {
            const std::string greater_length = placeholder();
            bindings.push_back(text_binding(minimum->second));
            const std::string equal_length = placeholder();
            bindings.push_back(text_binding(minimum->second));
            const std::string lexical_minimum = placeholder();
            bindings.push_back(text_binding(minimum->second));
            sql += " AND (length(s.assigned_difficulty_dec)>length(" +
                   greater_length + ") OR (length(s.assigned_difficulty_dec)="
                   "length(" + equal_length + ") AND "
                   "s.assigned_difficulty_dec>=" + lexical_minimum + "))";
        }
        add_time("s.received_unix_us", "after_time", true);
        add_time("s.received_unix_us", "before_time", false);
        sql += " ORDER BY s.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return share_resource(row); });
    }
    case ApiCollection::top_shares:
    case ApiCollection::recent_high_shares: {
        const std::uint64_t configured_limit =
            request.resource == ApiCollection::top_shares ?
                api_.top_shares_limit : api_.recent_high_shares_limit;
        ApiCollectionRequest bounded_request = request;
        bounded_request.limit = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(request.limit, configured_limit));
        sql =
            "SELECT s.id,c.public_id,s.worker_id,s.job_public_id,"
            "s.request_sequence,s.miner_request_id_type,s.miner_request_id_text,"
            "s.received_unix_us,s.completed_unix_us,s.nonce,s.height,"
            "s.assigned_difficulty_dec,s.actual_difficulty_dec,"
            "s.network_difficulty_dec,s.height_is_older,s.claimed_candidate,"
            "s.candidate_admission,s.status,s.error_code,s.error_message,"
            "s.provenance,s.credited_difficulty_dec,s.verifier_ticket_dec,"
            "s.verifier_seed_id_dec,s.verifier_queue_ns,s.verifier_hash_ns,"
            "s.verifier_total_ns,ch.hash,co.hash,ch.meets_share_target,"
            "co.meets_share_target,ch.meets_network_target,co.meets_network_target,"
            "s.candidate_id,s.round_id,s.template_generation,s.retention_reason "
            "FROM shares s LEFT JOIN connections c ON c.id=s.connection_id "
            "LEFT JOIN share_hashes ch ON ch.share_id=s.id AND ch.role='claimed' "
            "LEFT JOIN share_hashes co ON co.share_id=s.id AND co.role='computed' "
            "WHERE s.status='accepted' AND s.actual_difficulty_dec IS NOT NULL";
        if (request.resource == ApiCollection::top_shares) {
            if (const auto round = request.filters.find("round_id");
                round != request.filters.end()) {
                add_integer("s.round_id", std::stoll(round->second));
            }
            sql += " ORDER BY length(s.actual_difficulty_dec) DESC,"
                   "s.actual_difficulty_dec DESC,s.id DESC";
        }
        else {
            const std::string minimum = std::to_string(
                api_.recent_high_share_min_difficulty);
            const std::string greater_length = placeholder();
            bindings.push_back(text_binding(minimum));
            const std::string equal_length = placeholder();
            bindings.push_back(text_binding(minimum));
            const std::string lexical_minimum = placeholder();
            bindings.push_back(text_binding(minimum));
            sql += " AND (length(s.actual_difficulty_dec)>length(" +
                   greater_length + ") OR (length(s.actual_difficulty_dec)="
                   "length(" + equal_length + ") AND "
                   "s.actual_difficulty_dec>=" + lexical_minimum + "))"
                   " ORDER BY s.received_unix_us DESC,s.id DESC";
        }
        return run_bounded_collection(
            std::move(sql), std::move(bindings), bounded_request,
            [this](ReadStatement &row) { return share_resource(row); });
    }
    case ApiCollection::hashes: {
        sql =
            "SELECT sh.share_id,sh.role,sh.hash,sh.meets_share_target,"
            "sh.meets_network_target,s.received_unix_us,s.status,c.public_id,"
            "s.worker_id,s.job_public_id,s.assigned_difficulty_dec,"
            "s.actual_difficulty_dec,s.network_difficulty_dec,"
            "s.credited_difficulty_dec,s.provenance,s.round_id "
            "FROM share_hashes sh JOIN shares s "
            "ON s.id=sh.share_id LEFT JOIN connections c "
            "ON c.id=s.connection_id WHERE 1=1";
        const std::uint64_t last_share = request.after_database_id / 2U;
        const std::uint64_t last_role = request.after_database_id % 2U;
        if (last_share > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
            return {};
        }
        sql += " AND (sh.share_id>" + placeholder();
        bindings.push_back(integer_binding(static_cast<std::int64_t>(last_share)));
        sql += " OR (sh.share_id=" + placeholder();
        bindings.push_back(integer_binding(static_cast<std::int64_t>(last_share)));
        sql += " AND CASE sh.role WHEN 'claimed' THEN 0 ELSE 1 END>" +
               placeholder() + "))";
        bindings.push_back(integer_binding(static_cast<std::int64_t>(last_role)));
        if (const auto role = request.filters.find("role");
            role != request.filters.end()) add_text("sh.role", role->second);
        if (const auto statuses = request.filters.find("share_status");
            statuses != request.filters.end()) add_list("s.status", statuses->second);
        if (const auto connection = request.filters.find("connection_id");
            connection != request.filters.end()) add_blob("c.public_id", connection->second);
        if (const auto worker = request.filters.find("worker_id");
            worker != request.filters.end()) add_integer("s.worker_id", std::stoll(worker->second));
        if (const auto job = request.filters.find("job_id");
            job != request.filters.end()) add_blob("s.job_public_id", job->second);
        add_time("s.received_unix_us", "after_time", true);
        add_time("s.received_unix_us", "before_time", false);
        sql += " ORDER BY sh.share_id,CASE sh.role WHEN 'claimed' THEN 0 ELSE 1 END";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return hash_resource(row); }, true);
    }
    case ApiCollection::submissions: {
        sql =
            "SELECT ca.id,ca.candidate_key,ca.first_share_id,ca.job_public_id,"
            "c.public_id,ca.height,ca.peer_family,ca.peer_address,ca.miner_tx_hash,"
            "ca.expected_block_id,ca.canonical_block_id,ca.state,ca.attempt_count,"
            "ca.max_attempts,ca.had_indeterminate,ca.reconciliation_cycle_count,"
            "ca.next_reconciliation_unix_us,ca.reconciliation_exhausted_unix_us,"
            "ca.created_unix_us,ca.updated_unix_us,ca.accepted_unix_us,"
            "ca.terminal_reason,ca.template_generation,ca.round_id "
            "FROM candidates ca LEFT JOIN connections c "
            "ON c.id=ca.connection_id WHERE 1=1";
        add_after_id("ca.id");
        if (const auto state = request.filters.find("state");
            state != request.filters.end()) add_list("ca.state", state->second);
        if (const auto connection = request.filters.find("connection_id");
            connection != request.filters.end()) add_blob("c.public_id", connection->second);
        if (const auto job = request.filters.find("job_id");
            job != request.filters.end()) add_blob("ca.job_public_id", job->second);
        if (const auto height = request.filters.find("height");
            height != request.filters.end()) add_integer("ca.height", std::stoll(height->second));
        add_peer("ca.peer_family", "ca.peer_address");
        add_time("ca.created_unix_us", "after_time", true);
        add_time("ca.created_unix_us", "before_time", false);
        sql += " ORDER BY ca.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return submission_resource(row); });
    }
    case ApiCollection::rounds: {
        sql =
            "SELECT r.id,r.opened_unix_us,r.closed_unix_us,r.state,"
            "r.accepted_candidate_id,r.accepted_height,r.miner_tx_hash,r.block_id,"
            "r.credited_difficulty_dec,r.accepted_share_count,"
            "r.effort_finalized_unix_us,r.finalized_effort_segment_count,"
            "r.max_share_height "
            "FROM rounds r WHERE 1=1";
        add_after_id("r.id");
        if (const auto state = request.filters.find("state");
            state != request.filters.end()) add_list("r.state", state->second);
        add_time("r.opened_unix_us", "after_time", true);
        add_time("r.opened_unix_us", "before_time", false);
        sql += " ORDER BY r.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return round_resource(row); });
    }
    case ApiCollection::bans: {
        sql =
            "SELECT b.id,b.peer_family,b.peer_address,b.created_unix_us,"
            "b.expires_unix_us,b.evidence_window_started_unix_us,"
            "b.evidence_window_ended_unix_us,b.reason,b.active FROM bans b WHERE 1=1";
        add_after_id("b.id");
        if (const auto active = request.filters.find("active");
            active != request.filters.end()) add_integer("b.active", active->second == "true" ? 1 : 0);
        add_peer("b.peer_family", "b.peer_address");
        add_time("b.created_unix_us", "after_time", true);
        add_time("b.created_unix_us", "before_time", false);
        sql += " ORDER BY b.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return ban_resource(row); });
    }
    case ApiCollection::events: {
        sql =
            "SELECT e.id,ss.public_id,e.created_unix_us,e.type,c.public_id,"
            "e.worker_id,e.template_generation,e.job_public_id,e.share_id,e.candidate_id,"
            "e.round_id,e.payload_json FROM events e JOIN server_sessions ss "
            "ON ss.id=e.session_id LEFT JOIN connections c ON c.id=e.connection_id "
            "WHERE 1=1";
        add_after_id("e.id");
        if (const auto types = request.filters.find("type");
            types != request.filters.end()) add_list("e.type", types->second);
        if (const auto connection = request.filters.find("connection_id");
            connection != request.filters.end()) add_blob("c.public_id", connection->second);
        if (const auto worker = request.filters.find("worker_id");
            worker != request.filters.end()) add_integer("e.worker_id", std::stoll(worker->second));
        if (const auto generation = request.filters.find("template_generation");
            generation != request.filters.end()) {
            add_integer("e.template_generation", std::stoll(generation->second));
        }
        if (const auto job = request.filters.find("job_id");
            job != request.filters.end()) add_blob("e.job_public_id", job->second);
        if (const auto share = request.filters.find("share_id");
            share != request.filters.end()) add_integer("e.share_id", std::stoll(share->second));
        if (const auto candidate = request.filters.find("candidate_id");
            candidate != request.filters.end()) add_integer("e.candidate_id", std::stoll(candidate->second));
        if (const auto round = request.filters.find("round_id");
            round != request.filters.end()) add_integer("e.round_id", std::stoll(round->second));
        add_time("e.created_unix_us", "after_time", true);
        add_time("e.created_unix_us", "before_time", false);
        sql += " ORDER BY e.id";
        return run_collection(
            std::move(sql), std::move(bindings), request,
            [this](ReadStatement &row) { return event_resource(row); });
    }
    }
    throw DatabaseError("unknown API collection resource");
}

std::optional<Json> SqliteApiReader::connection_detail(const std::string_view id)
{
    ReadStatement row(
        database_,
        "SELECT c.id,c.public_id,ss.public_id,c.worker_id,c.peer_family,"
        "c.peer_address,c.peer_port,c.listen_address,c.agent,c.opened_unix_us,"
        "c.authenticated_unix_us,c.closed_unix_us,c.close_reason,c.last_sent_height,"
        "c.rx_bytes,c.tx_bytes FROM connections c JOIN server_sessions ss "
        "ON ss.id=c.session_id WHERE c.public_id=?1");
    row.bind_blob(1, hex_decode(id));
    if (!row.row()) return std::nullopt;
    const std::int64_t connection_id = row.integer(0);
    Json connection = connection_resource(row);
    if (row.row()) throw DatabaseError("duplicate public connection ID");

    Json counters = {{"pending", "0"},
                     {"accepted", "0"},
                     {"stale", "0"},
                     {"duplicate", "0"},
                     {"low_difficulty", "0"},
                     {"invalid_result", "0"},
                     {"infrastructure_failed", "0"},
                     {"total", "0"}};
    ReadStatement count(
        database_,
        "SELECT status,count(*) FROM shares WHERE connection_id=?1 GROUP BY status");
    count.bind(1, connection_id);
    std::uint64_t total = 0;
    std::uint64_t infrastructure = 0;
    std::uint64_t pending = 0;
    while (count.row()) {
        const std::string status = count.text(0);
        const std::int64_t signed_value = count.integer(1);
        if (signed_value < 0) throw DatabaseError("negative API share count");
        const std::uint64_t value = static_cast<std::uint64_t>(signed_value);
        total += value;
        if (status == "received" || status == "verifying") pending += value;
        else if (status == "server_busy" || status == "verifier_failed" ||
                 status == "cancelled") infrastructure += value;
        else if (counters.contains(status)) counters[status] = std::to_string(value);
    }
    counters["pending"] = std::to_string(pending);
    counters["infrastructure_failed"] = std::to_string(infrastructure);
    counters["total"] = std::to_string(total);
    counters["retained_only"] = true;

    Json jobs = Json::array();
    Json shares = Json::array();
    ReadStatement recent_shares(
        database_,
        "SELECT id FROM shares WHERE connection_id=?1 ORDER BY id DESC LIMIT 20");
    recent_shares.bind(1, connection_id);
    while (recent_shares.row()) {
        const std::string share_id = std::to_string(recent_shares.integer(0));
        shares.push_back({{"id", share_id}, {"url", "/v1/shares/" + share_id}});
    }
    return Json{{"connection", std::move(connection)},
                {"counters", std::move(counters)},
                {"recent", {{"jobs", std::move(jobs)},
                             {"shares", std::move(shares)}}}};
}

std::optional<Json> SqliteApiReader::share_detail(const std::string_view id)
{
    ReadStatement row(
        database_,
        "SELECT s.id,c.public_id,s.worker_id,s.job_public_id,s.request_sequence,"
        "s.miner_request_id_type,s.miner_request_id_text,s.received_unix_us,"
        "s.completed_unix_us,s.nonce,s.height,s.assigned_difficulty_dec,"
        "s.actual_difficulty_dec,s.network_difficulty_dec,s.height_is_older,"
        "s.claimed_candidate,s.candidate_admission,s.status,s.error_code,"
        "s.error_message,s.provenance,s.credited_difficulty_dec,"
        "s.verifier_ticket_dec,s.verifier_seed_id_dec,s.verifier_queue_ns,"
        "s.verifier_hash_ns,s.verifier_total_ns,ch.hash,co.hash,"
        "ch.meets_share_target,co.meets_share_target,ch.meets_network_target,"
        "co.meets_network_target,s.candidate_id,s.round_id,s.template_generation,"
        "s.retention_reason FROM shares s LEFT JOIN connections c "
        "ON c.id=s.connection_id "
        "LEFT JOIN share_hashes ch ON ch.share_id=s.id AND ch.role='claimed' "
        "LEFT JOIN share_hashes co ON co.share_id=s.id AND co.role='computed' "
        "WHERE s.id=?1");
    row.bind(1, std::stoll(std::string(id)));
    if (!row.row()) return std::nullopt;
    Json share = share_resource(row);
    const Json submission_url = row.is_null(33) ? Json(nullptr) :
                                Json("/v1/submissions/" +
                                     std::to_string(row.integer(33)));
    if (row.row()) throw DatabaseError("duplicate share database ID");
    return Json{{"share", std::move(share)}, {"submission_url", submission_url}};
}

std::optional<Json> SqliteApiReader::submission_detail(
    const std::string_view id,
    const bool include_blobs)
{
    const std::int64_t candidate_id = std::stoll(std::string(id));
    ReadStatement row(
        database_,
        "SELECT ca.id,ca.candidate_key,ca.first_share_id,ca.job_public_id,"
        "c.public_id,ca.height,ca.peer_family,ca.peer_address,ca.miner_tx_hash,"
        "ca.expected_block_id,ca.canonical_block_id,ca.state,ca.attempt_count,"
        "ca.max_attempts,ca.had_indeterminate,ca.reconciliation_cycle_count,"
        "ca.next_reconciliation_unix_us,ca.reconciliation_exhausted_unix_us,"
        "ca.created_unix_us,ca.updated_unix_us,ca.accepted_unix_us,"
        "ca.terminal_reason,ca.template_generation,ca.round_id,"
        "ca.frozen_block_blob FROM candidates ca LEFT JOIN connections c "
        "ON c.id=ca.connection_id WHERE ca.id=?1");
    row.bind(1, candidate_id);
    if (!row.row()) return std::nullopt;
    Json submission = submission_resource(row);
    if (include_blobs) submission["frozen_block_blob"] = hex_encode(row.blob(24));
    if (row.row()) throw DatabaseError("duplicate candidate database ID");

    Json attempts = Json::array();
    ReadStatement attempt(
        database_,
        "SELECT id,candidate_id,attempt_number,rpc_request_id,started_unix_us,"
        "completed_unix_us,classification,http_status,rpc_error_code,daemon_status,"
        "daemon_block_id,response_excerpt FROM candidate_attempts "
        "WHERE candidate_id=?1 ORDER BY id");
    attempt.bind(1, candidate_id);
    while (attempt.row()) {
        attempts.push_back({{"id", std::to_string(attempt.integer(0))},
                            {"candidate_id", std::to_string(attempt.integer(1))},
                            {"attempt_number", attempt.integer(2)},
                            {"rpc_request_id", std::to_string(attempt.integer(3))},
                            {"started_at", format_rfc3339_utc_us(attempt.integer(4))},
                            {"completed_at", nullable_timestamp(attempt, 5)},
                            {"classification", attempt.text(6)},
                            {"http_status", attempt.is_null(7) ? Json(nullptr) : Json(attempt.integer(7))},
                            {"rpc_error_code", attempt.is_null(8) ? Json(nullptr) : Json(attempt.integer(8))},
                            {"daemon_status", nullable_text(attempt, 9)},
                            {"daemon_block_id", nullable_blob_hex(attempt, 10)},
                            {"response_excerpt", nullable_text(attempt, 11)}});
    }

    Json reconciliations = Json::array();
    ReadStatement reconciliation(
        database_,
        "SELECT id,candidate_id,cycle_number,lookup_kind,rpc_request_id,"
        "requested_block_id,started_unix_us,completed_unix_us,classification,"
        "observed_block_id,observed_height,observed_miner_tx_hash,observed_orphan,"
        "response_excerpt FROM candidate_reconciliations WHERE candidate_id=?1 "
        "ORDER BY id");
    reconciliation.bind(1, candidate_id);
    while (reconciliation.row()) {
        reconciliations.push_back({
            {"id", std::to_string(reconciliation.integer(0))},
            {"candidate_id", std::to_string(reconciliation.integer(1))},
            {"cycle_number", reconciliation.integer(2)},
            {"lookup_kind", reconciliation.text(3)},
            {"rpc_request_id", std::to_string(reconciliation.integer(4))},
            {"requested_block_id", nullable_blob_hex(reconciliation, 5)},
            {"started_at", format_rfc3339_utc_us(reconciliation.integer(6))},
            {"completed_at", nullable_timestamp(reconciliation, 7)},
            {"classification", reconciliation.text(8)},
            {"observed_block_id", nullable_blob_hex(reconciliation, 9)},
            {"observed_height", reconciliation.is_null(10) ? Json(nullptr) : Json(reconciliation.integer(10))},
            {"observed_miner_tx_hash", nullable_blob_hex(reconciliation, 11)},
            {"observed_orphan", reconciliation.is_null(12) ? Json(nullptr) : Json(reconciliation.integer(12) != 0)},
            {"response_excerpt", nullable_text(reconciliation, 13)}});
    }

    Json blocknotify = nullptr;
    ReadStatement notify(
        database_,
        "SELECT id,candidate_id,miner_tx_hash,state,attempt_count,"
        "next_attempt_unix_us,started_unix_us,completed_unix_us,exit_code,"
        "term_signal,stderr_excerpt,last_error FROM blocknotify_deliveries "
        "WHERE candidate_id=?1");
    notify.bind(1, candidate_id);
    if (notify.row()) {
        blocknotify = {{"id", std::to_string(notify.integer(0))},
                       {"candidate_id", std::to_string(notify.integer(1))},
                       {"miner_tx_hash", hex_encode(notify.blob(2))},
                       {"state", notify.text(3)},
                       {"attempt_count", notify.integer(4)},
                       {"next_attempt_at", nullable_timestamp(notify, 5)},
                       {"started_at", nullable_timestamp(notify, 6)},
                       {"completed_at", nullable_timestamp(notify, 7)},
                       {"exit_code", notify.is_null(8) ? Json(nullptr) : Json(notify.integer(8))},
                       {"term_signal", notify.is_null(9) ? Json(nullptr) : Json(notify.integer(9))},
                       {"stderr_excerpt", nullable_text(notify, 10)},
                       {"last_error", nullable_text(notify, 11)}};
        if (notify.row()) throw DatabaseError("duplicate candidate blocknotify row");
    }
    return Json{{"submission", std::move(submission)},
                {"attempts", std::move(attempts)},
                {"reconciliations", std::move(reconciliations)},
                {"blocknotify", std::move(blocknotify)}};
}

std::optional<Json> SqliteApiReader::current_round()
{
    ReadStatement row(
        database_,
        "SELECT id,opened_unix_us,closed_unix_us,state,accepted_candidate_id,"
        "accepted_height,miner_tx_hash,block_id,credited_difficulty_dec,"
        "accepted_share_count,effort_finalized_unix_us,"
        "finalized_effort_segment_count,max_share_height "
        "FROM rounds WHERE state='open'");
    if (!row.row()) return std::nullopt;
    Json result = round_resource(row);
    if (row.row()) throw DatabaseError("database contains multiple open rounds");
    return result;
}

Json SqliteApiReader::summary(Json live_summary,
                              std::optional<Json> live_daemon)
{
    std::scoped_lock lock(mutex_);
    if (!live_summary.is_object() || !live_summary.contains("server") ||
        !live_summary["server"].is_object()) {
        throw DatabaseError("live API summary omitted the server skeleton");
    }

    const Json &input_server = live_summary["server"];
    const auto server_field = [&](const char *const name) -> Json {
        const auto value = input_server.find(name);
        if (value == input_server.end()) {
            throw DatabaseError(std::string("live API summary omitted server.") +
                                name);
        }
        return *value;
    };
    Json server{
        {"version", server_field("version")},
        {"git_commit", server_field("git_commit")},
        {"session_id", server_field("session_id")},
        {"started_at", server_field("started_at")},
        {"uptime_seconds", server_field("uptime_seconds")},
        {"network", server_field("network")},
        {"verification", server_field("verification")},
        {"stratum_authentication", server_field("stratum_authentication")},
        {"api_authentication", server_field("api_authentication")},
    };

    Json input_daemon;
    if (const auto compact = live_summary.find("daemon");
        compact != live_summary.end() && compact->is_object()) {
        input_daemon = *compact;
    }
    else if (live_daemon.has_value() && live_daemon->is_object()) {
        input_daemon = std::move(*live_daemon);
    }
    else {
        input_daemon = Json::object();
    }
    const auto daemon_value = [&](const char *const compact_name,
                                  const char *const detail_name,
                                  Json fallback) -> Json {
        if (const auto value = input_daemon.find(compact_name);
            value != input_daemon.end()) return *value;
        if (const auto value = input_daemon.find(detail_name);
            value != input_daemon.end()) return *value;
        return fallback;
    };
    Json daemon{
        {"ready", daemon_value("ready", "ready", false)},
        {"rpc", daemon_value("rpc", "rpc_state", "unavailable")},
        {"zmq", daemon_value("zmq", "zmq_state", "disabled")},
        {"height", daemon_value("height", "height", nullptr)},
        {"template_generation",
         daemon_value("template_generation", "template_generation", nullptr)},
        {"template_id", daemon_value("template_id", "template_id", nullptr)},
    };

    ReadStatement connection_counts(
        database_,
        "SELECT count(*),count(CASE WHEN closed_unix_us IS NULL THEN 1 END) "
        "FROM connections");
    if (!connection_counts.row()) {
        throw DatabaseError("could not aggregate API connection summary");
    }
    const std::int64_t total_connections = connection_counts.integer(0);
    const std::int64_t active_connections = connection_counts.integer(1);
    if (connection_counts.row()) {
        throw DatabaseError("connection summary returned multiple rows");
    }

    ReadStatement worker_counts(
        database_,
        "SELECT (SELECT count(*) FROM workers),count(DISTINCT worker_id) "
        "FROM connections WHERE closed_unix_us IS NULL AND worker_id IS NOT NULL");
    if (!worker_counts.row()) {
        throw DatabaseError("could not aggregate API worker summary");
    }
    const std::int64_t total_workers = worker_counts.integer(0);
    const std::int64_t active_workers = worker_counts.integer(1);
    if (worker_counts.row()) {
        throw DatabaseError("worker summary returned multiple rows");
    }

    std::uint64_t accepted_shares = 0;
    std::uint64_t stale_shares = 0;
    std::uint64_t duplicate_shares = 0;
    std::uint64_t low_difficulty_shares = 0;
    std::uint64_t invalid_result_shares = 0;
    std::uint64_t infrastructure_failed_shares = 0;
    std::uint64_t terminal_shares = 0;
    ReadStatement share_counts(
        database_,
        "SELECT status,sum(share_count) FROM share_totals GROUP BY status");
    while (share_counts.row()) {
        const std::string status = share_counts.text(0);
        const std::int64_t signed_count = share_counts.integer(1);
        if (signed_count < 0) {
            throw DatabaseError("negative API aggregate share count");
        }
        const auto count = static_cast<std::uint64_t>(signed_count);
        if (terminal_shares > std::numeric_limits<std::uint64_t>::max() - count) {
            throw DatabaseError("API aggregate share count overflow");
        }
        terminal_shares += count;
        if (status == "accepted") accepted_shares = count;
        else if (status == "stale") stale_shares = count;
        else if (status == "duplicate") duplicate_shares = count;
        else if (status == "low_difficulty") low_difficulty_shares = count;
        else if (status == "invalid_result") invalid_result_shares = count;
        else if (status == "server_busy" || status == "verifier_failed" ||
                 status == "cancelled") {
            if (infrastructure_failed_shares >
                std::numeric_limits<std::uint64_t>::max() - count) {
                throw DatabaseError(
                    "API infrastructure share count overflow");
            }
            infrastructure_failed_shares += count;
        }
    }
    ReadStatement pending_share_counts(
        database_,
        "SELECT count(*) FROM shares WHERE status IN ('received','verifying')");
    if (!pending_share_counts.row() || pending_share_counts.integer(0) < 0) {
        throw DatabaseError("could not aggregate pending API shares");
    }
    const auto retained_pending_shares = static_cast<std::uint64_t>(
        pending_share_counts.integer(0));
    const DatabaseWriterStats writer = writer_stats_
                                           ? writer_stats_()
                                           : DatabaseWriterStats{};
    if (pending_share_counts.row() ||
        retained_pending_shares >
            std::numeric_limits<std::uint64_t>::max() -
                writer.pending_transient_shares ||
        terminal_shares >
            std::numeric_limits<std::uint64_t>::max() -
                (retained_pending_shares + writer.pending_transient_shares)) {
        throw DatabaseError("API total share count overflow");
    }
    const std::uint64_t pending_shares = retained_pending_shares +
                                         writer.pending_transient_shares;
    Json shares{
        {"pending", std::to_string(pending_shares)},
        {"accepted", std::to_string(accepted_shares)},
        {"stale", std::to_string(stale_shares)},
        {"duplicate", std::to_string(duplicate_shares)},
        {"low_difficulty", std::to_string(low_difficulty_shares)},
        {"invalid_result", std::to_string(invalid_result_shares)},
        {"infrastructure_failed",
         std::to_string(infrastructure_failed_shares)},
        {"total", std::to_string(terminal_shares + pending_shares)},
    };

    ReadStatement candidate_counts(
        database_,
        "SELECT count(*),"
        "count(CASE WHEN state IN ('journaled','dispatching','retry_wait') "
        "THEN 1 END),"
        "count(CASE WHEN state IN ('accepted','accepted_by_reconciliation') "
        "THEN 1 END),"
        "count(CASE WHEN state='rejected' THEN 1 END),"
        "count(CASE WHEN state='ambiguous' THEN 1 END) FROM candidates");
    if (!candidate_counts.row()) {
        throw DatabaseError("could not aggregate API candidate summary");
    }
    Json candidates{
        {"active", std::to_string(candidate_counts.integer(1))},
        {"accepted", std::to_string(candidate_counts.integer(2))},
        {"rejected", std::to_string(candidate_counts.integer(3))},
        {"ambiguous", std::to_string(candidate_counts.integer(4))},
        {"total", std::to_string(candidate_counts.integer(0))},
    };
    if (candidate_counts.row()) {
        throw DatabaseError("candidate summary returned multiple rows");
    }

    std::optional<Json> open_round = current_round();
    if (!open_round.has_value()) {
        throw DatabaseError("API summary has no open round");
    }
    Json compact_round{
        {"id", open_round->at("id")},
        {"state", open_round->at("state")},
        {"opened_at", open_round->at("opened_at")},
        {"estimated_hashes", open_round->at("estimated_hashes")},
        {"accepted_share_count", open_round->at("accepted_share_count")},
        {"max_share_height", open_round->at("max_share_height")},
        {"effort", open_round->at("effort")},
    };

    return Json{
        {"server", std::move(server)},
        {"daemon", std::move(daemon)},
        {"connections",
         {{"active", active_connections},
          {"total", std::to_string(total_connections)}}},
        {"workers",
         {{"active", active_workers}, {"total", std::to_string(total_workers)}}},
        {"shares", std::move(shares)},
        {"candidates", std::move(candidates)},
        {"round", std::move(compact_round)},
        {"hashrate", hashrate_resource("global", 0)},
    };
}

Json SqliteApiReader::persistence()
{
    const DatabaseWriterStats writer = writer_stats_
                                           ? writer_stats_()
                                           : DatabaseWriterStats{};
    ReadStatement journal_mode(database_, "PRAGMA journal_mode");
    if (!journal_mode.row()) {
        throw DatabaseError("API persistence journal mode is unavailable");
    }
    const std::string journal = journal_mode.text(0);
    if (journal_mode.row()) {
        throw DatabaseError("API persistence journal mode returned multiple rows");
    }

    ReadStatement synchronous_mode(database_, "PRAGMA synchronous");
    if (!synchronous_mode.row()) {
        throw DatabaseError("API persistence synchronous mode is unavailable");
    }
    const std::int64_t synchronous_value = synchronous_mode.integer(0);
    if (synchronous_mode.row()) {
        throw DatabaseError("API persistence synchronous mode returned multiple rows");
    }
    const std::string synchronous = synchronous_value == 2 ? "full" :
                                    synchronous_value == 3 ? "extra" :
                                    synchronous_value == 1 ? "normal" : "off";

    ReadStatement foreign_keys(database_, "PRAGMA foreign_keys");
    if (!foreign_keys.row()) {
        throw DatabaseError("API persistence foreign key state is unavailable");
    }
    const bool foreign_keys_enabled = foreign_keys.integer(0) == 1;
    if (foreign_keys.row()) {
        throw DatabaseError("API persistence foreign key state returned multiple rows");
    }

    ReadStatement last_commit(database_, "SELECT max(created_unix_us) FROM events");
    if (!last_commit.row()) {
        throw DatabaseError("API persistence commit timestamp is unavailable");
    }
    const Json last_commit_at = last_commit.is_null(0) ? Json(nullptr) :
        Json(format_rfc3339_utc_us(last_commit.integer(0)));
    if (last_commit.row()) {
        throw DatabaseError("API persistence commit timestamp returned multiple rows");
    }

    ReadStatement pending(
        database_,
        "SELECT "
        "(SELECT count(*) FROM candidates WHERE state IN "
        "('journaled','dispatching','retry_wait','ambiguous') AND "
        "reconciliation_exhausted_unix_us IS NULL),"
        "(SELECT count(*) FROM blocknotify_deliveries WHERE state IN "
        "('pending','running','retry_wait'))");
    if (!pending.row()) {
        throw DatabaseError("API persistence pending counts are unavailable");
    }
    const std::int64_t unresolved_candidates = pending.integer(0);
    const std::int64_t pending_blocknotify = pending.integer(1);
    if (pending.row()) {
        throw DatabaseError("API persistence pending counts returned multiple rows");
    }

    return Json{
        {"schema_version", 3},
        {"journal_mode", journal},
        {"synchronous", synchronous},
        {"foreign_keys", foreign_keys_enabled},
        {"database_bytes", std::to_string(file_bytes(options_.path))},
        {"wal_bytes", std::to_string(file_bytes(options_.path + "-wal"))},
        {"writer_queue_items", writer.queued_items},
        {"writer_queue_bytes", writer.queued_bytes},
        {"writer_priority_items", writer.priority_items},
        {"pending_accounting_items", writer.pending_accounting_items},
        {"pending_transient_shares", writer.pending_transient_shares},
        {"last_commit_at", std::move(last_commit_at)},
        {"last_writer_error_at", nullptr},
        {"last_writer_error_code", nullptr},
        {"last_writer_error_message", nullptr},
        {"unresolved_candidates", std::to_string(unresolved_candidates)},
        {"pending_blocknotify", std::to_string(pending_blocknotify)},
    };
}

std::optional<Json> SqliteApiReader::detail(const ApiDetailRequest &request)
{
    std::scoped_lock lock(mutex_);
    switch (request.resource) {
    case ApiDetail::connection: return connection_detail(request.id);
    case ApiDetail::share: return share_detail(request.id);
    case ApiDetail::submission:
        return submission_detail(request.id, request.include_blobs);
    }
    return std::nullopt;
}

std::optional<Json> SqliteApiReader::singleton(const ApiSingleton resource)
{
    std::scoped_lock lock(mutex_);
    if (resource == ApiSingleton::current_round) return current_round();
    if (resource == ApiSingleton::persistence) return persistence();
    return std::nullopt;
}

} // namespace

ApiDataSource make_sqlite_api_data_source(SqliteApiDataSourceOptions options)
{
    if (!options.clock) options.clock = [] { return unix_time_us(); };
    auto reader = std::make_shared<SqliteApiReader>(
        options.database, options.api, options.active_hashrate_source, options.clock,
        std::move(options.writer_stats));
    ApiDataSource live = std::move(options.live);
    ApiDataSource result;
    result.readiness = std::move(live.readiness);
    result.collection = [reader](const ApiCollectionRequest &request) {
        return reader->collection(request);
    };
    result.detail = [reader](const ApiDetailRequest &request) {
        return reader->detail(request);
    };
    result.singleton =
        [reader, callback = std::move(live.singleton)](
            const ApiSingleton resource) -> std::optional<Json> {
        if (resource == ApiSingleton::summary) {
            if (!callback) return std::nullopt;
            std::optional<Json> skeleton = callback(ApiSingleton::summary);
            if (!skeleton.has_value()) return std::nullopt;
            std::optional<Json> daemon;
            if (!skeleton->contains("daemon")) {
                daemon = callback(ApiSingleton::daemon);
            }
            return reader->summary(std::move(*skeleton), std::move(daemon));
        }
        if (resource == ApiSingleton::current_round ||
            resource == ApiSingleton::persistence) {
            return reader->singleton(resource);
        }
        return callback ? callback(resource) : std::nullopt;
    };
    return result;
}

} // namespace monero_solo
