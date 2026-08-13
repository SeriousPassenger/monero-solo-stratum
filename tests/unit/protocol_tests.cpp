#include "monero_solo/blocknotify.hpp"
#include "monero_solo/daemon.hpp"
#include "monero_solo/defense.hpp"
#include "monero_solo/duplicate_registry.hpp"
#include "monero_solo/event_stream.hpp"
#include "monero_solo/share_policy.hpp"
#include "monero_solo/stratum.hpp"
#include "monero_solo/util.hpp"
#include "monero_solo/zmq.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Predicate>
void eventually(Predicate predicate, const char *message,
                std::chrono::milliseconds timeout = 3000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error(message);
}

int connect_tcp(std::uint16_t port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(socket_fd >= 0, "could not create TCP test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "could not encode loopback");
    require(::connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)) == 0,
            "could not connect to Stratum listener");
    timeval timeout{3, 0};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return socket_fd;
}

void send_all(int socket_fd, std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = send(socket_fd, value.data() + offset,
                                   value.size() - offset, MSG_NOSIGNAL);
        require(count > 0, "test socket write failed");
        offset += static_cast<std::size_t>(count);
    }
}

std::string read_line(int socket_fd) {
    std::string result;
    for (;;) {
        char byte{};
        const ssize_t count = recv(socket_fd, &byte, 1, 0);
        require(count == 1, "test socket read failed");
        if (byte == '\n') return result;
        result.push_back(byte);
    }
}

std::uint16_t unused_tcp_port() {
    const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(descriptor >= 0, "could not create port probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "port probe bind failed");
    socklen_t length = sizeof(address);
    require(getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "port probe getsockname failed");
    const auto port = ntohs(address.sin_port);
    close(descriptor);
    return port;
}

void test_daemon_json_rpc_envelope() {
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(listener >= 0, "could not create daemon test listener");
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "daemon test listener bind failed");
    require(listen(listener, 2) == 0, "daemon test listener listen failed");
    socklen_t length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "daemon test listener getsockname failed");

    std::exception_ptr server_failure;
    std::jthread server([&] {
        try {
            for (unsigned request = 0; request < 2U; ++request) {
                const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                require(client >= 0, "daemon test accept failed");
                std::string headers;
                std::array<char, 1024> buffer{};
                while (headers.find("\r\n\r\n") == std::string::npos) {
                    const ssize_t count = recv(client, buffer.data(), buffer.size(), 0);
                    require(count > 0, "daemon test request read failed");
                    headers.append(buffer.data(), static_cast<std::size_t>(count));
                }
                const std::string body = request == 0U
                    ? "{\"id\":1,\"result\":{\"status\":\"OK\"}}"
                    : "{\"status\":\"OK\"}";
                const std::string response =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Connection: close\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body;
                send_all(client, response);
                close(client);
            }
        }
        catch (...) {
            server_failure = std::current_exception();
        }
    });

    monero_solo::DaemonRpcClient daemon(
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)),
        "", "", 1000, 4096);
    const auto rpc = daemon.json_rpc("get_info", Json::object());
    const auto endpoint = daemon.endpoint("/getheight", Json::object());

    server.join();
    close(listener);
    if (server_failure) std::rethrow_exception(server_failure);
    require(rpc.kind == monero_solo::RpcObservationKind::invalid_envelope,
            "JSON-RPC response without version was accepted");
    require(endpoint.valid(), "ID-less daemon endpoint incorrectly required JSON-RPC version");
}

void test_daemon_request_scheduler() {
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(listener >= 0, "could not create scheduler listener");
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
                listen(listener, 4) == 0,
            "scheduler listener setup failed");
    socklen_t length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "scheduler listener port lookup failed");

    std::atomic<unsigned> active{};
    std::atomic<unsigned> maximum{};
    std::exception_ptr server_failure;
    std::jthread server([&] {
        try {
            std::vector<std::jthread> handlers;
            for (unsigned index = 0; index < 2U; ++index) {
                const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                require(client >= 0, "scheduler accept failed");
                handlers.emplace_back([&, client] {
                    const unsigned now_active = active.fetch_add(1U) + 1U;
                    unsigned observed = maximum.load();
                    while (observed < now_active &&
                           !maximum.compare_exchange_weak(observed, now_active)) {
                    }
                    std::string request;
                    std::array<char, 1024> buffer{};
                    Json rpc;
                    for (;;) {
                        const ssize_t count = recv(client, buffer.data(), buffer.size(), 0);
                        require(count > 0, "scheduler request read failed");
                        request.append(buffer.data(), static_cast<std::size_t>(count));
                        const auto body = request.find("\r\n\r\n");
                        if (body != std::string::npos) {
                            rpc = Json::parse(request.substr(body + 4U), nullptr, false);
                            if (!rpc.is_discarded()) break;
                        }
                    }
                    std::this_thread::sleep_for(120ms);
                    const std::string body = Json{{"jsonrpc", "2.0"},
                                                  {"id", rpc.at("id")},
                                                  {"result", {{"status", "OK"}}}}.dump();
                    send_all(client,
                             "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" + body);
                    close(client);
                    --active;
                });
            }
            handlers.clear();
        }
        catch (...) {
            server_failure = std::current_exception();
        }
    });

    monero_solo::DaemonRpcClient daemon(
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)),
        "", "", 2000, 4096, 1, 3);
    std::array<monero_solo::RpcObservation, 2> results;
    std::jthread first([&] { results[0] = daemon.get_info(); });
    std::jthread second([&] { results[1] = daemon.get_info(); });
    first.join();
    second.join();
    server.join();
    close(listener);
    if (server_failure) std::rethrow_exception(server_failure);
    require(results[0].valid() && results[1].valid(),
            "scheduled daemon request failed");
    require(maximum == 1U,
            "daemon max_concurrent_requests did not serialize on-wire calls");
}

