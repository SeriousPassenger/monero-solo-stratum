#include "monero_solo/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace monero_solo {
namespace {

constexpr std::string_view kSchemaSql =
#include "schema.sql"
    ;

constexpr std::string_view kEmptyPayload =
    "{\"payload_schema_version\":1,\"data\":{}}";
constexpr std::string_view kUint128Max =
    "340282366920938463463374607431768211455";
constexpr std::string_view kUint64Max = "18446744073709551615";
constexpr std::uint64_t kWriterEnvelopeBytes = 512;

void require(bool condition, std::string_view message);

void validate_event_payload(std::string_view payload)
{
    bool duplicate_key = false;
    bool depth_exceeded = false;
    bool invalid_key = false;
    std::vector<std::set<std::string>> object_keys;
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(
            payload.begin(), payload.end(),
            [&](int depth, nlohmann::json::parse_event_t event,
                nlohmann::json &parsed) {
                if (depth < 0 || depth > 8) {
                    depth_exceeded = true;
                }
                if (event == nlohmann::json::parse_event_t::object_start) {
                    object_keys.emplace_back();
                }
                else if (event == nlohmann::json::parse_event_t::key) {
                    const std::string &key = parsed.get_ref<const std::string &>();
                    if (key.empty() || key.size() > 128U) {
                        invalid_key = true;
                    }
                    if (object_keys.empty() ||
                        !object_keys.back().insert(key).second) {
                        duplicate_key = true;
                    }
                }
                else if (event == nlohmann::json::parse_event_t::object_end) {
                    if (object_keys.empty()) {
                        duplicate_key = true;
                    }
                    else {
                        object_keys.pop_back();
                    }
                }
                return true;
            },
            true, false);
    }
    catch (const std::exception &) {
        throw DatabaseError("event payload is not valid UTF-8 JSON");
    }
    require(!duplicate_key, "event payload contains a duplicate object key");
    require(!depth_exceeded, "event payload exceeds maximum JSON depth 8");
    require(!invalid_key,
            "event payload object key length is outside 1..128 bytes");
    require(object_keys.empty(), "event payload object nesting is invalid");
    require(document.is_object() && document.size() == 2U &&
                document.contains("payload_schema_version") &&
                document.contains("data"),
            "event payload must contain exactly payload_schema_version and data");
    const auto &version = document.at("payload_schema_version");
    require(version.is_number_unsigned() && version.get<std::uint64_t>() == 1U,
            "event payload schema version must be integer 1");
    require(document.at("data").is_object(),
            "event payload data must be an object");
}

[[noreturn]] void throw_database(sqlite3 *db, std::string_view operation,
                                 int code = SQLITE_ERROR)
{
    std::string message(operation);
    message += ": ";
    message += db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    throw DatabaseError(message);
}

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw DatabaseError(std::string(message));
    }
}

/*
 * Synchronous callers retain the simple Database API, but admission and
 * execution are two separate phases. Only admitted waiters are writer-queue
 * items. Callers that would consume the priority reserve remain backpressured
 * outside the queue until capacity is returned. Once admitted, priority FIFO
 * always runs before ordinary FIFO.
 */
class WriterScheduler final {
public:
    explicit WriterScheduler(const DatabaseOptions &options)
        : max_items_(options.max_writer_queue_items),
          max_bytes_(options.max_writer_queue_bytes),
          reserved_items_(options.writer_priority_reserve_items),
          reserved_bytes_(checked_reserved_bytes(reserved_items_))
    {
        require(max_items_ > reserved_items_,
                "database writer queue has no ordinary command capacity");
        require(reserved_bytes_ <= max_bytes_ &&
                    max_bytes_ - reserved_bytes_ >= kWriterEnvelopeBytes,
                "database writer byte queue has no ordinary command capacity");
    }

    WriterScheduler(const WriterScheduler &) = delete;
    WriterScheduler &operator=(const WriterScheduler &) = delete;

    void lock()
    {
        acquire(false);
    }

    void lock_priority()
    {
        acquire(true);
    }

    void unlock() noexcept
    {
        {
            std::lock_guard guard(state_mutex_);
            running_ = false;
        }
        condition_.notify_all();
    }

    [[nodiscard]] DatabaseWriterStats stats() const noexcept
    {
        std::lock_guard guard(state_mutex_);
        return DatabaseWriterStats{
            .queued_items = queued_items_,
            .queued_bytes = queued_bytes_,
            .priority_items = static_cast<std::uint64_t>(priority_.size()),
            .pending_accounting_items = 0,
            .pending_transient_shares = 0,
        };
    }

private:
    struct Waiter {
        bool priority{};
    };

    static std::uint64_t checked_reserved_bytes(std::uint64_t items)
    {
        require(items <= std::numeric_limits<std::uint64_t>::max() /
                             kWriterEnvelopeBytes,
                "database writer priority byte reserve overflows");
        return items * kWriterEnvelopeBytes;
    }

    [[nodiscard]] bool has_capacity(bool priority) const noexcept
    {
        if (queued_items_ >= max_items_ ||
            queued_bytes_ > max_bytes_ - kWriterEnvelopeBytes) {
            return false;
        }
        if (priority) return true;

        /*
         * The reserve is a limit on ordinary commands, not a requirement that
         * priority commands arrive after them.  Counting an already queued
         * priority waiter against the ordinary limit made admission depend on
         * thread scheduling and could leave ordinary capacity unused.
         */
        const auto ordinary_items =
            static_cast<std::uint64_t>(ordinary_.size());
        const std::uint64_t ordinary_bytes =
            ordinary_items * kWriterEnvelopeBytes;
        return ordinary_items < max_items_ - reserved_items_ &&
               ordinary_bytes <= max_bytes_ - reserved_bytes_ -
                                     kWriterEnvelopeBytes;
    }

    void acquire(bool is_priority)
    {
        Waiter waiter{.priority = is_priority};
        std::unique_lock guard(state_mutex_);
        condition_.wait(guard, [&] { return has_capacity(is_priority); });
        if (is_priority) {
            priority_.push_back(&waiter);
        }
        else {
            ordinary_.push_back(&waiter);
        }
        ++queued_items_;
        queued_bytes_ += kWriterEnvelopeBytes;
        condition_.notify_all();

        condition_.wait(guard, [&] {
            if (running_) return false;
            if (is_priority) {
                return !priority_.empty() && priority_.front() == &waiter;
            }
            return priority_.empty() && !ordinary_.empty() &&
                   ordinary_.front() == &waiter;
        });
        if (is_priority) {
            priority_.pop_front();
        }
        else {
            ordinary_.pop_front();
        }
        --queued_items_;
        queued_bytes_ -= kWriterEnvelopeBytes;
        running_ = true;
        condition_.notify_all();
    }

    const std::uint64_t max_items_;
    const std::uint64_t max_bytes_;
    const std::uint64_t reserved_items_;
    const std::uint64_t reserved_bytes_;
    mutable std::mutex state_mutex_;
    std::condition_variable condition_;
    std::deque<Waiter *> priority_;
    std::deque<Waiter *> ordinary_;
    std::uint64_t queued_items_{};
    std::uint64_t queued_bytes_{};
    bool running_{};
};

class PriorityWriterLock final {
public:
    explicit PriorityWriterLock(WriterScheduler &scheduler)
        : scheduler_(scheduler)
    {
        scheduler_.lock_priority();
    }

    ~PriorityWriterLock()
    {
        scheduler_.unlock();
    }

    PriorityWriterLock(const PriorityWriterLock &) = delete;
    PriorityWriterLock &operator=(const PriorityWriterLock &) = delete;

private:
    WriterScheduler &scheduler_;
};

void require_no_nul(std::string_view value, std::string_view name)
{
    require(value.find('\0') == std::string_view::npos,
            std::string(name) + " contains an embedded NUL");
}

void require_i64(std::uint64_t value, std::string_view name)
{
    require(value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
            std::string(name) + " is outside SQLite INTEGER range");
}

bool is_canonical_unsigned(std::string_view value, bool allow_zero = true)
{
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return false;
    }
    return allow_zero || value != "0";
}

void require_uint128(std::string_view value, std::string_view name,
                     bool allow_zero = true)
{
    require(is_canonical_unsigned(value, allow_zero),
            std::string(name) + " is not canonical unsigned decimal");
    require(value.size() < kUint128Max.size() ||
                (value.size() == kUint128Max.size() && value <= kUint128Max),
            std::string(name) + " exceeds uint128");
}

void require_uint64_decimal(std::string_view value, std::string_view name,
                            bool allow_zero = true)
{
    require(is_canonical_unsigned(value, allow_zero),
            std::string(name) + " is not canonical unsigned decimal");
    require(value.size() < kUint64Max.size() ||
                (value.size() == kUint64Max.size() && value <= kUint64Max),
            std::string(name) + " exceeds uint64");
}

std::string add_uint128(std::string_view left, std::string_view right)
{
    require_uint128(left, "left decimal");
    require_uint128(right, "right decimal");

    std::string result;
    result.reserve(std::max(left.size(), right.size()) + 1);
    std::ptrdiff_t i = static_cast<std::ptrdiff_t>(left.size()) - 1;
    std::ptrdiff_t j = static_cast<std::ptrdiff_t>(right.size()) - 1;
    int carry = 0;
    while (i >= 0 || j >= 0 || carry != 0) {
        int digit = carry;
        if (i >= 0) {
            digit += left[static_cast<std::size_t>(i--)] - '0';
        }
        if (j >= 0) {
            digit += right[static_cast<std::size_t>(j--)] - '0';
        }
        result.push_back(static_cast<char>('0' + (digit % 10)));
        carry = digit / 10;
    }
    std::reverse(result.begin(), result.end());
    require_uint128(result, "decimal sum");
    return result;
}

