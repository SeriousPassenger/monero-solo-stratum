#!/usr/bin/env bash

set -euo pipefail

export LC_ALL=C
export TZ=UTC
umask 022

readonly DEFAULT_SOURCE_DATE_EPOCH=1786499396
readonly ARCHIVE_ROOT=monero-solo-stratum

die() {
    printf 'make-source-archive: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat >&2 <<'EOF'
Usage: scripts/make-source-archive.sh OUTPUT.tar.gz

Create a deterministic, self-contained source archive. OUTPUT must not already
exist and must be outside the source tree. SOURCE_DATE_EPOCH may be set to the
release epoch; otherwise the starting-snapshot epoch is used.
EOF
    exit 2
}

[[ $# -eq 1 ]] || usage

for command_name in \
    basename chmod cp cut dirname find grep gzip head mkdir mktemp mv \
    python3 realpath rm sha256sum sort stat tar
do
    command -v "$command_name" >/dev/null 2>&1 ||
        die "required command is unavailable: $command_name"
done

case "$(tar --version 2>/dev/null)" in
    *'GNU tar'*) ;;
    *) die 'GNU tar is required for deterministic --sort and owner handling' ;;
esac

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly source_root="$(cd -- "$script_dir/.." && pwd -P)"
readonly output_path="$(realpath -m -- "$1")"
readonly output_dir="$(dirname -- "$output_path")"
readonly output_name="$(basename -- "$output_path")"
readonly source_date_epoch="${SOURCE_DATE_EPOCH:-$DEFAULT_SOURCE_DATE_EPOCH}"

[[ "$output_name" == *.tar.gz ]] || die 'output name must end in .tar.gz'
[[ "$source_date_epoch" =~ ^[0-9]+$ ]] ||
    die 'SOURCE_DATE_EPOCH must contain decimal digits only'
[[ -d "$output_dir" ]] || die "output directory does not exist: $output_dir"
[[ ! -e "$output_path" && ! -L "$output_path" ]] ||
    die "refusing to overwrite existing output: $output_path"
case "$output_path" in
    "$source_root"|"$source_root"/*)
        die 'output must be outside the source tree'
        ;;
esac

readonly -a source_entries=(
    .github
    .gitignore
    .gitmodules
    CMakeLists.txt
    LICENSE
    README.md
    SOURCE_MANIFEST.md
    THIRD_PARTY_NOTICES.md
    cmake
    config.example.json
    docs
    include
    packaging
    scripts
    src
    tests
    third_party
)

for entry in "${source_entries[@]}"; do
    [[ -e "$source_root/$entry" && ! -L "$source_root/$entry" ]] ||
        die "required source entry is missing or is a symbolic link: $entry"
done

# When invoked from a Git checkout, require the audited dependency gitlinks and
# clean submodule worktrees. Extracted release archives intentionally have no
# Git metadata and are validated through SHA256SUMS instead.
if [[ -d "$source_root/.git" || -f "$source_root/.git" ]]; then
    command -v git >/dev/null 2>&1 ||
        die 'required command is unavailable in Git checkout: git'
    readonly -a expected_submodules=(
        '856c015de433a23fe45d88a18dc08c821e50f1cb third_party/monero-stratum-pow-verifier'
        '6c4340ba4561aec9a3611c1aedf9931239777fb3 third_party/monero-stratum-pow-verifier/third_party/RandomX'
        '9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 third_party/nlohmann-json'
        '15d0ff10ebc7e7225eced1de84bb52137000899b third_party/sqlite-amalgamation'
    )
    for expected in "${expected_submodules[@]}"; do
        revision=${expected%% *}
        submodule_path=${expected#* }
        if [[ "$submodule_path" != */third_party/RandomX ]]; then
            gitlink="$(git -C "$source_root" ls-files -s -- "$submodule_path")"
            [[ "$gitlink" == "160000 $revision 0"$'\t'"$submodule_path" ]] ||
                die "superproject gitlink mismatch: $submodule_path"
        fi
        actual="$(git -C "$source_root/$submodule_path" rev-parse HEAD 2>/dev/null || true)"
        [[ "$actual" == "$revision" ]] ||
            die "submodule revision mismatch: $submodule_path (expected $revision, found ${actual:-missing})"
        [[ -z "$(git -C "$source_root/$submodule_path" status --porcelain --untracked-files=all --ignore-submodules=none)" ]] ||
            die "submodule worktree is dirty: $submodule_path"
    done
