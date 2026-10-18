// Monitor::shutdown(): a session that is open when the monitor is stopped on
// purpose is closed as INTERRUPTED with its energy kept exactly, never left as an
// orphan with unknown energy and never invoiced automatically.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_ledger.hpp"
#include "charger_monitor.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

CanFrame measurements(double v, double a, Clock::time_point t) {
    CanFrame f;
    f.id = 0x180;
    f.length = 8;
    f.received_at = t;
    const auto rv = static_cast<std::uint16_t>(v * 100), ri = static_cast<std::uint16_t>(a * 100);
    f.data[0] = static_cast<std::uint8_t>(rv & 0xff);
    f.data[1] = static_cast<std::uint8_t>(rv >> 8);
    f.data[2] = static_cast<std::uint8_t>(ri & 0xff);
    f.data[3] = static_cast<std::uint8_t>(ri >> 8);
    return f;
}

CanFrame status(std::uint8_t s, Clock::time_point t) {
    CanFrame f;
    f.id = 0x181;
    f.length = 1;
    f.data[0] = s;
    f.received_at = t;
    return f;
}

void feed(Monitor& m, double v, double a, std::uint8_t st, Clock::time_point t) {
    m.ingest(measurements(v, a, t));
    m.ingest(status(st, t));
    m.tick(t);
}

Config demo_config() {
    Config cfg;
    cfg.asset_id = "bay-07";
    cfg.tariff.energy_micro_eur_per_kwh = 250000;
    cfg.tariff.time_micro_eur_per_hour = 3600000;
    cfg.tariff.flat_micro_eur = 500000;
    // Minute-spaced samples, as in charger_demo.
    cfg.max_integration_step = std::chrono::minutes(2);
    cfg.measurement_timeout = std::chrono::minutes(3);
    cfg.status_timeout = std::chrono::minutes(3);
    return cfg;
}

bool has_code(const std::vector<Event>& events, const std::string& code) {
    for (const auto& e : events) {
        if (e.code == code) return true;
    }
    return false;
}

void test_interrupted_keeps_exact_energy() {
    Monitor m(demo_config());
    const auto t0 = Clock::now();
    const auto min = std::chrono::minutes(1);
    feed(m, 480.0, 10.0, 2, t0);
    for (int i = 1; i <= 60; ++i) feed(m, 480.0, 10.0, 2, t0 + min * i);

    const auto before = m.snapshot(t0 + min * 60);
    require(before.session.has_value(), "a session is open");
    require(before.session->quality == "OK", "the live record of an open session is not INTERRUPTED");

    const auto events = m.shutdown(t0 + min * 60);
    require(events.size() == 1 && has_code(events, "SESSION_CLOSED"), "exactly one SESSION_CLOSED event");

    const auto snap = m.snapshot(t0 + min * 60);
    require(!snap.session.has_value(), "no session is open any more");
    require(snap.last_session.has_value(), "the closed session is the last session");
    const auto& s = *snap.last_session;
    require(s.closed, "closed");
    require(s.stop_reason == "MONITOR_SHUTDOWN" && s.stop_reason == STOP_MONITOR_SHUTDOWN, "the stop reason");
    require(s.energy_uwh == 4800000000ull && s.energy_wh == 4800, "480 V x 10 A for 1 h is exactly 4 800 Wh");
    require(s.duration_ms == 3600000 && s.charging_s == 3600, "the duration runs to the shutdown instant");
    require(s.quality == "INTERRUPTED", "quality INTERRUPTED");
    require(!s.billing_complete, "an interrupted session is never billing_complete");
    require(s.energy_lower_bound, "what happened after the stop is unknown: energy is a lower bound");
    require(!s.time_upper_bound, "time_upper_bound is for timeouts, not for a planned stop");
    require(!s.undersampled && s.integration_gaps == 0, "the data it has is complete");
    // The cost is still computed from the exact registers, so a reviewer sees what it is worth.
    require(s.energy_cost_micro_eur == 1200000 && s.time_cost_micro_eur == 3600000 && s.flat_cost_micro_eur == 500000,
            "ENERGY 1.2 + TIME 3.6 + FLAT 0.5");
    require(s.total_cost_micro_eur == 5300000, "5.30 EUR held for review, not invoiced");
    require(snap.kpi.sessions_closed == 1 && snap.kpi.sessions_billable == 0, "KPI: closed but not billable");
    require(snap.kpi.energy_closed_uwh == 4800000000ull, "the energy is counted in the closed total");
    require(json_session_line(s, "bay-07", "EUR", "2026-10-18T08:00:00Z").find("\"quality\":\"INTERRUPTED\"") !=
                std::string::npos,
            "the JSON carries the quality");
}

