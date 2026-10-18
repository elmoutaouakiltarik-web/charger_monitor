# Charger Control Room

A read-only, offline control room for `charger_monitor`. One HTML file, no
server, no dependencies, no telemetry. It verifies a journal's hash chain in
your browser, shows what may be invoiced and what is held for review, opens the
evidence behind any session, and watches live bays.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="dashboard-overview-dark.png">
    <img src="dashboard-overview-light.png" alt="Overview: chain verified, ready-to-invoice total, held sessions, energy per day, value per bay, heat map" width="900">
  </picture>
</p>

It is deliberately **a window, not a remote control**. There is no code path in
it that can command a charger, a bus or a server. That is the same boundary the
monitor keeps, and it is why you can put it in front of an auditor.

## Open it

| You have | Do |
| --- | --- |
| nothing yet | open [`dashboard/charger-control-room.html`](../dashboard/charger-control-room.html) and press **Demo shift** (a synthetic journal produced by the real C++ engine) |
| a journal | drop it on the page, or **Open journal…**; an anchor (`charger.anchor.v1`) or a captured snapshot stream can be dropped the same way |
| journals on a server | serve the page next to them and open `…/charger-control-room.html?journal=bay-07.journal` |
| a report from a colleague | open the saved `*-report.html`: it re-verifies its embedded journal when it loads |

The file is self-contained, so it works from a USB stick, by double-click, on an
air-gapped plant network, or attached to an e-mail.

## The five views

**Overview.** The chain verdict first, because nothing below it means anything
if the file was altered. Then the number finance wants: *ready to invoice*
(sessions with `billing_complete`), next to what is *held for review* and why.
Energy per day, value per bay, a UTC hour × weekday heat map of when energy is
delivered (an estimate: the journal stores totals, so each session is spread
over the hours it covered at constant power, with an integer carry so the slices
sum back to the total), and a "needs attention" panel that ranks held reasons
and lists orphan sessions. Every chart has a **Table** switch with the same data.

**Fleet.** One row per asset, rebuilt from the journal alone: sessions, billable,
held share, energy, value, charging time, mean power, fault stops, timeouts,
undersampled sessions and a per-day energy sparkline. Availability is not shown
on purpose: a list of sessions carries no in-service time. The live monitor
reports it (Live view).

**Sessions.** Every closed session, filterable by asset, status, reason, date
and free text; sortable; exportable as the same settlement CSV `charger_settle`
writes. Click a row for the evidence:

- the meter registers as exact integers (µWh, ms, samples, gaps);
- the cost broken into `ENERGY`, `TIME` and `FLAT` with the arithmetic shown
  (`12 610 Wh × 0.40 €/kWh`), from the **tariff of record** embedded in the
  close line;
- an **independent recalculation** of every derived number (Wh register, OCPI
  step ceilings, the three costs, the min/max clamp, the quality label and the
  `billing_complete` decision) against the meter registers, each with a ✓ or ✕;
- the chain provenance (open and close line numbers, `prev_hash`, hash);
- three shell lines an auditor can run **without any of our software**, and the
  `charger_settle --dispute` command for the full evidence pack.

**Integrity.** The chain verdict with the file's SHA-256, line counts, genesis
and last hash, and a strip with one cell per group of lines. When the chain is
broken it names the first bad line, says whether the record was *altered* (the
link is intact but the content no longer hashes to its recorded hash) or lines
were *removed, inserted or reordered*, and shows how many later lines are still
self-consistent, which is evidence about the shape of the damage and **never** a
reason to trust them: the trust boundary stays at the first bad line, exactly as
in `charger_monitor`. Orphan sessions are listed here. The anchor panel creates
`charger.anchor.v1` documents (byte-compatible with
`charger_monitor --anchor-journal`) and verifies a dropped anchor: the prefix is
intact, how many lines were appended since, or *the prefix was rewritten*.

<p align="center">
  <img src="dashboard-integrity.png" alt="A journal with one altered record: the chain is broken at line 41, only the verified prefix is shown" width="900">
</p>

**Live.** Polls status files written by `charger_monitor --snapshot-file` and
shows state, V / I / P, a power trend, active advisories, the session in
progress, availability and utilisation, and, most importantly, whether the file
itself is still being updated: *a dead monitor looks exactly like a healthy one
unless the page checks the file's age*. A **captured stream** (the monitor's
stdout, or a series of status files) can be replayed with a timeline of power
and machine state, which shows what an operator would have seen.

<p align="center">
  <img src="dashboard-live.png" alt="Replay of a captured day: power line, machine-state strip, and the live card at the selected frame" width="900">
</p>

## Live setup

The monitor opens no listening socket, so there is no endpoint to attack.
Instead it rewrites a file atomically, and any static web server can publish it:

