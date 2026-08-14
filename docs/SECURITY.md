# Security model

This service accepts adversarial network traffic and can submit blocks that
affect an operator's mining income. Treat its database, daemon connection,
configuration, and binary supply chain as production security boundaries.

## Trust boundaries

Untrusted inputs are every miner byte and timing pattern, immutable accepted
peer address, API request, event-stream client behavior, daemon/ZMQ response,
SQLite contents after an unclean shutdown, and hook child result. The local
administrator and selected config file are trusted, but config secrets still
must never be copied into logs or data interfaces. `monerod` is the sole block
consensus authority; the native verifier is the ordinary-share PoW authority
when enabled.

Core invariants include:

- bounded frames, JSON depth, bodies, output queues, connections, worker
  queues, verifier inputs/outstanding work, candidate inflight state, API
  pages, event clients, and hook processes;
- checked offset/length arithmetic before reserved-byte or nonce mutation;
- full reparse after template mutation and final nonce insertion;
- constant-time comparisons for nonempty Stratum password, API token, and
  claimed/computed hashes;
- prepared SQL and conditional/unique database transitions;
- no candidate HTTP request before a unique FULL-synchronous journal commit;
- exact frozen bytes for every retry;
- no runtime fallback from verified to trusted mode;
- no shell interpretation for hooks;
- safe inspection before unlinking a Unix event socket;
- Digest-only daemon authentication when credentials are nonempty, redirects
  disabled, HTTPS certificate and hostname validation left enabled;
- no acceptance inference from a timeout, malformed response, or not-found
  reconciliation query.

## Verified and trusted modes

Verified mode (`verifier.enabled=true`) independently calculates every raw
RandomX hash using the exact seed ID associated with the retained job. Queue,
seed, memory, or verifier failure is an infrastructure result with no share
credit and no miner strike. A claimed/computed mismatch is rejected; the
computed value remains authoritative for duplicate and real-candidate rescue.

Trusted mode (`verifier.enabled=false`) allocates no RandomX verifier resources
and uses the miner's claimed hash for ordinary target/accounting decisions.
This allows an untrusted miner to forge shares and reported hashrate. It does
not let a miner make `monerod` accept an invalid block, but can consume
candidate/admission/RPC resources. Use trusted mode only on an
operator-controlled, authenticated, isolated miner network. Provenance is
reported as `claimed`, never misleadingly as verified.

## Entropy and private jobs

At startup the entropy manager reads exactly 32 bytes from Linux
`getrandom(2)`. It initializes the specified HMAC-DRBG-SHA-256 construction
with domain `monero-solo-stratum/HMAC-DRBG-SHA256/v1`. State is mutex-owned,
never persisted/logged, and re-seeded with a fresh independent 32-byte sample
on interval, output-call limit, or PID/fork change. The default timed interval
is 1,200 seconds (20 minutes); new job issuance fails closed if no reseed has
succeeded by the default 1,260-second maximum age.

Every private job makes two separate generates with domains
`private-template-entropy/v1` and `private-job-id/v1`. A database transaction
reserves each independently unique 16-byte output; collisions discard both.

A timed reseed failure preserves existing state, reports degraded health, and
retries with bounded exponential delay. New job issuance stops when maximum
reseed age is exceeded; retained submissions, verifier completions,
candidates/reconciliation, API, and recovery may continue. Count-limit/fork
reseed failure returns no output. A child process never returns parent-state
output.

## Immediate candidate safety

A miner-claimed hash that meets the full Monero target is forwarded without
waiting for RandomX only after:

1. strict connection/job/nonce/result structure checks;
2. global 48-byte share-key reservation;
3. exact nonce mutation and full-block reparse;
4. unique candidate fingerprint lookup;
5. immutable peer-IP candidate rate/in-flight admission;
6. durable unique candidate/frozen-block/dispatch commit.

The fingerprint is over frozen block bytes, not the claimed hash, so rotating
lies cannot trigger independent logical sequences. In verified mode, a claim
denied by a policy cap remains eligible for normal verification; a computed
real candidate bypasses policy caps and uses a reserved journal/daemon path. In
trusted mode there is no proof/rescue path, so denied admission is `Server
busy` and receives no credit.

Defaults are 12 candidates/minute per IP with burst 3, at most 2 in flight per
IP and 64 globally. Production rejects zero candidate policy limits; only
regtest may set all four to zero. Candidates remain bounded by database/RPC
capacity even there.

## Network and protocol defense

