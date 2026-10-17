# Security Policy

## Scope

This project is a **monitoring library and demo**. It reads a CAN bus and
produces an operational ledger. It does not command the power stage.

Before reporting an issue, please check whether you are looking at one of the
declared limits below. They are deliberate design decisions, not oversights.

## Deliberate non-goals

This project **is not**:

- firmware or a certified controller;
- a safety function under IEC 61508, ISO 26262 or IEC 62443;
- a legal or fiscal meter (MID) — energy figures are an operational record;
- a cryptographically non-repudiable audit trail.

The journal's hash chain detects casual edits and truncated histories. It does
**not** provide non-repudiation: an attacker who rewrites the entire file from
the genesis block produces a valid chain. Non-repudiation requires the CSMS
signature (OCPP) or a site PKI, neither of which is implemented here.

The library never resets a physical fault from software. `reset_fault()` denies
protected fault codes by design. Please do not request a "force" flag.

## What we consider a vulnerability

- Integer overflow or wraparound in the energy accumulator, the tariff
  computation or the hash chain.
- A crafted CAN frame that bypasses range validation, corrupts the state
  machine, or causes a billing record to be marked `billing_complete` when the
  data is not.
- A communication dropout that is silently interpolated into billed energy.
- A journal that reports an intact chain when a record was altered.
- Path traversal or injection through `asset_id`, currency or config values.
- Any crash on malformed input (replay lines, config files, journal rows).

## Supported versions

| Version | Supported |
| --- | --- |
| 1.3.x | yes |
| < 1.3 | no |

## Reporting

Report privately through GitHub Security Advisories on the repository, or by
opening an issue if advisories are not enabled. Please include the input that
triggers the problem, the observed behaviour and the expected behaviour.

Do not open a public issue for an unfixed vulnerability before a fix exists.

## Verifying the ledger yourself

`sha256_hex()` is exported so the chain can be checked with any standard tool.
The CLI also provides:

```bash
charger_monitor --verify-journal --journal <path>   # exit 0 intact, 3 altered
```

Any auditor, without trusting this binary, can recompute the chain with
`sha256sum` and the documented line layout.