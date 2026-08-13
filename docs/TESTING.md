# Testing

This document describes commands and the intended evidence from each test
class. It does not claim that a command passed on a machine where it was not
run; the release/handoff record should include the actual configure command,
compiler, test output, and sanitizer/regtest results.

The server build prerequisites apply, and a test-enabled configure additionally
requires Bash and `jq` for the installed human-monitor fixture.

## Standard build and tests

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful discovery/repetition commands:

```sh
ctest --test-dir build --show-only
ctest --test-dir build --output-on-failure --repeat until-fail:20
ctest --test-dir build -R 'config|database|protocol' --output-on-failure
```

`MSS_BUILD_TESTS=OFF` omits server tests. CTest is enabled by default.

## Included server test executables

| Test | Coverage in the current source tree |
| --- | --- |
| `config_tests` | Defaults/required keys, strict/duplicate JSON, unknown keys, address and cross-capacity/security rules, no-shell blocknotify tokenizer, and complete example config loading |
| `entropy_tests` | Exact HMAC-DRBG construction vector, independent/reseed behavior, failure atomicity, and timed retry/backoff |
| `logger_tests` | JSONL schema/time/severity/typed-field output, filtering and bounds, opt-in sensitive-entropy gating, mode-0600 append/open safety, stderr/failure containment, and concurrent noninterleaving writes |
| `monero_tests` | Legacy Keccak/address validation, share/network target boundaries, constant-time hash comparison, block/parser/mutation vectors, duplicate and candidate keys |
| `monero_template_tests` | Realistic daemon template parse/regeneration, private reserved-byte mutation, exact nonce/frozen candidate reconstruction |
| `database_tests` | Schema/pragmas, sessions/jobs/shares/duplicates, candidate journaling/attempt/reconciliation state, bounded writer item/byte reserve and priority ordering/stats, durable share-to-round assignment, exact/frozen round effort segments, rounds/hashrate, bans/verdicts/events, strict event-payload validation, hook recovery, orphan lifecycle cleanup, duplicate-capacity reclamation, and restart event attribution |
| `verifier_tests` | Exact server-to-native config mapping, known RandomX answer, tag/ticket/seed correlation, rotation/release, completion draining, cancel and drain shutdown |
| `api_tests` | Bearer/null-empty semantics, GET-only/errors/health, strict filters/cursor binding, sensitive views, persisted SQLite resources/detail shapes, mixed-difficulty hashrate/effort, and bounded top/recent-high share rankings |
| `protocol_tests` | Duplicate registry bounds/lifecycle, defense/ban admission, loopback Stratum login/job/submit/keepalive/framing, ordinary and NiceHash compact-target simple-mode jobs, full nonce preservation, Unix event stream, blocknotify supervisor |
| `http_tests` | Real TCP HTTP framing, empty-body enforcement, transfer-encoding rejection, duplicate headers, and transport-error envelopes |
| `runtime_tests` | In-process mock `monerod`, real Stratum/API sockets, private jobs, trusted share persistence, restart state, candidate/template round-boundary gating, fail-closed template validation, verbose post-commit job/share JSONL correlation, entropy opt-in/out, and configured-secret non-disclosure |
| `rental_fanin_tests` | 200 simultaneous authenticated miners across three loopback source IPs, fixed event/worker thread count, refresh delivery to every miner, bounded concurrent submits, connection reaping, and timing/RSS diagnostics |
| `watch_status_tests` | Stubbed API monitor run, human-readable daemon/hashrate/round/verifier output, configured-cap-safe share requests, and complete non-null NDJSON snapshots |

Tests are standalone C++ executables with nonzero failure exit; no third-party
unit-test framework is required. CTest applies a 180-second default per-test
timeout; the bounded rental fan-in regression has a 120-second override.

## Example configuration validation

The example should be checked both through the parser unit test and the actual
CLI:

```sh
./build/monero-solo-stratum --check-config --config ./config.example.json
```

Expected stdout is `configuration valid` and exit status zero. This performs no
database mutation, entropy allocation, listener bind, or daemon request. Run
from the source directory because the example paths are absolute but the
config filename above is relative.

Also exercise expected failures without printing secrets:

```sh
cp config.example.json /tmp/mss-bad.json
# Deliberately edit one field to an invalid value, then require nonzero status.
if ./build/monero-solo-stratum --check-config --config /tmp/mss-bad.json; then
  echo 'unexpected acceptance' >&2
  exit 1
fi
```

Do not place real secrets in captured CI logs.

## Standalone verifier tests

The top-level build disables the verifier project's own tests, because the
server adapter test links the embedded target. Validate the dependency boundary
separately when producing a release:

```sh
cmake -S third_party/monero-stratum-pow-verifier -B build-verifier \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMSPV_BUILD_TESTS=ON \
  -DMSPV_BUILD_EXAMPLES=ON \
  -DMSPV_BUILD_BENCHMARK=OFF
cmake --build build-verifier --parallel
ctest --test-dir build-verifier --output-on-failure
```

This exercises the light-mode native API, C API, and installed C/C++ consumers.
Fast integration is opt-in and uses more than 2.3 GiB:

```sh
cmake -S third_party/monero-stratum-pow-verifier -B build-verifier-fast \
  -DCMAKE_BUILD_TYPE=Release \
  -DMSPV_BUILD_TESTS=ON \
  -DMSPV_BUILD_FAST_TESTS=ON
cmake --build build-verifier-fast --parallel
ctest --test-dir build-verifier-fast --output-on-failure
```

