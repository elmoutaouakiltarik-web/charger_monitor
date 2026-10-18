#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_reconcile.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

SettledSession session(const std::string& asset, std::uint64_t id, std::uint64_t uwh, bool complete = true,
                       bool lower = false, const std::string& quality = "OK", const std::string& stop = "STATUS_OFF") {
    SettledSession s;
    s.asset_id = asset;
    s.id = id;
    s.record.energy_uwh = uwh;
    s.record.billing_complete = complete;
    s.record.energy_lower_bound = lower;
    s.record.quality = quality;
    s.record.stop_reason = stop;
    return s;
}

constexpr std::uint64_t kWh = 1000000ull;  // micro-Wh per Wh

MeterReading reading(const std::string& asset, std::uint64_t id, std::uint64_t uwh) {
    MeterReading r;
    r.asset_id = asset;
    r.session_id = id;
    r.meter_uwh = uwh;
    return r;
}

const ReconcileRow* row_of(const ReconcileResult& r, const std::string& asset, std::uint64_t id) {
    for (const auto& x : r.rows) {
        if (x.asset_id == asset && x.session_id == id) return &x;
    }
    return nullptr;
}

bool refuses(const std::string& text, const std::string& must_contain) {
    std::string err;
    const auto r = parse_meter_csv(text, err);
    return !r && err.find(must_contain) != std::string::npos;
}

}  // namespace

