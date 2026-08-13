#!/usr/bin/env bash

set -euo pipefail
umask 077

readonly program_name="${0##*/}"

api_url="http://127.0.0.1:8787"
api_token=""
interval_seconds="5"
snapshot_file=""
color_mode="auto"
bar_width="28"
once=false
clear_screen=true

usage() {
    printf '%s\n' \
        "Usage: $program_name [OPTIONS]" \
        "" \
        "Lightweight human-readable monitor for monero-solo-stratum." \
        "" \
        "Options:" \
        "  --api-url URL          API base URL (default http://127.0.0.1:8787)" \
        "  --api-token TOKEN      Optional exact Bearer token" \
        "  --interval SECONDS     Refresh interval (default 5)" \
        "  --snapshot-file PATH   Append raw API snapshots as NDJSON" \
        "  --color MODE           auto, always, or never (default auto)" \
        "  --bar-width COLUMNS    ASCII progress-bar width, 10-60 (default 28)" \
        "  --once                 Print one snapshot and exit" \
        "  --no-clear             Do not clear the terminal between updates" \
        "  -h, --help             Show this help"
}

missing_value() {
    printf '%s: %s requires a value\n' "$program_name" "$1" >&2
    usage >&2
    exit 2
}

while (($#)); do
    case "$1" in
        --api-url|--api-token|--interval|--snapshot-file|--color|--bar-width)
            (($# >= 2)) || missing_value "$1"
            option="$1"
            value="$2"
            shift 2
            case "$option" in
                --api-url) api_url="$value" ;;
                --api-token) api_token="$value" ;;
                --interval) interval_seconds="$value" ;;
                --snapshot-file) snapshot_file="$value" ;;
                --color) color_mode="$value" ;;
                --bar-width) bar_width="$value" ;;
            esac
            ;;
        --api-url=*) api_url="${1#*=}"; shift ;;
        --api-token=*) api_token="${1#*=}"; shift ;;
        --interval=*) interval_seconds="${1#*=}"; shift ;;
        --snapshot-file=*) snapshot_file="${1#*=}"; shift ;;
        --color=*) color_mode="${1#*=}"; shift ;;
        --bar-width=*) bar_width="${1#*=}"; shift ;;
        --once) once=true; shift ;;
        --no-clear) clear_screen=false; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *)
            printf '%s: unknown argument: %s\n' "$program_name" "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if (($#)); then
    printf '%s: unexpected positional argument: %s\n' \
        "$program_name" "$1" >&2
    usage >&2
    exit 2
fi

[[ -n "$api_url" ]] || {
    printf '%s: --api-url must not be empty\n' "$program_name" >&2
    exit 2
}
api_url="${api_url%/}"

[[ "$interval_seconds" =~ ^[1-9][0-9]*$ ]] || {
    printf '%s: --interval must be a positive integer\n' "$program_name" >&2
    exit 2
}

[[ "$bar_width" =~ ^[0-9]+$ ]] &&
    ((bar_width >= 10 && bar_width <= 60)) || {
    printf '%s: --bar-width must be an integer from 10 through 60\n' \
        "$program_name" >&2
    exit 2
}

case "$color_mode" in
    auto|always|never) ;;
    *)
        printf '%s: --color must be auto, always, or never\n' \
            "$program_name" >&2
        exit 2
        ;;
esac

if [[ -n "$snapshot_file" && ( -L "$snapshot_file" ||
      ( -e "$snapshot_file" && ! -f "$snapshot_file" ) ) ]]; then
    printf '%s: snapshot path must be a regular file, not a link\n' \
        "$program_name" >&2
    exit 2
fi

for command_name in curl date jq sleep; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf '%s: required command is unavailable: %s\n' \
            "$program_name" "$command_name" >&2
        exit 1
    }
done

use_color=false
case "$color_mode" in
    always) use_color=true ;;
    auto)
        if [[ -t 1 && "${TERM:-dumb}" != "dumb" ]]; then
            use_color=true
        fi
        ;;
esac

curl_args=(--fail --silent --show-error --max-time 10)
if [[ -n "$api_token" ]]; then
    curl_args+=(-H "Authorization: Bearer $api_token")
fi

fetch() {
    curl "${curl_args[@]}" "$api_url$1"
}

while :; do
    captured_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

    if ! summary="$(fetch /v1/summary)"; then
        printf '%s: API request failed at %s\n' \
            "$program_name" "$captured_at" >&2
        $once && exit 1
        sleep "$interval_seconds"
        continue
    fi

    round_id="$(jq -r '
        .data.round.id // empty |
        tostring |
        select(test("^[1-9][0-9]*$"))
    ' <<<"$summary")"

    top='{"schema_version":1,"data":[]}'
    top_path=""
    if [[ -n "$round_id" ]]; then
        top_path="/v1/shares/top?round_id=$round_id"
    fi

    if ! ready="$(fetch /v1/health/ready)" ||
       ! verifier="$(fetch /v1/verifier)" ||
       ! persistence="$(fetch /v1/persistence)" ||
       { [[ -n "$top_path" ]] && ! top="$(fetch "$top_path")"; } ||
       ! high="$(fetch /v1/shares/recent-high)"
    then
        printf '%s: API request failed at %s\n' \
            "$program_name" "$captured_at" >&2
        $once && exit 1
        sleep "$interval_seconds"
        continue
    fi

    if [[ -n "$snapshot_file" ]]; then
        jq -cn \
            --arg captured_at "$captured_at" \
            --arg current_round_id "$round_id" \
            --argjson summary "$summary" \
            --argjson ready "$ready" \
            --argjson verifier "$verifier" \
            --argjson persistence "$persistence" \
            --argjson top_shares "$top" \
            --argjson recent_high_shares "$high" \
            '{captured_at:$captured_at,current_round_id:$current_round_id,'\
