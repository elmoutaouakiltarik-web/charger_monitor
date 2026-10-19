# Roadmap

The project's contract with its users: **everything claimed as working ships in
this release**. Anything listed here is *not yet implemented*. A feature moves
from this file to the changelog only when it has tests that fail without it.

## Planned — v2.2

- **OCMF `signed_data` end-to-end verification.** The meter's OCMF payload is
  already passed through verbatim into session records (see
  `charger_monitor.hpp`, `SessionRecord.ocmf_signed_data`). Verifying the
  ECDSA signature chain against the public meter certificate requires an
  ASN.1/DER reader; it is intentionally not half-done. Until it ships, the
  payload is treated as *unverified meter attestation*: present, immutable
  in the journal, but not a substitute for reading the meter's own audit
  log.
- **Per-connector baselines for anomaly detection.** The z-score and IQR
  detectors shipped in 2.0/2.1 reason over per-asset history and across
  bays; baselining each connector separately (a DC bay serving both light
  and heavy vehicles sees bimodal sessions) needs per-connector grouping
  in the journal first.
- **Anomaly scoring beyond explainable statistics.** Machine-learning
  scoring stays out until the explainable layer (z-score, IQR, drift,
  degradation) is proven in the field.

## Under consideration

- **HTTPS webhook delivery.** The built-in sender speaks plain HTTP only (see
  the `webhook_url` validation in `charger_config.cpp`); TLS delivery is by
  reverse proxy today. A compiled-in TLS client adds a dependency the core
  does not currently carry.
- **Linux SocketCAN live capture** in the monitor binary (candump replay and
  synthetic generation are shipped; a live interface is an integration-test
  problem, not a library problem).
- **Windows service wrapper** with automatic restart and journal recovery on
  boot.

## Explicitly out of scope

- A JSON library dependency. The hand-written parsers in `charger_config.cpp`
  are strict, locate errors by field, and are covered by tests; the journal
  format is line-oriented JSON written by the library itself.
- Cloud anything. The monitor runs where the charger runs; it writes files
  and (optionally) HTTP webhooks. What happens downstream is another
  deployment's business.
