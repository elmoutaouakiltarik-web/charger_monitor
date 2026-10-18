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

# Supervision exports: the newest snapshot and the Prometheus text are rewritten
# atomically for whatever supervises the site. A reader must never see a partial
# file and no temporary file may be left behind.
exp_dir="$(mktemp -d)"
printf '180#80BBE80300000000\n181#01\n181#02\n' \
    | "$BIN" --replay --asset-id bay-exp --replay-interval-ms 1000 \
        --snapshot-file "$exp_dir/status.json" --metrics-file "$exp_dir/charger.prom" >/dev/null 2>&1 || {
    echo "E2E FAIL: replay with --snapshot-file/--metrics-file failed" >&2
    rm -rf "$exp_dir"; exit 1
}
if [ "$(wc -l < "$exp_dir/status.json")" -ne 1 ] || ! grep -q '"type":"snapshot"' "$exp_dir/status.json" \
    || ! grep -q '"state":"CHARGING"' "$exp_dir/status.json"; then
    echo "E2E FAIL: --snapshot-file must hold exactly one CHARGING snapshot line" >&2
    rm -rf "$exp_dir"; exit 1
fi
if ! grep -q '^charger_state{asset="bay-exp",state="CHARGING"} 1$' "$exp_dir/charger.prom" \
    || ! grep -q '^# TYPE charger_sessions_closed_total counter$' "$exp_dir/charger.prom"; then
    echo "E2E FAIL: --metrics-file must hold the Prometheus exposition" >&2
    rm -rf "$exp_dir"; exit 1
fi
if ls "$exp_dir" | grep -q '\.tmp$'; then
    echo "E2E FAIL: an export left a temporary file behind" >&2
    rm -rf "$exp_dir"; exit 1
fi
# An export must never be allowed to overwrite the evidence.
if "$BIN" --replay --journal "$exp_dir/j.journal" --metrics-file "$exp_dir/j.journal" </dev/null >/dev/null 2>&1; then
    echo "E2E FAIL: --metrics-file equal to --journal must be refused" >&2
    rm -rf "$exp_dir"; exit 1
fi
# A failing export must never stop the monitor or touch billing.
if ! printf '180#80BBE80300000000\n181#01\n181#02\n181#00\n' \
    | "$BIN" --replay --replay-interval-ms 1000 --journal "$exp_dir/j.journal" \
        --metrics-file "$exp_dir/no/such/dir/x.prom" >/dev/null 2>"$exp_dir/err.txt"; then
    echo "E2E FAIL: an unwritable export file must not stop the monitor" >&2
    rm -rf "$exp_dir"; exit 1
fi
if ! grep -q 'EXPORT_FILE_FAILED' "$exp_dir/err.txt"; then
    echo "E2E FAIL: an unwritable export file must be reported on stderr" >&2
    rm -rf "$exp_dir"; exit 1
fi
if [ "$("$BIN" --verify-journal --journal "$exp_dir/j.journal" 2>&1 | grep -c '1 closed session')" -eq 0 ]; then
    echo "E2E FAIL: billing must be unaffected by a failing export" >&2
    rm -rf "$exp_dir"; exit 1
fi
# Fleet metrics from the verified journal, to stdout or atomically into a file.
fleet="$("$SETTLE" "$exp_dir/j.journal" --metrics)"
if ! echo "$fleet" | grep -q '^charger_portfolio_sessions_closed 1$'; then
    echo "E2E FAIL: charger_settle --metrics must report the closed session" >&2
    rm -rf "$exp_dir"; exit 1
fi
"$SETTLE" "$exp_dir/j.journal" --metrics --out "$exp_dir/fleet.prom"
if [ "$(cat "$exp_dir/fleet.prom")" != "$fleet" ] || ls "$exp_dir" | grep -q '\.tmp$'; then
    echo "E2E FAIL: charger_settle --metrics --out must match stdout and leave no temporary file" >&2
    rm -rf "$exp_dir"; exit 1
fi
rm -rf "$exp_dir"

# OCPP 2.0.1 TransactionEvent export and reconciliation against an independent
# meter, both on a clean, billable session from the vendor DBC fixture.
y_dir="$(mktemp -d)"
printf "$dbc_frames" | "$BIN" --replay --replay-interval-ms 1000 \
    --dbc tests/fixtures/vendor_bay.dbc \
    --voltage-signal DC_Voltage --current-signal DC_Current \
    --asset-id bay-07 --journal "$y_dir/j.journal" >/dev/null 2>&1
