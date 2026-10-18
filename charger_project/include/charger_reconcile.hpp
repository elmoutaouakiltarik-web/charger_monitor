#pragma once

// Second opinion: reconcile the ledger against an independent meter.
//
// The ledger integrates V x I read off a CAN bus. A certified meter (or any
// other instrument the operator trusts) measures the same sessions by another
// route. Putting the two side by side is the cheapest way to notice a scaling
// error, a wrong DBC factor, a drifting sensor or a wiring fault long before a
// customer disputes an invoice.
//
// What this is NOT: a calibration, and not a verdict on which instrument is
// right. The accuracy of each figure is bounded by the instrument behind it,
// and the tolerance is the operator's to choose: this module never supplies a
// default threshold, because a threshold that nobody derived from an
// instrument's specification is a guess dressed up as a limit.
//
// All arithmetic is integer (microwatt-hours and parts per million, 128-bit
// intermediates): a reconciliation that drifts by itself is no reconciliation.
//
// A session whose ledger energy is a known lower bound (a measurement gap, a
// communication timeout, too few samples, an interrupted session) is reported
// as NOT_COMPARABLE rather than as drift: comparing a known-partial figure
// with a full meter reading would manufacture a discrepancy that is not there.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "charger_ledger.hpp"
#include "charger_monitor.hpp"

namespace charger {

// One row of the independent meter's export: the energy it attributes to a
// session, in microwatt-hours (parsed from decimal Wh without floating point).
struct MeterReading {
    std::string asset_id;
    std::uint64_t session_id{0};
    std::uint64_t meter_uwh{0};
    std::uint64_t line{0};  // physical line in the CSV, for messages
};

// Parses `asset_id,session_id,meter_wh` (header required, exactly that).
//   * CRLF and a leading UTF-8 byte-order mark are tolerated; blank lines and
//     lines whose first non-blank character is '#' are ignored;
//   * meter_wh is a non-negative decimal number of Wh with at most 6 decimals
//     (digits, optionally '.' and 1-6 more digits; no sign, no exponent);
//   * asset_id must be 1-64 characters from [A-Za-z0-9._:-], as the monitor
//     itself enforces; session_id is a non-negative integer;
//   * a duplicate (asset, session) is refused, and so is a file with no data row.
// On any problem it returns nullopt and `error` names the physical line.
std::optional<std::vector<MeterReading>> parse_meter_csv(const std::string& text, std::string& error);

// Row status.
//   OK              |delta_ppm| <= tolerance
//   DRIFT           |delta_ppm| >  tolerance, or the meter says 0 and the ledger does not
//                   (the ppm figure is then undefined and printed as "-")
//   NOT_COMPARABLE  the ledger figure is a lower bound or the session is not
//                   billing_complete, or the (asset, id) occurs more than once in the journal
//   NO_SESSION      the meter row names a session that is not closed in the verified journal
struct ReconcileRow {
    std::string asset_id;
    std::uint64_t session_id{0};
    bool has_ledger{false};
    std::uint64_t ledger_uwh{0};
    std::uint64_t meter_uwh{0};
    // ledger - meter, exact (may exceed int64 for corrupt inputs, hence WideInt).
    WideInt delta_uwh{0};
    // delta relative to the METER value, parts per million, truncated toward zero.
    bool ppm_defined{false};
    WideInt delta_ppm{0};
    std::string status;
    std::string note;
};

struct ReconcileAssetSummary {
    std::string asset_id;
    std::uint64_t rows{0};
    std::uint64_t ok{0};
    std::uint64_t drift{0};
    std::uint64_t not_comparable{0};
    std::uint64_t no_session{0};
    // Closed sessions of this asset in the journal that no meter row refers to.
    std::uint64_t sessions_without_meter{0};
    // Over the OK and DRIFT rows whose ppm is defined.
    std::uint64_t ppm_rows{0};
    WideInt mean_abs_ppm{0};  // truncated toward zero
    bool has_worst{false};
    WideInt worst_ppm{0};  // signed, the one with the largest magnitude (first on a tie)
    // Over all OK and DRIFT rows: (sum ledger - sum meter) * 1e6 / sum meter.
    // An indicator of a scale or offset error in the V/I chain, not a calibration.
    std::uint64_t compared{0};
    WideInt sum_ledger_uwh{0};
    WideInt sum_meter_uwh{0};
    bool bias_defined{false};
    WideInt bias_ppm{0};
};

struct ReconcileSummary {
    std::uint64_t tolerance_ppm{0};
    std::uint64_t rows{0};
    std::uint64_t ok{0};
    std::uint64_t drift{0};
    std::uint64_t not_comparable{0};
    std::uint64_t no_session{0};
    std::uint64_t sessions_without_meter{0};
    std::vector<ReconcileAssetSummary> assets;  // assets that have at least one meter row, in file order
};

struct ReconcileResult {
    std::vector<ReconcileRow> rows;  // in the order of the meter file
    ReconcileSummary summary;
};

ReconcileResult reconcile(const std::vector<SettledSession>& sessions, const std::vector<MeterReading>& readings,
                          std::uint64_t tolerance_ppm);

// Exact decimal renderings, used by the CSV and by the tests.
std::string format_wide(WideInt value);            // signed integer
std::string format_wh_from_uwh(WideInt micro_wh);  // "4800.000000", sign-aware, six decimals

// asset_id,session_id,ledger_wh,meter_wh,delta_wh,delta_ppm,status,note
// Text cells that a spreadsheet would execute as a formula are neutralised with
// a leading apostrophe: the asset id comes from a file the operator did not write.
std::string reconcile_csv(const ReconcileResult& result);

// Human-readable summary, with the standing caveat about what it does not prove.
std::string reconcile_summary_text(const ReconcileResult& result, const std::string& journal_path,
                                   const std::string& meter_path);

}  // namespace charger
