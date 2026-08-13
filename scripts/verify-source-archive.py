#!/usr/bin/env python3
"""Fail-closed validation for monero-solo-stratum source archives."""

from __future__ import annotations

import gzip
import hashlib
import pathlib
import re
import stat
import sys
import tarfile


MAX_MEMBERS = 100_000
MAX_TOTAL_FILE_BYTES = 1 << 30
MANIFEST_LINE = re.compile(rb"([0-9a-f]{64})  ([\x20-\x7e]+)\n")
FORBIDDEN_PATH_PARTS = {".git", "__pycache__", "CMakeFiles", "Testing"}
FORBIDDEN_FILE_NAMES = {
    ".DS_Store",
    ".env",
    "CMakeCache.txt",
    "Thumbs.db",
    "compile_commands.json",
    "id_ed25519",
    "id_rsa",
}
FORBIDDEN_FILE_SUFFIXES = {
    ".core",
    ".crt",
    ".db",
    ".key",
    ".log",
    ".p12",
    ".pem",
    ".pfx",
    ".pid",
    ".pyc",
    ".pyo",
    ".sock",
    ".sqlite",
    ".sqlite3",
}


class VerificationError(Exception):
    """An archive violates the release format or integrity contract."""


def fail(message: str) -> None:
    raise VerificationError(message)


def validate_member_name(name: str, expected_root: str) -> str:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError:
        fail(f"non-ASCII archive member: {name!r}")
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        fail(f"control byte in archive member: {name!r}")
    if "\\" in name:
        fail(f"backslash in archive member: {name!r}")

    trimmed = name[:-1] if name.endswith("/") else name
    pure = pathlib.PurePosixPath(trimmed)
    if pure.is_absolute() or not pure.parts:
        fail(f"absolute or empty archive member: {name!r}")
    if any(part in ("", ".", "..") for part in pure.parts):
        fail(f"non-normal archive member: {name!r}")
    if trimmed != "/".join(pure.parts):
        fail(f"non-normal archive member: {name!r}")
    if pure.parts[0] != expected_root:
        fail(f"member outside expected root {expected_root!r}: {name!r}")
    if any(part == ".git" for part in pure.parts):
        fail(f"Git metadata in source archive: {name!r}")
    return "/".join(pure.parts)


def validate_nlohmann_subset(relative: str) -> None:
    prefix = "third_party/nlohmann-json/"
    if not relative.startswith(prefix):
        return
    child = relative[len(prefix) :]
    allowed_files = {
        "CMakeLists.txt",
        "README.md",
        "LICENSE.MIT",
        "LICENSES/Apache-2.0.txt",
    }
    if child in allowed_files:
        return
    if child in ("include", "cmake", "LICENSES"):
        return
    if child.startswith("include/") or child.startswith("cmake/"):
        return
    fail(f"file outside the nlohmann/json release subset: {relative!r}")


def validate_release_path(relative: str) -> None:
    """Reject common build, runtime, credential, and editor artifacts."""
    if not relative:
        return
    pure = pathlib.PurePosixPath(relative)
    if any(part in FORBIDDEN_PATH_PARTS for part in pure.parts):
        fail(f"forbidden generated path in source archive: {relative!r}")
    name = pure.name
    lowered = name.lower()
    if name in FORBIDDEN_FILE_NAMES or lowered.startswith("core."):
        fail(f"forbidden generated or sensitive file: {relative!r}")
    if any(lowered.endswith(suffix) for suffix in FORBIDDEN_FILE_SUFFIXES):
        fail(f"forbidden generated or sensitive file: {relative!r}")
    if lowered.endswith((".db-shm", ".db-wal", ".sqlite-shm", ".sqlite-wal",
                         ".sqlite3-shm", ".sqlite3-wal")):
        fail(f"forbidden SQLite sidecar file: {relative!r}")


def validate_release_payload(relative: str, data: bytes) -> None:
    """Reject compiled executables, databases, and private-key payloads."""
    if data.startswith(b"\x7fELF"):
        fail(f"compiled ELF payload in source archive: {relative!r}")
    if data.startswith(b"SQLite format 3" + bytes((0,))):
        fail(f"SQLite database payload in source archive: {relative!r}")
    private_key_markers = (
        b"-----BEGIN " + b"PRIVATE KEY-----",
        b"-----BEGIN " + b"ENCRYPTED PRIVATE KEY-----",
        b"-----BEGIN " + b"OPENSSH PRIVATE KEY-----",
        b"-----BEGIN " + b"RSA PRIVATE KEY-----",
        b"-----BEGIN " + b"EC PRIVATE KEY-----",
    )
    if any(marker in data for marker in private_key_markers):
        fail(f"private-key payload in source archive: {relative!r}")