std::string divide_decimal(std::string_view value, std::uint32_t divisor)
{
    require(divisor != 0, "decimal divisor is zero");
    require_uint128(value, "decimal dividend");
    std::string quotient;
    quotient.reserve(value.size());
    std::uint32_t remainder = 0;
    for (const char c : value) {
        const std::uint32_t current = remainder * 10U +
                                      static_cast<std::uint32_t>(c - '0');
        const char q = static_cast<char>('0' + current / divisor);
        if (!quotient.empty() || q != '0') {
            quotient.push_back(q);
        }
        remainder = current % divisor;
    }
    return quotient.empty() ? "0" : quotient;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

class Statement final {
public:
    Statement(sqlite3 *db, std::string_view sql) : db_(db)
    {
        const int result = sqlite3_prepare_v2(
            db, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw_database(db, "prepare statement", result);
        }
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void bind(std::string_view value, int index)
    {
        require_no_nul(value, "database text");
        check(sqlite3_bind_text(statement_, index, value.data(),
                                static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    void bind(std::span<const std::uint8_t> value, int index)
    {
        const void *data = value.empty() ? static_cast<const void *>("") : value.data();
        check(sqlite3_bind_blob(statement_, index, data,
                                static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    template <std::size_t N>
    void bind(const std::array<std::uint8_t, N> &value, int index)
    {
        bind(std::span<const std::uint8_t>(value), index);
    }

    void bind(std::int64_t value, int index)
    {
        check(sqlite3_bind_int64(statement_, index, value));
    }

    void bind(int value, int index)
    {
        check(sqlite3_bind_int(statement_, index, value));
    }

    void bind_null(int index)
    {
        check(sqlite3_bind_null(statement_, index));
    }

    template <class T>
    void bind_optional(const std::optional<T> &value, int index)
    {
        if (value.has_value()) {
            bind(*value, index);
        }
        else {
            bind_null(index);
        }
    }

    [[nodiscard]] bool row()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        throw_database(db_, "step statement", result);
    }

    void done()
    {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_DONE) {
            throw_database(db_, "execute statement", result);
        }
    }

    [[nodiscard]] std::int64_t integer(int column) const
    {
        return sqlite3_column_int64(statement_, column);
    }

    [[nodiscard]] bool is_null(int column) const
    {
        return sqlite3_column_type(statement_, column) == SQLITE_NULL;
    }

    [[nodiscard]] std::string text(int column) const
    {
        const unsigned char *value = sqlite3_column_text(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (value == nullptr) {
            return {};
        }
        return std::string(reinterpret_cast<const char *>(value),
                           static_cast<std::size_t>(bytes));
    }

    [[nodiscard]] ByteVector blob(int column) const
    {
        const auto *value = static_cast<const std::uint8_t *>(
            sqlite3_column_blob(statement_, column));
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (bytes <= 0) {
            return {};
        }
        return ByteVector(value, value + bytes);
    }

private:
    void check(int result)
    {
        if (result != SQLITE_OK) {
            throw_database(db_, "bind statement", result);
        }
    }

    sqlite3 *db_{};
    sqlite3_stmt *statement_{};
};

void execute(sqlite3 *db, std::string_view sql)
{
    char *error = nullptr;
    const int result = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr,
                                    &error);
    if (result != SQLITE_OK) {
        std::string message = "execute SQL: ";
        if (error != nullptr) {
            message += error;
        }
        else {
            message += sqlite3_errmsg(db);
        }
        sqlite3_free(error);
        throw DatabaseError(message);
    }
}

class Transaction final {
public:
    explicit Transaction(sqlite3 *db) : db_(db)
    {
        execute(db_, "BEGIN IMMEDIATE");
    }

    ~Transaction()
    {
        if (!committed_) {
            (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void commit()
    {
        execute(db_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3 *db_{};
    bool committed_{};
};

void validate_worker(const WorkerInsert &worker)
{
    require(!worker.login.empty(), "worker login is empty");
    require_no_nul(worker.login, "worker login");
    require_no_nul(worker.rigid, "worker rigid");
}

std::int64_t upsert_worker_unlocked(sqlite3 *db, const WorkerInsert &worker)
{
    Statement insert(
        db,
        "INSERT INTO workers(login,rigid,first_seen_unix_us,last_seen_unix_us) "
        "VALUES(?1,?2,?3,?3) "
        "ON CONFLICT(login,rigid) DO UPDATE SET "
        "last_seen_unix_us=max(last_seen_unix_us,excluded.last_seen_unix_us)");
    insert.bind(worker.login, 1);
    insert.bind(worker.rigid, 2);
    insert.bind(worker.seen_unix_us, 3);
    insert.done();

    Statement select(
        db, "SELECT id FROM workers WHERE login=?1 AND rigid=?2");
    select.bind(worker.login, 1);
    select.bind(worker.rigid, 2);
    require(select.row(), "worker upsert did not produce a row");
    return select.integer(0);
}

void authenticate_connection_unlocked(sqlite3 *db,
                                      std::int64_t connection_id,
                                      std::int64_t worker_id,
                                      std::string_view agent,
                                      std::int64_t authenticated_unix_us)
{
    Statement statement(
        db,
        "UPDATE connections SET worker_id=?1,agent=?2,authenticated_unix_us=?3 "
        "WHERE id=?4 AND closed_unix_us IS NULL AND authenticated_unix_us IS NULL");
    statement.bind(worker_id, 1);
    statement.bind(agent, 2);
    statement.bind(authenticated_unix_us, 3);
    statement.bind(connection_id, 4);
    statement.done();
    require(sqlite3_changes(db) == 1,
            "connection is absent, closed, or already authenticated");
}

std::int64_t last_insert_id(sqlite3 *db)
{
    return sqlite3_last_insert_rowid(db);
}

CandidateState parse_candidate_state(std::string_view value)
{
    if (value == "journaled") return CandidateState::journaled;
    if (value == "dispatching") return CandidateState::dispatching;
    if (value == "retry_wait") return CandidateState::retry_wait;
    if (value == "accepted") return CandidateState::accepted;
    if (value == "rejected") return CandidateState::rejected;
    if (value == "ambiguous") return CandidateState::ambiguous;
    if (value == "accepted_by_reconciliation") {
        return CandidateState::accepted_by_reconciliation;
    }
    throw DatabaseError("database contains an invalid candidate state");
}

std::string verdict_kind_text(CandidateVerdictKind kind)
{
    return kind == CandidateVerdictKind::false_candidate
               ? "false_candidate"
               : "candidate_mismatch";
}

CandidateVerdictDisposition parse_verdict_disposition(std::string_view value)
{
    if (value == "pending") return CandidateVerdictDisposition::pending;
    if (value == "actionable") return CandidateVerdictDisposition::actionable;
    if (value == "suppressed") return CandidateVerdictDisposition::suppressed;
    throw DatabaseError("database contains an invalid candidate verdict disposition");
}

template <std::size_t N>
std::array<std::uint8_t, N> exact_array(ByteVector bytes,
                                        std::string_view field)
{
    require(bytes.size() == N, std::string(field) + " has invalid length");
    std::array<std::uint8_t, N> result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

std::int64_t checked_add_time(std::int64_t value, std::int64_t delta)
{
    require(delta >= 0 && value <= std::numeric_limits<std::int64_t>::max() - delta,
            "timestamp overflow");
    return value + delta;
}

std::uint32_t blocknotify_delay_seconds(std::uint32_t attempt_count)
{
    switch (attempt_count) {
    case 0:
    case 1:
        return 1;
    case 2:
        return 5;
    case 3:
        return 30;
    case 4:
        return 120;
    case 5:
        return 600;
    default:
        return 3600;
    }
}

} // namespace

std::string_view to_string(CandidateState value) noexcept
{
    switch (value) {
    case CandidateState::journaled: return "journaled";
    case CandidateState::dispatching: return "dispatching";
    case CandidateState::retry_wait: return "retry_wait";
    case CandidateState::accepted: return "accepted";
    case CandidateState::rejected: return "rejected";
    case CandidateState::ambiguous: return "ambiguous";
    case CandidateState::accepted_by_reconciliation:
        return "accepted_by_reconciliation";
    }
    return "journaled";
}

std::string_view to_string(HashrateSource value) noexcept
{
    return value == HashrateSource::verified ? "verified" : "claimed";
}

struct StagedShareHash {
    Hash32 hash{};
    std::optional<bool> meets_share_target;
    std::optional<bool> meets_network_target;
};

struct StagedShare {
    ShareInsert value;
    std::int64_t round_id{};
    std::optional<std::string> verifier_ticket_dec;
    std::optional<std::string> verifier_seed_id_dec;
    std::map<std::string, StagedShareHash> hashes;
};

struct ShareTotalKey {
    std::string status;
    std::string provenance;

    [[nodiscard]] bool operator<(const ShareTotalKey &other) const noexcept
    {
        return std::tie(status, provenance) <
               std::tie(other.status, other.provenance);
    }
};

struct ShareTotalDelta {
    std::uint64_t count{};
    std::int64_t first_unix_us{};
    std::int64_t last_unix_us{};
};

struct HashrateKey {
    std::string scope_type;
    std::int64_t scope_id{};
    std::int64_t second_utc{};
    HashrateSource source{HashrateSource::verified};

    [[nodiscard]] bool operator<(const HashrateKey &other) const noexcept
    {
        return std::tie(scope_type, scope_id, second_utc, source) <
               std::tie(other.scope_type, other.scope_id, other.second_utc,
                        other.source);
    }
};

struct WorkDelta {
    std::string credited_difficulty_dec{"0"};
    std::uint64_t accepted_shares{};
};

struct AccountingApplyResult {
    std::set<std::int64_t> affected_rounds;
    std::optional<std::int64_t> hashrate_pruned_through;
};

struct RoundWorkKey {
    std::int64_t round_id{};
    HashrateSource source{HashrateSource::verified};
    std::string network_difficulty_dec;

    [[nodiscard]] bool operator<(const RoundWorkKey &other) const noexcept
    {
        return std::tie(round_id, source, network_difficulty_dec) <
               std::tie(other.round_id, other.source,
                        other.network_difficulty_dec);
    }
};

struct Database::Impl {
    explicit Impl(DatabaseOptions value)
        : options(std::move(value)), mutex(options)
    {
        require(!options.path.empty(), "database path is empty");
        require(options.busy_timeout_ms >= 1 && options.busy_timeout_ms <= 60000,
                "database busy timeout is outside 1..60000 ms");
        require(options.min_persisted_share_difficulty >= 1,
                "minimum persisted share difficulty must be positive");
        require(options.accounting_flush_interval_ms >= 10 &&
                    options.accounting_flush_interval_ms <= 60000,
                "accounting flush interval is outside 10..60000 ms");

        // Lock the database inode itself, not a pathname-derived sidecar. A
        // hard-link alias therefore resolves to the same advisory lock and
        // cannot be used by a second server instance to run live recovery
        // mutations against this database.
        lock_descriptor = ::open(
            options.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (lock_descriptor < 0) {
            throw DatabaseError(
                "open database ownership lock: " +
                std::string(std::strerror(errno)));
        }
        struct stat lock_state {};
        if (::fstat(lock_descriptor, &lock_state) != 0 ||
            !S_ISREG(lock_state.st_mode)) {
            (void)::close(lock_descriptor);
            lock_descriptor = -1;
            throw DatabaseError("database ownership target is not a regular file");
        }
        if (::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            (void)::close(lock_descriptor);
            lock_descriptor = -1;
            if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
                throw DatabaseError("database is already owned by another server instance");
            }
            throw DatabaseError(
                "lock database ownership target: " +
                std::string(std::strerror(lock_error)));
        }

        sqlite3 *opened = nullptr;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
        const int result = sqlite3_open_v2(options.path.c_str(), &opened, flags, nullptr);
        db = opened;
        if (result != SQLITE_OK) {
            const std::string message = db != nullptr ? sqlite3_errmsg(db)
                                                      : sqlite3_errstr(result);
            if (db != nullptr) {
                sqlite3_close_v2(db);
                db = nullptr;
            }
            (void)::flock(lock_descriptor, LOCK_UN);
            (void)::close(lock_descriptor);
            lock_descriptor = -1;
            throw DatabaseError("open SQLite database: " + message);
        }

        try {
            struct stat path_state {};
            if (::lstat(options.path.c_str(), &path_state) != 0 ||
                !S_ISREG(path_state.st_mode) ||
                path_state.st_dev != lock_state.st_dev ||
                path_state.st_ino != lock_state.st_ino) {
                throw DatabaseError(
                    "database pathname changed while acquiring ownership");
            }
            sqlite3_extended_result_codes(db, 1);
            if (sqlite3_busy_timeout(db, static_cast<int>(options.busy_timeout_ms)) !=
                SQLITE_OK) {
                throw_database(db, "set SQLite busy timeout");
            }
            execute(db, "PRAGMA foreign_keys = ON");
            set_and_verify_pragmas();
            migrate();
            verify_foreign_keys();
            recover_inflight_candidates_unlocked();
            recover_blocknotify_unlocked();
        }
        catch (...) {
            sqlite3_close_v2(db);
            db = nullptr;
            (void)::flock(lock_descriptor, LOCK_UN);
            (void)::close(lock_descriptor);
            lock_descriptor = -1;
            throw;
        }
    }

    ~Impl()
    {
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        if (lock_descriptor >= 0) {
            (void)::flock(lock_descriptor, LOCK_UN);
            (void)::close(lock_descriptor);
        }
    }

    int lock_descriptor{-1};

    void set_and_verify_pragmas()
    {
        {
            Statement statement(db, "PRAGMA journal_mode = WAL");
            require(statement.row(), "PRAGMA journal_mode returned no row");
            require(lower_ascii(statement.text(0)) == "wal",
                    "SQLite WAL mode could not be established");
            require(!statement.row(), "PRAGMA journal_mode returned multiple rows");
        }
        execute(db, "PRAGMA synchronous = FULL");

        const DatabasePragmas state = read_pragmas_unlocked();
        require(state.journal_mode == "wal", "SQLite journal_mode is not WAL");
        require(state.synchronous == "full", "SQLite synchronous is not FULL");
        require(state.foreign_keys, "SQLite foreign keys are not enabled");
        require(state.busy_timeout_ms == options.busy_timeout_ms,
                "SQLite busy timeout verification failed");
    }

    DatabasePragmas read_pragmas_unlocked() const
    {
        DatabasePragmas state;
        {
            Statement statement(db, "PRAGMA journal_mode");
            require(statement.row(), "PRAGMA journal_mode returned no row");
            state.journal_mode = lower_ascii(statement.text(0));
        }
        {
            Statement statement(db, "PRAGMA synchronous");
            require(statement.row(), "PRAGMA synchronous returned no row");
            const std::int64_t value = statement.integer(0);
            state.synchronous = value == 2 ? "full" : std::to_string(value);
        }
        {
            Statement statement(db, "PRAGMA foreign_keys");
            require(statement.row(), "PRAGMA foreign_keys returned no row");
            state.foreign_keys = statement.integer(0) == 1;
        }
        {
            Statement statement(db, "PRAGMA busy_timeout");
            require(statement.row(), "PRAGMA busy_timeout returned no row");
            const auto value = statement.integer(0);
            require(value >= 0 &&
                        value <= std::numeric_limits<std::uint32_t>::max(),
                    "SQLite busy_timeout returned an invalid value");
            state.busy_timeout_ms = static_cast<std::uint32_t>(value);
        }
        return state;
    }

    bool table_exists(std::string_view name) const
    {
        Statement statement(
            db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1");
        statement.bind(name, 1);
        return statement.row();
    }

    void migrate()
    {
        if (!table_exists("schema_meta")) {
            Statement count(
                db,
                "SELECT count(*) FROM sqlite_schema "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%'");
            require(count.row(), "could not inspect empty SQLite database");
            require(count.integer(0) == 0,
                    "database has tables but no monero-solo schema metadata");

            Transaction transaction(db);
            execute(db, kSchemaSql);
            execute(db, "PRAGMA user_version = 3");
            transaction.commit();
            return;
        }

        Statement statement(
            db, "SELECT value FROM schema_meta WHERE key='schema_version'");
        require(statement.row(), "database schema_version is missing");
        const std::string version = statement.text(0);
        if (version == "1" || version == "2") {
            throw DatabaseError(
                "database schema version " + version +
                " requires a clean reset; stop the service and remove the "
                "SQLite database, WAL, and SHM files");
        }
        require(version == "3",
                "unsupported database schema version (expected clean schema v3)");
        require(!statement.row(), "database has duplicate schema_version metadata");
    }

    void verify_foreign_keys() const
    {
        Statement statement(db, "PRAGMA foreign_key_check");
        require(!statement.row(), "database failed foreign-key integrity check");
    }

    std::int64_t latest_session_id_unlocked() const
    {
        Statement statement(db, "SELECT id FROM server_sessions ORDER BY id DESC LIMIT 1");
        return statement.row() ? statement.integer(0) : 0;
    }

    std::int64_t insert_event_unlocked(const EventInsert &event)
    {
        require(event.session_id > 0, "event session ID must be positive");
        require(!event.type.empty(), "event type is empty");
        require(event.type.size() <= 128, "event type is too long");
        require_no_nul(event.type, "event type");
        require(!event.payload_json.empty(), "event payload is empty");
        require(event.payload_json.size() <= 65536, "event payload exceeds 65536 bytes");
        require_no_nul(event.payload_json, "event payload");
        validate_event_payload(event.payload_json);

        Statement statement(
            db,
            "INSERT INTO events(session_id,created_unix_us,type,connection_id,"
            "worker_id,template_generation,job_public_id,share_id,candidate_id,round_id,payload_json) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
        statement.bind(event.session_id, 1);
        statement.bind(event.created_unix_us, 2);
        statement.bind(event.type, 3);
        statement.bind_optional(event.connection_id, 4);
        statement.bind_optional(event.worker_id, 5);
        statement.bind_optional(event.template_generation, 6);
        statement.bind_optional(event.job_public_id, 7);
        statement.bind_optional(event.share_id, 8);
        statement.bind_optional(event.candidate_id, 9);
        statement.bind_optional(event.round_id, 10);
        statement.bind(event.payload_json, 11);
        statement.done();
        return last_insert_id(db);
    }

    void recover_blocknotify_unlocked()
    {
        execute(db,
                "UPDATE blocknotify_deliveries SET state='pending',"
                "next_attempt_unix_us=NULL WHERE state='running'");
    }

    void recover_inflight_candidates_unlocked()
    {
        Transaction transaction(db);
        execute(db,
                "UPDATE candidate_attempts SET classification='indeterminate',"
                "response_excerpt=coalesce(response_excerpt,"
                "'process restarted while daemon request was in flight') "
                "WHERE classification='dispatching'");
        execute(db,
                "UPDATE candidates SET had_indeterminate=1,state=CASE "
                "WHEN attempt_count<max_attempts THEN 'retry_wait' ELSE 'ambiguous' END,"
                "terminal_reason=CASE WHEN attempt_count>=max_attempts THEN "
                "'process_restarted_with_exhausted_inflight_attempt' ELSE NULL END "
                "WHERE state='dispatching'");
        execute(db,
                "UPDATE candidate_reconciliations SET classification='indeterminate',"
                "response_excerpt=coalesce(response_excerpt,"
                "'process restarted while reconciliation request was in flight') "
                "WHERE classification='querying'");
        transaction.commit();
    }

    struct CandidateContext {
        std::int64_t session_id{};
        std::optional<std::int64_t> connection_id;
        std::optional<std::int64_t> worker_id;
        std::int64_t template_generation{};
        PublicId job_public_id{};
        std::optional<std::int64_t> share_id;
        std::int64_t round_id{};
        std::int64_t height{};
        Hash32 miner_tx_hash{};
        CandidateState state{CandidateState::journaled};
    };

    CandidateContext candidate_context_unlocked(std::int64_t candidate_id) const
    {
        Statement statement(
            db,
            "SELECT c.connection_id,s.worker_id,c.template_generation,c.job_public_id,"
            "c.first_share_id,c.round_id,c.height,c.miner_tx_hash,c.state "
            "FROM candidates c "
            "LEFT JOIN shares s ON s.id=c.first_share_id WHERE c.id=?1");
        statement.bind(candidate_id, 1);
        require(statement.row(), "candidate does not exist");
        CandidateContext result;
        if (!statement.is_null(0)) result.connection_id = statement.integer(0);
        if (!statement.is_null(1)) {
            result.worker_id = statement.integer(1);
        }
        result.template_generation = statement.integer(2);
        result.job_public_id = exact_array<16>(statement.blob(3), "candidate job public ID");
        if (!statement.is_null(4)) result.share_id = statement.integer(4);
        result.round_id = statement.integer(5);
        result.height = statement.integer(6);
        result.miner_tx_hash = exact_array<32>(statement.blob(7), "miner transaction hash");
        result.state = parse_candidate_state(statement.text(8));
        // Candidate correlation stays bound to its immutable originating
        // connection/job/share. The event itself is emitted by whichever
        // server process performs this state transition, including restart
        // recovery, so attribute it to that latest server session.
        result.session_id = latest_session_id_unlocked();
        require(result.session_id > 0, "candidate event has no server session");
        return result;
    }

    std::int64_t insert_candidate_event_unlocked(
        std::int64_t candidate_id, const CandidateContext &context,
        std::int64_t time_unix_us, std::string_view type,
        std::optional<std::int64_t> round_id = std::nullopt)
    {
        return insert_event_unlocked(EventInsert{
            .session_id = context.session_id,
            .created_unix_us = time_unix_us,
            .type = std::string(type),
            .connection_id = context.connection_id,
            .worker_id = context.worker_id,
            .template_generation = context.template_generation,
            .job_public_id = context.job_public_id,
            .share_id = context.share_id,
            .candidate_id = candidate_id,
            .round_id = round_id,
            .payload_json = std::string(kEmptyPayload),
        });
    }

    bool accept_candidate_in_transaction(
        std::int64_t candidate_id, std::int64_t accepted_unix_us,
        const std::optional<Hash32> &canonical_block_id,
        bool by_reconciliation)
    {
        CandidateContext context = candidate_context_unlocked(candidate_id);
        if (context.state == CandidateState::accepted ||
            context.state == CandidateState::accepted_by_reconciliation) {
            return false;
        }
        require(context.state != CandidateState::rejected,
                "an explicitly rejected candidate cannot later be accepted");
        if (by_reconciliation) {
            require(canonical_block_id.has_value(),
                    "positive reconciliation requires a canonical block ID");
            require(context.state == CandidateState::journaled ||
                        context.state == CandidateState::dispatching ||
                        context.state == CandidateState::retry_wait ||
                        context.state == CandidateState::ambiguous,
                    "candidate is not eligible for positive reconciliation");
        }
        else {
            require(context.state == CandidateState::journaled ||
                        context.state == CandidateState::dispatching ||
                        context.state == CandidateState::retry_wait,
                    "candidate is not eligible for daemon acceptance");
        }

        Statement open_round(db, "SELECT id FROM rounds WHERE state='open'");
        require(open_round.row(), "candidate acceptance found no open round");
        const std::int64_t closed_round_id = open_round.integer(0);
        require(!open_round.row(), "candidate acceptance found multiple open rounds");
        require(closed_round_id == context.round_id,
                "candidate origin round is no longer the open round");

        require(!round_has_higher_share_height_unlocked(
                    context.round_id, context.height),
                "candidate origin round contains a higher-height share");

        const CandidateState accepted_state = by_reconciliation
                                                    ? CandidateState::accepted_by_reconciliation
                                                    : CandidateState::accepted;
        Statement update(
            db,
            "UPDATE candidates SET state=?1,canonical_block_id=coalesce(?2,canonical_block_id),"
            "accepted_unix_us=?3,updated_unix_us=?3,terminal_reason=?4 "
            "WHERE id=?5 AND state IN ('journaled','dispatching','retry_wait','ambiguous')");
        update.bind(to_string(accepted_state), 1);
        update.bind_optional(canonical_block_id, 2);
        update.bind(accepted_unix_us, 3);
        update.bind(by_reconciliation ? "positive_reconciliation" : "daemon_ok", 4);
        update.bind(candidate_id, 5);
        update.done();
        if (sqlite3_changes(db) == 0) {
            return false;
        }

        Statement pending_count(
            db,
            "SELECT count(*) FROM candidate_verdicts "
            "WHERE candidate_id=?1 AND disposition='pending'");
        pending_count.bind(candidate_id, 1);
        require(pending_count.row(), "could not count pending candidate verdicts");
        const std::int64_t contradictions = pending_count.integer(0);

        Statement suppress(
            db,
            "UPDATE candidate_verdicts SET disposition='suppressed',"
            "resolved_unix_us=?1 WHERE candidate_id=?2 AND disposition='pending'");
        suppress.bind(accepted_unix_us, 1);
        suppress.bind(candidate_id, 2);
        suppress.done();

        Statement close_round(
            db,
            "UPDATE rounds SET closed_unix_us=?1,state='closed',"
            "accepted_candidate_id=?2,accepted_height=?3,miner_tx_hash=?4,block_id=?5 "
            "WHERE id=?6 AND state='open'");
        close_round.bind(accepted_unix_us, 1);
        close_round.bind(candidate_id, 2);
        close_round.bind(context.height, 3);
        close_round.bind(context.miner_tx_hash, 4);
        close_round.bind_optional(canonical_block_id, 5);
        close_round.bind(closed_round_id, 6);
        close_round.done();
        require(sqlite3_changes(db) == 1, "open round changed during candidate acceptance");

        Statement next_round(
            db, "INSERT INTO rounds(opened_unix_us,state) VALUES(?1,'open')");
        next_round.bind(accepted_unix_us, 1);
        next_round.done();
        const std::int64_t next_round_id = last_insert_id(db);

        insert_candidate_event_unlocked(candidate_id, context, accepted_unix_us,
                                         "round_closed", closed_round_id);
        insert_candidate_event_unlocked(candidate_id, context, accepted_unix_us,
                                         by_reconciliation ? "candidate_reconciled"
                                                           : "candidate_result",
                                         closed_round_id);
        insert_event_unlocked(EventInsert{
            .session_id = context.session_id,
            .created_unix_us = accepted_unix_us,
            .type = "round_opened",
            .connection_id = std::nullopt,
            .worker_id = std::nullopt,
            .template_generation = std::nullopt,
            .job_public_id = std::nullopt,
            .share_id = std::nullopt,
            .candidate_id = std::nullopt,
            .round_id = next_round_id,
            .payload_json = std::string(kEmptyPayload),
        });

        if (contradictions > 0) {
            insert_candidate_event_unlocked(candidate_id, context, accepted_unix_us,
                                             "verifier_consistency_error",
                                             closed_round_id);
        }

        if (options.blocknotify_enabled) {
            Statement notify(
                db,
                "INSERT INTO blocknotify_deliveries(candidate_id,miner_tx_hash,state) "
                "VALUES(?1,?2,'pending') ON CONFLICT(candidate_id) DO NOTHING");
            notify.bind(candidate_id, 1);
            notify.bind(context.miner_tx_hash, 2);
            notify.done();
        }
        (void)try_finalize_round_unlocked(closed_round_id, accepted_unix_us);
        return true;
    }

    void add_round_work_segment_unlocked(
        std::int64_t round_id, HashrateSource source,
        std::string_view network_difficulty,
        std::string_view credited_difficulty,
        std::uint64_t accepted_shares = 1)
    {
        require(round_id > 0, "round work segment has an invalid round ID");
        require_uint128(network_difficulty, "round network difficulty", false);
        require_uint128(credited_difficulty, "round credited difficulty", false);
        require(accepted_shares >= 1 &&
                    accepted_shares <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()),
                "round accepted-share delta is outside SQLite range");

        Statement select(
            db,
            "SELECT credited_difficulty_dec,accepted_share_count "
            "FROM round_work_segments WHERE round_id=?1 AND source=?2 "
            "AND network_difficulty_dec=?3");
        select.bind(round_id, 1);
        select.bind(to_string(source), 2);
        select.bind(network_difficulty, 3);
        if (select.row()) {
            const std::string updated = add_uint128(
                select.text(0), credited_difficulty);
            const std::int64_t count = select.integer(1);
            const auto count_delta = static_cast<std::int64_t>(accepted_shares);
            require(count <= std::numeric_limits<std::int64_t>::max() -
                                 count_delta,
                    "round work segment share counter overflow");
            Statement update(
                db,
                "UPDATE round_work_segments SET credited_difficulty_dec=?1,"
                "accepted_share_count=?2 WHERE round_id=?3 AND source=?4 "
                "AND network_difficulty_dec=?5");
            update.bind(updated, 1);
            update.bind(count + count_delta, 2);
            update.bind(round_id, 3);
            update.bind(to_string(source), 4);
            update.bind(network_difficulty, 5);
            update.done();
            require(sqlite3_changes(db) == 1,
                    "round work segment update failed");
            return;
        }

        Statement insert(
            db,
            "INSERT INTO round_work_segments(round_id,source,"
            "network_difficulty_dec,credited_difficulty_dec,accepted_share_count) "
            "VALUES(?1,?2,?3,?4,?5)");
        insert.bind(round_id, 1);
        insert.bind(to_string(source), 2);
        insert.bind(network_difficulty, 3);
        insert.bind(credited_difficulty, 4);
        insert.bind(static_cast<std::int64_t>(accepted_shares), 5);
        insert.done();
    }

    bool try_finalize_round_unlocked(
        std::int64_t round_id, std::int64_t finalized_unix_us,
        bool pending_accounting_already_applied = false)
    {
        require(round_id > 0, "round finalization has an invalid round ID");
        Statement round(
            db,
            "SELECT state,closed_unix_us,credited_difficulty_dec,"
            "accepted_share_count,effort_finalized_unix_us "
            "FROM rounds WHERE id=?1");
        round.bind(round_id, 1);
        require(round.row(), "round finalization target does not exist");
        const std::string state = round.text(0);
        if (state == "open" || !round.is_null(4)) {
            return false;
        }
        require(state == "closed" && !round.is_null(1),
                "round has an invalid persisted state");
        const std::int64_t closed_unix_us = round.integer(1);
        const std::string credited_total = round.text(2);
        const std::int64_t accepted_total = round.integer(3);

        for (const auto &[id, staged] : transient_shares) {
            (void)id;
            if (staged.round_id == round_id) {
                return false;
            }
        }
        if (!pending_accounting_already_applied) {
            for (const auto &[key, delta] : pending_round_work) {
                (void)delta;
                if (key.round_id == round_id) {
                    return false;
                }
            }
        }

        Statement pending(
            db,
            "SELECT 1 FROM shares WHERE round_id=?1 "
            "AND status IN ('received','verifying') LIMIT 1");
        pending.bind(round_id, 1);
        if (pending.row()) {
            return false;
        }

        std::string segment_total = "0";
        std::int64_t share_count = 0;
        std::int64_t segment_count = 0;
        Statement segments(
            db,
            "SELECT credited_difficulty_dec,accepted_share_count "
            "FROM round_work_segments WHERE round_id=?1 ORDER BY source,"
            "network_difficulty_dec");
        segments.bind(round_id, 1);
        while (segments.row()) {
            segment_total = add_uint128(segment_total, segments.text(0));
            const std::int64_t count = segments.integer(1);
            require(count > 0 &&
                        share_count <= std::numeric_limits<std::int64_t>::max() - count,
                    "round work segment share count is invalid");
            share_count += count;
            require(segment_count < std::numeric_limits<std::int64_t>::max(),
                    "round work segment count overflow");
            ++segment_count;
        }
        require(segment_total == credited_total,
                "round work segments do not match credited difficulty");
        require(share_count == accepted_total,
                "round work segments do not match accepted share count");

        Statement finalize(
            db,
            "UPDATE rounds SET effort_finalized_unix_us=?1,"
            "finalized_effort_segment_count=?2 WHERE id=?3 AND state='closed' "
            "AND effort_finalized_unix_us IS NULL");
        finalize.bind(std::max(finalized_unix_us, closed_unix_us), 1);
        finalize.bind(segment_count, 2);
        finalize.bind(round_id, 3);
        finalize.done();
        require(sqlite3_changes(db) == 1,
                "round effort changed while being finalized");
        return true;
    }

    void add_bucket_unlocked(std::string_view scope_type, std::int64_t scope_id,
                             std::int64_t second_utc, std::string_view difficulty,
                             HashrateSource source,
                             std::uint64_t accepted_shares_delta = 1)
    {
        require(accepted_shares_delta >= 1 &&
                    accepted_shares_delta <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()),
                "hashrate accepted-share delta is outside SQLite range");
        Statement select(
            db,
            "SELECT credited_difficulty_dec,accepted_shares FROM hashrate_buckets "
            "WHERE scope_type=?1 AND scope_id=?2 AND second_utc=?3 AND source=?4");
        select.bind(scope_type, 1);
        select.bind(scope_id, 2);
        select.bind(second_utc, 3);
        select.bind(to_string(source), 4);
        if (select.row()) {
            const std::string updated = add_uint128(select.text(0), difficulty);
            const std::int64_t accepted_shares = select.integer(1);
            const auto count_delta =
                static_cast<std::int64_t>(accepted_shares_delta);
            require(accepted_shares <=
                        std::numeric_limits<std::int64_t>::max() - count_delta,
                    "hashrate accepted-share counter overflow");
            Statement update(
                db,
                "UPDATE hashrate_buckets SET credited_difficulty_dec=?1,"
                "accepted_shares=?2 WHERE scope_type=?3 AND scope_id=?4 "
                "AND second_utc=?5 AND source=?6");
            update.bind(updated, 1);
            update.bind(accepted_shares + count_delta, 2);
            update.bind(scope_type, 3);
            update.bind(scope_id, 4);
            update.bind(second_utc, 5);
            update.bind(to_string(source), 6);
            update.done();
            require(sqlite3_changes(db) == 1, "hashrate bucket update failed");
            return;
        }

        Statement insert(
            db,
            "INSERT INTO hashrate_buckets(scope_type,scope_id,second_utc,"
            "credited_difficulty_dec,accepted_shares,source) "
            "VALUES(?1,?2,?3,?4,?5,?6)");
        insert.bind(scope_type, 1);
        insert.bind(scope_id, 2);
        insert.bind(second_utc, 3);
        insert.bind(difficulty, 4);
        insert.bind(static_cast<std::int64_t>(accepted_shares_delta), 5);
        insert.bind(to_string(source), 6);
        insert.done();
    }

    [[nodiscard]] static bool valid_retention_reason(
        std::string_view reason) noexcept
    {
        return reason == "high_difficulty" || reason == "candidate" ||
               reason == "security_evidence";
    }

    [[nodiscard]] bool meets_persistence_threshold(
        const std::optional<std::string> &actual_difficulty) const
    {
        if (!actual_difficulty.has_value()) {
            return false;
        }
        require_uint128(*actual_difficulty, "share actual difficulty");
        const std::string threshold =
            std::to_string(options.min_persisted_share_difficulty);
        return actual_difficulty->size() > threshold.size() ||
               (actual_difficulty->size() == threshold.size() &&
               *actual_difficulty >= threshold);
    }

    void remember_persisted_share_alias(
        std::int64_t transient_id, PersistedShareIdentity identity)
    {
        require(transient_id < 0 && identity.share_id > 0 &&
                    identity.round_id > 0,
                "invalid persisted share alias");
        const auto [iterator, inserted] = persisted_share_aliases.emplace(
            transient_id, identity);
        if (!inserted) {
            require(iterator->second.share_id == identity.share_id &&
                        iterator->second.round_id == identity.round_id,
                    "transient share alias changed identity");
            return;
        }
        persisted_share_alias_fifo.push_back(transient_id);
        while (persisted_share_alias_fifo.size() > 4096U) {
            persisted_share_aliases.erase(persisted_share_alias_fifo.front());
            persisted_share_alias_fifo.pop_front();
        }
    }

    void erase_transient_share_unlocked(
        std::map<std::int64_t, StagedShare>::iterator share)
    {
        transient_shares.erase(share);
        pending_transient_shares.store(
            static_cast<std::uint64_t>(transient_shares.size()),
            std::memory_order_release);
    }

    [[nodiscard]] bool round_has_higher_share_height_unlocked(
        std::int64_t round_id, std::int64_t candidate_height) const
    {
        Statement round(
            db, "SELECT max_share_height FROM rounds WHERE id=?1");
        round.bind(round_id, 1);
        require(round.row(), "candidate origin round does not exist");
        if (round.integer(0) > candidate_height) {
            return true;
        }
        for (const auto &[id, staged] : transient_shares) {
            (void)id;
            if (staged.round_id == round_id && staged.value.height.has_value() &&
                *staged.value.height >
                    static_cast<std::uint64_t>(candidate_height)) {
                return true;
            }
        }
        // Belt-and-suspenders protection for a retained row committed just
        // before an older build crashed without flushing its round maximum.
        Statement retained(
            db,
            "SELECT 1 FROM shares WHERE round_id=?1 AND height>?2 LIMIT 1");
        retained.bind(round_id, 1);
        retained.bind(candidate_height, 2);
        return retained.row();
    }

    [[nodiscard]] bool transient_references_connection_unlocked(
        std::int64_t connection_id) const noexcept
    {
        for (const auto &[id, staged] : transient_shares) {
            (void)id;
            if (staged.value.connection_id == connection_id) {
                return true;
            }
        }
        return false;
    }

    void prune_worker_if_orphaned_unlocked(std::int64_t worker_id)
    {
        Statement prune(
            db,
            "DELETE FROM workers WHERE id=?1 "
            "AND NOT EXISTS(SELECT 1 FROM connections WHERE worker_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM shares WHERE worker_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM events WHERE worker_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM hashrate_buckets "
            "WHERE scope_type='worker' AND scope_id=?1)");
        prune.bind(worker_id, 1);
        prune.done();
    }

    void prune_closed_connection_unlocked(std::int64_t connection_id,
                                          std::optional<std::int64_t> worker_id)
    {
        if (transient_references_connection_unlocked(connection_id)) {
            return;
        }
        Statement prune(
            db,
            "DELETE FROM connections WHERE id=?1 AND closed_unix_us IS NOT NULL "
            "AND NOT EXISTS(SELECT 1 FROM shares WHERE connection_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM candidates WHERE connection_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM abuse_events WHERE connection_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM events WHERE connection_id=?1) "
            "AND NOT EXISTS(SELECT 1 FROM hashrate_buckets "
            "WHERE scope_type='connection' AND scope_id=?1)");
        prune.bind(connection_id, 1);
        prune.done();
        if (sqlite3_changes(db) == 1 && worker_id.has_value()) {
            prune_worker_if_orphaned_unlocked(*worker_id);
        }
    }

    void prune_closed_connections_unlocked()
    {
        std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> rows;
        Statement query(
            db,
            "SELECT id,worker_id FROM connections WHERE closed_unix_us IS NOT NULL");
        while (query.row()) {
            rows.emplace_back(
                query.integer(0),
                query.is_null(1)
                    ? std::nullopt
                    : std::optional<std::int64_t>(query.integer(1)));
        }
        for (const auto &[connection_id, worker_id] : rows) {
            prune_closed_connection_unlocked(connection_id, worker_id);
        }
    }

    [[nodiscard]] bool prune_hashrate_unlocked(std::int64_t newest_second)
    {
        if (newest_second < last_hashrate_prune_second + 60) {
            return false;
        }
        Statement prune(
            db, "DELETE FROM hashrate_buckets WHERE second_utc<=?1");
        prune.bind(newest_second - 86400, 1);
        prune.done();
        prune_closed_connections_unlocked();
        return true;
    }

    [[nodiscard]] std::int64_t insert_staged_share_unlocked(
        const StagedShare &staged, std::string_view retention_reason)
    {
        require(valid_retention_reason(retention_reason),
                "invalid share retention reason");
        require(staged.value.status == "received" ||
                    staged.value.status == "verifying",
                "only an active staged share can be persisted");
        Statement statement(
            db,
            "INSERT INTO shares(session_id,round_id,connection_id,worker_id,"
            "job_public_id,template_generation,height,"
            "request_sequence,miner_request_id_type,miner_request_id_text,"
            "received_unix_us,nonce,assigned_difficulty_dec,"
            "network_difficulty_dec,height_is_older,claimed_candidate,"
            "candidate_admission,retention_reason,status,provenance,"
            "verifier_ticket_dec,verifier_seed_id_dec) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
            "?15,?16,?17,?18,?19,?20,?21,?22)");
        statement.bind(staged.value.session_id, 1);
        statement.bind(staged.round_id, 2);
        statement.bind_optional(staged.value.connection_id, 3);
        statement.bind_optional(staged.value.worker_id, 4);
        statement.bind_optional(staged.value.job_public_id, 5);
        statement.bind_optional(staged.value.template_generation, 6);
        if (staged.value.height) statement.bind(static_cast<std::int64_t>(*staged.value.height), 7);
        else statement.bind_null(7);
        statement.bind(
            static_cast<std::int64_t>(staged.value.request_sequence), 8);
        statement.bind_optional(staged.value.miner_request_id_type, 9);
        statement.bind_optional(staged.value.miner_request_id_text, 10);
        statement.bind(staged.value.received_unix_us, 11);
        statement.bind_optional(staged.value.nonce, 12);
        statement.bind_optional(staged.value.assigned_difficulty_dec, 13);
        statement.bind_optional(staged.value.network_difficulty_dec, 14);
        statement.bind(staged.value.height_is_older ? 1 : 0, 15);
        statement.bind(staged.value.claimed_candidate ? 1 : 0, 16);
        statement.bind(staged.value.candidate_admission, 17);
        statement.bind(retention_reason, 18);
        statement.bind(staged.value.status, 19);
        statement.bind(staged.value.provenance, 20);
        statement.bind_optional(staged.verifier_ticket_dec, 21);
        statement.bind_optional(staged.verifier_seed_id_dec, 22);
        statement.done();
        const std::int64_t persisted_id = last_insert_id(db);

        for (const auto &[role, value] : staged.hashes) {
            Statement hash(
                db,
                "INSERT INTO share_hashes(share_id,role,hash,"
                "meets_share_target,meets_network_target) "
                "VALUES(?1,?2,?3,?4,?5)");
            hash.bind(persisted_id, 1);
            hash.bind(role, 2);
            hash.bind(value.hash, 3);
            if (value.meets_share_target.has_value()) {
                hash.bind(*value.meets_share_target ? 1 : 0, 4);
            }
            else {
                hash.bind_null(4);
            }
            if (value.meets_network_target.has_value()) {
                hash.bind(*value.meets_network_target ? 1 : 0, 5);
            }
            else {
                hash.bind_null(5);
            }
            hash.done();
        }
        if (staged.value.height.has_value()) {
            Statement height(
                db,
                "UPDATE rounds SET max_share_height=max(max_share_height,?1) "
                "WHERE id=?2");
            height.bind(static_cast<std::int64_t>(*staged.value.height), 1);
            height.bind(staged.round_id, 2);
            height.done();
            require(sqlite3_changes(db) == 1,
                    "persisted share round does not exist");
        }
        return persisted_id;
    }

    void record_share_total_unlocked(
        std::string_view status, std::string_view provenance,
        std::int64_t completed_unix_us)
    {
        auto &delta = pending_share_totals[
            ShareTotalKey{std::string(status), std::string(provenance)}];
        require(delta.count <
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()),
                "pending share total overflows SQLite INTEGER");
        if (delta.count == 0) {
            delta.first_unix_us = completed_unix_us;
            delta.last_unix_us = completed_unix_us;
        }
        else {
            delta.first_unix_us = std::min(delta.first_unix_us,
                                           completed_unix_us);
            delta.last_unix_us = std::max(delta.last_unix_us,
                                          completed_unix_us);
        }
        ++delta.count;
        const auto prior = pending_accounting_items.load(
            std::memory_order_relaxed);
        require(prior < std::numeric_limits<std::uint64_t>::max(),
                "pending accounting item counter overflow");
        pending_accounting_items.store(prior + 1U,
                                       std::memory_order_release);
        pending_latest_completed_unix_us = std::max(
            pending_latest_completed_unix_us, completed_unix_us);
    }

    void add_share_total_unlocked(std::string_view status,
                                  std::string_view provenance,
                                  std::int64_t completed_unix_us,
                                  std::uint64_t count = 1)
    {
        require(count >= 1 && count <= static_cast<std::uint64_t>(
                                      std::numeric_limits<std::int64_t>::max()),
                "share total delta is outside SQLite range");
        Statement statement(
            db,
            "INSERT INTO share_totals(status,provenance,share_count,"
            "first_unix_us,last_unix_us) VALUES(?1,?2,?3,?4,?4) "
            "ON CONFLICT(status,provenance) DO UPDATE SET "
            "share_count=share_totals.share_count+excluded.share_count,"
            "first_unix_us=min(share_totals.first_unix_us,excluded.first_unix_us),"
            "last_unix_us=max(share_totals.last_unix_us,excluded.last_unix_us)");
        statement.bind(status, 1);
        statement.bind(provenance, 2);
        statement.bind(static_cast<std::int64_t>(count), 3);
        statement.bind(completed_unix_us, 4);
        statement.done();
    }

    void record_work_unlocked(const StagedShare &staged,
                              const ShareAcceptance &acceptance)
    {
        require(staged.value.network_difficulty_dec.has_value(),
                "accepted staged share has no network difficulty");
        const auto add_work = [&](WorkDelta &delta) {
            delta.credited_difficulty_dec = add_uint128(
                delta.credited_difficulty_dec,
                acceptance.assigned_difficulty_dec);
            require(delta.accepted_shares <
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()),
                    "pending accepted-share counter overflow");
            ++delta.accepted_shares;
        };
        const std::int64_t second_utc =
            acceptance.completed_unix_us / 1'000'000;
        add_work(pending_hashrate[HashrateKey{
            "global", 0, second_utc, acceptance.source}]);
        if (staged.value.connection_id.has_value()) {
            add_work(pending_hashrate[HashrateKey{
                "connection", *staged.value.connection_id, second_utc,
                acceptance.source}]);
        }
        if (staged.value.worker_id.has_value()) {
            add_work(pending_hashrate[HashrateKey{
                "worker", *staged.value.worker_id, second_utc,
                acceptance.source}]);
        }
        add_work(pending_round_work[RoundWorkKey{
            staged.round_id, acceptance.source,
            *staged.value.network_difficulty_dec}]);
    }

    [[nodiscard]] AccountingApplyResult apply_accounting_unlocked()
    {
        AccountingApplyResult result;
        for (const auto &[key, delta] : pending_share_totals) {
            add_share_total_unlocked(key.status, key.provenance,
                                     delta.first_unix_us, delta.count);
            if (delta.last_unix_us != delta.first_unix_us) {
                Statement update(db, "UPDATE share_totals SET last_unix_us=max(last_unix_us,?1) WHERE status=?2 AND provenance=?3");
                update.bind(delta.last_unix_us, 1);
                update.bind(key.status, 2);
                update.bind(key.provenance, 3);
                update.done();
            }
        }

        for (const auto &[key, delta] : pending_hashrate) {
            add_bucket_unlocked(
                key.scope_type, key.scope_id, key.second_utc,
                delta.credited_difficulty_dec, key.source,
                delta.accepted_shares);
        }

        std::map<std::int64_t, WorkDelta> round_totals;
        for (const auto &[key, delta] : pending_round_work) {
            add_round_work_segment_unlocked(
                key.round_id, key.source, key.network_difficulty_dec,
                delta.credited_difficulty_dec, delta.accepted_shares);
            auto &total = round_totals[key.round_id];
            total.credited_difficulty_dec = add_uint128(
                total.credited_difficulty_dec,
                delta.credited_difficulty_dec);
            require(total.accepted_shares <=
                        std::numeric_limits<std::uint64_t>::max() -
                            delta.accepted_shares,
                    "pending round accepted-share count overflow");
            total.accepted_shares += delta.accepted_shares;
            result.affected_rounds.insert(key.round_id);
        }
        for (const auto &[round_id, delta] : round_totals) {
            Statement select(
                db,
                "SELECT credited_difficulty_dec,accepted_share_count,"
                "effort_finalized_unix_us FROM rounds WHERE id=?1");
            select.bind(round_id, 1);
            require(select.row(), "pending accounting round does not exist");
            require(select.is_null(2),
                    "pending accounting targets a finalized round");
            const std::string credited = add_uint128(
                select.text(0), delta.credited_difficulty_dec);
            const std::int64_t prior_count = select.integer(1);
            require(delta.accepted_shares <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max() - prior_count),
                    "round accepted-share counter overflow");
            Statement update(
                db,
                "UPDATE rounds SET credited_difficulty_dec=?1,"
                "accepted_share_count=?2 WHERE id=?3 "
                "AND effort_finalized_unix_us IS NULL");
            update.bind(credited, 1);
            update.bind(
                prior_count +
                    static_cast<std::int64_t>(delta.accepted_shares),
                2);
            update.bind(round_id, 3);
            update.done();
            require(sqlite3_changes(db) == 1,
                    "pending accounting round changed during flush");
        }
        for (const auto &[round_id, max_height] : pending_round_max_height) {
            Statement update(
                db,
                "UPDATE rounds SET max_share_height=max(max_share_height,?1) "
                "WHERE id=?2");
            update.bind(max_height, 1);
            update.bind(round_id, 2);
            update.done();
            require(sqlite3_changes(db) == 1,
                    "pending share-height round does not exist");
        }
        if (pending_latest_completed_unix_us > 0) {
            const std::int64_t newest_second =
                pending_latest_completed_unix_us / 1'000'000;
            if (prune_hashrate_unlocked(newest_second)) {
                result.hashrate_pruned_through = newest_second;
            }
        }
        return result;
    }

    void clear_accounting_unlocked() noexcept
    {
        pending_share_totals.clear();
        pending_hashrate.clear();
        pending_round_work.clear();
        pending_round_max_height.clear();
        pending_accounting_items.store(0, std::memory_order_release);
        pending_latest_completed_unix_us = 0;
    }

    void flush_accounting_transaction_unlocked()
    {
        if (pending_accounting_items.load(std::memory_order_acquire) == 0U &&
            pending_round_max_height.empty()) {
            return;
        }
        Transaction transaction(db);
        const AccountingApplyResult applied = apply_accounting_unlocked();
        for (const std::int64_t round_id : applied.affected_rounds) {
            (void)try_finalize_round_unlocked(
                round_id, pending_latest_completed_unix_us,
                true);
        }
        transaction.commit();
        if (applied.hashrate_pruned_through.has_value()) {
            last_hashrate_prune_second = *applied.hashrate_pruned_through;
        }
        clear_accounting_unlocked();
    }

    std::int64_t insert_abuse_event_unlocked(const AbuseEventInsert &event)
    {
        require(event.peer_family == 2 || event.peer_family == 10,
                "abuse-event peer family must be AF_INET or AF_INET6");
        const std::size_t address_size = event.peer_family == 2 ? 4U : 16U;
        require(event.peer_address.size() == address_size,
                "abuse-event peer address has the wrong size");
        require(!event.kind.empty(), "abuse-event kind is empty");
        require_no_nul(event.kind, "abuse-event kind");
        require(event.weight > 0, "abuse-event weight must be positive");
        if (event.detail.has_value()) {
            require_no_nul(*event.detail, "abuse-event detail");
        }

        Statement statement(
            db,
            "INSERT INTO abuse_events(connection_id,share_id,candidate_id,peer_family,"
            "peer_address,kind,weight,created_unix_us,detail) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        statement.bind_optional(event.connection_id, 1);
        statement.bind_optional(event.share_id, 2);
        statement.bind_optional(event.candidate_id, 3);
        statement.bind(event.peer_family, 4);
        statement.bind(event.peer_address, 5);
        statement.bind(event.kind, 6);
        statement.bind(event.weight, 7);
        statement.bind(event.created_unix_us, 8);
        statement.bind_optional(event.detail, 9);
        statement.done();
        return last_insert_id(db);
    }

    struct ResolvedVerdicts {
        std::uint32_t false_candidates{};
        std::uint32_t candidate_mismatches{};
    };

    ResolvedVerdicts resolve_rejected_verdicts_unlocked(
        std::int64_t candidate_id, std::int64_t resolved_unix_us)
    {
        ResolvedVerdicts result;
        const CandidateContext context = candidate_context_unlocked(candidate_id);
        Statement peer(
            db, "SELECT peer_family,peer_address FROM candidates WHERE id=?1");
        peer.bind(candidate_id, 1);
        require(peer.row(), "candidate disappeared while resolving verdicts");
        const int peer_family = static_cast<int>(peer.integer(0));
        const ByteVector peer_address = peer.blob(1);

        for (const std::string_view verdict_kind :
             {std::string_view("false_candidate"),
              std::string_view("candidate_mismatch")}) {
            const std::string abuse_kind = verdict_kind == "false_candidate"
                                               ? "verified_false_candidate"
                                               : "candidate_mismatch";
            Statement pending(
                db,
                "SELECT share_id FROM candidate_verdicts WHERE candidate_id=?1 "
                "AND kind=?2 AND disposition='pending' ORDER BY share_id LIMIT 1");
            pending.bind(candidate_id, 1);
            pending.bind(verdict_kind, 2);
            if (!pending.row()) {
                continue;
            }
            const std::int64_t share_id = pending.integer(0);

            Statement existing(
                db,
                "SELECT id FROM abuse_events WHERE candidate_id=?1 AND kind=?2");
            existing.bind(candidate_id, 1);
            existing.bind(abuse_kind, 2);
            std::int64_t abuse_event_id = 0;
            if (existing.row()) {
                abuse_event_id = existing.integer(0);
            }
            else {
                abuse_event_id = insert_abuse_event_unlocked(AbuseEventInsert{
                    .connection_id = context.connection_id,
                    .share_id = share_id,
                    .candidate_id = candidate_id,
                    .peer_family = peer_family,
                    .peer_address = peer_address,
                    .kind = abuse_kind,
                    .weight = 1,
                    .created_unix_us = resolved_unix_us,
                    .detail = std::nullopt,
                });
            }

            Statement event_used(
                db,
                "SELECT 1 FROM candidate_verdicts WHERE abuse_event_id=?1 LIMIT 1");
            event_used.bind(abuse_event_id, 1);
            if (event_used.row()) {
                // Abuse is scored once per candidate/kind. A second share can
                // carry the same verdict, but the terminal candidate state means
                // it can never become independently actionable. Resolve it as
                // suppressed instead of leaving an immortal pending row.
                Statement suppressed(
                    db,
                    "UPDATE candidate_verdicts SET disposition='suppressed',"
                    "resolved_unix_us=?1 WHERE candidate_id=?2 AND kind=?3 "
                    "AND disposition='pending'");
                suppressed.bind(resolved_unix_us, 1);
                suppressed.bind(candidate_id, 2);
                suppressed.bind(verdict_kind, 3);
                suppressed.done();
                continue;
            }
            Statement actionable(
                db,
                "UPDATE candidate_verdicts SET disposition='actionable',"
                "resolved_unix_us=?1,abuse_event_id=?2 WHERE share_id=?3 AND kind=?4 "
                "AND disposition='pending'");
            actionable.bind(resolved_unix_us, 1);
            actionable.bind(abuse_event_id, 2);
            actionable.bind(share_id, 3);
            actionable.bind(verdict_kind, 4);
            actionable.done();
            if (sqlite3_changes(db) == 1) {
                if (verdict_kind == "false_candidate") {
                    ++result.false_candidates;
                }
                else {
                    ++result.candidate_mismatches;
                }
            }
            Statement suppress_rest(
                db,
                "UPDATE candidate_verdicts SET disposition='suppressed',"
                "resolved_unix_us=?1 WHERE candidate_id=?2 AND kind=?3 "
                "AND disposition='pending'");
            suppress_rest.bind(resolved_unix_us, 1);
            suppress_rest.bind(candidate_id, 2);
            suppress_rest.bind(verdict_kind, 3);
            suppress_rest.done();
        }
        return result;
    }

    DatabaseOptions options;
    sqlite3 *db{};
    mutable WriterScheduler mutex;
    std::int64_t next_transient_share_id{-1};
    std::map<std::int64_t, StagedShare> transient_shares;
    std::map<std::int64_t, PersistedShareIdentity> persisted_share_aliases;
    std::deque<std::int64_t> persisted_share_alias_fifo;
    std::map<ShareTotalKey, ShareTotalDelta> pending_share_totals;
    std::map<HashrateKey, WorkDelta> pending_hashrate;
    std::map<RoundWorkKey, WorkDelta> pending_round_work;
    std::map<std::int64_t, std::int64_t> pending_round_max_height;
    std::atomic<std::uint64_t> pending_accounting_items{0};
    std::atomic<std::uint64_t> pending_transient_shares{0};
    std::int64_t pending_latest_completed_unix_us{};
    std::int64_t last_hashrate_prune_second{};
};

Database::Database(DatabaseOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

Database::~Database() = default;

const DatabaseOptions &Database::options() const noexcept
{
    return impl_->options;
}

DatabaseWriterStats Database::writer_stats() const noexcept
{
    DatabaseWriterStats result = impl_->mutex.stats();
    result.pending_accounting_items = impl_->pending_accounting_items.load(
        std::memory_order_acquire);
    result.pending_transient_shares = impl_->pending_transient_shares.load(
        std::memory_order_acquire);
    return result;
}

DatabasePragmas Database::pragmas() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->read_pragmas_unlocked();
}

std::uint32_t Database::schema_version() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(
        impl_->db, "SELECT value FROM schema_meta WHERE key='schema_version'");
    require(statement.row(), "database schema_version is missing");
    const std::string value = statement.text(0);
    require(value == "3", "unsupported database schema version (expected 3)");
    return 3;
}

std::int64_t Database::start_session(const SessionStart &session)
{
    std::scoped_lock lock(impl_->mutex);
    require(!session.version.empty(), "server version is empty");
    require_no_nul(session.version, "server version");
    if (session.verifier_commit.has_value()) {
        require_no_nul(*session.verifier_commit, "verifier commit");
    }

    Transaction transaction(impl_->db);
    Statement insert(
        impl_->db,
        "INSERT INTO server_sessions(public_id,started_unix_us,version,"
        "verifier_commit,clean_shutdown) VALUES(?1,?2,?3,?4,0)");
    insert.bind(session.public_id, 1);
    insert.bind(session.started_unix_us, 2);
    insert.bind(session.version, 3);
    insert.bind_optional(session.verifier_commit, 4);
    insert.done();
    const std::int64_t id = last_insert_id(impl_->db);

    std::optional<std::int64_t> newly_opened_round;
    Statement open_round(impl_->db, "SELECT id FROM rounds WHERE state='open'");
    if (!open_round.row()) {
        Statement create(
            impl_->db,
            "INSERT INTO rounds(opened_unix_us,state) VALUES(?1,'open')");
        create.bind(session.started_unix_us, 1);
        create.done();
        newly_opened_round = last_insert_id(impl_->db);
    }
    else {
        require(!open_round.row(), "database contains multiple open rounds");
    }

    impl_->insert_event_unlocked(EventInsert{
        .session_id = id,
        .created_unix_us = session.started_unix_us,
        .type = "server_started",
        .connection_id = std::nullopt,
        .worker_id = std::nullopt,
        .template_generation = std::nullopt,
        .job_public_id = std::nullopt,
        .share_id = std::nullopt,
        .candidate_id = std::nullopt,
        .round_id = std::nullopt,
        .payload_json = std::string(kEmptyPayload),
    });
    if (newly_opened_round.has_value()) {
        impl_->insert_event_unlocked(EventInsert{
            .session_id = id,
            .created_unix_us = session.started_unix_us,
            .type = "round_opened",
            .connection_id = std::nullopt,
            .worker_id = std::nullopt,
            .template_generation = std::nullopt,
            .job_public_id = std::nullopt,
            .share_id = std::nullopt,
            .candidate_id = std::nullopt,
            .round_id = *newly_opened_round,
            .payload_json = std::string(kEmptyPayload),
        });
    }
    transaction.commit();
    return id;
}

InterruptedRuntimeRecovery Database::recover_interrupted_runtime(
    std::int64_t recovered_unix_us)
{
    require(recovered_unix_us > 0, "runtime recovery time must be positive");

    InterruptedRuntimeRecovery result;
    std::scoped_lock lock(impl_->mutex);
    Transaction transaction(impl_->db);
    std::vector<std::int64_t> pending_rounds;
    Statement pending_round_query(
        impl_->db,
        "SELECT id FROM rounds WHERE state='closed' "
        "AND effort_finalized_unix_us IS NULL ORDER BY id");
    while (pending_round_query.row()) {
        pending_rounds.push_back(pending_round_query.integer(0));
    }
    Statement cancelled_totals(
        impl_->db,
        "SELECT count(*),"
        "min(CASE WHEN received_unix_us>?1 THEN received_unix_us ELSE ?1 END),"
        "max(CASE WHEN received_unix_us>?1 THEN received_unix_us ELSE ?1 END) "
        "FROM shares WHERE status IN ('received','verifying')");
    cancelled_totals.bind(recovered_unix_us, 1);
    require(cancelled_totals.row(), "could not count interrupted shares");
    if (cancelled_totals.integer(0) > 0) {
        const std::uint64_t count = static_cast<std::uint64_t>(
            cancelled_totals.integer(0));
        const std::int64_t first = cancelled_totals.integer(1);
        const std::int64_t last = cancelled_totals.integer(2);
        impl_->add_share_total_unlocked("cancelled", "pending", first, count);
        if (last != first) {
            Statement update_last(
                impl_->db,
                "UPDATE share_totals SET last_unix_us=max(last_unix_us,?1) "
                "WHERE status='cancelled' AND provenance='pending'");
            update_last.bind(last, 1);
            update_last.done();
        }
    }
    Statement cancel_shares(
        impl_->db,
        "UPDATE shares SET status='cancelled',provenance='pending',"
        "completed_unix_us=CASE WHEN received_unix_us>?1 THEN received_unix_us ELSE ?1 END,"
        "retention_reason=coalesce(retention_reason,'security_evidence'),"
        "error_code='process_restarted',"
        "error_message='Server restarted before share completion' "
        "WHERE status IN ('received','verifying')");
    cancel_shares.bind(recovered_unix_us, 1);
    cancel_shares.done();
    Statement close_connections(
        impl_->db,
        "UPDATE connections SET closed_unix_us="
        "CASE WHEN opened_unix_us>?1 THEN opened_unix_us ELSE ?1 END,"
        "close_reason='process_restarted' WHERE closed_unix_us IS NULL");
    close_connections.bind(recovered_unix_us, 1);
    close_connections.done();
    result.connections_closed = static_cast<std::uint64_t>(
        sqlite3_changes(impl_->db));
    impl_->prune_closed_connections_unlocked();

    Statement stop_sessions(
        impl_->db,
        "UPDATE server_sessions SET stopped_unix_us="
        "CASE WHEN started_unix_us>?1 THEN started_unix_us ELSE ?1 END,"
        "clean_shutdown=0 WHERE stopped_unix_us IS NULL");
    stop_sessions.bind(recovered_unix_us, 1);
    stop_sessions.done();
    result.sessions_stopped = static_cast<std::uint64_t>(
        sqlite3_changes(impl_->db));
    for (const std::int64_t round_id : pending_rounds) {
        (void)impl_->try_finalize_round_unlocked(round_id, recovered_unix_us);
    }
    transaction.commit();
    return result;
}

void Database::finish_session(std::int64_t session_id,
                              std::int64_t stopped_unix_us,
                              bool clean_shutdown)
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(
        impl_->db,
        "UPDATE server_sessions SET stopped_unix_us=?1,clean_shutdown=?2 "
        "WHERE id=?3 AND stopped_unix_us IS NULL");
    statement.bind(stopped_unix_us, 1);
    statement.bind(clean_shutdown ? 1 : 0, 2);
    statement.bind(session_id, 3);
    statement.done();
    require(sqlite3_changes(impl_->db) == 1, "server session is absent or already stopped");
}

std::int64_t Database::ensure_open_round(std::int64_t opened_unix_us)
{
    std::scoped_lock lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement select(impl_->db, "SELECT id FROM rounds WHERE state='open'");
    if (select.row()) {
        const std::int64_t id = select.integer(0);
        require(!select.row(), "database contains multiple open rounds");
        transaction.commit();
        return id;
    }
    Statement insert(
        impl_->db, "INSERT INTO rounds(opened_unix_us,state) VALUES(?1,'open')");
    insert.bind(opened_unix_us, 1);
    insert.done();
    const std::int64_t id = last_insert_id(impl_->db);
    transaction.commit();
    return id;
}

std::int64_t Database::current_open_round_id() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(impl_->db, "SELECT id FROM rounds WHERE state='open'");
    require(statement.row(), "database has no open round");
    const std::int64_t id = statement.integer(0);
    require(!statement.row(), "database contains multiple open rounds");
    return id;
}

std::optional<std::uint64_t> Database::latest_accepted_height() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(
        impl_->db,
        "SELECT accepted_height FROM rounds WHERE state='closed' "
        "ORDER BY id DESC LIMIT 1");
    if (!statement.row()) {
        return std::nullopt;
    }
    const std::int64_t height = statement.integer(0);
    require(height > 0, "latest accepted round height is invalid");
    return static_cast<std::uint64_t>(height);
}

std::int64_t Database::insert_event(const EventInsert &event)
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->insert_event_unlocked(event);
}

std::int64_t Database::event_high_water_mark() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(impl_->db, "SELECT coalesce(max(id),0) FROM events");
    require(statement.row(), "could not read event high-water mark");
    return statement.integer(0);
}

std::vector<PersistedEvent> Database::load_events_after(
    std::int64_t last_event_id, std::size_t limit) const
{
    std::scoped_lock lock(impl_->mutex);
    require(last_event_id >= 0, "event cursor ID is negative");
    require(limit >= 1 && limit <= 10000,
            "event load limit is outside 1..10000");
    Statement statement(
        impl_->db,
        "SELECT e.id,ss.public_id,e.created_unix_us,e.type,cn.public_id,"
        "e.worker_id,e.template_generation,e.job_public_id,e.share_id,e.candidate_id,"
        "e.round_id,e.payload_json FROM events e "
        "JOIN server_sessions ss ON ss.id=e.session_id "
        "LEFT JOIN connections cn ON cn.id=e.connection_id "
        "WHERE e.id>?1 ORDER BY e.id ASC LIMIT ?2");
    statement.bind(last_event_id, 1);
    statement.bind(static_cast<std::int64_t>(limit), 2);
    std::vector<PersistedEvent> result;
    result.reserve(limit);
    while (statement.row()) {
        PersistedEvent value;
        value.id = statement.integer(0);
        value.session_public_id = exact_array<16>(statement.blob(1),
                                                  "event session public ID");
        value.created_unix_us = statement.integer(2);
        value.type = statement.text(3);
        if (!statement.is_null(4)) {
            value.connection_public_id = exact_array<16>(
                statement.blob(4), "event connection public ID");
        }
        if (!statement.is_null(5)) value.worker_id = statement.integer(5);
        if (!statement.is_null(6)) value.template_generation = statement.integer(6);
        if (!statement.is_null(7)) {
            value.job_public_id = exact_array<16>(statement.blob(7),
                                                  "event job public ID");
        }
        if (!statement.is_null(8)) value.share_id = statement.integer(8);
        if (!statement.is_null(9)) value.candidate_id = statement.integer(9);
        if (!statement.is_null(10)) value.round_id = statement.integer(10);
        value.payload_json = statement.text(11);
        result.push_back(std::move(value));
    }
    return result;
}

std::int64_t Database::upsert_worker(const WorkerInsert &worker)
{
    std::scoped_lock lock(impl_->mutex);
    validate_worker(worker);

    Transaction transaction(impl_->db);
    const std::int64_t id = upsert_worker_unlocked(impl_->db, worker);
    transaction.commit();
    return id;
}

std::int64_t Database::insert_connection(const ConnectionInsert &connection)
{
    std::scoped_lock lock(impl_->mutex);
    require(connection.session_id > 0, "connection session ID must be positive");
    require(connection.peer_family == 2 || connection.peer_family == 10,
            "connection peer family must be AF_INET or AF_INET6");
    const std::size_t expected_address_size = connection.peer_family == 2 ? 4U : 16U;
    require(connection.peer_address.size() == expected_address_size,
            "connection peer address has the wrong size");
    require(connection.peer_port >= 0 && connection.peer_port <= 65535,
            "connection peer port is invalid");
    require(!connection.listen_address.empty(), "connection listen address is empty");
    require_no_nul(connection.listen_address, "connection listen address");
    require_no_nul(connection.agent, "connection agent");

    Statement statement(
        impl_->db,
        "INSERT INTO connections(public_id,session_id,worker_id,peer_family,"
        "peer_address,peer_port,listen_address,agent,opened_unix_us) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    statement.bind(connection.public_id, 1);
    statement.bind(connection.session_id, 2);
    statement.bind_optional(connection.worker_id, 3);
    statement.bind(connection.peer_family, 4);
    statement.bind(connection.peer_address, 5);
    statement.bind(connection.peer_port, 6);
    statement.bind(connection.listen_address, 7);
    statement.bind(connection.agent, 8);
    statement.bind(connection.opened_unix_us, 9);
    statement.done();
    return last_insert_id(impl_->db);
}

std::int64_t Database::upsert_worker_and_authenticate_connection(
    std::int64_t connection_id, const WorkerInsert &worker,
    std::string_view agent, std::int64_t authenticated_unix_us)
{
    std::scoped_lock lock(impl_->mutex);
    require(connection_id > 0, "connection ID must be positive");
    validate_worker(worker);
    require_no_nul(agent, "connection agent");

    Transaction transaction(impl_->db);
    const std::int64_t worker_id = upsert_worker_unlocked(impl_->db, worker);
    authenticate_connection_unlocked(
        impl_->db, connection_id, worker_id, agent, authenticated_unix_us);
    transaction.commit();
    return worker_id;
}

void Database::authenticate_connection(std::int64_t connection_id,
                                       std::int64_t worker_id,
                                       std::string_view agent,
                                       std::int64_t authenticated_unix_us)
{
    std::scoped_lock lock(impl_->mutex);
    require(connection_id > 0 && worker_id > 0,
            "connection/worker IDs must be positive");
    require_no_nul(agent, "connection agent");
    authenticate_connection_unlocked(
        impl_->db, connection_id, worker_id, agent, authenticated_unix_us);
}

bool Database::update_connection_last_sent_height(
    std::int64_t connection_id, std::uint64_t height)
{
    std::scoped_lock lock(impl_->mutex);
    require(connection_id > 0, "connection ID must be positive");
    require_i64(height, "connection last-sent height");
    Statement update(
        impl_->db,
        "UPDATE connections SET last_sent_height=?1 WHERE id=?2 "
        "AND closed_unix_us IS NULL AND last_sent_height!=?1");
    update.bind(static_cast<std::int64_t>(height), 1);
    update.bind(connection_id, 2);
    update.done();
    if (sqlite3_changes(impl_->db) == 1) {
        return true;
    }
    Statement existing(
        impl_->db,
        "SELECT last_sent_height FROM connections WHERE id=?1 "
        "AND closed_unix_us IS NULL");
    existing.bind(connection_id, 1);
    require(existing.row(), "connection is absent or closed");
    require(existing.integer(0) == static_cast<std::int64_t>(height),
            "connection last-sent height update failed");
    return false;
}

bool Database::close_connection(std::int64_t connection_id,
                                std::int64_t closed_unix_us,
                                std::string_view reason)
{
    std::scoped_lock lock(impl_->mutex);
    require(connection_id > 0, "connection ID must be positive");
    require(!reason.empty(), "connection close reason is empty");
    require_no_nul(reason, "connection close reason");
    // Materialize any pending connection/worker buckets before deciding
    // whether their scope identities can be pruned.
    impl_->flush_accounting_transaction_unlocked();
    Transaction transaction(impl_->db);
    Statement worker(impl_->db, "SELECT worker_id FROM connections WHERE id=?1");
    worker.bind(connection_id, 1);
    if (!worker.row()) {
        transaction.commit();
        return false;
    }
    std::optional<std::int64_t> worker_id;
    if (!worker.is_null(0)) worker_id = worker.integer(0);
    Statement statement(
        impl_->db,
        "UPDATE connections SET closed_unix_us=?1,close_reason=?2 "
        "WHERE id=?3 AND closed_unix_us IS NULL");
    statement.bind(closed_unix_us, 1);
    statement.bind(reason, 2);
    statement.bind(connection_id, 3);
    statement.done();
    const bool changed = sqlite3_changes(impl_->db) == 1;
    if (changed) {
        impl_->prune_closed_connection_unlocked(connection_id, worker_id);
    }
    transaction.commit();
    return changed;
}

std::int64_t Database::insert_share(const ShareInsert &share)
{
    std::scoped_lock lock(impl_->mutex);
    require(share.session_id > 0, "share session ID must be positive");
    require(!share.connection_id || *share.connection_id > 0,
            "share connection ID must be positive when present");
    require(!share.worker_id || *share.worker_id > 0,
            "share worker ID must be positive when present");
    require(!share.template_generation || *share.template_generation > 0,
            "share template generation must be positive when present");
    require(!share.height || *share.height > 0,
            "share height must be positive when present");
    if (share.height.has_value()) {
        require_i64(*share.height, "share height");
    }
    require(share.request_sequence >= 1, "share request sequence must be positive");
    require_i64(share.request_sequence, "share request sequence");
    require(share.miner_request_id_type.has_value() ==
                share.miner_request_id_text.has_value(),
            "share request ID type and value must both be present or absent");
    if (share.miner_request_id_type.has_value()) {
        require(*share.miner_request_id_type == "integer" ||
                    *share.miner_request_id_type == "string",
                "share request ID type is invalid");
        require_no_nul(*share.miner_request_id_text, "share request ID");
    }
    if (share.assigned_difficulty_dec.has_value()) {
        require_uint128(*share.assigned_difficulty_dec, "share assigned difficulty",
                        false);
    }
    if (share.network_difficulty_dec.has_value()) {
        require_uint128(*share.network_difficulty_dec, "share network difficulty",
                        false);
    }
    require_no_nul(share.candidate_admission, "candidate admission");
    require_no_nul(share.status, "share status");
    require_no_nul(share.provenance, "share provenance");
    require(share.status == "received" || share.status == "verifying",
            "new transient share must be active");

    if (share.connection_id) {
        Statement connection(
            impl_->db,
            "SELECT session_id,worker_id,closed_unix_us FROM connections WHERE id=?1");
        connection.bind(*share.connection_id, 1);
        require(connection.row(), "share connection does not exist");
        require(connection.integer(0) == share.session_id,
                "share session differs from its connection");
        if (share.worker_id) {
            require(!connection.is_null(1) &&
                        connection.integer(1) == *share.worker_id,
                    "share worker differs from its connection");
        }
        require(connection.is_null(2), "share connection is already closed");
    }

    Statement round(impl_->db, "SELECT id FROM rounds WHERE state='open'");
    require(round.row(), "share insert found no open round");
    const std::int64_t round_id = round.integer(0);
    require(!round.row(), "share insert found multiple open rounds");

    require(impl_->next_transient_share_id !=
                std::numeric_limits<std::int64_t>::min(),
            "transient share ID space is exhausted");
    const std::int64_t id = impl_->next_transient_share_id--;
    const auto [iterator, inserted] = impl_->transient_shares.emplace(
        id, StagedShare{
                .value = share,
                .round_id = round_id,
                .verifier_ticket_dec = std::nullopt,
                .verifier_seed_id_dec = std::nullopt,
                .hashes = {},
            });
    (void)iterator;
    require(inserted, "transient share ID collision");
    if (share.height.has_value()) {
        auto &max_height = impl_->pending_round_max_height[round_id];
        max_height = std::max(
            max_height, static_cast<std::int64_t>(*share.height));
    }
    impl_->pending_transient_shares.store(
        static_cast<std::uint64_t>(impl_->transient_shares.size()),
        std::memory_order_release);
    return id;
}

PersistedShareIdentity Database::ensure_share_persisted(
    std::int64_t share_id, std::string_view retention_reason)
{
    PriorityWriterLock lock(impl_->mutex);
    require(impl_->valid_retention_reason(retention_reason),
            "invalid share retention reason");
    require(share_id != 0, "share ID must be nonzero");
    if (share_id > 0) {
        Statement update(
            impl_->db,
            "UPDATE shares SET retention_reason=CASE "
            "WHEN retention_reason IS NULL OR "
            "retention_reason='high_difficulty' THEN ?1 "
            "ELSE retention_reason END WHERE id=?2");
        update.bind(retention_reason, 1);
        update.bind(share_id, 2);
        update.done();
        require(sqlite3_changes(impl_->db) == 1,
                "persisted share does not exist");
        Statement round(impl_->db, "SELECT round_id FROM shares WHERE id=?1");
        round.bind(share_id, 1);
        require(round.row(), "persisted share does not exist");
        return {.share_id = share_id, .round_id = round.integer(0)};
    }

    if (const auto alias = impl_->persisted_share_aliases.find(share_id);
        alias != impl_->persisted_share_aliases.end()) {
        Statement update(
            impl_->db,
            "UPDATE shares SET retention_reason=CASE "
            "WHEN retention_reason IS NULL OR "
            "retention_reason='high_difficulty' THEN ?1 "
            "ELSE retention_reason END WHERE id=?2");
        update.bind(retention_reason, 1);
        update.bind(alias->second.share_id, 2);
        update.done();
        require(sqlite3_changes(impl_->db) == 1,
                "persisted alias target does not exist");
        return alias->second;
    }

    const auto found = impl_->transient_shares.find(share_id);
    require(found != impl_->transient_shares.end(),
            "transient share does not exist");
    Transaction transaction(impl_->db);
    const std::int64_t persisted_id = impl_->insert_staged_share_unlocked(
        found->second, retention_reason);
    transaction.commit();
    const std::int64_t round_id = found->second.round_id;
    impl_->erase_transient_share_unlocked(found);
    const PersistedShareIdentity identity{
        .share_id = persisted_id,
        .round_id = round_id,
    };
    impl_->remember_persisted_share_alias(share_id, identity);
    return identity;
}

void Database::flush_accounting()
{
    std::scoped_lock lock(impl_->mutex);
    impl_->flush_accounting_transaction_unlocked();
}

CandidateJournalResult Database::journal_candidate(const CandidateJournal &candidate)
{
    PriorityWriterLock lock(impl_->mutex);
    require(!candidate.first_share_id || *candidate.first_share_id > 0,
            "candidate share correlation must be positive when present");
    require(candidate.session_id > 0 && candidate.round_id > 0 &&
                candidate.template_generation > 0 && candidate.connection_id > 0,
            "candidate references must be positive");
    require(candidate.height > 0, "candidate height must be positive");
    require_i64(candidate.height, "candidate height");
    require(candidate.peer_family == 2 || candidate.peer_family == 10,
            "candidate peer family must be AF_INET or AF_INET6");
    const std::size_t peer_size = candidate.peer_family == 2 ? 4U : 16U;
    require(candidate.peer_address.size() == peer_size,
            "candidate peer address has the wrong size");
    require(!candidate.frozen_block_blob.empty(), "candidate frozen block is empty");
    require(candidate.max_attempts >= 1 && candidate.max_attempts <= 4,
            "candidate max attempts is outside 1..4");

    if (candidate.first_share_id) {
        Statement origin(
            impl_->db,
            "SELECT session_id,round_id,connection_id,job_public_id,"
            "template_generation,height FROM shares WHERE id=?1");
        origin.bind(*candidate.first_share_id, 1);
        require(origin.row(), "candidate share correlation does not exist");
        require(origin.integer(0) == candidate.session_id &&
                    origin.integer(1) == candidate.round_id &&
                    !origin.is_null(2) &&
                    origin.integer(2) == candidate.connection_id &&
                    !origin.is_null(3) &&
                    exact_array<16>(origin.blob(3), "candidate share job public ID") ==
                        candidate.job_public_id &&
                    !origin.is_null(4) &&
                    origin.integer(4) == candidate.template_generation &&
                    !origin.is_null(5) &&
                    origin.integer(5) == static_cast<std::int64_t>(candidate.height),
                "candidate snapshot differs from its share correlation");
    }

    impl_->flush_accounting_transaction_unlocked();
    Transaction transaction(impl_->db);
    Statement existing_candidate(
        impl_->db,
        "SELECT 1 FROM candidates WHERE candidate_key=?1");
    existing_candidate.bind(candidate.candidate_key, 1);
    const bool already_journaled = existing_candidate.row();
    if (!already_journaled) {
        Statement origin_round(
            impl_->db, "SELECT state FROM rounds WHERE id=?1");
        origin_round.bind(candidate.round_id, 1);
        require(origin_round.row(), "candidate origin round does not exist");
        const std::string origin_state = origin_round.text(0);
        require(origin_state == "open" || origin_state == "closed",
                "candidate origin round has an invalid state");
        if (origin_state != "open" ||
            impl_->round_has_higher_share_height_unlocked(
                candidate.round_id,
                static_cast<std::int64_t>(candidate.height))) {
            return CandidateJournalResult{
                .candidate_id = 0,
                .inserted = false,
                .state = CandidateState::journaled,
                .round_contaminated = true,
            };
        }
    }

    Statement insert(
        impl_->db,
        "INSERT INTO candidates(candidate_key,session_id,round_id,first_share_id,"
        "job_public_id,template_generation,connection_id,height,peer_family,"
        "peer_address,frozen_block_blob,miner_tx_hash,"
        "expected_block_id,state,max_attempts,created_unix_us,updated_unix_us) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'journaled',?14,?15,?15) "
        "ON CONFLICT(candidate_key) DO NOTHING");
    insert.bind(candidate.candidate_key, 1);
    insert.bind(candidate.session_id, 2);
    insert.bind(candidate.round_id, 3);
    insert.bind_optional(candidate.first_share_id, 4);
    insert.bind(candidate.job_public_id, 5);
    insert.bind(candidate.template_generation, 6);
    insert.bind(candidate.connection_id, 7);
    insert.bind(static_cast<std::int64_t>(candidate.height), 8);
    insert.bind(candidate.peer_family, 9);
    insert.bind(candidate.peer_address, 10);
    insert.bind(candidate.frozen_block_blob, 11);
    insert.bind(candidate.miner_tx_hash, 12);
    insert.bind_optional(candidate.expected_block_id, 13);
    insert.bind(static_cast<int>(candidate.max_attempts), 14);
    insert.bind(candidate.created_unix_us, 15);
    insert.done();
    const bool inserted = sqlite3_changes(impl_->db) == 1;

    Statement select(
        impl_->db,
        "SELECT id,state,frozen_block_blob,miner_tx_hash,session_id,round_id,"
        "job_public_id,template_generation,connection_id,height FROM candidates "
        "WHERE candidate_key=?1");
    select.bind(candidate.candidate_key, 1);
    require(select.row(), "candidate insert/conflict row disappeared");
    const std::int64_t candidate_id = select.integer(0);
    const CandidateState state = parse_candidate_state(select.text(1));
    require(select.blob(2) == candidate.frozen_block_blob,
            "candidate key collision has different frozen bytes");
    require(exact_array<32>(select.blob(3), "candidate miner transaction hash") ==
                candidate.miner_tx_hash,
            "candidate key collision has different miner transaction hash");
    require(select.integer(4) == candidate.session_id &&
                select.integer(5) == candidate.round_id &&
                exact_array<16>(select.blob(6), "stored candidate job public ID") ==
                    candidate.job_public_id &&
                select.integer(7) == candidate.template_generation &&
                select.integer(8) == candidate.connection_id &&
                select.integer(9) == static_cast<std::int64_t>(candidate.height),
            "candidate key collision has different snapshotted context");

    if (candidate.first_share_id) {
      Statement attach(
        impl_->db,
        "UPDATE shares SET candidate_id=?1,candidate_admission="
        "CASE WHEN claimed_candidate=1 THEN ?2 ELSE candidate_admission END "
        "WHERE id=?3 AND (candidate_id IS NULL OR candidate_id=?1)");
    attach.bind(candidate_id, 1);
    attach.bind(inserted ? "admitted" : "existing", 2);
    attach.bind(*candidate.first_share_id, 3);
    attach.done();
      require(sqlite3_changes(impl_->db) == 1,
            "candidate share is absent or attached to another candidate");
    }

    if (inserted) {
        Statement link(
            impl_->db,
            "UPDATE candidate_verdicts SET candidate_id=?1 "
            "WHERE candidate_key=?2 AND candidate_id IS NULL AND disposition='pending'");
        link.bind(candidate_id, 1);
        link.bind(candidate.candidate_key, 2);
        link.done();

        const auto context = impl_->candidate_context_unlocked(candidate_id);
        impl_->insert_candidate_event_unlocked(candidate_id, context,
                                                candidate.created_unix_us,
                                                "candidate_journaled");
    }
    transaction.commit();
    return CandidateJournalResult{
        .candidate_id = candidate_id,
        .inserted = inserted,
        .state = state,
        .round_contaminated = false,
    };
}

std::optional<CandidateJournalResult> Database::find_candidate_by_key(
    const Hash32 &candidate_key) const
{
    PriorityWriterLock lock(impl_->mutex);
    Statement statement(
        impl_->db, "SELECT id,state FROM candidates WHERE candidate_key=?1");
    statement.bind(candidate_key, 1);
    if (!statement.row()) {
        return std::nullopt;
    }
    return CandidateJournalResult{
        .candidate_id = statement.integer(0),
        .inserted = false,
        .state = parse_candidate_state(statement.text(1)),
        .round_contaminated = false,
    };
}

void Database::attach_share_to_candidate(std::int64_t share_id,
                                         std::int64_t candidate_id,
                                         std::string_view admission)
{
    PriorityWriterLock lock(impl_->mutex);
    require(share_id > 0 && candidate_id > 0,
            "share/candidate IDs must be positive");
    static constexpr std::array<std::string_view, 2> allowed{
        "existing", "admitted",
    };
    require(std::find(allowed.begin(), allowed.end(), admission) != allowed.end(),
            "candidate attachment admission must be existing or admitted");
    Statement statement(
        impl_->db,
        "UPDATE shares SET candidate_id=?1,candidate_admission=?2 WHERE id=?3 "
        "AND (candidate_id IS NULL OR candidate_id=?1)");
    statement.bind(candidate_id, 1);
    statement.bind(admission, 2);
    statement.bind(share_id, 3);
    statement.done();
    require(sqlite3_changes(impl_->db) == 1,
            "share is absent or attached to another candidate");
}

std::int64_t Database::start_candidate_attempt(
    std::int64_t candidate_id, std::uint32_t attempt_number,
    std::int64_t rpc_request_id, std::int64_t started_unix_us)
{
    PriorityWriterLock lock(impl_->mutex);
    require(candidate_id > 0, "candidate ID must be positive");
    require(attempt_number >= 1 && attempt_number <= 4,
            "candidate attempt number is outside 1..4");

    Transaction transaction(impl_->db);
    Statement candidate(
        impl_->db,
        "SELECT state,attempt_count,max_attempts FROM candidates WHERE id=?1");
    candidate.bind(candidate_id, 1);
    require(candidate.row(), "candidate does not exist");
    const CandidateState state = parse_candidate_state(candidate.text(0));
    const auto attempt_count = candidate.integer(1);
    const auto max_attempts = candidate.integer(2);
    require(state == CandidateState::journaled || state == CandidateState::retry_wait ||
                state == CandidateState::dispatching,
            "candidate is not dispatchable");
    require(static_cast<std::int64_t>(attempt_number) <= max_attempts,
            "candidate attempt exceeds snapshotted maximum");

    Statement existing(
        impl_->db,
        "SELECT id,rpc_request_id,started_unix_us,classification "
        "FROM candidate_attempts WHERE candidate_id=?1 AND attempt_number=?2");
    existing.bind(candidate_id, 1);
    existing.bind(static_cast<int>(attempt_number), 2);
    if (existing.row()) {
        require(existing.integer(1) == rpc_request_id &&
                    existing.integer(2) == started_unix_us &&
                    existing.text(3) == "dispatching",
                "candidate attempt number was already used differently");
        const std::int64_t id = existing.integer(0);
        transaction.commit();
        return id;
    }

    require(static_cast<std::int64_t>(attempt_number) == attempt_count + 1,
            "candidate attempts must be started sequentially");
    Statement insert(
        impl_->db,
        "INSERT INTO candidate_attempts(candidate_id,attempt_number,rpc_request_id,"
        "started_unix_us,classification) VALUES(?1,?2,?3,?4,'dispatching')");
    insert.bind(candidate_id, 1);
    insert.bind(static_cast<int>(attempt_number), 2);
    insert.bind(rpc_request_id, 3);
    insert.bind(started_unix_us, 4);
    insert.done();
    const std::int64_t id = last_insert_id(impl_->db);

    Statement update(
        impl_->db,
        "UPDATE candidates SET state='dispatching',attempt_count=?1,updated_unix_us=?2 "
        "WHERE id=?3 AND state IN ('journaled','retry_wait','dispatching')");
    update.bind(static_cast<int>(attempt_number), 1);
    update.bind(started_unix_us, 2);
    update.bind(candidate_id, 3);
    update.done();
    require(sqlite3_changes(impl_->db) == 1, "candidate became nondispatchable");

    const auto context = impl_->candidate_context_unlocked(candidate_id);
    impl_->insert_candidate_event_unlocked(candidate_id, context, started_unix_us,
                                            "candidate_attempt");
    transaction.commit();
    return id;
}

CandidateAttemptResult Database::finish_candidate_attempt(
    std::int64_t candidate_id, std::uint32_t attempt_number,
    const CandidateAttemptCompletion &completion)
{
    PriorityWriterLock lock(impl_->mutex);
    impl_->flush_accounting_transaction_unlocked();
    require(candidate_id > 0, "candidate ID must be positive");
    require(attempt_number >= 1 && attempt_number <= 4,
            "candidate attempt number is outside 1..4");
    if (completion.daemon_status.has_value()) {
        require_no_nul(*completion.daemon_status, "daemon status");
    }
    if (completion.response_excerpt.has_value()) {
        require_no_nul(*completion.response_excerpt, "daemon response excerpt");
        require(completion.response_excerpt->size() <= 4096,
                "daemon response excerpt exceeds 4096 bytes");
    }

    Transaction transaction(impl_->db);
    Statement candidate(
        impl_->db,
        "SELECT state,attempt_count,max_attempts,had_indeterminate "
        "FROM candidates WHERE id=?1");
    candidate.bind(candidate_id, 1);
    require(candidate.row(), "candidate does not exist");
    CandidateState state = parse_candidate_state(candidate.text(0));
    const auto attempt_count = candidate.integer(1);
    const auto max_attempts = candidate.integer(2);
    bool had_indeterminate = candidate.integer(3) == 1;
    require(attempt_count == static_cast<std::int64_t>(attempt_number),
            "candidate attempt is not the active attempt");

    Statement attempt(
        impl_->db,
        "SELECT classification FROM candidate_attempts "
        "WHERE candidate_id=?1 AND attempt_number=?2");
    attempt.bind(candidate_id, 1);
    attempt.bind(static_cast<int>(attempt_number), 2);
    require(attempt.row(), "candidate attempt does not exist");
    if (attempt.text(0) != "dispatching") {
        transaction.commit();
        return CandidateAttemptResult{
            .state = state,
            .terminal = state == CandidateState::accepted ||
                        state == CandidateState::accepted_by_reconciliation ||
                        state == CandidateState::rejected ||
                        state == CandidateState::ambiguous,
            .attempt_count = static_cast<std::uint32_t>(attempt_count),
            .had_indeterminate = had_indeterminate,
            .newly_actionable_false_candidates = 0,
            .newly_actionable_candidate_mismatches = 0,
            .trusted_candidate_rejection_recorded = false,
        };
    }

    const char *classification = "indeterminate";
    if (completion.classification == CandidateAttemptClassification::accepted) {
        classification = "accepted";
    }
    else if (completion.classification ==
             CandidateAttemptClassification::explicit_rejection) {
        classification = "explicit_rejection";
    }

    Statement finish(
        impl_->db,
        "UPDATE candidate_attempts SET completed_unix_us=?1,classification=?2,"
        "http_status=?3,rpc_error_code=?4,daemon_status=?5,daemon_block_id=?6,"
        "response_excerpt=?7 WHERE candidate_id=?8 AND attempt_number=?9 "
        "AND classification='dispatching'");
    finish.bind(completion.completed_unix_us, 1);
    finish.bind(classification, 2);
    finish.bind_optional(completion.http_status, 3);
    finish.bind_optional(completion.rpc_error_code, 4);
    finish.bind_optional(completion.daemon_status, 5);
    finish.bind_optional(completion.daemon_block_id, 6);
    finish.bind_optional(completion.response_excerpt, 7);
    finish.bind(candidate_id, 8);
    finish.bind(static_cast<int>(attempt_number), 9);
    finish.done();
    require(sqlite3_changes(impl_->db) == 1, "candidate attempt was finalized concurrently");

    const auto context = impl_->candidate_context_unlocked(candidate_id);
    if (state == CandidateState::accepted ||
        state == CandidateState::accepted_by_reconciliation) {
        // A positive reconciliation may commit while this already-dispatched
        // HTTP attempt is on wire. Preserve that authoritative acceptance and
        // merely close the attempt evidence; a later rejection or transport
        // result must never try to move the candidate out of accepted state.
        impl_->insert_candidate_event_unlocked(
            candidate_id, context, completion.completed_unix_us,
            "candidate_result");
        transaction.commit();
        return CandidateAttemptResult{
            .state = state,
            .terminal = true,
            .attempt_count = static_cast<std::uint32_t>(attempt_count),
            .had_indeterminate = had_indeterminate,
            .newly_actionable_false_candidates = 0,
            .newly_actionable_candidate_mismatches = 0,
            .trusted_candidate_rejection_recorded = false,
        };
    }
    bool terminal = false;
    Impl::ResolvedVerdicts resolved_verdicts;
    bool trusted_rejection_recorded = false;
    if (completion.classification == CandidateAttemptClassification::accepted) {
        (void)impl_->accept_candidate_in_transaction(candidate_id,
                                                      completion.completed_unix_us,
                                                      completion.daemon_block_id,
                                                      false);
        state = CandidateState::accepted;
        terminal = true;
    }
    else {
        had_indeterminate = had_indeterminate ||
                            completion.classification ==
                                CandidateAttemptClassification::indeterminate;
        if (static_cast<std::int64_t>(attempt_number) < max_attempts) {
            state = CandidateState::retry_wait;
        }
        else {
            state = had_indeterminate ? CandidateState::ambiguous
                                      : CandidateState::rejected;
            terminal = true;
        }
        Statement update(
            impl_->db,
            "UPDATE candidates SET state=?1,had_indeterminate=?2,updated_unix_us=?3,"
            "terminal_reason=?4 WHERE id=?5 AND state='dispatching'");
        update.bind(to_string(state), 1);
        update.bind(had_indeterminate ? 1 : 0, 2);
        update.bind(completion.completed_unix_us, 3);
        if (terminal) {
            update.bind(state == CandidateState::ambiguous
                            ? "attempts_exhausted_with_indeterminate"
                            : "attempts_explicitly_rejected",
                        4);
        }
        else {
            update.bind_null(4);
        }
        update.bind(candidate_id, 5);
        update.done();
        require(sqlite3_changes(impl_->db) == 1,
                "candidate state changed while finalizing attempt");

        if (state == CandidateState::rejected) {
            resolved_verdicts = impl_->resolve_rejected_verdicts_unlocked(
                candidate_id, completion.completed_unix_us);
            if (completion.trusted_mode) {
                Statement peer(
                    impl_->db,
                    "SELECT peer_family,peer_address FROM candidates WHERE id=?1");
                peer.bind(candidate_id, 1);
                require(peer.row(),
                        "candidate disappeared while recording trusted rejection");
                Statement existing(
                    impl_->db,
                    "SELECT id FROM abuse_events WHERE candidate_id=?1 "
                    "AND kind='trusted_candidate_rejection'");
                existing.bind(candidate_id, 1);
                if (!existing.row()) {
                    (void)impl_->insert_abuse_event_unlocked(AbuseEventInsert{
                        .connection_id = context.connection_id,
                        .share_id = context.share_id,
                        .candidate_id = candidate_id,
                        .peer_family = static_cast<int>(peer.integer(0)),
                        .peer_address = peer.blob(1),
                        .kind = "trusted_candidate_rejection",
                        .weight = 1,
                        .created_unix_us = completion.completed_unix_us,
                        .detail = std::nullopt,
                    });
                    trusted_rejection_recorded = true;
                }
            }
        }

        impl_->insert_candidate_event_unlocked(
            candidate_id, context, completion.completed_unix_us,
            terminal ? "candidate_result" : "candidate_retry");
    }
    transaction.commit();
    return CandidateAttemptResult{
        .state = state,
        .terminal = terminal,
        .attempt_count = static_cast<std::uint32_t>(attempt_count),
        .had_indeterminate = had_indeterminate,
        .newly_actionable_false_candidates = resolved_verdicts.false_candidates,
        .newly_actionable_candidate_mismatches =
            resolved_verdicts.candidate_mismatches,
        .trusted_candidate_rejection_recorded = trusted_rejection_recorded,
    };
}

CandidateReconciliationStartResult Database::start_candidate_reconciliation(
    const CandidateReconciliationStart &reconciliation)
{
    PriorityWriterLock lock(impl_->mutex);
    require(reconciliation.candidate_id > 0,
            "reconciliation candidate ID must be positive");
    require(reconciliation.cycle_number >= 1,
            "reconciliation cycle number must be positive");
    if (reconciliation.lookup_kind == ReconciliationLookupKind::expected_hash) {
        require(reconciliation.requested_block_id.has_value(),
                "expected-hash reconciliation requires a requested block ID");
    }
    else {
        require(!reconciliation.requested_block_id.has_value(),
                "height reconciliation must not have a requested block ID");
    }
    const std::string_view lookup_kind =
        reconciliation.lookup_kind == ReconciliationLookupKind::expected_hash
            ? "expected_hash"
            : "height";

    Transaction transaction(impl_->db);
    Statement candidate(
        impl_->db,
        "SELECT state FROM candidates WHERE id=?1");
    candidate.bind(reconciliation.candidate_id, 1);
    require(candidate.row(), "reconciliation candidate does not exist");
    const CandidateState candidate_state = parse_candidate_state(candidate.text(0));
    require(candidate_state == CandidateState::journaled ||
                candidate_state == CandidateState::dispatching ||
                candidate_state == CandidateState::retry_wait ||
                candidate_state == CandidateState::ambiguous,
            "candidate state does not permit reconciliation evidence");

    Statement existing(
        impl_->db,
        "SELECT id,rpc_request_id,requested_block_id,started_unix_us "
        "FROM candidate_reconciliations WHERE candidate_id=?1 "
        "AND cycle_number=?2 AND lookup_kind=?3");
    existing.bind(reconciliation.candidate_id, 1);
    existing.bind(static_cast<std::int64_t>(reconciliation.cycle_number), 2);
    existing.bind(lookup_kind, 3);
    if (existing.row()) {
        require(existing.integer(1) == reconciliation.rpc_request_id,
                "reconciliation lookup was already started with another RPC ID");
        if (reconciliation.requested_block_id.has_value()) {
            require(!existing.is_null(2) &&
                        exact_array<32>(existing.blob(2),
                                        "requested reconciliation block ID") ==
                            *reconciliation.requested_block_id,
                    "reconciliation lookup requested block ID changed");
        }
        else {
            require(existing.is_null(2),
                    "reconciliation lookup unexpectedly has a requested block ID");
        }
        require(existing.integer(3) == reconciliation.started_unix_us,
                "reconciliation lookup start time changed");
        const std::int64_t id = existing.integer(0);
        transaction.commit();
        return CandidateReconciliationStartResult{
            .reconciliation_id = id,
            .inserted = false,
        };
    }

    Statement insert(
        impl_->db,
        "INSERT INTO candidate_reconciliations(candidate_id,cycle_number,lookup_kind,"
        "rpc_request_id,requested_block_id,started_unix_us,classification) "
        "VALUES(?1,?2,?3,?4,?5,?6,'querying')");
    insert.bind(reconciliation.candidate_id, 1);
    insert.bind(static_cast<std::int64_t>(reconciliation.cycle_number), 2);
    insert.bind(lookup_kind, 3);
    insert.bind(reconciliation.rpc_request_id, 4);
    insert.bind_optional(reconciliation.requested_block_id, 5);
    insert.bind(reconciliation.started_unix_us, 6);
    insert.done();
    const std::int64_t id = last_insert_id(impl_->db);

    Statement update(
        impl_->db,
        "UPDATE candidates SET reconciliation_cycle_count="
        "max(reconciliation_cycle_count,?1),next_reconciliation_unix_us=NULL,"
        "updated_unix_us=max(updated_unix_us,?2) WHERE id=?3");
    update.bind(static_cast<std::int64_t>(reconciliation.cycle_number), 1);
    update.bind(reconciliation.started_unix_us, 2);
    update.bind(reconciliation.candidate_id, 3);
    update.done();
    require(sqlite3_changes(impl_->db) == 1,
            "reconciliation candidate disappeared while journaling lookup");
    transaction.commit();
    return CandidateReconciliationStartResult{
        .reconciliation_id = id,
        .inserted = true,
    };
}

CandidateReconciliationResult Database::finish_candidate_reconciliation(
    std::int64_t reconciliation_id,
    const CandidateReconciliationCompletion &completion)
{
    PriorityWriterLock lock(impl_->mutex);
    impl_->flush_accounting_transaction_unlocked();
    require(reconciliation_id > 0, "reconciliation ID must be positive");
    if (completion.observed_height.has_value()) {
        require_i64(*completion.observed_height,
                    "observed reconciliation height");
    }
    if (completion.response_excerpt.has_value()) {
        require_no_nul(*completion.response_excerpt,
                       "reconciliation response excerpt");
        require(completion.response_excerpt->size() <= 4096,
                "reconciliation response excerpt exceeds 4096 bytes");
    }

    const std::string_view classification =
        completion.classification == ReconciliationClassification::positive
            ? "positive"
            : completion.classification == ReconciliationClassification::inconclusive
                  ? "inconclusive"
                  : "indeterminate";

    Transaction transaction(impl_->db);
    Statement stored(
        impl_->db,
        "SELECT candidate_id,lookup_kind,requested_block_id,classification,"
        "completed_unix_us,observed_block_id,observed_height,"
        "observed_miner_tx_hash,observed_orphan,response_excerpt "
        "FROM candidate_reconciliations WHERE id=?1");
    stored.bind(reconciliation_id, 1);
    require(stored.row(), "reconciliation row does not exist");
    const std::int64_t candidate_id = stored.integer(0);
    const std::string lookup_kind = stored.text(1);
    std::optional<Hash32> requested_block_id;
    if (!stored.is_null(2)) {
        requested_block_id = exact_array<32>(stored.blob(2),
                                              "requested reconciliation block ID");
    }
    const std::string prior_classification = stored.text(3);

    auto optional_hash_matches = [&stored](int column,
                                           const std::optional<Hash32> &value,
                                           std::string_view field) {
        if (!value.has_value()) {
            return stored.is_null(column);
        }
        return !stored.is_null(column) &&
               exact_array<32>(stored.blob(column), field) == *value;
    };
    auto optional_height_matches = [&stored](
                                       int column,
                                       const std::optional<std::uint64_t> &value) {
        return value.has_value()
                   ? !stored.is_null(column) &&
                         stored.integer(column) == static_cast<std::int64_t>(*value)
                   : stored.is_null(column);
    };
    auto optional_bool_matches = [&stored](int column,
                                           const std::optional<bool> &value) {
        return value.has_value()
                   ? !stored.is_null(column) &&
                         stored.integer(column) == (*value ? 1 : 0)
                   : stored.is_null(column);
    };
    auto optional_text_matches = [&stored](
                                     int column,
                                     const std::optional<std::string> &value) {
        return value.has_value() ? !stored.is_null(column) &&
                                       stored.text(column) == *value
                                 : stored.is_null(column);
    };

    if (prior_classification != "querying") {
        require(prior_classification == classification,
                "reconciliation was already completed with another classification");
        require(!stored.is_null(4) &&
                    stored.integer(4) == completion.completed_unix_us,
                "reconciliation completion time changed");
        require(optional_hash_matches(5, completion.observed_block_id,
                                      "observed reconciliation block ID") &&
                    optional_height_matches(6, completion.observed_height) &&
                    optional_hash_matches(7, completion.observed_miner_tx_hash,
                                          "observed reconciliation miner tx hash") &&
                    optional_bool_matches(8, completion.observed_orphan) &&
                    optional_text_matches(9, completion.response_excerpt),
                "reconciliation was already completed with different evidence");
        const CandidateState state =
            impl_->candidate_context_unlocked(candidate_id).state;
        transaction.commit();
        return CandidateReconciliationResult{
            .candidate_state = state,
            .candidate_accepted = state == CandidateState::accepted ||
                                  state == CandidateState::accepted_by_reconciliation,
            .already_completed = true,
        };
    }

    const auto context = impl_->candidate_context_unlocked(candidate_id);
    if (completion.classification == ReconciliationClassification::positive) {
        require(completion.observed_block_id.has_value(),
                "positive reconciliation has no observed block ID");
        require(completion.observed_height.has_value() &&
                    *completion.observed_height ==
                        static_cast<std::uint64_t>(context.height),
                "positive reconciliation height does not match candidate");
        require(completion.observed_miner_tx_hash.has_value() &&
                    *completion.observed_miner_tx_hash == context.miner_tx_hash,
                "positive reconciliation miner transaction does not match candidate");
        require(completion.observed_orphan.has_value() &&
                    !*completion.observed_orphan,
                "positive reconciliation is missing non-orphan evidence");
        if (lookup_kind == "expected_hash") {
            require(requested_block_id.has_value() &&
                        *requested_block_id == *completion.observed_block_id,
                    "positive hash reconciliation returned another block ID");
        }
        else {
            require(lookup_kind == "height",
                    "reconciliation row has an invalid lookup kind");
        }
    }

    Statement finish(
        impl_->db,
        "UPDATE candidate_reconciliations SET completed_unix_us=?1,"
        "classification=?2,observed_block_id=?3,observed_height=?4,"
        "observed_miner_tx_hash=?5,observed_orphan=?6,response_excerpt=?7 "
        "WHERE id=?8 AND classification='querying'");
    finish.bind(completion.completed_unix_us, 1);
    finish.bind(classification, 2);
    finish.bind_optional(completion.observed_block_id, 3);
    if (completion.observed_height.has_value()) {
        finish.bind(static_cast<std::int64_t>(*completion.observed_height), 4);
    }
    else {
        finish.bind_null(4);
    }
    finish.bind_optional(completion.observed_miner_tx_hash, 5);
    if (completion.observed_orphan.has_value()) {
        finish.bind(*completion.observed_orphan ? 1 : 0, 6);
    }
    else {
        finish.bind_null(6);
    }
    finish.bind_optional(completion.response_excerpt, 7);
    finish.bind(reconciliation_id, 8);
    finish.done();
    require(sqlite3_changes(impl_->db) == 1,
            "reconciliation was completed concurrently");

    CandidateState result_state = context.state;
    bool accepted = result_state == CandidateState::accepted ||
                    result_state == CandidateState::accepted_by_reconciliation;
    if (completion.classification == ReconciliationClassification::positive) {
        (void)impl_->accept_candidate_in_transaction(
            candidate_id, completion.completed_unix_us,
            completion.observed_block_id, true);
        result_state = impl_->candidate_context_unlocked(candidate_id).state;
        accepted = true;
    }
    transaction.commit();
    return CandidateReconciliationResult{
        .candidate_state = result_state,
        .candidate_accepted = accepted,
        .already_completed = false,
    };
}

void Database::schedule_candidate_reconciliation(std::int64_t candidate_id,
                                                 std::int64_t next_unix_us)
{
    PriorityWriterLock lock(impl_->mutex);
    require(candidate_id > 0, "candidate ID must be positive");
    Statement statement(
        impl_->db,
        "UPDATE candidates SET next_reconciliation_unix_us=?1,"
        "updated_unix_us=max(updated_unix_us,?1) WHERE id=?2 "
        "AND state='ambiguous'");
    statement.bind(next_unix_us, 1);
    statement.bind(candidate_id, 2);
    statement.done();
    require(sqlite3_changes(impl_->db) == 1,
            "candidate is absent or nonambiguous");
}

bool Database::exhaust_candidate_reconciliation(std::int64_t candidate_id,
                                                std::int64_t exhausted_unix_us)
{
    PriorityWriterLock lock(impl_->mutex);
    require(candidate_id > 0, "candidate ID must be positive");
    Statement statement(
        impl_->db,
        "UPDATE candidates SET reconciliation_exhausted_unix_us=?1,"
        "next_reconciliation_unix_us=NULL,updated_unix_us=max(updated_unix_us,?1) "
        "WHERE id=?2 AND state='ambiguous' "
        "AND reconciliation_exhausted_unix_us IS NULL");
    statement.bind(exhausted_unix_us, 1);
    statement.bind(candidate_id, 2);
    statement.done();
    return sqlite3_changes(impl_->db) == 1;
}

bool Database::accept_candidate(std::int64_t candidate_id,
                                std::int64_t accepted_unix_us,
                                std::optional<Hash32> canonical_block_id,
                                bool by_reconciliation)
{
    PriorityWriterLock lock(impl_->mutex);
    require(candidate_id > 0, "candidate ID must be positive");
    impl_->flush_accounting_transaction_unlocked();
    Transaction transaction(impl_->db);
    const bool accepted = impl_->accept_candidate_in_transaction(
        candidate_id, accepted_unix_us, canonical_block_id, by_reconciliation);
    transaction.commit();
    return accepted;
}

std::vector<CandidateRecovery> Database::recoverable_candidates() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(
        impl_->db,
        "SELECT id,candidate_key,first_share_id,round_id,job_public_id,"
        "template_generation,connection_id,height,"
        "peer_family,peer_address,frozen_block_blob,miner_tx_hash,expected_block_id,"
        "state,attempt_count,max_attempts,had_indeterminate,"
        "reconciliation_cycle_count,created_unix_us,next_reconciliation_unix_us "
        "FROM candidates "
        "WHERE state IN ('journaled','dispatching','retry_wait','ambiguous') "
        "ORDER BY id");
    std::vector<CandidateRecovery> result;
    while (statement.row()) {
        CandidateRecovery value;
        value.candidate_id = statement.integer(0);
        value.candidate_key = exact_array<32>(statement.blob(1), "candidate key");
        if (!statement.is_null(2)) value.first_share_id = statement.integer(2);
        value.round_id = statement.integer(3);
        value.job_public_id = exact_array<16>(statement.blob(4), "candidate job public ID");
        value.template_generation = statement.integer(5);
        value.connection_id = statement.integer(6);
        const std::int64_t height = statement.integer(7);
        require(height > 0, "recovered candidate height is invalid");
        value.height = static_cast<std::uint64_t>(height);
        value.peer_family = static_cast<int>(statement.integer(8));
        value.peer_address = statement.blob(9);
        value.frozen_block_blob = statement.blob(10);
        value.miner_tx_hash = exact_array<32>(statement.blob(11),
                                              "candidate miner transaction hash");
        if (!statement.is_null(12)) {
            value.expected_block_id = exact_array<32>(
                statement.blob(12), "candidate expected block ID");
        }
        value.state = parse_candidate_state(statement.text(13));
        value.attempt_count = static_cast<std::uint32_t>(statement.integer(14));
        value.max_attempts = static_cast<std::uint32_t>(statement.integer(15));
        value.had_indeterminate = statement.integer(16) == 1;
        value.reconciliation_cycle_count =
            static_cast<std::uint32_t>(statement.integer(17));
        value.created_unix_us = statement.integer(18);
        if (!statement.is_null(19)) {
            value.next_reconciliation_unix_us = statement.integer(19);
        }
        result.push_back(std::move(value));
    }
    return result;
}

ShareAcceptanceResult Database::accept_share(
    std::int64_t share_id, std::int64_t completed_unix_us,
    std::string_view assigned_difficulty_dec, HashrateSource source)
{
    return accept_share(ShareAcceptance{
        .share_id = share_id,
        .completed_unix_us = completed_unix_us,
        .assigned_difficulty_dec = std::string(assigned_difficulty_dec),
        .source = source,
        .actual_difficulty_dec = std::nullopt,
        .verifier_ticket_dec = std::nullopt,
        .verifier_seed_id_dec = std::nullopt,
        .verifier_queue_ns = std::nullopt,
        .verifier_hash_ns = std::nullopt,
        .verifier_total_ns = std::nullopt,
    });
}

ShareAcceptanceResult Database::accept_share(const ShareAcceptance &acceptance)
{
    PriorityWriterLock lock(impl_->mutex);
    require(acceptance.share_id != 0, "share ID must be nonzero");
    require_uint128(acceptance.assigned_difficulty_dec,
                    "assigned difficulty", false);
    require(acceptance.completed_unix_us >= 0, "share completion time is negative");
    if (acceptance.actual_difficulty_dec.has_value()) {
        require_uint128(*acceptance.actual_difficulty_dec,
                        "share actual difficulty");
    }
    if (acceptance.verifier_ticket_dec.has_value()) {
        require_uint64_decimal(*acceptance.verifier_ticket_dec,
                               "verifier ticket", false);
    }
    if (acceptance.verifier_seed_id_dec.has_value()) {
        require_uint64_decimal(*acceptance.verifier_seed_id_dec,
                               "verifier seed ID", false);
    }
    auto require_timing = [](const std::optional<std::uint64_t> &value,
                             std::string_view name) {
        if (value.has_value()) {
            require_i64(*value, name);
        }
    };
    require_timing(acceptance.verifier_queue_ns, "verifier queue time");
    require_timing(acceptance.verifier_hash_ns, "verifier hash time");
    require_timing(acceptance.verifier_total_ns, "verifier total time");

    std::int64_t effective_share_id = acceptance.share_id;
    if (effective_share_id < 0) {
        const auto found = impl_->transient_shares.find(effective_share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        StagedShare &staged = found->second;
        require(staged.value.status == "received" || staged.value.status == "verifying",
                "transient share is already terminal");
        if (staged.value.assigned_difficulty_dec) {
            require(*staged.value.assigned_difficulty_dec == acceptance.assigned_difficulty_dec,
                    "accepted difficulty differs from staged share assignment");
        }
        require(staged.value.provenance == "pending" ||
                    staged.value.provenance == to_string(acceptance.source),
                "share provenance conflicts with active accounting mode");
        if (!impl_->meets_persistence_threshold(acceptance.actual_difficulty_dec)) {
            const std::int64_t round_id = staged.round_id;
            impl_->record_share_total_unlocked("accepted", to_string(acceptance.source),
                                               acceptance.completed_unix_us);
            impl_->record_work_unlocked(staged, acceptance);
            impl_->erase_transient_share_unlocked(found);
            if (impl_->pending_accounting_items.load(std::memory_order_acquire) >= 4096) {
                impl_->flush_accounting_transaction_unlocked();
            }
            return {
                .accepted = true,
                .round_id = round_id,
                .event_id = 0,
                .persisted_share_id = 0,
            };
        }
        impl_->flush_accounting_transaction_unlocked();
        const std::int64_t transient_alias = effective_share_id;
        const std::int64_t promoted_round_id = staged.round_id;
        Transaction promote(impl_->db);
        effective_share_id = impl_->insert_staged_share_unlocked(staged, "high_difficulty");
        promote.commit();
        impl_->erase_transient_share_unlocked(found);
        impl_->remember_persisted_share_alias(
            transient_alias, PersistedShareIdentity{effective_share_id,
                                                     promoted_round_id});
    }
    else {
        impl_->flush_accounting_transaction_unlocked();
    }

    Transaction transaction(impl_->db);
    Statement share(
        impl_->db,
        "SELECT status,connection_id,worker_id,job_public_id,"
        "assigned_difficulty_dec,provenance,session_id,template_generation,"
        "round_id,network_difficulty_dec FROM shares WHERE id=?1");
    share.bind(effective_share_id, 1);
    require(share.row(), "share does not exist");
    const std::string prior_status = share.text(0);
    std::optional<std::int64_t> connection_id;
    if (!share.is_null(1)) connection_id = share.integer(1);
    std::optional<std::int64_t> worker_id;
    if (!share.is_null(2)) {
        worker_id = share.integer(2);
    }
    std::optional<PublicId> job_public_id;
    if (!share.is_null(3)) {
        job_public_id = exact_array<16>(share.blob(3), "share job public ID");
    }
    if (!share.is_null(4)) {
        require(share.text(4) == acceptance.assigned_difficulty_dec,
                "accepted difficulty differs from the share's assigned difficulty");
    }
    const std::string prior_provenance = share.text(5);
    const std::int64_t session_id = share.integer(6);
    std::optional<std::int64_t> template_generation;
    if (!share.is_null(7)) {
        template_generation = share.integer(7);
    }
    const std::int64_t round_id = share.integer(8);
    require(!share.is_null(9),
            "accepted share has no snapshotted network difficulty");
    const std::string network_difficulty = share.text(9);
    require_uint128(network_difficulty, "share network difficulty", false);

    if (prior_status != "received" && prior_status != "verifying") {
        transaction.commit();
        return ShareAcceptanceResult{
            .accepted = false,
            .round_id = round_id,
            .event_id = 0,
            .persisted_share_id = effective_share_id,
        };
    }
    require(prior_provenance == "pending" ||
                prior_provenance == to_string(acceptance.source),
            "share provenance conflicts with active accounting mode");

    Statement update(
        impl_->db,
        "UPDATE shares SET status='accepted',completed_unix_us=?1,"
        "assigned_difficulty_dec=coalesce(assigned_difficulty_dec,?2),"
        "credited_difficulty_dec=?2,provenance=?3,actual_difficulty_dec=?4,"
        "verifier_ticket_dec=coalesce(?5,verifier_ticket_dec),"
        "verifier_seed_id_dec=coalesce(?6,verifier_seed_id_dec),"
        "verifier_queue_ns=?7,verifier_hash_ns=?8,verifier_total_ns=?9,"
        "error_code=NULL,error_message=NULL WHERE id=?10 "
        "AND status IN ('received','verifying')");
    update.bind(acceptance.completed_unix_us, 1);
    update.bind(acceptance.assigned_difficulty_dec, 2);
    update.bind(to_string(acceptance.source), 3);
    update.bind_optional(acceptance.actual_difficulty_dec, 4);
    update.bind_optional(acceptance.verifier_ticket_dec, 5);
    update.bind_optional(acceptance.verifier_seed_id_dec, 6);
    if (acceptance.verifier_queue_ns.has_value()) {
        update.bind(static_cast<std::int64_t>(*acceptance.verifier_queue_ns), 7);
    }
    else update.bind_null(7);
    if (acceptance.verifier_hash_ns.has_value()) {
        update.bind(static_cast<std::int64_t>(*acceptance.verifier_hash_ns), 8);
    }
    else update.bind_null(8);
    if (acceptance.verifier_total_ns.has_value()) {
        update.bind(static_cast<std::int64_t>(*acceptance.verifier_total_ns), 9);
    }
    else update.bind_null(9);
    update.bind(effective_share_id, 10);
    update.done();
    if (sqlite3_changes(impl_->db) == 0) {
        transaction.commit();
        return ShareAcceptanceResult{
            .accepted = false,
            .round_id = round_id,
            .event_id = 0,
            .persisted_share_id = effective_share_id,
        };
    }

    const std::int64_t second_utc = acceptance.completed_unix_us / 1'000'000;
    impl_->add_bucket_unlocked("global", 0, second_utc,
                               acceptance.assigned_difficulty_dec,
                               acceptance.source);
    if (connection_id) impl_->add_bucket_unlocked("connection", *connection_id, second_utc,
                                                   acceptance.assigned_difficulty_dec,
                                                   acceptance.source);
    if (worker_id.has_value()) {
        impl_->add_bucket_unlocked("worker", *worker_id, second_utc,
                                   acceptance.assigned_difficulty_dec,
                                   acceptance.source);
    }
    const bool pruned_hashrate = impl_->prune_hashrate_unlocked(second_utc);

    Statement round(
        impl_->db,
        "SELECT credited_difficulty_dec,accepted_share_count,state,"
        "effort_finalized_unix_us FROM rounds WHERE id=?1");
    round.bind(round_id, 1);
    require(round.row(), "share acceptance found no assigned round");
    const std::string credited = add_uint128(round.text(0),
                                             acceptance.assigned_difficulty_dec);
    const std::int64_t accepted_count = round.integer(1);
    const std::string round_state = round.text(2);
    require(round_state == "open" || round_state == "closed",
            "share assigned round has an invalid state");
    require(round.is_null(3),
            "share acceptance attempted to mutate finalized round effort");
    require(accepted_count < std::numeric_limits<std::int64_t>::max(),
            "round accepted-share counter overflow");

    impl_->add_round_work_segment_unlocked(
        round_id, acceptance.source, network_difficulty,
        acceptance.assigned_difficulty_dec);

    Statement update_round(
        impl_->db,
        "UPDATE rounds SET credited_difficulty_dec=?1,accepted_share_count=?2 "
        "WHERE id=?3 AND effort_finalized_unix_us IS NULL");
    update_round.bind(credited, 1);
    update_round.bind(accepted_count + 1, 2);
    update_round.bind(round_id, 3);
    update_round.done();
    require(sqlite3_changes(impl_->db) == 1,
            "assigned round changed during share acceptance");
    impl_->add_share_total_unlocked("accepted", to_string(acceptance.source),
                                    acceptance.completed_unix_us);

    const std::int64_t event_id = impl_->insert_event_unlocked(EventInsert{
        .session_id = session_id,
        .created_unix_us = acceptance.completed_unix_us,
        .type = "share_result",
        .connection_id = connection_id,
        .worker_id = worker_id,
        .template_generation = template_generation,
        .job_public_id = job_public_id,
        .share_id = effective_share_id,
        .candidate_id = std::nullopt,
        .round_id = round_id,
        .payload_json = std::string(kEmptyPayload),
    });
    if (round_state == "closed") {
        (void)impl_->try_finalize_round_unlocked(
            round_id, acceptance.completed_unix_us);
    }
    transaction.commit();
    if (pruned_hashrate) {
        impl_->last_hashrate_prune_second = second_utc;
    }
    return ShareAcceptanceResult{
        .accepted = true,
        .round_id = round_id,
        .event_id = event_id,
        .persisted_share_id = effective_share_id,
    };
}

HashrateWindows Database::hashrate(std::string_view scope_type,
                                   std::int64_t scope_id,
                                   HashrateSource source,
                                   std::int64_t now_second_utc) const
{
    std::scoped_lock lock(impl_->mutex);
    require(scope_type == "global" || scope_type == "connection" ||
                scope_type == "worker",
            "hashrate scope type is invalid");
    require((scope_type == "global" && scope_id == 0) ||
                (scope_type != "global" && scope_id > 0),
            "hashrate scope ID is invalid");
    require(now_second_utc >= 0, "hashrate time is negative");

    constexpr std::array<std::uint32_t, 6> windows = {
        60, 300, 600, 3600, 21600, 86400,
    };
    std::array<std::string, 6> work = {"0", "0", "0", "0", "0", "0"};
    Statement statement(
        impl_->db,
        "SELECT second_utc,credited_difficulty_dec FROM hashrate_buckets "
        "WHERE scope_type=?1 AND scope_id=?2 AND source=?3 "
        "AND second_utc>?4 AND second_utc<=?5 ORDER BY second_utc");
    statement.bind(scope_type, 1);
    statement.bind(scope_id, 2);
    statement.bind(to_string(source), 3);
    statement.bind(now_second_utc - static_cast<std::int64_t>(windows.back()), 4);
    statement.bind(now_second_utc, 5);
    while (statement.row()) {
        const std::int64_t second = statement.integer(0);
        const std::string difficulty = statement.text(1);
        require_uint128(difficulty, "persisted bucket difficulty");
        for (std::size_t i = 0; i < windows.size(); ++i) {
            if (second > now_second_utc - static_cast<std::int64_t>(windows[i])) {
                work[i] = add_uint128(work[i], difficulty);
            }
        }
    }

    return HashrateWindows{
        .one_minute = divide_decimal(work[0], windows[0]),
        .five_minutes = divide_decimal(work[1], windows[1]),
        .ten_minutes = divide_decimal(work[2], windows[2]),
        .one_hour = divide_decimal(work[3], windows[3]),
        .six_hours = divide_decimal(work[4], windows[4]),
        .twenty_four_hours = divide_decimal(work[5], windows[5]),
    };
}

std::int64_t Database::insert_abuse_event(const AbuseEventInsert &event)
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->insert_abuse_event_unlocked(event);
}

std::int64_t Database::create_ban(const BanInsert &ban)
{
    std::scoped_lock lock(impl_->mutex);
    require(ban.peer_family == 2 || ban.peer_family == 10,
            "ban peer family must be AF_INET or AF_INET6");
    const std::size_t address_size = ban.peer_family == 2 ? 4U : 16U;
    require(ban.peer_address.size() == address_size,
            "ban peer address has the wrong size");
    require(ban.expires_unix_us > ban.created_unix_us,
            "ban expiry must follow creation");
    require(ban.evidence_window_started_unix_us <=
                ban.evidence_window_ended_unix_us,
            "ban evidence window is reversed");
    require(!ban.reason.empty(), "ban reason is empty");
    require_no_nul(ban.reason, "ban reason");
    require(!ban.abuse_event_ids.empty(),
            "a ban must link at least one abuse event");

    Transaction transaction(impl_->db);
    Statement already(
        impl_->db,
        "SELECT id FROM bans WHERE peer_family=?1 AND peer_address=?2 AND active=1");
    already.bind(ban.peer_family, 1);
    already.bind(ban.peer_address, 2);
    if (already.row()) {
        const std::int64_t id = already.integer(0);
        transaction.commit();
        return id;
    }

    for (const std::int64_t evidence_id : ban.abuse_event_ids) {
        require(evidence_id > 0, "ban evidence ID must be positive");
        Statement evidence(
            impl_->db,
            "SELECT peer_family,peer_address,created_unix_us FROM abuse_events "
            "WHERE id=?1");
        evidence.bind(evidence_id, 1);
        require(evidence.row(), "ban evidence event does not exist");
        require(evidence.integer(0) == ban.peer_family &&
                    evidence.blob(1) == ban.peer_address,
                "ban evidence belongs to another peer");
        const std::int64_t evidence_time = evidence.integer(2);
        require(evidence_time >= ban.evidence_window_started_unix_us &&
                    evidence_time <= ban.evidence_window_ended_unix_us,
                "ban evidence is outside the declared evidence window");
    }

    Statement insert(
        impl_->db,
        "INSERT INTO bans(peer_family,peer_address,created_unix_us,expires_unix_us,"
        "evidence_window_started_unix_us,evidence_window_ended_unix_us,reason,active) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,1)");
    insert.bind(ban.peer_family, 1);
    insert.bind(ban.peer_address, 2);
    insert.bind(ban.created_unix_us, 3);
    insert.bind(ban.expires_unix_us, 4);
    insert.bind(ban.evidence_window_started_unix_us, 5);
    insert.bind(ban.evidence_window_ended_unix_us, 6);
    insert.bind(ban.reason, 7);
    insert.done();
    const std::int64_t ban_id = last_insert_id(impl_->db);

    for (const std::int64_t evidence_id : ban.abuse_event_ids) {
        Statement link(
            impl_->db,
            "INSERT INTO ban_abuse_events(ban_id,abuse_event_id) VALUES(?1,?2)");
        link.bind(ban_id, 1);
        link.bind(evidence_id, 2);
        link.done();
    }

    Statement link_count(
        impl_->db, "SELECT count(*) FROM ban_abuse_events WHERE ban_id=?1");
    link_count.bind(ban_id, 1);
    require(link_count.row() && link_count.integer(0) > 0,
            "ban evidence links were not persisted");

    const std::int64_t session_id = impl_->latest_session_id_unlocked();
    if (session_id > 0) {
        impl_->insert_event_unlocked(EventInsert{
            .session_id = session_id,
            .created_unix_us = ban.created_unix_us,
            .type = "ban_created",
            .connection_id = std::nullopt,
            .worker_id = std::nullopt,
            .template_generation = std::nullopt,
            .job_public_id = std::nullopt,
            .share_id = std::nullopt,
            .candidate_id = std::nullopt,
            .round_id = std::nullopt,
            .payload_json = std::string(kEmptyPayload),
        });
    }
    transaction.commit();
    return ban_id;
}

std::vector<ActiveBan> Database::load_active_bans(std::int64_t now_unix_us)
{
    std::scoped_lock lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement expiring(
        impl_->db,
        "SELECT id FROM bans WHERE active=1 AND expires_unix_us<=?1 ORDER BY id");
    expiring.bind(now_unix_us, 1);
    std::vector<std::int64_t> expired_ids;
    while (expiring.row()) expired_ids.push_back(expiring.integer(0));
    Statement expire(
        impl_->db,
        "UPDATE bans SET active=0 WHERE active=1 AND expires_unix_us<=?1");
    expire.bind(now_unix_us, 1);
    expire.done();
    require(sqlite3_changes(impl_->db) == static_cast<int>(expired_ids.size()),
            "startup ban expiry count changed unexpectedly");
    if (!expired_ids.empty()) {
        const std::int64_t session_id = impl_->latest_session_id_unlocked();
        if (session_id > 0) {
            for ([[maybe_unused]] const std::int64_t id : expired_ids) {
                impl_->insert_event_unlocked(EventInsert{
                    .session_id = session_id,
                    .created_unix_us = now_unix_us,
                    .type = "ban_expired",
                    .connection_id = std::nullopt,
                    .worker_id = std::nullopt,
                    .template_generation = std::nullopt,
                    .job_public_id = std::nullopt,
                    .share_id = std::nullopt,
                    .candidate_id = std::nullopt,
                    .round_id = std::nullopt,
                    .payload_json = std::string(kEmptyPayload),
                });
            }
        }
    }

    Statement statement(
        impl_->db,
        "SELECT id,peer_family,peer_address,expires_unix_us,reason FROM bans "
        "WHERE active=1 AND expires_unix_us>?1 ORDER BY id");
    statement.bind(now_unix_us, 1);
    std::vector<ActiveBan> result;
    while (statement.row()) {
        result.push_back(ActiveBan{
            .id = statement.integer(0),
            .peer_family = static_cast<int>(statement.integer(1)),
            .peer_address = statement.blob(2),
            .expires_unix_us = statement.integer(3),
            .reason = statement.text(4),
        });
    }
    transaction.commit();
    return result;
}

std::uint64_t Database::expire_bans(std::int64_t now_unix_us,
                                    std::int64_t event_session_id)
{
    std::scoped_lock lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement select(
        impl_->db,
        "SELECT id FROM bans WHERE active=1 AND expires_unix_us<=?1 ORDER BY id");
    select.bind(now_unix_us, 1);
    std::vector<std::int64_t> ids;
    while (select.row()) {
        ids.push_back(select.integer(0));
    }

    Statement expire(
        impl_->db,
        "UPDATE bans SET active=0 WHERE active=1 AND expires_unix_us<=?1");
    expire.bind(now_unix_us, 1);
    expire.done();
    require(sqlite3_changes(impl_->db) == static_cast<int>(ids.size()),
            "ban expiry update count changed unexpectedly");

    if (!ids.empty()) {
        const std::int64_t session_id = event_session_id > 0
                                            ? event_session_id
                                            : impl_->latest_session_id_unlocked();
        if (session_id > 0) {
            for ([[maybe_unused]] const std::int64_t id : ids) {
                impl_->insert_event_unlocked(EventInsert{
                    .session_id = session_id,
                    .created_unix_us = now_unix_us,
                    .type = "ban_expired",
                    .connection_id = std::nullopt,
                    .worker_id = std::nullopt,
                    .template_generation = std::nullopt,
                    .job_public_id = std::nullopt,
                    .share_id = std::nullopt,
                    .candidate_id = std::nullopt,
                    .round_id = std::nullopt,
                    .payload_json = std::string(kEmptyPayload),
                });
            }
        }
    }
    transaction.commit();
    return ids.size();
}

CandidateVerdictResult Database::record_candidate_verdict(
    const CandidateVerdictInsert &verdict)
{
    PriorityWriterLock lock(impl_->mutex);
    require(verdict.share_id > 0, "candidate verdict share ID must be positive");
    const std::string kind = verdict_kind_text(verdict.kind);

    Transaction transaction(impl_->db);
    Statement prior(
        impl_->db,
        "SELECT disposition,abuse_event_id FROM candidate_verdicts "
        "WHERE share_id=?1 AND kind=?2");
    prior.bind(verdict.share_id, 1);
    prior.bind(kind, 2);
    if (prior.row()) {
        CandidateVerdictResult result;
        result.disposition = parse_verdict_disposition(prior.text(0));
        if (!prior.is_null(1)) {
            result.abuse_event_id = prior.integer(1);
        }
        transaction.commit();
        return result;
    }

    std::optional<std::int64_t> candidate_id = verdict.candidate_id;
    if (!candidate_id.has_value()) {
        Statement find(
            impl_->db, "SELECT id FROM candidates WHERE candidate_key=?1");
        find.bind(verdict.candidate_key, 1);
        if (find.row()) {
            candidate_id = find.integer(0);
        }
    }

    CandidateVerdictDisposition disposition = CandidateVerdictDisposition::pending;
    std::optional<std::int64_t> abuse_event_id;
    bool emit_consistency_error = false;
    if (candidate_id.has_value()) {
        Statement candidate(
            impl_->db,
            "SELECT state,candidate_key,connection_id,peer_family,peer_address "
            "FROM candidates WHERE id=?1");
        candidate.bind(*candidate_id, 1);
        require(candidate.row(), "candidate verdict references an absent candidate");
        require(exact_array<32>(candidate.blob(1), "candidate verdict key") ==
                    verdict.candidate_key,
                "candidate verdict key does not match candidate");
        const CandidateState state = parse_candidate_state(candidate.text(0));
        if (state == CandidateState::accepted ||
            state == CandidateState::accepted_by_reconciliation) {
            disposition = CandidateVerdictDisposition::suppressed;
            emit_consistency_error = true;
        }
        else if (state == CandidateState::rejected) {
            const std::string abuse_kind =
                verdict.kind == CandidateVerdictKind::false_candidate
                    ? "verified_false_candidate"
                    : "candidate_mismatch";
            Statement existing(
                impl_->db,
                "SELECT id FROM abuse_events WHERE candidate_id=?1 AND kind=?2");
            existing.bind(*candidate_id, 1);
            existing.bind(abuse_kind, 2);
            std::int64_t event_id = 0;
            if (existing.row()) {
                event_id = existing.integer(0);
            }
            else {
                event_id = impl_->insert_abuse_event_unlocked(AbuseEventInsert{
                    .connection_id = candidate.integer(2),
                    .share_id = verdict.share_id,
                    .candidate_id = *candidate_id,
                    .peer_family = static_cast<int>(candidate.integer(3)),
                    .peer_address = candidate.blob(4),
                    .kind = abuse_kind,
                    .weight = 1,
                    .created_unix_us = verdict.created_unix_us,
                    .detail = std::nullopt,
                });
            }
            Statement used(
                impl_->db,
                "SELECT 1 FROM candidate_verdicts WHERE abuse_event_id=?1 LIMIT 1");
            used.bind(event_id, 1);
            if (!used.row()) {
                disposition = CandidateVerdictDisposition::actionable;
                abuse_event_id = event_id;
            }
            else {
                // The candidate/kind has already produced its one durable abuse
                // event. This later verdict is terminal but intentionally does
                // not score twice.
                disposition = CandidateVerdictDisposition::suppressed;
            }
        }
    }
    else {
        Statement evidence(
            impl_->db,
            "SELECT s.connection_id,c.peer_family,c.peer_address FROM shares s "
            "JOIN connections c ON c.id=s.connection_id WHERE s.id=?1");
        evidence.bind(verdict.share_id, 1);
        require(evidence.row(),
                "candidate verdict share has no durable peer evidence");
        const std::string abuse_kind =
            verdict.kind == CandidateVerdictKind::false_candidate
                ? "verified_false_candidate"
                : "candidate_mismatch";
        abuse_event_id = impl_->insert_abuse_event_unlocked(AbuseEventInsert{
            .connection_id = evidence.integer(0),
            .share_id = verdict.share_id,
            .candidate_id = std::nullopt,
            .peer_family = static_cast<int>(evidence.integer(1)),
            .peer_address = evidence.blob(2),
            .kind = abuse_kind,
            .weight = 1,
            .created_unix_us = verdict.created_unix_us,
            .detail = std::nullopt,
        });
        disposition = CandidateVerdictDisposition::actionable;
    }

    const char *disposition_text = "pending";
    if (disposition == CandidateVerdictDisposition::actionable) {
        disposition_text = "actionable";
    }
    else if (disposition == CandidateVerdictDisposition::suppressed) {
        disposition_text = "suppressed";
    }
    Statement insert(
        impl_->db,
        "INSERT INTO candidate_verdicts(share_id,kind,candidate_key,candidate_id,"
        "disposition,created_unix_us,resolved_unix_us,abuse_event_id) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    insert.bind(verdict.share_id, 1);
    insert.bind(kind, 2);
    insert.bind(verdict.candidate_key, 3);
    insert.bind_optional(candidate_id, 4);
    insert.bind(disposition_text, 5);
    insert.bind(verdict.created_unix_us, 6);
    if (disposition == CandidateVerdictDisposition::pending) {
        insert.bind_null(7);
    }
    else {
        insert.bind(verdict.created_unix_us, 7);
    }
    insert.bind_optional(abuse_event_id, 8);
    insert.done();
    if (emit_consistency_error) {
        require(candidate_id.has_value(),
                "suppressed verdict has no accepted candidate");
        const auto context = impl_->candidate_context_unlocked(*candidate_id);
        impl_->insert_candidate_event_unlocked(
            *candidate_id, context, verdict.created_unix_us,
            "verifier_consistency_error");
    }
    transaction.commit();
    return CandidateVerdictResult{
        .disposition = disposition,
        .abuse_event_id = abuse_event_id,
    };
}

void Database::recover_blocknotify_deliveries()
{
    std::scoped_lock lock(impl_->mutex);
    impl_->recover_blocknotify_unlocked();
}

std::optional<BlocknotifyDelivery> Database::claim_next_blocknotify(
    std::int64_t now_unix_us)
{
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->options.blocknotify_enabled) {
        return std::nullopt;
    }

    Transaction transaction(impl_->db);
    Statement select(
        impl_->db,
        "SELECT id,candidate_id,miner_tx_hash,attempt_count "
        "FROM blocknotify_deliveries WHERE state='pending' OR "
        "(state='retry_wait' AND next_attempt_unix_us<=?1) "
        "ORDER BY candidate_id LIMIT 1");
    select.bind(now_unix_us, 1);
    if (!select.row()) {
        transaction.commit();
        return std::nullopt;
    }
    BlocknotifyDelivery delivery;
    delivery.id = select.integer(0);
    delivery.candidate_id = select.integer(1);
    delivery.miner_tx_hash = exact_array<32>(select.blob(2), "blocknotify miner tx hash");
    const std::int64_t prior_attempt_count = select.integer(3);
    require(prior_attempt_count >= 0 &&
                prior_attempt_count < std::numeric_limits<std::int64_t>::max(),
            "blocknotify attempt counter is invalid");
    delivery.attempt_count = static_cast<std::uint32_t>(prior_attempt_count + 1);

    Statement claim(
        impl_->db,
        "UPDATE blocknotify_deliveries SET state='running',attempt_count=?1,"
        "started_unix_us=?2,completed_unix_us=NULL,next_attempt_unix_us=NULL,"
        "exit_code=NULL,term_signal=NULL,stderr_excerpt=NULL,last_error=NULL "
        "WHERE id=?3 AND (state='pending' OR "
        "(state='retry_wait' AND next_attempt_unix_us<=?2))");
    claim.bind(static_cast<std::int64_t>(delivery.attempt_count), 1);
    claim.bind(now_unix_us, 2);
    claim.bind(delivery.id, 3);
    claim.done();
    require(sqlite3_changes(impl_->db) == 1,
            "blocknotify delivery became unavailable while claiming");

    const auto context = impl_->candidate_context_unlocked(delivery.candidate_id);
    impl_->insert_candidate_event_unlocked(delivery.candidate_id, context,
                                            now_unix_us,
                                            "blocknotify_started");
    transaction.commit();
    return delivery;
}

void Database::finish_blocknotify(std::int64_t delivery_id,
                                  const BlocknotifyCompletion &completion)
{
    std::scoped_lock lock(impl_->mutex);
    require(delivery_id > 0, "blocknotify delivery ID must be positive");
    if (completion.stderr_excerpt.has_value()) {
        require_no_nul(*completion.stderr_excerpt, "blocknotify stderr excerpt");
        require(completion.stderr_excerpt->size() <= 4096,
                "blocknotify stderr excerpt exceeds 4096 bytes");
    }
    if (completion.last_error.has_value()) {
        require_no_nul(*completion.last_error, "blocknotify error");
        require(completion.last_error->size() <= 4096,
                "blocknotify error exceeds 4096 bytes");
    }
    if (completion.delivered) {
        require(completion.exit_code.has_value() && *completion.exit_code == 0 &&
                    !completion.term_signal.has_value(),
                "successful blocknotify must have exit code 0 and no signal");
    }

    Transaction transaction(impl_->db);
    Statement select(
        impl_->db,
        "SELECT candidate_id,attempt_count,state FROM blocknotify_deliveries "
        "WHERE id=?1");
    select.bind(delivery_id, 1);
    require(select.row(), "blocknotify delivery does not exist");
    const std::int64_t candidate_id = select.integer(0);
    const std::int64_t attempt_count = select.integer(1);
    if (select.text(2) != "running") {
        throw DatabaseError("blocknotify delivery is not running");
    }
    require(attempt_count >= 1 &&
                attempt_count <= std::numeric_limits<std::uint32_t>::max(),
            "blocknotify attempt count is invalid");

    std::optional<std::int64_t> next_attempt;
    if (!completion.delivered) {
        const auto seconds = blocknotify_delay_seconds(
            static_cast<std::uint32_t>(attempt_count));
        next_attempt = checked_add_time(
            completion.completed_unix_us,
            static_cast<std::int64_t>(seconds) * 1'000'000);
    }

    Statement finish(
        impl_->db,
        "UPDATE blocknotify_deliveries SET state=?1,next_attempt_unix_us=?2,"
        "completed_unix_us=?3,exit_code=?4,term_signal=?5,stderr_excerpt=?6,"
        "last_error=?7 WHERE id=?8 AND state='running'");
    finish.bind(completion.delivered ? "delivered" : "retry_wait", 1);
    finish.bind_optional(next_attempt, 2);
    finish.bind(completion.completed_unix_us, 3);
    finish.bind_optional(completion.exit_code, 4);
    finish.bind_optional(completion.term_signal, 5);
    finish.bind_optional(completion.stderr_excerpt, 6);
    finish.bind_optional(completion.last_error, 7);
    finish.bind(delivery_id, 8);
    finish.done();
    require(sqlite3_changes(impl_->db) == 1,
            "blocknotify delivery changed while finalizing");

    const auto context = impl_->candidate_context_unlocked(candidate_id);
    impl_->insert_candidate_event_unlocked(candidate_id, context,
                                            completion.completed_unix_us,
                                            "blocknotify_result");
    transaction.commit();
}

std::uint64_t Database::pending_blocknotify_count() const
{
    std::scoped_lock lock(impl_->mutex);
    Statement statement(
        impl_->db,
        "SELECT count(*) FROM blocknotify_deliveries "
        "WHERE state IN ('pending','running','retry_wait')");
    require(statement.row(), "could not count pending blocknotify deliveries");
    const std::int64_t value = statement.integer(0);
    require(value >= 0, "pending blocknotify count is negative");
    return static_cast<std::uint64_t>(value);
}

void Database::mark_share_verifying(std::int64_t share_id,
                                    std::string_view verifier_ticket_dec,
                                    std::string_view verifier_seed_id_dec)
{
    std::scoped_lock lock(impl_->mutex);
    require(share_id != 0, "share ID must be nonzero");
    require_uint64_decimal(verifier_ticket_dec, "verifier ticket", false);
    require_uint64_decimal(verifier_seed_id_dec, "verifier seed ID", false);
    if (share_id < 0) {
        auto found = impl_->transient_shares.find(share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        require(found->second.value.status == "received",
                "share changed while marking verifier admission");
        found->second.value.status = "verifying";
        found->second.verifier_ticket_dec = std::string(verifier_ticket_dec);
        found->second.verifier_seed_id_dec = std::string(verifier_seed_id_dec);
        return;
    }
    Statement statement(
        impl_->db,
        "UPDATE shares SET status='verifying',verifier_ticket_dec=?1,"
        "verifier_seed_id_dec=?2 WHERE id=?3 AND status='received'");
    statement.bind(verifier_ticket_dec, 1);
    statement.bind(verifier_seed_id_dec, 2);
    statement.bind(share_id, 3);
    statement.done();
    require(sqlite3_changes(impl_->db) == 1,
            "share changed while marking verifier admission");
}

void Database::set_share_height_is_older(std::int64_t share_id,
                                         bool height_is_older)
{
    PriorityWriterLock lock(impl_->mutex);
    require(share_id != 0, "share ID must be nonzero");
    if (share_id < 0) {
        auto found = impl_->transient_shares.find(share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        found->second.value.height_is_older = height_is_older;
        return;
    }
    Statement statement(
        impl_->db,
        "UPDATE shares SET height_is_older=?1 WHERE id=?2 "
        "AND status IN ('received','verifying')");
    statement.bind(height_is_older ? 1 : 0, 1);
    statement.bind(share_id, 2);
    statement.done();
    require(sqlite3_changes(impl_->db) == 1,
            "share changed while updating stale-height evidence");
}

void Database::insert_share_hash(std::int64_t share_id, std::string_view role,
                                 const Hash32 &hash,
                                 std::optional<bool> share_target,
                                 std::optional<bool> network_target)
{
    PriorityWriterLock lock(impl_->mutex);
    require(share_id != 0, "share ID must be nonzero");
    require(role == "claimed" || role == "computed", "invalid share hash role");
    if (share_id < 0) {
        auto found = impl_->transient_shares.find(share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        auto [it, inserted] = found->second.hashes.emplace(
            std::string(role), StagedShareHash{hash, share_target, network_target});
        if (!inserted) {
            require(it->second.hash == hash &&
                        it->second.meets_share_target == share_target &&
                        it->second.meets_network_target == network_target,
                    "share hash role was staged with another value");
        }
        return;
    }
    Transaction transaction(impl_->db);
    Statement statement(
        impl_->db,
        "INSERT INTO share_hashes(share_id,role,hash,meets_share_target,"
        "meets_network_target) VALUES(?1,?2,?3,?4,?5) "
        "ON CONFLICT(share_id,role) DO NOTHING");
    statement.bind(share_id, 1);
    statement.bind(role, 2);
    statement.bind(hash, 3);
    if (share_target.has_value()) statement.bind(*share_target ? 1 : 0, 4);
    else statement.bind_null(4);
    if (network_target.has_value()) statement.bind(*network_target ? 1 : 0, 5);
    else statement.bind_null(5);
    statement.done();
    if (sqlite3_changes(impl_->db) == 0) {
        Statement existing(
            impl_->db,
            "SELECT hash,meets_share_target,meets_network_target FROM share_hashes "
            "WHERE share_id=?1 AND role=?2");
        existing.bind(share_id, 1);
        existing.bind(role, 2);
        require(existing.row(), "share hash conflict row disappeared");
        require(exact_array<32>(existing.blob(0), "persisted share hash") == hash,
                "share hash role was already persisted with another hash");
        const bool share_target_matches =
            share_target ? !existing.is_null(1) &&
                               existing.integer(1) == (*share_target ? 1 : 0)
                         : existing.is_null(1);
        const bool network_target_matches =
            network_target ? !existing.is_null(2) &&
                                 existing.integer(2) == (*network_target ? 1 : 0)
                           : existing.is_null(2);
        require(share_target_matches && network_target_matches,
                "share hash target classification changed");
    }
    transaction.commit();
}

ShareFinalizationResult Database::finalize_share(
    std::int64_t share_id, const ShareFinalization &value)
{
    PriorityWriterLock lock(impl_->mutex);
    require(share_id != 0, "share ID must be nonzero");
    static constexpr std::array<std::string_view, 10> terminal{
        "stale", "duplicate", "low_difficulty", "invalid_result", "unknown_job",
        "malformed", "unauthenticated", "server_busy", "verifier_failed", "cancelled"};
    require(std::find(terminal.begin(), terminal.end(), value.status) != terminal.end(),
            "invalid terminal share status");
    require(value.provenance == "verified" || value.provenance == "claimed" ||
                value.provenance == "pending",
            "invalid share provenance");
    if (value.actual_difficulty_dec) {
        require_uint128(*value.actual_difficulty_dec, "actual difficulty");
    }
    if (value.verifier_ticket_dec) {
        require_uint64_decimal(*value.verifier_ticket_dec,
                               "verifier ticket", false);
    }
    if (value.verifier_seed_id_dec) {
        require_uint64_decimal(*value.verifier_seed_id_dec,
                               "verifier seed ID", false);
    }
    if (value.error_code) require_no_nul(*value.error_code, "share error code");
    if (value.error_message) {
        require_no_nul(*value.error_message, "share error message");
    }
    if (value.verifier_queue_ns) {
        require_i64(*value.verifier_queue_ns, "verifier queue time");
    }
    if (value.verifier_hash_ns) {
        require_i64(*value.verifier_hash_ns, "verifier hash time");
    }
    if (value.verifier_total_ns) {
        require_i64(*value.verifier_total_ns, "verifier total time");
    }
    if (share_id < 0) {
        auto found = impl_->transient_shares.find(share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        if (!impl_->meets_persistence_threshold(value.actual_difficulty_dec)) {
            const std::int64_t round_id = found->second.round_id;
            impl_->record_share_total_unlocked(value.status, value.provenance,
                                               value.completed_unix_us);
            impl_->erase_transient_share_unlocked(found);
            if (impl_->pending_accounting_items.load(std::memory_order_acquire) >= 4096) {
                impl_->flush_accounting_transaction_unlocked();
            }
            Transaction finalize_round(impl_->db);
            (void)impl_->try_finalize_round_unlocked(
                round_id, value.completed_unix_us);
            finalize_round.commit();
            return {
                .finalized = true,
                .persisted_share_id = 0,
            };
        }
        impl_->flush_accounting_transaction_unlocked();
        const std::int64_t transient_alias = share_id;
        const std::int64_t promoted_round_id = found->second.round_id;
        Transaction promote(impl_->db);
        share_id = impl_->insert_staged_share_unlocked(found->second,
                                                       "high_difficulty");
        promote.commit();
        impl_->erase_transient_share_unlocked(found);
        impl_->remember_persisted_share_alias(
            transient_alias,
            PersistedShareIdentity{share_id, promoted_round_id});
    }
    else {
        impl_->flush_accounting_transaction_unlocked();
    }
    Transaction transaction(impl_->db);
    Statement update(
        impl_->db,
        "UPDATE shares SET status=?1,provenance=?2,completed_unix_us=?3,"
        "actual_difficulty_dec=?4,error_code=?5,error_message=?6,"
        "verifier_ticket_dec=coalesce(?7,verifier_ticket_dec),"
        "verifier_seed_id_dec=coalesce(?8,verifier_seed_id_dec),"
        "verifier_queue_ns=?9,verifier_hash_ns=?10,verifier_total_ns=?11 "
        "WHERE id=?12 AND status IN ('received','verifying')");
    update.bind(value.status, 1);
    update.bind(value.provenance, 2);
    update.bind(value.completed_unix_us, 3);
    update.bind_optional(value.actual_difficulty_dec, 4);
    update.bind_optional(value.error_code, 5);
    update.bind_optional(value.error_message, 6);
    update.bind_optional(value.verifier_ticket_dec, 7);
    update.bind_optional(value.verifier_seed_id_dec, 8);
    if (value.verifier_queue_ns) update.bind(static_cast<std::int64_t>(*value.verifier_queue_ns), 9);
    else update.bind_null(9);
    if (value.verifier_hash_ns) update.bind(static_cast<std::int64_t>(*value.verifier_hash_ns), 10);
    else update.bind_null(10);
    if (value.verifier_total_ns) update.bind(static_cast<std::int64_t>(*value.verifier_total_ns), 11);
    else update.bind_null(11);
    update.bind(share_id, 12);
    update.done();
    const bool changed = sqlite3_changes(impl_->db) == 1;
    if (changed) {
        Statement context(
            impl_->db,
            "SELECT session_id,connection_id,worker_id,template_generation,"
            "job_public_id,candidate_id,round_id FROM shares WHERE id=?1");
        context.bind(share_id, 1);
        require(context.row(), "finalized share context disappeared");
        const std::int64_t round_id = context.integer(6);
        impl_->insert_event_unlocked(EventInsert{
            .session_id = context.integer(0),
            .created_unix_us = value.completed_unix_us,
            .type = "share_result",
            .connection_id = context.is_null(1) ? std::nullopt
                                                : std::optional<std::int64_t>(context.integer(1)),
            .worker_id = context.is_null(2) ? std::nullopt
                                            : std::optional<std::int64_t>(context.integer(2)),
            .template_generation = context.is_null(3) ? std::nullopt
                                                       : std::optional<std::int64_t>(context.integer(3)),
            .job_public_id = context.is_null(4) ? std::nullopt
                : std::optional<PublicId>(exact_array<16>(context.blob(4), "share job public ID")),
            .share_id = share_id,
            .candidate_id = context.is_null(5) ? std::nullopt
                                               : std::optional<std::int64_t>(context.integer(5)),
            .round_id = round_id,
            .payload_json = std::string(kEmptyPayload),
        });
        impl_->add_share_total_unlocked(value.status, value.provenance,
                                        value.completed_unix_us);
        (void)impl_->try_finalize_round_unlocked(
            round_id, value.completed_unix_us);
    }
    transaction.commit();
    return {
        .finalized = changed,
        .persisted_share_id = share_id,
    };
}

void Database::set_candidate_admission(std::int64_t share_id,
                                       std::string_view admission)
{
    PriorityWriterLock lock(impl_->mutex);
    static constexpr std::array<std::string_view, 5> allowed{
        "not_candidate", "admitted", "deferred", "existing", "trusted_rate_limited"};
    require(std::find(allowed.begin(), allowed.end(), admission) != allowed.end(),
            "invalid candidate admission");
    require(share_id != 0, "share ID must be nonzero");
    if (share_id < 0) {
        auto found = impl_->transient_shares.find(share_id);
        require(found != impl_->transient_shares.end(), "transient share does not exist");
        found->second.value.candidate_admission = std::string(admission);
        return;
    }
    Statement update(impl_->db,
                     "UPDATE shares SET candidate_admission=?1 WHERE id=?2");
    update.bind(admission, 1);
    update.bind(share_id, 2);
    update.done();
    require(sqlite3_changes(impl_->db) == 1, "candidate admission share was absent");
}

} // namespace monero_solo
