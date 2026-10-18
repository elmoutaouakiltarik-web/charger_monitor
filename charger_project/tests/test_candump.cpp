// Parser and replay tests for charger_candump.hpp.
//
// The fixture tests/fixtures/bay_session.candump is a hand-built DEMO-protocol
// recording (see make_bay_session.py). The expected figures below were derived
// independently by `make_bay_session.py --expect`, a plain-integer model of the
// documented rules, and the engine must reproduce them exactly.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_candump.hpp"
#include "charger_monitor.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

void test_log_format() {
    auto r = parse_candump_line("(1436509052.249713) vcan0 044#2A366C2BBA");
    require(r.has_value(), "the canonical candump -L line parses");
    require(r->has_time && r->time_us == 1436509052249713LL, "timestamp in exact microseconds");
    require(r->iface == "vcan0" && r->id == 0x44 && !r->extended && !r->rtr && !r->fd && !r->error_frame, "fields");
    require(r->length == 5 && r->data[0] == 0x2A && r->data[4] == 0xBA, "data bytes");

    r = parse_candump_line("(1.000001) can0 18FEF100#0102");
    require(r && r->extended && r->id == 0x18FEF100u && r->length == 2, "8 hex digits is a 29-bit id");

    r = parse_candump_line("(2.000000)   can0\t123#");
    require(r && r->length == 0 && r->id == 0x123, "a frame without data, any blank run between tokens");

    r = parse_candump_line("(2.5) can0 123#R");
    require(r && r->rtr && r->length == 0, "remote frame");
    r = parse_candump_line("(2.5) can0 123#R5");
    require(r && r->rtr, "remote frame with a length digit");

    r = parse_candump_line("(3.0) can0 123##1AABBCC");
    require(r && r->fd && r->length == 0, "CAN FD is recognised and flagged, never decoded");

    r = parse_candump_line("(4.0) can0 20000004#0000000000000000");
    require(r && r->error_frame && r->length == 8, "kernel error frame");

    r = parse_candump_line("(5.000000) can0 181#02\r");
    require(r && r->id == 0x181 && r->data[0] == 2, "a CRLF recording is accepted");

    // Digits beyond the microsecond are dropped, never rounded or sent through a double.
    r = parse_candump_line("(7.1234569) can0 100#00");
    require(r && r->time_us == 7123456, "sub-microsecond digits are truncated");
    r = parse_candump_line("(7.5) can0 100#00");
    require(r && r->time_us == 7500000, "short fractions are padded");
    r = parse_candump_line("(7) can0 100#00");
    require(r && r->time_us == 7000000, "a timestamp without a fraction");
}

void test_human_format() {
    auto r = parse_candump_line("  can0  180   [8]  80 BB E8 03 00 00 00 00");
    require(r && !r->has_time && r->iface == "can0" && r->id == 0x180 && r->length == 8, "default candump format");
    require(r->data[0] == 0x80 && r->data[7] == 0x00 && r->data[1] == 0xBB, "bytes");

    r = parse_candump_line("(1436509052.249713)  can0  181   [1]  02");
    require(r && r->has_time && r->time_us == 1436509052249713LL && r->length == 1, "-ta adds an absolute stamp");

    r = parse_candump_line("  can0  123   [0]");
    require(r && r->length == 0, "empty payload");

    r = parse_candump_line("  can0  20000004   [8]  00 00 00 00 00 00 00 00  ERRORFRAME");
    require(r && r->error_frame, "error frame marker");

    r = parse_candump_line("  can0  123   remote request");
    require(r && r->rtr, "remote request");
}

