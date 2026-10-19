# Charger Monitor — C++17 CLI and core library (v2.0.0)

Bay monitor for industrial DC chargers. It listens on the bus and never
commands the power stage.

## What this adds

A frame parser is worth nothing on its own. The value is in four things an
operator can invoice, defend and act on:

1. **A journal that is neither lost nor altered.** The ledger is append-only,
   every line is chained with SHA-256 and fsync-ated. A restart mid-session does
   not lose billing data, and sessions left open are reported as orphans.
2. **Site-level KPIs.** Which bays earn, which break? `--portfolio` rebuilds
   revenue, energy and per-asset figures from the journal alone.
3. **Diagnostics that prevent downtime.** Stuck sensor, voltage sag under load,
   current hunting: advisories raised before the connector overheats.
4. **A verifiable session contract.** Exact integer energy, cost per OCPI 2.2.1,
   and `billing_complete` stating whether the data may be invoiced or must be
   held for review.

## Safety and metrology boundary

This project is **not** firmware, **not** a certified controller, **not** a
safety function and **not** a fiscal or legal meter (MID).

- It does not command the power stage. It does not replace interlocks, contactor
  logic, insulation monitoring or emergency stop.
- Physical faults are not resettable from software: `reset_fault()` requires an
  authorized, independent procedure. Protected codes are always denied.
- Energy figures are an operational record, not a legal measurement.
- The hash chain detects casual edits and truncation. It is **not** proof of
  non-repudiation: rewriting the whole file from the genesis block still
  produces a valid chain. Non-repudiation needs the CSMS signature (OCPP) or a
  site PKI.
- IEC 61508 / ISO 26262 / IEC 62443 remain to be assessed through a formal
  process and a qualified reviewer. No compliance is claimed here.

See [../SECURITY.md](../SECURITY.md) for what counts as a vulnerability.

## Demo protocol (or the manufacturer DBC, see below)

- ID `0x180`, 8 bytes: voltage in centivolts in bytes 0–1 (unsigned 16-bit
  little-endian), current in centiamps in bytes 2–3, bytes 4–7 reserved and
  required to be zero.
- ID `0x181`, 1 byte: `0=OFF`, `1=READY`, `2=CHARGING`, `3=FAULT`.
- Demo ranges: 0–500 V, 0–200 A. Example values only.

## Build and test (Ubuntu/WSL2)

```bash
sudo apt update
sudo apt install -y build-essential cmake can-utils
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Presets: `default`, `ci`, `asan`, `fuzz` (`cmake --list-presets`).

Other checks CI runs:

```bash
bash scripts/e2e_replay.sh      # state sequence, ledger contract, tamper detection
bash scripts/consume_test.sh    # a third-party project can find_package and link us
```

The CAN live path requires SocketCAN (Linux). On Windows the library, the tests,
simulation and replay build and run; live listening is explicitly refused.

## Simulation and replay

```bash
./build/charger_monitor --simulate
printf '180#80BBE80300000000\n181#01\n181#02\n181#00\n' | ./build/charger_monitor --replay
```

`--replay-interval-ms N` advances a simulated clock per line, which is the only
way to exercise energy and pricing from the command line.

## Deployment

```bash
./build/charger_monitor can0 \
  --config bay-07.json \
  --asset-id bay-07 \
  --journal /var/lib/charger/bay-07.journal \
  --tariff-eur-per-kwh 0.25 \
  --tariff-eur-per-hour 3.60 \
  --flat-fee-eur 0.50 \
  --energy-step-wh 10 \
  --time-step-s 60
```

Generate a configuration template with `--print-config`. The file is the base
and CLI flags override it.

On restart the journal is replayed: counters are restored, orphans are reported
on stderr, and a broken chain is surfaced instead of being ignored.

```bash
./build/charger_monitor --verify-journal --journal bay-07.journal   # exit 0 intact, 3 altered
./build/charger_monitor --anchor-journal --journal bay-07.journal --anchor-file bay-07.anchor   # pin the prefix
./build/charger_monitor --verify-anchor --journal bay-07.journal --anchor-file bay-07.anchor    # exit 3 if rewritten
./build/charger_monitor --portfolio bay-07.journal                  # fleet KPI
./build/charger_settle bay-07.journal --dispute bay-07 12           # dispute evidence pack
./build/charger_settle bay-07.journal --cdr bay-07 12               # OCPI CDR subset (charger.cdr.v1)
./build/charger_settle bay-07.journal --check-tou tou.json          # refuse holes and overlaps (exit 2)
./build/charger_settle bay-07.journal --tou-settle tou.json --summary  # exact totals + labelled TOU estimate
```

Verified by the test suite: 480 V, 10 A, 1 h, priced at 0.25 EUR/kWh +
3.60 EUR/h + 0.50 EUR flat = **5.30 EUR** excluding VAT, with 4 800 Wh of
exactly integral energy.

## Journal line layout

```
prev_hash \t hash \t type \t asset \t session_id \t energy_uwh \t duration_ms \t
cost_micro_eur \t billable \t stop_reason \t undersampled \t json_payload
```

`hash = SHA256(prev_hash "\t" <everything after the hash field>)`.

The tab-delimited prefix exists so recovery needs no JSON parser. The JSON tail
is there for auditors. `sha256_hex()` is exported, so the chain can be
recomputed with any standard tool.

The close line also carries the **tariff of record** (exact integer micro-EUR
prices plus step sizes). This is what makes `charger_settle --dispute` work:
the pack combines meter values, the tariff that was in force, the calculation
and the chain provenance (`open`/`close` line numbers, `close_prev_hash`,
`close_hash`, the raw close line, and the digest of the whole journal file),
with a verification recipe readable without our binaries.

## Vendor DBC adoption

A real fleet speaks the OEM's protocol, not the demo one. Hand a vendor
`.dbc` to the monitor instead of inventing identifiers:

```bash
./build/charger_monitor --replay < frames.txt \
    --dbc vendor.dbc --voltage-signal DC_Voltage --current-signal DC_Current