int main() {
    // --- The meter file: what it accepts ---
    {
        std::string err;
        const std::string text =
            "\xEF\xBB\xBF# exported from the station meter\r\n"
            "\r\n"
            "asset_id,session_id,meter_wh\r\n"
            "bay-07,1,4800\r\n"
            "  bay-07 , 2 , 4800.5  \r\n"
            "# a comment in the middle\n"
            "bay-08,10,0.000001\n"
            "bay-08,11,12";  // no trailing newline
        const auto r = parse_meter_csv(text, err);
        require(r.has_value(), "a valid file with BOM, CRLF, comments and blanks parses: " + err);
        require(r->size() == 4, "four data rows");
        require((*r)[0].asset_id == "bay-07" && (*r)[0].session_id == 1 && (*r)[0].meter_uwh == 4800ull * kWh,
                "4800 Wh is 4 800 000 000 uWh, exactly");
        require((*r)[1].meter_uwh == 4800500000ull && (*r)[1].session_id == 2, "fields are trimmed; 4800.5 Wh exact");
        require((*r)[2].meter_uwh == 1, "0.000001 Wh is 1 uWh: six decimals, no float round trip");
        require((*r)[3].meter_uwh == 12ull * kWh && (*r)[3].line == 8,
                "last line without a newline; physical line number kept");
    }

    // --- The meter file: what it refuses, with the line number ---
    require(refuses("asset,session_id,meter_wh\nbay-07,1,1\n", "line 1"), "wrong header");
    require(refuses("# c\n\nasset_id,session_id\nbay-07,1,1\n", "line 3"), "short header, counted physically");
    require(refuses("", "no header"), "empty file");
    require(refuses("asset_id,session_id,meter_wh\n", "no data rows"), "header only");
    require(refuses("asset_id,session_id,meter_wh\n\n# nothing\n", "no data rows"), "comments only");
    require(refuses("asset_id,session_id,meter_wh\nbay-07,1\n", "line 2"), "two fields");
    require(refuses("asset_id,session_id,meter_wh\nbay-07,1,2,3\n", "expected 3 fields"), "four fields");
    require(refuses("asset_id,session_id,meter_wh\n\"bay-07\",1,5\n", "quoted"), "quoted fields");
    require(refuses("asset_id,session_id,meter_wh\nbay 07,1,5\n", "asset_id"), "a space inside an asset id");
    require(refuses("asset_id,session_id,meter_wh\n=cmd,1,5\n", "asset_id"), "a formula as an asset id");
    require(refuses("asset_id,session_id,meter_wh\n,1,5\n", "asset_id"), "empty asset id");
    require(refuses("asset_id,session_id,meter_wh\nbay-07,-1,5\n", "session_id"), "negative session id");
    require(refuses("asset_id,session_id,meter_wh\nbay-07,x,5\n", "session_id"), "non numeric session id");
    require(refuses("asset_id,session_id,meter_wh\nbay-07,18446744073709551616,5\n", "session_id"),
            "a session id beyond uint64 is refused, not wrapped");
    for (const char* bad : {"-5", "+5", "1e3", ".5", "5.", "1.1234567", "abc", "", "0x10", "1,5", "5 5"}) {
        require(refuses(std::string("asset_id,session_id,meter_wh\nbay-07,1,") + bad + "\n", "line 2"),
                std::string("meter_wh '") + bad + "' is refused");
    }
    require(refuses("asset_id,session_id,meter_wh\nbay-07,1,9223372036854.775808\n", "line 2"),
            "a value beyond int64 micro-Wh is refused");
    {
        std::string err;
        const auto dup = parse_meter_csv("asset_id,session_id,meter_wh\nbay-07,3,1\nbay-08,3,1\nbay-07,3,2\n", err);
        require(!dup && err.find("line 4") != std::string::npos && err.find("line 2") != std::string::npos,
                "a duplicate names both lines: " + err);
    }

    // --- Exact numbers: sign, truncation toward zero, the tolerance boundary ---
    {
        const std::vector<SettledSession> js = {session("bay-07", 1, 4800ull * kWh)};
        // meter 4810 Wh: delta = -10 Wh = -10 000 000 uWh; ppm = -1e13 / 4.81e9 = -2079.002 -> -2079
        const auto at = [&](std::uint64_t tol) { return reconcile(js, {reading("bay-07", 1, 4810ull * kWh)}, tol); };
        auto r = at(2079);
        require(r.rows.size() == 1 && r.rows[0].delta_uwh == -10000000, "delta is ledger minus meter, signed");
        require(r.rows[0].ppm_defined && r.rows[0].delta_ppm == -2079,
                "ppm truncates toward zero: -2079 (2079.002), not -2080");
        require(r.rows[0].status == "OK", "|ppm| equal to the tolerance is OK");
        require(at(2078).rows[0].status == "DRIFT", "one ppm tighter is DRIFT");
        require(at(0).rows[0].status == "DRIFT", "tolerance 0 is allowed and strict");
        // meter 4790 Wh: delta = +10 Wh; ppm = 1e13 / 4.79e9 = 2087.68 -> 2087
        const auto p = reconcile(js, {reading("bay-07", 1, 4790ull * kWh)}, 5000);
        require(p.rows[0].delta_ppm == 2087 && p.rows[0].status == "OK", "positive delta: +2087 ppm");
        const auto exact = reconcile(js, {reading("bay-07", 1, 4800ull * kWh)}, 0);
        require(exact.rows[0].delta_ppm == 0 && exact.rows[0].status == "OK",
                "identical readings are OK even at tolerance 0");
        require(exact.summary.drift == 0 && exact.summary.ok == 1, "summary counts");
    }

    // --- A zero meter: ppm is undefined, never divided ---
    {
        const std::vector<SettledSession> js = {session("a", 1, 5 * kWh), session("a", 2, 0)};
        const auto r = reconcile(js, {reading("a", 1, 0), reading("a", 2, 0)}, 1000000);
        require(r.rows[0].status == "DRIFT" && !r.rows[0].ppm_defined, "meter 0, ledger 5 Wh: DRIFT, ppm undefined");
        require(r.rows[1].status == "OK" && r.rows[1].ppm_defined && r.rows[1].delta_ppm == 0, "both zero: OK, ppm 0");
        const std::string csv = reconcile_csv(r);
        require(
            csv.find("a,1,5.000000,0.000000,5.000000,-,DRIFT,the meter reports 0 Wh and the ledger does not: ppm is "
                     "undefined\n") != std::string::npos,
            "undefined ppm prints as '-': " + csv);
        require(r.summary.assets[0].ppm_rows == 1, "an undefined ppm does not enter the ppm statistics");
        require(r.summary.assets[0].bias_defined == false, "the meter total is 0: the bias is undefined");
    }

    // --- Lower bounds and ambiguity are not compared ---
    {
        const std::vector<SettledSession> js = {
            session("bay-07", 1, 100 * kWh, false, true, "GAPS", "STATUS_OFF"),
            session("bay-07", 2, 100 * kWh, false, false, "ZERO_ENERGY", "STATUS_OFF"),
            session("bay-07", 3, 100 * kWh, true, true, "OK", "COMMUNICATION_TIMEOUT"),
            session("bay-07", 4, 100 * kWh),
            session("bay-07", 4, 200 * kWh),  // id reused after a restart
            session("bay-07", 5, 100 * kWh),
        };
        const auto r = reconcile(
            js,
            {reading("bay-07", 1, 150 * kWh), reading("bay-07", 2, 150 * kWh), reading("bay-07", 3, 150 * kWh),
             reading("bay-07", 4, 100 * kWh), reading("bay-07", 99, 150 * kWh)},
            10);
        for (std::uint64_t id : {1, 2, 3, 4}) {
            const auto* x = row_of(r, "bay-07", id);
            require(x != nullptr && x->status == "NOT_COMPARABLE",
                    "session " + std::to_string(id) + " is not compared");
            require(!x->note.empty(), "and it says why");
        }
        require(row_of(r, "bay-07", 1)->note.find("lower bound") != std::string::npos, "lower bound named");
        require(row_of(r, "bay-07", 2)->note.find("not billing_complete") != std::string::npos, "held named");
        require(row_of(r, "bay-07", 4)->note.find("occurs 2 times") != std::string::npos, "ambiguity named");
        require(row_of(r, "bay-07", 99)->status == "NO_SESSION", "a meter row with no session");
        require(r.summary.drift == 0 && r.summary.ok == 0 && r.summary.not_comparable == 4 && r.summary.no_session == 1,
                "no manufactured drift");
        require(r.summary.sessions_without_meter == 1, "session 5 has no meter row (id 4 counted once)");
        require(r.summary.assets[0].compared == 0 && !r.summary.assets[0].bias_defined, "nothing comparable, no bias");
        const std::string csv = reconcile_csv(r);
        require(csv.find("bay-07,1,100.000000,150.000000,,,NOT_COMPARABLE,") != std::string::npos,
                "no delta is offered for a non-comparable row: " + csv);
        require(csv.find("bay-07,99,,150.000000,,,NO_SESSION,") != std::string::npos, "NO_SESSION row layout");
    }

    // --- Per-asset statistics and the systematic bias ---
    {
        const std::vector<SettledSession> js = {session("bay-07", 1, 4800 * kWh), session("bay-07", 2, 4800 * kWh),
                                                session("bay-07", 3, 4800 * kWh), session("bay-07", 4, 1000 * kWh),
                                                session("bay-08", 1, 500 * kWh),  session("bay-08", 9, 1 * kWh)};
        const auto r = reconcile(
            js,
            {reading("bay-07", 1, 4800 * kWh), reading("bay-07", 2, 4810 * kWh), reading("bay-07", 3, 4790 * kWh),
             reading("bay-07", 4, 1001 * kWh), reading("bay-08", 1, 500 * kWh)},
            3000);
        require(r.summary.assets.size() == 2 && r.summary.assets[0].asset_id == "bay-07", "assets in file order");
        const auto& a = r.summary.assets[0];
        require(a.rows == 4 && a.ok == 4 && a.drift == 0 && a.compared == 4, "bay-07 counts");
        // ppm: 0, -2079, +2087, -999  -> mean |.| = (0+2079+2087+999)/4 = 1291 (trunc), worst = +2087
        require(a.ppm_rows == 4 && a.mean_abs_ppm == 1291, "mean absolute ppm");
        require(a.has_worst && a.worst_ppm == 2087, "worst ppm keeps its sign");
        // bias: ledger 15400 Wh vs meter 15401 Wh -> -1e12 / 15.401e9 = -64.93 -> -64
        require(a.bias_defined && a.bias_ppm == -64, "systematic bias, exact and truncated toward zero");
        require(r.summary.assets[1].asset_id == "bay-08" && r.summary.assets[1].sessions_without_meter == 1,
                "per-asset sessions without a meter row");
        require(r.summary.sessions_without_meter == 1 && r.summary.rows == 5 && r.summary.ok == 5, "totals");
        const std::string text = reconcile_summary_text(r, "j.journal", "m.csv");
        require(text.find("tolerance            3000 ppm") != std::string::npos, "tolerance echoed");
        require(text.find("systematic bias -64 ppm") != std::string::npos, "bias printed");
        require(text.find("not a calibration") != std::string::npos, "the standing caveat is printed");
    }

    // --- Values near the top of uint64 must not overflow ---
    {
        constexpr std::uint64_t kMax = 18446744073709551615ull;
        const std::vector<SettledSession> js = {session("big", 1, kMax), session("big", 2, kMax)};
        const auto r = reconcile(js, {reading("big", 1, kMax - 1), reading("big", 2, 1)}, 1);
        require(r.rows[0].delta_uwh == 1 && r.rows[0].delta_ppm == 0 && r.rows[0].status == "OK",
                "UINT64_MAX vs UINT64_MAX-1: +1 uWh, 0 ppm");
        require(r.rows[1].delta_ppm == static_cast<WideInt>(kMax - 1) * 1000000, "ppm is a 128-bit quantity");
        require(r.rows[1].status == "DRIFT", "and it is DRIFT");
        require(format_wh_from_uwh(static_cast<WideInt>(kMax)) == "18446744073709.551615",
                "UINT64_MAX uWh in Wh, exact");
        const std::string csv = reconcile_csv(r);
        require(csv.find("18446744073709.551614") != std::string::npos, "the big delta prints exactly");
        require(csv.find("18446744073709551614000000") != std::string::npos, "the big ppm prints exactly");
        const auto& a = r.summary.assets[0];
        require(a.sum_ledger_uwh == static_cast<WideInt>(kMax) * 2, "sums are 128-bit");
        require(a.bias_defined, "bias is defined");
    }

    // --- Formatting ---
    {
        require(format_wide(0) == "0" && format_wide(-7) == "-7" && format_wide(1234567890123) == "1234567890123",
                "format_wide");
        WideInt most_negative = static_cast<WideInt>(1) << 126;
        most_negative = -most_negative * 2;  // INT128_MIN
        require(format_wide(most_negative) == "-170141183460469231731687303715884105728", "INT128_MIN formats");
        require(format_wh_from_uwh(0) == "0.000000" && format_wh_from_uwh(1) == "0.000001" &&
                    format_wh_from_uwh(-1500000) == "-1.500000" && format_wh_from_uwh(4800000000) == "4800.000000",
                "six decimals, sign aware");
    }

    // --- The CSV never carries a live formula, whatever the meter file said ---
    {
        const auto r = reconcile({}, {reading("=1+1", 1, kWh), reading("@x", 2, kWh), reading("-9", 3, kWh)}, 1);
        const std::string csv = reconcile_csv(r);
        require(csv.find("\n'=1+1,1,") != std::string::npos, "= is neutralised");
        require(csv.find("\n'@x,2,") != std::string::npos, "@ is neutralised");
        require(csv.find("\n'-9,3,") != std::string::npos, "- is neutralised");
    }

    // --- Full CSV, exact bytes ---
    {
        const std::vector<SettledSession> js = {session("bay-07", 1, 4800 * kWh)};
        const auto r = reconcile(js, {reading("bay-07", 1, 4810 * kWh)}, 5000);
        require(reconcile_csv(r) ==
                    "asset_id,session_id,ledger_wh,meter_wh,delta_wh,delta_ppm,status,note\n"
                    "bay-07,1,4800.000000,4810.000000,-10.000000,-2079,OK,\n",
                "the CSV, byte for byte: " + reconcile_csv(r));
    }

    std::cout << "reconcile tests OK\n";
    return 0;
}
