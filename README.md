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

Git checkouts derive a human build revision and exact provenance automatically.
The version is SemVer with build metadata, for example `0.2.0+rev.20`, where
`20` is `git rev-list --count HEAD`; the separate 40-hex commit remains the
exact source identity. Tracked local changes are reported as `.dirty` and use
the all-zero commit rather than claiming that the binary matches `HEAD`.
Automatic revisioning requires a full, non-shallow history.

`monero-solo-stratum --version` also reports the UTC binary build time. It
honors `SOURCE_DATE_EPOCH` from the `cmake --build` environment, so release and
distribution builds can remain reproducible. Git-free package builds may set
all provenance inputs explicitly:

```sh
cmake -S . -B build-package \
  -DMSS_GIT_COMMIT=0123456789abcdef0123456789abcdef01234567 \
  -DMSS_BUILD_REVISION=18 \
  -DMSS_BUILD_TIMESTAMP="2026-08-17 00:00:00 UTC"
```

With no Git metadata or explicit overrides, the honest fallback is
`0.2.0+rev.0.unknown` with an all-zero commit.

The exact output is a five-line identity block containing the version, build
time, copyright, MIT license, and canonical GitHub source URL.

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

Configuration schema 2 retains individual shares only when their authoritative
actual difficulty is at least 80 G by default, or when candidate/security
evidence requires durability. Lower-value submission detail belongs in the
debug/trace JSONL; compact database totals and round/hashrate accounting still
cover every accepted result. Public templates and private jobs are live-memory
and trace data, never SQLite rows or blobs. SQLite schema 3 is clean-only and
does not migrate an older database. Same-height jobs intentionally remain
eligible until a higher-height job is queued, the connection closes, or the
process restarts; they are not capped by a history count or TTL. See
`docs/CONFIGURATION.md` for the exact schema-1 config transformation and safe
SQLite-file reset, and `docs/PERSISTENCE.md`.

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
./scripts/watch-status.sh --interval 5
```

`watch-status.sh` is the single entry point for both monitoring styles. In an
interactive terminal it opens a dependency-free Python split-screen monitor:
the compact status pane stays readable on the left while an adaptively sampled
event stream flows on the right. Redirected output, `--once`, `--no-clear`, and
`--plain` retain the lightweight Bash/jq monitor and its stable line-oriented
format unless a split-monitor-only option explicitly selects the Python
renderer. Installed builds provide `mss-watch-status`; the interactive engine
is also installed directly as `mss-watch-status-tui`.

The status pane groups related information instead of displaying an exhaustive
metric table. It includes:

- readiness, network, height, uptime, daemon RPC and ZMQ state;
- current network difficulty and monerod inbound/outbound peer counts;
- connections, workers, verified hashrate, shares, round luck and effort;
- active RandomX seed/verifier state and queue/failure pressure;
- compact CPU, network I/O, RAM, disk-space and disk-I/O load indicators; and
- current-round top and recent-high shares with human-readable difficulty and
  UTC timestamps.

Monero peer data comes from the local, read-only `monerod` RPC endpoint. Pool
state comes from the `/v1` API. Host data is sampled locally from Linux `/proc`
and `statvfs`; disk I/O follows the filesystem device selected by `--disk-path`
when Linux exposes an exact device match, avoiding RAID-member double counts.
No privileged helper or monitoring agent is started. If a source is
unavailable, its field is marked unavailable rather than blocking the rest of
the display.

The `Luck` bar derives cumulative block probability from total round effort as
`100 * (1 - exp(-effort / 100))`. The `Effort` bar shows the current 100%
cycle, resets at each 100% boundary, and advances through ten green-to-red
color tiers through 1000% total effort. Exact total effort remains visible next
to the cycling bar.

Configuration uses command-line arguments:

```sh
./scripts/watch-status.sh
./scripts/watch-status.sh --view both --layout vertical
./scripts/watch-status.sh --reverse --theme tokyo-neon --event-rate 3
./scripts/watch-status.sh --view events --stream-filter high-shares,templates
./scripts/watch-status.sh --ui tty --theme black --view events --once
./scripts/watch-status.sh --event-log /var/lib/monero-solo-stratum/debug.jsonl
./scripts/watch-status.sh --once --color never       # legacy snapshot
./scripts/watch-status.sh --plain --interval 2       # legacy live output
./scripts/watch-status.sh --snapshot-file ./status-snapshots.ndjson
./scripts/watch-status.sh \
  --api-url http://127.0.0.1:8787 \
  --api-token 'optional-bearer-token' \
  --monero-rpc-url http://127.0.0.1:18081
