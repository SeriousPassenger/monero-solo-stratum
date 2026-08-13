/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_solo/database.hpp"
#include "monero_solo/duplicate_registry.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace monero_solo;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <std::size_t N>
std::array<std::uint8_t, N> bytes(std::uint8_t first)
{
    std::array<std::uint8_t, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        result[i] = static_cast<std::uint8_t>(first + i);
    }
    return result;
}

std::filesystem::path test_database_path()
{
    return std::filesystem::temp_directory_path() /
           ("monero-solo-database-tests-" + std::to_string(::getpid()) + ".sqlite3");
}

void remove_database(const std::filesystem::path &path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
    std::filesystem::remove(path.string() + ".lock", ignored);
}

std::int64_t scalar(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "could not open test database for inspection");
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("could not prepare inspection query");
    }
    require(sqlite3_step(statement) == SQLITE_ROW,
            "inspection query returned no row");
    const std::int64_t result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

std::string scalar_text(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
                SQLITE_OK,
            "could not open test database for text inspection");
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("could not prepare text inspection query");
    }
    require(sqlite3_step(statement) == SQLITE_ROW &&
                sqlite3_column_type(statement, 0) == SQLITE_TEXT,
            "text inspection query returned no text row");
    const auto *value = reinterpret_cast<const char *>(
        sqlite3_column_text(statement, 0));
    const std::string result(value == nullptr ? "" : value);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

bool statement_rejected(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) ==
                SQLITE_OK,
            "could not open test database for rejected-statement inspection");
    char *message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    sqlite3_free(message);
    sqlite3_close(db);
    return result != SQLITE_OK;
}

struct Fixture {
    std::int64_t session_id{};
    std::int64_t worker_id{};
    std::int64_t connection_id{};
    std::int64_t template_id{};
    std::int64_t job_id{};
};

Fixture create_fixture(Database &database, std::int64_t now)
{
    SessionStart session;
    session.public_id = bytes<16>(1);
    session.started_unix_us = now;
    session.version = "database-test";
    session.verifier_commit = std::string(40, 'a');
    const std::int64_t session_id = database.start_session(session);

    WorkerInsert worker;
    worker.login = "rig-one";
    worker.rigid = "r1";
    worker.seen_unix_us = now;
    const std::int64_t worker_id = database.upsert_worker(worker);

    ConnectionInsert connection;
    connection.public_id = bytes<16>(20);
    connection.session_id = session_id;
    connection.worker_id = worker_id;
    connection.peer_family = 2;
    connection.peer_address = {127, 0, 0, 1};
    connection.peer_port = 3333;
    connection.listen_address = "127.0.0.1:3333";
    connection.agent = "database-tests";
    connection.opened_unix_us = now;
    const std::int64_t connection_id = database.insert_connection(connection);
    database.authenticate_connection(connection_id, worker_id,
                                     "database-tests/authenticated", now + 1);

    PublicTemplateInsert block_template;
    block_template.session_id = session_id;
    block_template.generation = 1;
    block_template.height = 42;
    block_template.prev_hash = bytes<32>(40);
    block_template.seed_hash = bytes<32>(80);
    block_template.difficulty_dec = "1000000";
    block_template.reserved_offset = 8;
    block_template.blocktemplate_blob = {1, 2, 3, 4};
    block_template.blockhashing_blob = {5, 6, 7, 8};
    block_template.fetched_unix_us = now;
    block_template.fetch_reason = "test";
    const std::int64_t template_id =
        database.insert_public_template(block_template);

    PrivateJobInsert job;
    job.public_job_id = bytes<16>(120);
    job.connection_id = connection_id;
    job.template_id = template_id;
    job.height = 42;
    job.entropy = bytes<16>(150);
    job.seed_hash = block_template.seed_hash;
    job.mspv_seed_id_dec = "18446744073709551615";
    job.assigned_difficulty_dec = "120";
    job.target64_le = bytes<8>(200);
    job.network_difficulty_dec = "1000000";
    job.nonce_offset = 2;
    job.reserved_offset = 8;
    job.private_block_blob = {9, 10, 11, 12};
    job.hashing_blob = {13, 14, 15, 16};
    job.created_unix_us = now;
    job.queued_unix_us = std::nullopt;
    job.expires_unix_us = now + 120'000'000;
    const std::int64_t job_id = database.insert_private_job(job);
    database.mark_job_queued(job_id, now + 2);

    return Fixture{
        .session_id = session_id,
        .worker_id = worker_id,
        .connection_id = connection_id,
        .template_id = template_id,
        .job_id = job_id,
    };
}

std::int64_t create_job(Database &database, const Fixture &fixture,
                        std::uint8_t tag, std::uint64_t height,
                        std::string assigned_difficulty,
                        std::string network_difficulty,
                        std::int64_t now)
{
    PrivateJobInsert job;
    job.public_job_id = bytes<16>(tag);
    job.connection_id = fixture.connection_id;
    job.template_id = fixture.template_id;
    job.height = height;
    job.entropy = bytes<16>(static_cast<std::uint8_t>(tag + 32U));
    job.seed_hash = bytes<32>(80);
    job.assigned_difficulty_dec = std::move(assigned_difficulty);
    job.target64_le = bytes<8>(static_cast<std::uint8_t>(tag + 64U));
    job.network_difficulty_dec = std::move(network_difficulty);
    job.nonce_offset = 2;
    job.reserved_offset = 8;
    job.private_block_blob = {tag, 10, 11, 12};
    job.hashing_blob = {tag, 13, 14, 15};
    job.created_unix_us = now;
    job.expires_unix_us = now + 120'000'000;
    return database.insert_private_job(job);
}

std::int64_t create_share(Database &database, const Fixture &fixture,
                          std::uint64_t sequence, std::uint8_t nonce_last,
                          bool claimed_candidate,
                          std::string assigned_difficulty = "120",
                          std::string network_difficulty = "1000000")
{
    ShareInsert share;
    share.connection_id = fixture.connection_id;
    share.worker_id = fixture.worker_id;
    share.job_id = fixture.job_id;
    share.request_sequence = sequence;
    share.miner_request_id_type = "integer";
    share.miner_request_id_text = std::to_string(sequence);
    share.received_unix_us = 1'700'000'000'000'000 +
                             static_cast<std::int64_t>(sequence);
    share.nonce = std::array<std::uint8_t, 4>{0, 0, 0, nonce_last};
    share.assigned_difficulty_dec = std::move(assigned_difficulty);
    share.network_difficulty_dec = std::move(network_difficulty);
    share.claimed_candidate = claimed_candidate;
    share.status = "verifying";
    share.provenance = "pending";
    return database.insert_share(share);
}

CandidateJournal make_candidate(const Fixture &fixture, std::int64_t share_id,
                                std::uint8_t tag, std::uint32_t max_attempts,
                                std::int64_t now)
{
    CandidateJournal value;
    value.candidate_key = bytes<32>(tag);
    value.first_share_id = share_id;
    value.job_id = fixture.job_id;
    value.connection_id = fixture.connection_id;
    value.height = 42;
    value.peer_family = 2;
    value.peer_address = {127, 0, 0, 1};
    value.frozen_block_blob = {tag, 1, 2, 3, 4};
    value.miner_tx_hash = bytes<32>(static_cast<std::uint8_t>(tag + 40));
    value.expected_block_id = bytes<32>(static_cast<std::uint8_t>(tag + 80));
    value.max_attempts = max_attempts;
    value.created_unix_us = now;
    return value;
}

