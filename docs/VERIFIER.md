# Native RandomX verifier

Verified mode uses the in-process C library pinned as a submodule at
`third_party/monero-stratum-pow-verifier`, exact commit
`856c015de433a23fe45d88a18dc08c821e50f1cb`, package 0.1.0. That tree includes
RandomX v1.2.2 at
`6c4340ba4561aec9a3611c1aedf9931239777fb3`. The authoritative ABI declaration
is `include/monero_stratum_pow_verifier.h` inside the verifier tree; this
document records the complete public surface and the server adapter mapping.

## Responsibility boundary

MSPV owns bounded asynchronous raw RandomX hashing, exact seed resources,
optional constant-time comparison with a claimed 32-byte hash, copied input,
completion timings, notifier hints, and diagnostics.

The server owns nonce insertion, hashing-blob reconstruction, job/connection
correlation, Monero share/network targets, duplicate/stale rules, block
reconstruction/daemon RPC, persistence/accounting, bans, and miner responses.
MSPV does not decide whether a hash is a share or block.

## Build and link

The top-level build uses:

```cmake
add_subdirectory(third_party/monero-stratum-pow-verifier)
target_link_libraries(monero_solo_core PUBLIC mspv::verifier)
```

The verifier requires CMake 3.16+ and C++17 internally. Its C header is public,
but a pure-C consumer must still link with a C++ driver/runtime. RandomX headers
are private. The source can also be installed and consumed with
`find_package(monero-stratum-pow-verifier 0.1.0 EXACT CONFIG REQUIRED)`; see
`SOURCE_MANIFEST.md`.

Verifier CMake switches include `MSPV_BUILD_TESTS`, `MSPV_BUILD_EXAMPLES`,
`MSPV_BUILD_BENCHMARK`, `MSPV_BUILD_FAST_TESTS` (needs over 2.3 GiB),
`MSPV_ENABLE_TRACE_LOGGING`, and `MSPV_ENABLE_SANITIZERS`. The server forces
tests/examples/benchmark off for the embedded build and supplies its own
adapter tests.

## ABI constants and types

```c
#define MSPV_ABI_VERSION 1u
#define MSPV_HASH_SIZE 32u
#define MSPV_WAIT_FOREVER UINT32_MAX

typedef struct mspv_context mspv_context;
typedef uint64_t mspv_seed_id;
typedef int32_t mspv_status;
```

This is a 0.x static-library ABI. `struct_size` and `abi_version` reject a
mismatch; they are not a promise that independently compiled future 0.x
headers/libraries mix safely. Rebuild the caller against the exact pin.

## Status values

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `MSPV_OK` | Success |
| 1 | `MSPV_INVALID_ARGUMENT` | Null/bad argument or size |
| 2 | `MSPV_INVALID_CONFIG` | Inconsistent/unsupported configuration |
| 3 | `MSPV_NO_MEMORY` | Allocation failed |
| 4 | `MSPV_NOT_RUNNING` | Operation requires a started context |
| 5 | `MSPV_ALREADY_RUNNING` | Repeated start |
| 6 | `MSPV_CLOSED` | Shutdown/drained context |
| 7 | `MSPV_SEED_NOT_FOUND` | Unknown/nonresident seed ID |
| 8 | `MSPV_SEED_NOT_READY` | Seed is not ready/current |
| 9 | `MSPV_SEED_RELEASING` | Seed no longer accepts work |
| 10 | `MSPV_SEED_ACTIVE` | Operation conflicts with current seed |
| 11 | `MSPV_SEED_CAPACITY` | `max_seeds` resident slots exhausted |
| 12 | `MSPV_QUEUE_FULL` | Pending/outstanding/input byte bound reached |
| 13 | `MSPV_TIMEOUT` | Poll/wait deadline expired |
| 14 | `MSPV_CANCELLED` | Work cancelled by shutdown |
| 15 | `MSPV_UNSUPPORTED` | Requested platform feature unavailable |
| 16 | `MSPV_RANDOMX_ERROR` | RandomX resource/hash failure |
| 17 | `MSPV_INTERNAL_ERROR` | Verifier invariant/owned-thread failure |

`mspv_status_string(status)` returns a diagnostic constant string. Never use
the string as a stable machine enum.

## Enumerations

| Type | Values |
| --- | --- |
| `mspv_memory_mode` | `MSPV_MEMORY_LIGHT` (~256 MiB plus worker/seed VM memory), `MSPV_MEMORY_FAST` (~2,080 MiB dataset plus temporary ~256 MiB cache) |
| `mspv_large_page_mode` | `DISABLED`, `TRY`, `REQUIRE` |
| option bits | `MSPV_OPTION_DISABLE_JIT`, `MSPV_OPTION_SECURE_JIT`, `MSPV_OPTION_DISABLE_HARD_AES`; NONE is zero; secure and disabled JIT cannot both be set |
| `mspv_seed_state` | `PREPARING`, `READY`, `CURRENT`, `RELEASING_STATE`, `FAILED` |
| `mspv_result` | `RESULT_OK`, `RESULT_CANCELLED`, `RESULT_FAILED` |
| `mspv_comparison` | `NOT_REQUESTED`, `MATCH`, `MISMATCH` |
| `mspv_shutdown_mode` | `SHUTDOWN_DRAIN`, `SHUTDOWN_CANCEL_PENDING` |
| `mspv_log_level` | `ERROR`, `WARNING`, `INFO`, `DEBUG`, `TRACE` |

