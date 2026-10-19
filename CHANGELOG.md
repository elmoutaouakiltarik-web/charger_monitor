# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- File-based TOU tariff (`charger.tou.v1`): `parse_tou_json()` loads windows
  plus an optional catch-all fallback from a reviewable document, decimal
  rates parsed without a float round trip, unknown fields refused.
  `tou_coverage()` reports covered/uncovered seconds of the 86400-second day
  and `price_time_of_use_strict()` refuses to price an incomplete tariff
  instead of billing the hole at zero. CLI: `charger_monitor --print-tou`,
  `charger_settle --check-tou FILE` (exit 2 on holes or overlaps) and
  `--tou-settle FILE`, which adds a labelled constant-power `tou_estimate_eur`
  column for night-load analysis while the exact ledger totals stay the only
  invoice of record.
- External journal anchor (`charger.anchor.v1`): `write_journal_anchor()` pins
  the journal prefix (whole-file digest, `last_hash`, line count, UTC time)
  into a separate file for append-only storage, refusing to anchor a broken
  chain; `verify_journal_anchor()` proves the prefix was not rewritten while
  reporting lines appended since the anchor. CLI: `charger_monitor
  --anchor-journal` / `--verify-anchor` (exit 3 on mismatch).
- OCPI CDR subset exporter (`charger.cdr.v1` via `json_cdr()` and
  `charger_settle --cdr ASSET SESSION_ID`): session start labelled
  `derived_start_utc`, VAT `omitted`, tariff of record and chain provenance
  embedded. Signed exchange stays in the backend.
- Field guide (`docs/FIELD_GUIDE.md`, in Italian): commissioning checklist,
  acceptance test, dispute SOP with auditor shell commands, TOU and anchor
  procedures, parametric revenue/dispute economics without invented tariffs,
  and example TOU files with placeholder rates in
  `charger_project/examples/tou/`.
- ChainedAnchor session anchor (`charger.anchor.v1`): `ChainedAnchor::to_json()`
  serialises the journal provenance record (schema, timestamp, digest,
  previous-anchor hash, signature) into a verifiable JSON document for dispute
  packs and audit trails.
- Multi-bay configuration (`charger_config.hpp`): `TimeOfUse::validate()` rejects
  degenerate, out-of-range or negative-rate windows, and `price_time_of_use()`
  splits sessions across window boundaries with an exact integer remainder,
  returning `TimeOfUseResult` with per-window `ChargingPeriod` entries and an
  `unpriced_ms` hole counter.

### Documentation

- `scripts/verify.sh` runs the whole local verification gate in one command and
  prints `PASS`/`FAIL`/`SKIP` per step with counts at the end. It mirrors the CI
  jobs that need no special toolchain, and reports a missing tool as `SKIP`
  rather than passing it silently.
- The pull request template now asks for the `verify.sh` summary as evidence
  instead of a list of checkmarks, and CONTRIBUTING states the merge policy: a
  real title, no template comments left in the body, no merge with failing
  checks, no unreviewed self-merge of billing-path changes.

### Fixed

- **A journal write that did not land was reported as recorded.**
  `append_session_open()` and `append_session_close()` returned `void` and
  discarded the result of `append()`, and the close path pushed the session into
  the in-memory `sessions_` regardless. A full disk or a revoked permission
  therefore produced a process that reported a settlement the file did not
  contain, and a portfolio rollup could bill on it. Both now return `bool`, the
  caller checks it, and the session enters `sessions_` only when the line is on
  disk. The monitor additionally skips the JSONL session log when the journal
  close failed, so the two records cannot disagree. **Source-compatible** with
  existing callers, which may keep ignoring the result.
- **A partial journal write left a torn line for the next append to build on.**
  A short `fwrite` returned an error but kept the bytes it had already written,
  so the following append started after a partial row and the chain could never
  verify again. The append now records where the valid journal ends and truncates
  back to exactly that point on failure, leaving the file verifiable and no valid
  data removed. A failed write leaves the journal byte-identical.
- **Durability was assumed, not checked.** `sync_file()` ignored the results of
  `fflush()`, `fsync()`/`_commit()` and the caller's `fclose()`, so a journal
  line could be reported as committed when the flush or the sync had failed. All
  three are now checked, the sync runs only after a successful flush, and a line
  that cannot be made durable is rolled back and reported as unwritten.
- `.gitattributes` was missing, so with `core.autocrlf=true` a Windows checkout
  rewrote the shell scripts to CRLF and `bash` refused to run them
  (`set: pipefail^M: invalid option name`). Line endings are now pinned per file
  type instead of left to each developer's configuration.
- `charger_synth_tests` shelled out through `std::system()`, which hands the
  command to `cmd /c` on Windows; the shell re-parsed the quoting and the run
  failed with a filename syntax error before the synthesiser did anything. It now
  spawns the binary directly with an argument vector.
- `charger_export_tests` held an `ifstream` open over a file it then deleted,
  which succeeds on Linux and fails on Windows where the handle locks the file.
- `charger_ocpp.cpp`, `charger_tou.cpp` and `charger_tou.hpp` were not
  clang-format clean, so the format job in CI would have failed.
- TOU slices were labelled by the second they end in, moving the second that
  ends exactly on a window boundary into the next window (a systematic
  one-second misallocation per crossing). Slices are now labelled by the
  second they start in; the pinned boundary test vectors were recomputed
  exactly (7200 s / 3600 s instead of 7199 s / 3601 s).

## [1.4.0]

### Added

- **Vendor DBC adoption** (`charger_dbc.hpp`). A real fleet is described by a
  Vector `.dbc` file, not by invented identifiers. The reader covers the subset
  a bay monitor needs (BO_/SG_/VAL_) including Motorola bit numbering, extended
  ids, value tables, and it cross-validated against cantools 45.x. The CLI
  validates and inspects it: `--dbc` validates the configured measurement id
  against the database, and `--dump-dbc` prints the parsed database.