void run_tests()
{
    const auto path = test_database_path();
    remove_database(path);
    constexpr std::int64_t now = 1'700'000'000'000'000;

    Fixture fixture;
    std::int64_t accepted_candidate_id = 0;
    std::int64_t recovered_candidate_id = 0;
    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 2345,
            .blocknotify_enabled = true,
        });
        require(database.schema_version() == 2, "schema version mismatch");
        require(scalar(path, "PRAGMA user_version") == 2,
                "SQLite user_version was not bumped to 2");
        require(scalar(path,
                       "SELECT count(*) FROM sqlite_schema WHERE type='index' "
                       "AND name IN ('shares_round_status',"
                       "'shares_accepted_actual_difficulty_rank',"
                       "'events_share_result_share',"
                       "'events_share_result_round_share')") == 4,
                "schema-v2 read indexes are incomplete");
        const DatabasePragmas pragmas = database.pragmas();
        require(pragmas.journal_mode == "wal", "WAL mode not active");
        require(pragmas.synchronous == "full", "FULL sync not active");
        require(pragmas.foreign_keys, "foreign keys not active");
        require(pragmas.busy_timeout_ms == 2345, "busy timeout mismatch");

        fixture = create_fixture(database, now);
        const std::int64_t original_round = database.current_open_round_id();

        const auto reject_event_payload = [&](std::string payload,
                                              std::string_view message) {
            bool rejected = false;
            try {
                (void)database.insert_event(EventInsert{
                    .session_id = fixture.session_id,
                    .created_unix_us = now + 1,
                    .type = "invalid_payload_fixture",
                    .payload_json = std::move(payload),
                });
            }
            catch (const DatabaseError &) {
                rejected = true;
            }
            require(rejected, message);
        };
        require(database.insert_event(EventInsert{
                    .session_id = fixture.session_id,
                    .created_unix_us = now + 1,
                    .type = "valid_payload_fixture",
                    .payload_json =
                        "{\"payload_schema_version\":1,\"data\":{\"ok\":true}}",
                }) > 0,
                "valid exact event payload wrapper was rejected");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"payload_schema_version\":1,"
            "\"data\":{}}",
            "duplicate event payload key was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":{},\"extra\":0}",
            "extra event payload root key was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1.0,\"data\":{}}",
            "non-integer event payload schema version was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":[]}",
            "non-object event payload data was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":{\"\":0}}",
            "empty event payload object key was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":{\"" +
                std::string(129, 'k') + "\":0}}",
            "oversized event payload object key was accepted");
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":{\"bad\":\"" +
                std::string(1, static_cast<char>(0xff)) + "\"}}",
            "invalid UTF-8 event payload was accepted");
        std::string deep_data = "{}";
        for (int depth = 0; depth < 9; ++depth) {
            deep_data = "{\"level\":" + deep_data + "}";
        }
        reject_event_payload(
            "{\"payload_schema_version\":1,\"data\":" + deep_data + "}",
            "over-depth event payload was accepted");

        const std::int64_t ordinary_share =
            create_share(database, fixture, 1, 1, false);
        database.set_share_height_is_older(ordinary_share, true);
        require(scalar(path,
                       "SELECT height_is_older FROM shares WHERE id=1") == 1,
                "live stale-height correction was not persisted");
        database.set_share_height_is_older(ordinary_share, false);
        const ShareAcceptanceResult acceptance = database.accept_share(
            ShareAcceptance{
                .share_id = ordinary_share,
                .completed_unix_us = now + 10'000'000,
                .assigned_difficulty_dec = "120",
                .source = HashrateSource::verified,
                .actual_difficulty_dec = "240",
                .verifier_ticket_dec = "18446744073709551615",
                .verifier_seed_id_dec = "18446744073709551615",
                .verifier_queue_ns = 10,
                .verifier_hash_ns = 20,
                .verifier_total_ns = 30,
            });
        require(acceptance.accepted, "ordinary share was not accepted");
        require(acceptance.round_id == original_round,
                "ordinary work was credited to the wrong round");
        require(!database.accept_share(ordinary_share, now + 11'000'000, "120",
                                       HashrateSource::verified)
                     .accepted,
                "ordinary share accounting was not idempotent");
        require(scalar(path,
                       "SELECT verifier_total_ns FROM shares WHERE id=1") == 30,
                "accepted share verifier timings were not persisted");
        const HashrateWindows rates = database.hashrate(
            "global", 0, HashrateSource::verified,
            (now + 10'000'000) / 1'000'000);
        require(rates.one_minute == "2", "one-minute hashrate is wrong");
        require(rates.five_minutes == "0",
                "nominal five-minute denominator was not used");

        const std::int64_t claimed_share =
            create_share(database, fixture, 40, 40, false);
        require(database.accept_share(claimed_share, now + 10'000'001, "120",
                                      HashrateSource::claimed)
                    .accepted,
                "claimed-mode share was not accepted");
        Fixture low_network_fixture = fixture;
        low_network_fixture.job_id = create_job(
            database, fixture, 10, 42, "120", "100", now + 3);
        const std::int64_t over_effort_share =
            create_share(database, low_network_fixture, 41, 41, false,
                         "120", "100");
        require(database.accept_share(over_effort_share, now + 10'000'002,
                                      "120", HashrateSource::verified)
                    .accepted,
                "mixed-network-difficulty share was not accepted");
        require(scalar(path,
                       "SELECT count(*) FROM round_work_segments WHERE round_id=1") == 3,
                "round work was not split by source and network difficulty");
        require(scalar_text(
                    path,
                    "SELECT credited_difficulty_dec FROM round_work_segments "
                    "WHERE round_id=1 AND source='verified' "
                    "AND network_difficulty_dec='100'") == "120",
                "greater-than-100-percent effort segment was not exact");

        DuplicateKey duplicate = bytes<48>(11);
        const auto reservation = database.reserve_duplicate(
            duplicate, 42, ordinary_share, DuplicateRole::claimed,
            now + 1, 91);
        require(reservation.reserved, "first duplicate key reservation failed");
        require(!database.reserve_duplicate(duplicate, 42, ordinary_share,
                                            DuplicateRole::claimed, now + 2, 92)
                     .reserved,
                "duplicate key was reserved twice");
        require(!database.reserve_duplicate(duplicate, 42, ordinary_share,
                                            DuplicateRole::computed, now + 2, 93)
                     .reserved,
                "computed role made an existing duplicate nonduplicate");
        const auto active_duplicates = database.load_active_duplicates();
        require(active_duplicates.size() == 1 &&
                    active_duplicates.front().role == DuplicateRole::both,
                "claimed/computed duplicate roles were not upgraded to both");
        require(!database.retire_duplicate(duplicate, 92, now + 3),
                "stale generation token retired a duplicate key");
        require(database.retire_duplicate(duplicate, 91, now + 3),
                "matching generation token did not retire a duplicate key");
        const auto retried_reservation = database.reserve_duplicate(
            duplicate, 42, ordinary_share, DuplicateRole::claimed,
            now + 4, 94);
        require(retried_reservation.reserved &&
                    retried_reservation.generation_token == 94,
                "retired provisional duplicate key could not be retried");

        const std::int64_t block_share =
            create_share(database, fixture, 2, 2, true);
        CandidateJournal block_candidate =
            make_candidate(fixture, block_share, 31, 4, now + 20);
        auto journal = database.journal_candidate(block_candidate);
        require(journal.inserted, "candidate was not journaled");
        accepted_candidate_id = journal.candidate_id;
        require(database.find_candidate_by_key(block_candidate.candidate_key)
                        .value()
                        .candidate_id == accepted_candidate_id,
                "durable candidate-key lookup failed");
        require(!database.journal_candidate(block_candidate).inserted,
                "candidate fingerprint was inserted twice");

        const std::int64_t attached_share =
            create_share(database, fixture, 4, 4, true);
        database.attach_share_to_candidate(attached_share, accepted_candidate_id);

        const std::int64_t accepted_attempt_id = database.start_candidate_attempt(
            accepted_candidate_id, 1, 500, now + 30);
        require(accepted_attempt_id > 0, "candidate attempt was not persisted");
        CandidateAttemptCompletion accepted;
        accepted.classification = CandidateAttemptClassification::accepted;
        accepted.completed_unix_us = now + 40;
        accepted.http_status = 200;
        accepted.daemon_status = "OK";
        accepted.daemon_block_id = bytes<32>(240);
        const auto result = database.finish_candidate_attempt(
            accepted_candidate_id, 1, accepted);
        require(result.state == CandidateState::accepted && result.terminal,
                "daemon OK did not accept candidate");
        require(database.current_open_round_id() != original_round,
                "candidate acceptance did not advance the round");
        require(scalar(path,
                       "SELECT count(*) FROM rounds WHERE id=1 AND state='closed' "
                       "AND effort_finalized_unix_us IS NULL") == 1,
                "round effort finalized before its pending shares drained");
        require(database.accept_share(block_share, now + 41, "120",
                                      HashrateSource::verified)
                    .round_id == original_round,
                "late candidate share leaked into the successor round");
        require(database.finalize_share(attached_share, ShareFinalization{
                    .status = "invalid_result",
                    .provenance = "verified",
                    .completed_unix_us = now + 42,
                    .error_code = "test_terminal",
                    .error_message = "test pending-share drain",
                }),
                "pending attached share did not finalize");
        require(scalar(path,
                       "SELECT count(*) FROM rounds WHERE id=1 "
                       "AND effort_finalized_unix_us IS NOT NULL "
                       "AND finalized_effort_segment_count=3") == 1,
                "closed round effort did not freeze after pending shares drained");
        require(scalar_text(
                    path,
                    "SELECT credited_difficulty_dec FROM rounds WHERE id=1") == "480",
                "late accepted work was not retained in the origin round");
        require(statement_rejected(
                    path,
                    "UPDATE round_work_segments SET credited_difficulty_dec='481' "
                    "WHERE round_id=1 AND source='verified' "
                    "AND network_difficulty_dec='1000000'"),
                "finalized round work segment was mutable");
        require(statement_rejected(
                    path,
                    "UPDATE shares SET round_id=2 WHERE id=1"),
                "persisted share round attribution was mutable");
        require(!database.accept_candidate(accepted_candidate_id, now + 50,
                                           bytes<32>(240), false),
                "candidate acceptance was not idempotent");
        require(database.pending_blocknotify_count() == 1,
                "candidate did not create one blocknotify delivery");

        const auto delivery = database.claim_next_blocknotify(now + 60);
        require(delivery.has_value() && delivery->attempt_count == 1,
                "blocknotify delivery was not claimed");
        const std::int64_t recovery_share =
            create_share(database, fixture, 6, 6, true);
        const auto recovery_candidate = database.journal_candidate(
            make_candidate(fixture, recovery_share, 91, 2, now + 61));
        recovered_candidate_id = recovery_candidate.candidate_id;
        require(database.start_candidate_attempt(recovered_candidate_id, 1, 900,
                                                 now + 62) > 0,
                "recovery fixture attempt was not started");
        // Simulate a crash while the child is running. The next Database open
        // must return the hook row to pending and classify the daemon attempt
        // indeterminate without reusing its attempt number.
    }

    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 2345,
            .blocknotify_enabled = true,
        });
        require(database.current_open_round_id() > 0,
                "open round did not survive restart");
        const auto recovered_candidates = database.recoverable_candidates();
        const auto recovered = std::find_if(
            recovered_candidates.begin(), recovered_candidates.end(),
            [recovered_candidate_id](const CandidateRecovery &value) {
                return value.candidate_id == recovered_candidate_id;
            });
        require(recovered != recovered_candidates.end() &&
                    recovered->state == CandidateState::retry_wait &&
                    recovered->attempt_count == 1 && recovered->max_attempts == 2 &&
                    recovered->had_indeterminate && recovered->height == 42 &&
                    !recovered->frozen_block_blob.empty() &&
                    recovered->connection_id == fixture.connection_id,
                "in-flight candidate did not recover with immutable context");
        require(database.start_candidate_attempt(recovered_candidate_id, 2, 901,
                                                 now + 69) > 0,
                "recovered candidate did not advance to the next attempt");
        const auto delivery = database.claim_next_blocknotify(now + 70);
        require(delivery.has_value() && delivery->candidate_id == accepted_candidate_id &&
                    delivery->attempt_count == 2,
                "running blocknotify was not recovered at least once");
        BlocknotifyCompletion failed;
        failed.delivered = false;
        failed.completed_unix_us = now + 80;
        failed.exit_code = 7;
        failed.stderr_excerpt = "fixture failure";
        failed.last_error = "nonzero exit";
        database.finish_blocknotify(delivery->id, failed);
        require(!database.claim_next_blocknotify(now + 5'000'079).has_value(),
                "blocknotify retry ran before its five-second delay");
        const auto retry = database.claim_next_blocknotify(now + 5'000'080);
        require(retry.has_value() && retry->attempt_count == 3,
                "blocknotify retry did not run at its exact due time");
        BlocknotifyCompletion completed;
        completed.delivered = true;
        completed.completed_unix_us = now + 5'000'081;
        completed.exit_code = 0;
        database.finish_blocknotify(retry->id, completed);
        require(database.pending_blocknotify_count() == 0,
                "successful blocknotify remains pending");

        const std::int64_t rejected_share =
            create_share(database, fixture, 3, 3, true);
        CandidateJournal rejected_candidate =
            make_candidate(fixture, rejected_share, 51, 1, now + 90);
        const auto pending_verdict = database.record_candidate_verdict(
            CandidateVerdictInsert{
                .share_id = rejected_share,
                .kind = CandidateVerdictKind::false_candidate,
                .candidate_key = rejected_candidate.candidate_key,
                .candidate_id = std::nullopt,
                .created_unix_us = now + 91,
            });
        require(pending_verdict.disposition ==
                    CandidateVerdictDisposition::pending,
                "unjournaled verdict was not pending");
        const auto rejected_journal = database.journal_candidate(rejected_candidate);
        const std::int64_t repeated_false_share_one =
            create_share(database, fixture, 30, 30, true);
        const std::int64_t repeated_false_share_two =
            create_share(database, fixture, 31, 31, true);
        database.attach_share_to_candidate(repeated_false_share_one,
                                           rejected_journal.candidate_id);
        database.attach_share_to_candidate(repeated_false_share_two,
                                           rejected_journal.candidate_id);
        for (const std::int64_t repeated_share :
             {repeated_false_share_one, repeated_false_share_two}) {
            require(database.record_candidate_verdict(CandidateVerdictInsert{
                                .share_id = repeated_share,
                                .kind = CandidateVerdictKind::false_candidate,
                                .candidate_key = rejected_candidate.candidate_key,
                                .candidate_id = rejected_journal.candidate_id,
                                .created_unix_us = now + 91,
                            }).disposition ==
                        CandidateVerdictDisposition::pending,
                    "pre-rejection repeated verdict was not pending");
        }
        const std::int64_t rejected_attempt_id = database.start_candidate_attempt(
            rejected_journal.candidate_id, 1, 501, now + 92);
        require(rejected_attempt_id > 0,
                "rejected candidate attempt was not persisted");
        CandidateAttemptCompletion rejected;
        rejected.classification =
            CandidateAttemptClassification::explicit_rejection;
        rejected.completed_unix_us = now + 93;
        rejected.http_status = 200;
        rejected.rpc_error_code = 0;
        rejected.daemon_status = "BUSY";
        const auto rejected_result = database.finish_candidate_attempt(
            rejected_journal.candidate_id, 1, rejected);
        require(rejected_result.state == CandidateState::rejected &&
                    rejected_result.terminal && !rejected_result.had_indeterminate,
                "wholly explicit attempt sequence did not reject");
        require(rejected_result.newly_actionable_false_candidates == 1 &&
                    rejected_result.newly_actionable_candidate_mismatches == 0 &&
                    !rejected_result.trusted_candidate_rejection_recorded,
                "newly actionable verdict counts were not returned");
        const std::string rejected_prefix =
            "SELECT count(*) FROM candidate_verdicts WHERE candidate_id=" +
            std::to_string(rejected_journal.candidate_id) +
            " AND kind='false_candidate' AND disposition=";
        require(scalar(path, (rejected_prefix + "'actionable'").c_str()) == 1 &&
                    scalar(path, (rejected_prefix + "'suppressed'").c_str()) == 2 &&
                    scalar(path, (rejected_prefix + "'pending'").c_str()) == 0,
                "terminal rejection did not resolve every repeated verdict");

        const std::int64_t ambiguous_share =
            create_share(database, fixture, 5, 5, true);
        CandidateJournal ambiguous_candidate =
            make_candidate(fixture, ambiguous_share, 71, 1, now + 94);
        const auto ambiguous_journal = database.journal_candidate(ambiguous_candidate);
        const std::int64_t ambiguous_attempt_id = database.start_candidate_attempt(
            ambiguous_journal.candidate_id, 1, 502, now + 95);
        require(ambiguous_attempt_id > 0, "ambiguous attempt was not persisted");
        CandidateAttemptCompletion indeterminate;
        indeterminate.classification = CandidateAttemptClassification::indeterminate;
        indeterminate.completed_unix_us = now + 96;
        const auto ambiguous_result = database.finish_candidate_attempt(
            ambiguous_journal.candidate_id, 1, indeterminate);
        require(ambiguous_result.state == CandidateState::ambiguous,
                "indeterminate exhausted sequence was not ambiguous");
        database.schedule_candidate_reconciliation(ambiguous_journal.candidate_id,
                                                   now + 5'000'096);

        CandidateReconciliationStart height_lookup;
        height_lookup.candidate_id = ambiguous_journal.candidate_id;
        height_lookup.cycle_number = 1;
        height_lookup.lookup_kind = ReconciliationLookupKind::height;
        height_lookup.rpc_request_id = 700;
        height_lookup.started_unix_us = now + 5'000'096;
        const auto reconciliation =
            database.start_candidate_reconciliation(height_lookup);
        require(reconciliation.inserted && reconciliation.reconciliation_id > 0,
                "reconciliation lookup was not journaled");
        require(!database.start_candidate_reconciliation(height_lookup).inserted,
                "same reconciliation lookup was inserted twice");

        CandidateReconciliationCompletion inconclusive;
        inconclusive.classification = ReconciliationClassification::inconclusive;
        inconclusive.completed_unix_us = now + 5'000'097;
        inconclusive.observed_height = 42;
        inconclusive.observed_orphan = true;
        const auto inconclusive_result = database.finish_candidate_reconciliation(
            reconciliation.reconciliation_id, inconclusive);
        require(!inconclusive_result.candidate_accepted &&
                    inconclusive_result.candidate_state == CandidateState::ambiguous,
                "inconclusive reconciliation accepted a candidate");
        require(database.finish_candidate_reconciliation(
                            reconciliation.reconciliation_id, inconclusive)
                        .already_completed,
                "reconciliation completion was not idempotent");

        CandidateReconciliationStart positive_lookup;
        positive_lookup.candidate_id = ambiguous_journal.candidate_id;
        positive_lookup.cycle_number = 2;
        positive_lookup.lookup_kind = ReconciliationLookupKind::expected_hash;
        positive_lookup.rpc_request_id = 701;
        positive_lookup.requested_block_id = ambiguous_candidate.expected_block_id;
        positive_lookup.started_unix_us = now + 5'000'098;
        const auto positive_row =
            database.start_candidate_reconciliation(positive_lookup);
        CandidateReconciliationCompletion positive;
        positive.classification = ReconciliationClassification::positive;
        positive.completed_unix_us = now + 5'000'099;
        positive.observed_block_id = ambiguous_candidate.expected_block_id;
        positive.observed_height = ambiguous_candidate.height;
        positive.observed_miner_tx_hash = ambiguous_candidate.miner_tx_hash;
        positive.observed_orphan = false;
        const auto positive_result = database.finish_candidate_reconciliation(
            positive_row.reconciliation_id, positive);
        require(positive_result.candidate_accepted &&
                    positive_result.candidate_state ==
                        CandidateState::accepted_by_reconciliation,
                "positive reconciliation did not accept candidate");

        const std::int64_t abuse_event = database.insert_abuse_event(
            AbuseEventInsert{
                .connection_id = fixture.connection_id,
                .share_id = rejected_share,
                .candidate_id = std::nullopt,
                .peer_family = 2,
                .peer_address = {127, 0, 0, 1},
                .kind = "malformed",
                .weight = 1,
                .created_unix_us = now + 100,
                .detail = std::nullopt,
            });
        BanInsert ban;
        ban.peer_family = 2;
        ban.peer_address = {127, 0, 0, 1};
        ban.created_unix_us = now + 101;
        ban.expires_unix_us = now + 7'200'000'000;
        ban.evidence_window_started_unix_us = now;
        ban.evidence_window_ended_unix_us = now + 101;
        ban.reason = "test threshold";
        ban.abuse_event_ids = {abuse_event};
        require(database.create_ban(ban) > 0, "ban was not persisted");
        require(database.load_active_bans(now + 102).size() == 1,
                "active ban did not load");
        require(database.expire_bans(ban.expires_unix_us, fixture.session_id) == 1,
                "ban did not expire at its exact expiry");
        require(database.load_active_bans(ban.expires_unix_us).empty(),
                "expired ban remained active");

        require(database.event_high_water_mark() > 0,
                "persistent event stream has no events");
        const auto committed_events = database.load_events_after(0, 1000);
        require(!committed_events.empty() &&
                    committed_events.front().id > 0 &&
                    committed_events.front().session_public_id == bytes<16>(1),
                "committed-event replay did not return typed ordered events");
        require(scalar(path, "SELECT count(*) FROM rounds WHERE state='open'") == 1,
                "database does not contain exactly one open round");
        require(scalar(path, "SELECT count(*) FROM blocknotify_deliveries") == 2,
                "each of two accepted candidates did not create exactly one delivery");
        require(scalar(path,
                       "SELECT count(*) FROM candidate_verdicts "
                       "WHERE disposition='actionable'") == 1,
                "rejected candidate verdict did not become actionable");

        // Verdicts produced after a candidate was deferred may still have no
        // candidate row when their private job leaves the active history.
        // Retiring that job is the durable point at which they become abuse
        // evidence, and Runtime needs the persisted peer snapshot immediately.
        const std::int64_t deferred_false_share =
            create_share(database, fixture, 20, 20, true);
        const std::int64_t deferred_mismatch_share =
            create_share(database, fixture, 21, 21, false);
        require(database.record_candidate_verdict(CandidateVerdictInsert{
                            .share_id = deferred_false_share,
                            .kind = CandidateVerdictKind::false_candidate,
                            .candidate_key = bytes<32>(180),
                            .candidate_id = std::nullopt,
                            .created_unix_us = now + 7'000'000'000,
                        }).disposition == CandidateVerdictDisposition::pending,
                "deferred false-candidate verdict was not pending");
        require(database.record_candidate_verdict(CandidateVerdictInsert{
                            .share_id = deferred_mismatch_share,
                            .kind = CandidateVerdictKind::candidate_mismatch,
                            .candidate_key = bytes<32>(181),
                            .candidate_id = std::nullopt,
                            .created_unix_us = now + 7'000'000'001,
                        }).disposition == CandidateVerdictDisposition::pending,
                "deferred candidate-mismatch verdict was not pending");
        const JobRetirementResult retirement =
            database.retire_job(fixture.job_id, now + 7'100'000'000);
        require(retirement.retired &&
                    retirement.newly_actionable_false_candidates == 1 &&
                    retirement.newly_actionable_candidate_mismatches == 1 &&
                    retirement.actionable_verdicts.size() == 2,
                "job retirement did not return its newly actionable verdicts");
        for (const ActionableCandidateVerdict &verdict :
             retirement.actionable_verdicts) {
            require(verdict.abuse_event_id > 0 && verdict.share_id > 0 &&
                        verdict.connection_id == fixture.connection_id &&
                        verdict.peer_family == 2 &&
                        verdict.peer_address == ByteVector({127, 0, 0, 1}),
                    "job retirement did not return immutable peer evidence");
        }
        const JobRetirementResult retired_again =
            database.retire_job(fixture.job_id, now + 7'100'000'001);
        require(!retired_again.retired &&
                    retired_again.newly_actionable_false_candidates == 0 &&
                    retired_again.newly_actionable_candidate_mismatches == 0 &&
                    retired_again.actionable_verdicts.empty(),
                "repeated job retirement replayed abuse scoring");
        require(scalar(path,
                       "SELECT count(*) FROM candidate_verdicts "
                       "WHERE disposition='actionable'") == 3,
                "job-retirement verdicts were not durably actionable");
        require(database.close_connection(fixture.connection_id,
                                          now + 8'000'000'000,
                                          "test complete"),
                "connection did not close");
        require(!database.close_connection(fixture.connection_id,
                                           now + 8'000'000'001,
                                           "already closed"),
                "connection close was not idempotent");
    }

    remove_database(path);
}