A comparison mismatch is a successfully computed hash. The completion's hash
is authoritative; mismatch is not an MSPV infrastructure error.

## Configuration structure

Call `mspv_config_init` before overriding fields.

| Field | Purpose |
| --- | --- |
| `struct_size`, `abi_version` | Exact ABI validation |
| `worker_count` | Private hashing workers/VMs |
| `seed_init_threads` | Parallel FAST dataset construction threads; builds remain serialized by seed |
| `pending_capacity` | Queued, not currently running jobs |
| `max_outstanding` | Accepted jobs until their completions are polled |
| `max_input_size` | Per-job input bytes |
| `max_seed_key_size` | Key-size bound; portable RandomX keys are 1..60 and Monero uses 32 |
| `max_seeds` | Resident preparing/ready/current/releasing resources |
| `max_buffered_input_bytes` | Total copied input bound |
| `memory_mode`, `large_pages`, `options` | RandomX allocation/JIT/AES policy |
| `notify`, `notify_user_data` | Optional event-loop wake hint |
| `log`, `log_user_data`, `log_level` | Optional diagnostic sink |

Server config maps directly: workers, seed init threads, queue/outstanding/input
and resident seed bounds map to same-named native fields; server `fast/light`
maps to native memory mode; `disabled/try/require` maps to large pages; JIT
`disabled` sets DISABLE, `secure` sets SECURE, and `enabled` sets neither;
software AES sets DISABLE_HARD_AES. The server uses a 32-byte seed key.

The server's writer-capacity validation is intentionally cross-component: it
reserves completion/candidate database command capacity before MSPV admission.

## Callback contract

```c
typedef void (*mspv_notify_fn)(void *user_data);
typedef void (*mspv_log_fn)(void *user_data,
                            mspv_log_level level,
                            const char *message);
```

Callbacks may run concurrently on any verifier-owned thread. Storage and user
data must remain alive until shutdown/destroy returns. They return promptly,
must not throw across the C boundary, and must not call any MSPV function using
the same context. Notification is a coalescible hint emitted for an empty-to-
nonempty completion queue, resolved seed preparation, or owned-thread failure;
the receiver drains completions and queries tracked seed state. No callback
occurs after `mspv_shutdown` returns.

The C++ adapter catches accidental C++ callback exceptions, counts them, and
uses a coalescible atomic wake flag, but this is containment rather than a
supported throwing callback model.

## Result structures

`mspv_seed_info` contains seed ID/state/last error, key size, queued/running job
counts, preparation nanoseconds, `memory_uses_large_pages`, and
`all_vms_use_large_pages`.

`mspv_completion` contains result/error/comparison, ticket, durable caller
`user_tag`, exact seed ID, 32-byte computed hash, and queue/hash/total
nanoseconds. Only a completion with `result == MSPV_RESULT_OK` and
`error == MSPV_OK` supplies an authoritative successful hash.

`mspv_stats` contains configured workers; seed/preparing/ready counts;
pending/running/completion/outstanding counts; buffered input bytes; active
seed ID; and lifetime submitted/completed/cancelled/failed counts.

The server adapter extends snapshots with retained-job/tracked-verification
references and release flags. Its completion adds expected ticket/seed and a
correlation enum (`matched`, unknown tag, ticket mismatch, seed mismatch, or
both mismatch). Correlation failure is an infrastructure/invariant failure,
not miner proof.

## Complete function surface

```c
mspv_status mspv_config_init(mspv_config *config);
mspv_status mspv_create(const mspv_config *config, mspv_context **out_context);
mspv_status mspv_start(mspv_context *context);

mspv_status mspv_seed_prepare(mspv_context *context,
                              const void *key, size_t key_size,
                              mspv_seed_id *out_seed_id);
mspv_status mspv_seed_wait_ready(mspv_context *context,
                                 mspv_seed_id seed_id,
                                 uint32_t timeout_ms);
mspv_status mspv_seed_get_info(mspv_context *context,
                               mspv_seed_id seed_id,
                               mspv_seed_info *out_info);
mspv_status mspv_seed_activate(mspv_context *context, mspv_seed_id seed_id);
mspv_status mspv_seed_deactivate(mspv_context *context);
mspv_status mspv_seed_release(mspv_context *context, mspv_seed_id seed_id);
mspv_status mspv_seed_wait_released(mspv_context *context,
                                    mspv_seed_id seed_id,
                                    uint32_t timeout_ms);

mspv_status mspv_hash_submit(mspv_context *context,
                             mspv_seed_id seed_id,
                             const void *input, size_t input_size,
                             uint64_t user_tag, uint64_t *out_ticket);
mspv_status mspv_verify_submit(mspv_context *context,
                               mspv_seed_id seed_id,
                               const void *input, size_t input_size,
                               const uint8_t claimed_hash[32],
                               uint64_t user_tag, uint64_t *out_ticket);
mspv_status mspv_completion_poll(mspv_context *context,
                                 mspv_completion *out_completion,
                                 uint32_t timeout_ms);
mspv_status mspv_get_stats(mspv_context *context, mspv_stats *out_stats);
mspv_status mspv_shutdown(mspv_context *context, mspv_shutdown_mode mode);
void mspv_destroy(mspv_context *context);
const char *mspv_status_string(mspv_status status);
```

