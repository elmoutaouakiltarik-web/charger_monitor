#include "charger_reconcile.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace charger {
namespace {

constexpr std::int64_t kMicro = 1000000;

// Unsigned 128-bit counterpart of WideInt, aliased once so -Wpedantic does not
// fire at every use site (same approach as charger_monitor.hpp).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using WideUInt = unsigned __int128;
#pragma GCC diagnostic pop

bool asset_chars_ok(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char ch : id) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                        ch == '_' || ch == ':' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool parse_u64_strict(const std::string& s, std::uint64_t& out) {
    if (!all_digits(s)) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (0xffffffffffffffffull - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// digits [ '.' digits{1,6} ]: nothing parse_fixed_point would be lenient about.
bool decimal_shape_ok(const std::string& s) {
    const auto dot = s.find('.');
    if (dot == std::string::npos) return all_digits(s);
    const std::string whole = s.substr(0, dot), frac = s.substr(dot + 1);
    return all_digits(whole) && all_digits(frac) && frac.size() <= 6;
}

WideInt wabs(WideInt v) { return v < 0 ? -v : v; }

std::string neutralise_cell(const std::string& s) {
    std::string v = s;
    if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@' || v[0] == '\t' || v[0] == '\r')) {
        v.insert(v.begin(), '\'');
    }
    bool quote = false;
    for (char c : v) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') quote = true;
    }
    if (!quote) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += '"';
    return out;
}

std::string key_of(const std::string& asset, std::uint64_t id) { return asset + '\t' + std::to_string(id); }

}  // namespace

std::optional<std::vector<MeterReading>> parse_meter_csv(const std::string& text, std::string& error) {
    std::vector<MeterReading> out;
    std::map<std::string, std::uint64_t> seen;  // (asset, session) -> line
    bool header_seen = false;
    std::uint64_t lineno = 0;
    std::size_t pos = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;  // BOM from a spreadsheet export
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineno;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const auto fields = split_commas(line);
        const std::string where = "meter file line " + std::to_string(lineno) + ": ";
        if (!header_seen) {
            if (fields.size() != 3 || trim(fields[0]) != "asset_id" || trim(fields[1]) != "session_id" ||
                trim(fields[2]) != "meter_wh") {
                error = where + "the header must be exactly asset_id,session_id,meter_wh";
                return std::nullopt;
            }
            header_seen = true;
            continue;
        }
        if (line.find('"') != std::string::npos) {
            error = where + "quoted fields are not supported";
            return std::nullopt;
        }
        if (fields.size() != 3) {
            error = where + "expected 3 fields (asset_id,session_id,meter_wh), found " + std::to_string(fields.size());
            return std::nullopt;
        }
        MeterReading r;
        r.line = lineno;
        r.asset_id = trim(fields[0]);
        if (!asset_chars_ok(r.asset_id)) {
            error = where + "asset_id must be 1-64 characters from [A-Za-z0-9._:-]";
            return std::nullopt;
        }
        if (!parse_u64_strict(trim(fields[1]), r.session_id)) {
            error = where + "session_id must be a non-negative integer";
            return std::nullopt;
        }
        const std::string wh = trim(fields[2]);
        if (!decimal_shape_ok(wh)) {
            error = where + "meter_wh must be a non-negative decimal number of Wh with at most 6 decimals";
            return std::nullopt;
        }
        const auto micro = parse_fixed_point(wh, kMicro);
        if (!micro || *micro < 0) {
            error = where + "meter_wh is out of range";
            return std::nullopt;
        }
        r.meter_uwh = static_cast<std::uint64_t>(*micro);
        const auto key = key_of(r.asset_id, r.session_id);
        const auto dup = seen.find(key);
        if (dup != seen.end()) {
            error = where + "duplicate of " + r.asset_id + " #" + std::to_string(r.session_id) + " on line " +
                    std::to_string(dup->second);
            return std::nullopt;
        }
        seen.emplace(key, lineno);
        out.push_back(std::move(r));
    }
    if (!header_seen) {
        error = "meter file has no header: expected asset_id,session_id,meter_wh";
        return std::nullopt;
    }
    if (out.empty()) {
        error = "meter file has a header but no data rows";
        return std::nullopt;
    }
    return out;
}