./build/charger_monitor --dump-dbc --dbc vendor.dbc    # inspect what was parsed
```

Binding rules, enforced before any byte is billed:

- measurement signals must be exactly 16 bits, unsigned, zero-offset, and
  byte-aligned (Intel or Motorola), so the codec maps them bit-exactly;
- the factor must be representable as an exact rational scale (`0.01` →
  `num=1/den=100`), never approximated;
- the status carrier must be a 1-byte message whose `VAL_` table maps to at
  least two distinct meanings (OFF/READY/CHARGING/FAULT); negated labels such
  as "not charging" are refused rather than guessed;
- anything else aborts startup with the reason: a guessed decode of a billing
  channel silently invoices wrong numbers.

The DBC reader is validated bit-exactly against cantools 45.x. The bit layout
was checked for both endiannesses at every start position 0..63 and every
length from 1 to 64 bits, including Motorola signals that start in the middle
of a byte; the test suite pins the byte-aligned, mid-byte and signed-with-offset
cases with payloads cantools itself produces.

## Time-of-use windows

An hour of charging between 02:00 and 03:00 is not worth the same as the same
hour between 17:00 and 18:00. A single flat rate cannot express that, and a
session that crosses a window boundary is one of the most common reasons a
settlement is disputed.

`charger_tou.hpp` splits one session into `ChargingPeriod` stretches and prices
each one at its own rate:

```cpp
charger::TimeOfUse tou;
tou.windows.push_back({"OFFPEAK", 0, 8 * 3600, 150000, 2000000});
tou.windows.push_back({"PEAK", 8 * 3600, 20 * 3600, 400000, 3000000});
tou.fallback = charger::TimeWindow{"FLAT", 20 * 3600, 8 * 3600, 250000, 2500000};

const charger::TimeOfUseResult r =
    charger::price_time_of_use(tou, samples, 250000, 1, 1);
