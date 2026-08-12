# Reproducible source release

Release archives are produced from a frozen source tree with the checked-in
tools under `scripts/`. They require Bash, GNU tar, gzip, GNU coreutils and
findutils, and Python 3.10 or newer. The application build remains completely
offline; Python is only a release-verification dependency.

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