'summary:$summary,ready:$ready,verifier:$verifier,'\
'persistence:$persistence,top_shares:$top_shares,'\
'recent_high_shares:$recent_high_shares}' \
            >> "$snapshot_file"
    fi

    if $clear_screen && [[ -t 1 ]] && ! $once; then
        printf '\033[H\033[2J'
    fi

    jq -nr \
        --arg captured_at "$captured_at" \
        --argjson color "$use_color" \
        --argjson bar_width "$bar_width" \
        --argjson s "$summary" \
        --argjson r "$ready" \
        --argjson v "$verifier" \
        --argjson p "$persistence" \
        --argjson top "$top" \
        --argjson high "$high" '
        def number_or_null: try tonumber catch null;
        def fixed($places):
          . as $number |
          pow(10; $places) as $scale |
          (($number * $scale) | round) as $scaled |
          (($scaled / $scale) | floor) as $whole |
          (($scaled - ($whole * $scale)) | fabs | floor | tostring) as $fraction |
          if $places == 0 then "\($whole)"
          else "\($whole).\("0" * ($places - ($fraction | length)))\($fraction)"
          end;
        def flexible:
          fabs as $magnitude |
          if $magnitude < 10 then fixed(2)
          elif $magnitude < 100 then fixed(1)
          else fixed(0)
          end;
        def human_si($unit):
          (number_or_null // 0) as $number |
          ($number | fabs) as $magnitude |
          if $magnitude >= 1000000000000 then
            "\($number / 1000000000000 | flexible) T\($unit)"
          elif $magnitude >= 1000000000 then
            "\($number / 1000000000 | flexible) G\($unit)"
          elif $magnitude >= 1000000 then
            "\($number / 1000000 | flexible) M\($unit)"
          elif $magnitude >= 1000 then
            "\($number / 1000 | flexible) k\($unit)"
          else "\($number | fixed(0)) \($unit)" end;
        def human_rate: human_si("H/s");
        def human_hashes: human_si("H");
        def human_difficulty: human_si("");
        def human_bytes:
          (number_or_null // 0) as $number |
          if $number >= 1099511627776 then
            "\($number / 1099511627776 | flexible) TiB"
          elif $number >= 1073741824 then
            "\($number / 1073741824 | flexible) GiB"
          elif $number >= 1048576 then
            "\($number / 1048576 | flexible) MiB"
          elif $number >= 1024 then
            "\($number / 1024 | flexible) KiB"
          else "\($number | fixed(0)) B" end;
        def human_duration:
          (number_or_null // 0 | floor) as $total |
          ($total / 86400 | floor) as $days |
          (($total % 86400) / 3600 | floor) as $hours |
          (($total % 3600) / 60 | floor) as $minutes |
          ($total % 60) as $seconds |
          ([if $days > 0 then "\($days)d" else empty end,
            if $hours > 0 then "\($hours)h" else empty end,
            if $minutes > 0 then "\($minutes)m" else empty end,
            if $seconds > 0 or $total == 0 then "\($seconds)s" else empty end]
           | join(" "));
        def human_time:
          if type == "string" and length >= 19 then
            "\(.[0:10]) \(.[11:19]) UTC"
          else "time unknown" end;
        def sgr($code):
          if $color then "\u001b[\($code)m" else "" end;
        def fg256($code):
          if $color then "\u001b[38;5;\($code)m" else "" end;
        def styled($code; $text):
          "\(sgr($code))\($text)\(sgr("0"))";
        def shaded($code; $text):
          "\(fg256($code))\($text)\(sgr("0"))";
        def palette($index):
          [46, 40, 34, 70, 106, 142, 178, 214, 208, 196][$index];
        def progress($percent; $shade):
          (if $percent < 0 then 0
           elif $percent > 100 then 100
           else $percent end) as $bounded |
          ($bounded * $bar_width / 100 | floor) as $filled |
          ("#" * $filled) as $marks |
          ("-" * ($bar_width - $filled)) as $empty |
          if $color then
            "[\(shaded(palette($shade); $marks))\(sgr("2"))\($empty)\(sgr("0"))]"
          else "[\($marks)\($empty)]" end;
        def health($value):
          ($value // "unknown" | tostring) as $text |
          if $text == "healthy" or $text == "ready" or $text == "running" or $text == "open" then
            styled("1;32"; $text)
          elif $text == "disabled" or $text == "idle" then
            styled("33"; $text)
          else styled("1;31"; $text) end;
        def count_word($number; $singular; $plural):
          "\($number) \(if ($number | tonumber? // 0) == 1 then $singular else $plural end)";
        def share_line:
          "  \(.key + 1). \(.value.actual_difficulty // 0 | human_difficulty)" +
          "  \(.value.completed_at // .value.received_at | human_time)" +
          "  share #\(.value.id // "?")" +
          "  worker #\(.value.worker_id // "?")";

        ($s.data // {}) as $data |
        ($data.round // {}) as $round |
        ($data.hashrate // {}) as $rate |
        ($data.shares // {}) as $shares |
        ($data.candidates // {}) as $candidates |
        ($v.data // {}) as $verifier |
        ($p.data // {}) as $storage |
        (($round.effort.value // null) | number_or_null) as $effort_raw |
        (if $effort_raw != null and $effort_raw >= 0 then $effort_raw else null end) as $effort |
        (if $effort == null then null
         else 100 * (1 - (($effort / -100) | exp)) end) as $chance |
        (if $effort == null then null
         else $effort - (($effort / 100 | floor) * 100) end) as $effort_cycle |
        (if $effort == null then 0
         else [9, ($effort / 100 | floor)] | min end) as $effort_shade |
        (if $chance == null then 0
         else [9, ($chance / 10 | floor)] | min end) as $chance_shade |
        (if $effort == null then 1
         else [10, (($effort / 100 | floor) + 1)] | min end) as $tier |

        "\(styled("1;38;5;208"; "MONERO SOLO STRATUM"))  \(sgr("2"))\($captured_at)\(sgr("0"))  " +
          (if ($r.data.ready // false) then styled("1;32"; "READY")
           else styled("1;31"; "NOT READY") end),
        "\($data.server.network // "unknown")  height \($data.daemon.height // "?")  uptime \($data.server.uptime_seconds // 0 | human_duration)",
        "Node      RPC \(health($data.daemon.rpc))  ZMQ \(health($data.daemon.zmq))  template #\($data.daemon.template_id // "?") / gen \($data.daemon.template_generation // "?")",
        "Miners    \(count_word($data.connections.active // 0; "connection"; "connections"))  \(count_word($data.workers.active // 0; "worker"; "workers"))",
        "Hashrate  1m \($rate["1m"] // 0 | human_rate)  5m \($rate["5m"] // 0 | human_rate)  1h \($rate["1h"] // 0 | human_rate)  [\($rate.source // "unknown")]",
        "",
        "\(styled("1;36"; "ROUND #\($round.id // "?")"))  \(health($round.state))",
        (if $chance == null then
           "Luck      [n/a]  block chance unavailable"
         else
           "Luck      \(progress($chance; $chance_shade))  \($chance | fixed(6))% block chance"
         end),
        (if $effort == null then
           "Effort    [n/a]  round effort unavailable"
         else
           "Effort    \(progress($effort_cycle; $effort_shade))  \($effort_cycle | fixed(6))% cycle  \($effort | fixed(6))% total  tier \($tier)/10"
         end),
        "Work      \($round.estimated_hashes // $round.credited_difficulty // 0 | human_hashes) estimated  \(count_word($round.accepted_share_count // 0; "accepted share"; "accepted shares"))",
        "Shares    \(styled("1;32"; "\($shares.accepted // 0) accepted")) / \($shares.total // 0) total  stale \($shares.stale // 0)  duplicate \($shares.duplicate // 0)  low \($shares.low_difficulty // 0)  invalid \($shares.invalid_result // 0)",
        "Candidates  accepted \($candidates.accepted // 0) / total \($candidates.total // 0)  active \($candidates.active // 0)  ambiguous \($candidates.ambiguous // 0)",
        "",
        "\(styled("1;36"; "SYSTEM"))",
        "Verifier  \(if $verifier.enabled // false then styled("1;32"; "enabled") else styled("1;31"; "disabled") end)  \($verifier.provenance // "unknown") / \($verifier.configuration.memory_mode // "unknown")  outstanding \($verifier.stats.outstanding // 0)  pending \($verifier.stats.pending // 0)  failed \($verifier.stats.failed // 0)",
        "Storage   \($storage.database_bytes // 0 | human_bytes) DB + \($storage.wal_bytes // 0 | human_bytes) WAL  writer queue \($storage.writer_queue_items // 0)  unresolved \($storage.unresolved_candidates // 0)",
        "",
        "\(styled("1;36"; "CURRENT ROUND TOP SHARES"))  \(sgr("2"))round #\($round.id // "?")\(sgr("0"))",
        (($top.data // [])[0:5] |
          if length == 0 then "  (none)"
          else to_entries[] | share_line end),
        "",
        "\(styled("1;36"; "RECENT HIGH SHARES"))",
        (($high.data // [])[0:5] |
          if length == 0 then "  (none)"
          else to_entries[] | share_line end)
    '

    $once && break
    sleep "$interval_seconds"
done
