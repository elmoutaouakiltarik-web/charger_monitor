#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "charger_ocpp.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

SettledSession session(const std::string& asset, std::uint64_t id, const std::string& closed, std::int64_t duration_ms,
                       std::uint64_t energy_uwh, const std::string& stop) {
    SettledSession s;
    s.asset_id = asset;
    s.id = id;
    s.closed_at_utc = closed;
    s.record.duration_ms = duration_ms;
    s.record.charging_s = duration_ms / 1000;
    s.record.energy_uwh = energy_uwh;
    s.record.stop_reason = stop;
    s.record.quality = "OK";
    s.record.billing_complete = true;
    return s;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

int main() {
    // --- Strict parsing of the journal's close label ---
    require(parse_utc_epoch("1970-01-01T00:00:00Z") == 0, "the epoch");
    require(parse_utc_epoch("2026-10-18T00:06:40Z") == 1792282000, "a known instant");
    require(parse_utc_epoch("2000-02-29T12:00:00Z").has_value(), "2000 was a leap year");
    require(!parse_utc_epoch("1900-02-29T12:00:00Z").has_value(), "1900 was not");
    require(parse_utc_epoch("2028-02-29T00:00:00Z").has_value() && !parse_utc_epoch("2026-02-29T00:00:00Z").has_value(),
            "leap days");
    for (const char* bad :
         {"", "2026-10-18", "2026-10-18 00:06:40", "2026-10-18T00:06:40", "2026-10-18t00:06:40Z",
          "2026-10-18T00:06:40z", "2026-10-18T00:06:40+00:00", "2026-13-01T00:00:00Z", "2026-00-10T00:00:00Z",
          "2026-04-31T00:00:00Z", "2026-10-00T00:00:00Z", "2026-10-18T24:00:00Z", "2026-10-18T00:60:00Z",
          "2026-10-18T00:00:60Z", "2026-10-18T00:00:0aZ", "202-10-18T00:00:00ZZ", " 2026-10-18T00:06:40Z"}) {
        require(!parse_utc_epoch(bad).has_value(), std::string("refused: '") + bad + "'");
    }

    // --- Formatting, including before 1970 ---
    require(format_utc_epoch(0) == "1970-01-01T00:00:00Z", "format the epoch");
    require(format_utc_epoch(-1) == "1969-12-31T23:59:59Z", "one second before the epoch (floor division)");
    require(format_utc_epoch(-86400) == "1969-12-31T00:00:00Z", "one day before");
    require(format_utc_epoch(-86401) == "1969-12-30T23:59:59Z", "one second before that");
    require(format_utc_epoch(1792282000) == "2026-10-18T00:06:40Z", "round trip of a known instant");
    require(format_utc_epoch(951782400) == "2000-02-29T00:00:00Z", "leap day");
    require(format_utc_epoch(253402300799) == "9999-12-31T23:59:59Z", "last representable second");
    require(!format_utc_epoch(253402300800).has_value(), "year 10000 is refused");
    require(!format_utc_epoch(-62167219201).has_value(), "year -1 is refused");
    require(format_utc_epoch(-62167219200) == "0000-01-01T00:00:00Z", "year 0 is allowed");
    for (std::int64_t t :
         {0LL, 1LL, 59LL, 86399LL, 86400LL, 1792282000LL, 4102444800LL, -1LL, -86399LL, -2208988800LL}) {
        const auto s = format_utc_epoch(t);
        require(s.has_value() && parse_utc_epoch(*s) == t, "parse(format(t)) == t for " + std::to_string(t));
    }

    // --- derive_start_utc ---
    require(derive_start_utc("2026-10-05T02:19:00Z", 1620000) == "2026-10-05T01:52:00Z", "27 minutes before the close");
    require(derive_start_utc("2026-10-05T02:19:00Z", 1620500) == "2026-10-05T01:51:59Z",
            "a sub-second remainder rounds DOWN to the second the session had started in");
    require(derive_start_utc("2026-10-05T02:19:00Z", 1620999) == "2026-10-05T01:51:59Z", "still down at .999");
    require(derive_start_utc("2026-10-05T02:19:00Z", 1621000) == "2026-10-05T01:51:59Z", "an exact second");
    require(derive_start_utc("2026-10-05T02:19:00Z", 0) == "2026-10-05T02:19:00Z", "zero duration: start == close");
    require(derive_start_utc("2026-10-05T00:10:00Z", 1800000) == "2026-10-04T23:40:00Z", "across midnight");
    require(derive_start_utc("2026-03-01T00:00:10Z", 20000) == "2026-02-28T23:59:50Z", "across a month end, non-leap");
    require(derive_start_utc("2028-03-01T00:00:00Z", 1000) == "2028-02-29T23:59:59Z", "across a leap day");
    require(derive_start_utc("2027-01-01T00:00:05Z", 10000) == "2026-12-31T23:59:55Z", "across a year end");
    require(derive_start_utc("1970-01-01T00:00:00Z", 1000) == "1969-12-31T23:59:59Z", "before the epoch");
    require(derive_start_utc("1970-01-01T00:00:00Z", 1) == "1969-12-31T23:59:59Z",
            "1 ms before the epoch floors to the previous second");
    require(derive_start_utc("2026-10-05T02:19:00Z", 86400000LL * 400) == "2025-08-31T02:19:00Z", "400 days");
    require(!derive_start_utc("2026-10-05T02:19:00Z", -1).has_value(), "a negative duration is refused");
    require(!derive_start_utc("2026-10-05 02:19:00", 1000).has_value(), "an unparseable close label is refused");
    require(!derive_start_utc("2026-02-30T02:19:00Z", 1000).has_value(), "a day that does not exist is refused");
    require(!derive_start_utc("0000-01-01T00:00:00Z", 1000).has_value(), "a start before year 0 is refused");
    require(!derive_start_utc("2026-10-05T02:19:00Z", INT64_MAX).has_value(),
            "an absurd duration does not wrap around");

    // --- Mapping tables ---
    require(std::string(ocpp_stopped_reason("STATUS_OFF")) == "Local", "STATUS_OFF -> Local");
    require(std::string(ocpp_stopped_reason("STATUS_READY")) == "Local", "STATUS_READY -> Local");
    for (const char* other : {"COMMUNICATION_TIMEOUT", "POWER_STAGE_FAULT", "MEASUREMENT_OUT_OF_RANGE",
                              "ENERGY_COUNTER_SATURATED", "MONITOR_SHUTDOWN", "SOMETHING_NEW", ""}) {
        require(std::string(ocpp_stopped_reason(other)) == "Other", std::string(other) + " -> Other");
        require(std::string(ocpp_ended_trigger_reason(other)) == "AbnormalCondition",
                std::string(other) + " -> AbnormalCondition");
    }
    require(std::string(ocpp_ended_trigger_reason("STATUS_OFF")) == "ChargingStateChanged", "clean stop trigger");
    require(std::string(ocpp_ended_trigger_reason("STATUS_READY")) == "ChargingStateChanged", "clean stop trigger 2");

    // --- The export, byte for byte ---
    {
        const auto s = session("bay-07", 12, "2026-10-05T02:19:00Z", 1620000, 19759682110ull, "STATUS_OFF");
        std::string err;
        const std::string out = json_ocpp_events(s, err);
        require(!out.empty() && err.empty(), "a clean session exports: " + err);
        const std::string expected =
            "{\"schema\":\"charger.ocpp-events.v1\",\"note\":\"OCPP 2.0.1 TransactionEventRequest payloads rebuilt "
            "from "
            "the local ledger (a file export, no transport). The energy is a session-relative integral in "
            "microwatt-hours (unit Wh, multiplier -6) that starts at 0: it is not a meter register and carries no "
            "signature (no OCMF). The Started timestamp is derived as close minus the measured duration; the Ended "
            "timestamp is the wall-clock label written at close.\","
            "\"session\":{\"asset_id\":\"bay-07\",\"session_id\":12,\"billing_complete\":true,\"quality\":\"OK\","
            "\"stop_reason\":\"STATUS_OFF\",\"energy_lower_bound\":false,\"time_upper_bound\":false},"
            "\"events\":[{\"eventType\":\"Started\",\"timestamp\":\"2026-10-05T01:52:00Z\",\"triggerReason\":"
            "\"ChargingStateChanged\",\"seqNo\":0,\"transactionInfo\":{\"transactionId\":\"bay-07-12\","
            "\"chargingState\":\"Charging\"},\"meterValue\":[{\"timestamp\":\"2026-10-05T01:52:00Z\",\"sampledValue\":"
            "[{\"value\":0,\"context\":\"Transaction.Begin\",\"measurand\":\"Energy.Active.Import.Register\","
            "\"unitOfMeasure\":{\"unit\":\"Wh\",\"multiplier\":-6}}]}]},"
            "{\"eventType\":\"Ended\",\"timestamp\":\"2026-10-05T02:19:00Z\",\"triggerReason\":"
            "\"ChargingStateChanged\",\"seqNo\":1,\"transactionInfo\":{\"transactionId\":\"bay-07-12\","
            "\"chargingState\":\"Idle\",\"timeSpentCharging\":1620,\"stoppedReason\":\"Local\"},\"meterValue\":"
            "[{\"timestamp\":\"2026-10-05T02:19:00Z\",\"sampledValue\":[{\"value\":19759682110,\"context\":"
            "\"Transaction.End\",\"measurand\":\"Energy.Active.Import.Register\",\"unitOfMeasure\":{\"unit\":\"Wh\","
            "\"multiplier\":-6}}]}]}]}";
        require(out == expected, "the export, byte for byte:\n" + out);
    }

    // --- An abnormal stop and the monitor's own code kept next to the events ---
    {
        auto s = session("bay-04", 29, "2026-10-10T17:12:00Z", 1320000, 17111000000ull, "POWER_STAGE_FAULT");
        s.record.energy_lower_bound = false;
        std::string err;
        const std::string out = json_ocpp_events(s, err);
        require(
            contains(out, "\"triggerReason\":\"AbnormalCondition\"") && contains(out, "\"stoppedReason\":\"Other\""),
            "a fault is AbnormalCondition / Other");
        require(contains(out, "\"stop_reason\":\"POWER_STAGE_FAULT\""), "the monitor's own code is preserved");
    }
    {
        auto s = session("bay-02", 10, "2026-10-06T18:00:00Z", 1281000, 9168000000ull, "COMMUNICATION_TIMEOUT");
        s.record.billing_complete = false;
        s.record.energy_lower_bound = true;
        s.record.time_upper_bound = true;
        std::string err;
        const std::string out = json_ocpp_events(s, err);
        require(contains(out, "\"billing_complete\":false") && contains(out, "\"energy_lower_bound\":true") &&
                    contains(out, "\"time_upper_bound\":true"),
                "a held session says so in the wrapper");
        require(contains(out, "\"timeSpentCharging\":1281"), "timeSpentCharging is whole seconds of charging");
    }

    // --- Refusals: never invent a value ---
    {
        std::string err;
        auto s = session("bay-07", 1, "2026-10-05 02:19:00", 1000, 1, "STATUS_OFF");
        require(json_ocpp_events(s, err).empty() && contains(err, "made-up time"), "unparseable close label: " + err);
        s = session("bay-07", 1, "2026-10-05T02:19:00Z", -5, 1, "STATUS_OFF");
        require(json_ocpp_events(s, err).empty() && contains(err, "negative"), "negative duration: " + err);
        s = session("a-very-long-asset-identifier-x", 1234567, "2026-10-05T02:19:00Z", 1000, 1, "STATUS_OFF");
        require(json_ocpp_events(s, err).empty() && contains(err, "36 characters"), "transactionId too long: " + err);
        s = session("bay-07", 1, "2026-10-05T02:19:00Z", 1000, 9007199254740993ull, "STATUS_OFF");
        require(json_ocpp_events(s, err).empty() && contains(err, "2^53"), "energy beyond 2^53: " + err);
        s = session("bay-07", 1, "2026-10-05T02:19:00Z", 1000, 9007199254740992ull, "STATUS_OFF");
        require(!json_ocpp_events(s, err).empty(), "exactly 2^53 uWh is still exact in a double");
        // "<asset>-<id>" must fit OCPP's 36 characters: 34 + "-1" is exactly 36.
        s = session(std::string(34, 'a'), 1, "2026-10-05T02:19:00Z", 1000, 1, "STATUS_OFF");
        require(!json_ocpp_events(s, err).empty(), "34-char asset + '-1' = 36 characters is allowed");
        s = session(std::string(35, 'a'), 1, "2026-10-05T02:19:00Z", 1000, 1, "STATUS_OFF");
        require(json_ocpp_events(s, err).empty() && contains(err, "36 characters"), "37 characters is refused");
        s = session(std::string(32, 'a'), 123, "2026-10-05T02:19:00Z", 1000, 1, "STATUS_OFF");
        require(!json_ocpp_events(s, err).empty(), "32 + '-' + 3 digits = 36 is allowed");
    }

    // --- Hostile text cannot break the JSON ---
    {
        auto s = session("bay\"07", 1, "2026-10-05T02:19:00Z", 1000, 1, "STATUS_OFF");
        s.record.quality = "a\\b\nc";
        std::string err;
        const std::string out = json_ocpp_events(s, err);
        require(contains(out, "\"asset_id\":\"bay\\\"07\"") && contains(out, "\"transactionId\":\"bay\\\"07-1\""),
                "quotes are escaped");
        require(contains(out, "\"quality\":\"a\\\\b\\nc\""), "backslash and newline are escaped");
    }

    std::cout << "ocpp tests OK\n";
    return 0;
}
