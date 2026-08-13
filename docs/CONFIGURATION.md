# Configuration

Configuration is one explicit startup-only JSON file. Unknown or duplicate
keys, non-object sections, wrong types, exponent/fraction number notation,
negative numbers, negative zero, invalid UTF-8/NUL, invalid addresses or
endpoints, and out-of-range values fail before public listeners open. There is
no fallback file and no hot reload. All fields are checked even when their
subsystem is disabled.

Use `config.example.json` as the canonical complete example:

```sh
monero-solo-stratum --check-config --config /absolute/path/config.json
```

The example binds Stratum and API to loopback and uses `/tmp` only so static
validation works on a development machine. Production should normally use
`/var/lib/monero-solo-stratum/state.sqlite3`,
`/run/monero-solo-stratum/events.sock`, mode `0600` for a credential-bearing
config file, and nonempty independent Stratum/API secrets.

## Required structure

The top level must contain exactly `schema_version`, `network`,
`wallet_address`, `blocknotify`, and the objects `stratum`, `daemon`,
`difficulty`, `verifier`, `entropy`, `database`, `events`, `api`, `defense`,
and `logging`.

The following nested keys are required even when null/disabled:

- `stratum.listen`, `stratum.access_password`
- `daemon.rpc_url`, `daemon.zmq_address`
- `difficulty.mode`, `difficulty.value`
- `verifier.enabled`
- `database.path`
- `events.enabled`, `api.enabled`, `defense.enabled`

Every other key below is optional and takes the listed default.

## Identity and top-level settings

| Key | Type/default | Validation and meaning |
| --- | --- | --- |
| `schema_version` | required integer `1` | No other configuration schema is accepted. |
| `network` | required string | `mainnet`, `testnet`, `stagenet`, or `regtest`. Daemon nettype must respectively be `mainnet`, `testnet`, `stagenet`, or `fakechain`. |
| `wallet_address` | required string, max 256 bytes | Checksum-valid primary address only. Prefixes: mainnet/regtest 18, testnet 53, stagenet 24. Integrated/subaddress and network-mismatched addresses are rejected. |
| `blocknotify` | required null/string | Null or empty disables. Otherwise max 65,536 bytes, must parse to nonempty argv, contain `%s`, and name an absolute executable. See below. |

## Stratum

| Key | Default | Allowed value |
| --- | ---: | --- |
| `listen` | required | 1..32 unique `IPv4:port`, `[IPv6]:port`, or `hostname:port` strings; port 1..65535. Every bind must succeed. |
| `access_password` | required `null` | Null or string up to 4,096 bytes. Null/empty disables auth; nonempty is an exact, untrimmed match. |
| `max_connections` | 2,048 | 1..1,000,000 |
| `max_connections_per_ip` | 128 | 1..`max_connections`; sized for rental-service fan-in |
| `login_timeout_ms` | 10,000 | 1,000..600,000 |
| `idle_timeout_ms` | 300,000 | 10,000..86,400,000 |
| `max_line_bytes` | 16,384 | 1,024..1,048,576, excluding LF |
| `max_output_bytes_per_connection` | 1,048,576 | 4,096..67,108,864 |
| `max_json_depth` | 32 | 4..128 |
| `job_history` | 6 | 1..64 current/prior jobs |
| `job_ttl_ms` | 120,000 | 1,000..3,600,000 for prior jobs |
| `max_pending_verifications_per_connection` | 8 | 1..4,096 and no greater than `verifier.max_outstanding` |
| `submit_workers` | 0 (auto) | 0..256; zero derives a nonzero count from available CPU while reserving capacity for I/O |

With a nonempty `access_password`, the pre-authentication connection-rate
bucket is bypassed so rental services can reconnect many miners behind one IP.
The configured global/per-IP connection ceilings still apply, and failed
authentication plus all post-connect request/submit limits remain enforced.

## Daemon

| Key | Default | Allowed value |
| --- | ---: | --- |
| `rpc_url` | required | Absolute `http`/`https` URL up to 4,096 bytes; no userinfo, query, fragment, redirect, or path other than `/`. |
| `rpc_username`, `rpc_password` | null | Both null, both empty, or both nonempty strings up to 4,096 bytes. Mixed states fail. Nonempty uses HTTP Digest only; HTTPS also verifies certificate and hostname. |
| `zmq_address` | required null | Null/empty for polling only; otherwise a libzmq endpoint up to 4,096 bytes. ZMQ is only a refresh hint. |
| `poll_interval_ms` | 20,000 | 1,000..300,000 |
| `request_timeout_ms` | 15,000 | 100..300,000 |
| `max_concurrent_requests` | 8 | 1..1,024 |
| `max_pending_requests` | 256 | 2..100,000 |
| `max_response_bytes` | 16,777,216 | 4,096..67,108,864, enforced while receiving |
| `refresh_retry_ms` | 1,000 | 100..60,000 |
| `submit_attempts` | 4 | 1..4; snapshotted into each candidate |
| `submit_retry_ms` | 2,000 | 100..60,000 |

