#!/usr/bin/env bash
# Regenerates the committed demo data and the golden outputs the dashboard
# tests compare against. Run it after changing charger_synth or the journal
# format; commit the result. Needs a built tree (CHARGER_BUILD_DIR or $1).
#
#   cmake -S charger_project -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   bash dashboard/tests/regen_golden.sh build
#
# Everything here is produced by the real C++ tools. The demo journal is
# SYNTHETIC (charger_synth drives the real Monitor and Ledger with a scenario
# clock); nothing in it is a measurement.
set -euo pipefail
cd "$(dirname "$0")/../.."
BUILD="${1:-${CHARGER_BUILD_DIR:-build}}"
for tool in charger_synth charger_settle charger_monitor; do
    [ -x "$BUILD/$tool" ] || { echo "missing $BUILD/$tool (build first)" >&2; exit 1; }
done
DATA=dashboard/data
GOLD=dashboard/tests/golden
mkdir -p "$DATA" "$GOLD"

"$BUILD/charger_synth" --out "$DATA/demo.journal" --force \
    --snapshots-out "$DATA/demo-capture.ndjson" --snapshots-every 300 2>/dev/null

"$BUILD/charger_settle" "$DATA/demo.journal" > "$GOLD/settle.csv"
"$BUILD/charger_settle" "$DATA/demo.journal" --summary > "$GOLD/summary.txt"
"$BUILD/charger_monitor" --portfolio "$DATA/demo.journal" > "$GOLD/portfolio.json"

# One session of each interesting kind, picked deterministically from the CSV.
pick() { awk -F, -v col="$1" -v val="$2" -v bill="$3" 'NR>1 && $col==val && (bill=="" || $5==bill) {print $1","$2; exit}' "$GOLD/settle.csv"; }
rm -f "$GOLD"/dispute-*.json
printf '[' > "$GOLD/dispute-index.json"
first=1
for sel in "4 OK yes" "4 GAPS no" "4 NO_INTERVAL no" "6 COMMUNICATION_TIMEOUT no" "6 POWER_STAGE_FAULT yes"; do
    set -- $sel
    row="$(pick "$1" "$2" "$3")"
    [ -n "$row" ] || continue
    asset="${row%,*}"; id="${row#*,}"
    "$BUILD/charger_settle" "$DATA/demo.journal" --dispute "$asset" "$id" > "$GOLD/dispute-$asset-$id.json"
    [ $first -eq 1 ] || printf ',' >> "$GOLD/dispute-index.json"
    first=0
    printf '{"asset":"%s","id":%s}' "$asset" "$id" >> "$GOLD/dispute-index.json"
done
printf ']\n' >> "$GOLD/dispute-index.json"

# Demo anchor: pinned at 3/4 of the journal so the dashboard shows an anchor
# that verifies while the journal has grown since. The timestamp is fixed so
# the file is reproducible; verification never reads it.
total=$(wc -l < "$DATA/demo.journal")
keep=$(( total * 3 / 4 ))
tmp="$(mktemp)"
head -n "$keep" "$DATA/demo.journal" > "$tmp"
"$BUILD/charger_monitor" --anchor-journal --journal "$tmp" --anchor-file "$DATA/demo.anchor" >/dev/null
sed -i 's/"anchored_at_utc":"[^"]*"/"anchored_at_utc":"2026-10-09T18:00:00Z"/' "$DATA/demo.anchor"
rm -f "$tmp"
"$BUILD/charger_monitor" --verify-anchor --journal "$DATA/demo.journal" --anchor-file "$DATA/demo.anchor" | tail -1
echo "regenerated demo data and goldens in $DATA and $GOLD"
