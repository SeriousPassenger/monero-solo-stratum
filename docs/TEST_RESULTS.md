# Release verification record

This record describes the final local verification performed on 2026-08-12.
It is evidence for these source bytes, not a claim about an untested production
host or hostile-Internet capacity.

## Environment

| Item | Value |
| --- | --- |
| Host | Linux x86_64, kernel 6.18.35; 9 available CPUs; 21.9 GiB RAM |
| C/C++ compiler | GCC/G++ 13.3.0 |
| CMake | 3.31.8 |
| glibc | 2.39 |
| OpenSSL | 3.0.13 |
| libcurl | 8.5.0 |
| Starting snapshot | `b1f1e365d7ab344ca5ca7f3334fdfbea5da7f9fd` |
| Native verifier | `856c015de433a23fe45d88a18dc08c821e50f1cb` |
| RandomX | `6c4340ba4561aec9a3611c1aedf9931239777fb3` |
| Final Release binary SHA-256 | `e1f5e32627828c0dffe7f442bae50fddfe192c42414a9ededbd5a574963cd881` |

The recorded validation was performed locally before publication. The same
source content was then prepared as a commit and draft pull request; no
production deployment or release claim is implied by this test record.

## Server builds and tests

### Strict Release

```sh
cmake -S . -B build-release \
  -DMSS_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 3
ctest --test-dir build-release --output-on-failure
```

Result: all 12 server tests passed, 0 failures, in 10.95 seconds. The core
compiled with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
-Wshadow -Wformat=2 -Wundef -Wnull-dereference -Werror`.

The built binary returned `monero-solo-stratum 0.1.0`, and

```sh
monero-solo-stratum --check-config --config ./config.example.json
```

returned `configuration valid` with exit status zero.

### AddressSanitizer and UndefinedBehaviorSanitizer

```sh
cmake -S . -B build-asan -DMSS_BUILD_TESTS=ON \
  -DMSS_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --parallel 3
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-asan --output-on-failure
```

Result: all 12 tests passed, 0 failures, in 74.81 seconds. Leak detection was
disabled because this restricted/ptraced runner does not support reliable
LeakSanitizer execution; this result is ASan/UBSan evidence, not leak evidence.

### ThreadSanitizer

```sh
cmake -S . -B build-tsan -DMSS_BUILD_TESTS=ON \
  -DMSS_ENABLE_TSAN=ON -DMSS_ENABLE_SANITIZERS=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build build-tsan --parallel 3
TSAN_OPTIONS=halt_on_error=1:abort_on_error=1:second_deadlock_stack=1:history_size=7 \
ctest --test-dir build-tsan --output-on-failure
```

Result: all 12 tests passed, 0 failures, in 13.26 seconds.

## Native verifier

The exact pinned verifier submodule was independently configured with its own tests.
Its light-mode native API, C API, and installed-consumer tests all passed: 3/3
in 20.61 seconds. The opt-in 2.08 GiB fast-dataset known-answer test was also
built and passed in 13.01 seconds. This exercises both verifier memory modes at
the pinned RandomX revision.

## Real Monero fakechain/regtest

The automated harness was run against the official Monero v0.18.5.1-release
`monerod` (binary SHA-256
`9b3b2676ea7868c1a7186feea9569c2cf7683ae79d2fcc769c846a91c810a1f5`)
and the exact pinned `mspv_verify` build:

```sh
python3 tests/regtest/run_monero_regtest.py \
  --server ./build-release/monero-solo-stratum \
  --monerod /path/to/monero-v0.18.5.1/monerod \
  --mspv-verify /path/to/mspv_verify
```

Result:

```json
{"candidate_attempts":1,"final_height":2,"initial_height":1,"monerod":"v0.18.5.1-release","network":"fakechain","result":"pass","share":"accepted/verified"}
```

The harness consumed the real daemon template wire shape, including an empty
`next_seed_hash`, derived a private Stratum job, independently computed its
RandomX result, submitted a verified network candidate, observed exactly one
height advance, and required the accepted share/candidate/attempt plus SQLite
integrity, foreign-key, and clean-session checks.

## Rental-service fan-in

The deterministic Stratum regression retained 200 authenticated clients across
three loopback source IPs with a fixed event/worker thread count:

```text
rental_fanin clients=200 sources=3 connect_ms=65 login_ms=54 refresh_ms=2 submit_ms=0 total_ms=145 threads_baseline=1 threads_server=7 threads_loaded=7 threads_stopped=1 rss_baseline_kib=5424 rss_loaded_kib=7956
```

A second end-to-end run used real Monero v0.18.5.1, the full Runtime, and
SQLite for all 200 clients (67/67/66 across three source IPs). It produced:

- 1.854 ms total connection time;
- 1.388 seconds for login plus 200 durable per-client jobs;
- 400 durable queued jobs after one height refresh;
- a 1.087 ms network fan-out span for the refreshed jobs;
- exactly 33 application threads both before and after all 200 clients;
- 3,356 KiB additional RSS and one additional file descriptor per client;
- one independently computed RandomX candidate accepted/verified in 46.054 ms,
  advancing fakechain height 2 to 3;
- zero protocol errors, SQLite integrity/foreign-key success, 200 durable
  connection closures, clean exit status zero, and 63.735 ms shutdown.

This is strong functional fan-in evidence for the multiplexed connection model;
it is not a claim of maximum throughput for another CPU, disk, or difficulty.

## Install and source-package validation

A fresh configure with `-DCMAKE_INSTALL_PREFIX=/usr` and a `DESTDIR` stage
placed the binary at `/usr/bin`, the example at
`/etc/monero-solo-stratum/config.example.json`, and the unit under
`/usr/lib/systemd/system`. The unit's `ExecStartPre`/`ExecStart` refer to
`/usr/bin` and `/etc/monero-solo-stratum/config.json`. Documentation, complete
MIT/Apache/BSD notices, verifier headers/static libraries, and CMake package
files were present. Both supplied installed C and C++ verifier consumers
configured, built, and ran successfully. The installed server has no
RPATH/RUNPATH and has a non-executable GNU stack.

The deterministic archive self-test requires two independently created
archives and one rebuild from verified extracted source to be byte-identical;
it also requires manifest tampering and path traversal to be rejected. The
final external archive SHA-256 is intentionally recorded beside the release
artifact rather than embedded recursively inside it.

## Explicitly deferred evidence

- The live Unix event-stream socket test was skipped because this sandbox
  rejects AF_UNIX creation with `EPERM`; serialization, ordering, admission,
  and backpressure remain covered deterministically.
- The real-daemon tests used the protocol harness rather than a native XMRig
  binary. They nevertheless computed and submitted exact RandomX work through
  the same `rx/0` Stratum interface.
- Live ZMQ notifications, an external lost-response fault proxy, extended
  reorg/process-kill recovery matrices, coverage-guided fuzzing, unrestricted
  LeakSanitizer, and sustained hostile saturation/DDoS/fairness testing remain
  deferred. They are not basic mining-system release gates for this handoff.

The implemented deterministic suite still covers malformed input, bounded
queues, duplicate/candidate ownership, post-commit job rollback, verifier and
Stratum fatal propagation, startup reconciliation, graceful drain, restart,
idempotency, and seed rotation/reorg reactivation.