def read_regular_file(archive: tarfile.TarFile, member: tarfile.TarInfo) -> bytes:
    stream = archive.extractfile(member)
    if stream is None:
        fail(f"could not read regular member: {member.name!r}")
    data = stream.read()
    if len(data) != member.size:
        fail(f"short read for archive member: {member.name!r}")
    return data


def verify(archive_path: pathlib.Path, expected_root: str) -> tuple[int, int, int]:
    if not archive_path.is_file() or archive_path.is_symlink():
        fail(f"archive is not a regular file: {archive_path}")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", expected_root):
        fail(f"unsafe expected root name: {expected_root!r}")

    with archive_path.open("rb") as stream:
        header = stream.read(10)
    if len(header) != 10 or header[:3] != b"\x1f\x8b\x08":
        fail("archive is not a gzip-compressed tar stream")
    if header[3] != 0:
        fail("gzip -n contract violated: optional gzip header fields are present")
    if header[4:8] != b"\0\0\0\0":
        fail("gzip -n contract violated: gzip timestamp is not zero")

    members: dict[str, tarfile.TarInfo] = {}
    ordered_names: list[str] = []
    file_bytes = 0
    archive_mtime: int | None = None
    root_seen = False

    try:
        with tarfile.open(archive_path, mode="r:gz") as archive:
            for index, member in enumerate(archive, start=1):
                if index > MAX_MEMBERS:
                    fail(f"archive contains more than {MAX_MEMBERS} members")
                normalized = validate_member_name(member.name, expected_root)
                if normalized in members:
                    fail(f"duplicate archive member: {member.name!r}")
                if not (member.isdir() or member.isreg()):
                    fail(f"links and special members are forbidden: {member.name!r}")
                if member.uid != 0 or member.gid != 0:
                    fail(f"nonzero owner in archive member: {member.name!r}")
                if archive_mtime is None:
                    archive_mtime = member.mtime
                elif member.mtime != archive_mtime:
                    fail(f"nonuniform member timestamp: {member.name!r}")

                expected_mode = 0o755 if member.isdir() else 0o644
                relative = normalized[len(expected_root) + 1 :] if normalized != expected_root else ""
                if relative in {
                    "scripts/make-source-archive.sh",
                    "scripts/test-source-archive.sh",
                    "scripts/verify-source-archive.py",
                    "scripts/watch-status.sh",
                    "scripts/watch-status-tui.py",
                }:
                    expected_mode = 0o755
                if stat.S_IMODE(member.mode) != expected_mode:
                    fail(
                        f"unexpected mode {stat.S_IMODE(member.mode):04o} "
                        f"for {member.name!r}"
                    )

                if normalized == expected_root:
                    if not member.isdir():
                        fail("archive root is not a directory")
                    root_seen = True
                elif relative:
                    validate_release_path(relative)
                    validate_nlohmann_subset(relative)

                if member.isreg():
                    file_bytes += member.size
                    if file_bytes > MAX_TOTAL_FILE_BYTES:
                        fail("archive's declared regular-file size exceeds 1 GiB")
                members[normalized] = member
                ordered_names.append(normalized)

            if not root_seen:
                fail("archive root directory is absent")
            if ordered_names != sorted(ordered_names, key=lambda item: item.encode("ascii")):
                fail("archive members are not byte-sorted")

            manifest_name = f"{expected_root}/SHA256SUMS"
            manifest_member = members.get(manifest_name)
            if manifest_member is None or not manifest_member.isreg():
                fail("regular SHA256SUMS manifest is absent")
            manifest = read_regular_file(archive, manifest_member)
            if manifest and not manifest.endswith(b"\n"):
                fail("SHA256SUMS is missing its final newline")

            declared: dict[str, bytes] = {}
            manifest_paths: list[str] = []
            position = 0
            for match in MANIFEST_LINE.finditer(manifest):
                if match.start() != position:
                    fail("malformed SHA256SUMS line")
                position = match.end()
                digest, encoded_path = match.groups()
                path = encoded_path.decode("ascii")
                normalized = validate_member_name(f"{expected_root}/{path}", expected_root)
                if normalized == manifest_name:
                    fail("SHA256SUMS must not list itself")
                if path in declared:
                    fail(f"duplicate SHA256SUMS path: {path!r}")
                declared[path] = digest
                manifest_paths.append(path)
            if position != len(manifest):
                fail("malformed SHA256SUMS line")
            if manifest_paths != sorted(manifest_paths, key=lambda item: item.encode("ascii")):
                fail("SHA256SUMS paths are not byte-sorted")

            actual_files = {
                name[len(expected_root) + 1 :]: member
                for name, member in members.items()
                if member.isreg() and name != manifest_name
            }
            if set(declared) != set(actual_files):
                missing = sorted(set(actual_files) - set(declared))
                extra = sorted(set(declared) - set(actual_files))
                fail(f"SHA256SUMS coverage mismatch; missing={missing}, extra={extra}")
            for path in manifest_paths:
                data = read_regular_file(archive, actual_files[path])
                actual_digest = hashlib.sha256(data).hexdigest().encode("ascii")
                if actual_digest != declared[path]:
                    fail(f"SHA256 mismatch: {path}")
                validate_release_payload(path, data)
    except (gzip.BadGzipFile, tarfile.TarError, EOFError, OSError) as exc:
        fail(f"could not parse archive: {exc}")

    required = {
        ".github/workflows/ci.yml",
        ".gitmodules",
        "CMakeLists.txt",
        "LICENSE",
        "README.md",
        "SOURCE_MANIFEST.md",
        "THIRD_PARTY_NOTICES.md",
        "config.example.json",
        "docs/TESTING.md",
        "docs/images/README.md",
        "include/monero_solo/runtime.hpp",
        "scripts/make-source-archive.sh",
        "scripts/test-source-archive.sh",
        "scripts/verify-source-archive.py",
        "scripts/watch-status.sh",
        "scripts/watch-status-tui.py",
        "src/main.cpp",
        "tests/integration/watch_status_tui_tests.py",
        "src/runtime/runtime.cpp",
        "tests/regtest/run_monero_regtest.py",
        "third_party/monero-stratum-pow-verifier/CMakeLists.txt",
        "third_party/monero-stratum-pow-verifier/.gitmodules",
        "third_party/monero-stratum-pow-verifier/LICENSE",
        "third_party/monero-stratum-pow-verifier/include/monero_stratum_pow_verifier.h",
        "third_party/monero-stratum-pow-verifier/src/monero_stratum_pow_verifier.cpp",
        "third_party/monero-stratum-pow-verifier/third_party/RandomX/CMakeLists.txt",
        "third_party/monero-stratum-pow-verifier/third_party/RandomX/LICENSE",
        "third_party/monero-stratum-pow-verifier/third_party/RandomX/src/randomx.cpp",
        "third_party/nlohmann-json/CMakeLists.txt",
        "third_party/nlohmann-json/README.md",
        "third_party/nlohmann-json/LICENSE.MIT",
        "third_party/nlohmann-json/LICENSES/Apache-2.0.txt",
        "third_party/nlohmann-json/include",
        "third_party/nlohmann-json/cmake",
        "third_party/sqlite-amalgamation/LICENSE",
        "third_party/sqlite-amalgamation/sqlite3.c",
        "third_party/sqlite-amalgamation/sqlite3.h",
    }
    relative_members = {
        name[len(expected_root) + 1 :]
        for name in members
        if name.startswith(f"{expected_root}/")
    }
    absent = sorted(required - relative_members)
    if absent:
        fail(f"required release entries are absent: {absent}")

    assert archive_mtime is not None
    return len(members), len(actual_files) + 1, archive_mtime


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(
            "usage: scripts/verify-source-archive.py ARCHIVE.tar.gz "
            "[EXPECTED_ROOT]",
            file=sys.stderr,
        )
        return 2
    expected_root = argv[2] if len(argv) == 3 else "monero-solo-stratum"
    try:
        member_count, file_count, epoch = verify(pathlib.Path(argv[1]), expected_root)
    except VerificationError as exc:
        print(f"verify-source-archive: {exc}", file=sys.stderr)
        return 1
    print(
        f"verified {argv[1]}: {member_count} members, {file_count} files, "
        f"epoch {epoch}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
