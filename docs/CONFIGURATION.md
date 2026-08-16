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

## Upgrading configuration schema 1

Configuration schema 2 is intentionally strict and has no implicit upgrade.
Before starting this release, preserve the original file, change only the
following fields, and validate the result:

- set top-level `schema_version` to `2`;
- remove `stratum.job_history`;
- remove `database.retention_days` and `database.store_rejected_shares`;
- add `database.min_persisted_share_difficulty` if desired (default
  `80000000000`, inclusive);
- add `database.accounting_flush_interval_ms` if desired (default `1000`); and
- ensure `api.recent_high_share_min_difficulty` is at least the database
  persistence threshold. The old example value `20000000000` must therefore be
  raised to at least `80000000000` when the new default is used.

This example performs those exact transformations while preserving every other
JSON value and the original ownership/mode. It then moves the old schema-1/2
SQLite files aside instead of deleting them. Review `CONFIG_PATH` and the
derived `DB_PATH` before running it:

```sh
set -euo pipefail

CONFIG_PATH=/etc/monero-solo-stratum/config.json
DB_PATH=$(jq -er '.database.path | select(type == "string")' "$CONFIG_PATH")
case "$DB_PATH" in
  /*) ;;
  *) printf 'database.path is not absolute: %s\n' "$DB_PATH" >&2; exit 1 ;;
esac
test "$DB_PATH" != /

UPGRADE_STAMP=$(date -u +%Y%m%dT%H%M%SZ)
CONFIG_BACKUP="$CONFIG_PATH.schema1.$UPGRADE_STAMP"
test ! -e "$CONFIG_BACKUP"
printf 'Configuration to transform after exact backup:\n  %s\n  -> %s\n' \
  "$CONFIG_PATH" "$CONFIG_BACKUP"
printf 'Existing SQLite files to move aside (none are deleted):\n'
for DB_SOURCE in "$DB_PATH" "$DB_PATH-wal" "$DB_PATH-shm"; do
  if test -e "$DB_SOURCE"; then
    test ! -e "$DB_SOURCE.schema-old.$UPGRADE_STAMP"
    printf '  %s\n  -> %s\n' \
      "$DB_SOURCE" "$DB_SOURCE.schema-old.$UPGRADE_STAMP"
  fi
done
printf 'Proceed? [y/N] '
read -r UPGRADE_APPROVAL
case "$UPGRADE_APPROVAL" in
  y|Y|yes|YES) ;;
  *) printf 'aborted; nothing changed\n' >&2; exit 1 ;;
esac

systemctl stop monero-solo-stratum.service
cp --archive --no-clobber -- "$CONFIG_PATH" "$CONFIG_BACKUP"
CONFIG_TMP=$(mktemp --tmpdir="$(dirname -- "$CONFIG_PATH")" .config.json.XXXXXX)
jq '
  .schema_version = 2
  | del(.stratum.job_history,
        .database.retention_days,
        .database.store_rejected_shares)
  | .database.min_persisted_share_difficulty //= 80000000000
  | .database.accounting_flush_interval_ms //= 1000
  | .api.recent_high_share_min_difficulty =
      ([.api.recent_high_share_min_difficulty // 80000000000,
        .database.min_persisted_share_difficulty] | max)
' "$CONFIG_PATH" >"$CONFIG_TMP"
chown --reference="$CONFIG_PATH" -- "$CONFIG_TMP"
chmod --reference="$CONFIG_PATH" -- "$CONFIG_TMP"
monero-solo-stratum --check-config --config "$CONFIG_TMP"
mv -- "$CONFIG_TMP" "$CONFIG_PATH"

for DB_SOURCE in "$DB_PATH" "$DB_PATH-wal" "$DB_PATH-shm"; do
  if test -e "$DB_SOURCE"; then
    mv --no-clobber -- \
      "$DB_SOURCE" "$DB_SOURCE.schema-old.$UPGRADE_STAMP"
  fi
done

monero-solo-stratum --check-config --config "$CONFIG_PATH"
systemctl start monero-solo-stratum.service
```

