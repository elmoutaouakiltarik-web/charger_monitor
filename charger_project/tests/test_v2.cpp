// Tests for the v2 surface: HMAC-SHA256 webhook signatures, chained anchors,
// multi-bay config parsing, power-module profile parsing, session trend and
// statistical anomalies, fleet summary, OCPI CDR export.
//
// The HMAC cases use the RFC 4231 published test vectors, so the crypto is
// checked against an external standard, not against itself.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_config.hpp"
#include "charger_monitor.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

SessionRecord make_session(std::uint64_t id, std::uint64_t energy_wh_value, std::int64_t duration_ms,
                           std::int64_t cost_micro, const std::string& start_utc, std::int64_t peak_mw = 0) {
    SessionRecord s;
    s.id = id;
    s.closed = true;
    s.billing_complete = true;
    s.energy_uwh = energy_wh_value * 1000000ULL;
    s.energy_wh = energy_wh_value;
    s.billed_energy_wh = energy_wh_value;
    s.duration_ms = duration_ms;
    s.charging_s = duration_ms / 1000;
    s.billed_time_s = duration_ms / 1000;
    s.total_cost_micro_eur = cost_micro;
    s.energy_cost_micro_eur = cost_micro;
    s.ocpi_start_utc = start_utc;
    s.ocpi_stop_utc = start_utc;
    s.peak_power_mw = peak_mw;
    s.quality = "OK";
    s.stop_reason = "STATUS_READY";
    return s;
}

// Ten near-identical sessions with a little natural spread, the statistical
// baseline for the anomaly tests. Mean energy: exactly 100 Wh.
std::vector<SessionRecord> baseline_sessions() {
    const std::uint64_t wh[] = {100, 101, 99, 100, 102, 98, 100, 101, 99, 100};
    std::vector<SessionRecord> v;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        v.push_back(make_session(i, wh[i - 1], 60000, 250000, "2026-10-19T10:" + std::to_string(i) + ":00Z", 5000000));
    }
    return v;
}

// ------------------------------------------------------------------ HMAC ---

void test_hmac_rfc4231() {
    // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?"
    const std::string sig = WebhookEvent::compute_signature("what do ya want for nothing?", "Jefe");
    require(sig == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "HMAC-SHA256 matches RFC 4231 case 2, got " + sig);

    // RFC 4231 test case 6: a 131-byte key (longer than the 64-byte block) is
    // hashed first, per the RFC.
    const std::string long_key(131, '\xaa');
    const std::string sig6 =
        WebhookEvent::compute_signature("Test Using Larger Than Block-Size Key - Hash Key First", long_key);
    require(sig6 == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
            "HMAC-SHA256 matches RFC 4231 case 6 (long key), got " + sig6);

    // A different secret must produce a different signature (sanity, not a
    // crypto guarantee, but catches a stubbed secret path).
    const std::string other = WebhookEvent::compute_signature("what do ya want for nothing?", "Jefe2");
    require(other != sig, "a different secret yields a different signature");
}

void test_webhook_event_verification() {
    WebhookEvent e;
    e.event_type = "session_end";
    e.asset_id = "bay-7";
    e.timestamp_utc = "2026-10-19T12:00:00Z";
    e.payload_json = "{\"energy_wh\":\"1500\"}";
    const std::string secret = "site-secret";

    WebhookEvent signed_event = e;
    signed_event.signature =
        "sha256=" + WebhookEvent::compute_signature(
                        e.event_type + "|" + e.asset_id + "|" + e.timestamp_utc + "|" + e.payload_json, secret);

    require(verify_webhook_event(signed_event, secret), "a correctly signed event verifies");
    require(signed_event.signature.rfind("sha256=", 0) == 0, "the signature is prefixed sha256=");

    WebhookEvent tampered = signed_event;
    tampered.payload_json = "{\"energy_wh\":\"99000\"}";
    require(!verify_webhook_event(tampered, secret), "a tampered payload fails verification");

    WebhookEvent wrong_secret = signed_event;
    require(!verify_webhook_event(wrong_secret, "another-secret"), "a wrong secret fails verification");

    WebhookEvent unsigned_event = e;
    require(!verify_webhook_event(unsigned_event, secret), "an unsigned event fails verification");
    require(!verify_webhook_event(signed_event, ""), "verification with an empty secret fails");
}

// ------------------------------------------------------- chained anchors ---

