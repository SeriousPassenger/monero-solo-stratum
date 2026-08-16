#include "monero_solo/config.hpp"
#include "monero_solo/util.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string valid_config()
{
    return R"json({
      "schema_version":2,
      "network":"mainnet",
      "wallet_address":"44AFFq5kSiGBoZ4NMDwYtN18obc8AemS33DBLWs3H7otXft3XjrpDtQGv7SqSsaBYBb98uNbr2VBBEt7f2wfn3RVGQBEP3A",
      "blocknotify":null,
      "stratum":{"listen":["127.0.0.1:3333"],"access_password":null},
      "daemon":{"rpc_url":"http://127.0.0.1:18081","zmq_address":null},
      "difficulty":{"mode":"fixed","value":1048576},
      "verifier":{"enabled":true},
      "entropy":{},
      "database":{"path":"/tmp/monero-solo-stratum-test.sqlite3"},
      "events":{"enabled":false},
      "api":{"enabled":false},
      "defense":{"enabled":true},
      "logging":{}
    })json";
}

template <typename Function>
void rejects(Function function, const char *message)
{
    try {
        function();
    } catch (const monero_solo::ValidationError &) {
        return;
    }
    throw std::runtime_error(message);
}

std::string quote_blocknotify_argument(std::string_view value)
{
    std::string result{"\""};
    result.reserve(value.size() + 2U);
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

void test_defaults_and_required_fields()
{
    const auto config = monero_solo::parse_config_json(
        valid_config(), {.validate_paths = false,
                         .validate_blocknotify_executable = false});
    require(config.schema_version == 2, "schema version lost");
    require(config.stratum.max_connections == 2048 &&
                config.stratum.max_connections_per_ip == 128,
            "stratum connection defaults differ");
    require(config.daemon.submit_attempts == 4, "attempt default differs");
    require(config.daemon.submit_retry_ms == 2000, "retry default differs");
    require(config.stratum.submit_workers == 0,
            "Stratum auto-worker default differs");
    require(config.verifier.workers == 0 &&
                config.verifier.seed_init_threads == 0,
            "verifier auto-worker defaults differ");
    require(config.verifier.max_seeds == 2, "seed default differs");
    require(config.entropy.reseed_interval_seconds == 1200 &&
                config.entropy.max_reseed_age_seconds == 1260 &&
                config.entropy.max_generate_calls == 1048576,
            "entropy defaults differ");
    require(config.database.min_persisted_share_difficulty ==
                80'000'000'000ULL &&
                config.database.accounting_flush_interval_ms == 1000,
            "database persistence defaults differ");
    require(config.api.top_shares_limit == 100 &&
                config.api.recent_high_shares_limit == 100 &&
                config.api.recent_high_share_min_difficulty == 80'000'000'000ULL,
            "share analytics defaults differ");
    require(!config.stratum.access_password, "null password must stay disabled");
    require(!config.logging.include_private_job_entropy,
            "private job entropy logging default differs");

    std::string empty_zmq = valid_config();
    empty_zmq.replace(empty_zmq.find("\"zmq_address\":null"), 18,
                      "\"zmq_address\":\"\"");
    const auto polling_only = monero_solo::parse_config_json(
        empty_zmq, {.validate_paths = false,
                    .validate_blocknotify_executable = false});
    require(!polling_only.daemon.zmq_address,
            "empty ZMQ address did not canonicalize to polling-only mode");

    std::string maximum_difficulty = valid_config();
    maximum_difficulty.replace(maximum_difficulty.find("1048576"), 7,
                               "18446744073709551615");
    const auto maximum = monero_solo::parse_config_json(
        maximum_difficulty, {.validate_paths = false,
                             .validate_blocknotify_executable = false});
    require(maximum.difficulty.value == UINT64_MAX,
            "UINT64_MAX difficulty did not round-trip");

    std::string explicit_workers = valid_config();
    explicit_workers.replace(explicit_workers.find("\"access_password\":null"), 22,
                             "\"access_password\":null,\"submit_workers\":7");
    explicit_workers.replace(explicit_workers.find(
                                 "\"enabled\":true",
                                 explicit_workers.find("\"verifier\"")),
                             14, "\"enabled\":true,\"workers\":9,\"seed_init_threads\":11");
    const auto explicit_config = monero_solo::parse_config_json(
        explicit_workers, {.validate_paths = false,
                           .validate_blocknotify_executable = false});
    require(explicit_config.stratum.submit_workers == 7 &&
                explicit_config.verifier.workers == 9 &&
                explicit_config.verifier.seed_init_threads == 11,
            "explicit worker counts changed during parsing");
}

void test_private_job_entropy_logging_gate()
{
    std::string enabled = valid_config();
    enabled.replace(enabled.find("\"logging\":{}"), 12,
                    "\"logging\":{\"level\":\"debug\","
                    "\"file\":\"/tmp/mss-debug.jsonl\","
                    "\"include_private_job_entropy\":true}");
    const auto parsed = monero_solo::parse_config_json(
        enabled, {.validate_paths = false,
                  .validate_blocknotify_executable = false});
    require(parsed.logging.include_private_job_entropy,
            "private job entropy logging gate did not parse");

    std::string trace_file = valid_config();
    trace_file.replace(trace_file.find("\"logging\":{}"), 12,
        "\"logging\":{\"level\":\"trace\","
        "\"file\":\"/tmp/mss-trace.jsonl\"}");
    const auto trace_parsed = monero_solo::parse_config_json(
        trace_file, {.validate_paths = false,
                     .validate_blocknotify_executable = false});
    require(trace_parsed.logging.level == "trace" &&
                !trace_parsed.logging.include_private_job_entropy,
            "trace logging changed the explicit entropy option");

    std::string trace_stderr = valid_config();
    trace_stderr.replace(trace_stderr.find("\"logging\":{}"), 12,
        "\"logging\":{\"level\":\"trace\"}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            trace_stderr, {.validate_paths = false,
                           .validate_blocknotify_executable = false});
    }, "trace private job entropy logging to stderr was accepted");

    std::string stderr_target = valid_config();
    stderr_target.replace(stderr_target.find("\"logging\":{}"), 12,
        "\"logging\":{\"level\":\"debug\","
        "\"include_private_job_entropy\":true}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            stderr_target, {.validate_paths = false,
                            .validate_blocknotify_executable = false});
    }, "private job entropy logging to stderr was accepted");

    std::string info_level = valid_config();
    info_level.replace(info_level.find("\"logging\":{}"), 12,
        "\"logging\":{\"level\":\"info\","
        "\"file\":\"/tmp/mss-debug.jsonl\","
        "\"include_private_job_entropy\":true}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            info_level, {.validate_paths = false,
                         .validate_blocknotify_executable = false});
    }, "private job entropy logging at info was accepted");
}

