# Reproducible source release

## Version and provenance

Release versions follow SemVer. While the public contract remains pre-1.0,
bug fixes increment PATCH and incompatible configuration, protocol,
persistence, or API changes increment MINOR. This clean persistence redesign
is therefore `0.2.0`. After 1.0, incompatible public-contract changes increment
MAJOR.

A Git build appends the automatically derived full-history counter as SemVer
build metadata (`0.2.0+rev.N`). The 40-hex commit remains a separate exact
provenance field rather than serving as the human version. Release CI must use
a full checkout (`fetch-depth: 0`). Tag the reviewed merged commit, not a PR
head, with an annotated or signed `v0.2.0` tag.

The embedded UTC build timestamp is derived at binary build time and honors
`SOURCE_DATE_EPOCH` exported for each `cmake --build` invocation. Record that
epoch with published artifacts. Git-free builds must pass `MSS_GIT_COMMIT`,
`MSS_BUILD_REVISION`, and `MSS_BUILD_TIMESTAMP` explicitly when exact
provenance is required.

Release archives are produced from a frozen source tree with the checked-in
tools under `scripts/`. They require Bash, GNU tar, gzip, GNU coreutils and
findutils, and Python 3.10 or newer. The application build remains completely
offline. Python verifies the archive and runs the optional installed
split-screen monitor; it is not linked into the server process.

## Freeze and create

Stop source-generating processes and finish all intended edits before staging.
The archive tool reads the working tree rather than Git's index so that the
exact reviewed local source is packaged. It refuses to overwrite an output,
write inside the source tree, follow symbolic links, archive hard links or
special files, or silently omit an unclassified top-level path.

From a Git checkout, first initialize the complete dependency graph:

```sh
git submodule update --init --recursive
git submodule status --recursive
```

The archive builder then requires all four recorded revisions (including
nested RandomX), the three exact superproject gitlinks, and clean submodule
worktrees. GitHub's automatic source ZIP/tarball omits submodule contents; the
deterministic flattened archive produced below is the supported release asset.

Create and verify an archive outside the source tree:

```sh
export SOURCE_DATE_EPOCH=1786499396
scripts/make-source-archive.sh \
  /tmp/monero-solo-stratum-source.tar.gz
scripts/verify-source-archive.py \
  /tmp/monero-solo-stratum-source.tar.gz
sha256sum /tmp/monero-solo-stratum-source.tar.gz
```

The default epoch is the starting-snapshot commit timestamp. An official
release may choose another stable epoch, but it must record that value with
the external archive digest. The builder uses a fixed
`monero-solo-stratum/` archive root, byte-sorted GNU tar members, numeric
owner/group zero, normalized modes, one fixed member timestamp, and
`gzip -n -9`.

Every regular file except `SHA256SUMS` is listed in that internal manifest.
The bundled verifier checks the gzip header, every member path/type/owner/mode
and timestamp, the exact nlohmann/json release subset, complete manifest
coverage, and each SHA-256 digest before an operator extracts the archive.

## Reproducibility check

Run the self-test on the frozen tree:

```sh
scripts/test-source-archive.sh
```

It creates two archives in a private temporary directory with the same epoch,
verifies them independently, rebuilds a third archive from verified extracted
source, and requires byte-for-byte equality for all three. The extracted
tree's old `SHA256SUMS` is ignored and regenerated. A source edit during those
builds is expected to make the check fail; freeze and repeat rather than
accepting mismatched output.

The release subset of `third_party/nlohmann-json` is intentionally limited to:

- `include/`
- `cmake/`
- `CMakeLists.txt`
- `README.md`
- `LICENSE.MIT`
- `LICENSES/Apache-2.0.txt`

This removes upstream tests, fuzz corpora, tools, and their unrelated
third-party payloads while retaining everything used by this project's CMake
build and all applicable license material.

## Build and staged install

Configure the intended final prefix before building. The generated systemd
unit contains full binary and configuration paths evaluated during configure,
so an install-time `--prefix` must not be used to change that decision.

For a normal `/usr/local` install:

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
cmake --install build-release
```

For a distribution package rooted at `/usr`, stage with `DESTDIR`:

```sh
cmake -S . -B build-package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-package --parallel
ctest --test-dir build-package --output-on-failure
DESTDIR="$PWD/package-root" cmake --install build-package
```

Inspect the staged service unit and require its `ExecStartPre` and `ExecStart`
paths to match the configured prefix before publishing the package. With the
`/usr` example above, `config.example.json` stages below
`package-root/etc/monero-solo-stratum/`; copy and edit it as `config.json`
before enabling the service, whose unit refers to
`/etc/monero-solo-stratum/config.json`.

The staged command set must also contain both executable monitor entry points:

```sh
test -x package-root/usr/bin/mss-watch-status
test -x package-root/usr/bin/mss-watch-status-tui
python3 -c 'import ast,pathlib; ast.parse(pathlib.Path("package-root/usr/bin/mss-watch-status-tui").read_text())'
package-root/usr/bin/mss-watch-status --help >/dev/null
package-root/usr/bin/mss-watch-status-tui --help >/dev/null
```

The flattened source archive treats `scripts/watch-status.sh`,
`scripts/watch-status-tui.py`, `scripts/make-source-archive.sh`,
`scripts/test-source-archive.sh`, and `scripts/verify-source-archive.py` as its
only executable script members. The archive verifier requires both TUI source
and its integration test, so an older release allowlist cannot silently emit a
server archive without the installed monitor it documents.
