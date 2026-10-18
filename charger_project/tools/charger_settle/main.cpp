// charger_settle — billing settlement report from a durable journal.
//
// The finance question is never "what happened" but "what may I invoice, and
// why is the rest held?". This tool reads the hash-chained journal, verifies it,
// and emits a reconciliation CSV separating billable sessions from held ones,
// with the reason each held session was held.
//
//   charger_settle <journal>                 CSV to stdout
//   charger_settle <journal> --summary       human-readable totals
//   charger_settle <journal> --out FILE.csv  write to a file
//   charger_settle <journal> --dispute ASSET SESSION_ID
//                                            complete dispute evidence pack
//                                            (charger.dispute.v1 JSON)
//   charger_settle <journal> --cdr ASSET SESSION_ID
//                                            OCPI CDR subset (charger.cdr.v1 JSON)
//   charger_settle <journal> --metrics [--out FILE.prom]
//                                            fleet rollup as Prometheus text (charger_portfolio_*,
//                                            charger_asset_*); --out replaces FILE atomically, so it can
//                                            feed the node_exporter textfile collector from cron
//   charger_settle <journal> --ocpp ASSET SESSION_ID [--out FILE]
//                                            the Started/Ended OCPP 2.0.1 TransactionEvent payloads for one
//                                            session (charger.ocpp-events.v1; a file export, no transport)
//   charger_settle <journal> --reconcile METER.csv --tolerance-ppm N [--out FILE.csv] [--summary]
//                                            second opinion: compare session energy with an independent
//                                            meter's export (asset_id,session_id,meter_wh). The tolerance is
//                                            REQUIRED: the tool never invents a threshold
//   charger_settle <journal> --check-tou tou.json
//                                            validate a TOU tariff file and its 24 h coverage
//   charger_settle <journal> --tou-settle tou.json [--summary] [--out FILE]
//                                            exact ledger CSV plus a TOU *estimate* column
//                                            (constant-power apportionment, labelled, never the invoice)
//
// Exit codes: 0 settled, 2 usage or verification error, 3 journal altered, 4 reconciliation found drift.

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include <charger_export.hpp>
#include <charger_ledger.hpp>
#include <charger_monitor.hpp>
#include <charger_ocpp.hpp>
#include <charger_reconcile.hpp>
#include <charger_tou.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace charger;