void test_api_share_analytics_bounds()
{
    std::string configured = valid_config();
    configured.replace(configured.find("\"database\":{") + 12, 0,
                       "\"min_persisted_share_difficulty\":100,");
    configured.replace(configured.find("\"api\":{\"enabled\":false}"), 23,
        "\"api\":{\"enabled\":false,\"top_shares_limit\":7,"
        "\"recent_high_shares_limit\":9,"
        "\"recent_high_share_min_difficulty\":123}");
    const auto parsed = monero_solo::parse_config_json(
        configured, {.validate_paths = false,
                     .validate_blocknotify_executable = false});
    require(parsed.api.top_shares_limit == 7 &&
                parsed.api.recent_high_shares_limit == 9 &&
                parsed.api.recent_high_share_min_difficulty == 123,
            "share analytics settings did not parse");

    std::string too_many_top = valid_config();
    too_many_top.replace(too_many_top.find("\"api\":{\"enabled\":false}"), 23,
        "\"api\":{\"enabled\":false,\"top_shares_limit\":101}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            too_many_top, {.validate_paths = false,
                           .validate_blocknotify_executable = false});
    }, "top-share cap above 100 accepted");

    std::string zero_recent = valid_config();
    zero_recent.replace(zero_recent.find("\"api\":{\"enabled\":false}"), 23,
        "\"api\":{\"enabled\":false,\"recent_high_shares_limit\":0}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            zero_recent, {.validate_paths = false,
                          .validate_blocknotify_executable = false});
    }, "zero recent-high share cap accepted");

    std::string zero_threshold = valid_config();
    zero_threshold.replace(zero_threshold.find("\"api\":{\"enabled\":false}"),
                           23,
        "\"api\":{\"enabled\":false,"
        "\"recent_high_share_min_difficulty\":0}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            zero_threshold, {.validate_paths = false,
                             .validate_blocknotify_executable = false});
    }, "zero recent-high share threshold accepted");

    std::string below_persistence = valid_config();
    below_persistence.replace(
        below_persistence.find("\"api\":{\"enabled\":false}"), 23,
        "\"api\":{\"enabled\":false,"
        "\"recent_high_share_min_difficulty\":79999999999}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            below_persistence, {.validate_paths = false,
                                .validate_blocknotify_executable = false});
    }, "API recent-high threshold below persistence floor accepted");
}