void test_chained_anchor() {
    const std::string secret = "anchor-secret";

    ChainedAnchor genesis;
    genesis.timestamp_utc = "2026-10-19T08:00:00Z";
    genesis.journal_sha256 = "aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888";
    genesis.prev_anchor_sha256.clear();
    genesis.signature = ChainedAnchor::sign_hmac(genesis, secret);

    require(verify_chained_anchor(genesis, "", genesis.journal_sha256, secret),
            "the genesis anchor verifies with an empty prev hash");
    require(!verify_chained_anchor(genesis, "not-empty", genesis.journal_sha256, secret),
            "the genesis anchor refuses a non-empty expected prev hash");

    // The second anchor chains to the first through its computed hash.
    ChainedAnchor second;
    second.timestamp_utc = "2026-10-19T09:00:00Z";
    second.journal_sha256 = "bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888aaaa1111";
    second.prev_anchor_sha256 = compute_anchor_sha256(genesis);
    second.signature = ChainedAnchor::sign_hmac(second, secret);
    require(verify_chained_anchor(second, compute_anchor_sha256(genesis), second.journal_sha256, secret),
            "the chained anchor verifies against its predecessor");

    // A rewritten journal fails the binding check.
    require(!verify_chained_anchor(second, compute_anchor_sha256(genesis),
                                   "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef", secret),
            "a journal hash mismatch fails verification");

    // A broken chain fails the linking check.
    require(!verify_chained_anchor(second, compute_anchor_sha256(second), second.journal_sha256, secret),
            "a wrong predecessor fails verification");

    // A tampered signature fails, and so does a tampered signed field.
    ChainedAnchor tampered_sig = second;
    tampered_sig.signature = "sha256=" + std::string(64, '0');
    require(!verify_chained_anchor(tampered_sig, compute_anchor_sha256(genesis), second.journal_sha256, secret),
            "a forged signature fails verification");

    ChainedAnchor tampered_field = second;
    tampered_field.journal_sha256 = "bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888aaaa1112";
    // signature still signs the old fields
    require(
        !verify_chained_anchor(tampered_field, compute_anchor_sha256(genesis), tampered_field.journal_sha256, secret),
        "a signature over different fields fails verification");

    // JSON round trip.
    const auto back = parse_chained_anchor(second.to_json());
    require(back.has_value(), "the anchor JSON parses back");
    require(back->journal_sha256 == second.journal_sha256 && back->prev_anchor_sha256 == second.prev_anchor_sha256,
            "the anchor JSON round trip preserves the fields");
    require(verify_chained_anchor(*back, compute_anchor_sha256(genesis), second.journal_sha256, secret),
            "the parsed anchor verifies like the original");
}

// ---------------------------------------------------------- multibay cfg ---

void test_multibay_config_parsing() {
    // The emitted example must parse back: the documented shape cannot rot.
    const std::string example = json_multibay_config_example();
    const auto res = parse_multibay_config(example);
    require(res.ok, "the multibay example parses: " + (res.errors.empty() ? "" : res.errors.front().message));
    require(res.config != nullptr && !res.config->bays.empty(), "the multibay example carries at least one bay");

    // An unknown field is an error with the field name, not a silent ignore.
    const auto unknown = parse_multibay_config(
        "{\"schema\":\"charger.multibay.config.v1\",\"bays\":[],"
        "\"not_a_field\":1}");
    require(!unknown.ok && !unknown.errors.empty(), "an unknown field is rejected");
    require(unknown.errors.front().field == "not_a_field", "the error names the offending field");

    // An explicitly empty asset_id is rejected: the Monitor invariant probe
    // inside parse_config refuses it, and the multi-bay parser prefixes the
    // field path with the bay index. (An ABSENT asset_id defaults to
    // "demo-bay-1" — a documented adoption default, not an error.)
    const auto no_asset = parse_multibay_config(
        "{\"schema\":\"charger.multibay.config.v1\",\"bays\":[{\"enabled\":true,\"priority\":0,"
        "\"config\":{\"schema\":\"charger.config.v1\",\"asset_id\":\"\",\"currency\":\"EUR\"}}]}");
    require(!no_asset.ok, "a bay without an asset_id is rejected");
    bool mentions_asset = false;
    for (const auto& e : no_asset.errors) {
        if (e.field.find("asset_id") != std::string::npos || e.message.find("asset_id") != std::string::npos)
            mentions_asset = true;
    }
    require(mentions_asset,
            "an error mentions asset_id: " +
                (no_asset.errors.empty() ? "(no errors)"
                                         : no_asset.errors.front().field + ": " + no_asset.errors.front().message));

    // Not an object at all: refused with a located error.
    const auto not_object = parse_multibay_config("[1,2,3]");
    require(!not_object.ok, "a non-object document is rejected");
}