Semantics:

- `config_init` writes safe defaults and ABI metadata.
- `create` allocates an inert context; `start` creates preparation controller
  and workers.
- `seed_prepare` asynchronously prepares an arbitrary key and returns a stable
  opaque ID. Repeating a resident key is idempotent. `wait_ready` returns OK
  for READY/CURRENT after PREPARING resolves.
- `seed_activate` marks one ready seed current; the old current becomes READY
  and is not released. Submissions always name an exact ID.
- `seed_deactivate` clears only the designation.
- `seed_release` immediately stops new admission for a noncurrent seed while
  already accepted jobs retain it; `wait_released` waits for destruction.
- `hash_submit` calculates a raw hash. `verify_submit` also compares a copied
  claimed hash and still returns the computed hash on mismatch. Both copy input
  before returning and allocate an outstanding ticket.
- `completion_poll` returns arbitrary completion order; polling is what frees
  the bounded outstanding reservation.
- `shutdown(DRAIN)` finishes queued work. `CANCEL_PENDING` emits cancelled
  completions for queued work; already-running RandomX calls complete normally.
  Both stop admission and join owned threads.
- `destroy` requires exclusive ownership after every other context call and
  callback returned.

All context-taking operations are synchronized and may be called concurrently
except `destroy`. Shutdown waits for concurrent context calls. The caller must
still avoid callback reentry.

## Context and seed lifecycle

```text
create -> inert -> start -> running -> shutdown -> closed -> destroy

prepare -> PREPARING -> READY -> CURRENT -> READY
                         |          |
                         +-> RELEASING -> absent
PREPARING -----------------> FAILED (on resource error)
```

The server prepares the current template's exact seed before issuing its work,
activates that ID, and opportunistically prepares a distinct next seed. It
retains an old seed while any live job or accepted verification references it.
`max_seeds=2` is the minimum; a third future prefetch is deferred rather than
releasing live work.

The adapter keeps seed key storage while preparation is unresolved, tracks
every submission by durable numeric `user_tag` (the share row ID), retains the
seed per accepted native job, verifies completion ticket/seed correlation,
and starts a requested native release only when the seed is noncurrent and all
job/submission references are gone.

## Server submission sequence

```cpp
auto prepared = verifier.prepare_seed(seed_hash);
if (prepared.status != MSPV_OK) { /* fail template closed */ }
if (verifier.wait_seed_ready(prepared.seed.seed_id, timeout) != MSPV_OK) {
    /* do not issue the job */
}
if (verifier.activate_seed(prepared.seed.seed_id) != MSPV_OK) {
    /* do not issue the job */
}

auto submitted = verifier.submit_verify(prepared.seed.seed_id,
                                        exact_nonce_mutated_hashing_blob,
                                        claimed_hash,
                                        durable_share_id);
if (submitted.status == MSPV_QUEUE_FULL) {
    /* release only provisional claimed duplicate; return infrastructure busy */
}
```

A dedicated loop clears/consumes the wake hint, calls the adapter's zero-timeout
drain, looks up `user_tag`, validates correlation, persists the computed hash
and timing, applies duplicate/target/stale precedence, and then sends exactly
one share response if the connection/request route still exists.

Graceful shutdown uses DRAIN; forced shutdown may cancel pending work. After a
successful shutdown, all remaining completions are polled/persisted until the
terminal status is `MSPV_CLOSED`, then the context is destroyed.

## Backpressure and memory

Admission can fail independently on pending queue count, outstanding ticket
count, copied-input byte total, per-input size, seed state/capacity, or server
per-connection pending count. No failure falls back to claimed-hash credit.
Polling completions promptly is required: a completion still consumes one
outstanding reservation until it is polled.

Fast-mode memory is per resident seed dataset, not only active seed. Operators
must budget boundary overlap plus private VM/scratchpad memory and possible
temporary build cache. `large_pages=require` makes allocation failure fatal;
`try` exposes the actual outcome through seed info.

## Publicly unspecified edges

The pinned public contract does not promise immediate copy lifetime for seed
key bytes, every possible status at every state edge, exact failed-seed wait
status, repeated wait-release behavior, ticket wraparound/lifetime uniqueness,
all failure out-parameters, timeout clock choice, restart after shutdown,
implicit live-context destroy behavior, or future 0.x ABI compatibility. It
also deliberately does not define Monero endianness/target rules.

The adapter therefore retains key bytes, initializes outputs, checks result and
error before other completion fields, persists numeric correlations, performs
explicit shutdown/drain, and implements all Monero target logic separately.
