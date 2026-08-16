/* Copyright (c) 2026 SeriousPassenger; SPDX-License-Identifier: MIT */
#include "monero_solo/database.hpp"
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

namespace { using namespace monero_solo;
void require(bool v, std::string_view m) { if (!v) throw std::runtime_error(std::string(m)); }
template <std::size_t N> std::array<std::uint8_t,N> bytes(std::uint8_t x) { std::array<std::uint8_t,N> r{}; for (std::size_t i=0;i<N;++i) r[i]=static_cast<std::uint8_t>(x+i); return r; }
template <std::size_t N> std::array<std::uint8_t,N> id_bytes(std::uint64_t x) { std::array<std::uint8_t,N> r{}; for (std::size_t i=0;i<8 && i<N;++i) { r[i]=static_cast<std::uint8_t>(x&0xffU); x>>=8U; } return r; }
std::filesystem::path path_for(std::string_view s) { return std::filesystem::temp_directory_path()/("mss-db-v3-"+std::to_string(::getpid())+"-"+std::string(s)+".sqlite3"); }
void remove_db(const std::filesystem::path &p) { std::error_code e; for (const std::string s:{"","-wal","-shm",".lock"}) std::filesystem::remove(p.string()+s,e); }
std::int64_t scalar(const std::filesystem::path &p,std::string_view q) { sqlite3 *d=nullptr; require(sqlite3_open_v2(p.c_str(),&d,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open inspection DB"); sqlite3_stmt *s=nullptr; require(sqlite3_prepare_v2(d,q.data(),static_cast<int>(q.size()),&s,nullptr)==SQLITE_OK,"prepare inspection query"); require(sqlite3_step(s)==SQLITE_ROW,"inspection query has no row"); auto r=sqlite3_column_int64(s,0); sqlite3_finalize(s); sqlite3_close(d); return r; }
std::string scalar_text(const std::filesystem::path &p,std::string_view q) { sqlite3 *d=nullptr; require(sqlite3_open_v2(p.c_str(),&d,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open text DB"); sqlite3_stmt *s=nullptr; require(sqlite3_prepare_v2(d,q.data(),static_cast<int>(q.size()),&s,nullptr)==SQLITE_OK,"prepare text query"); require(sqlite3_step(s)==SQLITE_ROW,"text query has no row"); const auto *v=reinterpret_cast<const char*>(sqlite3_column_text(s,0)); std::string r=v? v:""; sqlite3_finalize(s); sqlite3_close(d); return r; }
void execute(const std::filesystem::path &p,std::string_view q) { sqlite3 *d=nullptr; require(sqlite3_open_v2(p.c_str(),&d,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,"open mutable DB"); char *e=nullptr; int rc=sqlite3_exec(d,std::string(q).c_str(),nullptr,nullptr,&e); std::string m=e?e:""; sqlite3_free(e); sqlite3_close(d); require(rc==SQLITE_OK,"mutation failed: "+m); }
bool statement_rejected(const std::filesystem::path &p,std::string_view q) { sqlite3 *d=nullptr; require(sqlite3_open_v2(p.c_str(),&d,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,"open rejection DB"); char *e=nullptr; int rc=sqlite3_exec(d,std::string(q).c_str(),nullptr,nullptr,&e); sqlite3_free(e); sqlite3_close(d); return rc!=SQLITE_OK; }

struct Fixture { std::int64_t session{},worker{},connection{}; };
Fixture fixture(Database &db,std::int64_t now,std::uint8_t salt=1) {
 auto session=db.start_session(SessionStart{.public_id=bytes<16>(salt),.started_unix_us=now,.version="db-v3-test",.verifier_commit=std::nullopt});
 auto worker=db.upsert_worker(WorkerInsert{.login="worker-"+std::to_string(salt),.rigid="rig",.seen_unix_us=now});
 auto connection=db.insert_connection(ConnectionInsert{.public_id=bytes<16>(static_cast<std::uint8_t>(salt+32)),.session_id=session,.worker_id=worker,.peer_family=2,.peer_address={127,0,0,salt},.peer_port=3333,.listen_address="127.0.0.1:3333",.agent="db-v3-test",.opened_unix_us=now});
 db.authenticate_connection(connection,worker,"authenticated",now+1); return {session,worker,connection};
}
ShareInsert share(const Fixture &f,std::uint64_t seq,std::int64_t now) { ShareInsert v; v.session_id=f.session; v.connection_id=f.connection; v.worker_id=f.worker; v.job_public_id=bytes<16>(static_cast<std::uint8_t>(seq)); v.template_generation=7; v.height=42; v.request_sequence=seq; v.received_unix_us=now; v.nonce=bytes<4>(9); v.assigned_difficulty_dec="4194304"; v.network_difficulty_dec="700000000000"; return v; }
ShareAcceptance accepted(std::int64_t id,std::int64_t now,std::string actual) { ShareAcceptance v; v.share_id=id; v.completed_unix_us=now; v.assigned_difficulty_dec="4194304"; v.source=HashrateSource::verified; v.actual_difficulty_dec=std::move(actual); return v; }
ShareFinalization finalized(std::string status,std::string provenance,std::int64_t now,std::optional<std::string> actual=std::nullopt) { ShareFinalization v; v.status=std::move(status); v.provenance=std::move(provenance); v.completed_unix_us=now; v.actual_difficulty_dec=std::move(actual); return v; }

ShareInsert share_at_height(const Fixture &fixture, std::uint64_t sequence,
                            std::uint64_t height, std::int64_t now)
{
    ShareInsert value = share(fixture, sequence, now);
    value.height = height;
    value.job_public_id = bytes<16>(static_cast<std::uint8_t>(sequence));
    return value;
}

CandidateJournal candidate(const Fixture &fixture,
                             const PersistedShareIdentity &origin,
                             std::uint64_t sequence, std::uint8_t tag,
                             std::uint64_t height, std::int64_t now,
                             std::uint32_t max_attempts = 4)
{
    return CandidateJournal{
        .candidate_key = bytes<32>(tag),
        .first_share_id = origin.share_id,
        .session_id = fixture.session,
        .round_id = origin.round_id,
        .job_public_id = bytes<16>(static_cast<std::uint8_t>(sequence)),
        .template_generation = 7,
        .connection_id = fixture.connection,
        .height = height,
        .peer_family = 2,
        .peer_address = {127, 0, 0, static_cast<std::uint8_t>(tag)},
        .frozen_block_blob = {tag, 1, 2, 3, 4},
        .miner_tx_hash = bytes<32>(static_cast<std::uint8_t>(tag + 40U)),
        .expected_block_id = bytes<32>(static_cast<std::uint8_t>(tag + 80U)),
        .max_attempts = max_attempts,
        .created_unix_us = now,
    };
}

template <typename Function>
bool database_error_contains(Function &&function, std::string_view fragment)
{
    try {
        function();
    }
    catch (const DatabaseError &error) {
        return std::string_view(error.what()).find(fragment) !=
               std::string_view::npos;
    }
    return false;
}

void selective_test() {
 auto p=path_for("selective"); remove_db(p); {
  Database db(DatabaseOptions{.path=p.string()}); auto f=fixture(db,1'000'000);
  require(db.schema_version()==3,"schema is not v3");
  const auto pragmas=db.pragmas(); require(pragmas.journal_mode=="wal"&&pragmas.synchronous=="full"&&pragmas.foreign_keys&&pragmas.busy_timeout_ms==5000,"required SQLite pragmas are not enforced");
  require(scalar(p,"SELECT count(*) FROM sqlite_schema WHERE name IN ('public_templates','private_jobs','duplicate_keys')")==0,"routine tables remain");
  auto low=db.insert_share(share(f,1,2'000'000)); require(low<0,"share ID is not transient"); require(db.writer_stats().pending_transient_shares==1,"active transient share count missing"); require(scalar(p,"SELECT count(*) FROM shares")==0,"transient share was written");
  auto result=db.accept_share(accepted(low,3'000'000,"79999999999")); require(result.accepted&&result.event_id==0,"low share was not aggregated"); require(db.writer_stats().pending_accounting_items==1,"pending accounting not exposed"); require(scalar(p,"SELECT count(*) FROM shares")==0,"79,999,999,999 share retained");
  db.flush_accounting(); require(scalar(p,"SELECT share_count FROM share_totals WHERE status='accepted' AND provenance='verified'")==1,"accepted total missing"); require(scalar_text(p,"SELECT credited_difficulty_dec FROM rounds WHERE state='open'")=="4194304","round work missing"); require(scalar(p,"SELECT accepted_shares FROM hashrate_buckets WHERE scope_type='global'")==1,"hashrate missing");
  auto high=db.insert_share(share(f,2,4'000'000)); db.mark_share_verifying(high,"9","10"); db.insert_share_hash(high,"claimed",bytes<32>(2),true,false); db.insert_share_hash(high,"computed",bytes<32>(3),true,false); const auto high_result=db.accept_share(accepted(high,5'000'000,"80000000000")); require(high_result.accepted&&high_result.persisted_share_id>0,"80G share failed"); require(db.ensure_share_persisted(high,"high_difficulty").share_id==high_result.persisted_share_id,"auto-promotion alias did not resolve"); require(scalar(p,"SELECT count(*) FROM shares WHERE retention_reason='high_difficulty' AND status='accepted'")==1,"80G share not retained"); require(scalar(p,"SELECT count(*) FROM share_hashes WHERE share_id="+std::to_string(high_result.persisted_share_id))==2,"promoted share hashes were lost"); require(scalar(p,"SELECT share_count FROM share_totals WHERE status='accepted' AND provenance='verified'")==2,"retained total missing");
  auto stale=db.insert_share(share(f,3,6'000'000)); db.mark_share_verifying(stale,"1","1"); db.set_share_height_is_older(stale,true); db.insert_share_hash(stale,"claimed",bytes<32>(3),false,false); require(db.finalize_share(stale,finalized("stale","verified",7'000'000,"1")),"stale finalize failed"); db.flush_accounting(); require(scalar(p,"SELECT share_count FROM share_totals WHERE status='stale' AND provenance='verified'")==1,"stale total missing"); require(scalar(p,"SELECT count(*) FROM shares")==1,"low terminal row persisted");
 } remove_db(p);
}

void candidate_test() {
 auto p=path_for("candidate"); remove_db(p); {
  Database db(DatabaseOptions{.path=p.string(),.blocknotify_enabled=true}); auto f=fixture(db,10'000'000,10); auto transient=db.insert_share(share(f,1,11'000'000)); auto identity=db.ensure_share_persisted(transient,"candidate"); require(identity.share_id>0&&identity.round_id==db.current_open_round_id(),"promotion identity wrong");
  auto journal=db.journal_candidate(CandidateJournal{.candidate_key=bytes<32>(40),.first_share_id=identity.share_id,.session_id=f.session,.round_id=identity.round_id,.job_public_id=bytes<16>(1),.template_generation=7,.connection_id=f.connection,.height=42,.peer_family=2,.peer_address={127,0,0,10},.frozen_block_blob={1,2,3,4},.miner_tx_hash=bytes<32>(80),.expected_block_id=bytes<32>(120),.created_unix_us=12'000'000}); require(journal.inserted,"candidate not journaled");
  auto recovered=db.recoverable_candidates(); require(recovered.size()==1&&recovered[0].round_id==identity.round_id&&recovered[0].job_public_id==bytes<16>(1)&&recovered[0].template_generation==7,"candidate context did not recover");
  require(db.accept_share(accepted(identity.share_id,13'000'000,"700000000000")).accepted,"candidate share accept failed"); require(db.accept_candidate(journal.candidate_id,14'000'000,bytes<32>(120),false),"candidate accept failed"); require(scalar(p,"SELECT count(*) FROM rounds WHERE state='closed' AND accepted_candidate_id IS NOT NULL")==1,"round not closed");
 } remove_db(p);
}

void bounds_test() {
 auto p=path_for("bounds"); remove_db(p); {
  Database db(DatabaseOptions{.path=p.string()}); auto f=fixture(db,20'000'000,20);
  for (std::uint64_t i=1;i<=4096;++i) { auto id=db.insert_share(share(f,i,21'000'000+static_cast<std::int64_t>(i))); require(db.finalize_share(id,finalized("malformed","pending",22'000'000+static_cast<std::int64_t>(i))),"malformed finalize failed"); }
  require(db.writer_stats().pending_accounting_items==0,"4096 bound did not flush"); require(scalar(p,"SELECT share_count FROM share_totals WHERE status='malformed' AND provenance='pending'")==4096,"bounded flush lost totals"); require(db.close_connection(f.connection,30'000'000,"test"),"close failed"); require(!db.close_connection(f.connection,30'000'001,"repeated"),"pruned connection close was not idempotent"); require(scalar(p,"SELECT count(*) FROM connections")==0&&scalar(p,"SELECT count(*) FROM workers")==0,"connection churn not pruned");
 } remove_db(p);
}

void hundred_thousand_low_share_test()
{
    const auto path = path_for("100k-low");
    remove_db(path);
    {
        Database db(DatabaseOptions{.path = path.string()});
        const Fixture value = fixture(db, 40'000'000, 30);
        constexpr std::uint64_t share_count = 100'000;
        for (std::uint64_t sequence = 1; sequence <= share_count; ++sequence) {
            const auto id = db.insert_share(
                share(value, sequence,
                      41'000'000 + static_cast<std::int64_t>(sequence)));
            require(db.finalize_share(
                        id, finalized(
                                "malformed", "pending",
                                42'000'000 +
                                    static_cast<std::int64_t>(sequence))),
                    "100k low-share finalization failed");
        }
        db.flush_accounting();
        require(db.writer_stats().pending_transient_shares == 0,
                "transient shares leaked after 100k completions");
        require(scalar(path, "SELECT count(*) FROM shares") == 0,
                "ordinary low shares grew the retained table");
        require(scalar(path, "SELECT count(*) FROM events") == 2,
                "ordinary low shares grew the event table");
        require(scalar(path, "SELECT count(*) FROM share_totals") == 1 &&
                    scalar(path,
                           "SELECT share_count FROM share_totals WHERE "
                           "status='malformed' AND provenance='pending'") ==
                        static_cast<std::int64_t>(share_count),
                "100k low-share aggregate is not exact and bounded");
        require(scalar(path, "SELECT count(*) FROM hashrate_buckets") == 0 &&
                    scalar(path, "SELECT count(*) FROM round_work_segments") == 0,
                "nonaccepted low shares created work rows");
    }
    remove_db(path);
}

void round_height_contamination_test()
{
    const auto run_prejournal_case = [](std::string_view name,
                                        bool finalize_higher,
                                        bool flush_higher) {
        const auto path = path_for(name);
        remove_db(path);
        {
            Database db(DatabaseOptions{.path = path.string()});
            const Fixture value = fixture(db, 50'000'000, 40);
            const auto origin_alias = db.insert_share(
                share_at_height(value, 1, 42, 51'000'000));
            const auto origin = db.ensure_share_persisted(
                origin_alias, "candidate");
            const auto higher = db.insert_share(
                share_at_height(value, 2, 43, 52'000'000));
            if (finalize_higher) {
                require(db.finalize_share(
                            higher,
                            finalized("malformed", "pending", 53'000'000)),
                        "higher-height fixture did not finalize");
            }
            if (flush_higher) {
                db.flush_accounting();
            }
            const CandidateJournalResult result = db.journal_candidate(
                candidate(value, origin, 1, 41, 42, 54'000'000));
            require(result.round_contaminated && result.candidate_id == 0 &&
                        !result.inserted,
                    "old-height candidate was not rejected before journaling");
            require(scalar(path, "SELECT count(*) FROM candidates") == 0,
                    "contaminated candidate became durable");
            require(scalar(path,
                           "SELECT max_share_height FROM rounds WHERE state='open'") ==
                        43,
                    "round maximum did not include admitted higher-height work");
        }
        remove_db(path);
    };

    run_prejournal_case("height-active", false, false);
    run_prejournal_case("height-pending", true, false);
    run_prejournal_case("height-flushed", true, true);

    const auto path = path_for("height-accept-invariant");
    remove_db(path);
    {
        Database db(DatabaseOptions{.path = path.string()});
        const Fixture value = fixture(db, 60'000'000, 50);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, 61'000'000)),
            "candidate");
        const auto journal = db.journal_candidate(
            candidate(value, origin, 1, 51, 42, 62'000'000, 1));
        require(journal.inserted && !journal.round_contaminated,
                "clean candidate was not journaled");
        require(db.start_candidate_attempt(
                    journal.candidate_id, 1, 700, 63'000'000) > 0,
                "contamination attempt did not start");
        (void)db.insert_share(
            share_at_height(value, 2, 43, 64'000'000));
        CandidateAttemptCompletion completion;
        completion.classification = CandidateAttemptClassification::accepted;
        completion.completed_unix_us = 65'000'000;
        completion.http_status = 200;
        completion.daemon_status = "OK";
        require(database_error_contains(
                    [&] {
                        (void)db.finish_candidate_attempt(
                            journal.candidate_id, 1, completion);
                    },
                    "higher-height"),
                "acceptance invariant did not fail closed on contamination");
        require(scalar(path,
                       "SELECT count(*) FROM candidate_attempts WHERE "
                       "classification='dispatching'") == 1 &&
                    scalar(path,
                           "SELECT count(*) FROM candidates WHERE "
                           "state='dispatching'") == 1 &&
                    scalar(path,
                           "SELECT count(*) FROM rounds WHERE state='open'") == 1 &&
                    scalar(path,
                           "SELECT count(*) FROM rounds WHERE state='closed'") == 0,
                "failed candidate acceptance was not rolled back atomically");
    }
    remove_db(path);
}

void hashrate_scope_identity_and_pruning_test()
{
    const auto path = path_for("hashrate-scope");
    remove_db(path);
    {
        Database db(DatabaseOptions{.path = path.string()});
        const Fixture first = fixture(db, 70'000'000, 60);
        const auto low = db.insert_share(
            share(first, 1, 90'000'000));
        require(db.accept_share(
                    accepted(low, 100'000'000, "79999999999"))
                    .accepted,
                "hashrate fixture share was not accepted");
        db.flush_accounting();
        require(db.close_connection(first.connection, 101'000'000, "rotate"),
                "hashrate fixture connection did not close");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE id=" +
                           std::to_string(first.connection)) == 1,
                "connection identity was pruned while its 24h bucket remained");

        const std::int64_t second_connection = db.insert_connection(
            ConnectionInsert{
                .public_id = bytes<16>(93),
                .session_id = first.session,
                .worker_id = first.worker,
                .peer_family = 2,
                .peer_address = {127, 0, 0, 93},
                .peer_port = 3333,
                .listen_address = "127.0.0.1:3333",
                .agent = "scope-reuse-test",
                .opened_unix_us = 102'000'000,
            });
        require(second_connection > first.connection,
                "connection AUTOINCREMENT identity was reused");
        require(db.update_connection_last_sent_height(second_connection, 100),
                "first last-sent-height update was ignored");
        require(!db.update_connection_last_sent_height(second_connection, 100),
                "equal last-sent height was reported as changed");
        require(db.update_connection_last_sent_height(second_connection, 99),
                "downward reorg height was not persisted");
        require(scalar(path,
                       "SELECT last_sent_height FROM connections WHERE id=" +
                           std::to_string(second_connection)) == 99,
                "downward last-sent height is not durable");

        Fixture second = first;
        second.connection = second_connection;
        const auto retained = db.insert_share(
            share(second, 2, 86'500'000'000));
        const auto result = db.accept_share(
            accepted(retained, 86'501'000'000, "80000000000"));
        require(result.accepted && result.persisted_share_id > 0,
                "retained-only pruning fixture was not durable");
        require(scalar(path,
                       "SELECT count(*) FROM hashrate_buckets WHERE "
                       "scope_type='connection' AND scope_id=" +
                           std::to_string(first.connection)) == 0,
                "expired connection bucket survived retained-only traffic");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE id=" +
                           std::to_string(first.connection)) == 0,
                "expired closed connection context was not pruned");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE id=" +
                           std::to_string(second_connection)) == 1,
                "retained share lost its live connection context");
    }
    remove_db(path);
}

void atomic_worker_authentication_pruning_test()
{
    const auto path = path_for("atomic-worker-auth");
    remove_db(path);
    {
        Database db(DatabaseOptions{.path = path.string()});
        const Fixture reconnecting = fixture(db, 70'000'000, 61);
        const auto low = db.insert_share(
            share(reconnecting, 1, 90'000'000));
        require(db.accept_share(
                    accepted(low, 100'000'000, "79999999999"))
                    .accepted,
                "reconnect fixture share was not accepted");
        db.flush_accounting();
        require(db.close_connection(
                    reconnecting.connection, 101'000'000, "reconnect"),
                "reconnect fixture connection did not close");

        const std::int64_t new_connection = db.insert_connection(
            ConnectionInsert{
                .public_id = bytes<16>(95),
                .session_id = reconnecting.session,
                .worker_id = std::nullopt,
                .peer_family = 2,
                .peer_address = {127, 0, 0, 95},
                .peer_port = 3333,
                .listen_address = "127.0.0.1:3333",
                .agent = "pre-auth",
                .opened_unix_us = 102'000'000,
            });
        const WorkerInsert worker{
            .login = "worker-61",
            .rigid = "rig",
            .seen_unix_us = 102'000'001,
        };
        require(db.upsert_worker(worker) == reconnecting.worker,
                "reconnect did not initially resolve the existing worker");

        const Fixture pruning = fixture(db, 103'000'000, 62);
        const auto retained = db.insert_share(
            share(pruning, 2, 86'500'000'000));
        require(db.accept_share(
                    accepted(retained, 86'501'000'000, "80000000000"))
                    .persisted_share_id > 0,
                "pruning fixture share was not retained");
        require(scalar(path,
                       "SELECT count(*) FROM workers WHERE id=" +
                           std::to_string(reconnecting.worker)) == 0 &&
                    scalar(path,
                           "SELECT count(*) FROM connections WHERE id=" +
                               std::to_string(new_connection) +
                               " AND worker_id IS NULL") == 1,
                "fixture did not reproduce the split upsert/authenticate race");
        require(database_error_contains(
                    [&] {
                        db.authenticate_connection(
                            new_connection, reconnecting.worker,
                            "stale-worker", 86'501'000'001);
                    },
                    "FOREIGN KEY"),
                "a pruned worker ID unexpectedly remained authenticatable");

        const std::int64_t replacement =
            db.upsert_worker_and_authenticate_connection(
                new_connection, worker, "atomic-auth", 86'501'000'002);
        require(replacement > reconnecting.worker,
                "pruned worker AUTOINCREMENT identity was reused");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE id=" +
                           std::to_string(new_connection) +
                           " AND worker_id=" + std::to_string(replacement) +
                           " AND agent='atomic-auth' AND "
                           "authenticated_unix_us=86501000002") == 1,
                "atomic worker authentication did not retain its worker");

        require(database_error_contains(
                    [&] {
                        (void)db.upsert_worker_and_authenticate_connection(
                            new_connection,
                            WorkerInsert{
                                .login = "rolled-back-worker",
                                .rigid = "rig",
                                .seen_unix_us = 86'501'000'003,
                            },
                            "repeated-auth", 86'501'000'003);
                    },
                    "already authenticated"),
                "repeated atomic authentication was not rejected");
        require(scalar(path,
                       "SELECT count(*) FROM workers WHERE "
                       "login='rolled-back-worker'") == 0,
                "failed atomic authentication left an orphan worker");
    }
    remove_db(path);
}

void crash_recovery_pruning_test()
{
    const auto empty_path = path_for("crash-prune-empty");
    remove_db(empty_path);
    {
        Database db(DatabaseOptions{.path = empty_path.string()});
        (void)fixture(db, 90'000'000, 70);
    }
    {
        Database db(DatabaseOptions{.path = empty_path.string()});
        const InterruptedRuntimeRecovery recovery =
            db.recover_interrupted_runtime(91'000'000);
        require(recovery.sessions_stopped == 1 &&
                    recovery.connections_closed == 1,
                "crash recovery did not close process-local rows");
        require(scalar(empty_path, "SELECT count(*) FROM connections") == 0 &&
                    scalar(empty_path, "SELECT count(*) FROM workers") == 0,
                "crash recovery retained unreferenced connection context");
        const InterruptedRuntimeRecovery repeated =
            db.recover_interrupted_runtime(92'000'000);
        require(repeated.sessions_stopped == 0 &&
                    repeated.connections_closed == 0,
                "crash recovery was not idempotent");
    }
    remove_db(empty_path);

    const auto evidence_path = path_for("crash-retained-evidence");
    remove_db(evidence_path);
    std::int64_t retained_id = 0;
    {
        Database db(DatabaseOptions{.path = evidence_path.string()});
        const Fixture value = fixture(db, 100'000'000, 80);
        ShareInsert interrupted = share(value, 1, 101'000'000);
        interrupted.status = "verifying";
        interrupted.provenance = "claimed";
        retained_id = db.ensure_share_persisted(
            db.insert_share(interrupted), "security_evidence").share_id;
    }
    {
        Database db(DatabaseOptions{.path = evidence_path.string()});
        (void)db.recover_interrupted_runtime(102'000'000);
        require(scalar(evidence_path,
                       "SELECT count(*) FROM shares WHERE id=" +
                           std::to_string(retained_id) +
                           " AND status='cancelled' AND provenance='pending' "
                           "AND error_code='process_restarted'") == 1,
                "interrupted retained share was not cancelled consistently");
        require(scalar(evidence_path,
                       "SELECT share_count FROM share_totals WHERE "
                       "status='cancelled' AND provenance='pending'") == 1 &&
                    scalar(evidence_path,
                           "SELECT count(*) FROM share_totals WHERE "
                           "status='cancelled' AND provenance='claimed'") == 0,
                "cancelled aggregate provenance differs from rewritten row");
        require(scalar(evidence_path, "SELECT count(*) FROM connections") == 1 &&
                    scalar(evidence_path, "SELECT count(*) FROM workers") == 1,
                "crash pruning removed retained security context");
    }
    remove_db(evidence_path);
}

void closed_round_crash_finalization_test()
{
    const auto path = path_for("closed-round-recovery");
    remove_db(path);
    constexpr std::int64_t now = 150'000'000;
    {
        Database db(DatabaseOptions{.path = path.string()});
        const Fixture value = fixture(db, now, 85);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        const auto journal = db.journal_candidate(
            candidate(value, origin, 1, 86, 42, now + 2));
        require(db.accept_candidate(journal.candidate_id, now + 3,
                                    bytes<32>(87), true),
                "crash-finalization candidate was not accepted");
        require(scalar(path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NULL") == 1,
                "closed round did not wait for its active retained share");
    }
    {
        Database db(DatabaseOptions{.path = path.string()});
        require(db.latest_accepted_height() == 42,
                "restart lost latest accepted height");
        (void)db.recover_interrupted_runtime(now + 1'000'000);
        require(scalar(path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NOT NULL "
                       "AND finalized_effort_segment_count=0") == 1,
                "crash recovery did not finalize a drained closed round");
    }
    remove_db(path);
}

void candidate_recovery_and_blocknotify_test()
{
    const auto path = path_for("candidate-recovery");
    remove_db(path);
    std::int64_t candidate_id = 0;
    std::int64_t connection_id = 0;
    constexpr std::int64_t now = 200'000'000;
    {
        Database db(DatabaseOptions{
            .path = path.string(),
            .blocknotify_enabled = true,
        });
        const Fixture value = fixture(db, now, 90);
        connection_id = value.connection;
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        CandidateJournal snapshot =
            candidate(value, origin, 1, 91, 42, now + 2, 2);
        const auto journal = db.journal_candidate(snapshot);
        require(journal.inserted && journal.candidate_id > 0,
                "candidate recovery row was not journaled");
        candidate_id = journal.candidate_id;
        const auto duplicate = db.journal_candidate(snapshot);
        require(!duplicate.inserted && !duplicate.round_contaminated &&
                    duplicate.candidate_id == candidate_id,
                "candidate journal was not idempotent");
        snapshot.frozen_block_blob.push_back(9);
        require(database_error_contains(
                    [&] { (void)db.journal_candidate(snapshot); },
                    "different frozen bytes"),
                "candidate-key collision accepted different frozen bytes");
        require(db.start_candidate_attempt(candidate_id, 1, 800, now + 3) > 0,
                "candidate recovery attempt did not start");
    }

    {
        Database db(DatabaseOptions{
            .path = path.string(),
            .blocknotify_enabled = true,
        });
        const auto recoverable = db.recoverable_candidates();
        require(recoverable.size() == 1 &&
                    recoverable.front().candidate_id == candidate_id &&
                    recoverable.front().state == CandidateState::retry_wait &&
                    recoverable.front().attempt_count == 1 &&
                    recoverable.front().had_indeterminate &&
                    recoverable.front().connection_id == connection_id &&
                    !recoverable.front().frozen_block_blob.empty(),
                "in-flight candidate did not recover immutable context");
        const auto recovery = db.recover_interrupted_runtime(now + 10);
        require(recovery.sessions_stopped == 1 &&
                    recovery.connections_closed == 1,
                "candidate restart recovery did not close the old runtime");
        require(scalar(path,
                       "SELECT count(*) FROM connections WHERE id=" +
                           std::to_string(connection_id)) == 1,
                "candidate recovery pruned required connection context");
        (void)db.start_session(SessionStart{
            .public_id = bytes<16>(111),
            .started_unix_us = now + 11,
            .version = "candidate-replacement",
            .verifier_commit = std::nullopt,
        });
        require(db.start_candidate_attempt(candidate_id, 2, 801, now + 12) > 0,
                "recovered candidate did not advance attempt number");
        CandidateAttemptCompletion accepted_completion;
        accepted_completion.classification =
            CandidateAttemptClassification::accepted;
        accepted_completion.completed_unix_us = now + 13;
        accepted_completion.http_status = 200;
        accepted_completion.daemon_status = "OK";
        accepted_completion.daemon_block_id = bytes<32>(171);
        const auto accepted_result = db.finish_candidate_attempt(
            candidate_id, 2, accepted_completion);
        require(accepted_result.terminal &&
                    accepted_result.state == CandidateState::accepted,
                "recovered candidate was not accepted");
        require(!db.accept_candidate(candidate_id, now + 14,
                                     bytes<32>(171), false),
                "candidate acceptance was not idempotent");
        require(db.pending_blocknotify_count() == 1,
                "accepted candidate did not enqueue one notification");
        const auto delivery = db.claim_next_blocknotify(now + 15);
        require(delivery.has_value() && delivery->attempt_count == 1,
                "blocknotify was not claimed");
    }

    {
        Database db(DatabaseOptions{
            .path = path.string(),
            .blocknotify_enabled = true,
        });
        const auto recovered_delivery = db.claim_next_blocknotify(now + 16);
        require(recovered_delivery.has_value() &&
                    recovered_delivery->candidate_id == candidate_id &&
                    recovered_delivery->attempt_count == 2,
                "running blocknotify was not recovered at least once");
        db.finish_blocknotify(
            recovered_delivery->id,
            BlocknotifyCompletion{
                .delivered = false,
                .completed_unix_us = now + 17,
                .exit_code = 7,
                .term_signal = std::nullopt,
                .stderr_excerpt = "fixture failure",
                .last_error = "nonzero exit",
            });
        require(!db.claim_next_blocknotify(now + 5'000'016).has_value(),
                "blocknotify retried before its five-second delay");
        const auto retry = db.claim_next_blocknotify(now + 5'000'017);
        require(retry.has_value() && retry->attempt_count == 3,
                "blocknotify did not retry at its exact due time");
        db.finish_blocknotify(
            retry->id,
            BlocknotifyCompletion{
                .delivered = true,
                .completed_unix_us = now + 5'000'018,
                .exit_code = 0,
                .term_signal = std::nullopt,
                .stderr_excerpt = std::nullopt,
                .last_error = std::nullopt,
            });
        require(db.pending_blocknotify_count() == 0,
                "successful recovered blocknotify remained pending");
    }
    remove_db(path);
}

void candidate_rejection_and_reconciliation_test()
{
    constexpr std::int64_t now = 300'000'000;
    const auto rejection_path = path_for("candidate-rejection");
    remove_db(rejection_path);
    {
        Database db(DatabaseOptions{.path = rejection_path.string()});
        const Fixture value = fixture(db, now, 100);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        const CandidateJournal snapshot =
            candidate(value, origin, 1, 101, 42, now + 2, 1);
        const auto journal = db.journal_candidate(snapshot);
        const auto verdict = db.record_candidate_verdict(
            CandidateVerdictInsert{
                .share_id = origin.share_id,
                .kind = CandidateVerdictKind::false_candidate,
                .candidate_key = snapshot.candidate_key,
                .candidate_id = journal.candidate_id,
                .created_unix_us = now + 3,
            });
        require(verdict.disposition == CandidateVerdictDisposition::pending,
                "pre-rejection verdict was not pending");
        require(db.start_candidate_attempt(
                    journal.candidate_id, 1, 900, now + 4) > 0,
                "rejection attempt did not start");
        CandidateAttemptCompletion rejected;
        rejected.classification =
            CandidateAttemptClassification::explicit_rejection;
        rejected.completed_unix_us = now + 5;
        rejected.http_status = 200;
        rejected.daemon_status = "REJECTED";
        const auto result = db.finish_candidate_attempt(
            journal.candidate_id, 1, rejected);
        require(result.terminal && result.state == CandidateState::rejected &&
                    result.newly_actionable_false_candidates == 1 &&
                    result.newly_actionable_candidate_mismatches == 0,
                "explicit rejection did not resolve security evidence");
        require(scalar(rejection_path,
                       "SELECT count(*) FROM candidate_verdicts WHERE "
                       "disposition='actionable'") == 1 &&
                    scalar(rejection_path,
                           "SELECT count(*) FROM abuse_events WHERE "
                           "kind='verified_false_candidate'") == 1,
                "rejected candidate evidence was not made durable/actionable");
    }
    remove_db(rejection_path);

    const auto reconciliation_path = path_for("candidate-reconciliation");
    remove_db(reconciliation_path);
    {
        Database db(DatabaseOptions{.path = reconciliation_path.string()});
        const Fixture value = fixture(db, now + 1'000'000, 110);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(
                value, 1, 42, now + 1'000'001)),
            "candidate");
        const CandidateJournal snapshot = candidate(
            value, origin, 1, 111, 42, now + 1'000'002, 1);
        const auto journal = db.journal_candidate(snapshot);
        require(db.start_candidate_attempt(
                    journal.candidate_id, 1, 901, now + 1'000'003) > 0,
                "ambiguous attempt did not start");
        CandidateAttemptCompletion indeterminate;
        indeterminate.classification =
            CandidateAttemptClassification::indeterminate;
        indeterminate.completed_unix_us = now + 1'000'004;
        require(db.finish_candidate_attempt(
                    journal.candidate_id, 1, indeterminate)
                    .state == CandidateState::ambiguous,
                "indeterminate exhaustion did not become ambiguous");
        require(db.exhaust_candidate_reconciliation(
                    journal.candidate_id, now + 1'000'005),
                "ambiguous candidate was not durably marked exhausted");
        const auto exhausted_boundary = db.recoverable_candidates();
        require(std::any_of(
                    exhausted_boundary.begin(), exhausted_boundary.end(),
                    [&](const CandidateRecovery &row) {
                        return row.candidate_id == journal.candidate_id;
                    }),
                "reconciliation-exhausted candidate lost recovery authority");
        db.schedule_candidate_reconciliation(
            journal.candidate_id, now + 1'000'006);
        const auto lookup = db.start_candidate_reconciliation(
            CandidateReconciliationStart{
                .candidate_id = journal.candidate_id,
                .cycle_number = 1,
                .lookup_kind = ReconciliationLookupKind::expected_hash,
                .rpc_request_id = 902,
                .requested_block_id = snapshot.expected_block_id,
                .started_unix_us = now + 1'000'006,
            });
        require(lookup.inserted &&
                    !db.start_candidate_reconciliation(
                           CandidateReconciliationStart{
                               .candidate_id = journal.candidate_id,
                               .cycle_number = 1,
                               .lookup_kind =
                                   ReconciliationLookupKind::expected_hash,
                               .rpc_request_id = 902,
                               .requested_block_id = snapshot.expected_block_id,
                               .started_unix_us = now + 1'000'006,
                           })
                         .inserted,
                "reconciliation start was not idempotent");
        CandidateReconciliationCompletion positive;
        positive.classification = ReconciliationClassification::positive;
        positive.completed_unix_us = now + 1'000'007;
        positive.observed_block_id = snapshot.expected_block_id;
        positive.observed_height = 42;
        positive.observed_miner_tx_hash = snapshot.miner_tx_hash;
        positive.observed_orphan = false;
        const auto accepted = db.finish_candidate_reconciliation(
            lookup.reconciliation_id, positive);
        require(accepted.candidate_accepted &&
                    accepted.candidate_state ==
                        CandidateState::accepted_by_reconciliation,
                "positive reconciliation did not accept candidate");
        require(db.finish_candidate_reconciliation(
                    lookup.reconciliation_id, positive)
                    .already_completed,
                "reconciliation completion was not idempotent");
    }
    remove_db(reconciliation_path);
}

void candidate_race_and_round_boundary_test()
{
    constexpr std::int64_t now = 400'000'000;
    const auto race_path = path_for("candidate-attempt-race");
    remove_db(race_path);
    {
        Database db(DatabaseOptions{
            .path = race_path.string(),
            .blocknotify_enabled = true,
        });
        const Fixture value = fixture(db, now, 120);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        const auto journal = db.journal_candidate(
            candidate(value, origin, 1, 121, 42, now + 2, 2));
        require(db.start_candidate_attempt(
                    journal.candidate_id, 1, 1000, now + 3) > 0,
                "race attempt did not start");
        require(db.accept_candidate(journal.candidate_id, now + 4,
                                    bytes<32>(122), true),
                "reconciliation did not win dispatch race");
        CandidateAttemptCompletion late;
        late.classification =
            CandidateAttemptClassification::explicit_rejection;
        late.completed_unix_us = now + 5;
        late.http_status = 200;
        late.rpc_error_code = -7;
        const auto finished = db.finish_candidate_attempt(
            journal.candidate_id, 1, late);
        require(finished.terminal &&
                    finished.state ==
                        CandidateState::accepted_by_reconciliation &&
                    db.pending_blocknotify_count() == 1,
                "late rejection reversed reconciliation acceptance");
        require(scalar(race_path,
                       "SELECT count(*) FROM candidate_attempts WHERE "
                       "classification='explicit_rejection'") == 1,
                "late race evidence was not durably closed");
    }
    remove_db(race_path);

    const auto boundary_path = path_for("candidate-origin-boundary");
    remove_db(boundary_path);
    {
        Database db(DatabaseOptions{.path = boundary_path.string()});
        const Fixture value = fixture(db, now + 1'000'000, 130);
        const auto first_origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(
                value, 1, 42, now + 1'000'001)),
            "candidate");
        const auto second_origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(
                value, 2, 42, now + 1'000'002)),
            "candidate");
        const auto closed_origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(
                value, 3, 42, now + 1'000'003)),
            "candidate");
        const auto winner = db.journal_candidate(candidate(
            value, first_origin, 1, 131, 42, now + 1'000'004, 1));
        const auto late = db.journal_candidate(candidate(
            value, second_origin, 2, 132, 42, now + 1'000'004, 1));
        require(db.start_candidate_attempt(
                    winner.candidate_id, 1, 1010, now + 1'000'005) > 0 &&
                    db.start_candidate_attempt(
                        late.candidate_id, 1, 1011, now + 1'000'005) > 0,
                "parallel round attempts did not start");
        require(db.accept_candidate(winner.candidate_id, now + 1'000'006,
                                    bytes<32>(133), true),
                "origin-round winner was not accepted");
        const CandidateJournalResult refused = db.journal_candidate(candidate(
            value, closed_origin, 3, 134, 42, now + 1'000'007, 1));
        require(refused.round_contaminated && refused.candidate_id == 0 &&
                    !refused.inserted &&
                    scalar(boundary_path,
                           "SELECT count(*) FROM candidates") == 2 &&
                    scalar(boundary_path,
                           "SELECT candidate_id IS NULL FROM shares WHERE id=" +
                               std::to_string(closed_origin.share_id)) == 1,
                "candidate was journaled after its origin round closed");
        CandidateAttemptCompletion daemon_ok;
        daemon_ok.classification = CandidateAttemptClassification::accepted;
        daemon_ok.completed_unix_us = now + 1'000'008;
        daemon_ok.http_status = 200;
        daemon_ok.daemon_status = "OK";
        require(database_error_contains(
                    [&] {
                        (void)db.finish_candidate_attempt(
                            late.candidate_id, 1, daemon_ok);
                    },
                    "origin round"),
                "late candidate closed its successor round");
        require(scalar(boundary_path,
                       "SELECT count(*) FROM rounds WHERE state='closed'") == 1 &&
                    scalar(boundary_path,
                           "SELECT count(*) FROM rounds WHERE state='open'") == 1 &&
                    scalar(boundary_path,
                           "SELECT count(*) FROM candidate_attempts WHERE "
                           "candidate_id=" + std::to_string(late.candidate_id) +
                           " AND classification='dispatching'") == 1,
                "late origin-round acceptance did not roll back atomically");
    }
    remove_db(boundary_path);
}

void round_finalization_and_alias_bound_test()
{
    constexpr std::int64_t now = 500'000'000;
    const auto round_path = path_for("round-finalization");
    remove_db(round_path);
    {
        Database db(DatabaseOptions{.path = round_path.string()});
        const Fixture value = fixture(db, now, 140);
        const auto first = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        const auto second = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 2, 42, now + 2)),
            "candidate");
        const auto journal = db.journal_candidate(
            candidate(value, first, 1, 141, 42, now + 3));
        db.attach_share_to_candidate(second.share_id, journal.candidate_id);
        require(db.accept_candidate(journal.candidate_id, now + 4,
                                    bytes<32>(142), true),
                "round finalization fixture candidate was not accepted");
        require(scalar(round_path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NULL") == 1,
                "round finalized before its retained shares drained");
        const auto accepted_result = db.accept_share(
            accepted(first.share_id, now + 5, "700000000000"));
        require(accepted_result.accepted &&
                    accepted_result.persisted_share_id == first.share_id,
                "late retained share did not report its durable ID");
        const auto finalized_result = db.finalize_share(
            second.share_id,
            finalized("invalid_result", "verified", now + 6, "1"));
        require(finalized_result.finalized &&
                    finalized_result.persisted_share_id == second.share_id,
                "retained terminal share did not report its durable ID");
        require(scalar(round_path,
                       "SELECT count(*) FROM rounds WHERE state='closed' "
                       "AND effort_finalized_unix_us IS NOT NULL "
                       "AND finalized_effort_segment_count=1") == 1,
                "round effort did not freeze after retained shares drained");
        require(statement_rejected(
                    round_path,
                    "UPDATE rounds SET max_share_height=43 WHERE state='closed'"),
                "finalized round maximum remained mutable");
        require(statement_rejected(
                    round_path,
                    "UPDATE round_work_segments SET credited_difficulty_dec='1' "
                    "WHERE round_id=" + std::to_string(first.round_id)),
                "finalized round work remained mutable");
    }
    remove_db(round_path);