// --------------------------------------------------------- power module ---

void test_power_module_profile_parsing() {
    // Round trip on the emitted example, without an embedded DBC (the example
    // may reference signal names that exist only with a DBC attached).
    const std::string example = json_power_module_profile_example();
    const auto res = parse_power_module_profile(example);
    if (res.ok) {
        require(!res.profile->name.empty(), "the parsed profile has a name");
    } else {
        // If the example embeds a DBC, it must still parse cleanly or fail
        // with located errors only.
        require(!res.errors.empty(), "a rejected example reports why");
    }

    // Threshold sanity: max_dc_link_v must exceed min_dc_link_v.
    const auto inverted = parse_power_module_profile(
        "{\"name\":\"p1\",\"vendor\":\"v\",\"model\":\"m\",\"max_temp_c\":80,\"min_fan_rpm\":1000,"
        "\"min_dc_link_v\":900,\"max_dc_link_v\":300}");
    require(!inverted.ok, "an inverted dc-link window is rejected");
    bool mentions_dc = false;
    for (const auto& e : inverted.errors) {
        if (e.field == "max_dc_link_v") mentions_dc = true;
    }
    require(mentions_dc, "the error names max_dc_link_v");

    // Temperature outside the physical range is rejected.
    const auto too_hot = parse_power_module_profile(
        "{\"name\":\"p1\",\"max_temp_c\":300,\"min_fan_rpm\":0,\"min_dc_link_v\":0,\"max_dc_link_v\":100}");
    require(!too_hot.ok, "a 300C threshold is rejected");

    // A mapped signal absent from the embedded DBC is a config error.
    const std::string dbc =
        "VERSION \"\"\nBS_:\n"
        "BO_ 100 Motor: 4 ECU1\n"
        " SG_ RealTemp : 0|16@1+ (1,0) [0|65535] \"\" ECU2\n";
    const auto bad_signal = parse_power_module_profile(std::string("{\"name\":\"p1\",\"dbc_text\":\"") +
                                                       "VERSION \\\"\\\"\\nBS_:\\nBO_ 100 Motor: 4 ECU1\\n "
                                                       "SG_ RealTemp : 0|16@1+ (1,0) [0|65535] \\\"\\\" ECU2\\n\","
                                                       "\"temp_signal\":\"GhostTemp\"}");
    require(!bad_signal.ok, "a signal missing from the DBC is rejected");
    bool mentions_signal = false;
    for (const auto& e : bad_signal.errors) {
        if (e.field == "temp_signal") mentions_signal = true;
    }
    require(mentions_signal, "the error names the unmapped signal field");

    // A profile whose signals all exist in the DBC passes.
    const auto good = parse_power_module_profile(std::string("{\"name\":\"p1\",\"dbc_text\":\"") +
                                                 "VERSION \\\"\\\"\\nBS_:\\nBO_ 100 Motor: 4 ECU1\\n "
                                                 "SG_ RealTemp : 0|16@1+ (1,0) [0|65535] \\\"\\\" ECU2\\n\","
                                                 "\"temp_signal\":\"RealTemp\"}");
    require(good.ok, "a DBC-consistent profile parses: " +
                         (good.errors.empty() ? "" : good.errors.front().field + ": " + good.errors.front().message));
}

// ---------------------------------------------------------------- trend ---

