# Interoperability

This library does not implement OCPP or OCPI. It produces the local,
reproducible ledger that a backend needs, and this document maps every field
onto the standard vocabulary so an existing charge-management system can
consume it without reverse-engineering anything.

Sources (checked against the primary texts or their published schemas):

- **OCPP 2.0.1** (IEC 63584), Part 2: `TransactionEvent`, `MeterValue`,
  `ReasonEnumType`, `MeasurandEnumType`. OCPP **2.1** (released January 2025,
  IEC 63584-210:2025) is backward compatible with 2.0.1 and adds, among other
  things, bidirectional charging and DER control; none of that changes the
  mapping below.
- **OCPI 2.2.1**, Tariffs and CDRs modules. OCPI **2.3.0** (February 2025)
  keeps the CDR and tariff objects unchanged; the mapping applies to both.

## Where this sits in a charging architecture

`charger_monitor` is a **passive listener on the cabinet-internal CAN segment**:
the module and bay telemetry that a vendor describes in a `.dbc` file. It sits
*below* OCPP/OCPI (backend side) and *beside* ISO 15118 / DIN SPEC 70121
(vehicle side, carried over PLC). It sees neither of those, and CCS PLC frames
never appear on this bus. That is why it can be an independent witness: it is
not in the control path and does not depend on the station firmware or the
uplink being healthy.

## Session to TransactionEvent

The monitor emits one open and one close per charging session. In OCPP 2.0.1 a
session is a stream of `TransactionEvent` messages with `Started`, zero or more
`Updated`, then `Ended`.

| Monitor event | OCPP 2.0.1 equivalent | Note |
| --- | --- | --- |
| `SESSION_OPENED` | `TransactionEvent(eventType=Started, transactionInfo.transactionId, triggerReason=Authorized)` | `transactionId` is a string of at most 36 characters; the monitor uses a `uint64` id **per asset**, so a backend must build a unique one (for example `<asset_id>-<id>`) |
| accepted measurement frames | `TransactionEvent(eventType=Updated)` | the monitor logs locally instead of streaming, so the backend decides the cadence |
| `SESSION_CLOSED` | `TransactionEvent(eventType=Ended, transactionInfo.stoppedReason=...)` | the stop reason is mapped below |

### stoppedReason mapping

`Local`, `Timeout` and `Other` are members of `ReasonEnumType`. The list below
is a suggestion for a backend, not something the monitor emits.

| Monitor `stop_reason` | OCPP 2.0.1 `stoppedReason` | Rationale |
| --- | --- | --- |
| `STATUS_OFF` | `Local` | the power stage was switched off locally |
| `STATUS_READY` | `Local` | the stage left the charging state locally |
| `COMMUNICATION_TIMEOUT` | `Other` (carry the monitor code in `transactionInfo.stoppedReason` text or a custom field) | OCPP has no "bus dropout" reason; `Timeout` is defined for a different timeout, so prefer `Other` unless your CSMS documents otherwise |
| `POWER_STAGE_FAULT` | `Other` | the stage reported a fault; use a specific value (`OvercurrentFault`, `GroundFault`, `PowerQuality`, …) only when the cabinet tells you which |
| `MEASUREMENT_OUT_OF_RANGE` | `Other` | a sensor or scaling fault, not a stop by the driver |
| `ENERGY_COUNTER_SATURATED` | `Other` | internal integrity failure, must not be invoiced |

The monitor does not emit OCPP enums itself. The mapping is documented so a
backend can translate, and so the reason stays auditable locally.

## Measurands

OCPP 2.0.1 `MeterValue.sampledValue` carries a `measurand`, a `value` and a
`unitOfMeasure`. All four measurands below are members of `MeasurandEnumType`.

| Ledger field | OCPP | Unit | Conversion |
| --- | --- | --- | --- |
| `energy_wh` | `Energy.Active.Import.Register` | Wh | integer as-is, **plus a baseline** (see below) |
| `energy_uwh` | `Energy.Active.Import.Register` | Wh | `unitOfMeasure {unit: "Wh", multiplier: -6}` carries µWh exactly |
| `power_mw` | `Power.Active.Import` | W | divide by 1 000 |
| `voltage_mv` | `Voltage` | V | divide by 1 000 |
| `current_ma` | `Current.Import` | A | divide by 1 000 |
| `charging_s` | `transactionInfo.timeSpentCharging` | s | integer as-is |

