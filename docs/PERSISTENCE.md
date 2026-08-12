# Persistence and recovery

The SQLite database is part of the mining safety boundary, not optional
telemetry. A candidate is never sent until its exact frozen block and unique
fingerprint are durable, and an accepted ordinary share is committed before an
`OK` response is queued to the miner.

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
busy timeout aborts startup. A new empty database receives schema version 1 in
one transaction and `PRAGMA user_version=1`. An existing database must contain
`schema_meta('schema_version','1')` and pass `foreign_key_check`. This release
contains only the initial forward schema; an unknown version or a nonempty
foreign database without schema metadata fails rather than being guessed or
downgraded.

The schema source is `src/db/schema.sql`, embedded into the binary at compile
time. It uses SQLite STRICT tables, check constraints, foreign keys, unique
indexes, and conditional transition statements. The synchronous writer API is
serialized through a bounded two-FIFO scheduler around one connection.
Candidate journal/attempt/reconciliation/acceptance and admitted verifier
completion/finalization commands have priority. Each queued command consumes
one exact 512-byte envelope; referenced immutable block/job blobs are not
copied. Ordinary callers are backpressured before the configured priority
reserve, while priority callers may use the full configured item/byte bounds.
The HTTP API has a separate `SQLITE_OPEN_READONLY` WAL connection and prepared,
bounded queries.

## Logical tables

| Area | Tables and durable purpose |
| --- | --- |
| Identity/lifecycle | `server_sessions`, `workers`, `connections` |
| Work | `public_templates`, `private_jobs`, `shares`, `share_hashes`, `duplicate_keys` |
| Blocks | `candidates`, `candidate_attempts`, `candidate_reconciliations`, `candidate_verdicts` |
| Accounting | `rounds`, `hashrate_buckets` |
| Defense/audit | `abuse_events`, `bans`, `ban_abuse_events`, `events` |
| Hooks | `blocknotify_deliveries` |

Binary public/private IDs, entropy, hashes, keys, nonces, addresses, and blobs
are BLOBs. UTC time is signed Unix microseconds. Unsigned values that do not fit
SQLite's signed integer domain—RandomX seed IDs/tickets and difficulty/work
values—are canonical unsigned decimal TEXT. Conversion to hex, decimal JSON,
and RFC 3339 happens at an interface boundary.

`database.retention_days` must be zero in v1. There is no automatic deletion;
historical candidates, attempts, bans, sessions, rounds, and events remain
until an operator performs an offline archival/migration procedure. Back up
the database and its WAL consistently while the process is stopped, or use
SQLite's online backup API from an external tool.

## Uniqueness and idempotency

Private job creation reserves both the 16-byte public job ID and independent
16-byte private entropy with database-lifetime unique constraints. On either
collision the entire pair is discarded and freshly drawn.

The ordinary duplicate key is the exact 48 bytes:

```text
private_job_entropy[16] || PoW_hash[32]
```

`duplicate_keys` stores its first share, claimed/computed role, activity,
height, and a generation token. The in-memory registry is reconstructed from
active rows before opening Stratum. A generation-tagged release cannot retire
a newer reservation of the same bytes. When a logically retired height loses
its final job, verifier, or candidate reference, the registry returns the exact
collected key/generation pairs and the runtime retires those durable active
rows. Historical rows remain queryable. At restart, ordinary keys belonging
only to jobs from the prior process retire immediately; recoverable candidates
hold their height buckets until their terminal outcome.

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

- Job insertion atomically reserves entropy and public job ID.
- Candidate journaling atomically inserts/attaches the unique candidate, links
  the share/verdict, stores frozen bytes, and creates dispatch intent before
  the first network send.
- Share acceptance atomically finalizes status, hashes, credited assigned
  difficulty, and global/connection/worker one-second work buckets.
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
- unexpired bans and active duplicate keys are loaded into memory;
- candidates in `journaled`, `dispatching`, `retry_wait`, or unresolved
  `ambiguous` state are loaded with the original frozen bytes and attempt
  snapshot.

Before a replacement server session is created, startup idempotently marks
every prior open session unclean, closes its open connections with
`process_restarted`, and retires every orphan private job through the normal
typed retirement workflow so deferred candidate evidence is not lost. A crash
during this recovery can safely resume it on the next invocation.

The runtime performs a reconciliation cycle before resuming due dispatch for a
recoverable nonexhausted candidate. Retries always transmit the journaled bytes.
An exhausted ambiguous candidate remains ambiguous; crash recovery never
rewrites uncertainty to rejection.

Server sessions record start/stop, binary public ID, version, verifier commit,
and a clean-shutdown flag. An unclean prior session remains an audit record; a
new process always receives a new public session ID. Events emitted while
recovering a prior candidate belong to the new emitting session, while their
connection/job/share correlation fields continue to name the immutable
originating records.

## Rounds and hashrate

A round closes only when one local candidate receives daemon `OK` or later
positive reconciliation. A height change, remote block, candidate claim,
rejection, or ambiguity does not close it. Closed rounds are not reopened for
later orphan/reorg correction in v1.

Only final `accepted` shares add assigned difficulty to one-second buckets for
global scope (`scope_id=0`), connection, and logical `(login,rigid)` worker.
Verified and claimed sources have separate conflict keys. API windows cover
exactly 60, 300, 600, 3,600, 21,600, and 86,400 seconds and divide by the
nominal window length; stale/low/duplicate/mismatch/infrastructure shares add
zero.

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