The database reset targets only the configured SQLite path and its exact
`-wal`/`-shm` companions. It does not inspect, move, or delete the Monero daemon
data directory, and it does not remove any other file under
`/etc/monero-solo-stratum`.

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
| `schema_version` | required integer `2` | No other configuration schema is accepted. |
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
| `job_ttl_ms` | 120,000 | 1,000..3,600,000; applies only to noncurrent jobs at a different height after reorg |
| `max_pending_verifications_per_connection` | 8 | 1..4,096 and no greater than `verifier.max_outstanding` |
| `submit_workers` | 0 (auto) | 0..256; zero derives a nonzero count from available CPU while reserving capacity for I/O |

With a nonempty `access_password`, the pre-authentication connection-rate
bucket is bypassed so rental services can reconnect many miners behind one IP.
The configured global/per-IP connection ceilings still apply, and failed
authentication plus all post-connect request/submit limits remain enforced.

Every successfully queued poll/notification refresh creates fresh jobs. All
jobs at the connection's latest queued height remain valid regardless of
`job_ttl_ms`; they accumulate until a strictly higher-height job is queued, the
connection closes, or the process restarts. This deliberately favors valid
same-height submissions over a fixed memory bound. `job_ttl_ms` applies only to
noncurrent work at a different height that can remain visible during a
downward-reorg sequence.

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
| `entropy.reseed_interval_seconds` | 1,200 | 1..86,400 |
| `entropy.max_reseed_age_seconds` | 1,260 | interval..604,800 |
| `entropy.max_generate_calls` | 1,048,576 | 1..4,294,967,295 |
| `database.path` | required | Nonempty absolute path up to 4,096 bytes. Parent must exist and be a directory; database must not be a symlink. A world-writable parent is accepted only when sticky and securely owned, as `/tmp` is. |
| `database.busy_timeout_ms` | 5,000 | 1..60,000 |
| `database.max_writer_queue_items` | 100,000 | 1,024..10,000,000 |
| `database.max_writer_queue_bytes` | 67,108,864 | 1,048,576..1,073,741,824 |
| `database.min_persisted_share_difficulty` | 80,000,000,000 | 1..18,446,744,073,709,551,615; inclusive authoritative actual-difficulty threshold |
| `database.accounting_flush_interval_ms` | 1,000 | 10..60,000; maximum ordinary accounting batch interval |

In verified mode, persisted-share selection uses only the independently
computed hash. In trusted mode it uses the claimed hash. Candidate and
security-evidence shares are always persisted regardless of the threshold.
Other terminal shares below the threshold update compact accounting totals but
do not receive individual database rows; detailed low-value submissions remain
available in the configured debug/trace JSONL. An unclean process exit may
lose at most one configured flush interval of ordinary aggregate telemetry.
Candidate state and security evidence are committed synchronously and are not
subject to that loss window. Public templates and private jobs are
live-memory/trace data and are never stored as SQLite rows or blobs. Same-height
jobs intentionally accumulate without a fixed count/TTL bound until a strictly
higher-height job is queued, the connection closes, or the process restarts.

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
| `api.recent_high_share_min_difficulty` | 80,000,000,000 | `database.min_persisted_share_difficulty`..18,446,744,073,709,551,615; inclusive actual-difficulty threshold |

The recent-high API threshold cannot be lower than the persistence threshold,
because `/v1/shares` exposes retained history rather than an unbounded record
of every submitted share.

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
| `include_private_job_entropy` | false | Boolean; enables private job entropy at `debug`; `trace` includes it automatically. Either path requires a nonempty absolute `file` |

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
Each `job.queued` record contains the 32-lowercase-hex `connection_public_id`
and independent `job_public_id`. At `trace`, it also contains the exact 16-byte
per-job template entropy as 32 lowercase hex in `private_job_entropy`.
`include_private_job_entropy=true` enables the same field at `debug`. Either
mode creates another durable copy of reconstruction material and therefore
requires a configured file; private job entropy is never accepted on stderr or
below debug severity. The OS seed, reseed material, and DRBG state are never
logged.

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
