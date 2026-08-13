# Read-only JSON API and event stream

The optional HTTP service exposes operational and persistent data only. It has
no HTML/dashboard assets and no control endpoint. Only `GET` is accepted;
POST/PUT/PATCH/DELETE return JSON `405` with `Allow: GET`.

## HTTP contract

- Base prefix: `/v1`.
- Response content is JSON. Unknown routes return a JSON 404.
- Every response includes `schema_version: 1` and `generated_at` as RFC 3339
  UTC with exactly six fractional digits and `Z`.
- Detail/singleton success wraps an object in `data`; collections also include
  a `page` object.
- Database IDs, H/s values, large counters, unsigned tickets/seed IDs, and
  difficulties are canonical decimal strings. Bounded heights/ports/counts
  remain JSON integers.
- Binary IDs/hashes/targets/blobs are lowercase hex without `0x`.
- Null is used for an unavailable nullable value. A missing detail is 404, not
  `data: null`.
- Ordinary collection order is increasing database ID. Default `limit` is 100
  and the maximum is `api.max_page_size`. Ranked share snapshots use their
  configured limit and deliberately do not accept cursors.
- Query names/values are percent decoded. Duplicate, empty, unknown, invalid,
  or oversized parameters return `400 invalid_query`.

Authentication is independent of Stratum:

```text
api.access_token null or ""  -> all routes accessible without a token
api.access_token nonempty    -> Authorization: Bearer <exact token>
```

The comparison is constant-time. Failure is 401 with
`WWW-Authenticate: Bearer`. Sensitive `include_blobs=true` views are available
only when nonempty API-token mode is configured and the request authenticated;
they return 403 when the API itself is configured unauthenticated.

Example success and error envelopes:

```json
{
  "schema_version": 1,
  "generated_at": "2026-08-12T05:30:00.000000Z",
  "data": {"alive": true, "version": "0.1.0", "uptime_seconds": 30}
}
```

```json
{
  "schema_version": 1,
  "generated_at": "2026-08-12T05:30:00.000000Z",
  "error": {
    "code": "invalid_cursor",
    "message": "The cursor is not valid for this resource"
  }
}
```

## Pagination and filters

A collection response is:

```json
{
  "schema_version": 1,
  "generated_at": "2026-08-12T05:30:00.000000Z",
  "data": [],
  "page": {"limit": 100, "next_cursor": null}
}
```

When `next_cursor` is nonnull, pass it unchanged with the same endpoint and
filters. It is unpadded base64url encoding of:

```text
0x01 || resource_tag_u16_be || last_database_id_u64_be
     || SHA256(canonical_filters)[0..15]
```

Canonical filters bind the endpoint path and sorted
`name NUL value NUL` pairs, excluding `cursor` and `limit`. A cursor from
another endpoint or filter set is invalid. It is opaque and is not an
authorization token.

Boolean filters are exactly `true`/`false`. IDs are canonical positive decimal
or the documented lowercase 32-hex public ID. Time bounds are inclusive,
strict RFC 3339 UTC microsecond strings. `peer` is one canonical IPv4/IPv6
address, never a CIDR. Comma enum lists contain unique values with no spaces.

## Endpoint matrix

All ordinary collection routes accept `limit` and `cursor` in addition to the
listed filters. The two ranked share routes accept `limit` only, bounded by
their respective configured cap.

