# monero-solo-stratum

`monero-solo-stratum` is a standalone Monero solo-mining Stratum server for
ordinary XMRig `rx/0` simple-mode miners. It obtains templates from one trusted
`monerod`, creates private per-connection jobs, validates shares, journals
network candidates before submission, and stores operational history in
SQLite. It is not a pool proxy: the configured primary address receives any
block reward, and a miner's `user` value is only a worker label.

The default verified mode links the in-process
`monero-stratum-pow-verifier` at the exact pinned submodule revision and computes every
RandomX result independently. A miner-claimed network candidate takes a
separate safety path: after global deduplication, abuse admission, and a
`synchronous=FULL` journal commit, its frozen block is sent to `monerod`
without waiting for the verifier queue. Verification continues in parallel for
share accounting and abuse classification.

## Build

The release build requires a C++20 compiler, CMake 3.16 or newer, OpenSSL 3,
libcurl, POSIX threads, and the platform dynamic-loader library. SQLite,
nlohmann/json, the verifier, and RandomX are included in the source tree. The
optional ZMQ listener loads `libzmq` at runtime; omit `daemon.zmq_address` if it
is unavailable.

Clone with all pinned dependencies:

```sh
git clone --recurse-submodules \
  https://github.com/SeriousPassenger/monero-solo-stratum.git
cd monero-solo-stratum
```

For an existing clone, initialize the same dependency graph with:

```sh
git submodule update --init --recursive
```

GitHub's automatically generated “Source code” ZIP and tarball do not contain
submodule contents and therefore cannot build this project. Use a recursive
Git clone or the deterministic flattened source archive attached to a release.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install into the chosen prefix (the default is `/usr/local`):

```sh
cmake --install build
```

Choose the final prefix at configure time. The systemd unit embeds the
configured full binary and configuration paths, so changing the prefix later
with `cmake --install --prefix ...` can produce a unit that points at the
wrong location. Package builders should configure the target prefix and use
`DESTDIR` only as a staging root, for example:

```sh
cmake -S . -B build-package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-package --parallel
ctest --test-dir build-package --output-on-failure
DESTDIR="$PWD/package-root" cmake --install build-package
```

For a committed release, pass the exact lowercase revision at configure time,
for example `-DMSS_GIT_COMMIT="$(git rev-parse HEAD)"`. The safe default is
forty zeroes, which explicitly identifies an uncommitted source tree instead
of mislabeling local work as its starting snapshot.

The supplied source archive contains dereferenced copies of every dependency;
OpenSSL, libcurl, the compiler, and CMake remain system prerequisites. The
Git repository pins the verifier, nlohmann/json, and SQLite amalgamation as
submodules, with RandomX nested under the verifier. See
[SOURCE_MANIFEST.md](SOURCE_MANIFEST.md) for the exact revisions.

## Configure and run

`config.example.json` is a complete, strict configuration that binds only to
loopback and writes disposable state below `/tmp`. Copy it to a service-owned
location before production use, change the payout address and authentication
values, and use a durable database path.

```sh
cp config.example.json config.json
./build/monero-solo-stratum --check-config --config ./config.json
./build/monero-solo-stratum --config ./config.json
```

The only command-line forms are:

```text
monero-solo-stratum --config PATH
monero-solo-stratum --check-config --config PATH
monero-solo-stratum --version
monero-solo-stratum --help
```

`--check-config` performs static parsing, range, address, endpoint, path, and
hook validation. It does not open the database, allocate RandomX memory, bind
listeners, or contact the daemon. Normal startup additionally checks the
daemon network, validates a full template locally, and prepares the active
RandomX seed before accepting miners.

The install generates a hardened systemd unit with the configured bindir and
sysconfdir and places it in `lib/systemd/system` by default. Override
`MSS_SYSTEMD_UNIT_DIR` when the target distribution uses another unit path.
Create the dedicated account and production directories, copy the installed
example config to `SYSCONFDIR/monero-solo-stratum/config.json`, replace its
disposable `/tmp` state paths with `/var/lib/monero-solo-stratum` and
`/run/monero-solo-stratum`, then enable the unit:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now monero-solo-stratum.service
```

An XMRig pool entry looks like this:

```json
{
  "url": "127.0.0.1:3333",
  "user": "rig-01",
  "pass": "x",
  "algo": "rx/0",
  "keepalive": true,
  "enabled": true
}
```

When `stratum.access_password` is `null` or `""`, any string `pass` is
accepted. Otherwise the match is exact and untrimmed. See
[docs/STRATUM_PROTOCOL.md](docs/STRATUM_PROTOCOL.md) for the wire contract.

The default per-IP ceiling is 128 so rental services may fan many miners in
through a few source addresses. A password-protected listener bypasses only
the pre-login connection-rate bucket; hard connection ceilings and subsequent
request/submit checks still apply. `stratum.submit_workers`, `verifier.workers`,
and `verifier.seed_init_threads` accept `0` for hardware-derived sizing. The
server reserves its configured connection-table/poll capacity up front and
keeps those hot allocations for predictable bare-metal operation.

## Operational interfaces

The optional HTTP service is a versioned, read-only JSON API under `/v1`; it
has no embedded dashboard and no mutation endpoints. The optional Unix-domain
event socket broadcasts committed records as NDJSON. For example:

```sh
curl --fail --silent --show-error http://127.0.0.1:8787/v1/summary
socat - UNIX-CONNECT:/tmp/monero-solo-stratum-events.sock
./scripts/watch-status.sh
```

`watch-status.sh` renders the live 1-minute/5-minute/1-hour hashrate, current
round effort, share/candidate counters, top shares, recent high-difficulty
shares, and persistence pressure. Set `MSS_SNAPSHOT_FILE` to append the raw API
responses as NDJSON during a long test. Installed builds provide the same tool
as `mss-watch-status`. The monitor requires `curl` and `jq`.

## Security and scope

Keep verified mode enabled for any miner you do not fully control. With
`verifier.enabled=false`, trusted mode accepts the miner's claimed PoW hash for
ordinary share credit and reported hashrate. A malicious miner can therefore
forge shares and accounting in trusted mode; `monerod` remains the only block
consensus authority. Trusted mode is intended only for an operator-controlled
network.

The server intentionally has no TLS listener, dashboard, wallet accounting,
pool failover, vardiff, self-select mining, NiceHash nonce-splitting mode, hot
reload, or administrative HTTP mutations. It does provide the narrow legacy
four-byte target encoding required by a NiceHash RandomXMonero client while
preserving ordinary CryptoNote simple mode and all four nonce bytes. Put a
carefully configured TCP/TLS proxy in front if transport encryption is
required; proxy-address trust is not built in. Review
[docs/SECURITY.md](docs/SECURITY.md) before exposing a listener.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Configuration](docs/CONFIGURATION.md)
- [HTTP API and Unix event stream](docs/API.md)
- [Persistence and recovery](docs/PERSISTENCE.md)
- [Stratum protocol](docs/STRATUM_PROTOCOL.md)
- [Security model](docs/SECURITY.md)
- [Native verifier integration](docs/VERIFIER.md)
- [Testing](docs/TESTING.md)
- [Release verification record](docs/TEST_RESULTS.md)
- [Reproducible source release](docs/RELEASING.md)
- [Implementation blueprint](docs/MONERO_SOLO_STRATUM_BLUEPRINT.md)

The project is MIT licensed. Vendored and reference-source provenance is
recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
