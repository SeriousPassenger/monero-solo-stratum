#!/usr/bin/env bash

set -euo pipefail

export LC_ALL=C
export TZ=UTC

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly work_dir="$(mktemp -d "${TMPDIR:-/tmp}/mss-archive-test.XXXXXXXX")"
cleanup() {
    rm -rf -- "$work_dir"
}
trap cleanup EXIT HUP INT TERM

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1786499396}"
"$script_dir/make-source-archive.sh" "$work_dir/first.tar.gz"
"$script_dir/make-source-archive.sh" "$work_dir/second.tar.gz"

cmp "$work_dir/first.tar.gz" "$work_dir/second.tar.gz"
python3 "$script_dir/verify-source-archive.py" "$work_dir/first.tar.gz"

# A verified, extracted release must also reproduce its own archive. Its old
# SHA256SUMS is an input artifact that the builder ignores and regenerates.
mkdir "$work_dir/extracted"
tar -C "$work_dir/extracted" -xzf "$work_dir/first.tar.gz"
"$work_dir/extracted/monero-solo-stratum/scripts/make-source-archive.sh" \
    "$work_dir/from-extracted.tar.gz"
cmp "$work_dir/first.tar.gz" "$work_dir/from-extracted.tar.gz"

# Change regular-file payload bytes without updating SHA256SUMS. Tar header
# checksums remain valid because payload bytes are not covered by them.
python3 - "$work_dir/first.tar.gz" "$work_dir/tampered.tar.gz" <<'PY'
import gzip
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
with gzip.open(source, "rb") as stream:
    payload = bytearray(stream.read())
needle = b"# monero-solo-stratum\n"
offset = payload.find(needle)
if offset < 0:
    raise SystemExit("could not locate tamper marker")
payload[offset + 2] ^= 1
with output.open("wb") as raw:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as stream:
        stream.write(payload)
PY
if python3 "$script_dir/verify-source-archive.py" \
    "$work_dir/tampered.tar.gz" >/dev/null 2>&1
then
    printf 'tampered archive was accepted\n' >&2
    exit 1
fi

# Construct an otherwise ordinary gzip/tar stream with a traversal member.
# Validation must fail before any extraction or manifest processing.
python3 - "$work_dir/unsafe.tar.gz" "$SOURCE_DATE_EPOCH" <<'PY'
import gzip
import io
import pathlib
import sys
import tarfile

output = pathlib.Path(sys.argv[1])
epoch = int(sys.argv[2])
with output.open("wb") as raw:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w", format=tarfile.GNU_FORMAT) as archive:
            root = tarfile.TarInfo("monero-solo-stratum")
            root.type = tarfile.DIRTYPE
            root.mode = 0o755
            root.mtime = epoch
            archive.addfile(root)
            bad = tarfile.TarInfo("monero-solo-stratum/../escape")
            bad.mode = 0o644
            bad.mtime = epoch
            bad.size = 1
            archive.addfile(bad, io.BytesIO(b"x"))
PY
if python3 "$script_dir/verify-source-archive.py" \
    "$work_dir/unsafe.tar.gz" >/dev/null 2>&1
then
    printf 'unsafe traversal archive was accepted\n' >&2
    exit 1
fi

printf 'byte-identical archive rebuild: %s\n' \
    "$(sha256sum "$work_dir/first.tar.gz" | cut -d ' ' -f 1)"
printf 'byte-identical rebuild from extracted source: passed\n'
printf 'tamper and traversal rejection: passed\n'
