#!/usr/bin/env bash

set -euo pipefail

readonly watcher="${1:?watch-status path is required}"
readonly python3="${2:-}"
readonly tui_test="${3:-}"
readonly tui_program="${4:-}"
readonly fixture_dir="$(mktemp -d)"
trap 'rm -rf -- "$fixture_dir"' EXIT
readonly snapshot="$fixture_dir/snapshots.jsonl"
readonly output="$fixture_dir/output.txt"
readonly color_output="$fixture_dir/color-output.txt"
readonly curl_log="$fixture_dir/curl.log"

curl() {
    local url="${!#}"
    local request="$*"
    local fail_on_http=false
    local write_http_status=false
    local status=200
    local body=""
    printf '%s\n' "${request//$'\n'/\\n}" >>"$WATCH_CURL_LOG"

    for argument in "$@"; do
        case "$argument" in
            --fail) fail_on_http=true ;;
            *'%{http_code}'*) write_http_status=true ;;
        esac
    done

    [[ "$url" == http://fixture.invalid/* ]] || return 98
    [[ "$url" != *'limit=5'* ]] || return 99

    if [[ "${WATCH_EXPECT_TOKEN:-false}" == true ]]; then
        [[ " $* " == *' -H Authorization: Bearer fixture-token '* ]] ||
            return 97
    fi

    case "$url" in
        */v1/summary)
            body="{\"data\":{\"server\":{\"network\":\"regtest\",\"uptime_seconds\":90061},\"daemon\":{\"height\":2,\"rpc\":\"healthy\",\"zmq\":\"healthy\",\"template_id\":\"9\",\"template_generation\":\"4\"},\"connections\":{\"active\":3},\"workers\":{\"active\":3},\"hashrate\":{\"1m\":\"10800\",\"5m\":\"9000\",\"1h\":\"6000\",\"source\":\"verified\"},\"shares\":{\"accepted\":\"4\",\"total\":\"5\",\"stale\":\"1\",\"duplicate\":\"0\",\"low_difficulty\":\"0\",\"invalid_result\":\"0\"},\"round\":{\"id\":\"2\",\"state\":\"open\",\"accepted_share_count\":\"4\",\"estimated_hashes\":\"1048640\",\"effort\":{\"value\":\"${WATCH_EFFORT:-12.345678}\"}},\"candidates\":{\"accepted\":\"1\",\"total\":\"1\",\"active\":\"0\",\"ambiguous\":\"0\"}}}"
            ;;
        */v1/health/ready)
            status="${WATCH_READY_HTTP_STATUS:-200}"
            body="{\"data\":{\"ready\":${WATCH_READY_VALUE:-true}}}"
            ;;
        */v1/verifier)
            body='{"data":{"enabled":true,"provenance":"verified","configuration":{"memory_mode":"fast"},"stats":{"outstanding":"0","pending":0,"failed":"0"}}}'
            ;;
        */v1/persistence)
            body='{"data":{"database_bytes":4096,"wal_bytes":512,"writer_queue_items":0,"unresolved_candidates":"0"}}'
            ;;
        *'/v1/shares/top?round_id=2')
            body='{"data":[{"id":"5","actual_difficulty":"90000000000","worker_id":"2","round_id":"2","completed_at":"2026-08-13T09:23:40.123456Z"}]}'
            ;;
        */v1/shares/top*)
            return 96
            ;;
        */v1/shares/recent-high)
            body='{"data":[{"id":"4","actual_difficulty":"20000000000","worker_id":"1","round_id":"1","completed_at":"2026-08-13T08:01:02.000000Z"}]}'
            ;;
        *) return 95 ;;
    esac

    printf '%s\n' "$body"
    $write_http_status && printf '%s\n' "$status"
    if $fail_on_http && ((status >= 400)); then
        return 22
    fi
}
export -f curl
export WATCH_CURL_LOG="$curl_log"
export WATCH_EXPECT_TOKEN=true

MSS_API_URL=http://poison.invalid \
MSS_API_TOKEN=poison-token \
MSS_INTERVAL_SECONDS=0 \
MSS_SNAPSHOT_FILE=/dev/null \
    "$watcher" \
        --api-url http://fixture.invalid \
        --api-token fixture-token \
        --interval 7 \
        --snapshot-file "$snapshot" \
        --color never \
        --bar-width 20 \
        --once \
        --no-clear \
        >"$output"

grep -F 'regtest  height 2  uptime 1d 1h 1m 1s' "$output" >/dev/null
grep -F 'Node      RPC healthy  ZMQ healthy  template #9 / gen 4' "$output" >/dev/null
grep -F 'Hashrate  1m 10.8 kH/s  5m 9.00 kH/s  1h 6.00 kH/s  [verified]' "$output" >/dev/null
grep -F 'Luck      [##------------------]  11.614016% block chance' "$output" >/dev/null
grep -F 'Effort    [##------------------]  12.345678% cycle  12.345678% total  tier 1/10' "$output" >/dev/null
grep -F 'Work      1.05 MH estimated  4 accepted shares' "$output" >/dev/null
grep -F 'Storage   4.00 KiB DB + 512 B WAL' "$output" >/dev/null
grep -F 'CURRENT ROUND TOP SHARES  round #2' "$output" >/dev/null
grep -F '1. 90.0 G  2026-08-13 09:23:40 UTC  share #5  worker #2' "$output" >/dev/null
grep -F '1. 20.0 G  2026-08-13 08:01:02 UTC  share #4  worker #1' "$output" >/dev/null

