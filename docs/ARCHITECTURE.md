# Architecture

## Product boundary

The process terminates ordinary XMRig CryptoNote `simple` Stratum connections,
talks directly to one configured `monerod`, and writes one SQLite database. It
does not proxy to a pool or wallet RPC. The configured primary address owns any
reward; login labels are accounting metadata only.

The main data flow is:

```text
monerod RPC poll / optional ZMQ hint
                 |
                 v
       validated public template
                 |
      private entropy + job ID
                 |
                 v
          XMRig simple job
                 |
                 v
   duplicate reserve / candidate test
          |                    |
          |                    +-- durable frozen-block journal
          |                                  |
          v                                  v
   in-process RandomX                 immediate submitblock
          |                                  |
          +--------------+-------------------+
                         v
               SQLite / API / events
```

## Components and ownership

| Component | Responsibility and owned state |
| --- | --- |
| `Config` | Strict startup-only JSON, complete key/range checking, endpoint/address/path validation, and parsed no-shell hook argv. |
| `Logger` | Thread-safe bounded JSONL records with UTC microseconds, severity filtering, stable codes, and a closed set of public string/integer correlation fields; stderr or an `O_NOFOLLOW` append-only mode-0600 file. |
| `EntropyManager` | Process-private HMAC-DRBG-SHA-256 state seeded with Linux `getrandom(2)`; independent private-template entropy and job-ID draws. |
| `Database` | Schema v1, WAL/FULL guarantees, sessions, templates/jobs/shares, duplicates, candidates, rounds, bans, events, hashrate, and notification delivery. |
| `DaemonRpcClient` | Bounded libcurl JSON-RPC requests, Digest-only configured authentication, template and submit/reconciliation observation classification. |
| `ZmqSubscriber` | Optional dynamically loaded `libzmq` subscriber. Notifications are refresh hints; polling remains authoritative. |
| Monero primitives | Primary-address decoding, strict block/miner-transaction parsing, reserved-byte and nonce mutation, tree/hash blobs, block IDs, and exact difficulty checks. |
| `Verifier` adapter | RAII wrapper around the exact `mspv_*` API, seed reference tracking, bounded submissions, completion correlation, and orderly shutdown. |
| `StratumServer` | Nonblocking socket event loop, bounded per-connection frames/output, authentication, job delivery, keepalive, and bounded submit worker queue. |
| `DuplicateRegistry` | Process-global generation-tagged `entropy[16] || result[32]` reservations, with height/source lifecycle references. |
| `DefenseEngine` | Immutable peer-IP token buckets, threshold windows, candidate admission, persistent ban callbacks, and active candidate counts. |
| Candidate workers | Send the exact journaled frozen block, classify attempts, preserve uncertainty, reconcile positive chain evidence, and release admission on terminal state. |
| `ApiService` | Bounded read-only HTTP `/v1` interface backed by a separate SQLite read-only connection and live snapshots. |
| `EventStream` | Optional Unix NDJSON broadcaster of committed event records with a per-client output cap. |
| `BlockNotifySupervisor` | One no-shell child at a time, durable at-least-once claims, timeout, captured stderr bound, and retry schedule. |

## Startup and readiness

Startup is fail-closed:

1. Strictly parse and statically validate the explicit config file.
2. Open SQLite, require `foreign_keys=ON`, `journal_mode=WAL`, and
   `synchronous=FULL`, validate schema v1, and recover interrupted durable
   states.
3. Create the server session/open round and restore bans and active duplicate
   identities.
4. Instantiate the OS-seeded entropy manager and, in verified mode, start the
   native verifier.
5. Start enabled read-only data interfaces in a not-ready state.
6. verify `get_info.nettype`, reconcile recoverable candidate rows, fetch and
   locally validate a template, and prepare/activate its exact RandomX seed.
7. Bind Stratum listeners and mark readiness true.

Readiness requires usable database and entropy state, a recent valid daemon
template, the exact seed in verified mode, and bound Stratum listeners. ZMQ,
event consumers, API history clients, and hook delivery are deliberately not
readiness dependencies.

