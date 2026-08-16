#include "monero_solo/config.hpp"

#include "monero_solo/monero.hpp"
#include "monero_solo/util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace monero_solo {
namespace {

using Json = nlohmann::json;

class JsonPreflight final {
public:
    explicit JsonPreflight(std::string_view input) : input_(input) {}

    void run()
    {
        whitespace();
        value(0);
        whitespace();
        if (position_ != input_.size()) fail("trailing data");
    }

private:
    [[noreturn]] void fail(const char *message) const
    {
        throw ValidationError("invalid JSON near byte " + std::to_string(position_) +
                              ": " + message);
    }

    void whitespace()
    {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    void expect(char expected)
    {
        if (position_ >= input_.size() || input_[position_] != expected) {
            fail("unexpected character");
        }
        ++position_;
    }

    [[nodiscard]] std::string string()
    {
        expect('"');
        std::string raw;
        while (position_ < input_.size()) {
            const char ch = input_[position_++];
            if (ch == '"') return raw;
            if (static_cast<unsigned char>(ch) < 0x20) fail("control byte in string");
            if (ch != '\\') {
                raw.push_back(ch);
                continue;
            }
            if (position_ >= input_.size()) fail("truncated string escape");
            const char escaped = input_[position_++];
            raw.push_back('\\');
            raw.push_back(escaped);
            if (escaped == 'u') {
                for (unsigned i = 0; i < 4; ++i) {
                    if (position_ >= input_.size() ||
                        !std::isxdigit(static_cast<unsigned char>(input_[position_]))) {
                        fail("invalid Unicode escape");
                    }
                    raw.push_back(input_[position_++]);
                }
            } else if (std::string_view{"\"\\/bfnrt"}.find(escaped) ==
                       std::string_view::npos) {
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    void number()
    {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) fail("truncated number");
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                fail("leading zero in number");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                fail("invalid number");
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            fail("configuration numbers must be integer tokens");
        }
        const std::string_view token = input_.substr(start, position_ - start);
        if (token == "-0") fail("negative zero is forbidden");
    }

    void literal(std::string_view expected)
    {
        if (input_.substr(position_, expected.size()) != expected) {
            fail("invalid literal");
        }
        position_ += expected.size();
    }

    void value(unsigned depth)
    {
        if (depth > 128) fail("JSON nesting exceeds 128");
        whitespace();
        if (position_ >= input_.size()) fail("missing value");
        switch (input_[position_]) {
        case '{': object(depth + 1); break;
        case '[': array(depth + 1); break;
        case '"': (void)string(); break;
        case 't': literal("true"); break;
        case 'f': literal("false"); break;
        case 'n': literal("null"); break;
        default:
            if (input_[position_] == '-' ||
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                number();
            } else {
                fail("unexpected value");
            }
        }
    }

    void object(unsigned depth)
    {
        expect('{');
        whitespace();
        std::unordered_set<std::string> keys;
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return;
        }
        for (;;) {
            whitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("object key must be a string");
            }
            const std::string raw_key = string();
            // Parsing this one JSON string canonicalizes escapes so equivalent
            // keys such as "a" and "\u0061" collide.
            std::string decoded;
            try {
                decoded = Json::parse("\"" + raw_key + "\"").get<std::string>();
            } catch (...) {
                fail("invalid object key encoding");
            }
            if (!keys.emplace(decoded).second) fail("duplicate object key");
            whitespace();
            expect(':');
            value(depth);
            whitespace();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return;
            }
            expect(',');
        }
    }

    void array(unsigned depth)
    {
        expect('[');
        whitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return;
        }
        for (;;) {
            value(depth);
            whitespace();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                return;
            }
            expect(',');
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

void require_object(const Json &value, std::string_view path)
{
    if (!value.is_object()) throw ValidationError(std::string(path) + " must be an object");
}

void keys(const Json &object, std::string_view path,
          std::initializer_list<std::string_view> allowed,
          std::initializer_list<std::string_view> required = {})
{
    require_object(object, path);
    for (const auto &[key, unused] : object.items()) {
        (void)unused;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw ValidationError(std::string(path) + " contains unknown key " + key);
        }
    }
    for (const auto required_key : required) {
        if (!object.contains(required_key)) {
            throw ValidationError(std::string(path) + " is missing required key " +
                                  std::string(required_key));
        }
    }
}

