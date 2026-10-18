# Operations — running a bay in production

How to run `charger_monitor` on a real cabinet host and keep it supervised,
backed up and auditable. Everything here is a *pattern*: the numbers (bit rate,
timeouts, retention, alert thresholds) are yours to set from the cabinet's
schema and your SLA. Nothing in this repository guesses them.

```
 cabinet CAN ──(listen-only)──► charger_monitor@bay-07 ──► /var/lib/charger/bay-07.journal   billing record, backed up
                                         │
                                         ├─► /run/charger/status/bay-07.json           tmpfs, atomic  ──► nginx ──► Control Room (Live)
                                         └─► /var/lib/node_exporter/textfile/…prom     atomic         ──► node_exporter ──► Prometheus ──► alerts
```

The monitor opens **no listening socket**. There is no endpoint to attack, no
credential to leak and no protocol to keep patched: it writes files, and the
things you already run read them.

## 1. Prepare the host

```bash
sudo useradd --system --home /var/lib/charger --shell /usr/sbin/nologin charger
sudo install -d -o root -g root -m 0755 /etc/charger
sudo install -m 0755 build/charger_monitor build/charger_settle /usr/local/bin/
sudo install -m 0644 deploy/systemd/charger-monitor@.service /etc/systemd/system/
sudo systemctl daemon-reload
```

**Time.** The journal's timestamps are wall-clock labels written at close. Run
NTP (or PTP) and monitor it: a clock that steps changes what a dispute reads.
Durations and timeouts use a monotonic clock and are not affected.

**CAN.** Bring the interface up in **listen-only** mode, so the controller
cannot drive the bus at all, not even to acknowledge a frame. With
systemd-networkd use `deploy/systemd/80-can0.network`; the bit rate in it is an
example, take yours from the cabinet's CAN schema. Listen-only is the hardware
version of "it never commands the power stage".

**Per-bay files.** For a bay called `bay-07`:

```bash
sudo sh -c 'echo CAN_IFACE=can0 > /etc/charger/bay-07.env'
charger_monitor --print-config | sudo tee /etc/charger/bay-07.json   # then edit it
```

Edit `asset_id`, the tariff (verify `energy_eur_per_kwh` is the contracted one:
a zero tariff invoices nothing and nobody notices), the stream timeouts and, with
the electrical engineer, the diagnostic thresholds. For a real fleet add the
vendor DBC and name the signals (`--dbc`, `--voltage-signal`, …); see the
field guide §2.

## 2. Run it

```bash
sudo systemctl enable --now charger-monitor@bay-07
journalctl -u charger-monitor@bay-07 -f       # events on stderr: SESSION_OPENED, MEASUREMENT_GAP, …
```

What the unit enforces, rather than promises: no privileges and no capability
(`NoNewPrivileges`, empty bounding set), a read-only filesystem except the
journal directory, the tmpfs status directory and the textfile directory,
address families limited to `AF_CAN` and `AF_UNIX`, no writable-and-executable
memory, a system-call allowlist, and `StandardOutput=null`. That last one
matters: the monitor prints one JSON snapshot per frame on stdout, which would
flood the host's journal at 3 500 frames/s. Supervision uses the status files,
billing uses the journal, and events go to stderr.

**Restarts and sessions.** On `SIGTERM` the monitor closes a session that is
open at that moment with stop reason `MONITOR_SHUTDOWN` and quality
`INTERRUPTED`: the energy accepted so far is kept exactly, flagged as a lower
bound, and the session is **held for review**, never invoiced automatically. A
crash or power loss cannot do that: the open line has no close, and the session
is reported as an **orphan** on the next start (energy unknown, not billable).
Either way, plan upgrades and rotations for when the bay is idle. After a
restart the session ids continue from the journal, so `(asset, id)` stays
unique across runs.

## 3. Supervise it

**Dashboard.** Put the Control Room and the status directory behind one web
server, on the plant LAN, behind your access control
(`deploy/nginx/charger-dashboard.conf`). Open the page, go to **Live**, list
`status/bay-07.json`. It flags a status file that is no longer being updated: a
dead monitor looks exactly like a healthy one if nothing checks the file's age.

**Prometheus.** Point node_exporter's textfile collector at the directory the
unit writes to (`--collector.textfile.directory=/var/lib/node_exporter/textfile`)
and load `deploy/prometheus/charger-alerts.yml` after `promtool check rules`.
Things worth knowing:

- Only `*.prom` files are read; the monitor writes `…prom.tmp` and renames, so a
  scrape never sees a partial file.
- A file with a parse error is dropped whole (`node_textfile_scrape_error`).
- **node_exporter never expires a stale file.** If the monitor dies, the last
  values are served forever with fresh scrape timestamps. Alert on
  `charger_export_timestamp_seconds`, not on the values.
- A quantity the monitor cannot know is omitted, not exported as zero: no
  availability ratio before there is in-service time, no voltage after the
  measurement stream went stale.