```bash
charger_monitor can0 --config bay-07.json --journal /var/lib/charger/bay-07.journal \
    --snapshot-file /run/charger/status/bay-07.json
```

Serve the dashboard and the status directory **from the same origin** (browsers
only allow `fetch` from there; a page opened from `file://` cannot read other
files, but drag-and-drop and captures still work). A ready server block is in
[`deploy/nginx/charger-dashboard.conf`](../deploy/nginx/charger-dashboard.conf);
for a quick look, `python3 -m http.server` in a directory holding the page and a
`status/` folder is enough. List the files on the Live tab (one per line) or
open `…/charger-control-room.html?live=status/bay-07.json,status/bay-08.json`.

"File fresh" and "not updated for N s" compare the snapshot's own timestamp with
the browser clock, so a clock skew between the two shows up there. The 10 s
threshold is a display choice, not an engineering limit; the monitor refreshes
the file at least once per report interval, also while the bus is silent.

## Saving a report

**Save report** downloads one HTML file with the current journal embedded. The
code is byte-identical to the original and only a JSON data block changes, so
the page's script-hash pin still holds; opening it re-verifies the journal from
scratch on the recipient's machine. That is the point: you hand an auditor a
file that proves itself, rather than a PDF that asks to be trusted.
**Print** produces a clean paper copy of the current view.

## What it will and will not tell you

It checks the arithmetic against the tariff stored in the **same close line**.
It cannot tell whether that tariff was the contracted one, whether the meter was
right, or who wrote the file. A verified chain proves internal consistency; an
anchor proves the prefix was not rewritten since the anchor was made; neither is
a signature. See [COMPLIANCE.md](COMPLIANCE.md).

## Security model

- **No network.** The page makes no external request. A
  Content-Security-Policy pins the one inline script by its SHA-256
  (`default-src 'none'`; `connect-src 'self'`), so a script that is not
  byte-identical to what the builder emitted does not run, and the only
  requests the page can ever make go to its own origin.
- **No markup from data.** Journal and snapshot text reaches the page through
  text nodes only; there is no `innerHTML` in the code base. A browser test
  loads a journal whose asset id is `<img src=x onerror=…>` and checks that it
  is shown as text and executes nothing.
- **Exact numbers.** Energy and money are `BigInt` end to end. The SHA-256 is a
  streaming implementation fuzz-tested against Node's `crypto`, and the journal
  is read as raw bytes: it is never round-tripped through a text decoder.
- **Spreadsheet-safe export.** Text cells in the CSV that a spreadsheet would run
  as a formula are neutralised with a leading apostrophe. (For journals written
  by the C++ tools, whose asset ids are restricted to `[A-Za-z0-9._:-]`, the
  output is byte-identical to `charger_settle`.)
- **Size and speed.** A journal is parsed in memory, about 2 KB per session. The
  pure-JavaScript SHA-256 verifies roughly 15 MB/s end to end (measured in
  Node 22: a 4.4 MB journal with 2 281 sessions in 0.28 s), so a 100 MB journal
  takes several seconds and shows a progress bar; the page never freezes
  because the work is sliced.

Requirements: an evergreen browser from 2021 or later (BigInt, `replaceChildren`,
`ResizeObserver`).

## Tests

```bash
# 32 tests: SHA-256 vs node:crypto, byte-exact chain verification, six tamper
# scenarios, CRLF, anchors both ways, price re-derivation (clamps, step ceilings,
# overflow), CSV neutralisation, and — with a build tree — live parity against
# the C++ tools on five freshly synthesised journals.
CHARGER_BUILD_DIR=build node --test dashboard/tests/core.test.mjs

# 10 browser tests (Playwright + Chromium, skipped when not installed): CSP pin,
# demo numbers, altered record, evidence drawer, hostile text, anchors, the
# self-verifying report, live polling with a stale and a missing file, phone
# width on every tab, Italian + dark theme without missing strings.
node --test dashboard/tests/ui.test.mjs
```

## Build and extend

```bash
python3 scripts/build_dashboard.py            # dashboard/src/* -> dashboard/charger-control-room.html
python3 scripts/build_dashboard.py --check    # exit 1 if the committed bundle is stale
bash dashboard/tests/regen_golden.sh build    # regenerate demo data and goldens from the C++ tools
node scripts/make_dashboard_screenshots.cjs   # regenerate the README screenshots
```

`src/core.js` holds every rule (no DOM, runs under Node); `src/i18n.js` holds the
English and Italian strings; adding a language is one more column. The demo
journal is **synthetic**: `charger_synth` drives the real Monitor and Ledger with
a scenario clock, so it is a genuine, verifiable journal whose contents are
invented. Nothing in it is a measurement and the tariff in it is a placeholder.