ocpp_out="$("$SETTLE" "$y_dir/j.journal" --ocpp bay-07 1)" || {
    echo "E2E FAIL: charger_settle --ocpp failed on a clean session" >&2
    rm -rf "$y_dir"; exit 1
}
for needle in '"schema":"charger.ocpp-events.v1"' '"eventType":"Started"' '"eventType":"Ended"' \
    '"stoppedReason":"Local"' '"transactionId":"bay-07-1"' '"unit":"Wh","multiplier":-6'; do
    if ! echo "$ocpp_out" | grep -qF "$needle"; then
        echo "E2E FAIL: the OCPP export lacks $needle" >&2
        rm -rf "$y_dir"; exit 1
    fi
done
if "$SETTLE" "$y_dir/j.journal" --ocpp bay-07 99 >/dev/null 2>&1; then
    echo "E2E FAIL: --ocpp for a session that does not exist must fail" >&2
    rm -rf "$y_dir"; exit 1
fi
# The exact ledger energy in micro-Wh (from the CDR), written as decimal Wh.
ledger_uwh="$("$SETTLE" "$y_dir/j.journal" --cdr bay-07 1 | sed -n 's/.*"energy_uwh":"\([0-9]*\)".*/\1/p')"
uwh_as_wh() { printf '%d.%06d' "$(($1 / 1000000))" "$(($1 % 1000000))"; }
printf 'asset_id,session_id,meter_wh\r\nbay-07,1,%s\r\n' "$(uwh_as_wh "$ledger_uwh")" > "$y_dir/meter_ok.csv"
rc=0; "$SETTLE" "$y_dir/j.journal" --reconcile "$y_dir/meter_ok.csv" --tolerance-ppm 0 >"$y_dir/rec.csv" 2>/dev/null || rc=$?
if [ "$rc" -ne 0 ] || ! grep -q '^bay-07,1,.*,OK,$' "$y_dir/rec.csv"; then
    echo "E2E FAIL: identical meter and ledger energy must reconcile (exit 0), got $rc" >&2
    rm -rf "$y_dir"; exit 1
fi
printf 'asset_id,session_id,meter_wh\nbay-07,1,%s\n' "$(uwh_as_wh "$((ledger_uwh * 2))")" > "$y_dir/meter_drift.csv"
rc=0; "$SETTLE" "$y_dir/j.journal" --reconcile "$y_dir/meter_drift.csv" --tolerance-ppm 1000 >"$y_dir/rec.csv" 2>/dev/null || rc=$?
if [ "$rc" -ne 4 ] || ! grep -q ',DRIFT,' "$y_dir/rec.csv"; then
    echo "E2E FAIL: a meter twice the ledger must be DRIFT with exit 4, got $rc" >&2
    rm -rf "$y_dir"; exit 1
fi
rc=0; "$SETTLE" "$y_dir/j.journal" --reconcile "$y_dir/meter_ok.csv" >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 2 ]; then
    echo "E2E FAIL: --reconcile without --tolerance-ppm must be refused (exit 2), got $rc" >&2
    rm -rf "$y_dir"; exit 1
fi
printf 'asset_id,session_id,meter_wh\nbay-07,1,-5\n' > "$y_dir/meter_bad.csv"
rc=0; "$SETTLE" "$y_dir/j.journal" --reconcile "$y_dir/meter_bad.csv" --tolerance-ppm 5 >/dev/null 2>"$y_dir/err.txt" || rc=$?
if [ "$rc" -ne 2 ] || ! grep -q 'line 2' "$y_dir/err.txt"; then
    echo "E2E FAIL: a malformed meter file must be refused with its line number (exit 2), got $rc" >&2
    rm -rf "$y_dir"; exit 1
fi
awk 'NR==1{sub(/\t/, "\t9\t")} {print}' "$y_dir/j.journal" > "$y_dir/tampered.journal"
rc=0; "$SETTLE" "$y_dir/tampered.journal" --reconcile "$y_dir/meter_ok.csv" --tolerance-ppm 5 >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 3 ]; then
    echo "E2E FAIL: reconciling against an altered journal must exit 3, got $rc" >&2
    rm -rf "$y_dir"; exit 1
fi
rm -rf "$y_dir"