[[nodiscard]] std::uint64_t integer(const Json &object, const char *name,
                                    std::uint64_t fallback, std::uint64_t minimum,
                                    std::uint64_t maximum, std::string_view path)
{
    if (!object.contains(name)) return fallback;
    const Json &value = object.at(name);
    if (!value.is_number_unsigned() &&
        value.type() != Json::value_t::number_integer) {
        throw ValidationError(std::string(path) + "." + name + " must be an integer");
    }
    if (value.type() == Json::value_t::number_integer &&
        value.get<std::int64_t>() < 0) {
        throw ValidationError(std::string(path) + "." + name + " must be unsigned");
    }
    const std::uint64_t result = value.get<std::uint64_t>();
    if (result < minimum || result > maximum) {
        throw ValidationError(std::string(path) + "." + name + " is out of range");
    }
    return result;
}

[[nodiscard]] bool boolean(const Json &object, const char *name, bool fallback,
                           std::string_view path)
{
    if (!object.contains(name)) return fallback;
    if (!object.at(name).is_boolean()) {
        throw ValidationError(std::string(path) + "." + name + " must be boolean");
    }
    return object.at(name).get<bool>();
}

[[nodiscard]] std::string string(const Json &object, const char *name,
                                 std::string fallback, std::size_t maximum,
                                 std::string_view path, bool nonempty = false)
{
    if (!object.contains(name)) return fallback;
    if (!object.at(name).is_string()) {
        throw ValidationError(std::string(path) + "." + name + " must be a string");
    }
    std::string result = object.at(name).get<std::string>();
    if (result.size() > maximum || (nonempty && result.empty())) {
        throw ValidationError(std::string(path) + "." + name + " has invalid length");
    }
    if (result.find('\0') != std::string::npos) {
        throw ValidationError(std::string(path) + "." + name + " contains NUL");
    }
    return result;
}

[[nodiscard]] std::optional<std::string> nullable_string(
    const Json &object, const char *name, std::optional<std::string> fallback,
    std::size_t maximum, std::string_view path)
{
    if (!object.contains(name)) return fallback;
    if (object.at(name).is_null()) return std::nullopt;
    return string(object, name, {}, maximum, path);
}

void one_of(const std::string &value, std::string_view path,
            std::initializer_list<std::string_view> values)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        throw ValidationError(std::string(path) + " has an unsupported value");
    }
}

void validate_rpc_url(std::string_view url)
{
    if (url.empty() || url.size() > 4096) throw ValidationError("daemon.rpc_url length invalid");
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos ||
        (url.substr(0, scheme) != "http" && url.substr(0, scheme) != "https")) {
        throw ValidationError("daemon.rpc_url must be absolute HTTP(S)");
    }
    const std::string_view rest = url.substr(scheme + 3);
    if (rest.empty() || rest.find('@') != std::string_view::npos ||
        rest.find('?') != std::string_view::npos || rest.find('#') != std::string_view::npos) {
        throw ValidationError("daemon.rpc_url contains forbidden URL components");
    }
    const std::size_t slash = rest.find('/');
    if (slash != std::string_view::npos && rest.substr(slash) != "/") {
        throw ValidationError("daemon.rpc_url path must be empty or /");
    }
    const std::string authority(rest.substr(0, slash));
    if (authority.empty()) throw ValidationError("daemon.rpc_url has no authority");
    // Parse via the endpoint grammar after adding the scheme's default port.
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) throw ValidationError("invalid daemon IPv6 URL");
        if (close + 1 == authority.size()) {
            (void)parse_endpoint(authority + (url.starts_with("https") ? ":443" : ":80"));
        } else {
            (void)parse_endpoint(authority);
        }
    } else if (authority.find(':') == std::string::npos) {
        (void)parse_endpoint(authority + (url.starts_with("https") ? ":443" : ":80"));
    } else {
        (void)parse_endpoint(authority);
    }
}