```

Guarantees:

- windows are UTC; `end < start` wraps past midnight, and `end == start` (a
  degenerate window) matches nothing, which `TimeOfUse::validate()` rejects;
- `TimeOfUse::validate()` also rejects out-of-range windows, negative rates and
  two windows covering the same second, which would otherwise be priced at
  whichever came first. Call it on any tariff you did not assemble yourself;
- intervals are walked in one-second slices, so a boundary inside a sample
  interval splits it exactly instead of billing the whole interval at whichever
  rate happened to be current when it started;
- each slice receives the difference between the cumulative apportionment and
  the one before it, so the slices of an interval sum back to the interval
  total exactly. Flooring each slice separately would leak up to one
  microwatt-hour per slice, and `total_energy_uwh` always equals the sum of
  `periods[].energy_uwh`;
- an interval whose energy register ran backwards cannot be priced, so its
  duration is reported in `dropped_ms` rather than silently disappearing;
- `unpriced_ms` counts the seconds that no window and no fallback covered: such
  time is priced at **zero**, which is why the field exists. A caller that
  ignores it will under-bill, so check it before settling;
- a cost that does not fit in 64 bits saturates and raises `price_overflow`. It
  is never truncated and never left at zero, because free energy is the worst
  possible direction to fail in on a billing channel;
- `flat_cost_micro_eur` is returned next to the windowed total, so the effect of
  a TOU tariff is measured rather than asserted.

The caller supplies the UTC second of day for each sample, which keeps the core
clock-injected and deterministic. This is a library component: the monitor's own
billing path prices a session at one rate, so apply TOU in `charger_settle` or
in your backend rather than expecting `--replay` to use it.

## Session model

| Moment | Behaviour |
| --- | --- |
| Entering `CHARGING` | Opens the session, seeds the current sample without billing the pre-session interval |
| Accepted sample | Integrates the trapezoid, updates peak power, min V and max I |
| Leaving `CHARGING` | Closes the session, computes duration and cost, records the `stop_reason` |
| Timeout | Closes with `COMMUNICATION_TIMEOUT`, `time_upper_bound = true` |
| Fault | Closes with the fault code, keeping the energy already accepted |

`stop_reason` uses stable codes: `STATUS_OFF`, `STATUS_READY`,
`COMMUNICATION_TIMEOUT`, or the fault code. They are comparable with
`TransactionEvent.stoppedReason` in OCPP 2.0.1 but remain independent of it.

`PARKING_TIME` is **not** computed: this protocol cannot prove that the
connector is occupied while not charging. An OEM profile that exposes connector
state must add that signal before an idle fee can be applied.

## Predictive diagnostics

All disabled by default: thresholds are installation-specific and must come
from real electrical data, never guessed.

| Signal | Configuration | Meaning |
| --- | --- | --- |
| `MEASUREMENT_STUCK` | `stuck_at_ms` | Measurements frozen at constant load: sensor or wiring |
| `VOLTAGE_RATE_EXCEEDED` | `max_voltage_rate_mv_per_s` | Voltage step beyond the plausible slope |
| `CURRENT_RATE_EXCEEDED` | `max_current_rate_ma_per_s` | Current step beyond the plausible slope |
| `VOLTAGE_SAG_UNDER_LOAD` | `sag_voltage_mv`, `sag_tolerance_mv` | Collapse under load: connector, fuses, contactor |
| `CURRENT_HUNTING` | `current_hunt_band_ma` | Repeated oscillation: unstable contact or arcing |

A diagnostic **never blocks charging**: halting a charge on a heuristic is not
safe. Each raises a warning, increments a counter and appears in
`snapshot.diagnostics`.

## JSON contract

Amounts and energy are integer **strings** so consumers cannot lose precision
beyond 2^53. `vat` is `"omitted"`, which is not "zero-rated VAT".

- `energy_uwh` — microwatt-hours integrated.
- `energy_wh` — Wh register, floored from uWh.
- `billed_energy_wh` — billable energy after the OCPI `step_size` ceiling.
- `total_cost_micro_eur` — total excluding VAT.
- `billing_complete` — true only when the data is complete and is not a bound.
- `availability_ppm` / `utilization_ppm` — `-1` means undefined (no in-service
  time), not zero percent.
- Unknown KPI values are `null`, never a negative number.

Schemas: `charger.snapshot.v1`, `charger.session.v1`, `charger.ledger.v1`,
`charger.portfolio.v1`, `charger.config.v1`. They are a compatibility contract:
adding fields is fine, renaming or retyping is a breaking change.

## Library reuse

Four independent headers, each usable on its own:

- `charger_monitor.hpp` — state machine, session ledger, tariff
- `charger_portfolio.hpp` — fleet rollup
- `charger_ledger.hpp` — durable journal, recovery, verification
- `charger_config.hpp` — versioned JSON configuration

```cmake
find_package(charger_monitor REQUIRED)
target_link_libraries(your_target PRIVATE charger::core)
```

The core is deterministic: `ingest`, `tick`, `snapshot` and `reset_fault` accept
a `Clock::time_point`, so it is reproducible in SIL, replay and regression tests
without a wall clock.

For a different OEM, change only `ProtocolProfile`:

```cpp
Config cfg;
cfg.protocol = ProtocolProfile::bare();
cfg.protocol.measurement_id = 0x200;
cfg.protocol.status_id = 0x201;
cfg.protocol.voltage = {1, 10};   // rational scale: SI = raw * num / den
cfg.protocol.current = {1, 10};
cfg.protocol.little_endian = false;
cfg.protocol.status_map[9] = kStatusCharging;
```

## Production gaps (close before real hardware)

1. Replace the demo protocol with the OEM specification: endianness, scaling,
   counters, CRC and alive signals.
2. Define electrical limits and plausibility checks with the electrical
   engineer.
3. Design an authorized, independently verified fault-reset procedure.
4. Add bus-off recovery, error-frame policy and interface supervision.
5. Integrate the real HMI transport and schema; stdout JSON is a demo output.
6. Add persistent structured logging, rotation and access controls.
7. Add coverage, hardware-in-the-loop tests and integration tests.
8. Complete hazard analysis and applicable standards work.

## Standards consulted for the tariff model

- **OCPI 2.2.1**, Tariffs module: the four tariff dimensions and the
  `step_size` rounding semantics (ceiling to multiples).
- **OCPP 2.0.1**, `TransactionEvent`: the Started/Updated/Ended transition model
  with `stoppedReason` and imported-energy measurands.
- **NIST ISO 23247**: separating the model, its synchronised data and its
  business purpose.

The implemented model is a declared subset: no VAT, no restrictions, no
`PARKING_TIME`. The billing path prices a session at one rate; time-of-use is a
separate opt-in module (see [Time-of-use windows](#time-of-use-windows)) that
the caller applies.

## Notes

- Monotonic time governs timeouts and durations; UTC is a log label only.
- A fault stays latched: no automatic recovery towards charging.
- `stale_timeout` exists for source compatibility; staleness uses the per-stream
  `measurement_timeout` and `status_timeout`.
- The library requires a 128-bit integer type, so GCC or Clang.
- Style and CI: `.clang-format`, `.editorconfig`, `clang-format --dry-run
  --Werror`, GCC and Clang builds with `-Werror`, sanitizers, coverage and a
  20 s libFuzzer run.