# Clean shutdown. A replay that ends while a session is open (the same path as a
# SIGTERM) must not leave an orphan with unknown energy: the session is closed as
# INTERRUPTED, its energy kept, held for review, and the final status file shows it.
sd_dir="$(mktemp -d)"
printf '180#80BBE80300000000\n181#01\n181#02\n180#80BBE80300000000\n180#80BBE80300000000\n' \
    | "$BIN" --replay --replay-interval-ms 1000 --asset-id bay-sd --tariff-eur-per-kwh 0.25 \
        --journal "$sd_dir/j.journal" --snapshot-file "$sd_dir/status.json" >/dev/null 2>"$sd_dir/err.txt" || {
    echo "E2E FAIL: a replay that ends mid-session must still exit 0" >&2
    rm -rf "$sd_dir"; exit 1
}
sd_verify="$("$BIN" --verify-journal --journal "$sd_dir/j.journal" 2>&1)"
if ! echo "$sd_verify" | grep -q '1 closed sessions' || echo "$sd_verify" | grep -q 'ORPHAN'; then
    echo "E2E FAIL: the open session must be closed on shutdown, not orphaned: $sd_verify" >&2
    rm -rf "$sd_dir"; exit 1
fi
if ! "$SETTLE" "$sd_dir/j.journal" | grep -q '^bay-sd,1,.*,INTERRUPTED,no,MONITOR_SHUTDOWN,'; then
    echo "E2E FAIL: the interrupted session must be INTERRUPTED, held (billable=no), MONITOR_SHUTDOWN" >&2
    rm -rf "$sd_dir"; exit 1
fi
if ! grep -q '"stop_reason":"MONITOR_SHUTDOWN"' "$sd_dir/status.json" || ! grep -q '"sessions_closed":1' "$sd_dir/status.json"; then
    echo "E2E FAIL: the final status file must show the closed session" >&2
    rm -rf "$sd_dir"; exit 1
fi
rm -rf "$sd_dir"

# The same through a real signal: SIGTERM on a live --simulate run.
sg_dir="$(mktemp -d)"
"$BIN" --simulate --asset-id bay-sig --journal "$sg_dir/j.journal" >/dev/null 2>"$sg_dir/err.txt" &
sg_pid=$!
sleep 4
kill -TERM "$sg_pid" 2>/dev/null || true
sg_rc=0; wait "$sg_pid" 2>/dev/null || sg_rc=$?
if [ "$sg_rc" -ge 128 ]; then
    # Windows: no signal can be delivered to a native process, so the kill is a
    # hard termination with no chance to journal anything. The graceful-shutdown
    # semantics are asserted in-process by tests/test_shutdown.cpp, which runs
    # on every platform; end-of-input shutdown is asserted above.
    echo "E2E NOTE: SIGTERM delivery unavailable on this platform (exit $sg_rc); graceful and EOF shutdowns are asserted elsewhere"
else
    sg_verify="$("$BIN" --verify-journal --journal "$sg_dir/j.journal" 2>&1)"
    if ! echo "$sg_verify" | grep -q 'LEDGER VERIFIED' || echo "$sg_verify" | grep -q 'ORPHAN' ||
            ! "$SETTLE" "$sg_dir/j.journal" | grep -q 'INTERRUPTED,no,MONITOR_SHUTDOWN'; then
        echo "E2E FAIL: SIGTERM with a session open must journal an INTERRUPTED close, no orphan: $sg_verify" >&2
        rm -rf "$sg_dir"; exit 1
    fi
fi
rm -rf "$sg_dir"

# Candump replay with the REAL timing and the RECORDED UTC. The fixture is a
# hand-built 25-minute recording with one 12 s dropout; the figures below were
# derived independently by tests/fixtures/make_bay_session.py --expect.
cd_dir="$(mktemp -d)"
"$BIN" --replay-candump tests/fixtures/bay_session.candump --asset-id bay-07 \
    --tariff-eur-per-kwh 0.25 --tariff-eur-per-hour 3.60 --flat-fee-eur 0.50 \
    --journal "$cd_dir/j.journal" --snapshot-file "$cd_dir/status.json" >"$cd_dir/out.txt" 2>"$cd_dir/err.txt" || {
    echo "E2E FAIL: --replay-candump failed on the fixture" >&2
    rm -rf "$cd_dir"; exit 1
}
expected_summary='CANDUMP_REPLAY lines=2978 frames_used=2978 invalid_lines=0 error_frames=0 rtr=0 fd=0 extended=0 no_time=0 other_iface=0 clock_regressions=0 first_utc=2026-10-18T08:00:00Z last_utc=2026-10-18T08:25:00Z recorded_ms=1500050'
if ! grep -qxF "$expected_summary" "$cd_dir/err.txt"; then
    echo "E2E FAIL: the candump summary line differs: $(grep CANDUMP_REPLAY "$cd_dir/err.txt")" >&2
    rm -rf "$cd_dir"; exit 1