std::string format_wide(WideInt value) {
    if (value == 0) return "0";
    const bool neg = value < 0;
    // Negate in the unsigned domain: -INT128_MIN is not representable.
    WideUInt u = neg ? static_cast<WideUInt>(0) - static_cast<WideUInt>(value) : static_cast<WideUInt>(value);
    std::string digits;
    while (u > 0) {
        digits += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) digits += '-';
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::string format_wh_from_uwh(WideInt micro_wh) {
    const bool neg = micro_wh < 0;
    WideUInt u = neg ? static_cast<WideUInt>(0) - static_cast<WideUInt>(micro_wh) : static_cast<WideUInt>(micro_wh);
    const WideUInt whole = u / static_cast<WideUInt>(kMicro);
    const auto frac = static_cast<std::uint64_t>(u % static_cast<WideUInt>(kMicro));
    std::string w = format_wide(static_cast<WideInt>(whole));
    std::string f = std::to_string(frac);
    f.insert(0, 6 - f.size(), '0');
    return (neg ? "-" : "") + w + "." + f;
}

ReconcileResult reconcile(const std::vector<SettledSession>& sessions, const std::vector<MeterReading>& readings,
                          std::uint64_t tolerance_ppm) {
    ReconcileResult res;
    res.summary.tolerance_ppm = tolerance_ppm;

    // The journal's sessions by (asset, id). An id that occurs twice (reused
    // after a restart in a journal written before ids were kept unique) is
    // ambiguous: it is refused as NOT_COMPARABLE rather than guessed.
    std::map<std::string, std::vector<const SettledSession*>> by_key;
    for (const auto& s : sessions) by_key[key_of(s.asset_id, s.id)].push_back(&s);

    std::vector<std::string> asset_order;
    std::map<std::string, ReconcileAssetSummary> per_asset;
    std::set<std::string> referenced;

    for (const auto& m : readings) {
        auto& a = per_asset[m.asset_id];
        if (a.asset_id.empty()) {
            a.asset_id = m.asset_id;
            asset_order.push_back(m.asset_id);
        }
        ReconcileRow row;
        row.asset_id = m.asset_id;
        row.session_id = m.session_id;
        row.meter_uwh = m.meter_uwh;
        const std::string key = key_of(m.asset_id, m.session_id);
        referenced.insert(key);
        const auto it = by_key.find(key);
        ++a.rows;
        if (it == by_key.end()) {
            row.status = "NO_SESSION";
            row.note = "the meter row names a session that is not closed in the verified journal";
            ++a.no_session;
            res.rows.push_back(std::move(row));
            continue;
        }
        const SettledSession& s = *it->second.front();
        row.has_ledger = true;
        row.ledger_uwh = s.record.energy_uwh;
        row.delta_uwh = static_cast<WideInt>(row.ledger_uwh) - static_cast<WideInt>(row.meter_uwh);
        if (it->second.size() > 1) {
            row.status = "NOT_COMPARABLE";
            row.note =
                "this (asset, session) occurs " + std::to_string(it->second.size()) +
                " times in the journal (ids restarted after a restart): cannot tell which one the meter row means";
            ++a.not_comparable;
            res.rows.push_back(std::move(row));
            continue;
        }
        if (s.record.energy_lower_bound || !s.record.billing_complete) {
            row.status = "NOT_COMPARABLE";
            std::ostringstream n;
            n << "ledger energy is ";
            if (s.record.energy_lower_bound) {
                n << "a lower bound";
            } else {
                n << "not billing_complete";
            }
            n << " (quality " << (s.record.quality.empty() ? "-" : s.record.quality) << ", stop "
              << (s.record.stop_reason.empty() ? "-" : s.record.stop_reason)
              << "): comparing it with a full meter reading would manufacture drift";
            row.note = n.str();
            ++a.not_comparable;
            res.rows.push_back(std::move(row));
            continue;
        }

        // Comparable: this row enters the statistics.
        ++a.compared;
        a.sum_ledger_uwh += static_cast<WideInt>(row.ledger_uwh);
        a.sum_meter_uwh += static_cast<WideInt>(row.meter_uwh);
        if (row.meter_uwh == 0) {
            if (row.ledger_uwh == 0) {
                row.ppm_defined = true;
                row.delta_ppm = 0;
                row.status = "OK";
            } else {
                row.ppm_defined = false;  // delta relative to zero is undefined: never divide, never invent
                row.status = "DRIFT";
                row.note = "the meter reports 0 Wh and the ledger does not: ppm is undefined";
            }
        } else {
            row.ppm_defined = true;
            row.delta_ppm = (row.delta_uwh * static_cast<WideInt>(kMicro)) / static_cast<WideInt>(row.meter_uwh);
            row.status = wabs(row.delta_ppm) <= static_cast<WideInt>(tolerance_ppm) ? "OK" : "DRIFT";
        }
        if (row.status == "OK")
            ++a.ok;
        else
            ++a.drift;
        if (row.ppm_defined) {
            ++a.ppm_rows;
            a.mean_abs_ppm += wabs(row.delta_ppm);  // divided below
            if (!a.has_worst || wabs(row.delta_ppm) > wabs(a.worst_ppm)) {
                a.has_worst = true;
                a.worst_ppm = row.delta_ppm;
            }
        }
        res.rows.push_back(std::move(row));
    }

    // Sessions the journal has but the meter file does not mention.
    std::set<std::string> counted;
    for (const auto& s : sessions) {
        const std::string key = key_of(s.asset_id, s.id);
        if (referenced.count(key) != 0) continue;
        if (!counted.insert(key).second) continue;
        ++res.summary.sessions_without_meter;
        const auto pa = per_asset.find(s.asset_id);
        if (pa != per_asset.end()) ++pa->second.sessions_without_meter;
    }

    for (const auto& id : asset_order) {
        auto a = per_asset[id];
        if (a.ppm_rows > 0) a.mean_abs_ppm /= static_cast<WideInt>(a.ppm_rows);
        if (a.compared > 0 && a.sum_meter_uwh > 0) {
            a.bias_defined = true;
            a.bias_ppm = ((a.sum_ledger_uwh - a.sum_meter_uwh) * static_cast<WideInt>(kMicro)) / a.sum_meter_uwh;
        }
        res.summary.rows += a.rows;
        res.summary.ok += a.ok;
        res.summary.drift += a.drift;
        res.summary.not_comparable += a.not_comparable;
        res.summary.no_session += a.no_session;
        res.summary.assets.push_back(a);
    }
    return res;
}

std::string reconcile_csv(const ReconcileResult& result) {
    std::ostringstream o;
    o << "asset_id,session_id,ledger_wh,meter_wh,delta_wh,delta_ppm,status,note\n";
    for (const auto& r : result.rows) {
        o << neutralise_cell(r.asset_id) << ',' << r.session_id << ',';
        if (r.has_ledger) {
            o << format_wh_from_uwh(static_cast<WideInt>(r.ledger_uwh)) << ','
              << format_wh_from_uwh(static_cast<WideInt>(r.meter_uwh)) << ',';
            if (r.status == "NOT_COMPARABLE") {
                o << ",,";  // a delta built from a known-partial figure is not offered
            } else {
                o << format_wh_from_uwh(r.delta_uwh) << ','
                  << (r.ppm_defined ? format_wide(r.delta_ppm) : std::string("-")) << ',';
            }
        } else {
            o << ',' << format_wh_from_uwh(static_cast<WideInt>(r.meter_uwh)) << ",,,";
        }
        o << r.status << ',' << neutralise_cell(r.note) << '\n';
    }
    return o.str();
}

std::string reconcile_summary_text(const ReconcileResult& result, const std::string& journal_path,
                                   const std::string& meter_path) {
    const auto& s = result.summary;
    std::ostringstream o;
    o << "\nReconciliation against an independent meter\n"
      << "  journal              " << journal_path << " (chain verified)\n"
      << "  meter file           " << meter_path << '\n'
      << "  tolerance            " << s.tolerance_ppm << " ppm (set by the operator; this tool never supplies one)\n"
      << "  meter rows           " << s.rows << '\n'
      << "  OK                   " << s.ok << '\n'
      << "  DRIFT                " << s.drift << '\n'
      << "  NOT_COMPARABLE       " << s.not_comparable << "  (lower-bound, held or ambiguous sessions: not compared)\n"
      << "  NO_SESSION           " << s.no_session << '\n'
      << "  journal sessions with no meter row  " << s.sessions_without_meter << '\n';
    for (const auto& a : s.assets) {
        o << "\n  " << a.asset_id << ": compared " << a.compared << " (OK " << a.ok << ", DRIFT " << a.drift
          << "), not comparable " << a.not_comparable << ", no session " << a.no_session
          << ", sessions without a meter row " << a.sessions_without_meter << '\n';
        if (a.ppm_rows > 0) {
            o << "      mean |delta| " << format_wide(a.mean_abs_ppm) << " ppm, worst " << format_wide(a.worst_ppm)
              << " ppm\n";
        } else {
            o << "      mean |delta| -, worst - (no comparable row with a defined ppm)\n";
        }
        if (a.bias_defined) {
            o << "      systematic bias " << format_wide(a.bias_ppm) << " ppm (ledger minus meter, over " << a.compared
              << " compared sessions)\n";
        } else {
            o << "      systematic bias - (nothing comparable, or the meter total is 0)\n";
        }
    }
    o << "\n  The systematic bias is an indicator of a scale or offset error in the CAN voltage/current chain,\n"
         "  not a calibration: the accuracy of each figure is bounded by the instrument behind it.\n";
    return o.str();
}

}  // namespace charger