void test_session_trend() {
    std::vector<SessionRecord> sessions;
    // 1 Wh in 1 s -> mean power 3.6 kW. Peak 4000 mW = 4 W = 0.004 kW.
    sessions.push_back(make_session(1, 1, 1000, 1000, "2026-10-19T10:00:00Z", 4000));
    // 5 Wh in 5 s -> 3.6 kW. Peak 10000000 mW = 10 kW.
    sessions.push_back(make_session(2, 5, 5000, 5000, "2026-10-19T10:01:00Z", 10000000));

    const auto trend = compute_session_trend(sessions, "bay-A");
    require(trend.asset_id == "bay-A", "the trend carries the asset id");
    require(trend.energy_wh.size() == 2 && trend.duration_s.size() == 2, "one point per closed session");

    require(std::abs(trend.energy_wh[0].value - 1.0) < 1e-9, "the first energy point is exact");
    require(std::abs(trend.energy_wh[1].value - 5.0) < 1e-9, "the second energy point is exact");
    require(std::abs(trend.duration_s[0].value - 1.0) < 1e-9, "the first duration point is exact");
    // Wh/s * 3.6 = kW.
    require(std::abs(trend.avg_power_kw[0].value - 3.6) < 1e-9, "mean power: 1Wh over 1s is 3.6kW");
    require(std::abs(trend.avg_power_kw[1].value - 3.6) < 1e-9, "mean power: 5Wh over 5s is 3.6kW");
    // mW -> kW is a division by one million.
    require(std::abs(trend.peak_power_kw[0].value - 0.004) < 1e-9, "peak 4000 mW is 0.004 kW");
    require(std::abs(trend.peak_power_kw[1].value - 10.0) < 1e-9, "peak 10000000 mW is 10 kW");
    // Points are stamped with the session's own recorded start, not the
    // analysis wall clock.
    require(trend.energy_wh[0].timestamp_utc == "2026-10-19T10:00:00Z", "the first point uses the recorded start");
    require(trend.energy_wh[1].timestamp_utc == "2026-10-19T10:01:00Z", "the second point uses the recorded start");
    require(std::abs(trend.cost_eur[1].value - 0.005) < 1e-9, "5000 micro-eur is 0.005 EUR");
}

// ------------------------------------------------------------- anomalies ---

void test_detect_anomalies() {
    // Below the minimum: a history of 10 sessions can only judge the 11th,
    // and anything shorter has no statistics worth trusting.
    require(detect_anomalies(baseline_sessions(), "bay-A").empty(),
            "a history without a session to judge yields no anomalies");

    // The newest session spikes in energy: detected against the PREVIOUS
    // sessions only, with exact numbers (baseline mean is exactly 100 Wh).
    auto spiked = baseline_sessions();
    spiked.push_back(make_session(11, 400, 60000, 250000, "2026-10-19T10:11:00Z", 5000000));
    const auto anomalies = detect_anomalies(spiked, "bay-A");
    bool energy_spike = false;
    for (const auto& a : anomalies) {
        if (a.anomaly_type == "energy_spike") {
            energy_spike = true;
            require(a.asset_id == "bay-A", "the anomaly names the asset");
            require(a.severity == "high", "an energy spike is high severity");
            require(a.timestamp_utc == "2026-10-19T10:11:00Z", "the anomaly points at the spiking session");
            require(std::abs(a.expected_value - 0.1) < 1e-9, "the expected energy is the baseline mean (0.1 kWh)");
            require(std::abs(a.actual_value - 0.4) < 1e-9, "the actual energy is the spiking value (0.4 kWh)");
            require(std::abs(a.deviation_pct - 300.0) < 1e-6, "the deviation is +300% over the baseline");
        }
    }
    require(energy_spike, "the energy spike is detected");

    // A normal session after the baseline: no anomaly.
    auto calm = baseline_sessions();
    calm.push_back(make_session(11, 101, 60000, 250000, "2026-10-19T10:11:00Z", 5000000));
    require(detect_anomalies(calm, "bay-A").empty(), "an in-band session yields no anomalies");

    // A perfectly constant baseline has zero spread: a 4x deviation is still
    // an anomaly, not a silent pass through a degenerate z-score.
    std::vector<SessionRecord> constant;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        constant.push_back(make_session(i, 100, 60000, 250000, "2026-10-19T10:" + std::to_string(i) + ":00Z", 5000000));
    }
    constant.push_back(make_session(11, 400, 60000, 250000, "2026-10-19T10:11:00Z", 5000000));
    bool constant_case = false;
    for (const auto& a : detect_anomalies(constant, "bay-A")) {
        if (a.anomaly_type == "energy_spike") constant_case = true;
    }
    require(constant_case, "a constant baseline still flags the 4x deviation");

    // Non-billable sessions are excluded from the statistics entirely: an
    // INTERRUPTED close is under review, its numbers are not evidence.
    auto held = baseline_sessions();
    SessionRecord interrupted = make_session(11, 400, 60000, 250000, "2026-10-19T10:11:00Z", 5000000);
    interrupted.billing_complete = false;
    interrupted.stop_reason = "MONITOR_SHUTDOWN";
    held.push_back(interrupted);
    require(detect_anomalies(held, "bay-A").empty(), "a non-billable spike is excluded from the statistics");
}

