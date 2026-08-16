#include "monero_solo/daemon.hpp"
#include "monero_solo/build_version.hpp"
#include "monero_solo/util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <curl/curl.h>

namespace monero_solo {
namespace {

constexpr std::size_t kExcerptLimit = 2048;

struct CurlGlobal final {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("curl global initialization failed");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl() {
    static CurlGlobal global;
    (void)global;
}

struct ReceiveBuffer {
    std::string bytes;
    std::size_t limit{};
    bool exceeded{};
};

std::size_t receive_callback(char *data, std::size_t size, std::size_t count,
                             void *opaque) noexcept {
    auto *buffer = static_cast<ReceiveBuffer *>(opaque);
    if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size) {
        buffer->exceeded = true;
        return 0;
    }
    const std::size_t amount = size * count;
    if (amount > buffer->limit || buffer->bytes.size() > buffer->limit - amount) {
        buffer->exceeded = true;
        return 0;
    }
    try {
        buffer->bytes.append(data, amount);
    }
    catch (...) {
        return 0;
    }
    return amount;
}

std::string sanitize_excerpt(std::string_view input) {
    std::string output;
    output.reserve(std::min(input.size(), kExcerptLimit));
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (output.size() == kExcerptLimit) {
            break;
        }
        if (byte == '\n' || byte == '\r' || byte == '\t' ||
            (byte >= 0x20U && byte != 0x7fU)) {
            output.push_back(static_cast<char>(byte));
        }
        else {
            output.push_back('?');
        }
    }
    return output;
}

std::optional<std::int64_t> json_integer(const nlohmann::json &value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto candidate = value.get<std::uint64_t>();
        if (candidate <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(candidate);
        }
    }
    return std::nullopt;
}

bool contains_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

std::optional<std::string> normalized_hash(const nlohmann::json &value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    std::string result = value.get<std::string>();
    if (!is_hex_64(result)) {
        return std::nullopt;
    }
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return result;
}

bool exact_string(const nlohmann::json &object, std::string_view key,
                  std::string_view expected) {
    const auto iterator = object.find(std::string(key));
    return iterator != object.end() && iterator->is_string() &&
           !contains_nul(iterator->get_ref<const std::string &>()) &&
           iterator->get_ref<const std::string &>() == expected;
}

} // namespace

bool is_hex_64(std::string_view value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return std::isxdigit(byte) != 0;
    });
}

DaemonRpcClient::DaemonRpcClient(std::string rpc_url,
                                 std::string username,
                                 std::string password,
                                 std::uint32_t timeout_ms,
                                 std::size_t max_response_bytes,
                                 std::uint32_t max_concurrent_requests,
                                 std::uint32_t max_pending_requests)
    : rpc_url_(std::move(rpc_url)),
      username_(std::move(username)),
      password_(std::move(password)),
      timeout_ms_(timeout_ms),
      max_response_bytes_(max_response_bytes),
      max_concurrent_requests_(max_concurrent_requests),
      max_pending_requests_(max_pending_requests) {
    ensure_curl();
    while (!rpc_url_.empty() && rpc_url_.back() == '/') {
        rpc_url_.pop_back();
    }
    if (rpc_url_.empty() || timeout_ms_ == 0U || max_response_bytes_ == 0U ||
        max_concurrent_requests_ == 0U || max_pending_requests_ < 2U) {
        throw std::invalid_argument("invalid daemon RPC client configuration");
    }
    if (username_.empty() != password_.empty()) {
        throw std::invalid_argument("daemon username and password must both be configured");
    }
}

bool DaemonRpcClient::acquire_request_slot(const bool priority) {
    std::unique_lock lock(scheduler_mutex_);
    if (scheduler_stopping_) return false;
    const std::uint32_t ordinary_limit = max_pending_requests_ - 1U;
    const std::uint32_t admitted_limit = priority
                                             ? max_pending_requests_
                                             : ordinary_limit;
    const std::uint64_t admitted =
        static_cast<std::uint64_t>(scheduler_inflight_) + scheduler_waiting_;
    if (admitted >= admitted_limit) {
        return false;
    }
    ++scheduler_waiting_;
    if (priority) ++scheduler_priority_waiting_;
    scheduler_condition_.wait(lock, [this, priority] {
        return scheduler_stopping_ ||
               (scheduler_inflight_ < max_concurrent_requests_ &&
                (priority || scheduler_priority_waiting_ == 0U));
    });
    --scheduler_waiting_;
    if (priority) --scheduler_priority_waiting_;
    if (scheduler_stopping_) return false;
    ++scheduler_inflight_;
    return true;
}

