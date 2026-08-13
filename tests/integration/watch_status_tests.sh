#!/usr/bin/env bash

set -euo pipefail

readonly watcher="${1:?watch-status path is required}"
readonly fixture_dir="$(mktemp -d)"
trap 'rm -rf -- "$fixture_dir"' EXIT
readonly snapshot="$fixture_dir/snapshots.jsonl"
readonly output="$fixture_dir/output.txt"

curl() {
    local url="${!#}"
    [[ "$url" != *'limit=5'* ]] || return 99
    case "$url" in
        */v1/summary)
            printf '%s\n' '{"data":{"server":{"network":"regtest","uptime_seconds":60},"daemon":{"height":2,"rpc":"healthy","zmq":"healthy","template_id":"9","template_generation":"4"},"connections":{"active":3},"workers":{"active":3},"hashrate":{"1m":"10800","5m":"9000","1h":"6000","source":"verified"},"shares":{"accepted":"4","total":"5","stale":"1","low_difficulty":"0"},"round":{"id":"2","state":"open","accepted_share_count":"4","estimated_hashes":"1048640","effort":{"value":"12.345678"}},"candidates":{"accepted":"1","total":"1","active":"0","ambiguous":"0"}}}'
            ;;
        */v1/health/ready)
            printf '%s\n' '{"data":{"ready":true}}'
            ;;
        */v1/verifier)
            printf '%s\n' '{"data":{"enabled":true,"provenance":"verified","configuration":{"memory_mode":"fast"},"stats":{"outstanding":"0","pending":0,"failed":"0"}}}'
            ;;
        */v1/persistence)
            printf '%s\n' '{"data":{"database_bytes":4096,"wal_bytes":512,"writer_queue_items":0,"unresolved_candidates":"0"}}'
            ;;
        */v1/shares/top)
            printf '%s\n' '{"data":[{"id":"5","actual_difficulty":"90000000000","worker_id":"2","round_id":"2"}]}'
            ;;
        */v1/shares/recent-high)
            printf '%s\n' '{"data":[{"id":"4","actual_difficulty":"20000000000","worker_id":"1","round_id":"1"}]}'
            ;;
        *) return 98 ;;
    esac
}
export -f curl

MSS_API_URL=http://fixture.invalid \
MSS_SNAPSHOT_FILE="$snapshot" \
    "$watcher" --once --no-clear >"$output"

grep -F 'daemon: rpc=healthy  zmq=healthy  template=9/gen 4' "$output" >/dev/null
grep -F 'hashrate: 1m 10.8 kH/s' "$output" >/dev/null
grep -F 'shares 4   estimated hashes 1048640   effort 12.345678%' "$output" >/dev/null
grep -F 'source=verified memory=fast outstanding=0 pending=0 failed=0' "$output" >/dev/null
jq -e '
    .summary.data.round.effort.value == "12.345678" and
    .ready.data.ready == true and
    .verifier.data.provenance == "verified" and
    (.persistence.data | type) == "object" and
    (.top_shares.data | type) == "array" and
    (.recent_high_shares.data | type) == "array"
' "$snapshot" >/dev/null
