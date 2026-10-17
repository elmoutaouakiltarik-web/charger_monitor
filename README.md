# charger_monitor

Deterministic monitoring library for industrial DC charger bays.

It listens to a CAN bus, reconstructs an **exact** charging-session ledger,
persists it in a **durable, tamper-evident journal**, and rolls it up into
**fleet KPIs**. It never commands the power stage.

[![CI](https://github.com/OWNER/charger_monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/charger_monitor/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/c%2B%2B-17-blue.svg)](CMakeLists.txt)
[![Dependencies](https://img.shields.io/badge/dependencies-none-success.svg)](CONTRIBUTING.md)

> **This is not firmware.** Not a certified controller, not a safety function,
> not a legal or fiscal meter (MID). See [SECURITY.md](SECURITY.md) for the
> deliberate non-goals before you deploy anything.

## Why this exists

Most charger telemetry answers "what is the state right now" and then loses the
interesting part. The interesting part is: **what was delivered, what it is
worth, and can I prove it after the process restarts**.

This library treats the ledger as the product:

| Problem | Approach |
| --- | --- |
| Energy with floating-point drift | Trapezoidal integration in **microwatt-hours** with 128-bit integer arithmetic and a carried remainder. 480 V × 10 A × 1 h = exactly 4 800 Wh |
| A dropout silently billed as energy | Dropouts are never interpolated. They produce `MEASUREMENT_GAP`, and the session is flagged `energy_lower_bound` |
| A restart losing billing data | Append-only **hash-chained journal**, fsync on commit, replayed on startup. Sessions opened but never closed are reported as orphans and are **not billable** |
| Disputed invoices | `sha256_hex()` is exported and the line layout is documented, so any auditor can recompute the chain with standard tools |
| Guesswork about when to invoice | `billing_complete` is true only when the data is complete. Everything else is held for review |
| "Which bay earns?" | Fleet rollup across assets, rebuildable from a journal with no running process |
| Finding a failing connector before it fails | Predictive diagnostics that raise advisories and **never block charging** |

**Zero external dependencies.** Everything is the C++17 standard library. An
industrial operator has to audit what it ships, and a dependency tree is a
supply-chain liability.

## Quick start

```bash
cmake -S charger_project -B build -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# One hour at 480 V / 10 A priced at 0.25 EUR/kWh + 3.60 EUR/h + 0.50 EUR flat
printf '180#80BBE80300000000\n181#02\n' > frames.txt
# (interleave the measurement every second with --replay-interval-ms 1000)

./build/charger_monitor --replay --journal bay.journal --replay-interval-ms 1000
./build/charger_monitor --verify-journal --journal bay.journal
./build/charger_monitor --portfolio bay.journal
```

## Use it as a library

```cmake
find_package(charger_monitor REQUIRED)
target_link_libraries(your_target PRIVATE charger::core)
```

A complete downstream project is in
[`examples/consume_library`](charger_project/examples/consume_library). CI builds
and runs it after installing the package, so the exported target cannot rot.

```cpp
#include <charger_monitor.hpp>

charger::Config cfg;
cfg.tariff.energy_micro_eur_per_kwh = 250000;   // 0.25 EUR, exact integers
charger::Monitor monitor(cfg);

const auto snapshot = monitor.snapshot();
if (snapshot.last_session && snapshot.last_session->billing_complete) {
    invoice(snapshot.last_session->total_cost_micro_eur);
}
```

Four independent headers, each usable on its own:

- `charger_monitor.hpp` — state machine, session ledger, tariff
- `charger_portfolio.hpp` — fleet rollup
- `charger_ledger.hpp` — durable journal, recovery, verification
- `charger_config.hpp` — versioned JSON configuration

## The billing contract

Energy is integrated only between consecutive accepted in-session samples. If
the interval exceeds `max_integration_step`, **no energy is added** and the
session is marked as a lower bound.

`billing_complete` is true only when the session closed cleanly, carried at
least two samples, had no integration gap and was not cut by a timeout.

| Field | Meaning |
| --- | --- |
| `energy_uwh` | Microwatt-hours integrated, as an integer string |
| `energy_wh` | Wh register, floored from uWh |
| `billed_energy_wh` | Energy after the OCPI `step_size` ceiling |
| `time_upper_bound` | True when charging was cut by a dropout: the billed time is an upper bound |
| `billing_complete` | The only field an automated invoice should trust |

Pricing follows an OCPI 2.2.1 subset: `ENERGY`, `TIME` and `FLAT`, with
`step_size` rounding and VAT omitted (which is **not** zero-rated VAT).
`PARKING_TIME` is deliberately **not** priced: this protocol cannot prove
connector occupancy while not charging, and inventing it would produce wrong
invoices.

## Deployment

```bash
charger_monitor can0 --config bay-07.json --journal /var/lib/charger/bay-07.journal
```

The JSON config is the base; CLI flags override it. A misspelled or malformed
field fails startup with its path rather than defaulting to zero, because a
silently-zero tariff invoices nothing and nobody notices.

On restart the journal is replayed: counters are restored, orphans are reported
on stderr, and a broken chain is surfaced instead of being ignored.

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md) — the five ground rules and the pre-merge checks
- [SECURITY.md](SECURITY.md) — deliberate non-goals and what counts as a vulnerability
- [CHANGELOG.md](CHANGELOG.md) — release history
- [charger_project/README.md](charger_project/README.md) — CLI reference, protocol, diagnostics, schema fields

## Verification

CI runs on every push:

| Job | What it proves |
| --- | --- |
| build (gcc + clang, `-Werror`) | No warnings under two compilers |
| sanitizers (ASan + UBSan) | No memory errors or undefined behaviour |
| coverage (gcovr) | Line and branch coverage of `src/` |
| fuzz (libFuzzer, 20 s) | No crash on malformed frames, tariffs or journal rows |
| end-to-end replay | The documented state sequence and ledger contract |
| journal tamper detection | A modified journal fails verification with a non-zero exit |
| consumable package | An external project can `find_package` and link it |
| clang-format | Formatting matches `.clang-format` |

## License

Apache-2.0. See [LICENSE](LICENSE).

Apache-2.0 rather than MIT because the patent grant is what unblocks adoption
in industrial products.