void test_reconciliation_candidate_identity() {
    const std::string expected_block(64, 'a');
    const std::string other_block(64, 'b');
    const std::string miner_tx(64, 'c');
    monero_solo::RpcObservation observation{
        .kind = monero_solo::RpcObservationKind::valid,
        .request_id = 7,
        .http_status = 200,
        .document = {
            {"jsonrpc", "2.0"},
            {"id", 7},
            {"result", {
                {"status", "OK"},
                {"block_header", {
                    {"orphan_status", false},
                    {"height", 123U},
                    {"hash", expected_block},
                }},
                {"miner_tx_hash", miner_tx},
                {"blob", "00"},
            }},
        },
    };
    const auto exact = monero_solo::DaemonRpcClient::classify_reconciliation(
        observation, 123U, miner_tx, expected_block);
    require(exact.positive,
            "exact reconciliation evidence was not accepted");

    observation.document["result"]["block_header"]["hash"] = other_block;
    const auto same_coinbase_other_block =
        monero_solo::DaemonRpcClient::classify_reconciliation(
            observation, 123U, miner_tx, expected_block);
    require(!same_coinbase_other_block.positive,
            "height reconciliation accepted another nonce candidate with the same coinbase");

    const auto mismatched_hash_lookup =
        monero_solo::DaemonRpcClient::classify_reconciliation(
            observation, 123U, miner_tx, expected_block, expected_block);
    require(!mismatched_hash_lookup.positive,
            "hash reconciliation accepted a block other than the requested locator");
}

void test_candidate_duplicate_capacity_policy() {
    using monero_solo::detail::DuplicateTerminal;
    using monero_solo::detail::continue_verification_after_claimed_capacity;
    using monero_solo::detail::verified_post_hash_plan;

    require(!continue_verification_after_claimed_capacity(true, false),
            "ordinary claimed capacity did not fail closed before verification");
    require(continue_verification_after_claimed_capacity(true, true),
            "claimed candidate capacity prevented verifier rescue/evidence");

    const auto rescue = verified_post_hash_plan(true, false, false, true);
    require(rescue.journal_computed_candidate &&
                rescue.duplicate_terminal == DuplicateTerminal::capacity,
            "computed network candidate was not journaled before capacity finalization");
    const auto precedence = verified_post_hash_plan(true, true, true, true);
    require(precedence.journal_computed_candidate &&
                precedence.duplicate_terminal == DuplicateTerminal::duplicate,
            "authoritative duplicate did not outrank duplicate-capacity failure");
}

monero_solo::DuplicateKey key(std::uint8_t value) {
    monero_solo::DuplicateKey result{};
    result.fill(value);
    return result;
}

void test_duplicate_registry() {
    monero_solo::DuplicateRegistry registry(4, 3);
    monero_solo::DuplicateToken first;
    require(registry.reserve(key(1), 10, 100, first) ==
                monero_solo::DuplicateReserveResult::reserved,
            "first duplicate reservation failed");
    monero_solo::DuplicateToken repeated;
    require(registry.reserve(key(1), 11, 101, repeated) ==
                monero_solo::DuplicateReserveResult::duplicate,
            "duplicate was not process-global");
    require(registry.reserve(key(2), 10, 100, repeated) ==
                monero_solo::DuplicateReserveResult::reserved,
            "distinct identity collided");
    registry.retain_height(10, 100);
    require(registry.retire_height(10, 100).empty(),
            "retired referenced bucket collected too early");
    const auto retired = registry.release_height(10, 100);
    require(retired.size() == 2 && retired[0].generation != retired[1].generation,
            "bucket collection did not return its durable generation tokens");
    require((retired[0].key == key(1) && retired[1].key == key(2)) ||
                (retired[0].key == key(2) && retired[1].key == key(1)),
            "bucket collection returned the wrong duplicate keys");
    require(registry.size() == 0, "retired duplicate bucket did not collect");
    monero_solo::DuplicateToken newer;
    require(registry.reserve(key(1), 10, 101, newer) ==
                monero_solo::DuplicateReserveResult::reserved,
            "identity could not be reserved in a new generation");
    require(!registry.release(first), "stale token erased a newer reservation");
    require(registry.release(newer), "current generation token did not release");

    std::atomic<unsigned> winners{};
    std::vector<std::jthread> threads;
    for (unsigned index = 0; index < 16; ++index) {
        threads.emplace_back([&] {
            monero_solo::DuplicateToken token;
            if (registry.reserve(key(3), 12, 200, token) ==
                monero_solo::DuplicateReserveResult::reserved) ++winners;
        });
    }
    threads.clear();
    require(winners == 1, "concurrent duplicate reservation had multiple winners");
}

