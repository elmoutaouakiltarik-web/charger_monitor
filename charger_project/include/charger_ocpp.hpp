#pragma once

// OCPP 2.0.1 TransactionEvent export (a file, not a transport).
//
// json_ocpp_events() turns one closed session of the verified journal into the
// two TransactionEventRequest payloads (Started, Ended) a CSMS integration
// would send for it, so a backend can ingest the local record without
// reverse-engineering the ledger. There is no WebSocket, no TLS, no credential
// and no state here: the library stays dependency-free, and the backend keeps
// the protocol.
//
// Every enum value and field name below was checked against the OCPP 2.0.1 JSON
// schema for TransactionEventRequest (TransactionEventEnumType,
// TriggerReasonEnumType, ChargingStateEnumType, ReasonEnumType,
// ReadingContextEnumType, MeasurandEnumType, UnitOfMeasureType); see
// scripts/validate_ocpp_export.py, which validates an export against the
// schema shipped in the `ocpp` Python package.
//
// stoppedReason (transactionInfo.stoppedReason, ReasonEnumType):
//   STATUS_OFF                  -> Local
//   STATUS_READY                -> Local
//   COMMUNICATION_TIMEOUT       -> Other   (OCPP has no "bus dropout" reason)
//   POWER_STAGE_FAULT           -> Other
//   MEASUREMENT_OUT_OF_RANGE    -> Other
//   ENERGY_COUNTER_SATURATED    -> Other
//   MONITOR_SHUTDOWN            -> Other
//   anything else               -> Other
// The monitor's own code is kept in the "session" block next to the events
// (the OCPP payloads accept no extra properties), so nothing is lost.
//
// triggerReason (TriggerReasonEnumType):
//   Started                                     -> ChargingStateChanged
//   Ended, stop STATUS_OFF or STATUS_READY      -> ChargingStateChanged
//   Ended, every other stop reason              -> AbnormalCondition
//
// The energy is a SESSION-RELATIVE integral of V x I, in microwatt-hours,
// reported as Energy.Active.Import.Register with unitOfMeasure {Wh, -6}: 0 at
// Transaction.Begin and the session total at Transaction.End. It is NOT a meter
// register (a backend that wants a register must add its own baseline), it
// carries NO signature (no OCMF), and it is not a legal measurement. The start
// timestamp is DERIVED (close minus the measured duration); the close timestamp
// is the wall-clock label the journal wrote at close.
//
// An export is refused, never invented, when: the close timestamp does not
// parse as strict YYYY-MM-DDTHH:MM:SSZ; the duration is negative; the
// transactionId "<asset>-<id>" would exceed OCPP's 36 characters; or the energy
// exceeds 2^53 uWh (a JSON number is a double for many parsers and would lose
// precision silently).

#include <cstdint>
#include <optional>
#include <string>

#include "charger_ledger.hpp"

namespace charger {

constexpr const char* OCPP_EVENTS_SCHEMA = "charger.ocpp-events.v1";

// Returns the JSON document, or an empty string with `error` set when the
// session cannot be exported honestly.
std::string json_ocpp_events(const SettledSession& session, std::string& error);

// The two mappings documented above. Unknown codes map to "Other" /
// "AbnormalCondition": an unfamiliar stop is never presented as a normal one.
const char* ocpp_stopped_reason(const std::string& stop_reason) noexcept;
const char* ocpp_ended_trigger_reason(const std::string& stop_reason) noexcept;

// Strict "YYYY-MM-DDTHH:MM:SSZ" (UTC, as the journal writes it) to epoch
// seconds. Rejects anything else: wrong length, separators, lower case, a day
// that does not exist in that month (leap years included), a leap second.
std::optional<std::int64_t> parse_utc_epoch(const std::string& text);

// Epoch seconds to "YYYY-MM-DDTHH:MM:SSZ" with floor-division semantics for
// instants before 1970. Years outside 0000..9999 return nullopt.
std::optional<std::string> format_utc_epoch(std::int64_t epoch_seconds);

// The session start the journal does not record: close minus the measured
// duration, in whole seconds, rounded DOWN (the close label is itself truncated
// to the second, so sub-second precision would be false precision). nullopt when
// the close label does not parse, the duration is negative, or the result is
// outside the year range 0000..9999.
//
// json_cdr() already wires this in as the fallback: sessions closed by a
// v2.0-or-later writer carry a measured ocpi_start_utc ("start_source":
// "journal"); older ones get this derivation, labelled "close-minus-duration",
// so a dispute reviewer always knows how the start was established.
std::optional<std::string> derive_start_utc(const std::string& closed_at_utc, std::int64_t duration_ms);

}  // namespace charger
