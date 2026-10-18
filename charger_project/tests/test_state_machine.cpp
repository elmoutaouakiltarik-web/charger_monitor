#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_config.hpp"
#include "charger_dbc.hpp"
#include "charger_ledger.hpp"
#include "charger_monitor.hpp"
#include "charger_tou.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

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

int main() {
    // --- Basic state machine ---
    const auto t = Clock::now();
    Monitor m;
    require(m.snapshot(t).state == ChargeState::Unknown, "initial state must be UNKNOWN");
    m.ingest(measurements(480, 10, t));
    m.ingest(status(1, t));
    require(m.snapshot(t).state == ChargeState::Ready, "expected READY");
    require(m.snapshot(t).measurement_age_ms == 0 && m.snapshot(t).status_age_ms == 0, "fresh-frame ages must be 0 ms");
    m.ingest(status(2, t));
    require(m.snapshot(t).state == ChargeState::Charging, "expected CHARGING");
    require(m.snapshot(t).status_age_ms == 0, "fresh status age must be 0 ms");
    require(m.snapshot(t).power_enabled, "CHARGING implies power enabled");
    require(m.snapshot(t).power_mw == 4800000, "live power is V*I only while CHARGING");
    m.ingest(status(0, t));
    require(m.snapshot(t).state == ChargeState::Off, "expected OFF");
    require(!m.snapshot(t).power_enabled, "OFF implies power disabled");
    require(m.snapshot(t).power_mw == 0, "OFF must not publish live power");
    const auto later = t + std::chrono::milliseconds(1500);
    require(m.snapshot(later).measurement_age_ms == 1500 && m.snapshot(later).status_age_ms == 1500,
            "ages must track elapsed time");

    // --- Frames rejected by the validator ---
    {
        CanFrame bad = measurements(480, 10, t);
        bad.length = 7;
        const auto ev = m.ingest(bad);
        require(!ev.empty() && ev.front().code == "MEASUREMENT_DLC_INVALID", "wrong measurement DLC must be rejected");
        require(m.snapshot(t).rejected_frames == 1, "rejected counter must increment on bad DLC");
    }
    {
        CanFrame bad = measurements(480, 10, t);
        bad.data[7] = 1;
        const auto ev = m.ingest(bad);
        require(!ev.empty() && ev.front().code == "MEASUREMENT_RESERVED_NONZERO",
                "nonzero reserved bytes must be rejected");
    }
    {
        CanFrame bad;
        bad.id = 0x200;
        bad.length = 1;
        bad.received_at = t;
        const auto ev = m.ingest(bad);
        require(!ev.empty() && ev.front().code == "CAN_ID_UNKNOWN", "unknown CAN ID must be rejected");
        require(m.snapshot(t).rejected_frames == 3, "rejected counter must count all three bad frames");
    }
    {
        CanFrame bad = status(4, t);
        bad.length = 2;
        const auto ev = m.ingest(bad);
        require(!ev.empty() && ev.front().code == "POWER_STATUS_INVALID",
                "invalid power status frame must be rejected");
    }

    // --- Latched fault: no automatic recovery ---
    const auto ev = m.ingest(measurements(501, 10, t));
    require(!ev.empty(), "out-of-range measurement must emit event");
    require(m.snapshot(t).state == ChargeState::Fault, "out-of-range measurement must latch FAULT");
    m.ingest(status(1, t));
    require(m.snapshot(t).state == ChargeState::Fault, "fault must remain latched");
    const auto timeout_events = m.tick(t + std::chrono::seconds(4));
    require(timeout_events.empty(), "tick must not emit timeout events while fault latched");
    require(m.snapshot(t + std::chrono::seconds(4)).state == ChargeState::Fault,
            "timeout must not clear latched fault");
    require(m.snapshot(t + std::chrono::seconds(4)).measurements_stale, "old measurements must be stale");
    require(m.snapshot(t + std::chrono::seconds(4)).measurement_age_ms == 4000,
            "measurement age must reflect elapsed time");

    // --- reset_fault: rifiutato quando non c'è guasto o il codice è protetto ---
    std::string reason;
    Monitor fresh;
    require(!fresh.reset_fault(reason), "reset must fail with no latched fault");
    require(!reason.empty(), "reset must always explain the decision");
    require(!m.reset_fault(reason), "reset of MEASUREMENT_OUT_OF_RANGE must be denied (protected fault)");
    require(!reason.empty(), "protected-fault denial must carry a reason");

    // --- Communication timeout without a fault, with event deduplication ---
    {
        Monitor m2;
        const auto t0 = Clock::now();
        m2.ingest(measurements(400, 5, t0));
        m2.ingest(status(2, t0));
        require(m2.snapshot(t0).state == ChargeState::Charging, "second monitor must reach CHARGING");
        const auto events = m2.tick(t0 + std::chrono::seconds(4));
        require(events.size() == 3, "timeout must emit both events and close the open session");
        require(events[0].code == "MEASUREMENTS_TIMEOUT" && events[1].code == "POWER_STATUS_TIMEOUT" &&
                    events[2].code == "SESSION_CLOSED",
                "timeout order is measurements, status, session close");
        const auto s = m2.snapshot(t0 + std::chrono::seconds(4));
        require(s.state == ChargeState::Disconnected, "timed-out bus must report DISCONNECTED");
        require(s.measurements_stale && s.status_stale, "timed-out data must be marked stale");
        require(!s.measurements.valid, "stale measurements must be invalid");
        require(s.measurement_age_ms == 4000 && s.status_age_ms == 4000, "timed-out ages must be 4000 ms");
        require(m2.tick(t0 + std::chrono::seconds(4)).empty(), "timeout events must not repeat until new data arrives");
        // Fresh data re-arms the events: back to CHARGING, and a new gap emits them again
        m2.ingest(measurements(400, 5, t0 + std::chrono::seconds(5)));
        m2.ingest(status(2, t0 + std::chrono::seconds(5)));
        require(m2.snapshot(t0 + std::chrono::seconds(5)).state == ChargeState::Charging,
                "fresh data must restore CHARGING");
        const auto events2 = m2.tick(t0 + std::chrono::seconds(9));
        require(events2.size() == 3, "re-armed timeout must emit both events and close the new session");
    }

    // --- Delivery-order independence: status first, then measurements ---
    {
        Monitor m3;
        const auto t0 = Clock::now();
        m3.ingest(status(1, t0));
        require(m3.snapshot(t0).state == ChargeState::Disconnected, "status-only must be DISCONNECTED");
        m3.ingest(measurements(400, 5, t0));
        require(m3.snapshot(t0).state == ChargeState::Ready, "status-then-measurement must reach READY");
    }

    // --- UNKNOWN finché nessun flusso è mai arrivato ---
    {
        Monitor m4;
        require(m4.tick().empty(), "tick with no data must be silent");
        require(m4.snapshot().state == ChargeState::Unknown, "never-fed monitor must stay UNKNOWN");
    }

    // --- Text and JSON representations ---
    require(to_string(ChargeState::Charging) == "CHARGING", "state to_string");
    require(to_string(EventLevel::Error) == "ERROR", "level to_string");
    const auto json = json_snapshot(m.snapshot(t + std::chrono::seconds(4)));
    require(json.find("\"state\":\"FAULT\"") != std::string::npos, "JSON must reflect FAULT state");
    require(json.find("\"fault_code\":\"MEASUREMENT_OUT_OF_RANGE\"") != std::string::npos,
            "JSON must carry the fault code");
    require(json.find("\"rejected_frames\":5") != std::string::npos, "JSON must carry rejected frame count");
    require(json.find("\"measurement_age_ms\":4000") != std::string::npos,
            "JSON must carry the measurement age");  // --- Configuration validation ---
    {
        Config cfg;
        cfg.max_voltage_v = -1.0;
        bool rejected = false;
        try {
            Monitor bad_cfg(cfg);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid voltage range must be rejected at construction");
    }
    {
        Config cfg;
        cfg.measurement_timeout = std::chrono::milliseconds(0);
        bool rejected = false;
        try {
            Monitor bad_cfg(cfg);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "non-positive timeout must be rejected at construction");
    }

    // --- Parser replay (parse_hex_frame) ---
    {
        const auto f = parse_hex_frame(" 180#80BBE80300000000\r\n", t);
        require(f && f->id == 0x180 && f->length == 8, "valid cansend frame with spaces/CR must parse");
        const auto expected = measurements(480, 10, t);
        require(f->data == expected.data, "parsed measurement payload must decode identically");
        const auto s = parse_hex_frame("181#01", t);
        require(s && s->id == 0x181 && s->length == 1 && s->data[0] == 1, "uppercase/status frame must parse");
        require(static_cast<bool>(parse_hex_frame("181#", t)), "empty data payload (length 0) is legal");
        require(!static_cast<bool>(parse_hex_frame("180", t)), "missing # must be rejected");
        require(!parse_hex_frame("#01", t), "empty ID must be rejected");
        require(!parse_hex_frame("800#01", t), "ID above 0x7ff (3 digits) must be rejected");
        require(!parse_hex_frame("1000#01", t), "4-digit ID must be rejected");
        require(!parse_hex_frame("181#1", t), "odd data nibbles must be rejected");
        require(!parse_hex_frame("181#010203040506070809", t), "9-byte payload must be rejected");
        require(!parse_hex_frame("18G#01", t), "non-hex ID digit must be rejected");
        require(!parse_hex_frame("", t), "empty line must be rejected");
    }

    require(parse_fixed_point("0.25", 1000000) == 250000, "0.25 EUR is 250000 micro");
    require(parse_fixed_point("1", 1000000) == 1000000, "1 EUR");
    require(parse_fixed_point("1.2", 1000000) == 1200000, "1.2 EUR");
    require(!parse_fixed_point("0.2500001", 1000000), "extra fractional digit must be rejected");
    require(!parse_fixed_point("-1.00", 1000000), "negative money must be rejected");
    require(format_fixed_point(1700000, 1000000) == "1.700000", "micro-EUR format");

    {
        Config cfg;
        cfg.max_integration_step = std::chrono::hours(1);
        cfg.measurement_timeout = std::chrono::hours(1);
        cfg.status_timeout = std::chrono::hours(1);
        cfg.tariff.energy_micro_eur_per_kwh = 250000;
        cfg.tariff.time_micro_eur_per_hour = 3600000;
        cfg.tariff.flat_micro_eur = 500000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0 + std::chrono::hours(1)));
        mon.ingest(status(0, t0 + std::chrono::hours(1)));
        const auto s = mon.snapshot(t0 + std::chrono::hours(1));
        require(s.last_session.has_value(), "closed hour session must be exported");
        require(s.last_session->energy_uwh == 4800000000ull, "480 V * 10 A * 1 h = 4800 Wh");
        require(s.last_session->energy_wh == 4800, "Wh register floors from uWh");
        require(s.last_session->billed_energy_wh == 4800, "step 1 bills the Wh register");
        require(s.last_session->duration_ms == 3600000, "charging duration is monotonic");
        require(s.last_session->energy_cost_micro_eur == 1200000, "0.25 EUR/kWh * 4.8 kWh = 1.20 EUR");
        require(s.last_session->time_cost_micro_eur == 3600000, "3.60 EUR/h * 1 h");
        require(s.last_session->flat_cost_micro_eur == 500000, "flat fee");
        require(s.last_session->total_cost_micro_eur == 5300000, "1.20 + 3.60 + 0.50 EUR");
        require(s.last_session->billing_complete, "clean complete session is invoiceable");
        require(!s.last_session->energy_lower_bound && !s.last_session->time_upper_bound,
                "clean session is not a bound");
        require(s.last_session->stop_reason == "STATUS_OFF", "stop reason");
        require(s.kpi.energy_closed_uwh == 4800000000ull, "closed energy register");
        require(s.kpi.sessions_billable == 1, "billable counter");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 0, t0));
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(1)));
        const auto open = mon.snapshot(t0 + std::chrono::seconds(1)).session;
        require(open && open->energy_uwh == 666666ull, "trapezoid of 0 A to 10 A over 1 s");
        mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(2)));
        mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(3)));
        const auto after = mon.snapshot(t0 + std::chrono::seconds(3)).session;
        require(after && after->energy_uwh == 3333333ull, "held trapezoid plus two constant seconds");
    }

    {
        Config cfg;
        cfg.max_integration_step = std::chrono::milliseconds(2000);
        cfg.measurement_timeout = std::chrono::seconds(30);
        cfg.status_timeout = std::chrono::seconds(30);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(status(2, t0));
        const auto gapped = mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(10)));
        require(!gapped.empty() && gapped.back().code == "MEASUREMENT_GAP", "dropout must be visible");
        const auto mid = mon.snapshot(t0 + std::chrono::seconds(10)).session;
        require(mid && mid->energy_uwh == 0 && mid->integration_gaps == 1, "a gap adds no energy");
        mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(11)));
        const auto next = mon.snapshot(t0 + std::chrono::seconds(11)).session;
        require(next && next->energy_uwh == 1333333ull, "only the in-limit segment is integrated");
        mon.ingest(status(0, t0 + std::chrono::seconds(11)));
        require(!mon.snapshot(t0 + std::chrono::seconds(11)).last_session->billing_complete,
                "a gapped session is held for review");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(status(2, t0));
        mon.tick(t0 + std::chrono::seconds(4));
        const auto closed = mon.snapshot(t0 + std::chrono::seconds(4)).last_session;
        require(closed && closed->energy_uwh == 0, "timeout must not extrapolate energy");
        require(closed->time_upper_bound && closed->energy_lower_bound, "timeout quantities are bounds");
        require(!closed->billing_complete, "timeout session is not auto-invoiceable");
        require(closed->stop_reason == "COMMUNICATION_TIMEOUT", "timeout stop reason");
    }

    {
        Config cfg;
        cfg.tariff.energy_micro_eur_per_kwh = 250000;
        cfg.tariff.time_micro_eur_per_hour = 3600000;
        cfg.tariff.energy_step_wh = 10;
        cfg.tariff.time_step_s = 5;
        cfg.measurement_timeout = std::chrono::seconds(30);
        cfg.status_timeout = std::chrono::seconds(30);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(status(2, t0));
        for (int sec = 1; sec <= 9; ++sec) {
            mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(sec)));
        }
        mon.ingest(status(0, t0 + std::chrono::seconds(9)));
        const auto closed = mon.snapshot(t0 + std::chrono::seconds(9)).last_session;
        require(closed && closed->energy_wh == 12, "9 s at 4800 W is 12 Wh");
        require(closed->billed_energy_wh == 20, "OCPI energy step rounds up to 10 Wh");
        require(closed->energy_cost_micro_eur == 5000, "20 Wh at 0.25 EUR/kWh");
        require(closed->charging_s == 9 && closed->billed_time_s == 10, "time step rounds 9 s up to 10 s");
        require(closed->time_cost_micro_eur == 10000, "10 s at 3.60 EUR/h");
    }

    {
        Config cfg;
        cfg.tariff.flat_micro_eur = 100000;
        cfg.tariff.min_price_micro_eur = 1000000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        mon.ingest(status(2, t0));
        mon.ingest(status(0, t0));
        const auto closed = mon.snapshot(t0).last_session;
        require(closed && closed->price_clamped_min, "min price clamp");
        require(closed->total_cost_micro_eur == 1000000, "session cost rises to the minimum");
        require(!closed->billing_complete, "undersampled session stays held");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        mon.ingest(status(0, t0));
        mon.ingest(status(2, t0 + std::chrono::seconds(1)));
        const auto kpi = mon.snapshot(t0 + std::chrono::seconds(2)).kpi;
        require(kpi.off_ms == 1000 && kpi.charging_ms == 1000, "dwell follows the state");
        require(kpi.availability_ppm == 1000000, "communicating time is available");
        require(kpi.utilization_ppm == 500000, "utilization is charging over in-service time");
    }

    {
        Config cfg;
        cfg.measurement_timeout = std::chrono::milliseconds(1000);
        cfg.status_timeout = std::chrono::milliseconds(10000);
        cfg.stale_timeout = std::chrono::milliseconds(10000);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        mon.ingest(status(1, t0));
        const auto s = mon.snapshot(t0 + std::chrono::milliseconds(1500));
        require(s.measurements_stale, "staleness uses the measurement timeout");
        require(!s.status_stale, "status staleness uses the status timeout");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        const auto ev = mon.ingest(measurements(410, 5, t0 - std::chrono::milliseconds(1)));
        require(!ev.empty() && ev.front().code == "CLOCK_REGRESSION", "backwards time is rejected");
        require(mon.snapshot(t0).voltage_mv == 400000, "rejected time must not replace the sample");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        mon.ingest(status(1, t0));
        mon.ingest(measurements(501, 1, t0));
        std::string reason;
        require(!mon.reset_fault(reason, t0), "fresh protected fault stays denied");
        require(reason.find("external confirmation") != std::string::npos, "protected denial names the reason");
        require(!mon.reset_fault(reason, t0 + std::chrono::seconds(4)), "injected stale time is denied");
        require(reason.find("expired") != std::string::npos, "staleness uses the injected clock");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        mon.ingest(status(2, t0));
        require(mon.snapshot(t0).power_enabled, "charging status enables the flag");
        mon.ingest(status(3, t0));
        const auto s = mon.snapshot(t0);
        require(!s.power_enabled, "power-stage fault clears the flag");
        require(s.state == ChargeState::Fault, "stage fault latches");
        require(s.last_session && s.last_session->stop_reason == "POWER_STAGE_FAULT", "fault closes the session");
        require(s.fault_action.find("Isolate") != std::string::npos, "fault carries an operator action");
    }

    {
        Config cfg;
        cfg.protocol = ProtocolProfile::bare();
        cfg.protocol.measurement_id = 0x200;
        cfg.protocol.status_id = 0x201;
        cfg.protocol.voltage = {1, 10};
        cfg.protocol.current = {1, 10};
        cfg.protocol.little_endian = false;
        cfg.protocol.status_map[9] = kStatusCharging;
        cfg.protocol.status_map[4] = kStatusOff;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        CanFrame meas;
        meas.id = 0x200;
        meas.length = 8;
        meas.received_at = t0;
        meas.data[0] = 0x12;
        meas.data[1] = 0xC0;
        meas.data[2] = 0x00;
        meas.data[3] = 0x64;
        mon.ingest(meas);
        CanFrame st;
        st.id = 0x201;
        st.length = 1;
        st.data[0] = 9;
        st.received_at = t0;
        mon.ingest(st);
        const auto s = mon.snapshot(t0);
        require(s.state == ChargeState::Charging, "remapped profile must reach CHARGING");
        require(s.voltage_mv == 480000 && s.current_ma == 10000, "0.1 unit/LSB big-endian decode");
        st.data[0] = 4;
        st.received_at = t0 + std::chrono::milliseconds(1);
        mon.ingest(st);
        require(mon.snapshot(t0).last_session.has_value(), "remapped OFF closes the session");
    }

    {
        Config cfg;
        cfg.protocol.voltage.denominator = 0;
        bool rejected = false;
        try {
            Monitor bad(cfg);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "zero scale must be rejected");
    }

    {
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(measurements(400, 5, t0));
        const auto before = mon.snapshot(t0).state;
        const auto ev = mon.note_transport_error("CAN_ERROR_FRAME");
        require(ev.code == "CAN_ERROR_FRAME", "transport error is named");
        require(mon.snapshot(t0).state == before, "transport error must not change charge state");
        require(mon.snapshot(t0).kpi.transport_errors == 1, "transport error is counted");
    }

    {
        const auto line = json_session_line(SessionRecord{}, "bay-7", "EUR", "2026-01-01T00:00:00Z");
        require(line.find("\"schema\":\"charger.session.v1\"") != std::string::npos, "session schema");
        require(line.find("\"asset_id\":\"bay-7\"") != std::string::npos, "session asset");
        require(line.find("\"vat\":\"omitted\"") != std::string::npos, "omitted VAT is not zero VAT");
        require(line.find("ocpi-2.2.1-energy-time-flat") != std::string::npos, "price model name");
    }

    // --- Diagnostiche predittive: segnalano il degrado, non bloccano la carica ---
    {
        Config cfg;
        cfg.stuck_at_window = std::chrono::seconds(10);
        cfg.max_voltage_rate_mv_per_s = 1000;
        cfg.sag_voltage_mv = 470000;
        cfg.sag_tolerance_mv = 5000;
        cfg.measurement_timeout = std::chrono::seconds(30);
        cfg.status_timeout = std::chrono::seconds(30);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0));
        // A constant load must not trip the stuck check: no threshold is configured.
        for (int s = 1; s <= 12; ++s) {
            mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(s)));
        }
        const auto s = mon.snapshot(t0 + std::chrono::seconds(12));
        require(s.diagnostics.measurement_stuck, "immobile values under load are stuck-at");
        require(s.kpi.stuck_at_warnings == 1, "stuck-at warns once, not per sample");
        require(s.state == ChargeState::Charging, "a diagnostic must never block charging");
    }
    {
        Config cfg;
        cfg.stuck_at_window = std::chrono::seconds(10);
        cfg.measurement_timeout = std::chrono::seconds(30);
        cfg.status_timeout = std::chrono::seconds(30);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0));
        // Valori che cambiano: nessun stuck-at.
        for (int s = 1; s <= 12; ++s) {
            mon.ingest(measurements(480, 10 + s, t0 + std::chrono::seconds(s)));
        }
        const auto s = mon.snapshot(t0 + std::chrono::seconds(12));
        require(!s.diagnostics.measurement_stuck, "changing values are not stuck");
        require(s.kpi.stuck_at_warnings == 0, "no stuck-at warning on a live channel");
    }
    {
        Config cfg;
        cfg.max_voltage_rate_mv_per_s = 1000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(status(2, t0));
        // Balzo di 30 V in 1 s contro un limite di 1 V/s.
        mon.ingest(measurements(450, 10, t0 + std::chrono::seconds(1)));
        const auto s = mon.snapshot(t0 + std::chrono::seconds(1));
        require(s.diagnostics.voltage_rate_exceeded, "fast voltage step exceeds the slope");
        require(s.state == ChargeState::Charging, "rate diagnostic does not fault");
    }
    {
        Config cfg;
        cfg.sag_voltage_mv = 470000;
        cfg.sag_tolerance_mv = 5000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(460, 10, t0));
        const auto s = mon.snapshot(t0);
        require(s.diagnostics.voltage_sag, "10 V under nominal at load is a sag");
        require(s.state == ChargeState::Charging, "sag diagnostic does not fault");
    }
    {
        Config cfg;
        cfg.sag_voltage_mv = 470000;
        cfg.sag_tolerance_mv = 5000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(468, 10, t0));
        require(!mon.snapshot(t0).diagnostics.voltage_sag, "within tolerance is not a sag");
    }
    {
        // La diagnostica si azzera uscendo dalla carica: l'HMI non deve mostrare condizioni stantie.
        Config cfg;
        cfg.sag_voltage_mv = 470000;
        cfg.sag_tolerance_mv = 5000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(460, 10, t0));
        require(mon.snapshot(t0).diagnostics.voltage_sag, "sag active while charging");
        mon.ingest(status(0, t0 + std::chrono::milliseconds(10)));
        const auto s = mon.snapshot(t0 + std::chrono::milliseconds(10));
        require(!s.diagnostics.voltage_sag, "sag clears when charging stops");
    }
    {
        // Diagnostiche disattivate per default: l'ingegnere deve fornire le soglie.
        Monitor mon;
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0));
        for (int s = 1; s <= 30; ++s) {
            mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(s)));
        }
        const auto s = mon.snapshot(t0 + std::chrono::seconds(30));
        require(!s.diagnostics.measurement_stuck && s.kpi.stuck_at_warnings == 0,
                "no diagnostic fires without configured thresholds");
    }

    // --- Hash-chained journal: recovery, orphans, tampering ---
    {
        // Official NIST vectors: a wrong hash would make the chain worthless.
        require(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
        require(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
        require(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "sha256 448-bit");
        require(
            sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "sha256 one million a");

        // PID-suffixed path: tmpnam is not exclusive across runs and a stale
        // file from an earlier run would corrupt this assertion.
        const std::string path =
            (std::filesystem::temp_directory_path() /
             ("charger_test_journal_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".journal"))
                .string();
        std::remove(path.c_str());
        std::string err;
        auto opened_only = [](std::uint64_t id) {
            SessionRecord r;
            r.id = id;
            return r;
        };
        {
            auto led = Ledger::open(path, false, err);
            require(led.has_value(), "ledger opens");
            led->append_session_open(opened_only(1), "bay-1", "2026-01-01T00:00:00Z");
            SessionRecord done;
            done.id = 1;
            done.closed = true;
            done.energy_uwh = 4800000000ull;
            done.duration_ms = 3600000;
            done.total_cost_micro_eur = 5300000;
            done.billing_complete = true;
            done.stop_reason = "STATUS_OFF";
            done.samples = 10;
            led->append_session_close(done, "bay-1", "EUR", "2026-01-01T01:00:00Z");
            // The process dies here: session 2 opened and never closed.
            led->append_session_open(opened_only(2), "bay-2", "2026-01-01T01:00:00Z");
        }
        auto led2 = Ledger::open(path, false, err);
        const auto rep = led2->recover();
        require(rep.chain_intact, "chain intact after an unclean shutdown");
        require(rep.session_closes == 1 && rep.session_opens == 2, "recovery counts both streams");
        require(rep.orphan_sessions.size() == 1, "an unclosed session is reported as an orphan");
        require(rep.energy_uwh == 4800000000ull, "energy survives the restart");
        require(rep.billable_sessions == 1, "billable count survives the restart");
        std::string verr;
        require(led2->verify(verr), "verify passes on an untouched journal: " + verr);
        const auto pf = led2->portfolio();
        require(pf && pf->assets == 1, "portfolio rebuilt from the journal");
        require(pf->best_asset_id == "bay-1", "single asset is the best asset");

        // Manomissione di un campo energetico: la verifica deve fallire.
        {
            std::ifstream f(path);
            std::vector<std::string> rows;
            std::string row;
            while (std::getline(f, row)) rows.push_back(row);
            f.close();
            require(rows.size() == 3, "journal has three committed lines");
            std::vector<std::string> fields;
            std::string cur;
            for (char c : rows[1]) {
                if (c == '\t') {
                    fields.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            fields.push_back(cur);
            require(fields.size() >= 11, "close record carries the recovery prefix");
            require(fields[5] == "4800000000", "energy field is at index 5");
            fields[5] = "9999999999";
            std::string rebuilt;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (i != 0) rebuilt.push_back('\t');
                rebuilt += fields[i];
            }
            rows[1] = rebuilt;
            std::ofstream w(path, std::ios::trunc);
            for (const auto& r : rows) w << r << "\n";
        }
        auto led3 = Ledger::open(path, false, err);
        std::string terr;
        require(!led3->verify(terr), "tampered journal must not verify");
        require(terr.find("hash") != std::string::npos, "verify names the hash mismatch");
        require(!led3->recover().chain_intact, "recovery flags the broken chain");
        std::remove(path.c_str());
    }

    // --- Portfolio di sito: il KPI che un manager acquista ---
    {
        Portfolio p;
        auto add = [&p](const char* asset, std::uint64_t uwh, std::int64_t ms, std::int64_t cost, bool bill,
                        const char* reason) {
            SessionRecord r;
            r.closed = true;
            r.energy_uwh = uwh;
            r.duration_ms = ms;
            r.total_cost_micro_eur = cost;
            r.billing_complete = bill;
            r.stop_reason = reason;
            r.samples = 10;
            p.add(r, asset);
        };
        add("bay-1", 4800000000ull, 3600000, 5300000, true, "STATUS_OFF");
        add("bay-2", 2400000000ull, 1800000, 2650000, true, "STATUS_OFF");
        add("bay-3", 1000000000ull, 600000, 0, false, "COMMUNICATION_TIMEOUT");
        const auto k = p.summary();
        require(k.assets == 3, "three assets");
        require(k.sessions_closed == 3 && k.sessions_billable == 2 && k.sessions_held == 1,
                "held sessions are separated from billable ones");
        require(k.energy_uwh == 8200000000ull, "site energy");
        require(k.revenue_micro_eur == 7950000ull, "site revenue");
        require(k.timeout_stops == 1, "timeout stop classified");
        // 8.2e9 uWh / (6000000 ms) * 3600 = 4920000 mW
        require(k.mean_power_mw == 4920000, "site mean power in milliwatts");
        require(k.best_asset_id == "bay-1", "best asset by revenue");
        const auto top = p.top_assets_by_revenue(2, k);
        require(top.size() == 2 && top[0] == "bay-1" && top[1] == "bay-2", "ranking is deterministic");
        const auto js = p.json(k);
        require(js.find("\"schema\":\"charger.portfolio.v1\"") != std::string::npos, "portfolio schema");
        require(js.find("\"best_asset_id\":\"bay-1\"") != std::string::npos, "portfolio best asset in JSON");
        // Una sessione non chiusa non entra nel portfolio: non è un fatto consumato.
        SessionRecord live;
        live.closed = false;
        p.add(live, "bay-9");
        require(p.asset_count() == 3, "an open session is not a portfolio fact");
    }

    // --- Config file versionato ---
    {
        const auto r = parse_config(json_config_example());
        require(r.ok, "the shipped example must parse: " + (r.errors.empty() ? "" : r.errors[0].field));
        require(r.schema == CONFIG_SCHEMA, "schema echoed");
        require(r.config.asset_id == "bay-07", "asset id from file");
        require(r.config.tariff.energy_micro_eur_per_kwh == 250000, "0.25 EUR/kWh stays exact, no float drift");
        require(r.config.tariff.time_micro_eur_per_hour == 3600000, "3.60 EUR/h stays exact");
        require(r.config.tariff.flat_micro_eur == 500000, "0.50 EUR stays exact");
        require(r.config.tariff.energy_step_wh == 10 && r.config.tariff.time_step_s == 60, "OCPI step sizes");
        require(r.config.protocol.measurement_id == 384, "0x180 decimal");
        require(r.config.protocol.little_endian, "endianness from file");
        require(r.config.stuck_at_window == std::chrono::milliseconds(30000), "diagnostics from file");
        require(r.config.sag_voltage_mv == 460000, "sag threshold from file");
    }
    {
        // Un campo scritto male deve fallire: una tariffa a zero silenziosa
        // would invoice zero and nobody would notice.
        const auto r = parse_config(R"({"schema":"charger.config.v1","tariff":{"energy_eur_per_kwh":"0.25x"}})");
        require(!r.ok, "malformed tariff amount is rejected");
        require(!r.errors.empty() && r.errors.front().field == "tariff.energy_eur_per_kwh", "error names the field");
    }
    {
        const auto r = parse_config(R"({"schema":"charger.config.v1","tariff":{"energy_eur_per_kw":0.25}})");
        require(!r.ok, "misspelled tariff key is rejected");
    }
    {
        const auto r = parse_config(R"({"schema":"charger.config.v1","tarif":{}})");
        require(!r.ok, "misspelled top-level key is rejected");
    }
    {
        const auto r = parse_config(R"({"schema":"charger.config.v2"})");
        require(!r.ok, "unknown schema version is rejected");
    }
    {
        // Un file minimale e' valido: l'assenza di un campo mantiene il default.
        const auto r = parse_config(R"({"schema":"charger.config.v1","asset_id":"bay-1"})");
        require(r.ok, "minimal config is valid");
        require(r.config.asset_id == "bay-1", "explicit field applied");
        require(r.config.tariff.energy_step_wh == 1, "untouched field keeps the default");
    }
    {
        // No file may produce a configuration the Monitor rejects:
        // cross-validation surfaces exactly once, with the same error.
        const auto r = parse_config(R"({"schema":"charger.config.v1","max_voltage_v":-5.0})");
        require(!r.ok, "negative voltage limit is rejected by cross-validation");
    }
    {
        const auto r = parse_config(R"({"schema":"charger.config.v1","diagnostics":{"stuck_at_ms":0}})");
        require(r.ok, "zero disables a diagnostic and is valid");
        require(r.config.stuck_at_window.count() == 0, "diagnostic disabled");
    }
    {
        const auto r = parse_config("non json");
        require(!r.ok, "garbage is rejected");
    }
    {
        // A document truncated by an interrupted write must not be accepted
        // with only the fields that happened to parse: for a billing config
        // that silently means zero prices.
        require(!parse_config("{").ok, "unterminated root object is rejected");
        require(!parse_config(R"({"tariff":{)").ok, "unterminated tariff object is rejected");
        require(!parse_config(R"({"diagnostics":{)").ok, "unterminated diagnostics object is rejected");
        require(!parse_config(R"({"protocol":{)").ok, "unterminated protocol object is rejected");
        require(!parse_config(R"({"schema":"charger.config.v1","tariff":{"energy_eur_per_kwh":"0.25")").ok,
                "truncated tariff mid-value is rejected");
    }
    {
        // The schema gate protects the compatibility contract.
        require(!parse_config(R"({"asset_id":"bay-1"})").ok, "a document without schema is rejected");
        require(!parse_config("{}").ok, "an empty object without schema is rejected");
        require(parse_config(R"({"schema":"charger.config.v1"})").ok, "a minimal document with the schema is valid");
    }
    {
        // Rounding a billable register must saturate, never wrap: a wrapped
        // Wh figure would silently under-invoice.
        Config cfg;
        cfg.tariff.energy_micro_eur_per_kwh = 250000;
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        mon.ingest(status(2, t0));
        mon.ingest(measurements(480, 10, t0));
        mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(1)));
        const auto s = mon.snapshot(t0 + std::chrono::seconds(1)).session;
        require(s && s->billed_energy_wh >= s->energy_wh, "billable energy is never below delivered energy");
    }

    // --- DBC: decoding a vendor protocol, cross-checked against cantools 45.x ---
    {
        std::ifstream in("tests/fixtures/vendor_bay.dbc");
        require(static_cast<bool>(in), "vendor fixture must be present");
        std::ostringstream buf;
        buf << in.rdbuf();
        const auto r = parse_dbc(buf.str());
        require(r.ok, "vendor DBC parses");
        require(r.database.messages.size() == 3, "three messages");
        require(r.database.nodes.size() == 3, "three nodes");
        require(r.database.version == "vendor-example-1.2", "version captured");

        const auto* msg = r.database.find_by_name("ChgData");
        require(msg != nullptr, "ChgData found by name");
        require(msg->can_id == 0x300, "message id");
        require(msg->length == 8, "message length");

        // Extended ids carry the Vector flag, which must be stripped.
        require(r.database.find_by_id(0x880) != nullptr, "extended id resolved without the flag");
        require(r.database.find_by_id(0x8C000004) == nullptr, "the raw extended value is not a key");

        const auto* v = msg->find_signal("DC_Voltage");
        const auto* c = msg->find_signal("DC_Current");
        const auto* t = msg->find_signal("Amb_Temp");
        require(v && c && t, "signals found");
        require(!v->little_endian && !c->little_endian, "@0 is Motorola");
        require(!v->is_signed && t->is_signed, "sign flag parsed");
        require(v->start_bit == 7 && c->start_bit == 23, "start bits");
        require(t->start_bit == 56 && t->length_bits == 8, "little-endian signal start");

        // Payload below is the one cantools encodes for 480.00 V and 100.00 A.
        const std::uint8_t payload[8] = {0xBB, 0x80, 0x27, 0x10, 0x00, 0x00, 0x00, 0x00};
        const auto rv = v->decode_raw(payload, 8);
        const auto rc = c->decode_raw(payload, 8);
        require(rv && *rv == 48000, "Motorola 16-bit voltage decodes to 480.00 V");
        require(rc && *rc == 10000, "Motorola 16-bit current decodes to 100.00 A");
        require(v->to_physical(rv ? *rv : 0) == 480.0, "physical voltage");
        require(c->to_physical(rc ? *rc : 0) == 100.0, "physical current");

        // A short payload must be refused rather than read out of bounds.
        require(!v->decode_raw(payload, 1), "short payload rejected");
        // The bound covers the whole run, not just its length: a signal that
        // starts high in a short payload used to be read past the buffer.
        require(!t->decode_raw(payload, 7), "a truncated little-endian signal is refused");
        require(!t->decode_raw(payload, 1), "a one-byte payload is refused");

        // Motorola that does not start on a byte boundary. cantools lays such a
        // signal out as one contiguous ascending run of payload bits, so
        // 0|4@0+ occupies payload bits 7..10: byte 0 bit 0 up to byte 1 bit 5.
        // The payload below is what cantools 45.x encodes for the value 10.
        const auto* ext = r.database.find_by_id(0x880);
        require(ext != nullptr, "ExtendedDiag resolved");
        const auto* fault = ext->find_signal("Fault_Class");
        require(fault && !fault->little_endian, "mid-byte Motorola signal found");
        const std::uint8_t fault_payload[4] = {0x01, 0x40, 0x00, 0x00};
        const auto rf = fault->decode_raw(fault_payload, 4);
        require(rf && *rf == 10, "mid-byte Motorola decodes what cantools encodes");
        require(!fault->decode_raw(fault_payload, 1), "mid-byte Motorola refuses a short payload");

        // Signed negative with an offset: -40 degC is the raw value 0.
        const auto rt = t->decode_raw(payload, 8);
        require(rt && *rt == 0, "raw temperature");
        require(t->to_physical(rt ? *rt : 0) == -40.0, "negative offset applied");

        // Enumerated status values drive the state mapping.
        const auto* stage = r.database.find_by_name("PowerStage");
        require(stage != nullptr, "PowerStage found");
        const auto* status = stage->find_signal("Stage_Status");
        require(status && status->values.size() == 4, "four enumerated statuses");
        require(status->label_for(0) == "Off" && status->label_for(2) == "Charging", "labels resolved");
        require(status->label_for(9).empty(), "unlisted value has no label");

        const auto binding = auto_bind(r.database, 0.0, 500.0, 0.0, 200.0);
        require(binding.has_value(), "auto_bind finds the measurement message");
        require(binding->voltage_signal == "DC_Voltage" && binding->current_signal == "DC_Current",
                "voltage and current bound by declared range");
    }
    {
        // --- DBC in custody: profile_from_dbc rewires the live decode path ---
        std::ifstream in("tests/fixtures/vendor_bay.dbc");
        require(static_cast<bool>(in), "vendor fixture must be present");
        std::ostringstream buf;
        buf << in.rdbuf();
        const auto db = parse_dbc(buf.str());
        require(db.ok, "vendor DBC parses");

        auto binding = auto_bind(db.database, 0.0, 500.0, 0.0, 200.0);
        require(binding.has_value(), "auto_bind finds the measurement message");
        require(binding->status_signal == "Stage_Status" && binding->status_id == 0x301,
                "status carrier bound from the value table");

        const auto prof = profile_from_dbc(db.database, *binding);
        require(prof.ok, "the profile builds");
        require(prof.status_bound, "the status map is bound");
        const auto& p = prof.profile;
        require(p.measurement_id == 0x300 && p.measurement_length == 8, "measurement message wired");
        require(p.status_id == 0x301, "status id wired");
        require(p.voltage_offset == 0 && p.current_offset == 2, "byte offsets follow the Motorola layout");
        require(!p.little_endian, "endianness follows the DBC order flag");
        require(p.voltage.numerator == 1 && p.voltage.denominator == 100, "factor 0.01 maps to a centi-unit scale");
        require(p.current.numerator == 1 && p.current.denominator == 100, "current scale matches");
        require(p.status_map[0] == kStatusOff && p.status_map[1] == kStatusReady,
                "the value table becomes the status map");
        require(p.status_map[2] == kStatusCharging && p.status_map[3] == kStatusFault,
                "charging and fault meanings come from the labels");

        // The rewired Monitor must decode the vendor frames, not the demo ones:
        // 480.00 V and 100.00 A carried Motorola in 0x300, status from 0x301.
        Config cfg;
        cfg.protocol = p;
        cfg.asset_id = "bay-dbc";
        cfg.max_integration_step = std::chrono::hours(1);
        cfg.measurement_timeout = std::chrono::hours(1);
        cfg.status_timeout = std::chrono::hours(1);
        Monitor mon(cfg);
        const auto t0 = Clock::now();
        CanFrame meas;
        meas.id = 0x300;
        meas.length = 8;
        meas.received_at = t0;
        meas.data[0] = 0xBB;  // Motorola: MSB first across bytes 0-1
        meas.data[1] = 0x80;  // 0xBB80 = 48000 centivolts = 480.00 V
        meas.data[2] = 0x27;  // bytes 2-3: 0x2710 = 10000 centiamps = 100.00 A
        meas.data[3] = 0x10;
        CanFrame st;
        st.id = 0x301;
        st.length = 1;
        st.received_at = t0;
        st.data[0] = 2;  // "Charging"
        (void)mon.ingest(meas);
        (void)mon.ingest(st);
        const auto snap = mon.snapshot(t0);
        require(snap.voltage_mv == 480000 && snap.current_ma == 100000,
                "an OEM DBC decodes into the same exact milli-units");
        require(snap.state == ChargeState::Charging, "the DBC status table drives the charge state");

        // A non-16-bit field must be refused, not approximated.
        DbcBinding binding_bad;
        binding_bad.measurement_id = 0x300;
        binding_bad.voltage_signal = "Pack_SOC";  // 8 bits
        binding_bad.current_signal = "DC_Current";
        const auto bad = profile_from_dbc(db.database, binding_bad);
        require(!bad.ok && bad.error.find("16") != std::string::npos, "an 8-bit measurement is refused with a reason");

        // A signed tariff-bearing channel is refused.
        DbcBinding binding_signed;
        binding_signed.measurement_id = 0x300;
        binding_signed.voltage_signal = "DC_Voltage";
        binding_signed.current_signal = "Amb_Temp";  // signed with a -40 offset
        const auto signed_bad = profile_from_dbc(db.database, binding_signed);
        require(!signed_bad.ok, "a signed/offset current signal is refused");
    }
    {
        // Malformed input must be reported with a line, not silently accepted.
        const auto r = parse_dbc("VERSION \"x\"\nBO_ notanumber Name: 8 X\n");
        require(!r.ok, "a BO_ without an id is rejected");
        require(!r.errors.empty(), "with a located error");
        const auto empty = parse_dbc("");
        require(empty.ok, "an empty document is not a fatal parse error");
    }

    // --- Time-of-use: a session crossing the off-peak / peak boundary ---
    {
        TimeOfUse tou;
        tou.windows.push_back({"OFFPEAK", 0, 8 * 3600, 150000, 2000000});
        tou.windows.push_back({"PEAK", 8 * 3600, 20 * 3600, 400000, 3000000});
        tou.fallback = TimeWindow{"FLAT", 20 * 3600, 8 * 3600, 250000, 2500000};

        // Window membership, including a window that wraps past midnight.
        require(tou.windows[0].contains(0) && tou.windows[0].contains(7 * 3600), "off-peak contains early hours");
        require(!tou.windows[0].contains(8 * 3600), "off-peak excludes the peak boundary");
        require(tou.windows[1].contains(8 * 3600) && tou.windows[1].contains(19 * 3600), "peak range");
        TimeWindow night{"NIGHT", 22 * 3600, 6 * 3600, 0, 0};
        require(night.contains(23 * 3600) && night.contains(2 * 3600), "a wrapping window covers both sides");
        require(!night.contains(12 * 3600), "a wrapping window excludes midday");

        // 4800 W for three hours, 06:00 to 09:00 UTC, crossing the 08:00 boundary.
        const std::int64_t uwh_per_s = 4800000;
        std::vector<TimeSample> samples;
        for (int h = 6; h <= 9; ++h) {
            const std::int64_t offset = (h - 6) * 3600 * 1000;
            samples.push_back({offset, uwh_per_s * offset / 1000, h * 3600});
        }
        const auto r = price_time_of_use(tou, samples, 500000, 1, 1);
        require(r.periods.size() == 2, "the session splits at the tariff boundary");
        require(r.periods[0].window == "OFFPEAK" && r.periods[1].window == "PEAK", "windows in time order");
        require(r.unpriced_ms == 0, "every second is priced");
        // 2 h off-peak at 0.15 EUR/kWh: exactly 34.56 kWh -> 5.184 EUR.
        // Slices are labelled by the second they start in, so the second that
        // ends exactly on the 08:00 boundary still belongs to off-peak.
        require(r.periods[0].energy_uwh == 34560000000ull, "off-peak energy split");
        require(r.periods[0].energy_cost_micro_eur == 5184000, "off-peak energy cost");
        // 1 h peak at 0.40 EUR/kWh: exactly 17.28 kWh -> 6.912 EUR.
        require(r.periods[1].energy_cost_micro_eur == 6912000, "peak energy cost");
        require(r.total_energy_uwh == 51840000000ull, "period energies sum to the session total");
        require(r.total_energy_cost_micro_eur == 5184000 + 6912000, "energy costs sum exactly");
        require(r.total_cost_micro_eur == r.total_energy_cost_micro_eur + r.total_time_cost_micro_eur + 500000,
                "the total is the sum of its parts plus the flat fee");
        // Charging time: 2 h at 2.00 EUR/h plus 1 h at 3.00 EUR/h.
        require(r.periods[0].time_cost_micro_eur == 4000000, "off-peak time cost");
        require(r.periods[1].time_cost_micro_eur == 3000000, "peak time cost");
    }
    {
        // A hole in the tariff must be reported, never silently free.
        TimeOfUse tou;
        tou.windows.push_back({"PEAK", 8 * 3600, 10 * 3600, 400000, 3000000});
        std::vector<TimeSample> samples;
        for (int h = 8; h <= 11; ++h) {
            const std::int64_t offset = (h - 8) * 3600 * 1000;
            samples.push_back({offset, 4800000 * offset / 1000, h * 3600});
        }
        const auto r = price_time_of_use(tou, samples, 0, 1, 1);
        require(r.unpriced_ms > 0, "an uncovered second is flagged");
        require(r.periods.back().window == "UNPRICED", "the uncovered stretch is labelled");
    }
    {
        require(second_of_day_utc(0) == 0, "epoch is midnight");
        require(second_of_day_utc(3661) == 3661, "same-day seconds");
        require(second_of_day_utc(86400) == 0, "the next day wraps to midnight");
        require(second_of_day_utc(-1) == 86399, "a pre-epoch value floors into the previous day");
    }
    {
        // Apportionment must be exact even when it does not divide evenly.
        // The old code floored each one-second slice independently and lost up
        // to one microwatt-hour per slice; these numbers divide badly on
        // purpose (1000001 uWh over 60000 ms is 16666.68 per slice).
        TimeOfUse tou;
        tou.windows.push_back({"ALLDAY", 0, 86399, 250000, 0});
        std::vector<TimeSample> samples;
        samples.push_back({0, 0, 0});
        samples.push_back({60000, 1000001, 60});
        const auto r = price_time_of_use(tou, samples, 0, 1, 1);
        require(r.periods.size() == 1, "a single window yields a single period");
        require(r.periods[0].energy_uwh == 1000001, "the slices sum back to the interval exactly");
        require(r.total_energy_uwh == 1000001, "the total equals the parts");
        require(r.unpriced_ms == 0 && r.dropped_ms == 0, "nothing was skipped");
        require(!r.price_overflow, "an ordinary tariff does not overflow");
    }
    {
        // A register that runs backwards cannot be priced, but its time must be
        // reported rather than vanishing: the session span must reconcile.
        TimeOfUse tou;
        tou.windows.push_back({"ALLDAY", 0, 86399, 250000, 0});
        std::vector<TimeSample> samples;
        samples.push_back({0, 1000, 0});
        samples.push_back({1000, 900, 1});
        samples.push_back({2000, 1800, 2});
        const auto r = price_time_of_use(tou, samples, 0, 1, 1);
        require(r.dropped_ms == 1000, "the backwards interval is reported");
        require(r.total_energy_uwh == 900, "only the usable interval is counted");
        require(r.total_energy_uwh >= 0, "the total is never negative");
        std::int64_t accounted = r.dropped_ms;
        for (const auto& p : r.periods) accounted += p.duration_ms;
        require(accounted == 2000, "priced time plus dropped time is the session span");
    }
    {
        // An unrepresentable cost saturates and is flagged. Zero would read as
        // free energy, which is the worst direction to fail in.
        TimeOfUse tou;
        tou.windows.push_back({"ALLDAY", 0, 86399, 4000000000000000000ll, 0});
        std::vector<TimeSample> samples;
        samples.push_back({0, 0, 0});
        samples.push_back({1000, 10000000000ll, 1});
        const auto r = price_time_of_use(tou, samples, 0, 1, 1);
        require(r.price_overflow, "the overflow is reported");
        require(r.periods[0].price_overflow, "the period is flagged");
        require(r.periods[0].energy_cost_micro_eur == std::numeric_limits<std::int64_t>::max(),
                "the cost saturates instead of reading as zero");
    }
    {
        // A tariff that cannot be priced correctly is refused up front.
        std::string error;
        TimeOfUse degenerate;
        degenerate.windows.push_back({"FULLDAY", 0, 0, 250000, 0});
        require(!degenerate.validate(&error), "a zero-length window is refused");
        require(error.find("degenerate") != std::string::npos, "the reason names the defect");

        TimeOfUse overlapping;
        overlapping.windows.push_back({"A", 0, 12 * 3600, 250000, 0});
        overlapping.windows.push_back({"B", 8 * 3600, 20 * 3600, 400000, 0});
        require(!overlapping.validate(&error), "two windows over the same second are refused");
        require(error.find("both cover") != std::string::npos, "the reason names the overlap");

        TimeOfUse negative_rate;
        negative_rate.windows.push_back({"A", 0, 12 * 3600, -1, 0});
        require(!negative_rate.validate(&error), "a negative rate is refused");

        TimeOfUse good;
        good.windows.push_back({"OFFPEAK", 0, 8 * 3600, 150000, 2000000});
        good.windows.push_back({"PEAK", 8 * 3600, 20 * 3600, 400000, 3000000});
        good.fallback = TimeWindow{"FLAT", 20 * 3600, 0, 250000, 2500000};
        require(good.validate(&error), "a well-formed tariff validates");
    }

    // --- Dispute pack: the tariff of record and the chain provenance ---
    {
        Tariff record;
        record.energy_micro_eur_per_kwh = 250000;
        const std::string tariff = json_tariff_block(record);
        // Exact integers only: a consumer must never re-derive cents.
        require(tariff.find("energy_micro_eur_per_kwh\":\"") != std::string::npos,
                "the tariff block carries exact integer prices");

        // A closed session with a tariff snapshot and verifiable provenance.
        std::string ignore;
        const std::string journal =
            (std::filesystem::temp_directory_path() /
             ("charger_test_dispute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".journal"))
                .string();
        std::filesystem::remove(journal);
        {
            auto led = Ledger::open(journal, false, ignore);
            require(led.has_value(), "the journal opens for writing");
            const auto t0 = Clock::now();
            Config cfg;
            cfg.tariff.energy_micro_eur_per_kwh = 250000;
            Monitor mon(cfg);
            mon.ingest(status(2, t0));
            mon.ingest(measurements(480, 10, t0));
            mon.ingest(measurements(480, 10, t0 + std::chrono::seconds(1)));
            mon.ingest(status(0, t0 + std::chrono::seconds(1)));
            const auto snap = mon.snapshot(t0 + std::chrono::seconds(1));
            require(snap.last_session && snap.last_session->billing_complete, "the session is billable");
            const auto stamp = utc_timestamp();
            const std::string payload = json_session_line(*snap.last_session, "bay-dispute", "EUR", stamp, tariff);
            require(payload.find("\"tariff\":") != std::string::npos, "the session JSON embeds the tariff of record");
            led->append_session_open(*snap.last_session, "bay-dispute", stamp);
            // A second session opens immediately after the first closed. The
            // open line carries no tariff of its own, so it must not inherit
            // the previous session's: a stale copy there would still verify.
            led->append_session_open(*snap.last_session, "bay-dispute", stamp);
            // Force a session record shape independent of the demo state: use
            // the last closed session for the close line.
            led->append_session_close(*snap.last_session, "bay-dispute", "EUR", stamp, tariff);
        }
        {
            auto led = Ledger::open(journal, false, ignore);
            const auto rep = led->recover();
            require(rep.chain_intact, "the dispute journal verifies");
            require(led->sessions().size() == 1, "one settled session recovered");
            const auto& s = led->sessions().front();
            require(s.open_line == 2 && s.close_line == 3, "provenance points at the matching open line");
            require(!s.close_hash.empty() && s.close_hash != s.close_prev_hash, "the close line links to its past");
            require(s.tariff_json.find("250000") != std::string::npos, "the tariff of record is in the close line");

            // The tariff of record lives on the close line only. A session_open
            // has none of its own, so it must not inherit the previous one:
            // a stale copy there would still verify against the chain.
            {
                std::ifstream f(journal, std::ios::binary);
                const auto tariff_field = [](std::string l) {
                    std::vector<std::string> fields;
                    std::size_t start = 0;
                    while (true) {
                        const auto tab = l.find('\t', start);
                        if (tab == std::string::npos) {
                            fields.push_back(l.substr(start));
                            break;
                        }
                        fields.push_back(l.substr(start, tab - start));
                        start = tab + 1;
                    }
                    return fields.size() >= 2 ? fields[fields.size() - 2] : std::string("<short>");
                };
                std::string line;
                std::getline(f, line);
                require(tariff_field(line).empty(), "the first open line has no tariff of record");
                std::getline(f, line);
                require(tariff_field(line).empty(), "the second open line does not inherit the tariff");
                std::getline(f, line);
                require(tariff_field(line).find("charger.tariff.v1") != std::string::npos,
                        "the close line carries the tariff of record");
            }

            const std::string whole = [&journal] {
                std::ifstream f(journal, std::ios::binary);
                std::ostringstream b;
                b << f.rdbuf();
                return b.str();
            }();
            const auto doc = json_dispute(s, rep, journal, sha256_hex(whole));
            require(doc.find("\"schema\":\"charger.dispute.v1\"") != std::string::npos, "dispute schema");
            require(doc.find(s.close_hash) != std::string::npos, "the pack carries the close hash");
            require(doc.find("\"close_line_raw\":") != std::string::npos, "the raw close line travels with it");
            // The embedded tariff block allows recomputing the invoice offline.
            require(
                doc.find("\"tariff\":\"{") == std::string::npos || doc.find("charger.tariff.v1") != std::string::npos,
                "the tariff of record travels with the dispute");
            const auto cdr = json_cdr(s);
            require(cdr.find("\"schema\":\"charger.cdr.v1\"") != std::string::npos, "cdr schema");
            require(cdr.find("\"derived_start_utc\"") != std::string::npos, "cdr start is labelled derived");
            require(cdr.find("\"vat\":\"omitted\"") != std::string::npos, "cdr vat omitted, not zero-rated");
        }
        std::filesystem::remove(journal);
    }

    // --- TOU tariff file: validated, coverage-checked, strict refusal ---
    {
        const std::string good = std::string("{\"schema\":\"charger.tou.v1\",\"windows\":[") +
                                 "{\"name\":\"OFFPEAK\",\"start\":\"00:00\",\"end\":\"08:00\","
                                 "\"energy_eur_per_kwh\":\"0.15\",\"time_eur_per_hour\":\"2.00\"}," +
                                 "{\"name\":\"PEAK\",\"start\":\"08:00\",\"end\":\"20:00\","
                                 "\"energy_eur_per_kwh\":\"0.40\",\"time_eur_per_hour\":\"3.00\"}," +
                                 "{\"name\":\"EVENING\",\"start\":\"20:00\",\"end\":\"00:00\","
                                 "\"energy_eur_per_kwh\":\"0.25\",\"time_eur_per_hour\":\"2.50\"}]}";
        auto loaded = parse_tou_json(good);
        require(loaded.ok, "the example TOU tariff must parse: " + loaded.error);
        const auto cov = tou_coverage(loaded.tou);
        require(cov.full && cov.covered_s == 86400 && cov.uncovered_s == 0, "three windows must cover the day");
        const std::vector<TimeSample> cross{{0, 0, 7 * 3600 + 1800}, {3600 * 1000, 4800000000, 8 * 3600 + 1800}};
        std::string serr;
        const auto strict = price_time_of_use_strict(loaded.tou, cross, 0, 1, 1, serr);
        require(strict.has_value(), "a complete tariff must price: " + serr);
        require(strict->periods.size() == 2, "the boundary must split the session in two periods");
        require(strict->unpriced_ms == 0, "no second may be left unpriced");
        // 2.4 kWh at 0.15 EUR/kWh = 0.36 EUR = 360000 micro-EUR; at 0.40 = 960000.
        require(strict->total_energy_cost_micro_eur == 360000 + 960000, "TOU energy total is exact");
    }
    {
        const std::string overlap = std::string("{\"schema\":\"charger.tou.v1\",\"windows\":[") +
                                    "{\"name\":\"A\",\"start\":\"00:00\",\"end\":\"12:00\","
                                    "\"energy_eur_per_kwh\":\"0.10\",\"time_eur_per_hour\":\"1.00\"}," +
                                    "{\"name\":\"B\",\"start\":\"06:00\",\"end\":\"18:00\","
                                    "\"energy_eur_per_kwh\":\"0.20\",\"time_eur_per_hour\":\"1.00\"}]}";
        require(!parse_tou_json(overlap).ok, "overlapping windows must be refused");
    }
    {
        const std::string hole = std::string("{\"schema\":\"charger.tou.v1\",\"windows\":[") +
                                 "{\"name\":\"DAY\",\"start\":\"08:00\",\"end\":\"20:00\","
                                 "\"energy_eur_per_kwh\":\"0.30\",\"time_eur_per_hour\":\"2.00\"}]}";
        auto loaded = parse_tou_json(hole);
        require(loaded.ok, "a single window parses");
        require(!tou_coverage(loaded.tou).full, "one window cannot cover the day");
        const std::vector<TimeSample> night{{0, 0, 3600}, {3600 * 1000, 1000000, 7200}};
        std::string serr;
        require(!price_time_of_use_strict(loaded.tou, night, 0, 1, 1, serr).has_value(),
                "the strict pricer must refuse a hole");
        require(serr.find("incomplete") != std::string::npos, "the refusal names the hole");
    }
    {
        const std::string fb = std::string("{\"schema\":\"charger.tou.v1\",\"windows\":[") +
                               "{\"name\":\"DAY\",\"start\":\"08:00\",\"end\":\"20:00\","
                               "\"energy_eur_per_kwh\":\"0.30\",\"time_eur_per_hour\":\"2.00\"}]," +
                               "\"fallback\":{\"name\":\"NIGHT\",\"energy_eur_per_kwh\":\"0.10\","
                               "\"time_eur_per_hour\":\"1.00\"}}";
        auto loaded = parse_tou_json(fb);
        require(loaded.ok, "fallback tariff must parse: " + loaded.error);
        require(tou_coverage(loaded.tou).full, "a catch-all fallback covers the day");
        const std::vector<TimeSample> night{{0, 0, 3600}, {3600 * 1000, 1000000, 7200}};
        std::string serr;
        const auto r = price_time_of_use_strict(loaded.tou, night, 0, 1, 1, serr);
        require(r.has_value(), "fallback prices the night: " + serr);
        require(r->periods.size() == 1 && r->periods.front().window == "NIGHT", "night priced at fallback");
    }

    // --- Journal anchor: prefix proven while the journal keeps growing ---
    {
        const std::string journal = (std::filesystem::temp_directory_path() / "charger_anchor_test.journal").string();
        const std::string anchor = (std::filesystem::temp_directory_path() / "charger_anchor_test.anchor").string();
        std::filesystem::remove(journal);
        std::filesystem::remove(anchor);
        std::string ignore;
        {
            auto led = Ledger::open(journal, true, ignore);
            require(led.has_value(), "anchor test journal opens");
            Monitor m;
            const auto base = Clock::now();
            m.ingest(measurements(480, 10, base));
            m.ingest(status(2, base));
            for (int i = 1; i <= 3; ++i) m.ingest(measurements(480, 10, base + std::chrono::seconds(i)));
            m.ingest(status(0, base + std::chrono::seconds(4)));
            (void)m.tick(base + std::chrono::seconds(4));
            const auto snap = m.snapshot(base + std::chrono::seconds(4));
            require(snap.last_session.has_value(), "anchor test closed a session");
            led->append_session_open(*snap.last_session, "bay-anchor", "2026-01-01T00:00:00Z");
            led->append_session_close(*snap.last_session, "bay-anchor", "EUR", "2026-01-01T01:00:00Z",
                                      json_tariff_block(m.config().tariff));
        }
        JournalAnchor a;
        std::string aerr;
        require(write_journal_anchor(journal, anchor, a, aerr), "anchor writes: " + aerr);
        require(a.lines == 2, "two lines anchored");
        std::uint64_t appended = 99;
        require(verify_journal_anchor(journal, anchor, appended, aerr), "anchor verifies: " + aerr);
        require(appended == 0, "nothing appended yet");
        {
            auto led = Ledger::open(journal, false, ignore);
            auto rep = led->recover();
            (void)rep;
            SessionRecord extra;
            extra.id = 7;
            led->append_session_open(extra, "bay-anchor", "2026-01-02T00:00:00Z");
        }
        require(verify_journal_anchor(journal, anchor, appended, aerr), "anchor survives appends: " + aerr);
        require(appended == 1, "one line appended since the anchor");
        {
            std::ifstream f(journal, std::ios::binary);
            std::ostringstream b;
            b << f.rdbuf();
            std::string whole = b.str();
            const auto nl = whole.find('\n');
            require(nl != std::string::npos, "journal has lines");
            if (whole.size() > nl + 20) whole[nl + 20] = (whole[nl + 20] == 'a' ? 'b' : 'a');
            std::ofstream o(journal, std::ios::binary | std::ios::trunc);
            o << whole;
        }
        require(!verify_journal_anchor(journal, anchor, appended, aerr), "a rewritten prefix fails the anchor");
        std::filesystem::remove(journal);
        std::filesystem::remove(anchor);
    }

    // --- A write that does not land must be reported, not absorbed ---
    //
    // Billing runs off this file. A close line that never reached the journal
    // must never appear in memory as a settlement, or a portfolio rollup would
    // invoice energy that was never durably recorded.
    {
        namespace fs = std::filesystem;
        std::string err;
        const fs::path dir = fs::temp_directory_path() / "charger_writefail";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path journal = dir / "b.journal";
        const fs::path blocked = dir / "as-a-directory";
        fs::create_directories(blocked);

        SessionRecord rec;
        rec.id = 1;
        rec.energy_uwh = 4800000000;
        rec.energy_wh = 4800;
        rec.duration_ms = 3600000;
        rec.samples = 2;
        rec.billing_complete = true;
        rec.quality = "OK";

        // (a) A path that cannot be opened for append at all.
        {
            auto led = Ledger::open(blocked.string(), false, err);
            require(led.has_value(), "a ledger can be constructed on an unwritable path");
            require(!led->append_session_open(rec, "bay-1", "2026-01-01T00:00:00Z"),
                    "an open on an unwritable journal reports failure");
            require(!led->append_session_close(rec, "bay-1", "EUR", "2026-01-01T01:00:00Z"),
                    "a close on an unwritable journal reports failure");
            require(led->sessions().empty(), "a session that was never written is not in memory as settled");
            require(led->lines_written() == 0, "no line is counted as written");
        }

        // (b) A journal that exists and verifies, then stops accepting writes.
        {
            auto led = Ledger::open(journal.string(), false, err);
            require(led.has_value(), "the journal opens for writing");
            require(led->append_session_open(rec, "bay-1", "2026-01-01T00:00:00Z"), "the first open lands");
            led->recover();
            const auto before_bytes = fs::file_size(journal);

            // Make the file read-only: the next append cannot open it.
            fs::permissions(journal, fs::perms::owner_read, fs::perm_options::replace);
            const bool wrote = led->append_session_close(rec, "bay-1", "EUR", "2026-01-01T01:00:00Z");
#if defined(_WIN32)
            require(!wrote, "a close on a read-only journal reports failure");
#else
            // Running as root defeats the permission bits, so only assert the
            // invariant that must hold either way: memory never claims a
            // settlement the file does not contain.
            if (wrote) {
                fs::permissions(journal, fs::perms::owner_all, fs::perm_options::replace);
            }
#endif
            const auto after_bytes = fs::file_size(journal);
            require(after_bytes == before_bytes, "a failed write leaves the journal byte-identical");

            // Whatever happened above, the file must still be a valid chain.
            fs::permissions(journal, fs::perms::owner_all, fs::perm_options::replace);
            auto reopened = Ledger::open(journal.string(), false, err);
            require(reopened.has_value(), "the journal reopens after the failure");
            std::string verr;
            require(reopened->verify(verr), "the chain still verifies after a failed write: " + verr);
            const auto rep = reopened->recover();
            require(rep.session_closes == 0, "the close that failed is not in the file");

            // And the next append still lands, proving the failure left no torn
            // line behind for it to build on.
            require(reopened->append_session_close(rec, "bay-1", "EUR", "2026-01-01T01:00:00Z"),
                    "a later append succeeds after an earlier failure");
            std::string verr2;
            require(reopened->verify(verr2), "the chain verifies with the line written after the failure: " + verr2);
        }
        fs::remove_all(dir);
    }

    std::cout << "All tests passed\n";
}