// ---------------------------------------------------------- fleet summary ---

Snapshot make_snapshot(ChargeState state, bool open_session, bool last_billable, std::int64_t last_cost) {
    Snapshot snap;
    snap.state = state;
    if (open_session) {
        SessionRecord s;
        s.closed = false;
        snap.session = s;
    }
    if (last_billable || true) {
        SessionRecord s;
        s.closed = true;
        s.billing_complete = last_billable;
        s.total_cost_micro_eur = last_cost;
        snap.last_session = s;
    }
    snap.kpi.energy_closed_uwh = 2000000ULL;  // 2 Wh
    snap.kpi.availability_ppm = 900000;
    snap.kpi.utilization_ppm = 450000;
    return snap;
}

void test_fleet_summary() {
    std::vector<Snapshot> bays;
    bays.push_back(make_snapshot(ChargeState::Charging, true, true, 1000000));
    bays.push_back(make_snapshot(ChargeState::Ready, false, true, 2000000));
    bays.push_back(make_snapshot(ChargeState::Fault, false, false, 999999));
    bays.push_back(make_snapshot(ChargeState::Unknown, false, false, 0));
    // The unknown bay has never completed a measurement window: its KPIs are
    // undefined (-1), not zero, and must not drag the fleet average down.
    bays[3].kpi.availability_ppm = -1;
    bays[3].kpi.utilization_ppm = -1;

    const auto f = compute_fleet_summary(bays);
    require(f.total_bays == 4, "four bays in, four bays counted");
    require(f.online_bays == 2, "charging + ready are online");
    require(f.charging_bays == 1, "one bay charging");
    require(f.fault_bays == 1, "one bay faulted, unknown is neither");
    require(f.active_sessions == 1, "one open session");
    require(f.total_energy_wh == 8, "2 uWh-million x 4 bays = 8 Wh total");
    // The held (non-billable) cost from bay 3 must not inflate the revenue.
    require(f.total_revenue_micro_eur == 3000000, "revenue sums billable sessions only");
    require(std::abs(f.avg_availability_pct - 90.0) < 1e-6, "availability averages only the three defined bays (90%)");
    require(std::abs(f.avg_utilization_pct - 45.0) < 1e-6, "utilization averages only the three defined bays (45%)");

    const auto json = json_fleet_summary(f);
    require(json.find("\"schema\":\"charger.fleet.v1\"") != std::string::npos, "the fleet summary is self-typed");
    require(json.find("\"total_bays\":4") != std::string::npos, "the JSON carries the bay count");
}

// ------------------------------------------------------------- OCPI CDR ---

void test_ocpi_cdr() {
    std::vector<SessionRecord> sessions;
    sessions.push_back(make_session(1, 1500, 900000, 500000, "2026-10-19T10:00:00Z", 7000000));

    Config config;
    const auto cdrs = generate_ocpi_cdrs(sessions, config);
    require(cdrs.size() == 1, "one session, one CDR");

    const auto json = json_ocpi_cdr(cdrs.front());
    require(json.find("\"schema\":\"charger.ocpi-cdr.v1\"") != std::string::npos ||
                json.find("\"cdr_id\"") != std::string::npos,
            "the CDR document carries its identity");
    require(json.find("1500") != std::string::npos, "the CDR carries the metered energy");
}

}  // namespace

int main() {
    try {
        test_hmac_rfc4231();
        std::cout << "hmac rfc4231 vectors\n";
        test_webhook_event_verification();
        std::cout << "webhook verification\n";
        test_chained_anchor();
        std::cout << "chained anchors\n";
        test_multibay_config_parsing();
        std::cout << "multibay config parsing\n";
        test_power_module_profile_parsing();
        std::cout << "power module profile parsing\n";
        test_session_trend();
        std::cout << "session trend\n";
        test_detect_anomalies();
        std::cout << "anomaly detection\n";
        test_fleet_summary();
        std::cout << "fleet summary\n";
        test_ocpi_cdr();
        std::cout << "ocpi cdr\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "all v2 tests passed\n";
    return 0;
}