namespace {

struct Options {
    std::string journal;
    std::string out;
    bool summary{false};
    bool verify_only{false};
    bool metrics{false};
    // Dispute pack: --dispute ASSET SESSION_ID.
    bool dispute{false};
    std::string dispute_asset;
    std::uint64_t dispute_session{0};
    // OCPI CDR subset: --cdr ASSET SESSION_ID.
    bool cdr{false};
    std::string cdr_asset;
    std::uint64_t cdr_session{0};
    // OCPP 2.0.1 TransactionEvent export: --ocpp ASSET SESSION_ID.
    bool ocpp{false};
    std::string ocpp_asset;
    std::uint64_t ocpp_session{0};
    // Reconciliation against an independent meter: --reconcile FILE --tolerance-ppm N.
    std::string reconcile;
    bool tolerance_set{false};
    std::uint64_t tolerance_ppm{0};
    // TOU tariff file: --check-tou FILE validates only; --tou-settle FILE adds
    // the estimate column to the settlement CSV.
    std::string check_tou;
    std::string tou_settle;
};

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

// Digits only, no overflow: a tolerance that wrapped around would silently
// change which sessions count as drift.
bool parse_ppm(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (0xffffffffffffffffull - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool parse_args(int argc, char** argv, Options& opt, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--summary") {
            opt.summary = true;
        } else if (a == "--verify-only") {
            opt.verify_only = true;
        } else if (a == "--metrics") {
            opt.metrics = true;
        } else if (a == "--dispute") {
            opt.dispute = true;
            if (i + 2 >= argc) {
                err = "--dispute needs ASSET SESSION_ID, e.g. --dispute bay-07 12";
                return false;
            }
            opt.dispute_asset = argv[++i];
            if (!parse_u64(argv[++i], opt.dispute_session)) {
                err = "the session id must be a non-negative integer";
                return false;
            }
        } else if (a == "--cdr") {
            opt.cdr = true;
            if (i + 2 >= argc) {
                err = "--cdr needs ASSET SESSION_ID, e.g. --cdr bay-07 12";
                return false;
            }
            opt.cdr_asset = argv[++i];
            if (!parse_u64(argv[++i], opt.cdr_session)) {
                err = "the session id must be a non-negative integer";
                return false;
            }
        } else if (a == "--ocpp") {
            opt.ocpp = true;
            if (i + 2 >= argc) {
                err = "--ocpp needs ASSET SESSION_ID, e.g. --ocpp bay-07 12";
                return false;
            }
            opt.ocpp_asset = argv[++i];
            if (!parse_u64(argv[++i], opt.ocpp_session)) {
                err = "the session id must be a non-negative integer";
                return false;
            }
        } else if (a == "--reconcile") {
            if (i + 1 >= argc) {
                err = "--reconcile needs a meter CSV (asset_id,session_id,meter_wh)";
                return false;
            }
            opt.reconcile = argv[++i];
        } else if (a == "--tolerance-ppm") {
            if (i + 1 >= argc || !parse_ppm(argv[i + 1], opt.tolerance_ppm)) {
                err = "--tolerance-ppm needs a non-negative integer (parts per million)";
                return false;
            }
            ++i;
            opt.tolerance_set = true;
        } else if (a == "--check-tou") {
            if (i + 1 >= argc) {
                err = "--check-tou needs a tariff file";
                return false;
            }
            opt.check_tou = argv[++i];
        } else if (a == "--tou-settle") {
            if (i + 1 >= argc) {
                err = "--tou-settle needs a tariff file";
                return false;
            }
            opt.tou_settle = argv[++i];
        } else if (a == "--out") {
            if (i + 1 >= argc) {
                err = "--out needs a path";
                return false;
            }
            opt.out = argv[++i];
        } else if (a.rfind("--", 0) == 0) {
            err = "unrecognised option: " + a;
            return false;
        } else if (opt.journal.empty()) {
            opt.journal = a;
        } else {
            err = "unexpected argument: " + a;
            return false;
        }
    }
    if (opt.journal.empty()) {
        err = "a journal path is required";
        return false;
    }
    if (!opt.reconcile.empty() && !opt.tolerance_set) {
        err =
            "--reconcile needs --tolerance-ppm N: the tolerance is yours to set from your instruments and your "
            "contract, and this tool never invents one";
        return false;
    }
    if (opt.tolerance_set && opt.reconcile.empty()) {
        err = "--tolerance-ppm only applies to --reconcile";
        return false;
    }
    if (opt.ocpp && !opt.reconcile.empty()) {
        err = "--ocpp and --reconcile cannot be combined";
        return false;
    }
    return true;
}

std::string csv_escape(const std::string& s) {
    bool needs_quotes = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n') needs_quotes = true;
    }
    if (!needs_quotes) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += '"';
    return out;
}