| Route | Filters | Result |
| --- | --- | --- |
| `GET /v1/health/live` | none | `{alive, version, uptime_seconds}`; 200 while served |
| `GET /v1/health/ready` | none | Overall readiness, height/null, and database/entropy/daemon/template/verifier/stratum component states; 200 ready, otherwise 503 |
| `GET /v1/summary` | none | Server identity/mode, compact daemon state, connection/worker/share/candidate counters, open round, active-source H/s |
| `GET /v1/daemon` | none | Redacted live daemon/RPC/ZMQ/template snapshot; no credentials |
| `GET /v1/verifier` | none | Enabled/mode, native stats and tracked seed snapshots |
| `GET /v1/hashrate` | `source=verified|claimed|all` | Global six-window H/s; default active mode; `all` may report `mixed` |
| `GET /v1/persistence` | none | Schema/pragmas, file sizes, last commit, unresolved candidates, pending hook deliveries |
| `GET /v1/connections` | `active`, `worker_id`, `peer`, `after_time`, `before_time` | Connection resources |
| `GET /v1/connections/{32hex}` | none | Connection, share counters, and at most 20 recent job/share links |
| `GET /v1/workers` | `active`, exact `login`, exact `rigid`, `after_time`, `before_time` | Worker resources |
| `GET /v1/templates` | `height`, times, `include_blobs` | Public templates |
| `GET /v1/jobs` | `connection_id`, `template_id`, `height`, `active`, times, `include_blobs` | Private jobs |
| `GET /v1/shares` | `status`, `connection_id`, `worker_id`, `job_id`, `candidate_id`, `height`, `min_difficulty`, times | Share resources |
| `GET /v1/shares/top` | optional `round_id` | Accepted shares ranked by exact actual difficulty, globally or for one round; bounded snapshot |
| `GET /v1/shares/recent-high` | none | Newest accepted shares whose actual difficulty meets the configured threshold, independent of round; bounded snapshot |
| `GET /v1/shares/{decimal}` | none | `{share, submission_url}` |
| `GET /v1/hashes` | `role`, `share_status`, `connection_id`, `worker_id`, `job_id`, times | Claimed/computed hash resources |
| `GET /v1/submissions` | `state`, `connection_id`, `job_id`, `height`, `peer`, times | Candidate submissions |
| `GET /v1/submissions/{decimal}` | `include_blobs` only | `{submission, attempts, reconciliations, blocknotify}` |
| `GET /v1/rounds` | `state`, times | Local rounds |
| `GET /v1/rounds/current` | none | The one open round, or 503 |
| `GET /v1/bans` | `active`, `peer`, times | Persistent ban history |
| `GET /v1/events` | `type`, linked connection/worker/template/job/share/candidate/round IDs, times | Persistent committed events |

`status`/`share_status` values are `received`, `verifying`, `accepted`,
`stale`, `duplicate`, `low_difficulty`, `invalid_result`, `unknown_job`,
`malformed`, `unauthenticated`, `server_busy`, `verifier_failed`, or
`cancelled`. Candidate states are `journaled`, `dispatching`, `retry_wait`,
`accepted`, `rejected`, `ambiguous`, and `accepted_by_reconciliation`. Round
states are `open` and `closed`; hash roles are `claimed` and `computed`.

## Resource fields

Every field listed here is always present except the explicitly conditional
blob fields.