- **DBC-driven decode** (`--voltage-signal` / `--current-signal` /
  `--status-signal`). When the operator names the signals, the wire layout
  (ids, byte offsets, endianness, exact rational scales, status map from the
  value-table labels) is built from the DBC and the monitor decodes the OEM
  protocol instead of the demo one. Anything the codec cannot decode exactly
  (non-16-bit fields, signed or offset measurements, overloaded ranges) aborts
  startup instead of guessing on a billing channel.
- **Dispute pack** (`charger_settle --dispute ASSET SESSION_ID`). One JSON
  document (`charger.dispute.v1`) with: the meter values as exact integers, the
  cost breakdown, the **tariff of record** embedded in the session close line,
  the chain provenance (open/close line numbers, `close_prev_hash`,
  `close_hash`, the raw close line), the digest of the whole journal file, and
  a step-by-step verification recipe recomputable with standard tools.
- **Time-of-use windows** (`charger_tou.hpp`), pricing each period of a session
  at its own rate with exact integer arithmetic. The apportionment carries a
  remainder, so the periods of an interval sum back to the interval total and
  `total_energy_uwh` always equals the sum of the parts. Time that no window
  covers is counted in `unpriced_ms` and priced at zero, so the caller must
  check it; an interval whose energy register ran backwards is reported in
  `dropped_ms`. `TimeOfUse::validate()` rejects a tariff that cannot be priced
  correctly (degenerate, out-of-range or negative-rate windows, and two windows
  covering the same second), and an unrepresentable cost saturates and raises
  `price_overflow` instead of reading as zero.
- Journal close lines now carry the tariff of record and the session JSONL
  log aligns, so a dispute can be verified years later against the tariff that
  was actually in force.

### Changed

- The CMake build links the GCC runtime statically on the executable targets,
  so a stray `libstdc++-6.dll` on `PATH` cannot change how a journal is read.
- The config schema is mandatory and truncated JSON documents are rejected
  outright (a power-interrupted write can no longer be read with only the
  fields that happened to parse, which silently meant zero pricing).

### Fixed

- Rounding to a `step_size` multiple saturates instead of wrapping: a wrapped
  billable register would silently invoice the wrong (smaller) amount.
- The price overflow guard is evaluated before the narrowing cast that the
  implementation-defined conversion could hide.
- DBC decoding could read past the end of a short payload. The bound now covers
  the whole bit run rather than only the field length, so a signal that starts
  high in a short frame is refused instead of read out of bounds.
- Motorola signals that do not start on a byte boundary were decoded with a
  different bit rule from the byte-aligned case, and disagreed with cantools.
  Both endiannesses now use one rule, verified against cantools for every start
  position and length, and the mid-byte case is covered by a test.
- A `VAL_` table on an extended-id message was dropped silently, because the id
  was not stripped of the extended flag the way `BO_` does.
- A vendor file declaring voltage and current in different byte orders was
  decoded using the voltage signal's order for both, yielding a plausible wrong
  current. Mixed byte orders are now refused.
- A time-of-use cost that did not fit in 64 bits was recorded as zero, which
  reads as free energy. It now saturates and raises `price_overflow`.
- Time-of-use apportionment floored each one-second slice independently and
  lost up to one microwatt-hour per slice, so the parts no longer summed to the
  interval total while `total_energy_uwh` was computed separately. Both are now
  derived from the same carried remainder.
- A time-of-use interval whose energy register ran backwards was dropped
  silently, leaving the reported periods short of the session span. Its duration
  is now reported in `dropped_ms`.
- A journal `session_open` line inherited the previous session's tariff of
  record, because the context was cleared for currency and timestamp but not for
  the tariff. The chain still verified, so it was silent.
- Dispute provenance for a session id reused after a restart pointed at the
  first run's opening line. The open-line entry is now replaced on open and
  erased on close.
- **Chained journal anchor** (`charger.anchor.v1`): `write_journal_anchor()` pins
  the journal prefix (whole-file digest, last hash, line count, UTC time) into a
  separate append-only file; `verify_journal_anchor()` proves the prefix was not
  rewritten while reporting lines appended since the anchor. CLI:
  `charger_monitor --anchor-journal` / `--verify-anchor` (exit 3 on mismatch).
- **Webhook streaming**: `charger_monitor --webhook-url http://host:port/path`
  posts session open/close and billing events as HMAC-signed JSON to a listener.
  No TLS in this build (use a local reverse proxy for HTTPS).
- **Multi-bay monitor** (`charger_monitor.hpp`): `MultiBayMonitor` manages
  multiple independent `Monitor` instances from one process, with per-bay config,
  hot-reload, fleet rollup, anomaly detection and per-bay billing.
- **Time-of-use tariff** (`charger_tou.hpp`): `price_time_of_use()` splits
  sessions across tariff windows with an exact integer remainder, so no
  microwatt-hour is lost at a window boundary. `price_time_of_use_strict()`
  refuses to price a tariff with uncovered seconds. CLI:
  `charger_settle --tou-settle FILE` adds a labelled `tou_estimate_eur` column.

### Documentation

- The repository card, the architecture map, the session lifecycle and the
  integer-exactness comparison are generated from `scripts/make_images.py`.
  Figures quoted in the README therefore come from the same source as the
  diagrams.

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

[Unreleased]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.4.0...HEAD
[1.4.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/releases/tag/v1.3.0
[1.2.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.2.0...v1.3.0
[1.1.0]: https://github.com/elmoutaouakiltarik-web/charger_monitor/compare/v1.1.0...v1.2.0