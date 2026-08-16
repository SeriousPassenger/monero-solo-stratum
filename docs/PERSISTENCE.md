# Persistence and recovery

The SQLite database is part of the mining safety boundary, not optional
telemetry. A candidate is never sent until its exact frozen block and unique
fingerprint are durable. Ordinary share accounting is compact, batched
telemetry; it is not part of the candidate-submission safety boundary.

## Open and schema policy

The configured path is opened read/write/create with SQLite full-mutex mode.
Every open establishes and verifies:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = <configured milliseconds>;
```

Failure to establish WAL, FULL synchronous mode, foreign keys, or the exact
busy timeout aborts startup. A new empty database receives schema version 3 in
one transaction and `PRAGMA user_version=3`. An existing database must contain
`schema_meta('schema_version','3')` and pass `foreign_key_check`. There is no
v1/v2 migration path: stop the server and deliberately move aside the exact
configured SQLite file and its `-wal`/`-shm` companions before starting this
release. Preserve the configuration file and update it separately using the
schema-1-to-2 checklist in `CONFIGURATION.md`. An unknown version or a nonempty
foreign database without schema metadata fails rather than being guessed,
deleted, or downgraded.

The schema source is `src/db/schema.sql`, embedded into the binary at compile
time. It uses SQLite STRICT tables, check constraints, foreign keys, unique
indexes, and conditional transition statements. The synchronous writer API is
serialized through a bounded two-FIFO scheduler around one connection.
Candidate journal/attempt/reconciliation/acceptance and admitted verifier
completion/finalization commands have priority. Each queued command consumes
one exact 512-byte envelope; referenced immutable candidate blobs are not
copied. Ordinary callers are backpressured before the configured priority
reserve, while priority callers may use the full configured item/byte bounds.
The HTTP API has a separate `SQLITE_OPEN_READONLY` WAL connection and prepared,
bounded queries.

The persistence snapshot distinguishes `pending_accounting_items` (terminal
ordinary contributions buffered for aggregate flush) from
`pending_transient_shares` (structurally admitted live submissions that have not
yet reached a terminal outcome). The latter is process memory, not a durable
row count. Summary pending/total counters include it exactly once.

## Logical tables

| Area | Tables and durable purpose |
| --- | --- |
| Identity/lifecycle | `server_sessions`, `workers`, `connections` |
| Significant work | retained `shares`, retained `share_hashes` |
| Blocks | `candidates`, `candidate_attempts`, `candidate_reconciliations`, `candidate_verdicts` |
| Accounting | `share_totals`, `rounds`, `round_work_segments`, `hashrate_buckets` |
| Defense/audit | `abuse_events`, `bans`, `ban_abuse_events`, `events` |
| Hooks | `blocknotify_deliveries` |

Binary public IDs, hashes, keys, nonces, addresses, and candidate blobs are
BLOBs. UTC time is signed Unix microseconds. Unsigned values that do not fit
SQLite's signed integer domain—RandomX seed IDs/tickets and difficulty/work
values—are canonical unsigned decimal TEXT. Conversion to hex, decimal JSON,
and RFC 3339 happens at an interface boundary.

`shares` is deliberately selective. An individual terminal row is retained
when its authoritative actual difficulty is at least
`database.min_persisted_share_difficulty`, or when it is candidate/security
evidence. The comparison is inclusive. Verified mode selects on the computed
hash, never the miner claim; trusted mode selects on the claim. Candidate and
security evidence cannot be disabled. Sub-threshold detail remains in the
configured debug/trace JSONL, while `share_totals` preserves compact outcome
counts. Historical candidates, attempts, bans, sessions, rounds, retained
shares, and events remain until an operator performs an offline archival
procedure. Back up the database and its WAL consistently while the process is
stopped, or use SQLite's online backup API from an external tool.

Public templates and private jobs are never inserted into SQLite. Every job at
the connection's latest queued height remains eligible and accumulates in live
memory until a strictly higher-height job is completely queued, the connection
closes, or the process restarts. Same-height jobs ignore TTL; TTL applies only
to noncurrent jobs at a different height after a downward reorg. Retained
shares denormalize their public job ID, template generation, height,
difficulties, nonce, status, provenance, timings, and hashes. Candidates carry
their own authoritative `round_id`, non-null originating `connection_id`,
public job ID, template generation, height, peer, frozen block, and block
identities. `first_share_id` is nullable
correlation only; candidate recovery/acceptance never requires a share row.
Routine template, job, connection, and low-value share events are trace-only
and do not enter durable `events`.

Connection and worker identity rows exist while live and remain only while a
retained share, candidate, significant audit event, abuse record, or rolling
hashrate bucket references them. Once a connection is closed and all such
references have aged out or been removed, its row and an orphaned worker row
are pruned. This avoids turning routine reconnect churn into permanent history.

## Uniqueness and idempotency

Private job creation refuses a public job-ID collision against the live
in-memory job set and draws a fresh pair. Private entropy is not checked for
uniqueness. SQLite stores neither jobs nor private entropy and imposes no
lifetime uniqueness promise on either independent 128-bit value.

The ordinary duplicate key is the exact 48 bytes:

```text
private_job_entropy[16] || PoW_hash[32]
```

Ordinary duplicate keys live only in the bounded process-global registry. A
restart invalidates every old private job, so an ordinary replay cannot cross
that boundary. Candidate idempotency is separate and durable: the frozen-block
candidate key remains unique for the database lifetime.

The candidate identity is independent of a claimed result:

```text
SHA256("monero-solo-stratum/candidate/v1\0" || frozen_full_block_blob)
```

`candidates.candidate_key` is unique for the database lifetime. A repeated
claim attaches to the existing logical submission; it cannot create another
retry sequence by rotating a fake result. `max_attempts`, frozen block,
coinbase/miner transaction hash, height, peer, expected block ID, and
uncertainty flag are snapshotted and never rebuilt from a later template.

## Atomic operations

Important writer operations are explicit transactions:

- Candidate journaling atomically inserts/attaches the unique candidate, links
  the share/verdict, stores frozen bytes, and creates dispatch intent before
  the first network send.
- Ordinary share results accumulate compact status/work deltas and flush in
  one transaction at most every configured accounting interval.
- Candidate acceptance first drains the closing round's pending accounting,
  then performs the durable round transition.
- Candidate attempt completion updates the attempt and candidate through a
  conditional state transition. The first daemon `OK` performs the one
  acceptance path.
- Candidate acceptance atomically marks canonical metadata, closes exactly one
  open round, opens the next round, suppresses linked candidate abuse verdicts,
  and creates one pending hook delivery when enabled.
- Candidate terminal rejection makes deferred false-candidate/mismatch
  evidence actionable in the same transaction; candidate acceptance suppresses
  it.
- A ban insertion requires at least one durable evidence link in
  `ban_abuse_events` or rolls back.

The database enforces one open round with a partial unique index and one hook
delivery per candidate. Conditional updates make duplicate RPC responses,
recovery, and concurrent reconciliation idempotent.

## Candidate attempts and uncertainty

Candidate states are:

```text
journaled -> dispatching -> retry_wait -> dispatching
                         -> accepted
                         -> rejected
                         -> ambiguous