void test_hostile_lines() {
    const std::vector<std::string> bad = {
        "",
        "   ",
        "\r\n",
        "not a frame",
        "(abc) can0 123#00",
        "() can0 123#00",
        "(1.) can0 123#00",
        "(.5) can0 123#00",
        "(1.0 can0 123#00",
        "(1.0) can0",
        "(1.0) can0 123#00 extra",
        "(1.0) can0 123#000102030405060708",  // nine bytes on a classic frame: refused, not truncated
        "(1.0) can0 123#0",                   // odd hex
        "(1.0) can0 123#GG",                  // not hex
        "(1.0) can0 FFF#00",                  // 3 digits above 0x7FF
        "(1.0) can0 1234#00",                 // 4 digits
        "(1.0) can0 12345678#00"
        "0",                       // trailing odd nibble
        "(1.0) can0 80000123#00",  // kernel EFF bit is not part of the text format
        "(1.0) can0 C0000123#00",
        "(1.0) can0 123#R55",
        "(1.0) can0 123##",    // FD without flags
        "(1.0) can0 123##1A",  // FD flags digit, then an odd payload nibble
        "(99999999999999999999.0) can0 123#00",
        "(1.12345678901) can0 123#00",  // more than nine fractional digits
        "(-1.0) can0 123#00",
        "(1.0) ca#n0 123#00",
        "(1.0) can0 123#00;rm",
        "can0 180 [9] 00 01 02 03 04 05 06 07 08",  // human format, more than eight bytes
        "can0 180 [8] 00 01",                       // fewer bytes than announced
        "can0 180 [2] 0 1",                         // one-digit bytes
        "can0 180 [x] 00",
        std::string(5000, 'A'),
        std::string("(1.0) can0 123#") + std::string(4000, '0'),
    };
    for (const auto& line : bad) {
        const auto r = parse_candump_line(line);
        require(!r.has_value(), "must be refused: " + line.substr(0, 40));
    }
    // A stream of arbitrary bytes never throws.
    std::string noise;
    for (int i = 0; i < 4096; ++i) noise.push_back(static_cast<char>((i * 131 + 7) & 0xFF));
    for (std::size_t i = 0; i + 40 < noise.size(); i += 17) {
        static_cast<void>(parse_candump_line(noise.substr(i, 40)));
    }
}

void test_utc_formatting() {
    require(utc_from_epoch_us(0) == "1970-01-01T00:00:00Z", "epoch");
    require(utc_from_epoch_us(1436509052249713LL) == "2015-07-10T06:17:32Z", "microseconds are dropped, not rounded");
    require(utc_from_epoch_us(1792310400000000LL) == "2026-10-18T08:00:00Z", "the fixture base time");
    require(utc_from_epoch_us(951782400000000LL) == "2000-02-29T00:00:00Z", "a leap day");
    require(utc_from_epoch_us(4107542400000000LL) == "2100-03-01T00:00:00Z", "2100 is not a leap year");
    require(utc_from_epoch_us(-1) == "1969-12-31T23:59:59Z", "just before the epoch");
    require(utc_from_epoch_us(999999) == "1970-01-01T00:00:00Z", "floor, not round");
}

// The replay loop of charger_monitor --replay-candump, in-process: frame time is
// the recorded time relative to the first frame, and the silence between two
// frames is walked in 200 ms idle ticks, bounded to just past the longest timeout.
struct Closed {
    SessionRecord rec;
    std::string closed_at;
};

std::vector<Closed> replay(const std::string& path, Config cfg, std::int64_t* first_us_out, std::int64_t* last_us_out) {
    std::ifstream in(path);
    require(static_cast<bool>(in), "cannot open the fixture: " + path);
    Monitor monitor(cfg);
    std::vector<Closed> closed;
    const auto base = Clock::now();
    const auto horizon = std::max(cfg.measurement_timeout, cfg.status_timeout) + std::chrono::milliseconds(1000);
    std::int64_t first_us = -1, last_us = -1;
    Clock::time_point prev{};
    bool have_prev = false;
    const auto collect = [&](const std::vector<Event>& events, Clock::time_point now, std::int64_t at_us) {
        for (const auto& e : events) {
            if (e.code != "SESSION_CLOSED") continue;
            const auto snap = monitor.snapshot(now);
            require(snap.last_session.has_value(), "a closed session is visible in the snapshot");
            closed.push_back({*snap.last_session, utc_from_epoch_us(at_us)});
        }
    };
    std::string line;
    while (std::getline(in, line)) {
        const auto rec = parse_candump_line(line);
        require(rec.has_value() && rec->has_time, "every fixture line parses: " + line);
        if (first_us < 0) first_us = rec->time_us;
        last_us = rec->time_us;
        const auto at = base + std::chrono::microseconds(rec->time_us - first_us);
        if (have_prev) {
            const auto limit = std::min(at, prev + horizon);
            for (auto t = prev + std::chrono::milliseconds(200); t < limit; t += std::chrono::milliseconds(200)) {
                const auto us = first_us + std::chrono::duration_cast<std::chrono::microseconds>(t - base).count();
                collect(monitor.tick(t), t, us);
            }
        }
        prev = at;
        have_prev = true;
        CanFrame f;
        f.id = rec->id;
        f.length = rec->length;
        f.data = rec->data;
        f.received_at = at;
        auto events = monitor.ingest(f);
        const auto tick_events = monitor.tick(at);
        events.insert(events.end(), tick_events.begin(), tick_events.end());
        collect(events, at, rec->time_us);
    }
    if (first_us_out) *first_us_out = first_us;
    if (last_us_out) *last_us_out = last_us;
    return closed;
}