void test_defense() {
    const auto v4 = monero_solo::PeerAddress::parse("192.0.2.9");
    const auto mapped = monero_solo::PeerAddress::parse("::ffff:192.0.2.9");
    const auto v6a = monero_solo::PeerAddress::parse("2001:db8::1");
    const auto v6b = monero_solo::PeerAddress::parse("2001:0db8:0:0:0:0:0:1");
    require(v4 == mapped, "IPv4-mapped IPv6 did not normalize");
    require(v6a == v6b && v6a.text() == "2001:db8::1",
            "IPv6 equivalent forms did not canonicalize");

    monero_solo::DefensePolicyConfig config;
    config.request_burst = 2;
    config.request_rate_per_second = 1;
    config.malformed_limit = 3;
    config.candidate_burst = 10;
    config.candidate_inflight_per_ip = 1;
    config.candidate_global_inflight = 1;
    std::atomic<unsigned> bans{};
    monero_solo::DefenseEngine defense(config, [&](const auto &record) {
        require(record.evidence_start <= record.evidence_end,
                "ban evidence window was inverted");
        ++bans;
    });
    require(defense.admit_request(v4) && defense.admit_request(v4) &&
                !defense.admit_request(v4),
            "request token burst was not exact");
    require(defense.admit_candidate(v4) && !defense.admit_candidate(v6a),
            "global candidate inflight cap failed");
    defense.candidate_finished(v4);
    require(defense.candidate_global_inflight() == 0,
            "candidate completion did not release cap");
    defense.record(v4, monero_solo::AbuseKind::malformed);
    defense.record(v4, monero_solo::AbuseKind::malformed);
    require(!defense.banned(v4), "ban threshold fired early");
    defense.record(v4, monero_solo::AbuseKind::malformed);
    require(defense.banned(mapped) && bans == 1, "exact ban threshold failed");

    // Sustained hammer detection is distinct from token denial. Exercise two
    // complete adjacent one-second buckets at exactly 2x a 1/s request rate.
    monero_solo::DefensePolicyConfig hammer_config;
    hammer_config.request_rate_per_second = 1;
    hammer_config.request_burst = 1;
    hammer_config.hammer_rate_multiplier = 2;
    hammer_config.hammer_sustain_seconds = 2;
    std::atomic<unsigned> hammer_bans{};
    monero_solo::DefenseEngine hammer(hammer_config,
        [&](const monero_solo::BanRecord &record) {
            require(record.reason == monero_solo::AbuseKind::rate_hammer,
                    "hammer ban used wrong reason");
            ++hammer_bans;
        });
    const auto peer = monero_solo::PeerAddress::parse("198.51.100.12");
    const auto bucket = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto advance_bucket = [&] {
        const auto original = bucket();
        eventually([&] { return bucket() != original; },
                   "steady-clock second did not advance", 1500ms);
    };
    advance_bucket();
    require(hammer.admit_request(peer), "first hammer request unexpectedly denied");
    require(!hammer.admit_request(peer), "token bucket did not reject isolated burst");
    require(!hammer.banned(peer), "one burst incorrectly caused a ban");
    advance_bucket();
    (void)hammer.admit_request(peer); // closes the first full bucket
    require(!hammer.banned(peer), "hammer ban fired before sustain duration");
    (void)hammer.admit_request(peer);
    advance_bucket();
    (void)hammer.admit_request(peer); // closes the second full bucket
    require(hammer.banned(peer) && hammer_bans == 1,
            "exact sustained hammer threshold did not ban");

    // Legitimately admitted candidate traffic does not count as cap-evasion
    // hammering. Only denied attempts feed the candidate hammer lane.
    monero_solo::DefensePolicyConfig candidate_config;
    candidate_config.candidate_rate_per_minute = 600;
    candidate_config.candidate_burst = 100;
    candidate_config.candidate_inflight_per_ip = 1;
    candidate_config.candidate_global_inflight = 1;
    candidate_config.hammer_rate_multiplier = 2;
    candidate_config.hammer_sustain_seconds = 1;
    std::atomic<unsigned> candidate_bans{};
    monero_solo::DefenseEngine candidate_defense(
        candidate_config, [&](const monero_solo::BanRecord &) { ++candidate_bans; });
    const auto candidate_peer = monero_solo::PeerAddress::parse("203.0.113.22");
    for (unsigned index = 0; index < 40U; ++index) {
        require(candidate_defense.admit_candidate(candidate_peer),
                "legitimate candidate admission failed");
        candidate_defense.candidate_finished(candidate_peer);
    }
    advance_bucket();
    require(candidate_defense.admit_candidate(candidate_peer),
            "candidate admission failed after bucket rollover");
    candidate_defense.candidate_finished(candidate_peer);
    require(!candidate_defense.banned(candidate_peer) && candidate_bans == 0,
            "admitted candidate traffic was misclassified as hammering");

    monero_solo::DefensePolicyConfig bounded_config;
    bounded_config.max_peer_states = 4;
    bounded_config.max_active_bans = 3;
    bounded_config.connection_burst = 100;
    bounded_config.candidate_burst = 100;
    bounded_config.candidate_inflight_per_ip = 4;
    bounded_config.candidate_global_inflight = 4;
    bounded_config.malformed_limit = 1;
    std::atomic<unsigned> bounded_bans{};
    monero_solo::DefenseEngine bounded(
        bounded_config,
        [&](const monero_solo::BanRecord &) { ++bounded_bans; });
    for (unsigned index = 1; index <= 1000U; ++index) {
        const auto churn = monero_solo::PeerAddress::parse(
            "2001:db8::" + std::to_string(index));
        require(bounded.admit_connection(churn),
                "bounded peer store denied evictable churn");
        require(bounded.peer_state_count() <= bounded_config.max_peer_states,
                "peer-state store exceeded its hard bound");
    }
    const auto protected_peer =
        monero_solo::PeerAddress::parse("2001:db8:1::1");
    require(bounded.admit_candidate(protected_peer),
            "bounded peer store could not admit protected candidate");
    for (unsigned index = 1; index <= 100U; ++index) {
        const auto churn = monero_solo::PeerAddress::parse(
            "2001:db8:2::" + std::to_string(index));
        (void)bounded.admit_connection(churn);
    }
    require(bounded.candidate_peer_inflight(protected_peer) == 1,
            "LRU churn evicted an in-flight candidate peer");
    bounded.candidate_finished(protected_peer);
    require(bounded.candidate_global_inflight() == 0,
            "bounded peer store lost candidate accounting");
    for (unsigned index = 1; index <= 20U; ++index) {
        const auto churn = monero_solo::PeerAddress::parse(
            "2001:db8:3::" + std::to_string(index));
        bounded.record(churn, monero_solo::AbuseKind::malformed);
        require(bounded.active_ban_count() <= bounded_config.max_active_bans,
                "active-ban store exceeded its hard bound");
    }
    require(bounded_bans == 4 && bounded.active_ban_count() == 3 &&
                bounded.banned(monero_solo::PeerAddress::parse("198.51.100.250")),
            "ban-cap overflow did not enter consistent fail-closed mode");

    monero_solo::DefensePolicyConfig protected_config;
    protected_config.max_peer_states = 2;
    protected_config.candidate_rate_per_minute = 0;
    protected_config.candidate_burst = 0;
    protected_config.candidate_inflight_per_ip = 0;
    protected_config.candidate_global_inflight = 0;
    monero_solo::DefenseEngine all_protected(protected_config);
    const auto protected_one = monero_solo::PeerAddress::parse("2001:db8:4::1");
    const auto protected_two = monero_solo::PeerAddress::parse("2001:db8:4::2");
    const auto protected_three = monero_solo::PeerAddress::parse("2001:db8:4::3");
    require(all_protected.admit_candidate(protected_one) &&
                all_protected.admit_candidate(protected_two) &&
                !all_protected.admit_connection(protected_three) &&
                all_protected.peer_state_count() == 2,
            "all-protected peer capacity did not fail closed");
    all_protected.candidate_finished(protected_one);
    all_protected.candidate_finished(protected_two);
}

