#!/usr/bin/env bash

set -euo pipefail
umask 077

readonly api_url="${MSS_API_URL:-http://127.0.0.1:8787}"
readonly interval_seconds="${MSS_INTERVAL_SECONDS:-5}"
readonly snapshot_file="${MSS_SNAPSHOT_FILE:-}"
readonly api_token="${MSS_API_TOKEN:-}"

once=false
clear_screen=true
usage() {
    cat <<'EOF'
Usage: watch-status.sh [--once] [--no-clear]

Human-readable monitor for monero-solo-stratum. Environment:
  MSS_API_URL           API base URL (default http://127.0.0.1:8787)
  MSS_API_TOKEN         optional exact Bearer token
  MSS_INTERVAL_SECONDS  refresh interval (default 5)
  MSS_SNAPSHOT_FILE     optional append-only NDJSON snapshot file
EOF
}

while (($#)); do
    case "$1" in
        --once) once=true ;;
        --no-clear) clear_screen=false ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'watch-status: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

[[ "$interval_seconds" =~ ^[1-9][0-9]*$ ]] || {
    printf 'watch-status: MSS_INTERVAL_SECONDS must be a positive integer\n' >&2
    exit 2
}

if [[ -n "$snapshot_file" && ( -L "$snapshot_file" ||
      ( -e "$snapshot_file" && ! -f "$snapshot_file" ) ) ]]; then
    printf 'watch-status: snapshot path must be a regular file, not a link\n' >&2
    exit 2
fi

for command_name in curl date jq sleep; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'watch-status: required command is unavailable: %s\n' "$command_name" >&2
        exit 1
    }
done

curl_args=(--fail --silent --show-error --max-time 10)
if [[ -n "$api_token" ]]; then
    curl_args+=(-H "Authorization: Bearer $api_token")
fi

fetch() {
    curl "${curl_args[@]}" "$api_url$1"
}

while :; do
    captured_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    if ! summary="$(fetch /v1/summary)" ||
       ! ready="$(fetch /v1/health/ready)" ||
       ! verifier="$(fetch /v1/verifier)" ||
       ! persistence="$(fetch /v1/persistence)" ||
       ! top="$(fetch '/v1/shares/top')" ||
       ! high="$(fetch '/v1/shares/recent-high')"
    then
        printf 'watch-status: API request failed at %s\n' "$captured_at" >&2
        $once && exit 1
        sleep "$interval_seconds"
        continue
    fi

    if [[ -n "$snapshot_file" ]]; then
        jq -cn \
            --arg captured_at "$captured_at" \
            --argjson summary "$summary" \
            --argjson ready "$ready" \
            --argjson verifier "$verifier" \
            --argjson persistence "$persistence" \
            --argjson top_shares "$top" \
            --argjson recent_high_shares "$high" \
            '{captured_at:$captured_at,summary:$summary,ready:$ready,'\
'verifier:$verifier,persistence:$persistence,top_shares:$top_shares,'\
'recent_high_shares:$recent_high_shares}' \
            >> "$snapshot_file"
    fi

    if $clear_screen && [[ -t 1 ]] && ! $once; then printf '\033[H\033[2J'; fi
    jq -nr \
        --arg captured_at "$captured_at" \
        --argjson s "$summary" \
        --argjson r "$ready" \
        --argjson v "$verifier" \
        --argjson p "$persistence" \
        --argjson top "$top" \
        --argjson high "$high" '
        def hs:
          (tonumber? // 0) as $n |
          if $n >= 1000000000 then "\($n / 1000000000 | floor) GH/s"
          elif $n >= 1000000 then "\($n / 1000000 | floor) MH/s"
          elif $n >= 1000 then "\(($n / 1000 * 10 | floor) / 10) kH/s"
          else "\($n | floor) H/s" end;
        ($s.data // {}) as $d |
        ($d.round // {}) as $round |
        ($d.hashrate // {}) as $rate |
        ($p.data // {}) as $pd |
        "monero-solo-stratum  \($captured_at)",
        "ready=\($r.data.ready // false)  network=\($d.server.network // "?")  uptime=\($d.server.uptime_seconds // 0)s  height=\($d.daemon.height // "?")",
        "daemon: rpc=\($d.daemon.rpc // "?")  zmq=\($d.daemon.zmq // "?")  template=\($d.daemon.template_id // "?")/gen \($d.daemon.template_generation // "?")",
        "miners: \($d.connections.active // 0) connections / \($d.workers.active // 0) workers",
        "hashrate: 1m \(($rate["1m"] // "0") | hs)   5m \(($rate["5m"] // "0") | hs)   1h \(($rate["1h"] // "0") | hs)   [\($rate.source // "?")]",
        "shares: accepted \($d.shares.accepted // "0") / total \($d.shares.total // "0")   stale \($d.shares.stale // "0")   low \($d.shares.low_difficulty // "0")",
        "round: #\($round.id // "?") \($round.state // "?")   shares \($round.accepted_share_count // "0")   estimated hashes \($round.estimated_hashes // $round.credited_difficulty // "0")   effort \($round.effort.value // $round.effort_percent // "0.000000")%",
        "verifier: enabled=\($v.data.enabled // false) source=\($v.data.provenance // "?") memory=\($v.data.configuration.memory_mode // "?") outstanding=\($v.data.stats.outstanding // "0") pending=\($v.data.stats.pending // 0) failed=\($v.data.stats.failed // "0")",
        "candidates: accepted \($d.candidates.accepted // "0") / total \($d.candidates.total // "0")   active \($d.candidates.active // "0")   ambiguous \($d.candidates.ambiguous // "0")",
        "database: \($pd.database_bytes // 0) B + WAL \($pd.wal_bytes // 0) B   writer queue \($pd.writer_queue_items // 0)   unresolved candidates \($pd.unresolved_candidates // "0")",
        "top shares:",
        (($top.data // [])[0:5] | if length == 0 then "  (none)" else .[] | "  #\(.id) diff=\(.actual_difficulty // "?") worker=\(.worker_id // "?") round=\(.round_id // "?")" end),
        "recent high shares:",
        (($high.data // [])[0:5] | if length == 0 then "  (none)" else .[] | "  #\(.id) diff=\(.actual_difficulty // "?") worker=\(.worker_id // "?") round=\(.round_id // "?")" end)
    '

    $once && break
    sleep "$interval_seconds"
done