[[nodiscard]] Config build(const Json &root, ConfigValidationOptions options)
{
    keys(root, "config",
         {"schema_version", "network", "wallet_address", "blocknotify", "stratum",
          "daemon", "difficulty", "verifier", "entropy", "database", "events",
          "api", "defense", "logging"},
         {"schema_version", "network", "wallet_address", "blocknotify", "stratum",
          "daemon", "difficulty", "verifier", "entropy", "database", "events",
          "api", "defense", "logging"});
    Config result;
    result.schema_version = integer(root, "schema_version", 0, 2, 2, "config");
    result.network = parse_network(string(root, "network", {}, 16, "config", true));
    result.wallet_address = string(root, "wallet_address", {}, 256, "config", true);
    validate_primary_address(result.wallet_address, result.network);
    result.blocknotify = nullable_string(root, "blocknotify", std::nullopt, 65536, "config");
    if (result.blocknotify && !result.blocknotify->empty()) {
        result.blocknotify_argv = parse_blocknotify_argv(
            *result.blocknotify, options.validate_blocknotify_executable);
    }

    const Json &s = root.at("stratum");
    keys(s, "stratum",
         {"listen", "access_password", "max_connections", "max_connections_per_ip",
          "login_timeout_ms", "idle_timeout_ms", "max_line_bytes",
          "max_output_bytes_per_connection", "max_json_depth", "job_ttl_ms",
          "max_pending_verifications_per_connection",
          "submit_workers"},
         {"listen", "access_password"});
    if (!s.at("listen").is_array() || s.at("listen").empty() ||
        s.at("listen").size() > 32) {
        throw ValidationError("stratum.listen must be a nonempty array of at most 32 endpoints");
    }
    std::set<std::string> canonical_listeners;
    for (const Json &entry : s.at("listen")) {
        if (!entry.is_string()) throw ValidationError("stratum.listen entries must be strings");
        std::string endpoint = entry.get<std::string>();
        const std::string canonical = parse_endpoint(endpoint).canonical();
        if (!canonical_listeners.insert(canonical).second) {
            throw ValidationError("stratum.listen contains duplicate endpoints");
        }
        result.stratum.listen.push_back(std::move(endpoint));
    }
    result.stratum.access_password = nullable_string(s, "access_password", {}, 4096, "stratum");
    result.stratum.max_connections = integer(s, "max_connections", 2048, 1, 1000000, "stratum");
    result.stratum.max_connections_per_ip = integer(
        s, "max_connections_per_ip", 128, 1, result.stratum.max_connections, "stratum");
    result.stratum.login_timeout_ms = integer(s, "login_timeout_ms", 10000, 1000, 600000, "stratum");
    result.stratum.idle_timeout_ms = integer(s, "idle_timeout_ms", 300000, 10000, 86400000, "stratum");
    result.stratum.max_line_bytes = integer(s, "max_line_bytes", 16384, 1024, 1048576, "stratum");
    result.stratum.max_output_bytes_per_connection = integer(
        s, "max_output_bytes_per_connection", 1048576, 4096, 67108864, "stratum");
    result.stratum.max_json_depth = integer(s, "max_json_depth", 32, 4, 128, "stratum");
    result.stratum.job_ttl_ms = integer(s, "job_ttl_ms", 120000, 1000, 3600000, "stratum");
    result.stratum.max_pending_verifications_per_connection = integer(
        s, "max_pending_verifications_per_connection", 8, 1, 4096, "stratum");
    result.stratum.submit_workers = integer(
        s, "submit_workers", 0, 0, 256, "stratum");

    const Json &d = root.at("daemon");
    keys(d, "daemon",
         {"rpc_url", "rpc_username", "rpc_password", "zmq_address", "poll_interval_ms",
          "request_timeout_ms", "max_concurrent_requests", "max_pending_requests",
          "max_response_bytes", "refresh_retry_ms", "submit_attempts", "submit_retry_ms"},
         {"rpc_url", "zmq_address"});
    result.daemon.rpc_url = string(d, "rpc_url", {}, 4096, "daemon", true);
    validate_rpc_url(result.daemon.rpc_url);
    result.daemon.rpc_username = nullable_string(d, "rpc_username", {}, 4096, "daemon");
    result.daemon.rpc_password = nullable_string(d, "rpc_password", {}, 4096, "daemon");
    const auto credential_state = [](const std::optional<std::string> &value) {
        return !value ? 0 : value->empty() ? 1 : 2;
    };
    if (credential_state(result.daemon.rpc_username) !=
        credential_state(result.daemon.rpc_password)) {
        throw ValidationError(
            "daemon RPC username/password must both be null, both empty, or both nonempty");
    }
    result.daemon.zmq_address = nullable_string(d, "zmq_address", {}, 4096, "daemon");
    if (result.daemon.zmq_address && result.daemon.zmq_address->empty()) {
        result.daemon.zmq_address.reset();
    }
    result.daemon.poll_interval_ms = integer(d, "poll_interval_ms", 20000, 1000, 300000, "daemon");
    result.daemon.request_timeout_ms = integer(d, "request_timeout_ms", 15000, 100, 300000, "daemon");
    result.daemon.max_concurrent_requests = integer(d, "max_concurrent_requests", 8, 1, 1024, "daemon");
    result.daemon.max_pending_requests = integer(d, "max_pending_requests", 256, 2, 100000, "daemon");
    result.daemon.max_response_bytes = integer(d, "max_response_bytes", 16777216, 4096, 67108864, "daemon");
    result.daemon.refresh_retry_ms = integer(d, "refresh_retry_ms", 1000, 100, 60000, "daemon");
    result.daemon.submit_attempts = integer(d, "submit_attempts", 4, 1, 4, "daemon");
    result.daemon.submit_retry_ms = integer(d, "submit_retry_ms", 2000, 100, 60000, "daemon");

    const Json &difficulty = root.at("difficulty");
    keys(difficulty, "difficulty", {"mode", "value"}, {"mode", "value"});
    result.difficulty.mode = string(difficulty, "mode", {}, 16, "difficulty", true);
    one_of(result.difficulty.mode, "difficulty.mode", {"fixed", "minimum"});
    result.difficulty.value = integer(difficulty, "value", 0, 1,
                                      std::numeric_limits<std::uint64_t>::max(), "difficulty");

    const Json &v = root.at("verifier");
    keys(v, "verifier",
         {"enabled", "memory_mode", "workers", "seed_init_threads", "max_seeds",
          "pending_capacity", "max_outstanding", "max_input_size",
          "max_buffered_input_bytes", "large_pages", "jit", "aes", "log_level"},
         {"enabled"});
    result.verifier.enabled = boolean(v, "enabled", true, "verifier");
    result.verifier.memory_mode = string(v, "memory_mode", "fast", 16, "verifier");
    one_of(result.verifier.memory_mode, "verifier.memory_mode", {"light", "fast"});
    result.verifier.workers = integer(v, "workers", 0, 0, 256, "verifier");
    result.verifier.seed_init_threads = integer(
        v, "seed_init_threads", 0, 0, 256, "verifier");
    result.verifier.max_seeds = integer(v, "max_seeds", 2, 2, 64, "verifier");
    result.verifier.pending_capacity = integer(v, "pending_capacity", 256, 1, 1000000, "verifier");
    result.verifier.max_outstanding = integer(v, "max_outstanding", 512,
                                              result.verifier.pending_capacity, 1000000, "verifier");
    result.verifier.max_input_size = integer(v, "max_input_size", 4096, 1, 67108864, "verifier");
    result.verifier.max_buffered_input_bytes = integer(
        v, "max_buffered_input_bytes", 16777216, result.verifier.max_input_size,
        UINT64_C(17179869184), "verifier");
    result.verifier.large_pages = string(v, "large_pages", "try", 16, "verifier");
    one_of(result.verifier.large_pages, "verifier.large_pages", {"disabled", "try", "require"});
    result.verifier.jit = string(v, "jit", "secure", 16, "verifier");
    one_of(result.verifier.jit, "verifier.jit", {"disabled", "enabled", "secure"});
    result.verifier.aes = string(v, "aes", "auto", 16, "verifier");
    one_of(result.verifier.aes, "verifier.aes", {"auto", "software"});
    result.verifier.log_level = string(v, "log_level", "info", 16, "verifier");
    one_of(result.verifier.log_level, "verifier.log_level", {"error", "warning", "info", "debug", "trace"});
    if (result.stratum.max_pending_verifications_per_connection > result.verifier.max_outstanding) {
        throw ValidationError("per-connection verification cap exceeds verifier.max_outstanding");
    }

    const Json &e = root.at("entropy");
    keys(e, "entropy", {"reseed_interval_seconds", "max_reseed_age_seconds", "max_generate_calls"});
    result.entropy.reseed_interval_seconds = integer(e, "reseed_interval_seconds", 1200, 1, 86400, "entropy");
    result.entropy.max_reseed_age_seconds = integer(e, "max_reseed_age_seconds", 1260,
                                                    result.entropy.reseed_interval_seconds, 604800, "entropy");
    result.entropy.max_generate_calls = integer(e, "max_generate_calls", 1048576, 1, 4294967295ULL, "entropy");

    const Json &db = root.at("database");
    keys(db, "database",
         {"path", "busy_timeout_ms", "max_writer_queue_items", "max_writer_queue_bytes",
          "min_persisted_share_difficulty", "accounting_flush_interval_ms"},
         {"path"});
    result.database.path = string(db, "path", {}, 4096, "database", true);
    if (options.validate_paths) validate_database_path(result.database.path);
    else if (result.database.path.front() != '/') throw ValidationError("database.path must be absolute");
    result.database.busy_timeout_ms = integer(db, "busy_timeout_ms", 5000, 1, 60000, "database");
    result.database.max_writer_queue_items = integer(db, "max_writer_queue_items", 100000, 1024, 10000000, "database");
    result.database.max_writer_queue_bytes = integer(db, "max_writer_queue_bytes", 67108864, 1048576, 1073741824, "database");
    result.database.min_persisted_share_difficulty = integer(
        db, "min_persisted_share_difficulty", 80000000000ULL, 1,
        std::numeric_limits<std::uint64_t>::max(), "database");
    result.database.accounting_flush_interval_ms = integer(
        db, "accounting_flush_interval_ms", 1000, 10, 60000, "database");

    const Json &events = root.at("events");
    keys(events, "events", {"enabled", "unix_socket", "permissions", "max_clients",
                              "max_pending_bytes_per_client"}, {"enabled"});
    result.events.enabled = boolean(events, "enabled", true, "events");
    result.events.unix_socket = string(events, "unix_socket", result.events.unix_socket, 4096, "events");
    result.events.permissions = string(events, "permissions", "0660", 4, "events", true);
    if (result.events.permissions.size() != 4 || result.events.permissions[0] != '0' ||
        !std::all_of(result.events.permissions.begin() + 1, result.events.permissions.end(),
                     [](char ch) { return ch >= '0' && ch <= '7'; })) {
        throw ValidationError("events.permissions must be a four-character octal string");
    }
    const unsigned permissions = static_cast<unsigned>(std::stoul(result.events.permissions, nullptr, 8));
    if ((permissions & 0111U) != 0 || (permissions & 0007U) != 0) {
        throw ValidationError("events.permissions may not grant execute or other-user bits");
    }
    if (result.events.enabled && result.events.unix_socket.empty()) {
        throw ValidationError("events.unix_socket is required when events are enabled");
    }
    if (!result.events.unix_socket.empty()) {
        if (options.validate_paths) validate_unix_socket_path(result.events.unix_socket);
        else if (result.events.unix_socket.front() != '/') throw ValidationError("events.unix_socket must be absolute");
    }
    result.events.max_clients = integer(events, "max_clients", 8, 1, 1024, "events");
    result.events.max_pending_bytes_per_client = integer(events, "max_pending_bytes_per_client", 1048576, 4096, 67108864, "events");

    const Json &api = root.at("api");
    keys(api, "api", {"enabled", "listen", "access_token", "max_page_size", "max_connections",
                       "request_rate_per_second", "request_burst", "max_pending_bytes_per_connection",
                       "top_shares_limit", "recent_high_shares_limit",
                       "recent_high_share_min_difficulty"},
         {"enabled"});
    result.api.enabled = boolean(api, "enabled", true, "api");
    result.api.listen = string(api, "listen", result.api.listen, 4096, "api");
    if (result.api.enabled && result.api.listen.empty()) throw ValidationError("api.listen is required when enabled");
    if (!result.api.listen.empty()) (void)parse_endpoint(result.api.listen);
    result.api.access_token = nullable_string(api, "access_token", {}, 4096, "api");
    result.api.max_page_size = integer(api, "max_page_size", 1000, 1, 10000, "api");
    result.api.max_connections = integer(api, "max_connections", 64, 1, 10000, "api");
    result.api.request_rate_per_second = integer(api, "request_rate_per_second", 20, 1, 1000000, "api");
    result.api.request_burst = integer(api, "request_burst", 40, 1, 1000000, "api");
    result.api.max_pending_bytes_per_connection = integer(api, "max_pending_bytes_per_connection", 2097152, 4096, 67108864, "api");
    result.api.top_shares_limit = integer(api, "top_shares_limit", 100, 1, 100, "api");
    result.api.recent_high_shares_limit = integer(api, "recent_high_shares_limit", 100, 1, 100, "api");
    result.api.recent_high_share_min_difficulty = integer(
        api, "recent_high_share_min_difficulty", 80000000000ULL, 1,
        std::numeric_limits<std::uint64_t>::max(), "api");
    if (result.api.recent_high_share_min_difficulty <
        result.database.min_persisted_share_difficulty) {
        throw ValidationError(
            "api.recent_high_share_min_difficulty must be at least "
            "database.min_persisted_share_difficulty");
    }

    const Json &defense = root.at("defense");
    keys(defense, "defense",
         {"enabled", "profile", "ban_seconds", "connection_rate_per_minute", "connection_burst",
          "request_rate_per_second", "request_burst", "submit_rate_per_second", "submit_burst",
          "malformed_limit", "auth_failure_limit", "unknown_job_limit", "duplicate_limit",
          "abuse_window_seconds", "hammer_rate_multiplier", "hammer_sustain_seconds",
          "candidate_rate_per_minute", "candidate_burst", "candidate_inflight_per_ip",
          "candidate_global_inflight", "false_candidate_limit", "false_candidate_window_seconds",
          "trusted_candidate_rejection_limit", "trusted_candidate_rejection_window_seconds",
          "verification_mismatch_limit", "verification_mismatch_window_seconds"}, {"enabled"});
    result.defense.enabled = boolean(defense, "enabled", true, "defense");
    result.defense.profile = string(defense, "profile", "aggressive", 32, "defense");
    one_of(result.defense.profile, "defense.profile", {"aggressive"});
    auto defense_int = [&](const char *name, std::uint64_t fallback, std::uint64_t min,
                           std::uint64_t max = 1000000) {
        return integer(defense, name, fallback, min, max, "defense");
    };
    result.defense.ban_seconds = defense_int("ban_seconds", 7200, 1, 2592000);
    result.defense.connection_rate_per_minute = defense_int("connection_rate_per_minute", 60, 1);
    result.defense.connection_burst = defense_int("connection_burst", 20, 1);
    result.defense.request_rate_per_second = defense_int("request_rate_per_second", 50, 1);
    result.defense.request_burst = defense_int("request_burst", 100, 1);
    result.defense.submit_rate_per_second = defense_int("submit_rate_per_second", 20, 1);
    result.defense.submit_burst = defense_int("submit_burst", 40, 1);
    result.defense.malformed_limit = defense_int("malformed_limit", 10, 1);
    result.defense.auth_failure_limit = defense_int("auth_failure_limit", 10, 1);
    result.defense.unknown_job_limit = defense_int("unknown_job_limit", 20, 1);
    result.defense.duplicate_limit = defense_int("duplicate_limit", 20, 1);
    result.defense.abuse_window_seconds = defense_int("abuse_window_seconds", 60, 1, 86400);
    result.defense.hammer_rate_multiplier = defense_int("hammer_rate_multiplier", 4, 2, 1000);
    result.defense.hammer_sustain_seconds = defense_int("hammer_sustain_seconds", 5, 1, 3600);
    const std::uint64_t candidate_min = result.network == Network::regtest ? 0 : 1;
    result.defense.candidate_rate_per_minute = defense_int("candidate_rate_per_minute", 12, candidate_min);
    result.defense.candidate_burst = defense_int("candidate_burst", 3, candidate_min);
    result.defense.candidate_inflight_per_ip = defense_int("candidate_inflight_per_ip", 2, candidate_min);
    result.defense.candidate_global_inflight = defense_int("candidate_global_inflight", 64, candidate_min);
    const std::array candidate_values = {result.defense.candidate_rate_per_minute,
                                         result.defense.candidate_burst,
                                         result.defense.candidate_inflight_per_ip,
                                         result.defense.candidate_global_inflight};
    const bool any_zero = std::any_of(candidate_values.begin(), candidate_values.end(),
                                      [](std::uint64_t value) { return value == 0; });
    const bool all_zero = std::all_of(candidate_values.begin(), candidate_values.end(),
                                      [](std::uint64_t value) { return value == 0; });
    if (any_zero && !all_zero) throw ValidationError("regtest candidate-policy limits must all be zero or all nonzero");
    if (!all_zero && result.defense.candidate_inflight_per_ip > result.defense.candidate_global_inflight) {
        throw ValidationError("per-IP candidate inflight exceeds global inflight");
    }
    result.defense.false_candidate_limit = defense_int("false_candidate_limit", 3, 1);
    result.defense.false_candidate_window_seconds = defense_int("false_candidate_window_seconds", 600, 1, 86400);
    result.defense.trusted_candidate_rejection_limit = defense_int("trusted_candidate_rejection_limit", 3, 1);
    result.defense.trusted_candidate_rejection_window_seconds = defense_int("trusted_candidate_rejection_window_seconds", 600, 1, 86400);
    result.defense.verification_mismatch_limit = defense_int("verification_mismatch_limit", 10, 1);
    result.defense.verification_mismatch_window_seconds = defense_int("verification_mismatch_window_seconds", 600, 1, 86400);

    bool all_loopback = true;
    for (const auto &endpoint : result.stratum.listen) {
        all_loopback = all_loopback && parse_endpoint(endpoint).is_definitely_loopback();
    }
    const bool password_protected = result.stratum.access_password.has_value() &&
        !result.stratum.access_password->empty();
    if (!result.defense.enabled && result.network != Network::regtest &&
        !all_loopback && !password_protected) {
        throw ValidationError(
            "public listeners require defense or a nonempty Stratum password");
    }

    const Json &logging = root.at("logging");
    keys(logging, "logging", {"level", "file", "include_private_job_entropy"});
    result.logging.level = string(logging, "level", "info", 16, "logging");
    one_of(result.logging.level, "logging.level", {"error", "warning", "info", "debug", "trace"});
    result.logging.file = nullable_string(logging, "file", {}, 4096, "logging");
    if (result.logging.file && !result.logging.file->empty()) {
        if (options.validate_paths) validate_log_path(*result.logging.file);
        else if (result.logging.file->front() != '/') throw ValidationError("logging.file must be absolute");
    }
    result.logging.include_private_job_entropy = boolean(
        logging, "include_private_job_entropy", false, "logging");
    if (result.logging.include_private_job_entropy &&
        (result.logging.level != "debug" && result.logging.level != "trace")) {
        throw ValidationError(
            "logging.include_private_job_entropy requires debug or trace level");
    }
    const bool writes_private_job_entropy =
        result.logging.include_private_job_entropy ||
        result.logging.level == "trace";
    if (writes_private_job_entropy &&
        (!result.logging.file || result.logging.file->empty())) {
        throw ValidationError(
            "trace/private job entropy logging requires a log file");
    }

    {
        const std::uint64_t reserved_items =
            result.defense.candidate_global_inflight +
            (result.verifier.enabled ? result.verifier.max_outstanding : 0U);
        if (reserved_items > std::numeric_limits<std::uint64_t>::max() - 1024 ||
            result.database.max_writer_queue_items < reserved_items + 1024) {
            throw ValidationError("database writer item queue cannot cover mandatory verifier/candidate reserve");
        }
        if (reserved_items >= result.database.max_writer_queue_bytes / 512) {
            throw ValidationError("database writer byte queue cannot cover mandatory priority commands");
        }
    }
    return result;
}

} // namespace

Config parse_config_json(const std::string &json_text, ConfigValidationOptions options)
{
    JsonPreflight(json_text).run();
    Json parsed;
    try {
        parsed = Json::parse(json_text, nullptr, true, true);
    } catch (const std::exception &error) {
        throw ValidationError(std::string("invalid JSON configuration: ") + error.what());
    }
    return build(parsed, options);
}

Config load_config_file(const std::string &path, ConfigValidationOptions options)
{
    if (path.empty()) throw ValidationError("configuration path is empty");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ValidationError("could not open the explicitly configured file");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) throw ValidationError("could not read configuration file");
    return parse_config_json(contents.str(), options);
}

} // namespace monero_solo