void test_stratum() {
    const std::uint16_t port = unused_tcp_port();
    std::atomic<unsigned> submissions{};
    std::atomic<unsigned> opened{};
    std::atomic<unsigned> authenticated{};
    std::atomic<unsigned> stopped{};
    monero_solo::StratumServerConfig config;
    config.listen = {"127.0.0.1:" + std::to_string(port)};
    config.access_password = "secret";
    config.difficulty_floor = 2;
    config.login_timeout_ms = 3000;
    config.idle_timeout_ms = 5000;
    monero_solo::StratumServer server(
        config,
        [](const monero_solo::MinerConnection &) -> std::optional<monero_solo::StratumJob> {
            return monero_solo::StratumJob{
                std::string(152, 'a'), std::string(32, 'b'), "ffffffffffffff7f",
                std::string(64, 'c'), 42, {}, "1000000000"};
        },
        [&](const monero_solo::StratumSubmission &submission) {
            require(submission.request_sequence == submissions.fetch_add(1) + 1,
                    "submission sequence was not monotonic");
            return monero_solo::ShareResponse{monero_solo::ShareDisposition::accepted, "ok"};
        }, nullptr,
        [&](const monero_solo::MinerConnection &, std::string_view event) {
            if (event == "opened") ++opened;
            else if (event == "authenticated") ++authenticated;
            else if (event == "server stopping") ++stopped;
        });
    server.start();
    int malformed_login = connect_tcp(port);
    send_all(malformed_login,
             "{\"id\":99,\"method\":\"login\",\"params\":{"
             "\"login\":\"rig\",\"pass\":\"secret\",\"agent\":1}}\n");
    require(Json::parse(read_line(malformed_login))["error"]["message"] ==
                "Invalid login",
            "non-string optional login field escaped request validation");
    close(malformed_login);
    int client = connect_tcp(port);
    send_all(client, "{\"id\":0,\"method\":\"keepalived\",\"params\":{\"id\":\"x\"}}\r\n");
    require(Json::parse(read_line(client))["error"]["message"] == "Unauthenticated",
            "prelogin method was not rejected");

    const std::string login =
        "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{"
        "\"login\":\"rig\",\"pass\":\"secret\",\"agent\":\"XMRig/6\","
        "\"rigid\":\"r1\",\"algo\":[\"rx/0\"]}}\n";
    send_all(client, login.substr(0, 17));
    send_all(client, login.substr(17) + login);
    const Json logged_in = Json::parse(read_line(client));
    const std::string connection_id = logged_in["result"]["id"];
    const std::string job_id = logged_in["result"]["job"]["job_id"];
    require(connection_id.size() == 32 && logged_in["result"]["job"]["height"] == 42 &&
                logged_in["result"]["job"]["target"] == "ffffffffffffff7f" &&
                logged_in["result"]["extensions"] == Json::array({"algo", "keepalive"}),
            "simple-mode login response schema was wrong");
    require(Json::parse(read_line(client))["error"]["message"] == "Already authenticated",
            "coalesced second login was not framed independently");

    const std::string params = "{\"id\":\"" + connection_id +
        "\",\"job_id\":\"" + job_id +
        "\",\"nonce\":\"01020304\",\"result\":\"" +
        std::string(64, 'd') + "\",\"algo\":\"rx/0\"}";
    const std::string submit = "{\"id\":\"same\",\"method\":\"submit\",\"params\":" + params + "}\n";
    send_all(client, submit);
    require(Json::parse(read_line(client))["result"]["status"] == "OK",
            "ordinary submit did not receive OK");
    // Reuse is legal after the previous complete response frame was queued.
    send_all(client, submit);
    require(Json::parse(read_line(client))["result"]["status"] == "OK",
            "request ID was not reusable after response enqueue");

    const std::string keepalive = "{\"id\":3,\"method\":\"keepalived\",\"params\":{\"id\":\"" +
                                  connection_id + "\"}}\n";
    send_all(client, keepalive);
    const Json kept = Json::parse(read_line(client));
    require(kept["id"] == 3 && kept["result"]["status"] == "KEEPALIVED",
            "keepalive response was wrong");
    server.stop();
    require(opened == 2 && authenticated == 1 && stopped == 1,
            "Stratum shutdown did not close and report the live connection");
    close(client);
}