void test_fixture_replay(const std::string& fixture) {
    Config cfg;  // default demo profile, timeouts 3000 ms, integration step 2000 ms
    cfg.asset_id = "bay-07";
    cfg.tariff.energy_micro_eur_per_kwh = 250000;
    cfg.tariff.time_micro_eur_per_hour = 3600000;
    cfg.tariff.flat_micro_eur = 500000;
    std::int64_t first_us = 0, last_us = 0;
    const auto sessions = replay(fixture, cfg, &first_us, &last_us);

    require(first_us == 1792310400000000LL, "the recording starts at the recorded UTC");
    require(utc_from_epoch_us(first_us) == "2026-10-18T08:00:00Z", "first frame at 08:00:00Z");
    require(utc_from_epoch_us(last_us) == "2026-10-18T08:25:00Z", "last frame at 08:25:00Z: 25 minutes");
    require(sessions.size() == 2, "the recording contains exactly two sessions");

    // Session 1: the 12 s dropout. Closed by the first 200 ms idle tick that finds the
    // measurement older than 3 s, i.e. at 902.05 s, not when the bus comes back.
    const auto& a = sessions[0].rec;
    require(a.stop_reason == "COMMUNICATION_TIMEOUT", "session 1 ended by the dropout");
    require(a.energy_uwh == 9474699770ull, "session 1 energy, exact (uWh)");
    require(a.energy_wh == 9474 && a.billed_energy_wh == 9474, "session 1 Wh register");
    require(a.duration_ms == 842000 && a.charging_s == 842, "session 1 ran 60.05 s to 902.05 s");
    require(a.energy_cost_micro_eur == 2368500 && a.time_cost_micro_eur == 842000 && a.flat_cost_micro_eur == 500000,
            "session 1 cost lines");
    require(a.total_cost_micro_eur == 3710500, "session 1 total: 3.710500 EUR");
    require(a.time_upper_bound && a.energy_lower_bound && !a.billing_complete,
            "a dropout is held for review, never invoiced");
    require(a.integration_gaps == 0, "the silence closed the session; it was not interpolated across");
    require(sessions[0].closed_at == "2026-10-18T08:15:02Z", "session 1 closes at 08:15:02 recorded UTC");

    // Session 2: clean close by the power stage at 1440.05 s.
    const auto& b = sessions[1].rec;
    require(b.stop_reason == "STATUS_OFF" && b.quality == "OK", "session 2 closed cleanly");
    require(b.energy_uwh == 6219938550ull, "session 2 energy, exact (uWh)");
    require(b.energy_wh == 6219 && b.duration_ms == 528000 && b.charging_s == 528, "session 2 registers");
    require(b.energy_cost_micro_eur == 1554750 && b.time_cost_micro_eur == 528000, "session 2 cost lines");
    require(b.total_cost_micro_eur == 2582750, "session 2 total: 2.582750 EUR");
    require(b.billing_complete && !b.energy_lower_bound && !b.time_upper_bound, "session 2 is billable");
    require(sessions[1].closed_at == "2026-10-18T08:24:00Z", "session 2 closes at 08:24:00 recorded UTC");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        test_log_format();
        test_human_format();
        test_hostile_lines();
        test_utc_formatting();
        if (argc > 1) test_fixture_replay(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "candump test failed: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "candump tests passed" << std::endl;
    return 0;
}
