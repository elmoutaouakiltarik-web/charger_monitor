# Security Policy

## Scope

This project is a **monitoring library, a set of tools and a read-only
dashboard**. It reads a CAN bus and produces an operational ledger. It does not
command the power stage. It opens no listening socket: supervision happens
through files it rewrites atomically.

Before reporting an issue, please check whether you are looking at one of the
declared limits below. They are deliberate design decisions, not oversights.

## Deliberate non-goals

This project **is not**:

- firmware or a certified controller;
- a safety function (IEC 61508, ISO 13849), and it has **not been assessed or
  certified under IEC 62443**, which is a cybersecurity series, not functional
  safety;
- a measuring instrument or measuring system under Directive 2014/32/EU: energy
  figures are an operational record, not a legal measurement (see
  [docs/COMPLIANCE.md](docs/COMPLIANCE.md));
- a cryptographically non-repudiable audit trail.

The journal's hash chain detects casual edits and truncated histories. It does
**not** provide non-repudiation: an attacker who rewrites the entire file from
the genesis block produces a valid chain. Non-repudiation needs a signature made
with a key held **outside this host** (a certified signing meter, an HSM-backed
site key, a qualified seal). An external anchor, optionally time-stamped with an
RFC 3161 authority, narrows the gap by proving that a prefix was not rewritten
after the anchor was made; it is not a signature.

The library never resets a physical fault from software. `reset_fault()` denies
protected fault codes by design. Please do not request a "force" flag.

## What we consider a vulnerability

- Integer overflow or wraparound in the energy accumulator, the tariff
  computation or the hash chain.
- A crafted CAN frame that bypasses range validation, corrupts the state
  machine, or causes a billing record to be marked `billing_complete` when the
  data is not.
- A communication dropout that is silently interpolated into billed energy.
- A journal that reports an intact chain when a record was altered, or a
  verifier (C++ or the dashboard) that disagrees with the other on whether a
  chain is intact.
- Path traversal or injection through `asset_id`, currency or config values,
  including into the tab-delimited journal prefix, the JSON payloads, the CSV
  and Prometheus exports.
- Any crash, hang or unbounded memory use on malformed input (replay lines,
  config files, journal rows, DBC files, TOU tariffs, anchors).
- In the dashboard: any way for journal, anchor or snapshot content to execute
  script, make a network request, or escape the page's Content-Security-Policy.

## Supported versions

| Version | Supported |
| --- | --- |
| 1.4.x (current) | yes |
| 1.3.x | security fixes only, on request |
| < 1.3 | no |

## Reporting

Report **privately** through GitHub's *Report a vulnerability* button on the
repository's Security tab (private vulnerability reporting). Please include the
input that triggers the problem, the observed behaviour and the expected
behaviour. A minimal journal or frame sequence that reproduces it is the most
useful attachment.

Do not open a public issue for an unfixed vulnerability before a fix exists. If
the private reporting button is not available, open an issue that says only
"security contact request" and no details; a maintainer will arrange a private
channel.

## For integrators and manufacturers

If you embed this library in a product you will need a vulnerability contact, a
fix policy and a bill of materials. The contact is above. The fix policy is: a
confirmed vulnerability gets a patch release on the supported line and a
[CHANGELOG](CHANGELOG.md) entry that says what was wrong and who was exposed.
The bill of materials is [`sbom/charger_monitor.cdx.json`](sbom/charger_monitor.cdx.json)
(CycloneDX 1.6): zero third-party runtime components, because the library is the
C++17 standard library and the dashboard is one file with no third-party code.
Under the EU Cyber Resilience Act the due-diligence obligation for third-party
components falls on the manufacturer; this project is a single-maintainer open
source library and does not claim to be a CRA "open-source steward".

Operators of public charging infrastructure may fall under NIS2. The hardened
unit in [`deploy/systemd`](deploy/systemd/charger-monitor@.service) (no
privileges, read-only filesystem, no write path outside the journal and status
directories, address families limited to CAN and UNIX) exists for that reason.

## Verifying the ledger yourself

`sha256_hex()` is exported so the chain can be checked with any standard tool.
The CLI also provides:

```bash
charger_monitor --verify-journal --journal <path>   # exit 0 intact, 3 altered
```

Any auditor, without trusting this binary, can recompute a link with `sed`,
`cut` and `sha256sum`; note the `tr -d '\n'`, because the extracted line ends in
a newline that is not part of the hashed input:

```bash
sed -n '<line>p' bay.journal | cut -f1,3- | tr -d '\n' | sha256sum   # equals field 2 of that line
```

The dashboard in [`dashboard/`](dashboard/charger-control-room.html) is a second,
independent implementation of the same verification, tested byte for byte
against the C++ tools.