void test_database_persistence_bounds()
{
    std::string minimum = valid_config();
    minimum.replace(minimum.find("\"database\":{"), 12,
                    "\"database\":{"
                    "\"min_persisted_share_difficulty\":1,"
                    "\"accounting_flush_interval_ms\":10,");
    const auto parsed = monero_solo::parse_config_json(
        minimum, {.validate_paths = false,
                  .validate_blocknotify_executable = false});
    require(parsed.database.min_persisted_share_difficulty == 1 &&
                parsed.database.accounting_flush_interval_ms == 10,
            "minimum database persistence settings did not parse");

    std::string zero_difficulty = valid_config();
    zero_difficulty.replace(zero_difficulty.find("\"database\":{"), 12,
        "\"database\":{\"min_persisted_share_difficulty\":0,");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            zero_difficulty, {.validate_paths = false,
                              .validate_blocknotify_executable = false});
    }, "zero persisted-share difficulty accepted");

    std::string short_flush = valid_config();
    short_flush.replace(short_flush.find("\"database\":{"), 12,
        "\"database\":{\"accounting_flush_interval_ms\":9,");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            short_flush, {.validate_paths = false,
                          .validate_blocknotify_executable = false});
    }, "too-short accounting flush interval accepted");

    std::string long_flush = valid_config();
    long_flush.replace(long_flush.find("\"database\":{"), 12,
        "\"database\":{\"accounting_flush_interval_ms\":60001,");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            long_flush, {.validate_paths = false,
                         .validate_blocknotify_executable = false});
    }, "too-long accounting flush interval accepted");

    for (const std::string_view removed_key :
         {std::string_view("\"retention_days\":0,"),
          std::string_view("\"store_rejected_shares\":false,")}) {
        std::string legacy = valid_config();
        legacy.replace(legacy.find("\"database\":{"), 12,
                       "\"database\":{" + std::string(removed_key));
        rejects([&] {
            (void)monero_solo::parse_config_json(
                legacy, {.validate_paths = false,
                         .validate_blocknotify_executable = false});
        }, "removed database persistence key accepted");
    }
}

void test_removed_job_history()
{
    std::string legacy = valid_config();
    legacy.replace(legacy.find("\"access_password\":null"), 22,
                   "\"access_password\":null,\"job_history\":6");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            legacy, {.validate_paths = false,
                     .validate_blocknotify_executable = false});
    }, "removed stratum.job_history key accepted");
}

void test_strict_json_and_unknown_keys()
{
    std::string old_schema = valid_config();
    old_schema.replace(old_schema.find("\"schema_version\":2"), 18,
                       "\"schema_version\":1");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            old_schema, {.validate_paths = false,
                         .validate_blocknotify_executable = false});
    }, "configuration schema version 1 accepted");

    std::string duplicate = valid_config();
    duplicate.replace(duplicate.find("\"schema_version\":2"), 18,
                      "\"schema_version\":2,\"schema_version\":2");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            duplicate, {.validate_paths = false,
                        .validate_blocknotify_executable = false});
    }, "duplicate key accepted");

    std::string unknown = valid_config();
    unknown.replace(unknown.find("\"entropy\":{}"), 12,
                    "\"entropy\":{\"typo\":1}");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            unknown, {.validate_paths = false,
                      .validate_blocknotify_executable = false});
    }, "unknown key accepted");

    std::string fractional = valid_config();
    fractional.replace(fractional.find("1048576"), 7, "1e6");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            fractional, {.validate_paths = false,
                         .validate_blocknotify_executable = false});
    }, "exponent integer accepted");
}