    const auto alias_path = path_for("alias-bound");
    remove_db(alias_path);
    {
        Database db(DatabaseOptions{.path = alias_path.string()});
        const Fixture value = fixture(db, now + 1'000'000, 150);
        std::int64_t oldest_alias = 0;
        std::int64_t newest_alias = 0;
        PersistedShareIdentity newest_identity;
        for (std::uint64_t sequence = 1; sequence <= 4097; ++sequence) {
            const std::int64_t alias = db.insert_share(
                share(value, sequence,
                      now + 1'000'000 +
                          static_cast<std::int64_t>(sequence)));
            const auto identity = db.ensure_share_persisted(
                alias, "security_evidence");
            if (sequence == 1) {
                oldest_alias = alias;
            }
            newest_alias = alias;
            newest_identity = identity;
        }
        require(db.ensure_share_persisted(
                    newest_alias, "security_evidence").share_id ==
                    newest_identity.share_id,
                "newest persisted alias did not resolve idempotently");
        require(database_error_contains(
                    [&] {
                        (void)db.ensure_share_persisted(
                            oldest_alias, "security_evidence");
                    },
                    "transient share does not exist"),
                "persisted alias cache exceeded its 4096-entry bound");
    }
    remove_db(alias_path);
}

void connection_churn_bound_test()
{
    const auto path = path_for("connection-churn");
    remove_db(path);
    {
        Database db(DatabaseOptions{.path = path.string()});
        const std::int64_t session = db.start_session(SessionStart{
            .public_id = bytes<16>(160),
            .started_unix_us = 600'000'000,
            .version = "connection-churn",
            .verifier_commit = std::nullopt,
        });
        std::int64_t prior_connection = 0;
        std::int64_t prior_worker = 0;
        for (std::uint64_t index = 1; index <= 1000; ++index) {
            const std::int64_t worker = db.upsert_worker(WorkerInsert{
                .login = "churn-" + std::to_string(index),
                .rigid = "r",
                .seen_unix_us = 600'000'000 +
                                static_cast<std::int64_t>(index),
            });
            const std::int64_t connection = db.insert_connection(
                ConnectionInsert{
                    .public_id = id_bytes<16>(index + 1000),
                    .session_id = session,
                    .worker_id = worker,
                    .peer_family = 2,
                    .peer_address = {
                        127,
                        0,
                        static_cast<std::uint8_t>((index >> 8U) & 0xffU),
                        static_cast<std::uint8_t>(index & 0xffU),
                    },
                    .peer_port = 3333,
                    .listen_address = "127.0.0.1:3333",
                    .agent = "churn",
                    .opened_unix_us = 601'000'000 +
                                      static_cast<std::int64_t>(index),
                });
            require(connection > prior_connection && worker > prior_worker,
                    "AUTOINCREMENT scope identity was reused during churn");
            prior_connection = connection;
            prior_worker = worker;
            require(db.close_connection(
                        connection,
                        602'000'000 + static_cast<std::int64_t>(index),
                        "churn"),
                    "churn connection did not close");
        }
        require(scalar(path, "SELECT count(*) FROM connections") == 0 &&
                    scalar(path, "SELECT count(*) FROM workers") == 0,
                "unreferenced connection churn grew context tables");
    }
    remove_db(path);
}