void DaemonRpcClient::stop() noexcept {
    {
        std::lock_guard lock(scheduler_mutex_);
        scheduler_stopping_ = true;
    }
    scheduler_condition_.notify_all();
}

void DaemonRpcClient::release_request_slot() noexcept {
    {
        std::lock_guard lock(scheduler_mutex_);
        if (scheduler_inflight_ != 0U) --scheduler_inflight_;
    }
    scheduler_condition_.notify_all();
}

DaemonRequestCounts DaemonRpcClient::request_counts() const noexcept {
    std::lock_guard lock(scheduler_mutex_);
    return {scheduler_inflight_, scheduler_waiting_};
}

std::int64_t DaemonRpcClient::next_request_id() noexcept {
    for (;;) {
        const auto current = next_id_.fetch_add(1, std::memory_order_relaxed);
        if (current > 0) {
            return current;
        }
        next_id_.store(1, std::memory_order_relaxed);
    }
}

RpcObservation DaemonRpcClient::json_rpc(
    std::string_view method, const nlohmann::json &params,
    const std::function<void(std::int64_t)> &before_dispatch) {
    const std::int64_t id = next_request_id();
    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    return post(rpc_url_ + "/json_rpc", request.dump(), id, false,
                before_dispatch);
}

RpcObservation DaemonRpcClient::endpoint(std::string_view path,
                                         const nlohmann::json &body) {
    std::string normalized(path);
    if (normalized.empty() || normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    return post(rpc_url_ + normalized, body.dump(), std::nullopt);
}

RpcObservation DaemonRpcClient::get_info() {
    return json_rpc("get_info", nlohmann::json::object());
}

RpcObservation DaemonRpcClient::get_height() {
    return endpoint("/getheight", nlohmann::json::object());
}

RpcObservation DaemonRpcClient::get_block_template(std::string_view wallet_address) {
    return json_rpc("getblocktemplate", {
        {"wallet_address", wallet_address}, {"reserve_size", 16}});
}

SubmitObservation DaemonRpcClient::submit_block(
    std::span<const std::uint8_t> frozen_block,
    const std::function<void(std::int64_t)> &before_dispatch) {
    const std::int64_t id = next_request_id();
    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "submitblock"},
        {"params", nlohmann::json::array({hex_encode(frozen_block)})}};
    return classify_submit(post(rpc_url_ + "/json_rpc", request.dump(), id,
                                true, before_dispatch));
}

RpcObservation DaemonRpcClient::get_block_by_hash(
    std::string_view expected_hash,
    const std::function<void(std::int64_t)> &before_dispatch) {
    const std::int64_t id = next_request_id();
    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "get_block"},
        {"params", {{"hash", expected_hash}, {"height", 0},
                    {"fill_pow_hash", false}}}};
    return post(rpc_url_ + "/json_rpc", request.dump(), id, true,
                before_dispatch);
}

RpcObservation DaemonRpcClient::get_block_by_height(
    std::uint64_t height,
    const std::function<void(std::int64_t)> &before_dispatch) {
    const std::int64_t id = next_request_id();
    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", "get_block"},
        {"params", {{"hash", ""}, {"height", height},
                    {"fill_pow_hash", false}}}};
    return post(rpc_url_ + "/json_rpc", request.dump(), id, true,
                before_dispatch);
}