## Address/undefined sanitizers

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMSS_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-asan --output-on-failure
```

Some restricted/ptraced CI containers cannot run LeakSanitizer. If it fails for
that environmental reason, record the diagnostic, rerun ASan/UBSan with
`ASAN_OPTIONS=detect_leaks=0`, and perform a separate leak-enabled run on an
unrestricted host. Never report the leak-disabled run as leak coverage.

## Thread sanitizer

TSan is mutually exclusive with ASan/UBSan:

```sh
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMSS_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-tsan --output-on-failure
```

Exercise live socket tests repeatedly because verifier callbacks, job refresh,
connection teardown, event publication, and candidate completion cross thread
boundaries.

## Monero regtest validation

The source tree includes a standard-library Python smoke harness for an external
Monero v0.18.5.1 `monerod` and the exact pinned verifier's `mspv_verify`
example. It does not download or bundle either executable. Run it against a
fresh Release server build:

```sh
python3 tests/regtest/run_monero_regtest.py \
  --server ./build/monero-solo-stratum \
  --monerod /path/to/monero-v0.18.5.1/monerod \
  --mspv-verify ./build-verifier/mspv_verify
```

The harness starts `monerod` with `--regtest --offline --fixed-difficulty 1
--keep-fakechain --no-zmq`, requires `get_info.nettype=fakechain`, verifies the
real template wire shape (including an empty `next_seed_hash`), logs in through
Stratum, computes the exact RandomX hash, and submits one verified candidate.
It then requires exactly one height advance, one accepted verified share, one
accepted candidate/attempt, SQLite integrity/foreign-key success, and a clean
server session.

For extended fault/reorg validation beyond that core smoke, cover:

1. Use a checksum-valid mainnet-format primary payout address with
   `network=regtest`; require `get_info.nettype=fakechain`.
2. Start verified light mode and wait until `/v1/health/ready` is 200.
3. Connect XMRig/simple test client, receive a private `rx/0` job, submit a
   known valid share, and confirm one accepted row/credited bucket.
4. Generate a candidate at easy regtest difficulty. Verify that the candidate
   row/frozen bytes exist before the first `submitblock`, daemon `OK` closes
   exactly one round, and hook delivery is created once.
5. Force a lost submit response after daemon receipt. Confirm identical frozen
   bytes on retry and acceptance by positive `get_block` reconciliation.
6. Submit retained older valid work after a higher job is queued to that same
   connection; require `stale`. Same-height earlier work must not be stale.
7. Restart with a nonterminal candidate, active ban, active duplicate, and
   running hook row; verify recovery/idempotency.
8. Exercise a downward reorg/template parent change and ensure latest-sent
   height is replaced rather than treated as maximum-ever.

Use trusted mode only for an explicit negative test showing `provenance=claimed`;
do not treat it as PoW validation coverage.

## Crash-recovery matrix

Use process termination or a daemon fault proxy at each durable boundary:

| Cut point | Required restart result |
| --- | --- |
| Before candidate journal commit | No daemon request and no stranded reservation |
| After journal commit, before request | One recoverable logical candidate with exact bytes |
| During `dispatching`/lost response | Interrupted attempt becomes indeterminate; reconcile before resend |
| After daemon accepts, before local acceptance | Positive reconciliation accepts exactly once |
| After round close, before hook success | Round stays closed; pending/running hook retries at least once |
| During share finalization | No success response unless the accepted/accounting transaction committed |

After each crash, run `PRAGMA integrity_check`, `PRAGMA foreign_key_check`, and
API state assertions. Do not manually mutate the live test database.

## Fuzzing and malformed input

The current tree has deterministic malformed JSON/block/API tests but no
libFuzzer/AFL harness. Before a hostile Internet deployment, add or externally
run coverage-guided fuzzers for:

- Stratum LF framing, duplicate keys, JSON depth, Unicode and request IDs;
- config JSON preflight/strict schema;
- Monero Base58, varints, transaction/block parser, offsets and mutations;
- daemon submit/template/reconciliation JSON and response body limits;
- API percent decoding/query/cursor validation;
- no-shell hook tokenizer.

Use ASan/UBSan builds, bounded corpus/time/memory, and promote every crash or
hang to a deterministic regression fixture.

## Load and backpressure

`rental_fanin_tests` is a functional 200-connection regression, not a capacity
benchmark: it proves one multiplexed listener can retain/authenticate the
connections without per-miner threads, refresh every job, and bound submit
handlers. External saturation testing should still measure peak logins, lines,
submissions, verifier queue latency, API pages, candidate admission, SQLite WAL
growth, and slow-client disconnects. Prove hard bounds at and beyond configured
capacity, especially:

- per-IP/global connections and token buckets;
- per-connection output and pending verification cap;
- MSPV pending/outstanding/input-byte limits;
- duplicate registry global/source capacity;
- candidate per-IP/global inflight counts;
- API connection/rate/response bounds;
- Unix event subscriber pending bytes;
- one active `blocknotify` child.

Record CPU model, memory/huge-page/JIT/AES settings, verifier mode/workers,
share difficulty/rate, daemon latency, compiler, and duration. A benchmark from
another host is not a production capacity guarantee.

## Core release evidence checklist

- Clean recursive configure/build output.
- Complete `ctest --output-on-failure` log.
- Standalone MSPV tests at exact pins.
- ASan/UBSan and TSan results, with any environmental exceptions explicit.
- Example config CLI result.
- Monero regtest version/commands and assertions.
- Functional 200-connection rental fan-in result and thread/RSS diagnostics.
- Archive manifest/hash and confirmation that build directories, databases,
  sockets, and Git metadata are excluded from the distributable.

## Deferred hostile-deployment evidence

The following remains valuable before exposing an untrusted public endpoint,
but is deliberately separate from the basic mining-system release gate:

- external process-kill/fault-proxy crash matrix;
- coverage-guided fuzzing;
- sustained saturation, DDoS, slow-client, and fairness testing;
- unrestricted LeakSanitizer and live ZMQ notification testing.
