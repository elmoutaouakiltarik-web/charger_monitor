# Time-of-use tariff examples (`charger.tou.v1`)

> **The rates in these files are placeholders, not market data.** They exist so
> `charger_settle --check-tou` and `--tou-settle` can be exercised end to end
> and so the exactness of the window split is testable. Replace every rate with
> the contracted supplier tariff (or the ARERA reference for your use case)
> before any settlement analysis. The engine never invents a rate: a missing or
> misspelled field is a startup error, and an uncovered second of the day is a
> refusal, not a zero price.

## Files

| File | Shape |
| --- | --- |
| `tou_three_window_example.json` | Three windows covering the full 24 h (night / day / evening). No fallback needed. |
| `tou_day_fallback_example.json` | One day window plus a catch-all `NIGHT` fallback pricing everything else. |

## Validate (refuses holes and overlaps, exit 2)

```bash
./build/charger_settle bay.journal --check-tou charger_project/examples/tou/tou_three_window_example.json
./build/charger_monitor --print-tou   # blank template to fill with real rates
```

## Settlement estimate (constant-power apportionment, labelled, never invoiced)

```bash
./build/charger_settle bay.journal --tou-settle charger_project/examples/tou/tou_three_window_example.json --summary
```

The exact ledger totals stay the invoice of record. The `tou_estimate_eur`
column answers "what would this session have cost had it run entirely under
this windowed tariff, assuming constant power" — the input to night-load
shifting decisions. Sessions whose `closed_at_utc` is not parseable UTC are
listed as `refused`, not estimated.

## Writing a real tariff

- Times are `HH:MM` UTC, `end < start` wraps past midnight, `end == start` is
  degenerate and rejected.
- Rates are decimal strings with at most 6 fractional digits, parsed without a
  binary float round trip.
- Every second of the day must be covered by a window or the fallback;
  `tou_coverage()` reports `covered_s` / `uncovered_s` out of 86400.
- Slices are labelled by the second they start in: energy flowing during
  07:59:59 belongs to the window containing 07:59:59, even if the slice ends
  at 08:00:00.
