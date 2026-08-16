/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/api.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

void require(const bool condition, const char *const message)
{
    if (!condition) throw std::runtime_error(message);
}

Json body(const monero_solo::HttpResponse &response)
{
    return Json::parse(response.body);
}

monero_solo::HttpRequest request(std::string path,
                                 std::string query = {},
                                 std::string method = "GET",
                                 std::optional<std::string> token = "correct-token")
{
    monero_solo::HttpRequest result;
    result.method = std::move(method);
    result.path = std::move(path);
    result.query = std::move(query);
    result.target = result.path + (result.query.empty() ? "" : "?" + result.query);
    result.version = "HTTP/1.1";
    result.peer = "127.0.0.1";
    if (token.has_value()) {
        result.headers.emplace("authorization", "Bearer " + *token);
    }
    return result;
}

monero_solo::ApiReadinessSnapshot ready_snapshot()
{
    const monero_solo::ApiComponentState ready{true, false, std::nullopt};
    return {
        3736190,
        ready,
        ready,
        ready,
        ready,
        ready,
        ready,
    };
}

monero_solo::ApiService make_service(monero_solo::ApiDataSource source,
                                     std::optional<std::string> token =
                                         "correct-token",
                                     monero_solo::ApiConfig api = {})
{
    monero_solo::ApiServiceOptions options;
    options.api = std::move(api);
    options.api.enabled = false;
    options.api.access_token = std::move(token);
    options.api.max_page_size = 1000;
    options.identity.version = "0.1.0";
    options.identity.git_commit =
        "0123456789abcdef0123456789abcdef01234567";
    options.identity.session_id = "0123456789abcdef0123456789abcdef";
    options.identity.started_unix_us = 1'700'000'000'000'000;
    return monero_solo::ApiService(
        std::move(options), nullptr, std::move(source),
        [] { return std::int64_t{1'700'000'123'456'789}; });
}

void test_auth_method_and_health()
{
    monero_solo::ApiDataSource source;
    source.readiness = [] { return ready_snapshot(); };
    source.singleton = [](const monero_solo::ApiSingleton resource)
        -> std::optional<Json> {
        if (resource == monero_solo::ApiSingleton::summary) {
            return Json{{"server", {{"version", "0.1.0"}}}};
        }
        return std::nullopt;
    };
    auto service = make_service(std::move(source));

    const auto missing = service.handle(request("/v1/health/live", {}, "GET",
                                                 std::nullopt));
    require(missing.status == 401, "missing token did not return 401");
    require(body(missing)["error"]["code"] == "authentication_required",
            "wrong authentication error code");
    require(missing.headers.size() == 1U &&
                missing.headers.front().first == "WWW-Authenticate",
            "401 omitted Bearer challenge");

    const auto wrong = service.handle(request("/v1/health/live", {}, "GET",
                                               "wrong-token"));
    require(wrong.status == 401, "wrong token did not return 401");

    const auto live = service.handle(request("/v1/health/live"));
    const Json live_body = body(live);
    require(live.status == 200 && live_body.size() == 3U,
            "live envelope is wrong");
    require(live_body["schema_version"] == 1 &&
                live_body["generated_at"] == "2023-11-14T22:15:23.456789Z" &&
                live_body["data"]["alive"] == true &&
                live_body["data"]["uptime_seconds"] == 123,
            "live endpoint fields are wrong");

    const auto ready = service.handle(request("/v1/health/ready"));
    const Json ready_body = body(ready);
    require(ready.status == 200 && ready_body["data"]["ready"] == true &&
                ready_body["data"]["components"].size() == 6U &&
                ready_body["data"]["components"]["template"]["reason"].is_null(),
            "ready endpoint is wrong");

    const auto summary = service.handle(request("/v1/summary"));
    require(summary.status == 200 &&
                body(summary)["data"]["server"]["version"] == "0.1.0",
            "summary callback route failed");

    const auto method = service.handle(request("/v1/summary", {}, "POST"));
    require(method.status == 405 && method.headers.front().first == "Allow" &&
                body(method)["error"]["code"] == "method_not_allowed",
            "control method was not rejected exactly");

    const auto root = service.handle(request("/"));
    require(root.status == 404 &&
                body(root)["error"]["code"] == "not_found",
            "root unexpectedly exposed HTML/dashboard content");
}

void test_not_ready_and_null_empty_auth_semantics()
{
    monero_solo::ApiDataSource source;
    source.readiness = [] {
        monero_solo::ApiReadinessSnapshot snapshot = ready_snapshot();
        snapshot.verifier = {false, false, "current seed preparing"};
        return snapshot;
    };
    auto service = make_service(std::move(source), std::nullopt);
    const auto response = service.handle(
        request("/v1/health/ready", {}, "GET", std::nullopt));
    require(response.status == 503 && body(response)["data"]["ready"] == false &&
                body(response)["data"]["components"]["verifier"]["reason"] ==
                    "current seed preparing",
            "readiness failure contract is wrong");

    monero_solo::ApiDataSource empty_source;
    empty_source.readiness = [] { return ready_snapshot(); };
    auto empty_token_service = make_service(std::move(empty_source), "");
    require(empty_token_service
                .handle(request("/v1/health/live", {}, "GET", std::nullopt))
                .status == 200,
            "empty API token did not disable authentication");
}

void test_collection_cursor_and_strict_queries()
{
    struct State {
        int calls{};
        std::uint64_t last_after{};
    } state;
    monero_solo::ApiDataSource source;
    source.collection = [&](const monero_solo::ApiCollectionRequest &query) {
        ++state.calls;
        state.last_after = query.after_database_id;
        require(query.resource == monero_solo::ApiCollection::shares,
                "wrong collection resource");
        require(query.limit == 2U, "wrong collection limit");
        require(query.filters.at("status") == "accepted",
                "collection lost filters");
        if (query.after_database_id == 0) {
            return monero_solo::ApiCollectionResult{
                Json::array({Json{{"id", "1"}}, Json{{"id", "2"}}}), 2};
        }
        require(query.after_database_id == 2U,
                "cursor decoded wrong database ID");
        return monero_solo::ApiCollectionResult{
            Json::array({Json{{"id", "3"}}}), std::nullopt};
    };
    auto service = make_service(std::move(source));
    const auto first = service.handle(
        request("/v1/shares", "status=accepted&limit=2"));
    const Json first_body = body(first);
    require(first.status == 200 && first_body["data"].size() == 2U &&
                first_body["page"]["limit"] == 2 &&
                first_body["page"]["next_cursor"].is_string(),
            "first collection page is wrong");
    const std::string cursor = first_body["page"]["next_cursor"];

    const auto second = service.handle(request(
        "/v1/shares", "status=accepted&limit=2&cursor=" + cursor));
    require(second.status == 200 && body(second)["data"].size() == 1U &&
                body(second)["page"]["next_cursor"].is_null() &&
                state.last_after == 2U,
            "second cursor page is wrong");

    const auto wrong_endpoint = service.handle(request(
        "/v1/events", "type=share_result&limit=2&cursor=" + cursor));
    require(wrong_endpoint.status == 400 &&
                body(wrong_endpoint)["error"]["code"] == "invalid_cursor",
            "cursor was reusable across resource tags");
    const auto wrong_filters = service.handle(request(
        "/v1/shares", "status=stale&limit=2&cursor=" + cursor));
    require(wrong_filters.status == 400 &&
                body(wrong_filters)["error"]["code"] == "invalid_cursor",
            "cursor was reusable across filters");
    const auto unknown = service.handle(
        request("/v1/shares", "status=accepted&wat=1"));
    require(unknown.status == 400 &&
                body(unknown)["error"]["code"] == "invalid_query",
            "unknown collection filter was accepted");
    const auto duplicate = service.handle(
        request("/v1/shares", "status=accepted&status=stale"));
    require(duplicate.status == 400, "duplicate query filter was accepted");
    const auto invalid_enum = service.handle(
        request("/v1/shares", "status=accepted,%20stale"));
    require(invalid_enum.status == 400, "whitespace enum list was accepted");
    const auto injection = service.handle(
        request("/v1/shares", "worker_id=1%20OR%201=1"));
    require(injection.status == 400, "SQL-like numeric filter was accepted");
    const auto overflow = service.handle(request(
        "/v1/shares", "worker_id=18446744073709551615"));
    require(overflow.status == 400 &&
                body(overflow)["error"]["code"] == "invalid_query",
            "out-of-range SQLite identifier was accepted");
    const auto detail_overflow = service.handle(
        request("/v1/shares/18446744073709551615"));
    require(detail_overflow.status == 400 &&
                body(detail_overflow)["error"]["code"] == "invalid_id",
            "out-of-range SQLite detail ID was accepted");
}

void test_detail_sensitive_view_and_missing_readers()
{
    monero_solo::ApiDataSource source;
    source.detail = [](const monero_solo::ApiDetailRequest &query)
        -> std::optional<Json> {
        if (query.resource == monero_solo::ApiDetail::submission &&
            query.id == "7") {
            require(query.include_blobs && query.authenticated,
                    "sensitive detail flags are wrong");
            return Json{{"submission", {{"id", "7"},
                                         {"frozen_block_blob", "00"}}},
                        {"attempts", Json::array()},
                        {"reconciliations", Json::array()},
                        {"blocknotify", nullptr}};
        }
        return std::nullopt;
    };
    auto authenticated = make_service(std::move(source));
    const auto detail = authenticated.handle(
        request("/v1/submissions/7", "include_blobs=true"));
    require(detail.status == 200 &&
                body(detail)["data"]["submission"]["id"] == "7",
            "authenticated sensitive detail failed");
    const auto invalid_id = authenticated.handle(request("/v1/shares/00"));
    require(invalid_id.status == 400 &&
                body(invalid_id)["error"]["code"] == "invalid_id",
            "noncanonical decimal detail ID was accepted");
    const auto missing = authenticated.handle(request("/v1/shares/8"));
    require(missing.status == 404, "missing detail did not return 404");

    monero_solo::ApiDataSource public_source;
    public_source.detail = [](const monero_solo::ApiDetailRequest &)
        -> std::optional<Json> { return Json::object(); };
    auto public_service = make_service(std::move(public_source), std::nullopt);
    const auto forbidden = public_service.handle(request(
        "/v1/submissions/7", "include_blobs=true", "GET", std::nullopt));
    require(forbidden.status == 403 &&
                body(forbidden)["error"]["code"] == "sensitive_view_disabled",
            "public API exposed sensitive blobs");

    monero_solo::ApiDataSource no_readers;
    auto unavailable = make_service(std::move(no_readers));
    require(unavailable.handle(request("/v1/summary")).status == 503,
            "missing summary snapshot did not report not ready");
    require(unavailable.handle(request("/v1/shares")).status == 500,
            "missing collection reader did not report query failure");
    require(unavailable.handle(request("/v1/rounds/current")).status == 503,
            "missing current round did not report 503");
}

void test_sqlite_backed_persisted_resources()
{
    std::string pattern = "/tmp/monero-solo-api-test-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = mkstemp(writable.data());
    require(descriptor >= 0, "could not create API test database path");
    (void)close(descriptor);
    const std::string path(writable.data());
    struct Cleanup {
        std::string path;
        ~Cleanup()
        {
            std::error_code ignored;
            (void)std::filesystem::remove(path, ignored);
            (void)std::filesystem::remove(path + "-wal", ignored);
            (void)std::filesystem::remove(path + "-shm", ignored);
        }
    } cleanup{path};

    monero_solo::Database database({path, 5000, false});
    monero_solo::PublicId session_public{};
    session_public[0] = 1;
    const std::int64_t session = database.start_session({
        session_public,
        1'700'000'000'000'000,
        "0.1.0",
        std::string("856c015de433a23fe45d88a18dc08c821e50f1cb"),
    });
    const std::int64_t worker = database.upsert_worker(
        {"rig-a", "rigid-a", 1'700'000'001'000'000});
    monero_solo::PublicId connection_public{};
    connection_public[0] = 2;
    const std::int64_t connection = database.insert_connection({
        connection_public,
        session,
        worker,
        AF_INET,
        {127, 0, 0, 1},
        33333,
        "127.0.0.1:3333",
        "XMRig/6.26.0",
        1'700'000'001'000'000,
    });
    monero_solo::PublicId job_public{};
    job_public[0] = 5;
    std::array<std::uint8_t, 4> nonce{1, 2, 3, 4};
    const std::int64_t share = database.insert_share({
        .session_id = session,
        .connection_id = connection,
        .worker_id = worker,
        .job_public_id = job_public,
        .template_generation = 1,
        .height = 3736190,
        .request_sequence = 1,
        .miner_request_id_type = std::string("integer"),
        .miner_request_id_text = std::string("2"),
        .received_unix_us = 1'700'000'004'000'000,
        .nonce = nonce,
        .assigned_difficulty_dec = std::string("1048576"),
        .network_difficulty_dec = std::string("1000"),
        .height_is_older = false,
        .claimed_candidate = false,
        .candidate_admission = "not_candidate",
        .status = "received",
        .provenance = "pending",
    });
    monero_solo::Hash32 computed_hash{};
    computed_hash[0] = 0xaa;
    database.insert_share_hash(share, "computed", computed_hash, true, false);
    const auto accepted = database.accept_share({
        .share_id = share,
        .completed_unix_us = 1'700'000'004'500'000,
        .assigned_difficulty_dec = "1048576",
        .source = monero_solo::HashrateSource::verified,
        .actual_difficulty_dec = "100000000000",
        .verifier_ticket_dec = std::nullopt,
        .verifier_seed_id_dec = std::nullopt,
        .verifier_queue_ns = std::nullopt,
        .verifier_hash_ns = std::nullopt,
        .verifier_total_ns = std::nullopt,
    });
    require(accepted.accepted, "API fixture share did not accept");
    const std::int64_t retained_share = database.ensure_share_persisted(
        share, "high_difficulty").share_id;
    require(retained_share > 0,
            "high-difficulty fixture share was not retained");
    const auto insert_fractional_share =
        [&](const std::uint8_t public_marker,
            const std::uint64_t request_sequence,
            const std::string &assigned_difficulty,
            const std::string &network_difficulty,
            const std::string &actual_difficulty,
            const std::int64_t received_unix_us) {
            monero_solo::PublicId fractional_job_public{};
            fractional_job_public[0] = public_marker;
            std::array<std::uint8_t, 4> fractional_nonce{
                public_marker, 2, 3, 4};
            const std::int64_t fractional_share = database.insert_share({
                .session_id = session,
                .connection_id = connection,
                .worker_id = worker,
                .job_public_id = fractional_job_public,
                .template_generation = 1,
                .height = 3736190,
                .request_sequence = request_sequence,
                .miner_request_id_type = std::string("integer"),
                .miner_request_id_text = std::to_string(request_sequence),
                .received_unix_us = received_unix_us,
                .nonce = fractional_nonce,
                .assigned_difficulty_dec = assigned_difficulty,
                .network_difficulty_dec = network_difficulty,
                .height_is_older = false,
                .claimed_candidate = false,
                .candidate_admission = "not_candidate",
                .status = "received",
                .provenance = "pending",
            });
            const auto result = database.accept_share({
                .share_id = fractional_share,
                .completed_unix_us = received_unix_us + 100'000,
                .assigned_difficulty_dec = assigned_difficulty,
                .source = monero_solo::HashrateSource::verified,
                .actual_difficulty_dec = actual_difficulty,
                .verifier_ticket_dec = std::nullopt,
                .verifier_seed_id_dec = std::nullopt,
                .verifier_queue_ns = std::nullopt,
                .verifier_hash_ns = std::nullopt,
                .verifier_total_ns = std::nullopt,
            });
            require(result.accepted,
                    "fractional API fixture share did not accept");
            require(result.round_id == accepted.round_id,
                    "fixture shares unexpectedly crossed rounds");
            return fractional_share;
        };
    const std::int64_t threshold_share_alias = insert_fractional_share(
        7, 2, "1", "200000000", "80000000000",
        1'700'000'004'100'000);
    const std::int64_t threshold_share = database.ensure_share_persisted(
        threshold_share_alias, "high_difficulty").share_id;
    const std::int64_t below_threshold_share = insert_fractional_share(
        8, 3, "2", "400000000", "79999999999",
        1'700'000'004'200'000);
    require(threshold_share > 0 && below_threshold_share < 0,
            "selective share persistence threshold is wrong");

    monero_solo::Hash32 candidate_key{};
    monero_solo::Hash32 miner_tx_hash{};
    candidate_key[0] = 0xc1;
    miner_tx_hash[0] = 0xc2;
    const auto candidate = database.journal_candidate({
        .candidate_key = candidate_key,
        .first_share_id = retained_share,
        .session_id = session,
        .round_id = accepted.round_id,
        .job_public_id = job_public,
        .template_generation = 1,
        .connection_id = connection,
        .height = 3736190,
        .peer_family = AF_INET,
        .peer_address = {127, 0, 0, 1},
        .frozen_block_blob = {1, 2, 3, 4},
        .miner_tx_hash = miner_tx_hash,
        .expected_block_id = std::nullopt,
        .max_attempts = 4,
        .created_unix_us = 1'700'000'004'700'000,
    });
    require(candidate.inserted &&
                database.accept_candidate(candidate.candidate_id,
                                          1'700'000'004'800'000),
            "API fixture round did not close");
    const std::int64_t current_round_id = database.current_open_round_id();
    require(current_round_id != accepted.round_id,
            "API fixture did not open a successor round");

    monero_solo::ApiDataSource live;
    live.readiness = [] { return ready_snapshot(); };
    live.singleton = [](const monero_solo::ApiSingleton resource)
        -> std::optional<Json> {
        if (resource == monero_solo::ApiSingleton::summary) {
            return Json{{"server",
                         {{"version", "0.1.0"},
                          {"git_commit",
                           "0123456789abcdef0123456789abcdef01234567"},
                          {"session_id",
                           "01000000000000000000000000000000"},
                          {"started_at", "2023-11-14T22:13:20.000000Z"},
                          {"uptime_seconds", 5},
                          {"network", "mainnet"},
                          {"verification", "verified"},
                          {"stratum_authentication", "enabled"},
                          {"api_authentication", "enabled"}}}};
        }
        if (resource == monero_solo::ApiSingleton::daemon) {
            return Json{{"ready", true},
                        {"rpc_state", "healthy"},
                        {"zmq_state", "disabled"},
                        {"height", 3736190},
                        {"template_generation", "1"},
                        {"template_id", nullptr}};
        }
        return std::nullopt;
    };
    monero_solo::ApiConfig analytics_api;
    analytics_api.top_shares_limit = 2;
    analytics_api.recent_high_shares_limit = 1;
    analytics_api.recent_high_share_min_difficulty = 80'000'000'000ULL;
    auto source = monero_solo::make_sqlite_api_data_source({
        {path, 5000, false},
        analytics_api,
        monero_solo::HashrateSource::verified,
        std::move(live),
        [] { return std::int64_t{1'700'000'005'000'000}; },
        [] {
            return monero_solo::DatabaseWriterStats{
                .queued_items = 7,
                .queued_bytes = 3584,
                .priority_items = 2,
                .pending_accounting_items = 3,
                .pending_transient_shares = 2,
            };
        },
    });
    auto service = make_service(
        std::move(source), "correct-token", analytics_api);

    const Json connections = body(service.handle(request("/v1/connections")));
    require(connections["data"].size() == 1U &&
                connections["data"][0]["id"] ==
                    "02000000000000000000000000000000" &&
                connections["data"][0]["session_id"] ==
                    "01000000000000000000000000000000" &&
                connections["data"][0]["peer"] == "127.0.0.1" &&
                connections["data"][0]["worker_id"] == std::to_string(worker),
            "SQLite connection serialization is wrong");

    require(service.handle(request("/v1/jobs")).status == 404 &&
                service.handle(request("/v1/templates")).status == 404,
            "removed transient job/template history routes are still exposed");

    const Json shares = body(service.handle(request("/v1/shares")));
    require(shares["data"].size() == 2U &&
                shares["data"][0]["id"] == std::to_string(retained_share) &&
                shares["data"][0]["status"] == "accepted" &&
                shares["data"][0]["credited_difficulty"] == "1048576" &&
                shares["data"][0]["actual_difficulty"] == "100000000000" &&
                shares["data"][0]["round_id"] ==
                    std::to_string(accepted.round_id) &&
                shares["data"][0]["height"] == 3736190 &&
                shares["data"][0]["template_generation"] == "1" &&
                shares["data"][0]["retention_reason"] == "high_difficulty",
            "SQLite share serialization is wrong");

    const Json minimum = body(service.handle(
        request("/v1/shares", "min_difficulty=1048576")));
    require(minimum["data"].size() == 1U,
            "SQLite minimum-difficulty filter rejected an equal value");
    const Json above_minimum = body(service.handle(
        request("/v1/shares", "min_difficulty=1048577")));
    require(above_minimum["data"].empty(),
            "SQLite minimum-difficulty filter accepted a smaller value");

    const Json hashes = body(service.handle(request("/v1/hashes")));
    require(hashes["data"].size() == 1U &&
                hashes["data"][0]["share_id"] == std::to_string(retained_share) &&
                hashes["data"][0]["role"] == "computed" &&
                hashes["data"][0]["assigned_difficulty"] == "1048576" &&
                hashes["data"][0]["actual_difficulty"] == "100000000000" &&
                hashes["data"][0]["network_difficulty"] == "1000" &&
                hashes["data"][0]["credited_difficulty"] == "1048576" &&
                hashes["data"][0]["provenance"] == "verified" &&
                hashes["data"][0]["round_id"] ==
                    std::to_string(accepted.round_id),
            "SQLite hash resource is not self-contained");

    const Json top = body(service.handle(request("/v1/shares/top")));
    require(top["data"].size() == 2U &&
                top["data"][0]["id"] == std::to_string(retained_share) &&
                top["data"][1]["id"] == std::to_string(threshold_share) &&
                top["selection"]["kind"] == "top_actual_difficulty" &&
                top["selection"]["configured_limit"] == 2,
            "global top-share ranking/order/cap is wrong");
    const Json round_top = body(service.handle(request(
        "/v1/shares/top", "round_id=" + std::to_string(accepted.round_id))));
    require(round_top["data"].size() == 2U &&
                round_top["selection"]["round_id"] ==
                    std::to_string(accepted.round_id),
            "per-round top-share ranking is wrong");
    const Json empty_round_top = body(service.handle(request(
        "/v1/shares/top", "round_id=999999")));
    require(empty_round_top["data"].empty(),
            "per-round top-share filter leaked another round");
    require(service.handle(request("/v1/shares/top", "limit=3")).status == 400,
            "top-share configured cap could be exceeded");
    require(service.handle(request("/v1/shares/top", "cursor=x")).status == 400,
            "top-share bounded snapshot accepted a cursor");

    const Json recent_high = body(
        service.handle(request("/v1/shares/recent-high")));
    require(recent_high["data"].size() == 1U &&
                recent_high["data"][0]["id"] ==
                    std::to_string(threshold_share) &&
                recent_high["data"][0]["actual_difficulty"] == "80000000000" &&
                recent_high["selection"]["min_actual_difficulty"] ==
                    "80000000000" &&
                recent_high["selection"]["configured_limit"] == 1 &&
                recent_high["data"][0]["id"] !=
                    std::to_string(below_threshold_share),
            "recent high-share threshold/order/cap is wrong");

    const Json share_detail = body(service.handle(
        request("/v1/shares/" + std::to_string(retained_share))));
    require(share_detail["data"]["share"]["status"] == "accepted" &&
                share_detail["data"]["submission_url"] ==
                    "/v1/submissions/" +
                        std::to_string(candidate.candidate_id),
            "SQLite share detail is wrong");

    const Json round_history = body(service.handle(request("/v1/rounds")));
    require(round_history["data"].size() == 2U &&
                round_history["data"][0]["id"] ==
                    std::to_string(accepted.round_id) &&
                round_history["data"][0]["state"] == "closed" &&
                round_history["data"][0]["accepted_share_count"] == "3" &&
                round_history["data"][0]["max_share_height"] == 3736190 &&
                round_history["data"][0]["credited_difficulty"] == "1048579" &&
                round_history["data"][0]["estimated_hashes"] == "1048579" &&
                round_history["data"][0]["effort_finalized_at"].is_string() &&
                round_history["data"][0]["effort"]["value"] ==
                    "104857.600001" &&
                round_history["data"][0]["effort"]["finalized"] == true &&
                round_history["data"][0]["effort"]["segments"].size() == 3U &&
                round_history["data"][0]["effort"]["segments"][1]
                             ["effort_percent"] ==
                    "0.000000" &&
                round_history["data"][0]["effort"]["segments"][2]
                             ["effort_percent"] ==
                    "0.000000",
            "SQLite historical round effort/finalization is wrong");

    const Json current_round = body(
        service.handle(request("/v1/rounds/current")));
    require(current_round["data"]["id"] == std::to_string(current_round_id) &&
                current_round["data"]["state"] == "open" &&
                current_round["data"]["accepted_share_count"] == "0" &&
                current_round["data"]["max_share_height"] == 0 &&
                current_round["data"]["estimated_hashes"] == "0" &&
                current_round["data"]["effort_finalized_at"].is_null() &&
                current_round["data"]["effort"]["value"] == "0.000000" &&
                current_round["data"]["effort"]["finalized"] == false &&
                current_round["data"]["effort"]["segments"].empty(),
            "SQLite current-round statistics are wrong");

    const Json events = body(service.handle(request("/v1/events")));
    require(events["data"].size() >= 3U &&
                events["data"][0]["session_id"] ==
                    "01000000000000000000000000000000" &&
                events["data"][0]["payload"]["payload_schema_version"] == 1,
            "SQLite event serialization is wrong");

    const auto summary_response = service.handle(request("/v1/summary"));
    const Json summary = body(summary_response);
    require(summary_response.status == 200 &&
                summary["data"].size() == 8U &&
                summary["data"]["connections"]["active"] == 1 &&
                summary["data"]["connections"]["total"] == "1" &&
                summary["data"]["workers"]["active"] == 1 &&
                summary["data"]["shares"]["accepted"] == "3" &&
                summary["data"]["shares"]["pending"] == "2" &&
                summary["data"]["shares"]["total"] == "5" &&
                summary["data"]["candidates"]["accepted"] == "1" &&
                summary["data"]["candidates"]["total"] == "1" &&
                summary["data"]["round"]["state"] == "open" &&
                summary["data"]["round"]["estimated_hashes"] == "0" &&
                summary["data"]["round"]["accepted_share_count"] == "0" &&
                summary["data"]["round"]["max_share_height"] == 0 &&
                summary["data"]["round"]["effort"]["value"] ==
                    "0.000000" &&
                summary["data"]["daemon"]["rpc"] == "healthy" &&
                summary["data"]["hashrate"]["source"] == "verified" &&
                summary["data"]["hashrate"]["1m"] == "17476",
            "SQLite summary aggregation/merge is wrong");

    const auto persistence_response = service.handle(request("/v1/persistence"));
    const Json persistence = body(persistence_response);
    require(persistence_response.status == 200 &&
                persistence["data"]["schema_version"] == 3 &&
                persistence["data"]["journal_mode"] == "wal" &&
                persistence["data"]["synchronous"] == "full" &&
                persistence["data"]["foreign_keys"] == true &&
                persistence["data"]["unresolved_candidates"] == "0" &&
                persistence["data"]["pending_blocknotify"] == "0" &&
                persistence["data"]["writer_queue_items"] == 7 &&
                persistence["data"]["writer_queue_bytes"] == 3584 &&
                persistence["data"]["writer_priority_items"] == 2 &&
                persistence["data"]["pending_accounting_items"] == 3 &&
                persistence["data"]["pending_transient_shares"] == 2 &&
                persistence["data"]["last_commit_at"].is_string(),
            "SQLite persistence snapshot is wrong");
}

} // namespace

int main()
{
    try {
        test_auth_method_and_health();
        test_not_ready_and_null_empty_auth_semantics();
        test_collection_cursor_and_strict_queries();
        test_detail_sensitive_view_and_missing_readers();
        test_sqlite_backed_persisted_resources();
        std::cout << "API service tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "API service test failure: " << error.what() << '\n';
        return 1;
    }
}
