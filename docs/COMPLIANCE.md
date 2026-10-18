# Compliance and positioning

This page says what `charger_monitor` is allowed to claim, what it is not, how
to use it next to certified equipment, and what a vendor or integrator will ask
about its supply chain. It is an engineering position, not legal advice: confirm
anything that matters for your jurisdiction with your metrology authority or
counsel.

Every external fact below carries the confidence of how it was verified:
**primary** (the text or published schema was read), **secondary** (a reliable
summary of a primary source), or **unconfirmed**. Dates are as of October 2026.

## What this software is

An **operational record with exact arithmetic**. It integrates CAN-reported
voltage and current with 128-bit integers, never interpolates across a gap,
records every dropout, and writes the result into a SHA-256 hash-chained journal
that makes edits of its own file evident. Anyone can reproduce its numbers: the
journal format is documented, the hash is standard, and the dashboard in this
repository is a second, independent implementation checked against the C++
tools.

"Exact" always means **arithmetically exact**: no rounding drift in the
integration, the tariff or the roll-up. It does **not** mean accurate in the
metrological sense. The accuracy of an energy figure is bounded by the voltage
and current sensors behind the CAN signals, and nobody has assessed those here.

## What it may claim

- Arithmetic exactness of the integration and the pricing, and the proof of it:
  `charger_bench` shows the integer path agreeing with the closed form where a
  `double` path drifts.
- Explicit, flagged handling of measurement gaps (`MEASUREMENT_GAP`,
  `energy_lower_bound`, `time_upper_bound`) instead of invented energy.
- Tamper-*evidence* of its own journal, and, with an external anchor, evidence
  that a prefix was not rewritten after the anchor was made.
- Third-party reproducibility of every figure (journal layout, `sha256sum`,
  the tariff of record embedded in each close line).
- Use as reconciliation and dispute-support evidence next to the system of
  record.

## What it must not claim

- "MID / Eichrecht compliant", "calibrated", "billing-grade", "legally valid",
  or any accuracy percentage.
- Non-repudiation or proof of authorship. A hash chain anchored to a public
  genesis constant can be rewritten from scratch by anyone who can write the
  file, and the result verifies. An external anchor narrows that; it is still
  not a signature.
- That the figures replace a certified meter reading where the law requires one.

## Regulatory landscape (why the boundary sits where it does)

**European Union — measuring instruments.** Directive 2014/32/EU (MID), Annex V
(MI-003), covers active electrical energy meters for residential, commercial
and light-industrial use. Direct-current metering for charging was a known gap
for years. *Secondary sources report* that Directive (EU) 2026/706, published in
the Official Journal on 20 March 2026, amends the MID for direct current and adds
an annex for **measuring systems for electric vehicle supply equipment**
(referred to as MI-011), with transposition due in April 2028 and application
from October 2028; the instrument-level details are **unconfirmed** here, so
read the Official Journal text before relying on them. The practical
consequence is already clear: "not a MID meter" is an incomplete description,
because the regime is moving from the *meter* to the whole *measuring system at
the transfer point*. This project is neither, and says so.

**Germany.** The calibration law (MessEG / MessEV) applies to public charging.
The conformity route uses a certified signing meter, a signed data record in the
**OCMF** format (Open Charge Metering Format, SAFE e.V.; ECDSA over SHA-256) and
"transparency software" that lets the customer verify the signature against the
station's public key. The Eichrecht rules are **German law**; they do not apply
to an Italian site.

**Italy.** AFIR applies directly (below). The Italian transposition requires
public charging prices to be "ragionevoli, comparabili, trasparenti e non
discriminatori" (D.Lgs. 257/2016, art. 4, c. 11) *(secondary)*, and charging is
treated as a service rather than a supply of energy *(secondary, DL 76/2020
art. 57)*. Metrology rests on the MID transposition (D.Lgs. 22/2007) and the
periodic-verification rules (DM 93/2017) *(unconfirmed here)*. A national rule
beyond AFIR that makes per-kWh pricing mandatory or fixes the meter class was
**not found**; check before assuming one exists.