- Prometheus stores float64, exact for integers up to 2^53. An energy counter in
  Wh with six decimals stays exact up to about 9 GWh per series; the journal is
  the source of truth either way.

Metric families (all carry `asset`): `charger_info`, `charger_export_timestamp_seconds`,
`charger_state{state}`, `charger_power_enabled`, `charger_measurement_{voltage_volts,
current_amperes,power_watts}`, `charger_{measurements,status}_stale`,
`charger_{measurement,status}_age_seconds`, `charger_frames_{accepted,rejected}_total`,
`charger_sessions_{opened,closed,billable,undersampled}_total`,
`charger_integration_gaps_total`, `charger_energy_closed_watt_hours_total`,
`charger_state_seconds_total{state}`, `charger_in_service_seconds_total`,
`charger_{availability,utilization,frame_accept}_ratio`, `charger_{fault,timeout}_events_total`,
`charger_transport_errors_total`, `charger_diagnostic_{active,warnings_total}`,
`charger_fault_active{code}`, `charger_session_{active,energy_watt_hours,duration_seconds}`.
The fleet view from a verified journal is `charger_settle JOURNAL --metrics`
(`charger_portfolio_*`, `charger_asset_*`).

## 4. Keep the record

The journal is the billing system of record. Treat it like one.

- **Where.** `/var/lib/charger`, on a durable filesystem, `fsync` on (the
  default). Never `/tmp`, never NFS without synchronous commit. Back it up like a
  database: it is append-only, so incremental backups are cheap.
- **Who can read it.** Mode `0640`, group-readable by the billing user.
  Journals contain energy and money; do not publish them from the web server
  without access control.
- **Rotate monthly, when the bay is idle.** Stop the unit, move the journal to the
  archive, **anchor it**, and start a new one. A session open at that moment is
  closed as `INTERRUPTED` (above) in the old file; the next one starts in the new.
  Keep config, DBC and anchors with the journal: in three years the question will
  be "under which tariff and which decode", and the answer must live in the
  archive, not in someone's memory.
- **Anchor on a schedule.** At the end of every shift or day, write an anchor to
  append-only storage (a WORM share or an object-lock bucket):

  ```bash
  charger_monitor --anchor-journal --journal /var/lib/charger/bay-07.journal \
      --anchor-file /mnt/worm/bay-07.$(date -u +%Y%m%d).anchor
  ```

  For date certainty, time-stamp each anchor with an RFC 3161 authority
  (`scripts/notarise.sh`, field guide §6.1). An anchor proves what the prefix was;
  only the time stamp proves when. Neither is a signature.
- **Verify on a schedule too.** `charger_monitor --verify-journal` and
  `--verify-anchor` exit non-zero on tampering: wire them to your scheduler, and
  to the billing run (`charger_settle` refuses a broken journal with exit 3).

## 5. Settle and answer disputes

```bash
charger_settle bay-07.journal --summary              # what may be invoiced, what is held, and why
charger_settle bay-07.journal --out settlement.csv   # one row per session, with the reason it was held
charger_settle bay-07.journal --dispute bay-07 12    # the complete evidence pack for one session
```

Only `billing_complete` sessions are invoiced automatically. Held sessions are
neither dropped nor silently invoiced: each carries the reason (measurement gap,
communication timeout, too few samples, interruption, orphan), and the dashboard
shows it next to the arithmetic. The Control Room opens the same evidence in two
clicks, recalculates it independently, and prints the three shell lines an
auditor can run without any of our software.

## 6. Upgrade checklist

1. Wait for the bay to be idle (or accept an `INTERRUPTED` held session).
2. Stop the unit, take a backup of the journal and write an anchor.
3. Install the new binaries; `systemctl start`.
4. Re-run the acceptance test (field guide §3): replay a known sequence, verify
   the chain, check the 4 800 Wh benchmark, open the journal in the dashboard
   and compare.
5. Watch the first session end to end: it must open, accrue, close and appear in
   `--summary`.

## 7. Troubleshooting

| Symptom | Look at |
| --- | --- |
| Unit refuses to start | `journalctl -u charger-monitor@bay-07`: the config is rejected with the path of the bad field, by design |
| State stays `DISCONNECTED` | `candump can0` (are frames arriving? right IDs? the DBC's message ids and byte order?), stream timeouts in the config |
| Sessions all held | gaps and timeouts: bus quality, or timeouts tighter than the real frame period. `charger_probe` (when available) measures the period from a recording |
| `ORPHAN_SESSION` at start | the process died mid-session (crash, power); energy unknown, not billable; review by hand |
| Dashboard shows a stale file | the unit is down, or the browser's clock is off; check NTP on both |
| Alert `ChargerMonitorNotWriting` | the monitor or its host stopped; every other value on that bay is frozen |
| `EXPORT_FILE_FAILED` on stderr (at most once a minute) | the status or textfile directory is not writable; billing is unaffected |
