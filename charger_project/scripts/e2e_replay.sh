#!/bin/bash
# End-to-end test without a real bus: cansend-style frames into --replay,
# verifying the documented state sequence and ledger contract in the JSON output.
#
# The binary can be overridden with CHARGER_MONITOR_BIN so the same contract
# runs outside CI (WSL, a local build, a prebuilt artifact).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${CHARGER_MONITOR_BIN:-./build/charger_monitor}"
if [ ! -x "$BIN" ]; then
    echo "E2E FAIL: binary not found or not executable: $BIN" >&2
    exit 1
fi

out="$(printf '180#80BBE80300000000\n181#01\n181#02\n181#03\n181#01\nmalformed-line\n' \
    | "$BIN" --replay 2>/dev/null)"

state_line() { echo "$out" | grep -o '"state":"[A-Z]*"' | sed -n "$1p"; }
expected_states=('"state":"DISCONNECTED"' '"state":"READY"' '"state":"CHARGING"' '"state":"FAULT"' '"state":"FAULT"')

for i in "${!expected_states[@]}"; do
    got="$(state_line $((i + 1)))"
    if [ "$got" != "${expected_states[$i]}" ]; then
        echo "E2E FAIL: snapshot $((i + 1)) expected ${expected_states[$i]}, got $got" >&2
        exit 1
    fi
done
# 5 snapshots: the malformed line produces no JSON (correct behavior)
line_count="$(echo "$out" | wc -l)"
if [ "$line_count" -ne 5 ]; then
    echo "E2E FAIL: expected 5 snapshots, found $line_count" >&2
    exit 1
fi

# Contratto economico: la sessione aperta da CHARGING deve chiudersi al FAULT
# dello stadio di potenza, conservando l'energia accettata e dichiarando lo stop.
if ! echo "$out" | grep -q '"schema":"charger.snapshot.v1"'; then
    echo "E2E FAIL: snapshot schema missing" >&2
    exit 1
fi
if ! echo "$out" | grep -q '"stop_reason":"POWER_STAGE_FAULT"'; then
    echo "E2E FAIL: session did not close with POWER_STAGE_FAULT" >&2
    exit 1
fi
if ! echo "$out" | grep -q '"sessions_closed":1'; then
    echo "E2E FAIL: sessions_closed not counted" >&2
    exit 1
fi

# Journal durevole: la catena hash deve verificarsi e contenere la sessione.
journal="$(mktemp)"
out2="$(printf '180#80BBE80300000000\n181#01\n181#02\n181#00\n' \
    | "$BIN" --replay --asset-id bay-e2e --tariff-eur-per-kwh 0.25 --journal "$journal" --replay-interval-ms 1000 2>/dev/null)"
verify_out="$("$BIN" --verify-journal --journal "$journal" 2>&1)"
if ! echo "$verify_out" | grep -q 'LEDGER VERIFIED'; then
    echo "E2E FAIL: journal not verified: $verify_out" >&2
    rm -f "$journal"
    exit 1
fi
if ! echo "$verify_out" | grep -q '1 closed sessions'; then
    echo "E2E FAIL: journal must contain one closed session: $verify_out" >&2
    rm -f "$journal"
    exit 1
fi

# Tampering: altering a field must invalidate the hash and return a non-zero
# exit, so an audit step in CI fails for real.
awk 'NR==1{sub(/\t/, "\t9\t")} {print}' "$journal" > "${journal}.tampered"
if "$BIN" --verify-journal --journal "${journal}.tampered" >/dev/null 2>&1; then
    echo "E2E FAIL: a tampered journal must fail verification" >&2
    rm -f "$journal" "${journal}.tampered"
    exit 1
fi
rm -f "$journal" "${journal}.tampered"

# Vendor DBC path: decode the OEM wire layout from the cantools-validated
# fixture. Frames below are NOT the demo protocol: Motorola big-endian in
# 0x300, the status value table on the 1-byte message 0x301.
SETTLE="${CHARGER_SETTLE_BIN:-./build/charger_settle}"
dbc_journal="$(mktemp)"
dbc_frames='300#BB80271000000000\n301#02\n300#BB80271000000000\n300#BB80271000000000\n301#00\n'
printf "$dbc_frames" | "$BIN" --replay --replay-interval-ms 1000 \
    --dbc tests/fixtures/vendor_bay.dbc \
    --voltage-signal DC_Voltage --current-signal DC_Current \
    --asset-id bay-07 --journal "$dbc_journal" >/dev/null 2>&1 || {
    echo "E2E FAIL: the vendor DBC replay rejected valid frames" >&2
    rm -f "$dbc_journal"; exit 1
}
if [ "$("$BIN" --verify-journal --journal "$dbc_journal" 2>&1 | grep -c '1 closed session')" -eq 0 ]; then
    echo "E2E FAIL: the DBC-bound run must close exactly one session" >&2
    rm -f "$dbc_journal"; exit 1
fi

# Dispute pack: meter -> tariff of record -> calculation -> chain provenance,
# self-verifiable without these binaries.
dispute="$("$SETTLE" "$dbc_journal" --dispute bay-07 1 2>/dev/null)" || {
    echo "E2E FAIL: charger_settle --dispute failed" >&2
    rm -f "$dbc_journal"; exit 1
}
if ! echo "$dispute" | grep -q '"schema":"charger.dispute.v1"'; then
    echo "E2E FAIL: the dispute pack schema is missing" >&2
    rm -f "$dbc_journal"; exit 1
fi
if ! echo "$dispute" | grep -q '"quality":"OK"'; then
    echo "E2E FAIL: the disputed session is not billable after a clean close" >&2
    rm -f "$dbc_journal"; exit 1
fi
if ! echo "$dispute" | grep -q 'close_line_raw'; then
    echo "E2E FAIL: the pack must carry the chain provenance" >&2
    rm -f "$dbc_journal"; exit 1
fi
if ! echo "$dispute" | grep -q 'charger.tariff.v1'; then
    echo "E2E FAIL: the pack must carry the tariff of record" >&2
    rm -f "$dbc_journal"; exit 1
fi
rm -f "$dbc_journal"

echo "E2E OK: DISCONNECTED->READY->CHARGING->FAULT(latched), ledger verified, tampering detected," \
     "vendor DBC decoded live, dispute pack verified"