void run_crash_recovery_test()
{
    const auto path = std::filesystem::path(test_database_path().string() +
                                            "-recovery");
    remove_database(path);
    constexpr std::int64_t now = 1'700'100'000'000'000;
    const DuplicateKey candidate_duplicate = bytes<48>(211);
    const DuplicateKey ordinary_duplicate = bytes<48>(220);
    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 5000,
            .blocknotify_enabled = false,
        });
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t share = create_share(database, fixture, 1, 1, true);
        require(database.reserve_duplicate(
                    candidate_duplicate, 42, share, DuplicateRole::claimed,
                    now + 3, 717)
                    .reserved,
                "crash fixture duplicate reservation failed");
        require(database.journal_candidate(
                    make_candidate(fixture, share, 212, 4, now + 4)).inserted,
                "crash fixture candidate was not journaled");
        const std::int64_t verdict_share =
            create_share(database, fixture, 2, 2, true);
        require(database.record_candidate_verdict(CandidateVerdictInsert{
                            .share_id = verdict_share,
                            .kind = CandidateVerdictKind::false_candidate,
                            .candidate_key = bytes<32>(213),
                            .candidate_id = std::nullopt,
                            .created_unix_us = now + 5,
                        }).disposition == CandidateVerdictDisposition::pending,
                "crash fixture verdict was not pending");
        const std::int64_t interrupted_share =
            database.insert_share(ShareInsert{
                .connection_id = fixture.connection_id,
                .worker_id = fixture.worker_id,
                .job_id = fixture.job_id,
                .request_sequence = 4,
                .miner_request_id_type = "integer",
                .miner_request_id_text = "4",
                .received_unix_us = now + 5,
                .nonce = std::array<std::uint8_t, 4>{0, 0, 0, 4},
                .assigned_difficulty_dec = "120",
                .network_difficulty_dec = "1000000",
                .status = "received",
                .provenance = "pending",
            });
        database.mark_share_verifying(interrupted_share, "123", "456");
        const std::int64_t ordinary_share =
            create_share(database, fixture, 3, 3, false);
        require(database.reserve_duplicate(
                    ordinary_duplicate, 43, ordinary_share,
                    DuplicateRole::claimed, now + 6, 718).reserved,
                "crash fixture ordinary duplicate reservation failed");
        // Intentionally omit close/retire/finish: destruction models a process
        // that vanished after durable work was committed.
    }

    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 5000,
            .blocknotify_enabled = false,
        });
        const InterruptedRuntimeRecovery recovered =
            database.recover_interrupted_runtime(now + 1'000'000);
        require(recovered.sessions_stopped == 1 &&
                    recovered.connections_closed == 1 &&
                    recovered.jobs_retired == 1,
                "crash recovery did not close every process-local row");
        require(recovered.actionable_verdicts.size() == 1 &&
                    recovered.actionable_verdicts.front().share_id == 2,
                "crash recovery lost deferred candidate evidence");
        require(scalar(path,
                       "SELECT count(*) FROM server_sessions WHERE "
                       "stopped_unix_us IS NULL") == 0,
                "crashed server session remained active");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE "
                       "closed_unix_us IS NULL") == 0,
                "crashed connection remained active");
        require(scalar(path,
                       "SELECT count(*) FROM private_jobs WHERE "
                       "retired_unix_us IS NULL") == 0,
                "crashed private job remained active");
        require(scalar(path,
                       "SELECT count(*) FROM shares WHERE "
                       "status IN ('received','verifying')") == 0 &&
                    scalar(path,
                       "SELECT count(*) FROM shares WHERE "
                       "status='cancelled' AND error_code='process_restarted'") >= 1,
                "crash recovery left a share permanently nonterminal");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE "
                       "close_reason='process_restarted'") == 1,
                "crash recovery did not persist its close reason");
        const auto active = database.load_active_duplicates();
        require(active.size() == 2,
                "database recovery prematurely removed duplicate protection");

        // Reproduce Runtime's startup ordering: restore every durable key,
        // acquire candidate references, then logically retire all prior-job
        // buckets. The orphan ordinary height collects immediately while the
        // recoverable candidate keeps its bucket replay-protected.
        DuplicateRegistry registry(2, 2);
        for (const ActiveDuplicate &entry : active) {
            registry.restore(entry.key, entry.source_id, entry.height,
                             static_cast<std::uint64_t>(entry.generation_token));
        }
        const auto candidates = database.recoverable_candidates();
        require(candidates.size() == 1 && candidates.front().height == 42,
                "crash fixture candidate was not recoverable");
        const auto candidate_source = static_cast<std::uint64_t>(
            candidates.front().connection_id);
        registry.retain_height(candidate_source, candidates.front().height);
        require(registry.retire_height(candidate_source, 42).empty(),
                "candidate duplicate bucket was retired during recovery");
        const auto orphan = std::find_if(
            active.begin(), active.end(),
            [](const ActiveDuplicate &entry) { return entry.height == 43; });
        require(orphan != active.end(),
                "orphan duplicate source was not restored");
        const auto orphan_tokens = registry.retire_height(
            orphan->source_id, 43);
        require(orphan_tokens.size() == 1 &&
                    orphan_tokens.front().key == ordinary_duplicate,
                "orphan ordinary duplicate did not collect at startup");
        require(database.retire_duplicate(
                    orphan_tokens.front().key,
                    static_cast<std::int64_t>(orphan_tokens.front().generation),
                    now + 1'000'001),
                "orphan ordinary durable duplicate did not retire");
        require(database.load_active_duplicates().size() == 1,
                "candidate duplicate was not the sole retained startup key");

        DuplicateToken capacity_probe;
        require(registry.reserve(bytes<48>(230), 0, 44, capacity_probe) ==
                    DuplicateReserveResult::reserved,
                "orphan cleanup did not return duplicate-registry capacity");
        require(registry.release(capacity_probe),
                "capacity probe could not be released");
        const auto candidate_tokens = registry.release_height(
            candidate_source, 42);
        require(candidate_tokens.size() == 1 &&
                    candidate_tokens.front().key == candidate_duplicate,
                "terminal candidate did not release its duplicate bucket");
        require(database.retire_duplicate(
                    candidate_tokens.front().key,
                    static_cast<std::int64_t>(candidate_tokens.front().generation),
                    now + 1'000'002),
                "terminal candidate durable duplicate did not retire");
        require(database.load_active_duplicates().empty(),
                "retired startup buckets leaked active durable duplicates");

        const InterruptedRuntimeRecovery repeated =
            database.recover_interrupted_runtime(now + 2'000'000);
        require(repeated.sessions_stopped == 0 &&
                    repeated.connections_closed == 0 &&
                    repeated.jobs_retired == 0 &&
                    repeated.actionable_verdicts.empty(),
                "crash recovery was not idempotent");

        SessionStart replacement;
        replacement.public_id = bytes<16>(9);
        replacement.started_unix_us = now + 2'000'001;
        replacement.version = "database-recovery-test";
        const std::int64_t replacement_session =
            database.start_session(replacement);
        require(database.start_candidate_attempt(
                    candidates.front().candidate_id, 1, 990,
                    now + 2'000'002) > 0,
                "recovered candidate could not start in replacement session");
        require(scalar(path,
                       "SELECT session_id FROM events "
                       "WHERE type='candidate_attempt' ORDER BY id DESC LIMIT 1") ==
                    replacement_session,
                "recovered candidate event was attributed to its stopped session");
        require(scalar(path,
                       "SELECT count(*) FROM events e JOIN candidates c "
                       "ON c.id=e.candidate_id WHERE e.type='candidate_attempt' "
                       "AND e.connection_id=c.connection_id AND e.job_id=c.job_id "
                       "AND e.share_id=c.first_share_id") == 1,
                "recovered candidate event lost original correlation fields");
    }
    remove_database(path);
}