```

The default `--view both` can be changed to `status` or `events`. A
comma-separated `--stream-filter` selects one or more stable stream categories:
`high-shares`, `exceptional-shares`, `templates`, `jobs`, `connections`,
`candidates`, `system`, and `misc`. For example,
`--stream-filter high-shares,templates` keeps accepted high shares and template
events. The categories correspond to the labels in the runtime filter dialog;
they are not regular expressions or arbitrary event-code prefixes.
Layout `auto` chooses side-by-side panes when the terminal is wide enough and
stacked panes otherwise; `vertical` and `horizontal` force the split, and
`--reverse` swaps pane order. The event sampler keeps important errors,
candidate and non-accepted-share events visible while rate-limiting repetitive
accepted shares, templates, jobs and connection traffic toward `--event-rate`
rows per second. It follows truncation and rotation without loading an
unbounded log history.

### Interactive keys and runtime configuration

The most common changes require one keystroke and do not restart the server:

| Key | Action |
| --- | --- |
| `1`, `2`, `3` | Show status only, events only, or both panes |
| `r` / `l` | Reverse pane order / cycle automatic, vertical and horizontal layouts |
| `p` | Pause or resume the event viewport |
| Arrow keys, `PgUp`, `PgDn`, `Home`, `End` | Browse the retained event rows |
| `Space` / `x` | Select or deselect the focused event row |
| `Enter` | Inspect the complete focused event |
| `f` | Edit stream filters |
| `o` | Change refresh/event rates, share floor, view, layout and theme |
| `t` / `F5` | Cycle themes / refresh status immediately |
| `e` | Export selected rows after entering an output filename |
| `?` / `q` | Open key help / quit |

Inside the filter dialog, `Space` toggles the focused category, `A` selects
all, `N` selects none, `Enter` applies the choices, and `Esc` cancels them.

Exports are JSON, omit empty fields, and contain only the explicitly selected
event rows. The in-TUI dialog offers `mss-selected-events.json` as an editable
filename suggestion; the operator confirms the destination and any overwrite.
Bulk serialization and `fsync` run outside the UI thread. Pausing affects the
view, not the server or its event log.

### Themes

Three richer palettes target the full-screen curses UI (`--ui curses`):

- `nerv-asuka`: Evangelion NERV/Asuka-inspired red, orange, black and warning
  accents;
- `tokyo-neon`: Tokyo-night cyan, magenta and retrowave violet;
- `windows-classic`: a nostalgic Windows-classic desktop palette without any
  claim of Windows platform support.

Two restrained palettes are designed for the line-oriented TTY UI
(`--ui tty`) on an attached terminal:

- `black`: a high-contrast black theme with deliberately distinct health,
  warning and selection states;
- `windows-classic-tty`: a reduced-color Windows-classic variant.

`--ui auto` chooses curses for an attached capable terminal and the TTY
renderer otherwise. Redirected output is deliberately uncolored. `--ui plain`
forces unstyled line output, while `--ui tty` enables its selected restrained
palette only when stdout is an attached terminal. `--theme auto` selects a
suitable base for that renderer.
Runtime theme changes are available under `o`. Theme color never changes the
underlying health labels, selection marker, or exported JSON.

Runtime image placeholders (operator-provided captures will replace these):

- `docs/images/watch-status-split.png` — both pane orders and the reorganized
  status metrics;
- `docs/images/watch-status-export.png` — paused stream, multi-row selection
  and the JSON export dialog.

Capture guidance is recorded in [docs/images/README.md](docs/images/README.md).

<!-- After adding the files, replace the list above with:
![Split status and event panes](docs/images/watch-status-split.png)
![Event selection and JSON export](docs/images/watch-status-export.png)
-->

Run `./scripts/watch-status.sh --help` for every startup option. The split TUI
requires Python 3.10 or newer and only its standard library. The legacy monitor
requires Bash, `curl`, and `jq`. Reading a mode-0600 event log still requires
the invoking account to have ordinary filesystem permission; do not weaken log
permissions merely to run the monitor. A remote API token remains supported,
and the operator remains responsible for securing remote API/RPC transport.

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