void test_noop_without_session() {
    Monitor m(demo_config());
    const auto t0 = Clock::now();
    require(m.shutdown(t0).empty(), "nothing to close on a fresh monitor");
    feed(m, 480.0, 0.0, 1, t0);  // READY: no session
    const std::string before = json_snapshot(m.snapshot(t0));
    require(m.shutdown(t0).empty(), "no session is open while READY");
    require(json_snapshot(m.snapshot(t0)) == before, "and nothing at all changed");

    // A session that closed by itself earlier leaves nothing for shutdown to do.
    feed(m, 480.0, 10.0, 2, t0 + std::chrono::minutes(1));
    feed(m, 480.0, 10.0, 2, t0 + std::chrono::minutes(2));
    feed(m, 480.0, 0.0, 0, t0 + std::chrono::minutes(3));
    const auto snap = m.snapshot(t0 + std::chrono::minutes(3));
    require(snap.last_session && snap.last_session->stop_reason == "STATUS_OFF", "it closed cleanly by itself");
    const std::string after = json_snapshot(snap);
    require(m.shutdown(t0 + std::chrono::minutes(3)).empty(), "shutdown after a clean close is a no-op");
    require(json_snapshot(m.snapshot(t0 + std::chrono::minutes(3))) == after, "and changes nothing");
}

void test_twice_is_harmless() {
    Monitor m(demo_config());
    const auto t0 = Clock::now();
    const auto min = std::chrono::minutes(1);
    feed(m, 480.0, 10.0, 2, t0);
    for (int i = 1; i <= 5; ++i) feed(m, 480.0, 10.0, 2, t0 + min * i);
    require(m.shutdown(t0 + min * 5).size() == 1, "the first call closes the session");
    require(m.shutdown(t0 + min * 5).empty(), "the second call finds nothing to close");
    require(m.shutdown(t0 + min * 6).empty(), "a later call finds nothing either");
    const auto snap = m.snapshot(t0 + min * 6);
    require(snap.kpi.sessions_closed == 1 && snap.kpi.sessions_opened == 1, "one session, closed once");
}

void test_quality_precedence() {
    const auto min = std::chrono::minutes(1);
    {
        // A session with fewer than two samples is NO_INTERVAL, not INTERRUPTED.
        Monitor m(demo_config());
        const auto t0 = Clock::now();
        feed(m, 480.0, 10.0, 2, t0);
        const auto events = m.shutdown(t0);
        require(events.size() == 1, "closed");
        const auto s = *m.snapshot(t0).last_session;
        require(s.quality == "NO_INTERVAL" && s.undersampled && !s.billing_complete, "NO_INTERVAL wins");
        require(s.stop_reason == "MONITOR_SHUTDOWN", "the stop reason is still the shutdown");
        require(s.energy_lower_bound, "lower bound");
    }
    {
        // Zero energy is ZERO_ENERGY, not INTERRUPTED.
        Monitor m(demo_config());
        const auto t0 = Clock::now();
        feed(m, 480.0, 0.0, 2, t0);
        for (int i = 1; i <= 3; ++i) feed(m, 480.0, 0.0, 2, t0 + min * i);
        m.shutdown(t0 + min * 3);
        const auto s = *m.snapshot(t0 + min * 3).last_session;
        require(s.energy_uwh == 0 && s.quality == "ZERO_ENERGY" && !s.billing_complete, "ZERO_ENERGY wins");
    }
    {
        // A measurement gap is GAPS, not INTERRUPTED.
        Monitor m(demo_config());
        const auto t0 = Clock::now();
        feed(m, 480.0, 10.0, 2, t0);
        feed(m, 480.0, 10.0, 2, t0 + min);
        feed(m, 480.0, 10.0, 2, t0 + min * 2 + std::chrono::seconds(30));  // 90 s later... under the step
        feed(m, 480.0, 10.0, 2, t0 + min * 2 + std::chrono::seconds(30) + std::chrono::seconds(150));  // beyond it
        const auto events = m.shutdown(t0 + min * 5);
        require(events.size() == 1, "still open at the shutdown");
        const auto s = *m.snapshot(t0 + min * 5).last_session;
        require(s.integration_gaps >= 1 && s.quality == "GAPS" && !s.billing_complete, "GAPS wins");
    }
}

