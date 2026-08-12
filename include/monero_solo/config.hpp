#pragma once

#include "monero_solo/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monero_solo {

struct StratumConfig {
    std::vector<std::string> listen;
    std::optional<std::string> access_password;
    std::uint64_t max_connections{2048};
    std::uint64_t max_connections_per_ip{128};
    std::uint64_t login_timeout_ms{10000};
    std::uint64_t idle_timeout_ms{300000};
    std::uint64_t max_line_bytes{16384};
    std::uint64_t max_output_bytes_per_connection{1048576};
    std::uint64_t max_json_depth{32};
    std::uint64_t job_history{6};
    std::uint64_t job_ttl_ms{120000};
    std::uint64_t max_pending_verifications_per_connection{8};
    // Zero selects a hardware-derived value at Runtime construction.
    std::uint64_t submit_workers{};
};

struct DaemonConfig {
    std::string rpc_url;
    std::optional<std::string> rpc_username;
    std::optional<std::string> rpc_password;
    std::optional<std::string> zmq_address;
    std::uint64_t poll_interval_ms{20000};
    std::uint64_t request_timeout_ms{15000};
    std::uint64_t max_concurrent_requests{8};
    std::uint64_t max_pending_requests{256};
    std::uint64_t max_response_bytes{16777216};
    std::uint64_t refresh_retry_ms{1000};
    std::uint64_t submit_attempts{4};
    std::uint64_t submit_retry_ms{2000};
};

struct DifficultyConfig {
    std::string mode;
    std::uint64_t value{};
};

struct VerifierConfig {
    bool enabled{true};
    std::string memory_mode{"fast"};
    // Zero selects hardware-derived values at Runtime construction.
    std::uint64_t workers{};
    std::uint64_t seed_init_threads{};
    std::uint64_t max_seeds{2};
    std::uint64_t pending_capacity{256};
    std::uint64_t max_outstanding{512};
    std::uint64_t max_input_size{4096};
    std::uint64_t max_buffered_input_bytes{16777216};
    std::string large_pages{"try"};
    std::string jit{"secure"};
    std::string aes{"auto"};
    std::string log_level{"info"};
};

struct EntropySettings {
    std::uint64_t reseed_interval_seconds{1800};
    std::uint64_t max_reseed_age_seconds{1860};
    std::uint64_t max_generate_calls{1048576};
};

struct DatabaseConfig {
    std::string path;
    std::uint64_t busy_timeout_ms{5000};
    std::uint64_t max_writer_queue_items{100000};
    std::uint64_t max_writer_queue_bytes{67108864};
    std::uint64_t retention_days{0};
    bool store_rejected_shares{true};
};

struct EventsConfig {
    bool enabled{true};
    std::string unix_socket{"/run/monero-solo-stratum/events.sock"};
    std::string permissions{"0660"};
    std::uint64_t max_clients{8};
    std::uint64_t max_pending_bytes_per_client{1048576};
};

struct ApiConfig {
    bool enabled{true};
    std::string listen{"127.0.0.1:8787"};
    std::optional<std::string> access_token;
    std::uint64_t max_page_size{1000};
    std::uint64_t max_connections{64};
    std::uint64_t request_rate_per_second{20};
    std::uint64_t request_burst{40};
    std::uint64_t max_pending_bytes_per_connection{2097152};
};

struct DefenseConfig {
    bool enabled{true};
    std::string profile{"aggressive"};
    std::uint64_t ban_seconds{7200};
    std::uint64_t connection_rate_per_minute{60};
    std::uint64_t connection_burst{20};
    std::uint64_t request_rate_per_second{50};
    std::uint64_t request_burst{100};
    std::uint64_t submit_rate_per_second{20};
    std::uint64_t submit_burst{40};
    std::uint64_t malformed_limit{10};
    std::uint64_t auth_failure_limit{10};
    std::uint64_t unknown_job_limit{20};
    std::uint64_t duplicate_limit{20};
    std::uint64_t abuse_window_seconds{60};
    std::uint64_t hammer_rate_multiplier{4};
    std::uint64_t hammer_sustain_seconds{5};
    std::uint64_t candidate_rate_per_minute{12};
    std::uint64_t candidate_burst{3};
    std::uint64_t candidate_inflight_per_ip{2};
    std::uint64_t candidate_global_inflight{64};
    std::uint64_t false_candidate_limit{3};
    std::uint64_t false_candidate_window_seconds{600};
    std::uint64_t trusted_candidate_rejection_limit{3};
    std::uint64_t trusted_candidate_rejection_window_seconds{600};
    std::uint64_t verification_mismatch_limit{10};
    std::uint64_t verification_mismatch_window_seconds{600};
};

struct LoggingConfig {
    std::string level{"info"};
    std::optional<std::string> file;
};

struct Config {
    std::uint64_t schema_version{};
    Network network{Network::mainnet};
    std::string wallet_address;
    std::optional<std::string> blocknotify;
    std::vector<std::string> blocknotify_argv;
    StratumConfig stratum;
    DaemonConfig daemon;
    DifficultyConfig difficulty;
    VerifierConfig verifier;
    EntropySettings entropy;
    DatabaseConfig database;
    EventsConfig events;
    ApiConfig api;
    DefenseConfig defense;
    LoggingConfig logging;
};

struct ConfigValidationOptions {
    bool validate_paths{true};
    bool validate_blocknotify_executable{true};
};

[[nodiscard]] Config parse_config_json(
    const std::string &json_text,
    ConfigValidationOptions options = {});
[[nodiscard]] Config load_config_file(
    const std::string &path,
    ConfigValidationOptions options = {});

} // namespace monero_solo