RpcObservation DaemonRpcClient::post(std::string_view absolute_url,
                                     std::string_view encoded_body,
                                     std::optional<std::int64_t> expected_id,
                                     const bool priority,
                                     const std::function<void(std::int64_t)> &
                                         before_dispatch) {
    RpcObservation result;
    result.request_id = expected_id.value_or(0);
    if (!acquire_request_slot(priority)) {
        result.kind = RpcObservationKind::transport_error;
        result.diagnostic = "daemon request scheduler capacity exceeded";
        return result;
    }
    int permit = 0;
    auto release = [this](int *) noexcept { release_request_slot(); };
    const std::unique_ptr<int, decltype(release)> request_slot(&permit, release);
    using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlPtr curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        result.diagnostic = "curl handle allocation failed";
        return result;
    }

    ReceiveBuffer response{{}, max_response_bytes_, false};
    std::array<char, CURL_ERROR_SIZE> error{};
    curl_slist *raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Accept: application/json");
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    using HeaderPtr = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
    HeaderPtr headers(raw_headers, &curl_slist_free_all);

    const std::string url(absolute_url);
    const std::string body(encoded_body);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error.data());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS,
                     static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT,
                     "monero-solo-stratum/" MSS_VERSION);
    if (!username_.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, username_.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, password_.c_str());
    }

    // All local allocation/configuration has succeeded and the scheduler slot
    // is owned. Durably record dispatch intent at the last barrier before the
    // first operation that can put request bytes on the wire.
    if (before_dispatch) {
        {
            std::lock_guard lock(scheduler_mutex_);
            if (scheduler_stopping_) {
                result.diagnostic = "daemon request scheduler stopped";
                return result;
            }
        }
        before_dispatch(result.request_id);
    }
    const CURLcode status = curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.http_status);
    result.response_excerpt = sanitize_excerpt(response.bytes);
    if (response.exceeded) {
        result.kind = RpcObservationKind::response_too_large;
        result.diagnostic = "daemon response exceeded configured limit";
        return result;
    }
    if (status != CURLE_OK) {
        result.kind = RpcObservationKind::transport_error;
        result.diagnostic = error.front() != '\0' ? std::string(error.data())
                                                  : std::string(curl_easy_strerror(status));
        return result;
    }
    if (result.http_status != 200L) {
        result.kind = RpcObservationKind::http_error;
        result.diagnostic = "daemon returned non-200 HTTP status";
        return result;
    }
    try {
        result.document = nlohmann::json::parse(response.bytes);
    }
    catch (const nlohmann::json::exception &) {
        result.kind = RpcObservationKind::malformed_json;
        result.diagnostic = "daemon returned malformed JSON";
        return result;
    }
    if (!result.document.is_object()) {
        result.kind = RpcObservationKind::invalid_envelope;
        result.diagnostic = "daemon JSON root is not an object";
        return result;
    }
    if (expected_id.has_value()) {
        const auto version = result.document.find("jsonrpc");
        if (version == result.document.end() || !version->is_string() ||
            version->get_ref<const std::string &>() != "2.0") {
            result.kind = RpcObservationKind::invalid_envelope;
            result.diagnostic = "daemon JSON-RPC version was missing or invalid";
            return result;
        }
        const auto iterator = result.document.find("id");
        if (iterator == result.document.end() || json_integer(*iterator) != expected_id) {
            result.kind = RpcObservationKind::mismatched_id;
            result.diagnostic = "daemon JSON-RPC id did not match";
            return result;
        }
    }
    result.kind = RpcObservationKind::valid;
    return result;
}

SubmitObservation DaemonRpcClient::classify_submit(const RpcObservation &observation) {
    SubmitObservation result;
    result.request_id = observation.request_id;
    result.http_status = observation.http_status;
    result.response_excerpt = observation.response_excerpt;
    result.diagnostic = observation.diagnostic;
    if (!observation.valid()) {
        return result;
    }

    const auto &document = observation.document;
    const auto error_iterator = document.find("error");
    if (error_iterator != document.end() && !error_iterator->is_null()) {
        if (error_iterator->is_object()) {
            const auto message = error_iterator->find("message");
            if (message != error_iterator->end() && message->is_string() &&
                !contains_nul(message->get_ref<const std::string &>())) {
                result.classification = SubmitClassification::explicit_rejection;
                result.rpc_error_code = 0;
                const auto code = error_iterator->find("code");
                if (code != error_iterator->end()) {
                    if (const auto integer = json_integer(*code); integer.has_value()) {
                        result.rpc_error_code = integer;
                    }
                }
                result.diagnostic = "daemon returned an explicit JSON-RPC error";
                return result;
            }
        }
        result.diagnostic = "daemon returned an invalid JSON-RPC error object";
        return result;
    }

    const auto response = document.find("result");
    if (response == document.end() || !response->is_object()) {
        result.diagnostic = "daemon submit response has no result object";
        return result;
    }
    const auto status = response->find("status");
    if (status == response->end() || !status->is_string() ||
        status->get_ref<const std::string &>().empty() ||
        contains_nul(status->get_ref<const std::string &>())) {
        result.diagnostic = "daemon submit response has no valid status";
        return result;
    }
    result.daemon_status = status->get<std::string>();
    if (*result.daemon_status == "OK") {
        result.classification = SubmitClassification::accepted;
        if (const auto block = response->find("block_id"); block != response->end()) {
            result.daemon_block_id = normalized_hash(*block);
        }
        result.diagnostic.clear();
    }
    else {
        result.classification = SubmitClassification::explicit_rejection;
        result.diagnostic = "daemon returned a non-OK status";
    }
    return result;
}