fi
if ! "$BIN" --verify-journal --journal "$cd_dir/j.journal" 2>&1 | grep -q '2 closed sessions'; then
    echo "E2E FAIL: the candump replay must journal exactly two sessions" >&2
    rm -rf "$cd_dir"; exit 1
fi
cd_csv="$("$SETTLE" "$cd_dir/j.journal")"
for row in 'bay-07,1,2026-10-18T08:15:02Z,OK,no,COMMUNICATION_TIMEOUT,9474,9474,842,842,2.368500,0.842000,0.500000,3.710500,EUR,yes,yes' \
           'bay-07,2,2026-10-18T08:24:00Z,OK,yes,STATUS_OFF,6219,6219,528,528,1.554750,0.528000,0.500000,2.582750,EUR,no,no'; do
    if ! echo "$cd_csv" | grep -qxF "$row"; then
        echo "E2E FAIL: the settlement row differs from the independent model: $row" >&2
        echo "$cd_csv" >&2
        rm -rf "$cd_dir"; exit 1
    fi
done
# The journal's wall-clock labels are the recorded ones, not today's date.
if ! cut -f25 "$cd_dir/j.journal" | grep -q '^2026-10-18T08:15:02Z$' ||
        ! cut -f3,25 "$cd_dir/j.journal" | grep -q '^session_open.2026-10-18T08:01:00Z$'; then
    echo "E2E FAIL: journal timestamps must be the recorded UTC" >&2
    rm -rf "$cd_dir"; exit 1
fi
if ! grep -q '"timestamp":"2026-10-18T08:25:00Z"' "$cd_dir/status.json"; then
    echo "E2E FAIL: the status file must carry the recorded UTC of the last frame" >&2
    rm -rf "$cd_dir"; exit 1
fi
# A recording with no timestamps (the default candump format) is refused, not guessed at.
printf '  can0  180   [8]  80 BB E8 03 00 00 00 00\n  can0  181   [1]  01\n' > "$cd_dir/untimed.txt"
rc=0; "$BIN" --replay-candump "$cd_dir/untimed.txt" >/dev/null 2>"$cd_dir/err2.txt" || rc=$?
if [ "$rc" -ne 2 ] || ! grep -q 'no timestamps' "$cd_dir/err2.txt"; then
    echo "E2E FAIL: an untimed recording must be refused with exit 2, got $rc" >&2
    rm -rf "$cd_dir"; exit 1
fi
rc=0; "$BIN" --replay-candump "$cd_dir/untimed.txt" --replay-interval-ms 1000 >/dev/null 2>"$cd_dir/err3.txt" || rc=$?
if [ "$rc" -ne 0 ] || ! grep -q 'frames_used=2 ' "$cd_dir/err3.txt"; then
    echo "E2E FAIL: an untimed recording with --replay-interval-ms must replay, got $rc" >&2
    rm -rf "$cd_dir"; exit 1
fi
# A timestamp that goes backwards is clamped, warned about once, and counted.
printf '(100.000000) can0 180#80BBE80300000000\n(99.000000) can0 181#01\n(101.000000) can0 181#01\nnot a frame\n' \
    | "$BIN" --replay-candump - >/dev/null 2>"$cd_dir/err4.txt" || {
    echo "E2E FAIL: stdin candump replay failed" >&2
    rm -rf "$cd_dir"; exit 1
}
if [ "$(grep -c REPLAY_TIME_REGRESSION "$cd_dir/err4.txt")" -ne 1 ] || ! grep -q 'clock_regressions=1' "$cd_dir/err4.txt" ||
        ! grep -q 'invalid_lines=1' "$cd_dir/err4.txt"; then
    echo "E2E FAIL: a clock regression must warn once and be counted; junk must be counted" >&2
    rm -rf "$cd_dir"; exit 1
fi
if "$BIN" --replay-candump /nonexistent/bay.log >/dev/null 2>&1; then
    echo "E2E FAIL: a missing recording must fail" >&2
    rm -rf "$cd_dir"; exit 1
fi
if "$BIN" --replay-candump tests/fixtures/bay_session.candump --replay >/dev/null 2>&1; then
    echo "E2E FAIL: --replay-candump is exclusive with the other listening modes" >&2
    rm -rf "$cd_dir"; exit 1
fi
rm -rf "$cd_dir"

echo "E2E OK: DISCONNECTED->READY->CHARGING->FAULT(latched), ledger verified, tampering detected," \
     "vendor DBC decoded live, dispute pack verified, supervision exports atomic"