// Parses "YYYY-MM-DDTHH:MM:SSZ" (UTC, as written by utc_timestamp()) into
// epoch seconds. Returns false rather than guessing: a TOU estimate built on
// a misread clock would silently move energy into the wrong window.
bool parse_close_utc(const std::string& s, std::int64_t& epoch) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') {
        return false;
    }
    const auto digits = [&](std::size_t pos, std::size_t n, int& out) {
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    if (!digits(0, 4, Y) || !digits(5, 2, M) || !digits(8, 2, D) || !digits(11, 2, h) || !digits(14, 2, m) ||
        !digits(17, 2, sec)) {
        return false;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return false;
    // Days-from-civil (Howard Hinnant) to epoch, UTC, no libc timezone state.
    const int y = M <= 2 ? Y - 1 : Y;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((M + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(D - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    epoch = days * 86400 + h * 3600 + m * 60 + sec;
    return true;
}

std::string read_file_text(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    ok = true;
    return buf.str();
}

// TOU estimate for one session under an explicit, documented assumption:
// constant power across the session (linear energy apportionment over the
// measured duration). The journal stores exact totals; it does not store the
// per-second time series a windowed price would need, so this column is an
// estimate for night-load shifting analysis, never the invoice. Returns false
// with reason when the estimate cannot be built honestly.
bool tou_estimate_session(const TimeOfUse& tou, const SettledSession& s, std::int64_t& estimate_micro,
                          std::string& windows_note, std::string& reason) {
    std::int64_t close_epoch = 0;
    if (!parse_close_utc(s.closed_at_utc, close_epoch)) {
        reason = "closed_at_utc is not parseable UTC; estimate refused";
        return false;
    }
    if (s.record.duration_ms < 1000 || s.record.energy_uwh == 0) {
        reason = "session too short or zero energy; nothing to apportion";
        return false;
    }
    const std::int64_t dur_s = s.record.duration_ms / 1000;
    const std::int32_t close_sod = second_of_day_utc(close_epoch);
    const std::int32_t start_sod =
        static_cast<std::int32_t>((static_cast<std::int64_t>(close_sod) - (dur_s % 86400) + 86400) % 86400);
    const std::int64_t total = static_cast<std::int64_t>(s.record.energy_uwh);
    const std::vector<TimeSample> samples{{0, 0, start_sod}, {dur_s * 1000, total, close_sod}};
    std::string perr;
    const auto r = price_time_of_use_strict(tou, samples, s.record.flat_cost_micro_eur, 1, 1, perr);
    if (!r) {
        reason = perr;
        return false;
    }
    estimate_micro = r->total_cost_micro_eur;
    std::ostringstream note;
    for (std::size_t i = 0; i < r->periods.size(); ++i) {
        if (i > 0) note << '+';
        note << r->periods[i].window << ':' << r->periods[i].duration_ms / 1000 << 's';
    }
    windows_note = note.str();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // A Windows text-mode stdout injects CR, so `--metrics` on stdout would
    // differ byte-wise from the same content written to a file with --out.
    // Both are documents: emit raw bytes on both paths.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    Options opt;
    std::string err;
    if (!parse_args(argc, argv, opt, err)) {
        std::cerr
            << err << "\nUsage: charger_settle <journal> [--summary] [--verify-only] [--out FILE]\n"
            << "       charger_settle <journal> --dispute ASSET SESSION_ID\n"
            << "       charger_settle <journal> --cdr ASSET SESSION_ID\n"
            << "       charger_settle <journal> --metrics [--out FILE.prom]\n"
            << "       charger_settle <journal> --ocpp ASSET SESSION_ID [--out FILE]\n"
            << "       charger_settle <journal> --reconcile METER.csv --tolerance-ppm N [--out FILE.csv] [--summary]\n"
            << "         (the tolerance is required: this tool never invents a threshold)\n"
            << "       charger_settle <journal> --check-tou tou.json\n"
            << "       charger_settle <journal> --tou-settle tou.json [--summary] [--out FILE]\n";
        return 2;
    }

    std::string verr;
    // A missing file is an I/O problem (exit 2), not evidence of tampering
    // (exit 3): an audit step must not cry tamper over a mistyped path.
    std::ifstream probe(opt.journal, std::ios::binary);
    if (!probe) {
        std::cerr << "cannot read journal: " << opt.journal << '\n';
        return 2;
    }
    auto led = Ledger::open(opt.journal, false, verr);
    if (!led) {
        std::cerr << "cannot open journal: " << verr << '\n';
        return 2;
    }
    if (!led->verify(verr)) {
        std::cerr << "JOURNAL ALTERED: " << verr << '\n';
        return 3;
    }
    const auto rep = led->recover();
    if (!opt.check_tou.empty()) {
        bool ok = false;
        const std::string text = read_file_text(opt.check_tou, ok);
        if (!ok) {
            std::cerr << "cannot read TOU tariff: " << opt.check_tou << '\n';
            return 2;
        }
        const auto loaded = parse_tou_json(text);
        if (!loaded.ok) {
            std::cerr << "TOU tariff refused: " << loaded.error << '\n';
            return 2;
        }
        const auto cov = tou_coverage(loaded.tou);
        std::cout << "TOU tariff " << opt.check_tou << ": " << loaded.tou.windows.size() << " window(s)";
        if (loaded.tou.fallback.has_value()) std::cout << " + fallback '" << loaded.tou.fallback->name << '\'';
        std::cout << "\n  covered " << cov.covered_s << " s, uncovered " << cov.uncovered_s << " s of 86400\n";
        if (!cov.full) {
            std::cerr << "TOU tariff incomplete: " << cov.uncovered_s
                      << " s would be priced at zero; settlement refused until windows or fallback cover the day\n";
            return 2;
        }
        std::cout << "TOU tariff complete: every second of the day is priced\n";
        return 0;
    }
    if (opt.cdr) {
        const SettledSession* found = nullptr;
        for (const auto& s : led->sessions()) {
            if (s.asset_id == opt.cdr_asset && s.id == opt.cdr_session) {
                found = &s;
                break;
            }
        }
        if (found == nullptr) {
            std::cerr << "no closed session " << opt.cdr_asset << " #" << opt.cdr_session << " in a verified journal\n";
            return 2;
        }
        const std::string cdr = json_cdr(*found);
        if (opt.out.empty()) {
            std::cout << cdr << '\n';
        } else {
            std::ofstream fout(opt.out, std::ios::binary);
            if (!fout) {
                std::cerr << "cannot write " << opt.out << '\n';
                return 2;
            }
            fout << cdr << '\n';
        }
        return 0;
    }
    if (opt.metrics) {
        // The chain was verified above, so these gauges describe an intact journal.
        const auto kpi = led->portfolio();
        const std::string text = prometheus_portfolio(kpi ? *kpi : PortfolioKpi{});
        if (opt.out.empty()) {
            std::cout << text;
        } else {
            std::string werr;
            if (!write_file_atomic(opt.out, text, werr, false)) {
                std::cerr << "cannot write " << werr << '\n';
                return 2;
            }
        }
        return 0;
    }
    if (opt.ocpp) {
        const SettledSession* found = nullptr;
        for (const auto& s : led->sessions()) {
            if (s.asset_id == opt.ocpp_asset && s.id == opt.ocpp_session) {
                found = &s;
                break;
            }
        }
        if (found == nullptr) {
            std::cerr << "no closed session " << opt.ocpp_asset << " #" << opt.ocpp_session
                      << " in a verified journal\n";
            return 2;
        }
        std::string oerr;
        const std::string events = json_ocpp_events(*found, oerr);
        if (events.empty()) {
            std::cerr << "cannot export " << opt.ocpp_asset << " #" << opt.ocpp_session << ": " << oerr << '\n';
            return 2;
        }
        if (opt.out.empty()) {
            std::cout << events << '\n';
        } else {
            std::ofstream fout(opt.out, std::ios::binary);
            if (!fout) {
                std::cerr << "cannot write " << opt.out << '\n';
                return 2;
            }
            fout << events << '\n';
        }
        return 0;
    }
    if (!opt.reconcile.empty()) {
        bool ok = false;
        const std::string meter_text = read_file_text(opt.reconcile, ok);
        if (!ok) {
            std::cerr << "cannot read meter file: " << opt.reconcile << '\n';
            return 2;
        }
        std::string merr;
        const auto readings = parse_meter_csv(meter_text, merr);
        if (!readings) {
            std::cerr << "meter file refused: " << merr << '\n';
            return 2;
        }
        const auto result = reconcile(led->sessions(), *readings, opt.tolerance_ppm);
        const std::string csv_text = reconcile_csv(result);
        const std::string summary_text = reconcile_summary_text(result, opt.journal, opt.reconcile);
        if (!opt.out.empty()) {
            std::ofstream fout(opt.out, std::ios::binary);
            if (!fout) {
                std::cerr << "cannot write " << opt.out << '\n';
                return 2;
            }
            fout << csv_text;
        } else if (!opt.summary) {
            std::cout << csv_text;
        }
        // Pipelines keep a clean CSV on stdout: the summary goes to stdout only
        // when it is the product (--summary) or the CSV went to a file.
        (opt.summary || !opt.out.empty() ? std::cout : std::cerr) << summary_text;
        return result.summary.drift > 0 ? 4 : 0;
    }
    // Optional TOU estimate layer: validated first, refused if incomplete.
    // The exact ledger CSV stays the invoice of record; the estimate column is
    // constant-power apportionment for night-load analysis, labelled as such.
    const TimeOfUse* tou = nullptr;
    TimeOfUse tou_owned;
    if (!opt.tou_settle.empty()) {
        bool ok = false;
        const std::string text = read_file_text(opt.tou_settle, ok);
        if (!ok) {
            std::cerr << "cannot read TOU tariff: " << opt.tou_settle << '\n';
            return 2;
        }
        const auto loaded = parse_tou_json(text);
        if (!loaded.ok) {
            std::cerr << "TOU tariff refused: " << loaded.error << '\n';
            return 2;
        }
        const auto cov = tou_coverage(loaded.tou);
        if (!cov.full) {
            std::cerr << "TOU tariff incomplete: " << cov.uncovered_s
                      << " s would be priced at zero; --tou-settle refused\n";
            return 2;
        }
        tou_owned = loaded.tou;
        tou = &tou_owned;
    }
    if (opt.dispute) {
        const SettledSession* found = nullptr;
        for (const auto& s : led->sessions()) {
            if (s.asset_id == opt.dispute_asset && s.id == opt.dispute_session) {
                found = &s;
                break;
            }
        }
        if (found == nullptr) {
            std::cerr << "no closed session " << opt.dispute_asset << " #" << opt.dispute_session
                      << " in a verified journal\n";
            return 2;
        }
        // The digest of the whole file is the first thing an auditor checks:
        // it pins exactly which bytes the pack was built from.
        std::ifstream fin(opt.journal, std::ios::binary);
        std::ostringstream whole;
        whole << fin.rdbuf();
        const std::string dispute = json_dispute(*found, rep, opt.journal, sha256_hex(whole.str()));
        if (opt.out.empty()) {
            std::cout << dispute << '\n';
        } else {
            std::ofstream fout(opt.out, std::ios::binary);
            if (!fout) {
                std::cerr << "cannot write " << opt.out << '\n';
                return 2;
            }
            fout << dispute << '\n';
        }
        return 0;
    }
    if (opt.verify_only) {
        std::cout << "journal verified: " << rep.lines << " lines, " << rep.session_closes << " closed sessions\n";
        return 0;
    }

    std::uint64_t billable = 0, held = 0;
    std::uint64_t billable_revenue = 0, held_revenue = 0, billable_energy = 0;
    std::uint64_t energy_cost = 0, time_cost = 0, flat_cost = 0;

    std::ostringstream csv;
    csv << "asset_id,session_id,closed_at_utc,quality,billable,stop_reason,energy_wh,billed_energy_wh,"
           "charging_s,billed_time_s,energy_eur,time_eur,flat_eur,total_eur,currency,energy_lower_bound,"
           "time_upper_bound";
    if (tou != nullptr) csv << ",tou_estimate_eur,tou_windows,tou_note";
    csv << '\n';
    std::uint64_t tou_ok = 0, tou_refused = 0;
    for (const auto& s : led->sessions()) {
        const auto& r = s.record;
        const auto money = [](std::int64_t micro) { return format_fixed_point(micro, 1000000); };
        csv << csv_escape(s.asset_id) << ',' << s.id << ',' << csv_escape(s.closed_at_utc) << ','
            << csv_escape(r.quality) << ',' << (r.billing_complete ? "yes" : "no") << ',' << csv_escape(r.stop_reason)
            << ',' << r.energy_wh << ',' << r.billed_energy_wh << ',' << r.charging_s << ',' << r.billed_time_s << ','
            << money(r.energy_cost_micro_eur) << ',' << money(r.time_cost_micro_eur) << ','
            << money(r.flat_cost_micro_eur) << ',' << money(r.total_cost_micro_eur) << ',' << csv_escape(s.currency)
            << ',' << (r.energy_lower_bound ? "yes" : "no") << ',' << (r.time_upper_bound ? "yes" : "no");
        if (tou != nullptr) {
            std::int64_t est = 0;
            std::string windows, reason;
            if (tou_estimate_session(*tou, s, est, windows, reason)) {
                ++tou_ok;
                csv << ',' << money(est) << ',' << csv_escape(windows) << ",constant-power-estimate";
            } else {
                ++tou_refused;
                csv << ",," << csv_escape(std::string("refused: ") + reason);
            }
        }
        csv << '\n';

        if (r.billing_complete) {
            ++billable;
            billable_revenue += static_cast<std::uint64_t>(r.total_cost_micro_eur);
            billable_energy += r.energy_wh;
            energy_cost += static_cast<std::uint64_t>(r.energy_cost_micro_eur);
            time_cost += static_cast<std::uint64_t>(r.time_cost_micro_eur);
            flat_cost += static_cast<std::uint64_t>(r.flat_cost_micro_eur);
        } else {
            ++held;
            held_revenue += static_cast<std::uint64_t>(r.total_cost_micro_eur);
        }
    }

    if (!opt.out.empty()) {
        std::ofstream out(opt.out, std::ios::binary);
        if (!out) {
            std::cerr << "cannot write " << opt.out << '\n';
            return 2;
        }
        out << csv.str();
    } else if (!opt.summary) {
        std::cout << csv.str();
    }

    if (opt.summary || !opt.out.empty()) {
        const auto eur = [](std::uint64_t micro) {
            return format_fixed_point(static_cast<std::int64_t>(micro), 1000000);
        };
        std::printf("\nSettlement summary (journal %s, chain %s)\n", opt.journal.c_str(),
                    rep.chain_intact ? "verified" : "BROKEN");
        std::printf("  sessions closed      %llu\n", (unsigned long long)rep.session_closes);
        std::printf("  invoice now         %llu\n", (unsigned long long)billable);
        std::printf("  held for review     %llu", (unsigned long long)held);
        if (!rep.orphan_sessions.empty()) {
            std::printf("  (%llu orphan: process died mid-session)", (unsigned long long)rep.orphan_sessions.size());
        }
        std::printf("\n\n  billable energy      %llu Wh\n", (unsigned long long)billable_energy);
        std::printf("  ENERGY              %s EUR\n", eur(energy_cost).c_str());
        std::printf("  TIME                %s EUR\n", eur(time_cost).c_str());
        std::printf("  FLAT                %s EUR\n", eur(flat_cost).c_str());
        std::printf("  invoice total       %s EUR  (excl. VAT)\n", eur(billable_revenue).c_str());
        if (tou != nullptr) {
            std::printf(
                "  TOU estimates       %llu priced, %llu refused (constant-power apportionment, not invoiced)\n",
                (unsigned long long)tou_ok, (unsigned long long)tou_refused);
        }
        if (held > 0) {
            std::printf("\n  held (not invoiced) %s EUR across %llu session(s)\n", eur(held_revenue).c_str(),
                        (unsigned long long)held);
            std::printf("  reason: gaps, timeout, orphan or undersampled — see the CSV quality column\n");
        }
    }
    return 0;
}