Keep authenticated plain HTTP on loopback or a trusted private link. A password
does not encrypt transport.

## Difficulty

| Key | Allowed value |
| --- | --- |
| `mode` | Required `fixed` or `minimum`. |
| `value` | Required unsigned integer 1..18,446,744,073,709,551,615. |

`fixed` assigns exactly `value`. `minimum` treats it as a floor and accepts an
XMRig-compatible trailing `+<decimal>` request on a nonempty worker label. A
lower value is ignored; invalid/overflowing syntax rejects login. In fixed mode
the suffix remains part of the label. Vardiff is not implemented.

## Verifier

| Key | Default | Allowed value |
| --- | ---: | --- |
| `enabled` | required `true` | Boolean. False is trusted mode and allocates no MSPV/RandomX resources. |
| `memory_mode` | `fast` | `light` or `fast` |
| `workers` | 0 (auto) | 0..256; zero derives a nonzero count from available CPU while reserving capacity for I/O |
| `seed_init_threads` | 0 (auto) | 0..256; zero derives a nonzero count from available CPU while reserving capacity for I/O |
| `max_seeds` | 2 | 2..64 |
| `pending_capacity` | 256 | 1..1,000,000 |
| `max_outstanding` | 512 | `pending_capacity`..1,000,000 |
| `max_input_size` | 4,096 | 1..67,108,864 |
| `max_buffered_input_bytes` | 16,777,216 | `max_input_size`..17,179,869,184 |
| `large_pages` | `try` | `disabled`, `try`, or `require` |
| `jit` | `secure` | `disabled`, `enabled`, or `secure` |
| `aes` | `auto` | `auto` or `software` |
| `log_level` | `info` | `error`, `warning`, `info`, `debug`, or `trace` |

Fast mode needs roughly 2.08 GiB per resident seed dataset plus temporary cache
and VM memory. Light mode needs roughly 256 MiB per resident seed and is much
slower. At least two seeds are required for transitions. See `VERIFIER.md`.

## Entropy and database

| Key | Default | Allowed value |
| --- | ---: | --- |
| `entropy.reseed_interval_seconds` | 1,800 | 1..86,400 |
| `entropy.max_reseed_age_seconds` | 1,860 | interval..604,800 |
| `entropy.max_generate_calls` | 1,048,576 | 1..4,294,967,295 |
| `database.path` | required | Nonempty absolute path up to 4,096 bytes. Parent must exist and be a directory; database must not be a symlink. A world-writable parent is accepted only when sticky and securely owned, as `/tmp` is. |
| `database.busy_timeout_ms` | 5,000 | 1..60,000 |
| `database.max_writer_queue_items` | 100,000 | 1,024..10,000,000 |
| `database.max_writer_queue_bytes` | 67,108,864 | 1,048,576..1,073,741,824 |
| `database.retention_days` | 0 | Exactly 0 (unlimited) in this release |
| `database.store_rejected_shares` | true | Boolean |

Writer capacity must additionally cover
`defense.candidate_global_inflight + 1024` items in trusted mode, or
`verifier.max_outstanding + defense.candidate_global_inflight + 1024` items in
verified mode, and 512 bytes for each verifier/candidate reserve. Invalid
combinations fail static validation.

## Events and API

| Key | Default | Allowed value |
| --- | ---: | --- |
| `events.enabled` | required true | Boolean |
| `events.unix_socket` | `/run/monero-solo-stratum/events.sock` | Absolute path up to 4,096 bytes; nonempty when enabled |
| `events.permissions` | `0660` | Four octal characters; execute and other-user bits forbidden |
| `events.max_clients` | 8 | 1..1,024 |
| `events.max_pending_bytes_per_client` | 1,048,576 | 4,096..67,108,864 |
| `api.enabled` | required true | Boolean |
| `api.listen` | `127.0.0.1:8787` | One TCP endpoint, required when enabled |
| `api.access_token` | null | Null/string up to 4,096 bytes. Null/empty disables auth; nonempty requires exact `Bearer` token. |
| `api.max_page_size` | 1,000 | 1..10,000; request default is 100 |
| `api.max_connections` | 64 | 1..10,000 |
| `api.request_rate_per_second` | 20 | 1..1,000,000 per peer IP |
| `api.request_burst` | 40 | 1..1,000,000 per peer IP |
| `api.max_pending_bytes_per_connection` | 2,097,152 | 4,096..67,108,864 |
| `api.top_shares_limit` | 100 | 1..100; maximum/default rows in global and per-round actual-difficulty rankings |
| `api.recent_high_shares_limit` | 100 | 1..100; maximum/default rows in the round-independent recent-high view |
| `api.recent_high_share_min_difficulty` | 20,000,000,000 | 1..18,446,744,073,709,551,615; inclusive actual-difficulty threshold |

The API token and Stratum password are unrelated. An unauthenticated API cannot
request sensitive blob views even when it is loopback-only.

## Defense