| Resource | Fields |
| --- | --- |
| connection | `id` 32-hex, `session_id` 32-hex, `worker_id` decimal/null, `peer`, `peer_port`, `listen_address`, `agent`, `opened_at`, `authenticated_at` null/time, `closed_at` null/time, `close_reason` null/string, `last_sent_height`, `rx_bytes`, `tx_bytes`, `active`, `hashrate` |
| worker | `id`, `login`, `rigid`, `first_seen_at`, `last_seen_at`, `active_connections`, `accepted_shares`, `rejected_shares`, `hashrate` |
| template | `id`, `session_id`, `generation`, `height`, `prev_hash`, `seed_hash`, `next_seed_hash`, `difficulty`, `wide_difficulty_hex`, `reserved_offset`, `reserve_size`, `fetched_at`, `fetch_reason`; sensitive view adds `blocktemplate_blob`, `blockhashing_blob` |
| job | `id` 32-hex, `connection_id` 32-hex, `template_id`, `height`, `seed_hash`, `verifier_seed_id`, `assigned_difficulty`, `target64_le`, `network_difficulty`, nonce/reserved offsets and sizes, `created_at`, `queued_at`, `expires_at`, `retired_at`; sensitive view adds `private_entropy`, `private_block_blob`, `hashing_blob` |
| share | `id`, `connection_id`, `worker_id`, `job_id`, `request_sequence`, `miner_request_id_type`, `miner_request_id`, receive/complete times, `nonce`, `height`, assigned/actual/network difficulties, `height_is_older`, `claimed_candidate`, `candidate_admission`, `status`, error code/message, `provenance`, `credited_difficulty`, verifier ticket/seed/timings, claimed/computed hashes and target booleans, `candidate_id`, `round_id` |
| hash | `share_id`, `role`, `hash`, share/network target booleans, `received_at`, `share_status`, `connection_id`, `worker_id`, `job_id`, assigned/actual/network/credited difficulties, `provenance`, `round_id` |
| submission | `id`, `candidate_key`, `first_share_id`, `job_id`, `connection_id`, `height`, `peer`, `miner_tx_hash`, expected/canonical block IDs, `state`, attempt/max/indeterminate/reconciliation fields, create/update/accept times, `terminal_reason`; sensitive detail adds `frozen_block_blob` |
| attempt | `id`, `candidate_id`, `attempt_number`, `rpc_request_id`, start/complete times, `classification`, HTTP/error/status/block-ID/excerpt nullable observations |
| reconciliation | `id`, `candidate_id`, `cycle_number`, `lookup_kind`, `rpc_request_id`, requested/observed IDs, observed height/miner-tx/orphan, start/complete times, `classification`, excerpt |
| round | `id`, open/close times, `state`, accepted candidate/height, miner transaction hash, block ID, `credited_difficulty`, `estimated_hashes`, `accepted_share_count`, `effort_finalized_at`, `effort` |
| ban | `id`, `peer`, create/expiry/evidence-window times, `reason`, `active`, ordered `abuse_event_ids` |
| event | `id`, `session_id`, `created_at`, `type`, nullable linked connection/worker/template/job/share/candidate/round IDs, `payload` |

`candidate_admission` is `not_candidate`, `admitted`, `deferred`, `existing`,
or `trusted_rate_limited`. `provenance` is `pending`, `verified`, or `claimed`.
Attempt classification is `dispatching`, `accepted`, `explicit_rejection`, or
`indeterminate`; reconciliation is `querying`, `positive`, `inconclusive`, or
`indeterminate`.

Every `hashrate` object is exact whole-number hashes per second:

```json
{
  "unit": "H/s",
  "source": "verified",
  "1m": "0",
  "5m": "0",
  "10m": "0",
  "1h": "0",
  "6h": "0",
  "24h": "0"
}
```

Each value is `floor(credited assigned difficulty / nominal window seconds)`
over `(now-window, now]`. Only accepted shares contribute. The nominal
denominator is used even when uptime is shorter. Sources are never silently
mixed.

`estimated_hashes` is the exact sum of credited assigned difficulty, not a
count of hashes observed directly. Round `effort.value` is
`floor(100 * sum(segment_estimated_hashes / segment_network_difficulty), 6)`.
The sum is performed as exact rational arithmetic and is floored only once,
after all network-difficulty/source segments are combined. Each segment also
reports its own `network_difficulty`, `estimated_hashes`, accepted share count,
source, and individually floored percentage. Closed-round effort is immutable
when `effort.finalized` is true; `effort_finalized_at` records that boundary.

The top-share response includes a `selection` object identifying global versus
per-round scope and its configured cap. The recent-high response identifies
its configured actual-difficulty threshold and cap. Both operate only on
accepted shares with persisted actual difficulty.

The persistence response reports a live, exact snapshot of admitted writer
queue items, their fixed 512-byte envelopes, and the subset in the priority
FIFO. Commands blocked outside admission by the item/byte bounds are not queue
items. `last_writer_error_*` are null; `last_commit_at` is the latest persistent
event time.