void run_symlink_rejection_test()
{
    const auto target = std::filesystem::path(
        test_database_path().string() + "-symlink-target");
    const auto link = std::filesystem::path(
        test_database_path().string() + "-symlink-link");
    remove_database(target);
    std::error_code ignored;
    std::filesystem::remove(link, ignored);
    {
        Database initialize(DatabaseOptions{
            .path = target.string(),
            .busy_timeout_ms = 5000,
            .blocknotify_enabled = false,
        });
    }
    require(::symlink(target.c_str(), link.c_str()) == 0,
            "could not create database symlink fixture");
    bool rejected = false;
    try {
        Database database(DatabaseOptions{
            .path = link.string(),
            .busy_timeout_ms = 5000,
            .blocknotify_enabled = false,
        });
    }
    catch (const DatabaseError &) {
        rejected = true;
    }
    std::filesystem::remove(link, ignored);
    std::filesystem::remove(link.string() + ".lock", ignored);
    remove_database(target);
    require(rejected, "SQLite database open followed a symlink");
}

void run_sibling_lock_test()
{
    const auto path = std::filesystem::path(test_database_path().string() +
                                            "-single-owner");
    remove_database(path);
    {
        Database owner(DatabaseOptions{.path = path.string()});
        bool rejected = false;
        try {
            Database competitor(DatabaseOptions{.path = path.string()});
        }
        catch (const DatabaseError &error) {
            rejected = std::string_view(error.what()).find("already owned") !=
                       std::string_view::npos;
        }
        require(rejected,
                "a second live server instance acquired the same database");

        const auto alias = std::filesystem::path(path.string() + "-alias");
        std::error_code link_error;
        std::filesystem::remove(alias, link_error);
        link_error.clear();
        std::filesystem::create_hard_link(path, alias, link_error);
        require(!link_error, "could not create database hard-link alias");
        bool alias_rejected = false;
        try {
            Database competitor(DatabaseOptions{.path = alias.string()});
        }
        catch (const DatabaseError &error) {
            alias_rejected = std::string_view(error.what()).find("already owned") !=
                             std::string_view::npos;
        }
        require(alias_rejected,
                "a hard-link alias bypassed live database ownership");
        std::filesystem::remove(alias, link_error);
    }
    {
        Database successor(DatabaseOptions{.path = path.string()});
        require(successor.schema_version() == 2,
                "database lock was not released with its owning instance");
    }
    remove_database(path);
}

