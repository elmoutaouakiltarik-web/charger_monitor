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
//
// Exit codes: 0 settled, 2 usage or verification error, 3 journal altered.

#include <charger_ledger.hpp>
#include <charger_monitor.hpp>

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
};

bool parse_args(int argc, char** argv, Options& opt, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--summary") {
            opt.summary = true;
        } else if (a == "--verify-only") {
            opt.verify_only = true;
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

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parse_args(argc, argv, opt, err)) {
        std::cerr << err << "\nUsage: charger_settle <journal> [--summary] [--verify-only] [--out FILE]\n";
        return 2;
    }

    std::string verr;
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
           "time_upper_bound\n";
    for (const auto& s : led->sessions()) {
        const auto& r = s.record;
        const auto money = [](std::int64_t micro) { return format_fixed_point(micro, 1000000); };
        csv << csv_escape(s.asset_id) << ',' << s.id << ',' << csv_escape(s.closed_at_utc) << ','
            << csv_escape(r.quality) << ',' << (r.billing_complete ? "yes" : "no") << ',' << csv_escape(r.stop_reason)
            << ',' << r.energy_wh << ',' << r.billed_energy_wh << ',' << r.charging_s << ',' << r.billed_time_s << ','
            << money(r.energy_cost_micro_eur) << ',' << money(r.time_cost_micro_eur) << ','
            << money(r.flat_cost_micro_eur) << ',' << money(r.total_cost_micro_eur) << ',' << csv_escape(s.currency)
            << ',' << (r.energy_lower_bound ? "yes" : "no") << ',' << (r.time_upper_bound ? "yes" : "no") << '\n';

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
        if (held > 0) {
            std::printf("\n  held (not invoiced) %s EUR across %llu session(s)\n", eur(held_revenue).c_str(),
                        (unsigned long long)held);
            std::printf("  reason: gaps, timeout, orphan or undersampled — see the CSV quality column\n");
        }
    }
    return 0;
}