void test_nicehash_compact_target_simple_mode() {
    const std::uint16_t port = unused_tcp_port();
    monero_solo::StratumServerConfig config;
    config.listen = {"127.0.0.1:" + std::to_string(port)};
    config.difficulty_floor = 262144;
    config.login_timeout_ms = 3000;
    config.idle_timeout_ms = 5000;
    monero_solo::StratumServer server(
        config,
        [](const monero_solo::MinerConnection &connection)
            -> std::optional<monero_solo::StratumJob> {
            require(connection.agent == "rental-client nIcEhAsH/1.0.0",
                    "NiceHash agent was not preserved for job negotiation");
            require(connection.assigned_difficulty == 262160,
                    "NiceHash difficulty was not aligned with compact target");
            return monero_solo::StratumJob{
                std::string(152, 'a'), std::string(32, 'b'),
                "f0ff0300ff3f0000", std::string(64, 'c'), 42, {},
                "1000000000"};
        },
        [](const monero_solo::StratumSubmission &submission) {
            require(submission.connection.assigned_difficulty == 262160,
                    "NiceHash submit lost effective assigned difficulty");
            require(std::all_of(submission.nonce.begin(),
                                submission.nonce.end(),
                                [](std::uint8_t byte) {
                                    return byte == 0xffU;
                                }),
                    "NiceHash simple mode reserved or rewrote a nonce byte");
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::accepted, "ok"};
        });
    server.start();

    const int client = connect_tcp(port);
    send_all(client,
             "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\","
             "\"params\":{\"login\":\"rental\",\"pass\":\"x\","
             "\"agent\":\"rental-client nIcEhAsH/1.0.0\"}}\n");
    const Json login = Json::parse(read_line(client));
    require(login["result"]["job"]["blob"] == std::string(152, 'a') &&
                login["result"]["job"]["target"] == "ff3f0000" &&
                login["result"]["extensions"] ==
                    Json::array({"algo", "keepalive"}),
            "NiceHash login job did not use compact-target simple mode");

    const std::string connection_id = login["result"]["id"];
    const std::string job_id = login["result"]["job"]["job_id"];
    const std::string submit =
        "{\"id\":2,\"jsonrpc\":\"2.0\",\"method\":\"submit\","
        "\"params\":{\"id\":\"" + connection_id +
        "\",\"job_id\":\"" + job_id +
        "\",\"nonce\":\"ffffffff\",\"result\":\"" +
        std::string(64, 'd') + "\",\"algo\":\"rx/0\"}}\n";
    send_all(client, submit);
    require(Json::parse(read_line(client))["result"]["status"] == "OK",
            "NiceHash simple-mode nonce was not accepted intact");

    server.refresh_jobs();
    const Json refreshed = Json::parse(read_line(client));
    require(refreshed["method"] == "job" &&
                refreshed["params"]["target"] == "ff3f0000",
            "NiceHash refresh did not retain compact target encoding");

    server.stop();
    close(client);
}

void test_nicehash_unrepresentable_difficulty_is_explicit()
{
    const std::uint16_t port = unused_tcp_port();
    monero_solo::StratumServerConfig config;
    config.listen = {"127.0.0.1:" + std::to_string(port)};
    config.difficulty_floor =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
        1U;
    config.login_timeout_ms = 3000;
    config.idle_timeout_ms = 5000;
    std::atomic<unsigned> provider_calls{};
    monero_solo::StratumServer server(
        config,
        [&](const monero_solo::MinerConnection &)
            -> std::optional<monero_solo::StratumJob> {
            ++provider_calls;
            return std::nullopt;
        },
        [](const monero_solo::StratumSubmission &) {
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::server_busy, "unexpected"};
        });
    server.start();

    const int client = connect_tcp(port);
    send_all(client,
             "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\","
             "\"params\":{\"login\":\"rental\",\"pass\":\"x\","
             "\"agent\":\"NiceHash/1.0.0\"}}\n");
    const Json login = Json::parse(read_line(client));
    require(login["result"].is_null() &&
                login["error"]["message"] ==
                    "NiceHash difficulty is not representable" &&
                provider_calls.load() == 0U,
            "unrepresentable NiceHash difficulty queued invalid work");
    server.stop();
    close(client);
}

void test_stratum_fatal_event_supervision() {
    const std::uint16_t port = unused_tcp_port();
    std::atomic<unsigned> provider_calls{};
    std::atomic<unsigned> fatal_reports{};
    monero_solo::StratumServerConfig config;
    config.listen = {"127.0.0.1:" + std::to_string(port)};
    config.difficulty_floor = 2;
    monero_solo::StratumServer server(
        config,
        [&](const monero_solo::MinerConnection &)
            -> std::optional<monero_solo::StratumJob> {
            if (provider_calls.fetch_add(1U, std::memory_order_relaxed) != 0U) {
                throw std::runtime_error("forced job refresh failure");
            }
            return monero_solo::StratumJob{
                std::string(152, 'a'), std::string(32, 'b'),
                "ffffffffffffff7f", std::string(64, 'c'), 42, {},
                "1000000000"};
        },
        [](const monero_solo::StratumSubmission &) {
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::accepted, "ok"};
        },
        nullptr, {}, {},
        [&] { fatal_reports.fetch_add(1U, std::memory_order_release); });
    server.start();
    const int client = connect_tcp(port);
    send_all(client,
             "{\"id\":1,\"method\":\"login\",\"params\":{"
             "\"login\":\"rig\",\"pass\":\"\",\"algo\":\"rx/0\"}}\n");
    require(Json::parse(read_line(client))["result"]["status"] == "OK",
            "fatal-supervision fixture did not authenticate");
    server.refresh_jobs();
    eventually([&] {
        return !server.running() &&
            fatal_reports.load(std::memory_order_acquire) == 1U;
    }, "fatal Stratum event-loop failure was not reported");
    server.stop();
    close(client);
}