**AFIR, Regulation (EU) 2023/1804.** Article 5 requires ad-hoc payment at new
public points and, for points of 50 kW or more, an ad-hoc price **based on the
price per kWh delivered** (plus an optional occupancy element), shown before the
session *(secondary)*. The Commission states there are no numeric price
thresholds. No AFIR text mandating a metrology class was found *(a negative
claim, unconfirmed)*. Data obligations (Art. 20, DATEX II from April 2026) and
the ISO 15118-20 requirement for new or renovated points from 2027 are
backend- and vehicle-side matters, outside what a CAN monitor sees.

## Using it next to a certified meter

The sound pattern is **second opinion, not replacement**:

1. The certified meter and the backend remain the system of record where law
   requires it.
2. `charger_monitor` keeps an independent, exact local record on the cabinet
   bus. When the backend, the firmware or the uplink is missing or disputed,
   the journal still answers "what was delivered, under which tariff, and has
   this file been altered?".
3. Reconcile periodically: compare the session energy with the meter's own
   delta. A persistent drift points at a sensor, a scaling error or a wiring
   problem, which is exactly what the diagnostics exist to surface early.

The roadmap item `charger_settle --reconcile-ocmf` would read the start and end
readings that the station already logs in OCMF strings and report the drift. It
would **not** verify signatures (that needs ECDSA, which a zero-dependency
library does not carry); signature verification stays with the SAFE
transparency software.

## Cybersecurity and supply-chain posture

- **Zero third-party components.** The library is the C++17 standard library.
  [`sbom/charger_monitor.cdx.json`](../sbom/charger_monitor.cdx.json) is a
  CycloneDX 1.6 SBOM stating exactly that; it validates against the official
  schema. The dashboard is one HTML file with no third-party script, font or
  style, pinned by a Content-Security-Policy hash.
- **Engineering practices** a vendor can inspect: `-Wall -Wextra -Wpedantic
  -Werror` under two compilers, ASan + UBSan builds, a libFuzzer harness over
  malformed frames, tariffs and journal rows, a CI job that builds a downstream
  consumer from the installed package, and byte-for-byte parity tests between
  the C++ tools and the independent JavaScript verifier. These are *practices*;
  no certification is claimed. IEC 62443 is a cybersecurity series (4-1: secure
  development lifecycle; 4-2: component requirements), not functional safety;
  the project has not been assessed under it.
- **EU Cyber Resilience Act (Regulation (EU) 2024/2847).** Manufacturer
  reporting of actively exploited vulnerabilities has applied since
  11 September 2026 and the main obligations apply from 11 December 2027
  *(secondary, consistent across sources)*. Free and open-source software
  supplied outside a commercial activity is generally excluded, and a
  single-maintainer project is probably not an "open-source steward". The real
  effect is on **integrators**: a manufacturer who embeds this library has to
  exercise due diligence on third-party components and will ask for a
  vulnerability contact, a fix policy and an SBOM. [`SECURITY.md`](../SECURITY.md)
  and the SBOM are there for that conversation.
- **NIS2** lists operators of a recharging point among the electricity-sector
  entities *(secondary)*; your customer may be in scope, which makes the
  hardened deployment in [OPERATIONS.md](OPERATIONS.md) relevant.

## Date certainty for anchors

An anchor file proves *what* the journal prefix was; it does not prove *when* it
was made. For date certainty, time-stamp the anchor with an RFC 3161 authority
(`scripts/notarise.sh`, see FIELD_GUIDE §6). A **qualified** electronic time
stamp under eIDAS (Regulation 910/2014, Art. 41(2)) carries a legal presumption
of the accuracy of its date and the integrity of the bound data; a non-qualified
one is not denied legal effect solely for being electronic, but carries no such
presumption. Only a 32-byte digest leaves the site.

## Sources

- Directive 2014/32/EU, Annex V (MI-003). Directive (EU) 2026/706 *(secondary)*.
- Regulation (EU) 2023/1804 (AFIR), Art. 5 and Art. 20; Commission Q&A on AFIR.
- OCMF specification 1.4.1, SAFE e.V. (github.com/SAFE-eV/OCMF-Open-Charge-Metering-Format).
- Regulation (EU) 910/2014 (eIDAS), Arts. 41–42.
- Regulation (EU) 2024/2847 (CRA); Commission CRA reporting page.
- OCPP 2.0.1 / IEC 63584 and OCPP 2.1 / IEC 63584-210:2025 (Open Charge Alliance);
  OCPI 2.2.1 and 2.3.0 (github.com/ocpi/ocpi).