ReconciliationEvidence DaemonRpcClient::classify_reconciliation(
    const RpcObservation &observation,
    std::uint64_t expected_height,
    std::string_view expected_miner_tx_hash,
    std::string_view expected_block_id,
    std::optional<std::string_view> requested_block_id) {
    ReconciliationEvidence evidence;
    evidence.response_excerpt = observation.response_excerpt;
    evidence.diagnostic = observation.diagnostic;
    if (!observation.valid()) {
        evidence.indeterminate = observation.kind == RpcObservationKind::transport_error ||
                                 observation.kind == RpcObservationKind::http_error ||
                                 observation.kind == RpcObservationKind::response_too_large ||
                                 observation.kind == RpcObservationKind::malformed_json ||
                                 observation.kind == RpcObservationKind::mismatched_id;
        return evidence;
    }

    const auto &document = observation.document;
    if (const auto error = document.find("error");
        error != document.end() && !error->is_null()) {
        evidence.diagnostic = "get_block returned no positive evidence";
        return evidence;
    }
    const auto result = document.find("result");
    if (result == document.end() || !result->is_object() ||
        !exact_string(*result, "status", "OK")) {
        evidence.diagnostic = "get_block result was not authoritative OK";
        return evidence;
    }
    const auto header = result->find("block_header");
    if (header == result->end() || !header->is_object()) {
        evidence.diagnostic = "get_block omitted block_header";
        return evidence;
    }
    const auto orphan = header->find("orphan_status");
    if (orphan == header->end() || !orphan->is_boolean()) {
        evidence.diagnostic = "get_block omitted orphan_status";
        return evidence;
    }
    evidence.orphan = orphan->get<bool>();
    const auto height = header->find("height");
    if (height == header->end() || !height->is_number_unsigned()) {
        evidence.diagnostic = "get_block omitted height";
        return evidence;
    }
    evidence.height = height->get<std::uint64_t>();
    const auto block_id = header->find("hash");
    if (block_id == header->end()) {
        evidence.diagnostic = "get_block omitted block hash";
        return evidence;
    }
    evidence.block_id = normalized_hash(*block_id);
    const auto miner_tx = result->find("miner_tx_hash");
    if (miner_tx == result->end()) {
        evidence.diagnostic = "get_block omitted miner transaction hash";
        return evidence;
    }
    evidence.miner_tx_hash = normalized_hash(*miner_tx);
    const auto blob = result->find("blob");
    if (blob != result->end() && blob->is_string() &&
        !contains_nul(blob->get_ref<const std::string &>())) {
        evidence.blob_hex = blob->get<std::string>();
    }

    std::string normalized_expected_tx(expected_miner_tx_hash);
    std::transform(normalized_expected_tx.begin(), normalized_expected_tx.end(),
                   normalized_expected_tx.begin(), [](unsigned char byte) {
                       return static_cast<char>(std::tolower(byte));
                   });
    std::string normalized_expected_block(expected_block_id);
    std::transform(normalized_expected_block.begin(), normalized_expected_block.end(),
                   normalized_expected_block.begin(), [](unsigned char byte) {
                       return static_cast<char>(std::tolower(byte));
                   });
    if (!is_hex_64(normalized_expected_tx) ||
        !is_hex_64(normalized_expected_block)) {
        evidence.diagnostic = "local reconciliation identity was invalid";
        return evidence;
    }
    if (*evidence.orphan || *evidence.height != expected_height ||
        !evidence.block_id.has_value() || !evidence.miner_tx_hash.has_value() ||
        *evidence.block_id != normalized_expected_block ||
        *evidence.miner_tx_hash != normalized_expected_tx ||
        !evidence.blob_hex.has_value()) {
        evidence.diagnostic = "get_block evidence did not identify the local candidate";
        return evidence;
    }
    if (requested_block_id.has_value()) {
        std::string requested(*requested_block_id);
        std::transform(requested.begin(), requested.end(), requested.begin(),
                       [](unsigned char byte) {
                           return static_cast<char>(std::tolower(byte));
                       });
        if (!is_hex_64(requested) || *evidence.block_id != requested) {
            evidence.diagnostic = "hash reconciliation returned a different block";
            return evidence;
        }
    }

    // Full local blob parsing and independent miner-tx verification is performed
    // by CandidateManager before accepting this otherwise positive envelope.
    evidence.positive = true;
    evidence.diagnostic.clear();
    return evidence;
}

} // namespace monero_solo