`profile` is exactly `aggressive` in v1. Defense may be disabled for `regtest`,
for loopback-only listeners, or when a nonempty Stratum password protects every
listener. The supplied example uses the password/loopback-friendly simple mode;
the detailed rate/ban policy is available when hostile-network hardening is
enabled later.

| Key | Default | Allowed value |
| --- | ---: | --- |
| `enabled` | required; example false | Boolean |
| `profile` | `aggressive` | Exact string `aggressive` |
| `ban_seconds` | 7,200 | 1..2,592,000 |
| `connection_rate_per_minute`, `connection_burst` | 60, 20 | Each 1..1,000,000 |
| `request_rate_per_second`, `request_burst` | 50, 100 | Each 1..1,000,000 |
| `submit_rate_per_second`, `submit_burst` | 20, 40 | Each 1..1,000,000 |
| `malformed_limit`, `auth_failure_limit` | 10, 10 | Each 1..1,000,000 |
| `unknown_job_limit`, `duplicate_limit` | 20, 20 | Each 1..1,000,000 |
| `abuse_window_seconds` | 60 | 1..86,400 |
| `hammer_rate_multiplier` | 4 | 2..1,000 |
| `hammer_sustain_seconds` | 5 | 1..3,600 |
| `candidate_rate_per_minute`, `candidate_burst` | 12, 3 | Each 1..1,000,000 in production |
| `candidate_inflight_per_ip`, `candidate_global_inflight` | 2, 64 | Each 1..1,000,000; per-IP may not exceed global |
| `false_candidate_limit`, `false_candidate_window_seconds` | 3, 600 | Limit 1..1,000,000; window 1..86,400 |
| `trusted_candidate_rejection_limit`, window | 3, 600 | Same ranges |
| `verification_mismatch_limit`, window | 10, 600 | Same ranges |

Only on regtest may all four candidate rate/in-flight values be zero, which
removes that policy limit. A mixture of zero and nonzero fails validation.
Absolute database/HTTP bounds remain.

## Logging

| Key | Default | Allowed value |
| --- | ---: | --- |
| `level` | `info` | `error`, `warning`, `info`, `debug`, or `trace` |
| `file` | null | Null/empty for stderr or an absolute nonsymlink path up to 4,096 bytes with a safe existing parent |
| `include_private_job_entropy` | false | Boolean; true requires `level=debug|trace` and a nonempty absolute `file` |

All three settings are startup-only. Every accepted record is one complete JSONL
object with exactly `time`, `severity`, `code`, and `fields`. `time` is UTC
RFC 3339 with six fractional digits; `code` is a stable internal token; and
`fields` contains only closed, typed public-correlation keys. Severity
filtering occurs before record construction.

Null/empty `file` writes directly to stderr. A configured file is opened at
the exact path in append mode with `O_NOFOLLOW`; it must be a singly-linked
regular file owned by the service user and is forced to mode `0600`. One
mutex-protected low-level write emits each bounded record. Logging is not a
durability boundary and does not call `fsync`; a short/interrupted write is
handled, while a later write failure is contained and counted rather than
thrown into a mining thread.

The logger API accepts no arbitrary JSON, free-form message, or binary field.
Passwords, API token, daemon password, entropy/DRBG state, RandomX seed key
material, private blobs, and arbitrary exception text are never valid log
fields. Operational records may contain the public payout address, redacted
daemon endpoint, listener/ZMQ endpoint, fixed reason tokens, and bounded IDs.
At debug/trace the runtime records committed connection, job, and share
lifecycle correlation, target/difficulty/hash fields, verifier timings, and a
fixed `standard`/`nicehash` agent compatibility profile. It never copies raw,
miner-controlled agent text into JSONL.
`include_private_job_entropy=true` additionally writes the exact 16-byte job
entropy as 32 lowercase hex in `job.queued`; this is useful for a bounded test
capture but creates another durable copy of reconstruction material. The
dedicated typed field is rejected on stderr, below debug severity, or without
the explicit setting.

## `blocknotify` command grammar

The value is parsed once without a shell. ASCII whitespace separates argv,
single quotes preserve bytes, double quotes preserve whitespace and accept
backslash only for `"` and `\\`, and an outside backslash escapes the next
byte. Quotes must balance; argv must contain a literal `%s`; and `argv[0]` must
be an absolute regular executable. There is no variable expansion, globbing,
pipe, redirection, command substitution, or `PATH` search.

Every `%s` becomes the accepted candidate's lowercase 64-hex miner transaction
hash (not block ID). The command runs only after daemon `OK` or positive
reconciliation. It is durable at-least-once, so the hook must be idempotent by
that hash.

```json
"blocknotify": "/usr/local/bin/mined-block notify \"%s\""
```

## Secret semantics

Null and empty have the same disabled meaning for Stratum/API authentication.
Daemon username/password must have identical null/empty/nonempty state. Values
are byte-exact and never trimmed. Do not place a credential in a URL, worker
label, logging path, or hook argument. Protect the config file at the operating
system level; validation errors are designed not to echo secret values.