if LC_ALL=C grep -q $'\033' "$output"; then
    printf 'watch-status test: --color never emitted ANSI escapes\n' >&2
    exit 1
fi
if grep -Fq 'fixture-token' "$output" || grep -Fq 'fixture-token' "$snapshot"; then
    printf 'watch-status test: API token leaked into output or snapshot\n' >&2
    exit 1
fi

[[ "$(wc -l <"$curl_log")" -eq 6 ]]
grep -F 'http://fixture.invalid/v1/shares/top?round_id=2' "$curl_log" >/dev/null

jq -e '
    .current_round_id == "2" and
    .summary.data.round.effort.value == "12.345678" and
    .ready.data.ready == true and
    .verifier.data.provenance == "verified" and
    (.persistence.data | type) == "object" and
    .top_shares.data[0].completed_at == "2026-08-13T09:23:40.123456Z" and
    (.recent_high_shares.data | type) == "array"
' "$snapshot" >/dev/null

: >"$curl_log"
WATCH_READY_HTTP_STATUS=503 WATCH_READY_VALUE=false WATCH_EXPECT_TOKEN=false \
    "$watcher" \
        --api-url http://fixture.invalid \
        --color never \
        --once \
        --no-clear \
        >"$output"
grep -F 'NOT READY' "$output" >/dev/null
ready_request="$(grep -F '/v1/health/ready' "$curl_log")"
[[ "$ready_request" == *'--write-out'* ]]
[[ "$ready_request" != *'--fail'* ]]

: >"$curl_log"
WATCH_EXPECT_TOKEN=false \
    "$watcher" \
        --api-url=http://fixture.invalid/ \
        --interval=5 \
        --color=always \
        --bar-width=10 \
        --once \
        --no-clear \
        >"$color_output"

LC_ALL=C grep -Fq $'\033[' "$color_output"
grep -F $'\033[38;5;' "$color_output" >/dev/null

: >"$curl_log"
WATCH_EXPECT_TOKEN=false \
    "$watcher" --api-url http://fixture.invalid --color auto \
        --bar-width 10 --once --no-clear >"$output"
if LC_ALL=C grep -q $'\033' "$output"; then
    printf 'watch-status test: redirected auto color emitted ANSI escapes\n' >&2
    exit 1
fi

: >"$curl_log"
WATCH_EFFORT=100 WATCH_EXPECT_TOKEN=false \
    "$watcher" --api-url http://fixture.invalid --color never \
        --bar-width 10 --once --no-clear >"$output"
grep -F 'Luck      [######----]  63.212056% block chance' "$output" >/dev/null
grep -F 'Effort    [----------]  0.000000% cycle  100.000000% total  tier 2/10' "$output" >/dev/null

: >"$curl_log"
WATCH_EFFORT=250 WATCH_EXPECT_TOKEN=false \
    "$watcher" --api-url http://fixture.invalid --color never \
        --bar-width 10 --once --no-clear >"$output"
grep -F 'Effort    [#####-----]  50.000000% cycle  250.000000% total  tier 3/10' "$output" >/dev/null

: >"$curl_log"
WATCH_EFFORT=1000 WATCH_EXPECT_TOKEN=false \
    "$watcher" --api-url http://fixture.invalid --color never \
        --bar-width 10 --once --no-clear >"$output"
grep -F 'Luck      [#########-]  99.995460% block chance' "$output" >/dev/null
grep -F 'Effort    [----------]  0.000000% cycle  1000.000000% total  tier 10/10' "$output" >/dev/null

"$watcher" --help | grep -F -- '--snapshot-file PATH' >/dev/null

for bad_args in \
    '--interval 0' \
    '--interval 1.5' \
    '--bar-width 9' \
    '--bar-width 61' \
    '--color sometimes' \
    '--api-url='
do
    read -r -a args <<<"$bad_args"
    if "$watcher" "${args[@]}" >"$output" 2>&1; then
        printf 'watch-status test: invalid arguments accepted: %s\n' \
            "$bad_args" >&2
        exit 1
    fi
done

if "$watcher" --interval >"$output" 2>&1; then
    printf 'watch-status test: missing option value accepted\n' >&2
    exit 1
fi

if [[ -n "$python3" || -n "$tui_test" || -n "$tui_program" ]]; then
    [[ -n "$python3" && -n "$tui_test" && -n "$tui_program" ]] || {
        printf 'watch-status test: incomplete TUI test command\n' >&2
        exit 1
    }
    "$python3" "$tui_test" "$tui_program"

    dispatch_log="$fixture_dir/dispatch-events.jsonl"
    printf '%s\n' \
        '{"time":"2026-08-13T00:00:00Z","severity":"info","code":"runtime.ready","fields":{"height":7}}' \
        >"$dispatch_log"
    "$watcher" --view events --ui tty --theme black \
        --event-log "$dispatch_log" --from-start --event-rate 20 --once \
        >"$output"
    grep -F 'RUNTIME.READY  height=7' "$output" >/dev/null
    if LC_ALL=C grep -q $'\033' "$output"; then
        printf 'watch-status test: dispatched TTY output emitted ANSI escapes\n' >&2
        exit 1
    fi
fi