void run_reconciliation_attempt_race_test()
{
    const auto path = std::filesystem::path(test_database_path().string() +
                                            "-attempt-race");
    remove_database(path);
    constexpr std::int64_t now = 1'700'200'000'000'000;
    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 5000,
            .blocknotify_enabled = true,
        });
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t share = create_share(database, fixture, 1, 1, true);
        const auto candidate = database.journal_candidate(
            make_candidate(fixture, share, 241, 2, now + 1));
        require(candidate.inserted, "race candidate was not journaled");
        require(database.start_candidate_attempt(candidate.candidate_id, 1, 700,
                                                 now + 2) > 0,
                "race attempt was not dispatched");
        require(database.accept_candidate(candidate.candidate_id, now + 3,
                                          bytes<32>(242), true),
                "positive reconciliation did not win the race");

        CandidateAttemptCompletion late_rejection;
        late_rejection.classification =
            CandidateAttemptClassification::explicit_rejection;
        late_rejection.completed_unix_us = now + 4;
        late_rejection.http_status = 200;
        late_rejection.rpc_error_code = -7;
        late_rejection.response_excerpt = "late explicit rejection";
        const CandidateAttemptResult finished = database.finish_candidate_attempt(
            candidate.candidate_id, 1, late_rejection);
        require(finished.terminal &&
                    finished.state == CandidateState::accepted_by_reconciliation,
                "late attempt result reversed reconciliation acceptance");
        require(database.pending_blocknotify_count() == 1,
                "reconciliation race created the wrong hook count");
        require(scalar(path,
                       "SELECT count(*) FROM candidate_attempts "
                       "WHERE classification='explicit_rejection'") == 1,
                "late attempt evidence was not closed durably");
    }
    remove_database(path);
}

