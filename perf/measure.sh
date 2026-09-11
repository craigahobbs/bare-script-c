#!/bin/sh
# Size and memory figures for static/perf/data/size.json.
#
#   perf/measure.sh BARE LIB SUITE [INCLUDE_C]
set -e
BARE="$1"
LIB="$2"
SUITE="$3"
INCLUDE_C="${4:-src/includeSource.c}"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# The maximum resident set and peak footprint of a run, in MB. Always prints two numbers
# so a failed timed command cannot emit empty JSON fields.
mem() {
    /usr/bin/time -l "$@" > /dev/null 2> "$TMP" || true
    awk '/maximum resident set size/ { rss = $1 } /peak memory footprint/ { peak = $1 }
         END { printf "%.2f %.2f", rss / 1048576, peak / 1048576 }' "$TMP"
}

set -- $(mem "$BARE" -c '')
empty_rss=$1; empty_peak=$2
set -- $(mem "$BARE" -v vTest "'markdownElements'" perf/test.bare)
perf_rss=$1
set -- $(mem "$BARE" -d "$SUITE")
suite_rss=$1
set -- $(mem "$BARE" -d -m "$SUITE")
cover_rss=$1

include_bytes=$(grep -o 0x "$INCLUDE_C" | wc -l | tr -d ' ')

printf '{\n'
printf '  "when": "%s",\n' "$(date +%Y-%m-%d)"
printf '  "machine": "%s",\n' "${PERF_MACHINE:-Apple M3 Max}"
printf '  "emptyRss": %s,\n' "${empty_rss:-0}"
printf '  "emptyPeak": %s,\n' "${empty_peak:-0}"
printf '  "perfRss": %s,\n' "${perf_rss:-0}"
printf '  "suiteRss": %s,\n' "${suite_rss:-0}"
printf '  "coverRss": %s,\n' "${cover_rss:-0}"
printf '  "dylibBytes": %s,\n' "$(wc -c < "$LIB" | tr -d ' ')"
printf '  "textBytes": %s,\n' "$(size -m "$LIB" | awk '/Section __text/ { print $3; exit }')"
printf '  "includeBytes": %s\n' "$include_bytes"
printf '}\n'