Peer identity is the normalized address accepted by the socket. The server
does not trust `X-Forwarded-For`, miner JSON, or proxy protocol. IPv4-mapped
IPv6 normalizes to IPv4. Deploying a generic TCP proxy therefore makes the
proxy the abuse identity; do not assume end-client attribution.

The aggressive profile combines:

- global/per-IP connection caps and connection-rate bucket;
- login and idle timers;
- request and submit token buckets;
- input line/JSON-depth/output caps;
- bounded submit and verifier queues;
- candidate rate/in-flight bounds;
- API peer token bucket and connection/response caps;
- Unix stream client/output caps.

Within the configured window, defaults ban after 10 malformed protocols, 10
failed passwords, 20 unknown jobs, or 20 definitive duplicates. Two oversized
lines ban. A sustained request/submit/candidate hammer is detected at the
configured multiple/rate duration. Verified false candidates, mismatches, and
trusted terminal explicit-rejection candidates use their separate limits and
windows. Low-difficulty work and infrastructure failure alone do not quickly
ban.

Default ban duration is 7,200 seconds. A ban transaction links durable evidence
and closes existing connections from that IP; reconnect is dropped before JSON
allocation. Ban expiry deactivates it but keeps history. A ban never discards
an already journaled candidate. API v1 cannot remove bans.

## Secrets and logging

The following must not appear in normal/debug/trace logs, SQLite, API, events,
or application crash output:

- Stratum access password;
- API bearer token;
- daemon RPC password;
- DRBG key/value state, OS entropy, and reseed material;
- private RandomX seed key bytes;
- any future wallet secret.

The payout address is public and may be logged. Database private job blobs and
hash material are intentionally persisted for reconstruction/audit and are
available only through an authenticated sensitive API view. They are not
wallet secrets, but should still be treated as operationally sensitive.
The 16-byte per-job template entropy is the one explicit logging exception.
Trace logging copies it to `job.queued.private_job_entropy` automatically;
`logging.include_private_job_entropy=true` enables the same field at debug.
Both modes require a mode-0600 JSONL file. The field is never accepted by
stderr logging and does not permit the OS seed, reseed material, DRBG state,
RandomX seed key, password, token, full private blob, or wallet-secret fields.
Protect and rotate a trace/private-entropy log like the database.

Store config mode 0600 under the service account. Keep database/log directories
non-world-writable. Never embed daemon credentials in `rpc_url`. Nonempty
daemon credentials use HTTP Digest only, but plain HTTP does not encrypt the
request; use loopback/private transport or HTTPS.

## No-shell `blocknotify`

The command template is parsed into argv at startup. `argv[0]` is absolute and
rechecked; there is no `PATH` search, variable expansion, glob, pipe,
redirection, command substitution, or shell. The hook receives only the public
miner transaction hash substituted for `%s`, uses `/dev/null` stdin/stdout,
captures bounded stderr, runs one child at a time, and times out at 60 seconds
with TERM then KILL grace.

Delivery is at-least-once across crashes. The external program must implement
idempotency by the provided hash, minimize its privileges, validate its own
destinations, and not rely on the server for exactly-once side effects.

## Data interfaces

The API is read-only but may expose peer labels/history. Bind loopback by
default, configure a separate bearer token, and use an authenticating TLS
reverse proxy if remote access is necessary. Sensitive blob views require
token mode and authentication. There is no CORS/browser dashboard contract.

The Unix event stream has filesystem permissions but no application-level
auth. Its directory and group ownership are the authorization boundary. Slow
or writing clients are disconnected; there is no command channel.

## Failure behavior

| Failure | Behavior |
| --- | --- |
| Startup entropy failure | Fatal before listener |
| Timed entropy failure | Degraded/retry; stop new jobs at max age |
| SQLite/schema/FULL/WAL failure | Fatal/not ready; no unjournaled candidate send |
| API query failure | JSON error; mining continues |
| Slow API/event client | Rate-limit/disconnect; mining continues |
| ZMQ unavailable | Polling continues; no consensus authority lost |
| Invalid template | No new work from it; retain eligible old jobs; readiness false |
| MSPV admission/seed failure | Infrastructure response; no claimed credit/fallback |
| Lost submit response | Retry identical bytes and preserve possible ambiguity |
| Hook failure | Durable retry; accepted block/round unchanged |

## Deliberate limitations

The Stratum and API servers do not implement TLS. The server does not interpret
trusted proxy headers/protocol, perform wallet accounting, reopen a closed
round after reorg/orphan discovery, or offer an administrative mutation API.
Templates requiring miner signature/spend-secret material are unsupported and
fail closed. Put defense in depth around public deployments and validate on
Monero regtest before risking production rewards.