void test_stratum_submit_admission() {
    struct AdmissionLease {
        std::atomic<unsigned> *destroyed{};
        explicit AdmissionLease(std::atomic<unsigned> *counter)
            : destroyed(counter) {}
        AdmissionLease(const AdmissionLease &) = delete;
        AdmissionLease &operator=(const AdmissionLease &) = delete;
        ~AdmissionLease() { destroyed->fetch_add(1, std::memory_order_relaxed); }
    };

    // Ordinary verifier waits cannot consume the dedicated claimed-candidate
    // pool, and one candidate continuation cannot serialize the next. The same
    // exercise verifies opaque lease lifetime and a live (rather than
    // admission-time copied) stale height.
    const std::uint16_t lane_port = unused_tcp_port();
    std::atomic<std::uint64_t> job_height{42};
    std::atomic<unsigned> ordinary_started{};
    std::atomic<unsigned> candidate_started{};
    std::atomic<unsigned> leases_destroyed{};
    std::atomic<bool> lease_missing{};
    std::atomic<bool> handler_timed_out{};
    std::atomic<bool> second_candidate_started{};
    std::atomic<std::uint64_t> second_candidate_snapshot_height{};
    std::atomic<std::uint64_t> second_candidate_live_height{};
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release_ordinary = false;
    bool release_candidates = false;
    std::mutex candidate_order_mutex;
    std::vector<std::string> candidate_order;

    monero_solo::StratumServerConfig lane_config;
    lane_config.listen = {"127.0.0.1:" + std::to_string(lane_port)};
    lane_config.difficulty_floor = 2;
    lane_config.submit_workers = 2;
    lane_config.candidate_submit_workers = 2;
    lane_config.max_pending_submits = 8;
    lane_config.candidate_submit_reserve = 2;
    lane_config.max_pending_submits_per_connection = 6;
    monero_solo::StratumServer lane_server(
        lane_config,
        [&](const monero_solo::MinerConnection &)
            -> std::optional<monero_solo::StratumJob> {
            return monero_solo::StratumJob{
                std::string(152, 'a'), std::string(32, 'b'),
                "ffffffffffffff7f", std::string(64, 'c'),
                job_height.load(std::memory_order_acquire), {}, "1000000000"};
        },
        [&](const monero_solo::StratumSubmission &submission) {
            if (!submission.job_lease) {
                lease_missing.store(true, std::memory_order_release);
            }
            const bool candidate = std::all_of(
                submission.claimed_hash.begin(), submission.claimed_hash.end(),
                [](std::uint8_t byte) { return byte == 0U; });
            const std::string request_id =
                std::get<std::string>(submission.request_id);
            if (!candidate) {
                ordinary_started.fetch_add(1, std::memory_order_release);
                std::unique_lock lock(gate_mutex);
                if (!gate.wait_for(lock, 3s, [&] { return release_ordinary; })) {
                    handler_timed_out.store(true, std::memory_order_release);
                }
            }
            else {
                candidate_started.fetch_add(1U, std::memory_order_release);
                {
                    std::lock_guard lock(candidate_order_mutex);
                    candidate_order.push_back(request_id);
                }
                if (request_id == "candidate-2") {
                    second_candidate_snapshot_height.store(
                        submission.connection.last_sent_height,
                        std::memory_order_release);
                    second_candidate_started.store(true, std::memory_order_release);
                }
                std::unique_lock lock(gate_mutex);
                if (!gate.wait_for(lock, 3s, [&] { return release_candidates; })) {
                    handler_timed_out.store(true, std::memory_order_release);
                }
                if (request_id == "candidate-2") {
                    second_candidate_live_height.store(
                        submission.latest_queued_height(),
                        std::memory_order_release);
                }
            }
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::accepted, "ok"};
        },
        nullptr, {},
        [&](const monero_solo::StratumSubmission &submission) {
            return monero_solo::StratumAdmission{
                std::make_shared<AdmissionLease>(&leases_destroyed),
                submission.job_id == std::string(32, 'b')
                    ? "1000000000"
                    : std::string{}};
        });
    lane_server.start();
    const int lane_client = connect_tcp(lane_port);
    send_all(lane_client,
             "{\"id\":1,\"method\":\"login\",\"params\":{"
             "\"login\":\"rig\",\"pass\":\"\",\"algo\":\"rx/0\"}}\n");
    const Json lane_login = Json::parse(read_line(lane_client));
    const std::string lane_connection_id = lane_login["result"]["id"];
    const std::string lane_job_id = lane_login["result"]["job"]["job_id"];
    auto lane_submit = [&](std::string_view id, std::string_view nonce,
                           char result_digit) {
        return "{\"id\":\"" + std::string(id) +
            "\",\"method\":\"submit\",\"params\":{\"id\":\"" +
            lane_connection_id + "\",\"job_id\":\"" + lane_job_id +
            "\",\"nonce\":\"" + std::string(nonce) +
            "\",\"result\":\"" + std::string(64, result_digit) +
            "\",\"algo\":\"rx/0\"}}\n";
    };
    send_all(lane_client,
             lane_submit("ordinary-1", "01000000", 'f') +
             lane_submit("ordinary-2", "02000000", 'f'));
    eventually([&] {
        return ordinary_started.load(std::memory_order_acquire) == 2U;
    }, "ordinary submit workers did not enter their handlers");
    send_all(lane_client,
             lane_submit("candidate-1", "03000000", '0') +
             lane_submit("candidate-2", "04000000", '0'));
    eventually([&] {
        return candidate_started.load(std::memory_order_acquire) == 2U &&
               second_candidate_started.load(std::memory_order_acquire);
    }, "claimed candidate pool did not bypass occupied ordinary/RandomX workers");

    job_height.store(43, std::memory_order_release);
    lane_server.refresh_jobs();
    const Json refreshed = Json::parse(read_line(lane_client));
    require(refreshed["method"] == "job" &&
                refreshed["params"]["height"] == 43,
            "test job refresh was not successfully queued");
    {
        std::lock_guard lock(gate_mutex);
        release_candidates = true;
    }
    gate.notify_all();
    const Json candidate_response_one = Json::parse(read_line(lane_client));
    const Json candidate_response_two = Json::parse(read_line(lane_client));
    require(candidate_response_one["result"]["status"] == "OK" &&
                candidate_response_two["result"]["status"] == "OK" &&
                std::set<std::string>{
                    candidate_response_one["id"].get<std::string>(),
                    candidate_response_two["id"].get<std::string>()} ==
                    std::set<std::string>{"candidate-1", "candidate-2"},
            "claimed candidate responses were incomplete");
    {
        std::lock_guard lock(candidate_order_mutex);
        require(std::set<std::string>(candidate_order.begin(),
                                     candidate_order.end()) ==
                    std::set<std::string>{"candidate-1", "candidate-2"},
                "candidate pool did not execute both queued candidates");
    }
    require(second_candidate_snapshot_height.load(std::memory_order_acquire) == 42 &&
                second_candidate_live_height.load(std::memory_order_acquire) == 43,
            "submission did not expose the latest successfully queued height");
    {
        std::lock_guard lock(gate_mutex);
        release_ordinary = true;
    }
    gate.notify_all();
    const Json ordinary_response_one = Json::parse(read_line(lane_client));
    const Json ordinary_response_two = Json::parse(read_line(lane_client));
    const std::set<std::string> ordinary_ids{
        ordinary_response_one["id"].get<std::string>(),
        ordinary_response_two["id"].get<std::string>()};
    require(ordinary_ids == std::set<std::string>{"ordinary-1", "ordinary-2"},
            "ordinary lane responses were incomplete");
    eventually([&] {
        return leases_destroyed.load(std::memory_order_acquire) == 4U;
    }, "admission leases were not held exactly through submit handling");
    require(!lease_missing.load(std::memory_order_acquire) &&
                !handler_timed_out.load(std::memory_order_acquire),
            "submit admission lease/gating failed");
    lane_server.stop();
    close(lane_client);

    // The per-connection bound includes running and queued submissions.  A
    // rejected request neither consumes the durable request sequence nor the
    // ordinary FIFO slot.
    const std::uint16_t bound_port = unused_tcp_port();
    std::atomic<unsigned> bound_started{};
    std::mutex bound_mutex;
    std::condition_variable bound_gate;
    bool release_bound = false;
    std::vector<std::uint64_t> admitted_sequences;
    monero_solo::StratumServerConfig bound_config;
    bound_config.listen = {"127.0.0.1:" + std::to_string(bound_port)};
    bound_config.difficulty_floor = 2;
    bound_config.submit_workers = 1;
    bound_config.max_pending_submits = 8;
    bound_config.candidate_submit_reserve = 0;
    bound_config.max_pending_submits_per_connection = 2;
    monero_solo::StratumServer bound_server(
        bound_config,
        [](const monero_solo::MinerConnection &)
            -> std::optional<monero_solo::StratumJob> {
            return monero_solo::StratumJob{
                std::string(152, 'a'), std::string(32, 'd'),
                "ffffffffffffff7f", std::string(64, 'e'), 50, {},
                "1000000000"};
        },
        [&](const monero_solo::StratumSubmission &submission) {
            {
                std::lock_guard lock(bound_mutex);
                admitted_sequences.push_back(submission.request_sequence);
            }
            bound_started.fetch_add(1, std::memory_order_release);
            std::unique_lock lock(bound_mutex);
            (void)bound_gate.wait_for(lock, 3s, [&] { return release_bound; });
            return monero_solo::ShareResponse{
                monero_solo::ShareDisposition::accepted, "ok"};
        });
    bound_server.start();
    const int bound_client = connect_tcp(bound_port);
    send_all(bound_client,
             "{\"id\":1,\"method\":\"login\",\"params\":{"
             "\"login\":\"rig\",\"pass\":\"\",\"algo\":\"rx/0\"}}\n");
    const Json bound_login = Json::parse(read_line(bound_client));
    const std::string bound_connection_id = bound_login["result"]["id"];
    const std::string bound_job_id = bound_login["result"]["job"]["job_id"];
    auto bound_submit = [&](std::string_view id, std::string_view nonce) {
        return "{\"id\":\"" + std::string(id) +
            "\",\"method\":\"submit\",\"params\":{\"id\":\"" +
            bound_connection_id + "\",\"job_id\":\"" + bound_job_id +
            "\",\"nonce\":\"" + std::string(nonce) +
            "\",\"result\":\"" + std::string(64, 'f') +
            "\",\"algo\":\"rx/0\"}}\n";
    };
    send_all(bound_client, bound_submit("bound-1", "01000000"));
    eventually([&] {
        return bound_started.load(std::memory_order_acquire) == 1U;
    }, "bounded submit handler did not start");
    send_all(bound_client,
             bound_submit("bound-2", "02000000") +
             bound_submit("bound-3", "03000000"));
    const Json rejected = Json::parse(read_line(bound_client));
    require(rejected["id"] == "bound-3" &&
                rejected["error"]["message"] == "Server busy",
            "per-connection pending-submit bound was not enforced");
    {
        std::lock_guard lock(bound_mutex);
        release_bound = true;
    }
    bound_gate.notify_all();
    require(Json::parse(read_line(bound_client))["id"] == "bound-1" &&
                Json::parse(read_line(bound_client))["id"] == "bound-2",
            "bounded ordinary FIFO did not drain in order");
    send_all(bound_client, bound_submit("bound-4", "04000000"));
    require(Json::parse(read_line(bound_client))["id"] == "bound-4",
            "connection did not regain submit capacity after completion");
    {
        std::lock_guard lock(bound_mutex);
        require(admitted_sequences == std::vector<std::uint64_t>{1, 2, 3},
                "rejected submit consumed the per-connection request sequence");
    }
    bound_server.stop();
    close(bound_client);
}

