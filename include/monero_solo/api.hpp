/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#ifndef MONERO_SOLO_API_HPP
#define MONERO_SOLO_API_HPP

#include "monero_solo/config.hpp"
#include "monero_solo/database.hpp"
#include "monero_solo/http.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace monero_solo {

enum class ApiCollection {
    connections,
    workers,
    templates,
    jobs,
    shares,
    hashes,
    submissions,
    rounds,
    bans,
    events,
};

enum class ApiDetail {
    connection,
    share,
    submission,
};

enum class ApiSingleton {
    summary,
    daemon,
    verifier,
    persistence,
    current_round,
};

struct ApiComponentState {
    bool ready{};
    bool degraded{};
    std::optional<std::string> reason;
};

struct ApiReadinessSnapshot {
    std::optional<std::uint64_t> height;
    ApiComponentState database;
    ApiComponentState entropy;
    ApiComponentState daemon_rpc;
    ApiComponentState template_state;
    ApiComponentState verifier;
    ApiComponentState stratum;

    [[nodiscard]] bool ready() const noexcept;
};

struct ApiCollectionRequest {
    ApiCollection resource{ApiCollection::connections};
    std::string path;
    std::uint64_t after_database_id{};
    std::uint32_t limit{100};
    std::map<std::string, std::string, std::less<>> filters;
    bool authenticated{};
};

struct ApiCollectionResult {
    nlohmann::json rows = nlohmann::json::array();
    std::optional<std::uint64_t> next_database_id;
};

struct ApiDetailRequest {
    ApiDetail resource{ApiDetail::connection};
    std::string id;
    bool include_blobs{};
    bool authenticated{};
};

/*
 * Runtime/database readers implement this interface. It deliberately carries
 * already-shaped JSON records because Database v1 exposes mutation primitives
 * and a few aggregates, but no general read-only row API. ApiService still
 * owns routing, authentication, strict query validation, exact envelopes,
 * cursor binding, limits, sensitive-view admission, and HTTP errors.
 *
 * Each callback may execute concurrently on HttpServer worker threads and
 * must therefore provide its own snapshot/thread-safety guarantees. Returned
 * singleton/detail values must be JSON objects. Collection rows must be an
 * array of at most request.limit records, ordered by increasing database ID.
 */
struct ApiDataSource {
    std::function<ApiReadinessSnapshot()> readiness;
    std::function<std::optional<nlohmann::json>(ApiSingleton)> singleton;
    std::function<ApiCollectionResult(const ApiCollectionRequest &)> collection;
    std::function<std::optional<nlohmann::json>(const ApiDetailRequest &)> detail;
};

struct SqliteApiDataSourceOptions {
    DatabaseOptions database;
    HashrateSource active_hashrate_source{HashrateSource::verified};
    /*
     * Live callbacks supply readiness, daemon/verifier snapshots, and the
     * static `summary.server` skeleton. The factory merges that skeleton (and
     * optional live `summary.daemon`) with SQLite's persisted counters,
     * current round, and global hashrate. SQLite is authoritative for all
     * collections, details, the current round, and persistence.
     */
    ApiDataSource live;
    std::function<std::int64_t()> clock;
    /* Live writer-queue state; omitted for offline/read-only API users. */
    std::function<DatabaseWriterStats()> writer_stats;
};

/*
 * Creates a WAL-safe, read-only SQLite-backed source. It opens a dedicated
 * SQLITE_OPEN_READONLY connection and serializes its own snapshot access; it
 * never borrows Database's writer connection or exposes raw SQL to filters.
 */
[[nodiscard]] ApiDataSource make_sqlite_api_data_source(
    SqliteApiDataSourceOptions options);

struct ApiIdentity {
    std::string version{"0.1.0"};
    std::string git_commit{"0000000000000000000000000000000000000000"};
    std::string session_id{"00000000000000000000000000000000"};
    std::int64_t started_unix_us{};
};

struct ApiServiceOptions {
    ApiConfig api;
    ApiIdentity identity;
    HashrateSource active_hashrate_source{HashrateSource::verified};
    std::size_t worker_threads{4};
};

class ApiService final {
public:
    using Clock = std::function<std::int64_t()>;

    ApiService(ApiServiceOptions options,
               Database *database,
               ApiDataSource data_source = {},
               Clock clock = {});
    ApiService(const ApiService &) = delete;
    ApiService &operator=(const ApiService &) = delete;
    ~ApiService();

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string bound_endpoint() const;

    /* Public for deterministic unit tests; HttpServer uses this same handler. */
    [[nodiscard]] HttpResponse handle(const HttpRequest &request) const;

private:
    struct ParsedQuery;
    struct CollectionRoute;

    [[nodiscard]] HttpResponse handle_authenticated(
        const HttpRequest &request,
        bool authenticated,
        std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse handle_collection(
        const HttpRequest &request,
        const CollectionRoute &route,
        bool authenticated,
        std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse handle_detail(
        const HttpRequest &request,
        ApiDetail resource,
        std::string id,
        bool include_blobs,
        bool authenticated,
        std::int64_t now_unix_us) const;

    [[nodiscard]] HttpResponse singleton_response(
        ApiSingleton resource,
        std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse persistence_response(
        std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse hashrate_response(
        const ParsedQuery &query,
        std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse ready_response(std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse live_response(std::int64_t now_unix_us) const;

    [[nodiscard]] bool authenticate(const HttpRequest &request) const;
    [[nodiscard]] std::string generated_at(std::int64_t now_unix_us) const;
    [[nodiscard]] HttpResponse success(nlohmann::json data,
                                       std::int64_t now_unix_us,
                                       int status = 200) const;
    [[nodiscard]] HttpResponse error(int status,
                                     std::string_view code,
                                     std::string_view message,
                                     std::int64_t now_unix_us,
                                     std::vector<std::pair<std::string,
                                                          std::string>> headers = {}) const;

    ApiServiceOptions options_;
    Database *database_{};
    ApiDataSource data_source_;
    Clock clock_;
    std::optional<std::string> expected_authorization_;
    std::unique_ptr<HttpServer> server_;
};

[[nodiscard]] std::string_view api_collection_path(ApiCollection resource) noexcept;

} // namespace monero_solo

#endif
