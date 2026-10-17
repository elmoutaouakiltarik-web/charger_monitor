#pragma once

// Time-of-use tariff windows.
//
// Industrial and fleet energy is billed by wall-clock window, not by elapsed
// time: an hour of charging between 02:00 and 03:00 costs a different amount
// from the same hour between 17:00 and 18:00. A single flat rate cannot express
// that, and it is one of the most common reasons a settlement is disputed.
//
// This engine splits one session into charging periods by window and prices
// each period with its own rate. It keeps the project rule: window boundaries
// are resolved from UTC wall-clock seconds supplied by the caller, and every
// quantity stays an exact integer.
//
// The monitor core stays deterministic and clock-injected. This module is the
// bridge that turns injected timestamps into UTC seconds.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "charger_monitor.hpp"

namespace charger {

// One daily window, in UTC. start and end are seconds from midnight.
// end > start means a same-day window; end < start wraps past midnight;
// end == start is degenerate and matches nothing, which TimeOfUse::validate
// rejects.
struct TimeWindow {
    std::string name;
    std::int32_t start_s{0};
    std::int32_t end_s{0};
    std::int64_t energy_micro_eur_per_kwh{0};
    std::int64_t time_micro_eur_per_hour{0};

    // True when a second-of-day falls in this window. Handles wrapping.
    bool contains(std::int32_t second_of_day) const;
};

// A tariff made of mutually exclusive windows plus an optional fallback that
// applies outside every window. The fallback is what makes the engine safe:
// an uncovered second is flagged, never silently free.
struct TimeOfUse {
    std::vector<TimeWindow> windows;
    std::optional<TimeWindow> fallback;

    // The window covering a second of day, or nullopt when nothing matches.
    const TimeWindow* at(std::int32_t second_of_day) const;

    // Rejects a tariff that cannot be priced correctly: a window that is
    // degenerate or out of range, a negative rate, or two windows covering the
    // same second of day (which would silently price at whichever comes first).
    // price_time_of_use does not call this, so an application should validate a
    // tariff it did not assemble itself.
    bool validate(std::string* error = nullptr) const;
};

// One contiguous stretch of a session priced at a single rate.
struct ChargingPeriod {
    std::string window;
    std::int64_t start_offset_ms{0};
    std::int64_t end_offset_ms{0};
    std::int64_t duration_ms{0};
    std::int64_t energy_uwh{0};
    std::int64_t energy_wh{0};
    std::int64_t billed_energy_wh{0};
    std::int64_t charging_s{0};
    std::int64_t billed_time_s{0};
    std::int64_t energy_cost_micro_eur{0};
    std::int64_t time_cost_micro_eur{0};
    std::int64_t total_cost_micro_eur{0};
    // True when this period's cost did not fit in 64 bits. The cost fields are
    // then saturated, never truncated and never left at zero: free energy would
    // be the worst possible failure direction on a billing channel.
    bool price_overflow{false};
};

struct TimeOfUseResult {
    std::vector<ChargingPeriod> periods;
    // Always equal to the sum of periods[].energy_uwh: the parts are apportioned
    // with a carried remainder, so no interval can lose or invent microwatt-hours
    // at a window boundary.
    std::int64_t total_energy_uwh{0};
    std::int64_t total_energy_cost_micro_eur{0};
    std::int64_t total_time_cost_micro_eur{0};
    std::int64_t total_cost_micro_eur{0};
    std::int64_t flat_cost_micro_eur{0};
    // Seconds of the session that no window and no fallback covered. Must be
    // zero for a complete result; non-zero means the tariff has a hole. Such
    // time is priced at zero cost, so a caller that ignores this field will
    // under-bill. Check it.
    std::int64_t unpriced_ms{0};
    // Milliseconds of interval that were not priced at all because the samples
    // were out of order or the energy register ran backwards. Their energy is
    // excluded from total_energy_uwh.
    std::int64_t dropped_ms{0};
    // True when any period cost overflowed; the totals are saturated.
    bool price_overflow{false};
};

// Splits a session into periods by window and prices each one.
//
// samples must be ordered by time. Each entry carries the session-relative
// offset, the cumulative energy in uWh and the UTC second of the day at that
// offset. Intervals are walked in one-second slices, so a window boundary
// inside a sample interval splits it exactly instead of billing the whole
// interval at whichever rate happened to be current when it started. Each slice
// receives the difference of the cumulative apportionment, so the slices of an
// interval sum back to the interval total exactly.
struct TimeSample {
    std::int64_t offset_ms{0};
    std::int64_t energy_uwh{0};
    std::int32_t second_of_day{0};
};

TimeOfUseResult price_time_of_use(const TimeOfUse& tou, const std::vector<TimeSample>& samples,
                                  std::int64_t flat_micro_eur, std::int32_t energy_step_wh, std::int32_t time_step_s);

// Converts a UTC timestamp in seconds since the Unix epoch to a second of day.
// Negative values (pre-1970) are floored, not truncated, so the result is
// always in [0, 86400).
std::int32_t second_of_day_utc(std::int64_t epoch_seconds);

// File-based TOU tariff, so the rate that priced a settlement is a reviewable
// document rather than a CLI flag. JSON shape (all rates are decimal strings
// with at most 6 fractional digits, parsed with parse_fixed_point so the
// operator's text reaches the integer tariff without a binary float round
// trip):
//   {"schema":"charger.tou.v1","windows":[
//     {"name":"OFFPEAK","start":"00:00","end":"08:00",
//      "energy_eur_per_kwh":"0.15","time_eur_per_hour":"2.00"}, ...],
//    "fallback":{"name":"FLAT","energy_eur_per_kwh":"0.25","time_eur_per_hour":"2.50"}}
// start/end are "HH:MM" UTC ("24:00" allowed only as an end alias for 00:00
// next day is rejected: use "00:00" with wrap instead). Unknown fields are
// errors: a misspelled rate must never silently stay at zero.
struct TouLoadResult {
    bool ok{false};
    TimeOfUse tou{};
    std::string error;
};

TouLoadResult parse_tou_json(const std::string& text);
std::string json_tou_example();

// Coverage of the 24 h day: how many seconds are priced (window or fallback)
// and how many are holes priced at zero. A hole is a tariff defect, not a
// discount: settlement must refuse it rather than under-bill.
struct TouCoverage {
    std::int32_t covered_s{0};
    std::int32_t uncovered_s{0};
    bool full{false};
};

TouCoverage tou_coverage(const TimeOfUse& tou);

// Strict settlement gate. Validates the tariff, then refuses to price when the
// tariff is incomplete: if any second of the day is uncovered (no window and
// no fallback covers it) the result is nullopt with an explicit reason.
// A caller that ignores unpriced_ms would bill that time at zero.
std::optional<TimeOfUseResult> price_time_of_use_strict(const TimeOfUse& tou,
                                                         const std::vector<TimeSample>& samples,
                                                         std::int64_t flat_micro_eur, std::int32_t energy_step_wh,
                                                         std::int32_t time_step_s, std::string& error);

}  // namespace charger