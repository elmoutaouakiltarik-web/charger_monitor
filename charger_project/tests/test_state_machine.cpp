#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_config.hpp"
#include "charger_ledger.hpp"
#include "charger_monitor.hpp"

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
    // --- Macchina a stati di base ---
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

    // --- Frame rifiutati dal validatore ---
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

    // --- Guasto latched: nessun ripristino automatico ---
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

    // --- Timeout di comunicazione senza guasto, con deduplicazione degli eventi ---
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
        // Nuovi dati ri-armano gli event: riportano CHARGING e un nuovo gap produce di nuovo gli event
        m2.ingest(measurements(400, 5, t0 + std::chrono::seconds(5)));
        m2.ingest(status(2, t0 + std::chrono::seconds(5)));
        require(m2.snapshot(t0 + std::chrono::seconds(5)).state == ChargeState::Charging,
                "fresh data must restore CHARGING");
        const auto events2 = m2.tick(t0 + std::chrono::seconds(9));
        require(events2.size() == 3, "re-armed timeout must emit both events and close the new session");
    }

    // --- Ordine di arrivo indipendente: prima lo stato, poi le misure ---
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

    // --- Rappresentazioni testuali e JSON ---
    require(to_string(ChargeState::Charging) == "CHARGING", "state to_string");
    require(to_string(EventLevel::Error) == "ERROR", "level to_string");
    const auto json = json_snapshot(m.snapshot(t + std::chrono::seconds(4)));
    require(json.find("\"state\":\"FAULT\"") != std::string::npos, "JSON must reflect FAULT state");
    require(json.find("\"fault_code\":\"MEASUREMENT_OUT_OF_RANGE\"") != std::string::npos,
            "JSON must carry the fault code");
    require(json.find("\"rejected_frames\":5") != std::string::npos, "JSON must carry rejected frame count");
    require(json.find("\"measurement_age_ms\":4000") != std::string::npos,
            "JSON must carry the measurement age");  // --- Validazione della configurazione ---
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
        require(s.fault_action.find("Isolare") != std::string::npos, "fault carries an operator action");
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
        // Stesso valore per 12 s a carico costante: il sensore potrebbe essere bloccato.
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

    // --- Journal hash-chained: recovery, orfani, manomissione ---
    {
        // Vettori ufficiali NIST: un hash errato renderebbe la catena inutile.
        require(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
        require(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
        require(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "sha256 448-bit");
        require(
            sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "sha256 one million a");

        const std::string path = std::string(std::tmpnam(nullptr)) + ".journal";
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
            // Il processo muore qui: sessione 2 aperta e mai chiusa.
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
        // fatturerebbe zero euro e non se ne accorgerebbe nessuno.
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
        // Un file non puo' produrre una configurazione che Monitor rifiuta:
        // la validazione incrociata avviene una sola volta con lo stesso errore.
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

    std::cout << "All tests passed\n";
}