void test_journal_round_trip() {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("charger_shutdown_test_" + std::to_string(std::rand()) + ".journal");
    fs::remove(path);
    {
        Monitor m(demo_config());
        const auto t0 = Clock::now();
        const auto min = std::chrono::minutes(1);
        std::string err;
        auto led = Ledger::open(path.string(), false, err);
        require(led.has_value(), "journal opens: " + err);
        feed(m, 480.0, 10.0, 2, t0);
        const auto opened = *m.snapshot(t0).session;
        led->append_session_open(opened, "bay-07", "2026-10-18T08:00:00Z");
        for (int i = 1; i <= 60; ++i) feed(m, 480.0, 10.0, 2, t0 + min * i);
        const auto events = m.shutdown(t0 + min * 60);
        require(events.size() == 1, "closed on shutdown");
        const auto snap = m.snapshot(t0 + min * 60);
        led->append_session_close(*snap.last_session, "bay-07", "EUR", "2026-10-18T09:00:00Z",
                                  json_tariff_block(m.config().tariff));
    }
    {
        std::string err;
        auto led = Ledger::open(path.string(), false, err);
        require(led.has_value(), "journal reopens");
        const auto rep = led->recover();
        require(rep.chain_intact && rep.lines == 2, "two chained lines");
        require(rep.orphan_sessions.empty(), "an interrupted session is NOT an orphan");
        require(rep.session_opens == 1 && rep.session_closes == 1, "one open, one close");
        require(rep.billable_sessions == 0, "it is not billable");
        require(rep.energy_uwh == 4800000000ull, "the energy survives the restart, exactly");
        require(led->sessions().size() == 1, "it appears among the settled sessions");
        const auto& s = led->sessions().front();
        require(s.asset_id == "bay-07" && s.id == 1, "identity");
        require(s.record.stop_reason == "MONITOR_SHUTDOWN" && s.record.quality == "INTERRUPTED", "reason and quality");
        require(!s.record.billing_complete && s.record.energy_lower_bound, "held for review");
        require(s.record.energy_uwh == 4800000000ull && s.record.total_cost_micro_eur == 5300000, "registers");
        require(s.open_line == 1 && s.close_line == 2, "provenance of both lines");
        std::string verr;
        require(led->verify(verr), "the chain verifies: " + verr);
    }
    fs::remove(path);

    // The contrast the feature exists for: no close line means an orphan with unknown energy.
    {
        std::string err;
        auto led = Ledger::open(path.string(), false, err);
        require(led.has_value(), "journal opens");
        Monitor m(demo_config());
        const auto t0 = Clock::now();
        feed(m, 480.0, 10.0, 2, t0);
        led->append_session_open(*m.snapshot(t0).session, "bay-07", "2026-10-18T08:00:00Z");
    }
    {
        std::string err;
        auto led = Ledger::open(path.string(), false, err);
        const auto rep = led->recover();
        require(rep.orphan_sessions.size() == 1 && rep.session_closes == 0, "a crash leaves an orphan");
    }
    fs::remove(path);
}

}  // namespace

int main() {
    try {
        test_interrupted_keeps_exact_energy();
        test_noop_without_session();
        test_twice_is_harmless();
        test_quality_precedence();
        test_journal_round_trip();
    } catch (const std::exception& e) {
        std::cerr << "shutdown test failed: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "shutdown tests passed" << std::endl;
    return 0;
}
