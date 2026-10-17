// charger_demo — a narrated walkthrough of what the library guarantees.
//
// This is the fastest way to understand the project: it drives a Monitor through
// a realistic shift and prints exactly what an operator would need to defend an
// invoice or a downtime decision. It runs with no hardware and no network.
//
//   charger_demo            full walkthrough
//   charger_demo --brief    just the headline numbers

#include <charger_ledger.hpp>
#include <charger_monitor.hpp>
#include <charger_portfolio.hpp>

#include <cstdio>
#include <string>

using namespace charger;

namespace {

// 0x180: centivolts then centiamps, unsigned 16-bit little-endian.
CanFrame measurement_frame(double volts, double amps, Clock::time_point at) {
    CanFrame f;
    f.id = 0x180;
    f.length = 8;
    f.received_at = at;
    const auto cv = static_cast<std::uint16_t>(volts * 100.0);
    const auto ca = static_cast<std::uint16_t>(amps * 100.0);
    f.data[0] = static_cast<std::uint8_t>(cv & 0xff);
    f.data[1] = static_cast<std::uint8_t>(cv >> 8);
    f.data[2] = static_cast<std::uint8_t>(ca & 0xff);
    f.data[3] = static_cast<std::uint8_t>(ca >> 8);
    return f;
}

CanFrame status_frame(std::uint8_t status, Clock::time_point at) {
    CanFrame f;
    f.id = 0x181;
    f.length = 1;
    f.data[0] = status;
    f.received_at = at;
    return f;
}

void rule(char c) {
    for (int i = 0; i < 78; ++i) std::putchar(c);
    std::putchar('\n');
}

void heading(int step, const char* title) {
    std::printf("\n\033[1m%02d  %s\033[0m\n", step, title);
    rule('-');
}

void line(const char* label, const std::string& value) { std::printf("    %-22s %s\n", label, value.c_str()); }

std::string eur(std::int64_t micro) { return format_fixed_point(micro, 1000000) + " EUR"; }

// Prices used throughout: a realistic DC tariff, VAT excluded.
void apply_tariff(Config& cfg) {
    cfg.tariff.energy_micro_eur_per_kwh = 250000;  // 0.25 EUR/kWh
    cfg.tariff.time_micro_eur_per_hour = 3600000;  // 3.60 EUR/charging hour
    cfg.tariff.flat_micro_eur = 500000;            // 0.50 EUR per session
    cfg.asset_id = "bay-07";
    cfg.currency = "EUR";
    // Thresholds a real electrical engineer would set from nameplate data.
    cfg.sag_voltage_mv = 460000;
    cfg.sag_tolerance_mv = 5000;
    cfg.stuck_at_window = std::chrono::seconds(30);
    cfg.max_voltage_rate_mv_per_s = 50000;
}

void feed(Monitor& m, double v, double a, std::uint8_t status, Clock::time_point at) {
    for (const auto& e : m.ingest(measurement_frame(v, a, at))) {
        if (e.level == EventLevel::Warning || e.level == EventLevel::Error) {
            std::printf("    ! %s: %s\n", e.code.c_str(), e.message.c_str());
        }
    }
    for (const auto& e : m.ingest(status_frame(status, at))) {
        if (e.level == EventLevel::Warning || e.level == EventLevel::Error) {
            std::printf("    ! %s: %s\n", e.code.c_str(), e.message.c_str());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool brief = argc > 1 && std::string(argv[1]) == "--brief";
    const auto t0 = Clock::now();
    const auto min = std::chrono::minutes(1);

    std::puts("");
    rule('=');
    std::puts(" charger_monitor — exact session ledger, durable journal, fleet rollup");
    rule('=');

    // ---------------------------------------------------------------- cold start
    heading(1, "Cold start");
    Config cfg;
    apply_tariff(cfg);
    // The integration step must stay below the stream timeout: energy can only
    // be integrated between samples the link still considers fresh.
    cfg.max_integration_step = std::chrono::minutes(2);
    cfg.measurement_timeout = std::chrono::minutes(3);
    cfg.status_timeout = std::chrono::minutes(3);
    Monitor monitor(cfg);
    {
        const auto s = monitor.snapshot(t0);
        line("state", to_string(s.state));
        line("why", "no frame has ever been received");
        line("availability", "undefined (-1), not zero percent");
    }

    // ------------------------------------------------------------------ session
    heading(2, "A session starts");
    feed(monitor, 480.0, 10.0, 2, t0);
    {
        const auto s = monitor.snapshot(t0);
        line("state", to_string(s.state));
        line("power", format_fixed_point(s.power_mw, 1000) + " W  (480.00 V x 10.00 A)");
        line("session", std::to_string(s.session ? s.session->id : 0));
        line("seed rule", "power held at open, pre-session interval not billed");
    }

    // ------------------------------------------------------------------- energy
    heading(3, "Energy accrues with exact integer arithmetic");
    for (int i = 1; i <= 60; ++i) {
        feed(monitor, 480.0, 10.0, 2, t0 + min * i);
    }
    {
        const auto s = monitor.snapshot(t0 + min * 60);
        const auto& r = *s.session;
        line("elapsed", "60 min of charging");
        line("energy", std::to_string(r.energy_wh) + " Wh   (exact, no floating point)");
        line("charge time", std::to_string(r.charging_s) + " s");
        line("quality", r.quality);
        line("billable", r.billing_complete ? "yes" : "no");
        std::printf("\n    price check at 480 V / 10 A for one hour:\n");
        line("ENERGY  4.8 kWh", eur(1200000));
        line("TIME    1.0 h", eur(3600000));
        line("FLAT    1 session", eur(500000));
        line("TOTAL", eur(5300000) + "  excl. VAT");
    }

    // ---------------------------------------------------------------- close out
    heading(4, "Clean close");
    feed(monitor, 480.0, 10.0, 0, t0 + min * 61);
    SessionRecord settled;
    {
        const auto s = monitor.snapshot(t0 + min * 61);
        settled = *s.last_session;
        line("stop reason", settled.stop_reason);
        line("duration", std::to_string(settled.duration_ms / 1000) + " s");
        line("energy", std::to_string(settled.energy_wh) + " Wh");
        line("total", eur(settled.total_cost_micro_eur) + "  excl. VAT");
        line("billing_complete", settled.billing_complete ? "true — safe to invoice" : "false");
    }

    // ------------------------------------------------------------------- dropout
    heading(5, "A communication dropout");
    const auto t1 = t0 + min * 90;
    feed(monitor, 480.0, 10.0, 2, t1);
    feed(monitor, 480.0, 10.0, 2, t1 + min);
    // The bus goes quiet for ten minutes.
    for (const auto& e : monitor.tick(t1 + min * 11)) {
        if (e.code == "MEASUREMENTS_TIMEOUT") {
            line("detected", e.code + " — " + e.message);
        }
    }
    {
        const auto s = monitor.snapshot(t1 + min * 11);
        const auto& r = *s.last_session;
        line("state", to_string(s.state));
        line("session energy", std::to_string(r.energy_wh) + " Wh");
        line("gap handling", "not interpolated — no energy invented for the silent interval");
        line("time_upper_bound", r.time_upper_bound ? "true — billed time is an upper bound" : "false");
        line("billing_complete", r.billing_complete ? "true" : "false — held for review");
    }

    // ---------------------------------------------------------------- diagnostics
    heading(6, "Predictive diagnostics");
    Config dcfg;
    apply_tariff(dcfg);
    dcfg.sag_voltage_mv = 460000;
    dcfg.sag_tolerance_mv = 5000;
    dcfg.stuck_at_window = std::chrono::minutes(5);
    dcfg.max_integration_step = std::chrono::minutes(2);
    dcfg.measurement_timeout = std::chrono::minutes(10);
    dcfg.status_timeout = std::chrono::minutes(10);
    Monitor diag(dcfg);
    const auto t2 = t0;
    feed(diag, 455.0, 10.0, 2, t2);  // 25 V below nominal, under load
    {
        const auto s = diag.snapshot(t2);
        line("voltage_sag", s.diagnostics.voltage_sag ? "true" : "false");
        line("meaning", "connector, fuse or contactor losing headroom");
        line("state", to_string(s.state) + "  — a diagnostic never blocks charging");
    }
    // Frozen channel: same reading for longer than the stuck-at window.
    for (int i = 1; i <= 8; ++i) feed(diag, 455.0, 10.0, 2, t2 + min * i);
    {
        const auto s = diag.snapshot(t2 + min * 8);
        line("measurement_stuck", s.diagnostics.measurement_stuck ? "true" : "false");
        line("meaning", "frozen sensor at constant load");
        line("warned once", s.kpi.stuck_at_warnings == 1 ? "yes, once per window" : "no");
    }

    // ------------------------------------------------------------------- journal
    heading(7, "Durable journal and crash recovery");
    const std::string journal_path = "charger_demo.journal";
    std::remove(journal_path.c_str());
    {
        std::string err;
        auto led = Ledger::open(journal_path, true, err);
        led->append_session_open(settled, "bay-07", "2026-01-01T09:00:00Z");
        led->append_session_close(settled, "bay-07", "EUR", "2026-01-01T10:01:00Z");
        // Simulate a crash mid-session: an open with no matching close.
        SessionRecord orphan = settled;
        orphan.id = 99;
        led->append_session_open(orphan, "bay-07", "2026-01-01T11:00:00Z");
        // The process dies here. Nothing else is written.
    }
    {
        // A new process reopens the same file.
        std::string err;
        auto led = Ledger::open(journal_path, true, err);
        const auto rep = led->recover();
        line("chain intact", rep.chain_intact ? "true" : "false");
        line("closed sessions", std::to_string(rep.session_closes));
        line("energy recovered", std::to_string(rep.energy_uwh) + " uWh");
        line("billable", std::to_string(rep.billable_sessions));
        line("orphans",
             std::to_string(rep.orphan_sessions.size()) + " (session opened but never closed — not billable)");
        std::string verr;
        line("re-verify", led->verify(verr) ? "chain still valid" : "CHAIN BROKEN");
    }
    {
        // Flip one energy digit: the chain must refuse.
        FILE* f = std::fopen(journal_path.c_str(), "rb");
        std::string content;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        std::fclose(f);
        const auto tab = content.find('\t');
        if (tab != std::string::npos) content[tab + 1] = (content[tab + 1] == '0') ? '1' : '0';
        f = std::fopen(journal_path.c_str(), "wb");
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
        std::string err;
        auto led = Ledger::open(journal_path, false, err);
        std::string verr;
        line("after tampering", led->verify(verr) ? "ACCEPTED (bug)" : "refused — " + verr);
    }
    std::remove(journal_path.c_str());

    // ----------------------------------------------------------------- portfolio
    heading(8, "Fleet rollup");
    {
        Portfolio p;
        p.add(settled, "bay-07");
        SessionRecord second = settled;
        second.id = 2;
        second.energy_uwh = 2400000000ull;
        second.energy_wh = 2400;
        second.duration_ms = 1800000;
        second.total_cost_micro_eur = 2650000;
        p.add(second, "bay-08");
        SessionRecord held = settled;
        held.id = 3;
        held.energy_uwh = 0;
        held.energy_wh = 0;
        held.billing_complete = false;
        held.undersampled = true;
        held.total_cost_micro_eur = 0;
        held.stop_reason = "COMMUNICATION_TIMEOUT";
        p.add(held, "bay-09");

        const auto k = p.summary();
        line("assets", std::to_string(k.assets));
        line("sessions", std::to_string(k.sessions_closed) + " closed, " + std::to_string(k.sessions_billable) +
                             " billable, " + std::to_string(k.sessions_held) + " held");
        line("site energy", std::to_string(k.energy_uwh) + " uWh");
        line("site revenue", eur(k.revenue_micro_eur) + "  excl. VAT");
        line("mean power", std::to_string(k.mean_power_mw / 1000) + " W");
        line("best asset", k.best_asset_id);
        std::printf("\n    the held session is counted separately: it is not silently\n");
        std::printf("    dropped and it is not silently invoiced either.\n");
    }

    if (!brief) {
        heading(9, "What this buys you");
        std::printf(
            "    * a restart no longer loses billing data\n"
            "    * a dropout is never turned into invented energy\n"
            "    * every invoice is traceable to accepted samples\n"
            "    * a disputed session is provable with standard tools\n"
            "    * the KPI that justifies the next maintenance visit\n");
    }

    rule('=');
    std::puts(" exact integers in, defensible invoices out");
    rule('=');
    std::puts("");
    return 0;
}