void run_candidate_round_boundary_test()
{
    constexpr std::int64_t now = 1'700'250'000'000'000;
    const auto contamination_path = std::filesystem::path(
        test_database_path().string() + "-height-contamination");
    remove_database(contamination_path);
    {
        Database database(DatabaseOptions{.path = contamination_path.string()});
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t candidate_share =
            create_share(database, fixture, 1, 1, true);
        const auto candidate = database.journal_candidate(
            make_candidate(fixture, candidate_share, 22, 1, now + 1));
        require(database.start_candidate_attempt(candidate.candidate_id, 1, 800,
                                                 now + 2) > 0,
                "contamination candidate attempt did not start");

        Fixture higher = fixture;
        higher.job_id = create_job(
            database, fixture, 12, 43, "120", "1000000", now + 3);
        (void)create_share(database, higher, 2, 2, false);

        CandidateAttemptCompletion accepted;
        accepted.classification = CandidateAttemptClassification::accepted;
        accepted.completed_unix_us = now + 4;
        accepted.http_status = 200;
        accepted.daemon_status = "OK";
        bool rejected = false;
        try {
            (void)database.finish_candidate_attempt(
                candidate.candidate_id, 1, accepted);
        }
        catch (const DatabaseError &error) {
            rejected = std::string_view(error.what()).find("higher-height") !=
                       std::string_view::npos;
        }
        require(rejected,
                "candidate acceptance ignored higher-height round contamination");
        require(scalar(contamination_path,
                       "SELECT count(*) FROM rounds WHERE state='open'") == 1 &&
                    scalar(contamination_path,
                       "SELECT count(*) FROM rounds WHERE state='closed'") == 0 &&
                    scalar(contamination_path,
                       "SELECT count(*) FROM candidate_attempts "
                       "WHERE classification='dispatching'") == 1,
                "failed-closed contamination check did not roll back atomically");
    }
    remove_database(contamination_path);

    const auto origin_path = std::filesystem::path(
        test_database_path().string() + "-origin-round");
    remove_database(origin_path);
    {
        Database database(DatabaseOptions{.path = origin_path.string()});
        const Fixture fixture = create_fixture(database, now);
        const auto winner = database.journal_candidate(make_candidate(
            fixture, create_share(database, fixture, 1, 1, true),
            32, 1, now + 1));
        const auto late = database.journal_candidate(make_candidate(
            fixture, create_share(database, fixture, 2, 2, true),
            52, 1, now + 1));
        require(database.start_candidate_attempt(winner.candidate_id, 1, 810,
                                                 now + 2) > 0 &&
                    database.start_candidate_attempt(late.candidate_id, 1, 811,
                                                     now + 2) > 0,
                "parallel origin-round attempts did not start");

        CandidateAttemptCompletion accepted;
        accepted.classification = CandidateAttemptClassification::accepted;
        accepted.completed_unix_us = now + 3;
        accepted.http_status = 200;
        accepted.daemon_status = "OK";
        require(database.finish_candidate_attempt(winner.candidate_id, 1, accepted)
                    .state == CandidateState::accepted,
                "origin-round winner was not accepted");
        require(database.latest_accepted_height() == 42U,
                "latest durable accepted height was not recoverable");
        bool rejected = false;
        accepted.completed_unix_us = now + 4;
        try {
            (void)database.finish_candidate_attempt(late.candidate_id, 1,
                                                    accepted);
        }
        catch (const DatabaseError &error) {
            rejected = std::string_view(error.what()).find("origin round") !=
                       std::string_view::npos;
        }
        require(rejected,
                "late candidate from a closed round closed its successor");
        require(scalar(origin_path,
                       "SELECT count(*) FROM rounds WHERE state='closed'") == 1 &&
                    scalar(origin_path,
                       "SELECT count(*) FROM rounds WHERE state='open'") == 1 &&
                    scalar(origin_path,
                       "SELECT count(*) FROM candidate_attempts "
                       "WHERE candidate_id=2 AND classification='dispatching'") == 1,
                "late origin-round acceptance was not rolled back atomically");
    }
    remove_database(origin_path);

    const auto recovery_path = std::filesystem::path(
        test_database_path().string() + "-round-finalization-recovery");
    remove_database(recovery_path);
    {
        Database database(DatabaseOptions{.path = recovery_path.string()});
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t share =
            create_share(database, fixture, 1, 1, true);
        const auto candidate = database.journal_candidate(
            make_candidate(fixture, share, 72, 1, now + 1));
        require(database.accept_candidate(candidate.candidate_id, now + 2,
                                          bytes<32>(73), true),
                "recovery round fixture did not close its origin round");
        require(scalar(recovery_path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NULL") == 1,
                "recovery fixture did not retain its pending round effort");
    }
    {
        Database database(DatabaseOptions{.path = recovery_path.string()});
        require(database.latest_accepted_height() == 42U,
                "restart lost the durable accepted-height fence");
        (void)database.recover_interrupted_runtime(now + 1'000'000);
        require(scalar(recovery_path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NOT NULL "
                       "AND finalized_effort_segment_count=0") == 1,
                "startup recovery did not finalize a drained closed round");
    }
    remove_database(recovery_path);

    const auto exhaustion_path = std::filesystem::path(
        test_database_path().string() + "-candidate-exhaustion-recovery");
    remove_database(exhaustion_path);
    {
        Database database(DatabaseOptions{.path = exhaustion_path.string()});
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t share =
            create_share(database, fixture, 1, 1, true);
        const CandidateJournal journal =
            make_candidate(fixture, share, 92, 1, now + 1);
        const auto candidate = database.journal_candidate(journal);
        require(database.start_candidate_attempt(candidate.candidate_id, 1, 900,
                                                 now + 2) > 0,
                "exhaustion recovery candidate attempt did not start");
        CandidateAttemptCompletion indeterminate;
        indeterminate.classification =
            CandidateAttemptClassification::indeterminate;
        indeterminate.completed_unix_us = now + 3;
        require(database.finish_candidate_attempt(candidate.candidate_id, 1,
                                                  indeterminate)
                    .state == CandidateState::ambiguous,
                "exhaustion recovery fixture did not become ambiguous");
        require(database.exhaust_candidate_reconciliation(candidate.candidate_id,
                                                          now + 4),
                "ambiguous candidate was not marked reconciliation-exhausted");

        const auto recoverable = database.recoverable_candidates();
        require(std::any_of(recoverable.begin(), recoverable.end(),
                            [&](const CandidateRecovery &row) {
                                return row.candidate_id == candidate.candidate_id;
                            }),
                "reconciliation-exhausted candidate lost its startup boundary");
        database.schedule_candidate_reconciliation(candidate.candidate_id,
                                                   now + 5);
        const auto restarted = database.start_candidate_reconciliation({
            .candidate_id = candidate.candidate_id,
            .cycle_number = 1,
            .lookup_kind = ReconciliationLookupKind::expected_hash,
            .rpc_request_id = 901,
            .requested_block_id = journal.expected_block_id,
            .started_unix_us = now + 5,
        });
        require(restarted.inserted && restarted.reconciliation_id > 0,
                "startup could not resume authority checks for an exhausted candidate");
        require(scalar(exhaustion_path,
                       "SELECT count(*) FROM candidates "
                       "WHERE reconciliation_exhausted_unix_us IS NOT NULL "
                       "AND reconciliation_cycle_count=1") == 1,
                "resumed exhausted candidate lost its durable safety evidence");
    }
    remove_database(exhaustion_path);
}

void run_schema_v1_rejection_test()
{
    const auto path = std::filesystem::path(test_database_path().string() +
                                            "-schema-v1");
    remove_database(path);
    sqlite3 *raw = nullptr;
    require(sqlite3_open_v2(path.c_str(), &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) == SQLITE_OK,
            "could not create schema-v1 rejection fixture");
    require(sqlite3_exec(
                raw,
                "CREATE TABLE schema_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                "INSERT INTO schema_meta VALUES('schema_version','1');",
                nullptr, nullptr, nullptr) == SQLITE_OK,
            "could not initialize schema-v1 rejection fixture");
    sqlite3_close(raw);
    bool rejected = false;
    try {
        Database database(DatabaseOptions{.path = path.string()});
    }
    catch (const DatabaseError &error) {
        rejected = std::string_view(error.what()).find("clean schema v2") !=
                   std::string_view::npos;
    }
    require(rejected, "schema v1 was not rejected with a clear clean-v2 error");
    remove_database(path);
}

void run_writer_scheduler_test()
{
    const auto path = std::filesystem::path(test_database_path().string() +
                                            "-writer-queue");
    remove_database(path);
    constexpr std::int64_t now = 1'700'300'000'000'000;
    bool rejected_overflowing_reserve = false;
    try {
        Database invalid(DatabaseOptions{
            .path = path.string(),
            .max_writer_queue_items = UINT64_MAX,
            .max_writer_queue_bytes = UINT64_MAX,
            .writer_priority_reserve_items = UINT64_MAX - 1U,
        });
    }
    catch (const DatabaseError &) {
        rejected_overflowing_reserve = true;
    }
    require(rejected_overflowing_reserve,
            "overflowing writer priority reserve was accepted");
    remove_database(path);
    {
        Database database(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 4000,
            .blocknotify_enabled = false,
            .max_writer_queue_items = 2,
            .max_writer_queue_bytes = 1024,
            .writer_priority_reserve_items = 1,
        });
        const Fixture fixture = create_fixture(database, now);
        const std::int64_t share = create_share(database, fixture, 1, 1, false);

        sqlite3 *blocker = nullptr;
        require(sqlite3_open_v2(path.c_str(), &blocker,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                                nullptr) == SQLITE_OK,
                "could not open writer-queue blocker");
        require(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr,
                             nullptr) == SQLITE_OK,
                "could not acquire writer-queue blocker");

        std::exception_ptr first_error;
        std::exception_ptr ordinary_error;
        std::exception_ptr priority_error;
        std::exception_ptr backpressured_error;
        std::atomic<bool> first_entered{false};
        std::atomic<bool> backpressured_done{false};

        std::jthread first([&] {
            try {
                first_entered.store(true, std::memory_order_release);
                (void)database.insert_event(EventInsert{
                    .session_id = fixture.session_id,
                    .created_unix_us = now + 10,
                    .type = "queue_blocker",
                });
            }
            catch (...) { first_error = std::current_exception(); }
        });
        while (!first_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        std::jthread ordinary([&] {
            try {
                (void)database.insert_event(EventInsert{
                    .session_id = fixture.session_id,
                    .created_unix_us = now + 20,
                    .type = "ordinary_queued",
                });
            }
            catch (...) { ordinary_error = std::current_exception(); }
        });

        bool observed_ordinary = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const DatabaseWriterStats stats = database.writer_stats();
            if (stats.queued_items == 1 && stats.priority_items == 0) {
                observed_ordinary = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(observed_ordinary,
                "ordinary writer command was not admitted before priority fixture");

        std::jthread priority([&] {
            try {
                (void)database.accept_share(share, now + 30, "120",
                                            HashrateSource::verified);
            }
            catch (...) { priority_error = std::current_exception(); }
        });

        bool observed_full_queue = false;
        DatabaseWriterStats full_stats;
        for (int attempt = 0; attempt < 100; ++attempt) {
            full_stats = database.writer_stats();
            if (full_stats.queued_items == 2 &&
                full_stats.queued_bytes == 1024 &&
                full_stats.priority_items == 1) {
                observed_full_queue = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::jthread backpressured([&] {
            try {
                (void)database.insert_event(EventInsert{
                    .session_id = fixture.session_id,
                    .created_unix_us = now + 40,
                    .type = "ordinary_backpressured",
                });
            }
            catch (...) { backpressured_error = std::current_exception(); }
            backpressured_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const DatabaseWriterStats capped_stats = database.writer_stats();
        const bool held_back = !backpressured_done.load(std::memory_order_acquire);

        const bool released = sqlite3_exec(blocker, "COMMIT", nullptr, nullptr,
                                           nullptr) == SQLITE_OK;
        sqlite3_close(blocker);
        first.join();
        ordinary.join();
        priority.join();
        backpressured.join();

        require(released, "could not release writer-queue blocker");
        require(first_error == nullptr && ordinary_error == nullptr &&
                    priority_error == nullptr && backpressured_error == nullptr,
                "writer scheduler command failed");
        require(observed_full_queue,
                "writer scheduler did not expose exact full-queue statistics: " +
                    std::to_string(full_stats.queued_items) + "/" +
                    std::to_string(full_stats.queued_bytes) + "/" +
                    std::to_string(full_stats.priority_items));
        require(capped_stats.queued_items == 2 &&
                    capped_stats.queued_bytes == 1024 &&
                    capped_stats.priority_items == 1 && held_back,
                "ordinary admission consumed the writer priority reserve");
        require(scalar(path,
                       "SELECT count(*) FROM events p,events o "
                       "WHERE p.type='share_result' AND o.type='ordinary_queued' "
                       "AND p.id<o.id") == 1,
                "priority writer command did not run ahead of ordinary work");
        const DatabaseWriterStats empty = database.writer_stats();
        require(empty.queued_items == 0 && empty.queued_bytes == 0 &&
                    empty.priority_items == 0,
                "writer scheduler statistics did not drain to zero");
    }
    remove_database(path);
}

} // namespace

int main()
{
    try {
        run_tests();
        run_crash_recovery_test();
        run_symlink_rejection_test();
        run_sibling_lock_test();
        run_reconciliation_attempt_race_test();
        run_candidate_round_boundary_test();
        run_schema_v1_rejection_test();
        run_writer_scheduler_test();
        std::cout << "database tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "database tests failed: " << error.what() << '\n';
        return 1;
    }
}