void database_path_and_ownership_lock_test()
{
    const auto target = path_for("symlink-target");
    const auto link = path_for("symlink-link");
    remove_db(target);
    remove_db(link);
    {
        Database initialize(DatabaseOptions{.path = target.string()});
    }
    require(::symlink(target.c_str(), link.c_str()) == 0,
            "could not create database symlink fixture");
    require(database_error_contains(
                [&] { Database db(DatabaseOptions{.path = link.string()}); },
                "open database ownership lock"),
            "database open followed a symlink");
    std::error_code ignored;
    std::filesystem::remove(link, ignored);
    remove_db(target);

    const auto path = path_for("single-owner");
    remove_db(path);
    {
        Database owner(DatabaseOptions{.path = path.string()});
        require(database_error_contains(
                    [&] {
                        Database competitor(
                            DatabaseOptions{.path = path.string()});
                    },
                    "already owned"),
                "second live instance acquired the database");
        const auto alias = path_for("single-owner-alias");
        std::filesystem::remove(alias, ignored);
        std::filesystem::create_hard_link(path, alias, ignored);
        require(!ignored, "could not create database hard-link fixture");
        require(database_error_contains(
                    [&] {
                        Database competitor(
                            DatabaseOptions{.path = alias.string()});
                    },
                    "already owned"),
                "hard-link alias bypassed database ownership");
        std::filesystem::remove(alias, ignored);
    }
    {
        Database successor(DatabaseOptions{.path = path.string()});
        require(successor.schema_version() == 3,
                "database ownership lock was not released");
    }
    remove_db(path);
}

