# Interoperability

This library does not implement OCPP or OCPI. It produces the local,
defensible ledger that a backend needs. This document maps every field onto the
standard vocabulary so an existing charge-management system can consume it
without reverse-engineering anything.

Sources:

- **OCPP 2.0.1**, Part 2, `TransactionEvent` and `MeterValue`
- **OCPI 2.2.1**, Tariffs module and CDR module

## Session to TransactionEvent

The monitor emits one open and one close per charging session. In OCPP 2.0.1 a
session is a stream of `TransactionEvent` messages with `Started`, zero or more
`Updated`, then `Ended`.

| Monitor event | OCPP 2.0.1 equivalent | Note |
| --- | --- | --- |
| `SESSION_OPENED` | `TransactionEvent(eventType=Started, transactionInfo.transactionId, triggerReason=Authorized)` | `transactionId` is a string up to 36 chars; the monitor uses a `std::uint64_t` id, so a backend must map it (e.g. pad with the journal close timestamp) |
| accepted measurement frames | `TransactionEvent(eventType=Updated)` | the monitor logs locally instead of streaming, so the backend decides the cadence |
| `SESSION_CLOSED` | `TransactionEvent(eventType=Ended, stoppedReason=...)` | the stop reason is mapped below |

### stoppedReason mapping

| Monitor `stop_reason` | OCPP 2.0.1 `stoppedReason` | Rationale |
| --- | --- | --- |
| `STATUS_OFF` | `Local` | the power stage was switched off locally |
| `STATUS_READY` | `Local` | the stage left the charging state locally |
| `COMMUNICATION_TIMEOUT` | `Timeout` | the link to the charger was lost |
| `POWER_STAGE_FAULT` | `Other` | the stage reported a fault; the reason string is carried separately |
| `MEASUREMENT_OUT_OF_RANGE` | `Other` | a sensor or scaling fault, not a stop by the driver |
| `ENERGY_COUNTER_SATURATED` | `Other` | internal integrity failure, must not be invoiced |

The monitor does not emit OCPP enums itself. The mapping is documented so a
backend can translate, and so the reason stays auditable locally.

## Measurands

OCPP 2.0.1 `MeterValue.sampledValue` carries a `measurand`, a `value` and a
`unitOfMeasure`. The ledger already stores these as exact integers.

| Ledger field | OCPP measurand | Unit | Conversion |
| --- | --- | --- | --- |
| `energy_wh` | `Energy.Active.Import.Register` | Wh | integer as-is |
| `energy_uwh` | `Energy.Active.Import.Register` | Wh | divide by 1 000 000, or report in uWh if the backend allows |
| `power_mw` | `Power.Active.Import` | W | divide by 1 000 |
| `duration_ms` | `timeSpentCharging` | s | divide by 1 000, floor |
| `voltage_mv` | `Voltage` | V | divide by 1 000 |
| `current_ma` | `Current.Import` | A | divide by 1 000 |

`Energy.Active.Import.Register` is the OCPP default measurand, so the Wh register
maps onto it without interpretation.

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
an exact multiple is not increased.

VAT is omitted, which is distinct from zero-rated VAT. A backend that needs VAT
must apply it downstream; the monitor never guesses a tax regime.

## CDR

An OCPI Charge Data Record needs, per charging period: a start, an end, and the
dimensions consumed in that period.

| CDR field | Source |
| --- | --- |
| `start_date_time` | not tracked by the monitor; the journal carries a wall-clock label only for the close |
| `duration` in `TIME` | `charging_s` |
| `volume` in `ENERGY` | `energy_wh` |
| `cds` | `total_cost_micro_eur` excluding VAT |
| `session_id` | the `id` field of the session record |

The monitor deliberately does not produce a signed CDR. A CDR intended for
settlement must be signed and exchanged over the OCPI protocol; the local journal
is the evidence, not the wire format.

## What this library does not do

- It does not speak OCPP or OCPI. No WebSocket, no credentials, no tokens.
- It does not produce a signed CDR.
- It does not implement `PARKING_TIME`, because the demo protocol cannot prove
  that a connector is occupied while not charging.
- It does not manage authorisation or RFID.

These are deliberately out of scope. The library is the trustworthy local
record; the backend remains the system of record for interchange.