std::filesystem::path unique_path(std::string_view suffix) {
    return std::filesystem::temp_directory_path() /
           ("mss-protocol-" + std::to_string(getpid()) + "-" + std::string(suffix));
}

int connect_unix(const std::filesystem::path &path) {
    const int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(descriptor >= 0, "could not create Unix socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string encoded = path.string();
    require(encoded.size() < sizeof(address.sun_path), "Unix test path too long");
    std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);
    require(connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "could not connect event stream");
    return descriptor;
}

void test_event_stream() {
    const auto socket_path = unique_path("events.sock");
    monero_solo::EventStream stream(
        {socket_path.string(), 0600, 1, 4096},
        "0123456789abcdef0123456789abcdef");
    try {
        stream.start(7);
    } catch (const std::runtime_error &) {
        if (errno == EPERM || errno == EACCES) {
            std::cout << "event stream test skipped: AF_UNIX denied by sandbox\n";
            return;
        }
        throw;
    }
    int client = connect_unix(socket_path);
    const Json control = Json::parse(read_line(client));
    require(control.size() == 5 && control["control"] == "stream_open" &&
                control["last_committed_event_id"] == "7" &&
                !control.contains("event_id"),
            "event stream control frame was not exact");
    stream.publish_committed(8, "{\"schema_version\":1,\"event_id\":\"8\"}");
    require(Json::parse(read_line(client))["event_id"] == "8" &&
                stream.high_watermark() == 8,
            "committed event was not published");
    // Stream is explicitly output-only.
    send_all(client, "x");
    eventually([&] {
        char byte{};
        return recv(client, &byte, 1, MSG_DONTWAIT) == 0;
    }, "event stream did not disconnect a writer");
    close(client);
    stream.stop();
    require(!std::filesystem::exists(socket_path), "event socket path survived stop");
}