void writer_scheduler_test()
{
    const auto invalid_path = path_for("invalid-writer-reserve");
    remove_db(invalid_path);
    require(database_error_contains(
                [&] {
                    Database invalid(DatabaseOptions{
                        .path = invalid_path.string(),
                        .max_writer_queue_items = UINT64_MAX,
                        .max_writer_queue_bytes = UINT64_MAX,
                        .writer_priority_reserve_items = UINT64_MAX - 1U,
                    });
                },
                "reserve"),
            "overflowing writer reserve was accepted");
    remove_db(invalid_path);

    const auto path = path_for("writer-scheduler");
    remove_db(path);
    constexpr std::int64_t now = 700'000'000;
    {
        Database db(DatabaseOptions{
            .path = path.string(),
            .busy_timeout_ms = 4000,
            .max_writer_queue_items = 2,
            .max_writer_queue_bytes = 1024,
            .writer_priority_reserve_items = 1,
        });
        const Fixture value = fixture(db, now, 170);
        const auto origin = db.ensure_share_persisted(
            db.insert_share(share_at_height(value, 1, 42, now + 1)),
            "candidate");
        const auto journal = db.journal_candidate(
            candidate(value, origin, 1, 171, 42, now + 2));

        sqlite3 *blocker = nullptr;
        require(sqlite3_open_v2(path.c_str(), &blocker,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                                nullptr) == SQLITE_OK,
                "could not open writer blocker");
        require(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr,
                             nullptr) == SQLITE_OK,
                "could not acquire writer blocker");

        std::exception_ptr first_error;
        std::exception_ptr ordinary_error;
        std::exception_ptr priority_error;
        std::exception_ptr backpressured_error;
        std::atomic<bool> first_entered{false};
        std::atomic<bool> backpressured_done{false};
        const auto insert_named_event = [&](std::int64_t created_unix_us,
                                            std::string type) {
            EventInsert event;
            event.session_id = value.session;
            event.created_unix_us = created_unix_us;
            event.type = std::move(type);
            return db.insert_event(event);
        };

        std::jthread first([&] {
            try {
                first_entered.store(true, std::memory_order_release);
                (void)insert_named_event(now + 10, "queue_blocker");
            }
            catch (...) {
                first_error = std::current_exception();
            }
        });
        while (!first_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        std::jthread ordinary([&] {
            try {
                (void)insert_named_event(now + 20, "ordinary_queued");
            }
            catch (...) {
                ordinary_error = std::current_exception();
            }
        });
        bool observed_ordinary = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto stats = db.writer_stats();
            if (stats.queued_items == 1 && stats.priority_items == 0) {
                observed_ordinary = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(observed_ordinary,
                "ordinary writer did not enter the queue");

        std::jthread priority([&] {
            try {
                (void)db.start_candidate_attempt(
                    journal.candidate_id, 1, 1100, now + 30);
            }
            catch (...) {
                priority_error = std::current_exception();
            }
        });
        bool observed_full = false;
        DatabaseWriterStats full;
        for (int attempt = 0; attempt < 100; ++attempt) {
            full = db.writer_stats();
            if (full.queued_items == 2 && full.queued_bytes == 1024 &&
                full.priority_items == 1) {
                observed_full = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::jthread backpressured([&] {
            try {
                (void)insert_named_event(now + 40,
                                         "ordinary_backpressured");
            }
            catch (...) {
                backpressured_error = std::current_exception();
            }
            backpressured_done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto capped = db.writer_stats();
        const bool held_back =
            !backpressured_done.load(std::memory_order_acquire);

        const bool released =
            sqlite3_exec(blocker, "COMMIT", nullptr, nullptr, nullptr) ==
            SQLITE_OK;
        sqlite3_close(blocker);
        first.join();
        ordinary.join();
        priority.join();
        backpressured.join();

        require(released, "could not release writer blocker");
        require(first_error == nullptr && ordinary_error == nullptr &&
                    priority_error == nullptr &&
                    backpressured_error == nullptr,
                "writer scheduler command failed");
        require(observed_full && capped.queued_items == 2 &&
                    capped.queued_bytes == 1024 &&
                    capped.priority_items == 1 && held_back,
                "writer priority reserve was not enforced");
        require(scalar(path,
                       "SELECT count(*) FROM events p,events o WHERE "
                       "p.type='candidate_attempt' AND "
                       "o.type='ordinary_queued' AND p.id<o.id") == 1,
                "priority command did not pass queued ordinary work");
        const auto empty = db.writer_stats();
        require(empty.queued_items == 0 && empty.queued_bytes == 0 &&
                    empty.priority_items == 0,
                "writer queue statistics did not drain");
    }
    remove_db(path);
}

void schema_v2_test() {
 for (const std::string version : {"1", "2"}) {
  auto p=path_for("schema-"+version); remove_db(p); { Database db(DatabaseOptions{.path=p.string()}); } execute(p,"UPDATE schema_meta SET value='"+version+"' WHERE key='schema_version'"); bool rejected=false; try { Database db(DatabaseOptions{.path=p.string()}); } catch(const DatabaseError &e) { const std::string message=e.what(); rejected=message.find("schema version "+version)!=std::string::npos&&message.find("requires a clean reset")!=std::string::npos&&message.find("WAL, and SHM")!=std::string::npos; } require(rejected,"v1/v2 clean reset rejection missing or imprecise"); remove_db(p);
 }
}
}

int main()
{
    try {
        selective_test();
        candidate_test();
        bounds_test();
        hundred_thousand_low_share_test();
        round_height_contamination_test();
        hashrate_scope_identity_and_pruning_test();
        atomic_worker_authentication_pruning_test();
        crash_recovery_pruning_test();
        closed_round_crash_finalization_test();
        candidate_recovery_and_blocknotify_test();
        candidate_rejection_and_reconciliation_test();
        candidate_race_and_round_boundary_test();
        round_finalization_and_alias_bound_test();
        connection_churn_bound_test();
        database_path_and_ownership_lock_test();
        writer_scheduler_test();
        schema_v2_test();
        std::cout << "database tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "database tests failed: " << error.what() << '\n';
        return 1;
    }
}