This release reads persistence schema 2 only. It intentionally performs no
migration or statistical backfill; start it with a freshly initialized
database when replacing a schema-1 test installation.

## Summary and readiness examples

```json
{
  "schema_version": 1,
  "generated_at": "2026-08-12T05:30:00.000000Z",
  "data": {
    "server": {
      "version": "0.1.0",
      "git_commit": "b1f1e365d7ab344ca5ca7f3334fdfbea5da7f9fd",
      "session_id": "0123456789abcdef0123456789abcdef",
      "started_at": "2026-08-12T05:00:00.000000Z",
      "uptime_seconds": 1800,
      "network": "mainnet",
      "verification": "verified",
      "stratum_authentication": "disabled",
      "api_authentication": "disabled"
    },
    "daemon": {
      "ready": true,
      "rpc": "healthy",
      "zmq": "disabled",
      "height": 3736190,
      "template_generation": "42",
      "template_id": "42"
    },
    "connections": {"active": 1, "total": "1"},
    "workers": {"active": 1, "total": "1"},
    "shares": {
      "pending": "0", "accepted": "0", "stale": "0",
      "duplicate": "0", "low_difficulty": "0",
      "invalid_result": "0", "infrastructure_failed": "0", "total": "0"
    },
    "candidates": {
      "active": "0", "accepted": "0", "rejected": "0",
      "ambiguous": "0", "total": "0"
    },
    "round": {
      "id": "1", "state": "open",
      "opened_at": "2026-08-12T05:00:00.000000Z",
      "estimated_hashes": "0", "accepted_share_count": "0",
      "effort": {
        "unit": "percent", "value": "0.000000",
        "precision": "0.000001", "rounding": "down",
        "basis": "credited_assigned_difficulty/network_difficulty",
        "finalized": false, "segments": []
      }
    },
    "hashrate": {
      "unit": "H/s", "source": "verified", "1m": "0", "5m": "0",
      "10m": "0", "1h": "0", "6h": "0", "24h": "0"
    }
  }
}
```

```json
{
  "schema_version": 1,
  "generated_at": "2026-08-12T05:30:00.000000Z",
  "data": {
    "ready": false,
    "height": 3736190,
    "components": {
      "database": {"ready": true, "degraded": false, "reason": null},
      "entropy": {"ready": true, "degraded": false, "reason": null},
      "daemon_rpc": {"ready": true, "degraded": false, "reason": null},
      "template": {"ready": true, "degraded": false, "reason": null},
      "verifier": {"ready": false, "degraded": false, "reason": "current seed preparing"},
      "stratum": {"ready": false, "degraded": false, "reason": "waiting for verifier seed"}
    }
  }
}
```

## Unix event stream

When enabled, connect to `events.unix_socket` with a Unix stream client. The
first line is a control frame:

```json
{"schema_version":1,"control":"stream_open","session_id":"0123456789abcdef0123456789abcdef","time_utc":"2026-08-12T05:30:00.000000Z","last_committed_event_id":"42"}
```

Later lines are the same committed event resources exposed by `/v1/events`,
one compact JSON object plus LF. There is no replay request protocol and
clients must not write to the socket; writing or EOF closes the stream. Use the
control high-watermark and `/v1/events` cursor endpoint to fill history before
following live records. A subscriber that exceeds
`max_pending_bytes_per_client` is disconnected. Mining never waits for a
subscriber.

The process unlinks a pre-existing path only when it is a Unix socket owned by
the effective user, applies the configured mode, and on shutdown unlinks only
the socket it owns.

```sh
curl -H 'Authorization: Bearer YOUR_API_TOKEN' \
  'http://127.0.0.1:8787/v1/shares?status=accepted&limit=100'
socat - UNIX-CONNECT:/run/monero-solo-stratum/events.sock
```