void test_blocknotify(std::string_view test_executable) {
    const auto quote_argument = [](std::string_view value) {
        std::string result{"\""};
        result.reserve(value.size() + 2U);
        for (const char character : value) {
            if (character == '\\' || character == '"') result.push_back('\\');
            result.push_back(character);
        }
        result.push_back('"');
        return result;
    };
    const std::string executable = quote_argument(test_executable);
    const auto command = monero_solo::BlockNotifyCommand::parse(
        executable +
            " --blocknotify-success-fixture 'literal | ; $() `x`' \"%s\" back\\ slash");
    const std::string hash(64, 'a');
    const auto argv = command.instantiate(hash);
    require(argv.size() == 5 && argv[1] == "--blocknotify-success-fixture" &&
                argv[2] == "literal | ; $() `x`" && argv[3] == hash &&
                argv[4] == "back slash",
            "blocknotify no-shell tokenization was wrong");
    for (std::string_view invalid : {"true %s", "/bin/true", "/bin/true '%s",
                                     "/bin/true %s\\"}) {
        bool rejected = false;
        try { (void)monero_solo::BlockNotifyCommand::parse(invalid, false); }
        catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "invalid blocknotify template was accepted");
    }
    const auto ok = monero_solo::BlockNotifySupervisor::execute(command, hash, 1s);
    require(ok.delivered && ok.exit_code == 0, "blocknotify executable did not deliver");
    const auto fail = monero_solo::BlockNotifySupervisor::execute(
        monero_solo::BlockNotifyCommand::parse(
            executable + " --blocknotify-failure-fixture %s"),
        hash, 1s);
    require(!fail.delivered && fail.exit_code == 1,
            "nonzero blocknotify outcome was misclassified");

    const auto noisy = monero_solo::BlockNotifySupervisor::execute(
        monero_solo::BlockNotifyCommand::parse(
            executable + " --blocknotify-stderr-fixture %s"),
        hash, 1s);
    require(!noisy.delivered && noisy.exit_code.has_value() &&
                *noisy.exit_code != 0 &&
                noisy.stderr_excerpt.starts_with("A??\nZ") &&
                noisy.stderr_excerpt.find('\0') == std::string::npos &&
                std::all_of(noisy.stderr_excerpt.begin(),
                            noisy.stderr_excerpt.end(), [](unsigned char byte) {
                                return byte == '\n' || byte == '\r' || byte == '\t' ||
                                       (byte >= 0x20U && byte <= 0x7eU);
                            }),
            "blocknotify stderr was not converted to bounded NUL-free UTF-8 text");

    std::string script_template =
        (std::filesystem::path(test_executable).parent_path() /
         "mss-blocknotify-script-XXXXXX").string();
    std::vector<char> script_path(script_template.begin(), script_template.end());
    script_path.push_back('\0');
    const int script = mkstemp(script_path.data());
    require(script >= 0, "could not create blocknotify script fixture");
    constexpr std::string_view script_body = "#!/bin/sh\nexit 0\n";
    require(write(script, script_body.data(), script_body.size()) ==
                static_cast<ssize_t>(script_body.size()) &&
                fchmod(script, 0700) == 0,
            "could not initialize blocknotify script fixture");
    close(script);
    const auto shebang = monero_solo::BlockNotifySupervisor::execute(
        monero_solo::BlockNotifyCommand::parse(
            quote_argument(script_path.data()) + " %s"), hash, 1s);
    (void)unlink(script_path.data());
    require(shebang.delivered && shebang.exit_code == 0,
            "descriptor-based blocknotify execution did not support shebang scripts");
    const std::array<unsigned, 8> expected{1, 1, 5, 30, 120, 600, 3600, 3600};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(monero_solo::BlockNotifySupervisor::retry_delay(
                    static_cast<std::uint32_t>(index)).count() == expected[index],
                "blocknotify retry schedule was wrong");
    }
}

void test_zmq_immediate_stop() {
    for (unsigned iteration = 0; iteration < 64U; ++iteration) {
        monero_solo::ZmqSubscriber subscriber(
            "tcp://127.0.0.1:1", [](std::string_view) {});
        if (!subscriber.available()) {
            std::cout << "ZMQ immediate-stop test skipped: libzmq unavailable\n";
            return;
        }
        subscriber.start();
        subscriber.stop();
        require(!subscriber.running(),
                "ZMQ subscriber remained running after immediate stop");
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2) {
        const std::string_view fixture(argv[1]);
        if (fixture == "--blocknotify-success-fixture") _exit(0);
        if (fixture == "--blocknotify-failure-fixture") _exit(1);
        if (fixture == "--blocknotify-stderr-fixture" && argc == 3) {
            const std::array<char, 5> bytes{
                'A', '\0', static_cast<char>(0xff), '\n', 'Z'};
            _exit(write(STDERR_FILENO, bytes.data(), bytes.size()) ==
                          static_cast<ssize_t>(bytes.size())
                      ? 7
                      : 8);
        }
    }
    try {
        test_daemon_json_rpc_envelope();
        test_daemon_request_scheduler();
        test_reconciliation_candidate_identity();
        test_candidate_duplicate_capacity_policy();
        test_duplicate_registry();
        test_defense();
        test_stratum();
        test_nicehash_compact_target_simple_mode();
        test_nicehash_unrepresentable_difficulty_is_explicit();
        test_stratum_fatal_event_supervision();
        test_stratum_submit_admission();
        test_event_stream();
        test_zmq_immediate_stop();
        test_blocknotify(std::filesystem::canonical(argv[0]).string());
        std::cout << "protocol tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "protocol tests failed: " << error.what() << '\n';
        return 1;
    }
}