void test_cross_capacity_and_security_rules()
{
    std::string too_small = valid_config();
    too_small.replace(too_small.find("\"database\":{"), 12,
                      "\"database\":{\"max_writer_queue_items\":1024,");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            too_small, {.validate_paths = false,
                        .validate_blocknotify_executable = false});
    }, "impossible writer reserve accepted");

    std::string trusted_too_small = valid_config();
    const auto verifier_enabled = trusted_too_small.find(
        "\"enabled\":true", trusted_too_small.find("\"verifier\""));
    trusted_too_small.replace(verifier_enabled, 14, "\"enabled\":false");
    trusted_too_small.replace(trusted_too_small.find("\"database\":{"), 12,
                              "\"database\":{\"max_writer_queue_items\":1024,");
    trusted_too_small.replace(trusted_too_small.find("\"defense\":{"), 11,
        "\"defense\":{\"candidate_global_inflight\":1024,");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            trusted_too_small, {.validate_paths = false,
                                .validate_blocknotify_executable = false});
    }, "trusted-mode claimed-candidate writer reserve was not enforced");

    std::string public_no_defense = valid_config();
    public_no_defense.replace(public_no_defense.find("127.0.0.1:3333"), 14,
                              "0.0.0.0:3333");
    public_no_defense.replace(public_no_defense.find("\"enabled\":true", public_no_defense.find("\"defense\"")),
                              14, "\"enabled\":false");
    rejects([&] {
        (void)monero_solo::parse_config_json(
            public_no_defense, {.validate_paths = false,
                                .validate_blocknotify_executable = false});
    }, "public listener without defense accepted");

    std::string public_password_only = public_no_defense;
    public_password_only.replace(
        public_password_only.find("\"access_password\":null"), 22,
        "\"access_password\":\"rental-secret\"");
    const auto password_config = monero_solo::parse_config_json(
        public_password_only, {.validate_paths = false,
                               .validate_blocknotify_executable = false});
    require(!password_config.defense.enabled &&
                password_config.stratum.access_password == "rental-secret",
            "password-protected public listener could not use simple defense mode");
}

void test_blocknotify_tokenizer(std::string_view test_executable)
{
    const std::string executable =
        std::filesystem::canonical(test_executable).string();
    auto argv = monero_solo::parse_blocknotify_argv(
        quote_blocknotify_argument(executable) +
            " notify \"%s value\" 'literal;pipe'",
        true);
    require(argv.size() == 4, "blocknotify argv size incorrect");
    require(argv[0] == executable, "blocknotify executable path changed");
    require(argv[2] == "%s value", "double quoting incorrect");
    require(argv[3] == "literal;pipe", "metacharacter was interpreted");
    rejects([] { (void)monero_solo::parse_blocknotify_argv("true %s", false); },
            "relative executable accepted");
    rejects([] { (void)monero_solo::parse_blocknotify_argv("/bin/true", true); },
            "missing placeholder accepted");
}

void test_complete_example_config()
{
    const std::filesystem::path project_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto config = monero_solo::load_config_file(
        (project_root / "config.example.json").string(),
        {.validate_paths = false, .validate_blocknotify_executable = false});
    require(config.schema_version == 2, "example schema version differs");
    require(config.wallet_address ==
                "44AFFq5kSiGBoZ4NMDwYtN18obc8AemS33DBLWs3H7otXft3XjrpDtQGv7SqSsaBYBb98uNbr2VBBEt7f2wfn3RVGQBEP3A",
            "example payout address differs");
    require(config.stratum.listen.size() == 2,
            "example listener count differs");
    require(config.api.top_shares_limit == 100 &&
                config.api.recent_high_shares_limit == 100 &&
                config.api.recent_high_share_min_difficulty == 80'000'000'000ULL &&
                config.database.min_persisted_share_difficulty ==
                    80'000'000'000ULL &&
                config.database.accounting_flush_interval_ms == 1000,
            "example share analytics defaults differ");
    require(config.verifier.enabled, "example must default to verified mode");
    require(config.events.enabled && config.api.enabled && !config.defense.enabled,
            "example data/simple-defense defaults differ");
}

} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc >= 1 && argv[0] != nullptr,
                "test executable path is unavailable");
        test_defaults_and_required_fields();
        test_api_share_analytics_bounds();
        test_database_persistence_bounds();
        test_removed_job_history();
        test_private_job_entropy_logging_gate();
        test_strict_json_and_unknown_keys();
        test_cross_capacity_and_security_rules();
        test_blocknotify_tokenizer(argv[0]);
        test_complete_example_config();
        std::cout << "config tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "config tests failed: " << error.what() << '\n';
        return 1;
    }
}
