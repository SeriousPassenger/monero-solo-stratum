# Source manifest and bootstrap

The Git repository records three direct dependency submodules. The verifier
records RandomX as its nested submodule. A release archive dereferences this
graph into a self-contained tree that does not require Git metadata or a
network fetch. The following revisions identify the audited provenance.

| Component | Included revision/version | Role |
| --- | --- | --- |
| `monero-solo-stratum` repository base | `b1f1e365d7ab344ca5ca7f3334fdfbea5da7f9fd` | Initial MIT repository snapshot on which this complete implementation was built |
| `SeriousPassenger/monero-stratum-pow-verifier` | `856c015de433a23fe45d88a18dc08c821e50f1cb`, package 0.1.0 | Direct submodule; MIT in-process verifier |
| `tevador/RandomX` | `6c4340ba4561aec9a3611c1aedf9931239777fb3`, tag v1.2.2 | Nested verifier submodule; BSD-3-Clause engine |
| `nlohmann/json` | `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`, tag v3.11.3 | Direct submodule; MIT JSON library |
| `azadkuh/sqlite-amalgamation` | `15d0ff10ebc7e7225eced1de84bb52137000899b` | Direct submodule; amalgamation wrapper snapshot |
| SQLite amalgamation payload | SQLite 3.38.2, source ID `d33c709cc0af66bc5b6dc6216eba9f1f0b40960b9ae83694c986fbf4c1d6f08f` | Vendored public-domain database engine |

Two additional trees are design/consensus references and are not included or
linked:

- Monero v0.18.5.1, commit
  `4f92268d7c16741cfb41e5bbe2aa46cc260a9ea5` (BSD-3-Clause), used as the
  consensus oracle for address, parsing, block-hash, and target test vectors.
- `SeriousPassenger/xmrig-proxy` branch `improvised-daemon-mining`, commit
  `fe6977291b5bea14e88579e867987e759c96d584` (GPLv3), used only as an
  externally observable behavioral reference. No GPL source is included in
  the MIT implementation.

## Build the recursive archive

Install a C++20 compiler, CMake 3.16+, OpenSSL 3 development files, and libcurl
development files. On Debian-family systems, a typical dependency command is:

```sh
sudo apt-get install build-essential cmake libssl-dev libcurl4-openssl-dev
```

Then build without any source download:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`libzmq` is optional and discovered dynamically at runtime. Install a compatible
ZeroMQ shared library only when `daemon.zmq_address` is nonempty. Polling remains
active with or without ZMQ.

## Clone from Git

Clone the repository and its complete pinned dependency graph with:

```sh
git clone --recurse-submodules \
  https://github.com/SeriousPassenger/monero-solo-stratum.git
cd monero-solo-stratum
git submodule status --recursive
```

For an existing non-recursive clone:

```sh
git submodule update --init --recursive
```

The complete archive dereferences nlohmann/json, SQLite, the verifier, and
RandomX so archive consumers do not need Git or network access. Replacing a
pinned dependency with a system package is a new supply-chain decision: rerun
all tests and record the exact replacement in the release manifest.

The release archive carries the build-required nlohmann/json subset only:
`include/`, `cmake/`, `CMakeLists.txt`, `README.md`, and `LICENSE.MIT` from the
exact revision above, plus `LICENSES/Apache-2.0.txt` for the Abseil-derived
header content. Upstream tests, fuzz fixtures, tools, and unrelated auxiliary
licenses are deliberately excluded because they are neither built nor needed
to reproduce this program.

## Reproducible release staging

The checked-in release tool creates the narrow nlohmann/json subset, excludes
Git metadata and local build trees, normalizes owner/mode/time metadata, and
writes a byte-sorted `SHA256SUMS` covering every regular archive file other
than the manifest itself:

```sh
scripts/make-source-archive.sh \
  /tmp/monero-solo-stratum-source.tar.gz
scripts/verify-source-archive.py \
  /tmp/monero-solo-stratum-source.tar.gz
```

`SOURCE_DATE_EPOCH` selects the member timestamp. Its default,
`1786499396`, is the starting-snapshot commit timestamp. Set and record a
different stable epoch when the release policy requires one. `gzip -n` fixes
the gzip header timestamp and filename independently of the output name.

The verifier inspects paths and member types before reading contents, rejects
absolute/traversal/link/special/duplicate members, checks the fixed
owner/mode/time contract, enforces the nlohmann/json allowlist, and verifies
the internal hashes. The self-test creates two independent archives, rebuilds
once more from a verified extracted archive, and requires byte identity:

```sh
scripts/test-source-archive.sh
```

Run release staging only after the source tree is frozen; it is deliberately
fail-closed on an unclassified new top-level path. See
`docs/RELEASING.md` for the complete release and install-prefix procedure.

## Static versus separate verifier builds

The default top-level build compiles the verifier and RandomX as static targets
through `add_subdirectory` and links `mspv::verifier` into the server. To build
and install the verifier separately:

```sh
cmake -S third_party/monero-stratum-pow-verifier -B build-verifier \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-verifier --parallel
ctest --test-dir build-verifier --output-on-failure
cmake --install build-verifier --prefix /opt/mspv-0.1.0
```

A separate consumer can then use:

```cmake
find_package(monero-stratum-pow-verifier 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE mspv::verifier)
```

The supplied server CMake intentionally uses its pinned submodule target. Switching it
to `find_package` requires a small local CMake change; the C API and link target
remain the same. See `docs/VERIFIER.md` for ABI and lifecycle constraints.