ambiguous -> accepted_by_reconciliation
```

The default is four attempts separated by two seconds. Each observation is
stored as `accepted`, `explicit_rejection`, or `indeterminate`. Terminal logic
is intentionally conservative:

```text
any OK                         -> accepted
no OK and any indeterminate    -> ambiguous
all attempts explicit reject   -> rejected
```

HTTP errors, timeouts, malformed/mismatched JSON-RPC, oversized bodies, and
other transport uncertainty are indeterminate. A later explicit rejection
cannot erase the possibility that an earlier request reached the daemon.

Reconciliation uses `get_block` by expected block ID and then by exact height.
Only a non-orphan main-chain block at the candidate height whose independently
parsed blob has the journaled miner transaction hash and exact journaled block
ID is positive. This exact-ID check matters because two nonces from the same
private job share one miner transaction. Not-found, another block at the same
height, malformed evidence, and transport failure are inconclusive—not
rejection. Positive evidence calls the same idempotent
candidate/round acceptance transaction.

## Recovery

On database open:

- an interrupted `dispatching` attempt is completed as an indeterminate
  observation and the candidate becomes recoverable;
- orphaned `blocknotify` rows in `running` return to `pending`;
- an existing open round is reused, or one is created if absent;
- unexpired bans are loaded into memory; the ordinary duplicate registry starts
  empty because no job survives a process restart;
- candidates in `journaled`, `dispatching`, `retry_wait`, or unresolved
  `ambiguous` state are loaded with the original frozen bytes and attempt
  snapshot.

Before a replacement server session is created, startup idempotently marks
every prior open session unclean and closes its open connections with
`process_restarted`. Recoverable candidates already contain every byte and
identifier needed for reconciliation/retry; no private-job row is required. A
crash during this recovery can safely resume on the next invocation.

The runtime performs a reconciliation cycle before resuming due dispatch for a
recoverable nonexhausted candidate. Retries always transmit the journaled bytes.
An exhausted ambiguous candidate remains ambiguous; crash recovery never
rewrites uncertainty to rejection.

Server sessions record start/stop, binary public ID, version, verifier commit,
and a clean-shutdown flag. An unclean prior session remains an audit record; a
new process always receives a new public session ID. Events emitted while
recovering a prior candidate belong to the new emitting session, while their
connection, public-job, template-generation, retained-share, and candidate
correlation fields continue to name the immutable originating context.

## Rounds and hashrate

A round closes only when one local candidate receives daemon `OK` or later
positive reconciliation. A height change, remote block, candidate claim,
rejection, or ambiguity does not close it. Each in-flight result retains its
round assignment in memory, so a verification completing across the close
boundary is credited to its original round. Candidate acceptance uses a flush
barrier before freezing the closing round's work segments. Closed rounds are
not reopened for later orphan/reorg correction in v3.

Only final `accepted` shares add assigned difficulty to one-second buckets for
global scope (`scope_id=0`), connection, and logical `(login,rigid)` worker,
whether or not the individual share row is retained.
Verified and claimed sources have separate conflict keys. API windows cover
exactly 60, 300, 600, 3,600, 21,600, and 86,400 seconds and divide by the
nominal window length; stale/low/duplicate/mismatch/infrastructure shares add
zero. The same credited work is accumulated exactly by round, source, and
network difficulty. `estimated_hashes` is this unbiased share-based work
estimate; a Stratum server cannot observe the miner's exact attempted nonce
count. Round effort is `100 * sum(credited_work / network_difficulty)` and may
legitimately exceed 100 percent.

Ordinary accounting batches flush no later than
`database.accounting_flush_interval_ms` and on clean shutdown. An unclean exit
may lose at most one interval of ordinary aggregate telemetry. Candidate
journals, candidate transitions, and security evidence remain synchronous and
recoverable; they are never deferred into the ordinary batch.

## Durable `blocknotify`

Candidate acceptance creates at most one delivery keyed by candidate. The
supervisor atomically claims `pending`/due `retry_wait` as `running`, launches
one no-shell child, and records exit/signal/timeout plus at most the bounded
stderr excerpt. Exit zero marks `delivered`; all other outcomes enter
`retry_wait` with delays 1, 5, 30, 120, 600, then 3,600 seconds indefinitely.

Delivery is at-least-once. A crash after the external side effect but before
the success commit may repeat it; hook programs must deduplicate using the
provided miner transaction hash. Hook failure cannot alter an accepted block
or closed round.

## Operational cautions

- Do not edit an active database with a general SQLite client.
- Keep the database on reliable local storage; network filesystems may not
  honor SQLite locking/fsync semantics.
- Ensure enough disk for unlimited history and monitor both database and WAL
  file sizes through `/v1/persistence`.
- Never copy only the main `.sqlite3` file while a WAL writer is active.
- Manual ban removal or archival is an offline operator action; API v1 is
  deliberately read-only.