fi

# Every top-level path must be either an archived source path or an explicitly
# ignored local build/VCS path. This prevents a new source root from being
# silently left out when the project layout changes.
shopt -s dotglob nullglob
for path in "$source_root"/*; do
    name="${path##*/}"
    case "$name" in
        .git|SHA256SUMS|build|build-*|cmake-build-*)
            ;;
        .github|.gitignore|.gitmodules|CMakeLists.txt|LICENSE|README.md|SOURCE_MANIFEST.md|\
        THIRD_PARTY_NOTICES.md|cmake|config.example.json|docs|include|\
        packaging|scripts|src|tests|third_party)
            ;;
        *)
            die "unclassified top-level path (update the release allowlist): $name"
            ;;
    esac
done
shopt -u dotglob nullglob

validate_path() {
    local path=$1
    local relative=${path#"$source_root"/}

    [[ "$relative" != "$path" ]] || die "path escaped the source root: $path"
    [[ "$relative" != *'\'* ]] || die "backslash in source path: $relative"
    [[ ! "$relative" =~ [^[:print:]] ]] ||
        die "non-ASCII or control byte in source path: $relative"
    [[ ! -L "$path" ]] || die "symbolic links are not releasable: $relative"

    if [[ -f "$path" ]]; then
        [[ "$(stat -c '%h' -- "$path")" == 1 ]] ||
            die "hard-linked files are not releasable: $relative"
        [[ ! -u "$path" && ! -g "$path" ]] ||
            die "set-id files are not releasable: $relative"
    elif [[ ! -d "$path" ]]; then
        die "special filesystem object is not releasable: $relative"
    fi
}

validate_release_path() {
    local path=$1
    local relative=${path#"$source_root"/}
    local name=${relative##*/}
    local lower=${name,,}

    case "/$relative/" in
        */.git/*|*/__pycache__/*|*/CMakeFiles/*|*/Testing/*)
            die "forbidden generated directory in source tree: $relative"
            ;;
    esac
    case "$name" in
        .DS_Store|.env|CMakeCache.txt|Thumbs.db|compile_commands.json|\
        id_ed25519|id_rsa)
            die "forbidden generated or sensitive file: $relative"
            ;;
    esac
    case "$lower" in
        core.*|*.core|*.crt|*.db|*.db-shm|*.db-wal|*.key|*.log|*.p12|*.pem|\
        *.pfx|*.pid|*.pyc|*.pyo|*.sock|*.sqlite|*.sqlite-shm|*.sqlite-wal|\
        *.sqlite3|*.sqlite3-shm|*.sqlite3-wal)
            die "forbidden generated or sensitive file: $relative"
            ;;
    esac
    if [[ -f "$path" ]]; then
        case "$(LC_ALL=C head -c 16 -- "$path" 2>/dev/null || true)" in
            $'\177ELF'*) die "compiled ELF payload in source tree: $relative" ;;
            'SQLite format 3'*) die "SQLite database payload in source tree: $relative" ;;
        esac
        if LC_ALL=C grep -aEq -- \
            '-----BEGIN (ENCRYPTED |OPENSSH |RSA |EC )?PRIVATE KEY-----' \
            "$path"
        then
            die "private-key payload in source tree: $relative"
        fi
    fi
}

cd -- "$source_root"
while IFS= read -r -d '' path; do
    validate_path "$source_root/$path"
    validate_release_path "$source_root/$path"
done < <(
    find "${source_entries[@]}" \
        \( -name .git -o -path third_party/nlohmann-json \) -prune -o \
        -print0
)

readonly -a nlohmann_entries=(
    third_party/nlohmann-json/include
    third_party/nlohmann-json/cmake
    third_party/nlohmann-json/CMakeLists.txt
    third_party/nlohmann-json/README.md
    third_party/nlohmann-json/LICENSE.MIT
    third_party/nlohmann-json/LICENSES/Apache-2.0.txt
)
for entry in "${nlohmann_entries[@]}"; do
    [[ -e "$source_root/$entry" && ! -L "$source_root/$entry" ]] ||
        die "required nlohmann/json release entry is missing: $entry"
done
while IFS= read -r -d '' path; do
    validate_path "$source_root/$path"
    validate_release_path "$source_root/$path"
done < <(find "${nlohmann_entries[@]}" -print0)

readonly work_dir="$(mktemp -d "${TMPDIR:-/tmp}/mss-source-archive.XXXXXXXX")"
readonly stage_parent="$work_dir/stage"
readonly stage_root="$stage_parent/$ARCHIVE_ROOT"
readonly tar_path="$work_dir/archive.tar"
temporary_output=''
cleanup() {
    if [[ -n "$temporary_output" && -e "$temporary_output" ]]; then
        rm -f -- "$temporary_output"
    fi
    rm -rf -- "$work_dir"
}
trap cleanup EXIT HUP INT TERM

mkdir -p -- "$stage_root"

# Copy all admitted sources except nlohmann/json and Git administrative data.
# The nlohmann subtree is populated separately from its narrow release list.
tar \
    --exclude='.git' \
    --exclude='*/.git' \
    --exclude='third_party/nlohmann-json' \
    -cf - "${source_entries[@]}" |
    tar -C "$stage_root" -xf -

mkdir -p -- "$stage_root/third_party/nlohmann-json/LICENSES"
cp -a -- third_party/nlohmann-json/include \
    "$stage_root/third_party/nlohmann-json/"
cp -a -- third_party/nlohmann-json/cmake \
    "$stage_root/third_party/nlohmann-json/"
cp -a -- third_party/nlohmann-json/CMakeLists.txt \
    third_party/nlohmann-json/README.md \
    third_party/nlohmann-json/LICENSE.MIT \
    "$stage_root/third_party/nlohmann-json/"
cp -a -- third_party/nlohmann-json/LICENSES/Apache-2.0.txt \
    "$stage_root/third_party/nlohmann-json/LICENSES/"

# Normalize modes independently of the checkout's umask. Only the release
# tooling itself needs an executable bit in the source distribution.
find "$stage_root" -type d -exec chmod 0755 {} +
find "$stage_root" -type f -exec chmod 0644 {} +
chmod 0755 \
    "$stage_root/scripts/make-source-archive.sh" \
    "$stage_root/scripts/test-source-archive.sh" \
    "$stage_root/scripts/verify-source-archive.py"

# SHA256SUMS intentionally excludes itself. Paths are relative to the archive
# root, byte-sorted, printable ASCII, and therefore unambiguous to the bundled
# verifier.
manifest_path="$stage_root/SHA256SUMS"
while IFS= read -r -d '' file; do
    relative=${file#"$stage_root"/}
    digest="$(sha256sum -- "$file")"
    digest=${digest%% *}
    printf '%s  %s\n' "$digest" "$relative"
done < <(find "$stage_root" -type f ! -name SHA256SUMS -print0 | sort -z) \
    > "$manifest_path"
chmod 0644 "$manifest_path"

tar \
    --sort=name \
    --format=gnu \
    --mtime="@$source_date_epoch" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -C "$stage_parent" \
    -cf "$tar_path" \
    "$ARCHIVE_ROOT"

temporary_output="$(mktemp "$output_dir/.${output_name}.tmp.XXXXXXXX")"
gzip -n -9 < "$tar_path" > "$temporary_output"
python3 "$script_dir/verify-source-archive.py" \
    "$temporary_output" "$ARCHIVE_ROOT"
mv -- "$temporary_output" "$output_path"
temporary_output=''

printf 'created %s\n' "$output_path"
printf 'sha256  %s\n' "$(sha256sum -- "$output_path" | cut -d ' ' -f 1)"
printf 'epoch   %s\n' "$source_date_epoch"
