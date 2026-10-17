# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.3.0]

### Added

- **Durable hash-chained journal** (`charger_ledger.hpp`). Append-only,
  fsync-on-commit, SHA-256 chain with a documented line layout so an auditor can
  verify it with standard tools. Recovery restores lifetime counters and reports
  sessions opened but never closed as orphans, whose energy is explicitly not
  billable.
- **Fleet rollup** (`charger_portfolio.hpp`). Site-level revenue, energy,
  revenue per kWh, stop classification and per-asset ranking, rebuildable from a
  journal without a running process.
- **Predictive diagnostics**: `MEASUREMENT_STUCK`, `VOLTAGE_RATE_EXCEEDED`,
  `CURRENT_RATE_EXCEEDED`, `VOLTAGE_SAG_UNDER_LOAD`, `CURRENT_HUNTING`. All ship
  disabled, never latch a fault, and clear when charging stops.
- **Versioned JSON configuration** (`charger_config.hpp`). A file is the base
  and CLI flags override it. Unknown or malformed fields fail startup instead of
  silently defaulting to zero.
- **Consumable CMake package**: `find_package(charger_monitor)` with the
  `charger::` namespace and a pkg-config file.
- CLI: `--journal`, `--verify-journal`, `--portfolio`, `--config`,
  `--print-config`, `--replay-interval-ms`, and diagnostic thresholds.
- CI verifies journal tamper detection as a separate step.

### Changed

- User-facing messages are now English, matching the public documentation.
- The library requires a 128-bit integer type (GCC or Clang).

### Fixed

- File descriptor leak: `Ledger`'s destructor reopened the journal without
  closing it.
- `mean_power_mw` was wrong by a factor of 3600 in the portfolio rollup.
- Journal aggregation produced one row per session instead of per asset.
- `--verify-journal` reported zero lines because it never ran recovery.
- `revenue_per_kwh` printed `-0.001` from a wrong fixed-point scale.
- `--verify-journal` exited 0 on a tampered journal; it now exits 3 so an audit
  step in CI fails.
- Unknown KPI values are emitted as `null` rather than a negative number.

## [1.2.0]

### Added

- Exact integer session ledger: trapezoidal integration in microwatt-hours with
  128-bit arithmetic and a carried remainder.
- OCPI 2.2.1 subset pricing: ENERGY, TIME and FLAT with `step_size` rounding.
- `billing_complete`, `energy_lower_bound` and `time_upper_bound` so automation
  knows when a session may be invoiced.
- `ProtocolProfile` so a different OEM integrates without forking.
- JSONL session log and per-bay KPIs.

### Fixed

- Staleness used `stale_timeout` while the state machine used the per-stream
  timeouts, so state and stale flags could disagree.
- `reset_fault()` read the wall clock internally, making it untestable.
- `snapshot()` could report a stale state when `tick()` had not run.

## [1.1.0]

- Replay mode, libFuzzer harness, sanitizer builds, CMake presets.

[Unreleased]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.3.0...HEAD
[1.3.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/releases/tag/v1.3.0
[1.2.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.2.0...v1.3.0
[1.1.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.1.0...v1.2.0