Two points that matter when a backend ingests these:

- `Energy.Active.Import.Register` is a **cumulative meter register**. The
  ledger value is a **session-relative integral** of V × I. A backend must add
  its own baseline and must not present the ledger figure as a meter register:
  it is an operational record, not a metering instrument (see
  [COMPLIANCE.md](COMPLIANCE.md)).
- `timeSpentCharging` is a field of `transactionInfo`, not a measurand, and it
  is the time energy flowed. It maps from `charging_s`. `duration_ms` (the
  session wall time) has no OCPP field.

## Tariff dimensions

The monitor prices a **declared subset** of OCPI 2.2.1.

| Ledger field | OCPI 2.2.1 dimension | Unit | `step_size` basis |
| --- | --- | --- | --- |
| `energy_cost_micro_eur` | `ENERGY` | per kWh | 1 Wh |
| `time_cost_micro_eur` | `TIME` | per hour of charging | 1 s |
| `flat_cost_micro_eur` | `FLAT` | per session | no unit |
| not produced | `PARKING_TIME` | per hour not charging | would require proof of connector occupancy |

The rounding rule is OCPI's: the consumed amount is raised to the smallest
multiple of `step_size` greater than the consumption. An amount that is already
an exact multiple is not increased (OCPI's own example: step 25 bills 101–125 Wh
as 125 Wh).

VAT is omitted, which is distinct from zero-rated VAT. A backend that needs VAT
must apply it downstream; the monitor never guesses a tax regime.

## CDR

`charger_settle --cdr ASSET ID` writes `charger.cdr.v1`: **a local record that
carries the OCPI concepts a backend needs to build its CDR**, with the tariff of
record and the chain provenance attached. It is deliberately *not* an OCPI CDR
object, because several required OCPI fields are knowledge the cabinet does not
have (who the driver was, which location id the CPO uses, which party it is).

What an OCPI 2.2.1 CDR requires, and where each piece comes from:

| OCPI CDR field | Source |
| --- | --- |
| `id`, `session_id` | `cdr_id` / `session_id` (`<asset_id>-<id>`) |
| `start_date_time` | `derived_start_utc`: the journal carries a wall-clock label for the close only, so the start is **close minus the measured duration**. It is a derived value and must be declared as such (for example in `remark`), never presented as measured |
| `end_date_time` | `end_utc` |
| `charging_periods[].dimensions` `ENERGY` | **kWh** (`energy_wh` / 1000; OCPI numbers default to 4 decimals, so the backend must define a rounding rule) |
| `charging_periods[].dimensions` `TIME` | **hours** (`charging_s` / 3600) |
| `total_energy` | kWh, from `energy_wh` |
| `total_time` | hours, from `charging_s` |
| `total_cost` | `Price { excl_vat, incl_vat? }`: `costs.total_excl_vat`; `incl_vat` stays absent because VAT is omitted |
| `currency` | `currency` |
| `cdr_token`, `auth_method`, `authorization_reference` | **not known to the monitor**: the backend knows who authorised the session |
| `cdr_location`, `country_code`, `party_id` | **not known to the monitor**: operator configuration in the backend |
| `signed_data` (optional) | not produced; if the station has a certified signing meter, its OCMF record belongs here, passed through unchanged |

Settlement between parties travels over authenticated OCPI HTTPS. What can be
*signed* in a CDR is meter data (`signed_data`, for example OCMF from a
certified meter); the local journal is the reproducible evidence behind the
numbers, not the wire format.

## What this library does not do

- It does not speak OCPP or OCPI. No WebSocket, no credentials, no tokens.
- It does not produce an OCPI-conformant CDR or a signed CDR.
- It does not implement `PARKING_TIME`, because the demo protocol cannot prove
  that a connector is occupied while not charging.
- It does not manage authorisation or RFID.
- It does not carry signed meter values (OCMF) and does not verify signatures.

These are deliberately out of scope. The library is the trustworthy local
record; the backend remains the system of record for interchange.
