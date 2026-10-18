# Charger Control Room

A read-only, offline control room for `charger_monitor`: **one HTML file**, no
server, no dependencies, no telemetry.

- **Use it:** open [`charger-control-room.html`](charger-control-room.html) in a
  browser and press *Demo shift*, or drop a journal on it. Full guide:
  [`docs/DASHBOARD.md`](../docs/DASHBOARD.md).
- **What is here:**

| Path | What it is |
| --- | --- |
| `charger-control-room.html` | the built, committed single-file bundle (CSP-pinned script, embedded synthetic demo) |
| `src/core.js` | every rule: SHA-256, byte-exact chain verification, BigInt roll-up, price recalculation, anchors, prefix comparison. No DOM; runs under Node |
| `src/app.js`, `charts.js`, `dom.js`, `i18n.js`, `style.css` | the interface (English and Italian) |
| `data/` | the **synthetic** demo journal, anchor and capture, produced by `charger_synth` |
| `tests/` | `core.test.mjs` (parity with the C++ tools), `ui.test.mjs` (browser), goldens |

- **Rebuild:** `python3 scripts/build_dashboard.py` (and `--check` to see if the
  committed bundle is stale). Demo data and goldens: `bash dashboard/tests/regen_golden.sh build`.
- **Rule of this directory:** it must never gain a way to write to a charger, a
  bus or a server, and the journal rules here must change together with the C++
  library (see [CONTRIBUTING](../CONTRIBUTING.md), rule 8).