## Template and job ownership

Every valid `getblocktemplate` response is installed, including a same-height
or byte-identical response, and receives a monotonic generation. The full block
is parsed locally; its previous hash, coinbase height, exact 16-byte reserved
region, and regenerated hashing blob must match the daemon response.

For each connection and template, the runtime copies the full block, draws
16 independent entropy bytes and a separate 16-byte job ID, replaces only the
reserved region, reparses, and derives a new hashing blob. SQLite `UNIQUE`
constraints on both entropy and public job ID are the lifetime reservation.
The job retains the exact private block, hashing blob, seed ID, offsets,
targets, and expiry. A nonce is never applied to a newer template.

## Share and candidate paths

Cheap structural and ownership checks precede cryptographic work. A
structurally accepted share reserves the global binary identity
`private_entropy || claimed_hash`. Verified mode submits an owned hashing blob
copy to MSPV and treats only the computed hash as authoritative. Trusted mode
uses the claim and labels its provenance `claimed`.

Staleness is evaluated only after work validity is known:

```text
stale = work_is_valid
     && submitted_job_height != 0
     && connection_last_sent_height > submitted_job_height
```

It depends on the latest complete job queued to that same connection, not
global daemon height or template generation.

A claimed network candidate does not wait for RandomX. The runtime freezes the
nonce-mutated full block and calculates:

```text
SHA256("monero-solo-stratum/candidate/v1\0" || frozen_full_block)
```

The unique candidate row, frozen bytes, and dispatch intent commit before the
first RPC. A later computed real candidate uses the same high-priority journal
path even if claimed-candidate admission was deferred. All retries reuse the
identical bytes. One `OK` accepts; all-explicit rejection rejects; any
indeterminate observation with no `OK` leaves the candidate ambiguous.

## Concurrency and backpressure

- Stratum sockets are owned by one event-loop thread. Bounded, prestarted
  ordinary and claimed-candidate worker pools perform submit processing and
  return results to the loop; neither grows with connection count.
- MSPV owns seed-preparation and hashing threads. Its callbacks only set a
  coalescible wake state; completions are drained outside callbacks.
- Template refresh, verifier draining, committed-event forwarding, candidate
  dispatch/reconciliation, API HTTP workers, Unix event delivery, and hook
  supervision have independent bounded threads/queues.
- Each Stratum/API/event client has a hard output/body limit. Admission is
  all-or-nothing; slow clients are closed rather than allowed to delay mining.
- Logging serializes one bounded record at a time around an unbuffered
  append-write. It never `fsync`s the mining path and contains/counts
  post-construction output failures.
- Candidate bytes live in the durable journal. Capacity delays a dispatch but
  does not delete an admitted candidate.

The database wrapper serializes synchronous callers through one bounded,
prioritized scheduler around the SQLite writer connection. Candidate and
verifier-completion durability commands use the priority FIFO. Ordinary
admission stops before consuming the configured priority item and byte
reserve; all command envelopes are fixed at 512 bytes and reference immutable
blobs. The API uses its own read-only WAL connection and reports live admitted
queue item, byte, and priority counts.

## Shutdown and recovery

Graceful shutdown stops new admission/listeners, persists recoverable candidate
state, drains the verifier, stops worker threads, marks the session clean, and
closes data interfaces. An uncertain transport observation is never rewritten
as rejection merely to exit.

At open, `dispatching` attempts become recoverable, orphaned `blocknotify`
`running` rows return to `pending`, active bans/duplicates are restored, and
nonterminal candidates are reconciled before another due dispatch. Candidate
acceptance and round close are conditional transactions, so replays and
restart recovery cannot logically accept a candidate twice.

## Deliberate v1 limitations

There is no built-in TLS, proxy-protocol trust, pool upstream/failover, vardiff,
NiceHash/self-select mode, multiple payout identities, wallet accounting,
dashboard, hot reload, administrative HTTP mutation, or orphan/reorg reopening
of closed rounds. Templates that require miner signature material are
unsupported and fail closed.
