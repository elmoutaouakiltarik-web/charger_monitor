#pragma once

// Monitoring library for an industrial DC charger bay.
// NOT firmware, NOT a certified controller, NOT a safety function, NOT a
// legal-metrology or fiscal meter. It does not command the power stage.
// Energy and cost figures are an operational ledger: exact integer arithmetic
// on accepted samples, no interpolation across dropouts. Invoice only when
// SessionRecord::billing_complete is true. See README.md.

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace charger {

constexpr const char* VERSION = "2.1.0";
constexpr const char* PROJECT_NAME = "charger_monitor";
constexpr const char* SNAPSHOT_SCHEMA = "charger.snapshot.v1";
constexpr const char* SESSION_SCHEMA = "charger.session.v1";
// stop_reason of a session that was open when the monitor itself was stopped
// (an update, a restart, a signal). See Monitor::shutdown().
constexpr const char* STOP_MONITOR_SHUTDOWN = "MONITOR_SHUTDOWN";

// Schema for chained anchors (anchor file format v2).
constexpr const char* CHAINED_ANCHOR_SCHEMA = "charger.anchor.chained.v1";

// Schema for the multi-bay configuration file.
constexpr const char* MULTIBAY_CONFIG_SCHEMA = "charger.multibay.config.v1";

using Clock = std::chrono::steady_clock;

// Exact 128-bit arithmetic for energy, tariff and ratio products. Aliased once
// here so -Wpedantic does not fire at every use site.
#if defined(__SIZEOF_INT128__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using WideInt = __int128;
#pragma GCC diagnostic pop
#else
#error "charger_monitor requires a 128-bit integer type (GCC or Clang)"
#endif

enum class ChargeState { Unknown, Off, Ready, Charging, Fault, Disconnected };
enum class EventLevel { Info, Warning, Error };

constexpr std::int8_t kStatusInvalid = -1;
constexpr std::int8_t kStatusOff = 0;
constexpr std::int8_t kStatusReady = 1;
constexpr std::int8_t kStatusCharging = 2;
constexpr std::int8_t kStatusFault = 3;

// SI = raw * numerator / denominator. Default raw is centi-units (1/100).
struct Scale {
    std::int64_t numerator{1};
    std::int64_t denominator{100};
};

// Retargets the demo codec without forking the state machine or the ledger.
// status_map[wire_byte] is kStatusInvalid or one of the kStatus* meanings.
// The default map is the demo protocol: 0 OFF, 1 READY, 2 CHARGING, 3 FAULT.
struct ProtocolProfile {
    std::uint32_t measurement_id{0x180};
    std::uint32_t status_id{0x181};
    Scale voltage{1, 100};
    Scale current{1, 100};
    std::uint8_t voltage_offset{0};
    std::uint8_t current_offset{2};
    bool little_endian{true};
    std::uint8_t measurement_length{8};
    bool reserved_must_be_zero{true};
    std::array<std::int8_t, 256> status_map{};

    ProtocolProfile();
    static ProtocolProfile bare();
};

// OCPI 2.2.1 subset: ENERGY (per kWh), TIME (per charging hour), FLAT (once
// per session). PARKING_TIME is not priced: this protocol has no proof that
// the connector is occupied while not charging. Prices are micro-EUR
// (1 EUR = 1_000_000) excluding VAT. VAT is omitted, which is not 0% VAT.
// step sizes follow OCPI: energy in Wh, time in seconds, ceiling to a
// multiple (an exact multiple is not increased). No time-of-use, no min/max
// restrictions other than the optional session price clamps.
struct Tariff {
    std::int64_t energy_micro_eur_per_kwh{0};
    std::int64_t time_micro_eur_per_hour{0};
    std::int64_t flat_micro_eur{0};
    std::int32_t energy_step_wh{1};
    std::int32_t time_step_s{1};
    std::optional<std::int64_t> min_price_micro_eur;
    std::optional<std::int64_t> max_price_micro_eur;
};

struct Config {
    double min_voltage_v{0.0};
    double max_voltage_v{500.0};
    double min_current_a{0.0};
    double max_current_a{200.0};
    std::chrono::milliseconds measurement_timeout{3000};
    std::chrono::milliseconds status_timeout{3000};
    // Retained so existing configs still compile. Staleness uses the
    // per-stream timeouts above; this field is not a second threshold.
    std::chrono::milliseconds stale_timeout{3000};
    std::chrono::milliseconds report_interval{1000};
    // Longest sample interval over which a linear power segment is accepted.
    // Longer gaps add no energy. Default suits a 1 Hz stream. Max 1 hour.
    std::chrono::milliseconds max_integration_step{2000};
    // Predictive diagnostics. All disabled by default because the thresholds
    // are installation-specific: an engineer must set them from the real
    // electrical data, never guessed. A diagnostic never latches a fault, it
    // only raises a warning: blocking charging on a heuristic is unsafe.
    // Samples must repeat the same value for this long before stuck-at is
    // reported, and a rate change must exceed the configured slope.
    std::chrono::milliseconds stuck_at_window{0};
    std::int64_t max_voltage_rate_mv_per_s{0};
    std::int64_t max_current_rate_ma_per_s{0};
    std::int64_t sag_voltage_mv{0};
    std::int64_t sag_tolerance_mv{0};
    std::int64_t current_hunt_band_ma{0};
    ProtocolProfile protocol{};
    Tariff tariff{};
    std::string currency{"EUR"};
    std::string asset_id{"demo-bay-1"};

    // Multi-bay and advanced features
    // Optional CAN interface name (e.g., "can0", "vcan0"). Empty = use default.
    std::string can_interface{""};
    // Optional power module profile name for vendor-specific DBC mapping.
    std::string power_module_profile{""};
    // Optional webhook URL for real-time event streaming.
    std::string webhook_url{""};
    // Webhook secret for HMAC-SHA256 signature verification.
    std::string webhook_secret{""};
    // Optional OCMF (Open Charge Metering Format) signed_data passthrough.
    // When enabled, the monitor includes the meter's signed_data in session records.
    bool ocmf_enabled{false};
    // OCPI CDR export: operator-supplied metadata for CDR generation.
    std::string ocpi_operator_id{""};
    std::string ocpi_country_code{""};
    std::string ocpi_party_id{""};
    std::string ocpi_location_id{""};
    std::string ocpi_evse_uid{""};
    std::string ocpi_connector_id{"1"};
    // Time-of-use tariff profile name (if multiple TOU profiles configured).
    std::string tou_profile{"default"};
    // Hot-reload: when true, the monitor watches config file for changes.
    bool hot_reload_enabled{false};
    // Path to config file for hot-reload.
    std::string config_file_path{""};
};

struct Measurements {
    double voltage_v{0.0};
    double current_a{0.0};
    Clock::time_point received_at{};
    bool valid{false};
};

struct SessionRecord {
    std::uint64_t id{0};
    bool closed{false};
    std::int64_t duration_ms{0};
    std::uint64_t energy_uwh{0};
    std::uint64_t energy_wh{0};
    std::uint64_t billed_energy_wh{0};
    std::int64_t charging_s{0};
    std::int64_t billed_time_s{0};
    std::int64_t peak_power_mw{0};
    std::int64_t min_voltage_mv{0};
    std::int64_t max_current_ma{0};
    std::uint32_t samples{0};
    std::uint32_t integration_gaps{0};
    bool undersampled{false};
    bool billing_complete{false};
    bool energy_lower_bound{false};
    bool time_upper_bound{false};
    bool price_clamped_min{false};
    bool price_clamped_max{false};
    std::string quality;
    std::string stop_reason;
    std::int64_t energy_cost_micro_eur{0};
    std::int64_t time_cost_micro_eur{0};
    std::int64_t flat_cost_micro_eur{0};
    std::int64_t total_cost_micro_eur{0};
    // OCMF signed_data passthrough
    // When Config::ocmf_enabled is true, this field contains the meter's
    // signed meter value (OCMF signedData) as base64-encoded string.
    std::string ocmf_signed_data{};
    // OCPI CDR fields for session export.
    std::string ocpi_cdr_id{};
    std::string ocpi_start_utc{};
    std::string ocpi_stop_utc{};
    std::uint64_t ocpi_meter_start_wh{0};
    std::uint64_t ocpi_meter_stop_wh{0};
};

struct AssetKpi {
    std::int64_t in_service_ms{0};
    std::int64_t unknown_ms{0};
    std::int64_t off_ms{0};
    std::int64_t ready_ms{0};
    std::int64_t charging_ms{0};
    std::int64_t fault_ms{0};
    std::int64_t disconnected_ms{0};
    std::int64_t availability_ppm{-1};
    std::int64_t utilization_ppm{-1};
    std::int64_t accept_ppm{-1};
    std::uint64_t energy_closed_uwh{0};
    std::uint64_t sessions_opened{0};
    std::uint64_t sessions_closed{0};
    std::uint64_t sessions_billable{0};
    std::uint64_t sessions_undersampled{0};
    std::uint64_t integration_gaps{0};
    std::uint64_t fault_events{0};
    std::uint64_t timeout_events{0};
    std::uint64_t transport_errors{0};
    std::uint64_t accepted_frames{0};
    std::uint64_t rejected_frames{0};
    std::uint64_t stuck_at_warnings{0};
    std::uint64_t rate_of_change_warnings{0};
    std::uint64_t sag_warnings{0};
    std::uint64_t current_hunt_warnings{0};
};

// Active predictive conditions, independent of the event stream so an HMI can
// render them without replaying events.
struct Diagnostics {
    bool measurement_stuck{false};
    bool voltage_rate_exceeded{false};
    bool current_rate_exceeded{false};
    bool voltage_sag{false};
    bool current_hunting{false};
};

struct Snapshot {
    ChargeState state{ChargeState::Unknown};
    Measurements measurements{};
    std::string fault_code;
    std::string fault_message;
    std::string fault_action;
    bool power_enabled{false};
    bool measurements_stale{true};
    bool status_stale{true};
    std::int64_t measurement_age_ms{-1};
    std::int64_t status_age_ms{-1};
    std::int64_t voltage_mv{0};
    std::int64_t current_ma{0};
    std::int64_t power_mw{0};
    std::uint64_t accepted_measurement_frames{0};
    std::uint64_t accepted_status_frames{0};
    std::uint64_t rejected_frames{0};
    std::string currency{"EUR"};
    std::string asset_id;
    Diagnostics diagnostics{};
    AssetKpi kpi{};
    std::optional<SessionRecord> session;
    std::optional<SessionRecord> last_session;
    // Multi-bay aggregate snapshot
    // When this is a MultiBayMonitor aggregate snapshot, this field contains
    // per-bay snapshots keyed by asset_id.
    std::vector<Snapshot> bay_snapshots{};
};

struct CanFrame {
    std::uint32_t id{0};
    std::uint8_t length{0};
    std::array<std::uint8_t, 8> data{};
    Clock::time_point received_at{Clock::now()};
};

struct Event {
    EventLevel level{EventLevel::Info};
    std::string code;
    std::string message;
};

// Chained Anchor (Anchor v2)
// Extends the anchor format with prev_anchor_sha256 to detect anchor deletion.
// Format: JSON object with fields:
//   schema: "charger.anchor.chained.v1"
//   timestamp_utc: RFC3339 UTC timestamp
//   journal_sha256: SHA256 of the journal file at anchor time
//   prev_anchor_sha256: SHA256 of the previous anchor file (empty for first)
//   signature: optional RFC3161 timestamp token or HMAC
struct ChainedAnchor {
    std::string schema{CHAINED_ANCHOR_SCHEMA};
    std::string timestamp_utc;
    std::string journal_sha256;
    std::string prev_anchor_sha256;  // empty for genesis anchor
    std::string signature;           // HMAC-SHA256 (see sign_hmac) or an external RFC3161 token

    std::string to_json() const;
    static std::optional<ChainedAnchor> from_json(const std::string& json);
    static std::string compute_sha256(const ChainedAnchor& anchor);

    // HMAC-SHA256 over schema|timestamp_utc|journal_sha256|prev_anchor_sha256
    // (the signature never covers itself). This is the signature that
    // verify_chained_anchor can check locally, with the site secret.
    static std::string sign_hmac(const ChainedAnchor& anchor, const std::string& secret);
};

// Webhook Event for real-time streaming
// Events sent to configured webhook endpoint with HMAC-SHA256 signature.
struct WebhookEvent {
    std::string event_type;  // e.g., "session_start", "session_end", "fault", "diagnostic"
    std::string asset_id;
    std::string timestamp_utc;  // RFC3339
    std::string payload_json;   // JSON payload specific to event_type
    std::string signature;      // "sha256=<hex>" HMAC over the canonical string
                                // event_type|asset_id|timestamp_utc|payload_json
                                // with the webhook secret; empty when unsigned.

    std::string to_json() const;
    // HMAC-SHA256 (RFC 2104) of the message, keyed with the secret. Longer
    // keys are hashed first, per the RFC.
    static std::string compute_signature(const std::string& payload, const std::string& secret);
};

// Receiver-side verification of a webhook event. Returns true only when the
// event carries a signature that matches the HMAC of its canonical form under
// this secret. Comparison is constant-time.
bool verify_webhook_event(const WebhookEvent& event, const std::string& secret);

// Multi-Bay Configuration
struct BayConfig {
    Config config;       // Per-bay configuration
    bool enabled{true};  // Whether this bay is active
    int priority{0};     // Processing priority (higher = more frequent)
};

struct MultiBayConfig {
    std::string schema{MULTIBAY_CONFIG_SCHEMA};
    std::vector<BayConfig> bays;
    // Global settings
    std::chrono::milliseconds global_report_interval{5000};
    bool enable_cross_bay_analytics{true};
    std::string webhook_url{""};
    std::string webhook_secret{""};
    // Fleet-level OCPI metadata
    std::string ocpi_operator_id{""};
    std::string ocpi_country_code{""};
    std::string ocpi_party_id{""};
};

// Power Module Profile
// Vendor-specific CAN signal mappings for power module diagnostics.
struct PowerModuleProfile {
    std::string name;
    std::string vendor;
    std::string model;
    std::string dbc_text;  // Embedded DBC content
    // Signal mappings for power module telemetry
    std::string temp_signal;  // Temperature signal name
    std::string fan_speed_signal;
    std::string dc_link_voltage_signal;
    std::string interlock_signal;
    std::string fault_code_signal;
    // Thresholds
    std::int32_t max_temp_c{80};
    std::int32_t min_fan_rpm{1000};
    std::int32_t min_dc_link_v{300};
    std::int32_t max_dc_link_v{900};
};

// Advanced Analytics Types
struct TrendPoint {
    std::string timestamp_utc;
    double value;
};

struct SessionTrend {
    std::string asset_id;
    std::vector<TrendPoint> energy_wh;
    std::vector<TrendPoint> duration_s;
    std::vector<TrendPoint> peak_power_kw;
    std::vector<TrendPoint> avg_power_kw;
    std::vector<TrendPoint> efficiency_pct;
    std::vector<TrendPoint> cost_eur;
};

struct FleetSummary {
    std::int64_t timestamp_utc_ms{0};
    std::uint64_t total_bays{0};
    std::uint64_t online_bays{0};
    std::uint64_t charging_bays{0};
    std::uint64_t fault_bays{0};
    std::uint64_t total_energy_wh{0};
    std::int64_t total_revenue_micro_eur{0};
    std::uint64_t active_sessions{0};
    double avg_availability_pct{0.0};
    double avg_utilization_pct{0.0};
};

struct AnomalyResult {
    std::string asset_id;
    std::string anomaly_type;  // "energy_spike", "duration_outlier", "cost_anomaly", "pattern_break",
                               // "fleet_drift"
    std::string severity;      // "low", "medium", "high", "critical"
    std::string timestamp_utc;
    double expected_value;
    double actual_value;
    double deviation_pct;
    std::string description;
    // Which detector fired: "zscore" (leave-one-out z over the pre-session
    // baseline, sensitive to the mean it uses), "iqr" (Tukey fences over the
    // same baseline, robust to the tails that inflate a mean), or
    // "fleet_median" (cross-bay drift). A reviewer sees which test produced
    // the number, not just the number.
    std::string method;
};

// Duty-factor degradation over an asset's closed billable sessions. The
// duty factor (charging time / wall time, %) falls when a charger interrupts
// itself more and more: derating on temperature, protection trips, connector
// faults. The assessment is an ordinary least-squares slope of duty factor
// against session order, with R² saying how much of the variation the trend
// explains. degrading is true only when the slope is negative beyond
// -0.5 %/session AND R² >= 0.5 AND at least 10 sessions were used: a noisy
// flat series must not read as decay, and a steep drop explained by nothing
// (low R²) is an event, not a trend.
struct DegradationAssessment {
    std::string asset_id;
    double slope_pct_per_session{0.0};
    double intercept_pct{0.0};
    double r_squared{0.0};
    std::size_t sessions_used{0};
    bool degrading{false};
    std::string verdict;  // human-readable summary for the report line
};

// Multi-Bay Monitor forward declaration
class MultiBayMonitor;

// UNKNOWN: no frame yet. OFF/READY/CHARGING: both streams fresh.
// DISCONNECTED: a stream exceeded its own timeout. FAULT: latched.
// A charging session opens only on the transition into CHARGING and closes
// on the transition out. Energy is the trapezoid of accepted in-session
// samples, in microwatt-hours, with the sub-uWh remainder dropped at close
// so it is not billed to the next session.
class Monitor {
   public:
    explicit Monitor(Config config = {});

    std::vector<Event> ingest(const CanFrame& frame);
    std::vector<Event> tick(Clock::time_point now = Clock::now());
    Snapshot snapshot(Clock::time_point now = Clock::now()) const;

    // Transport failures are counted and reported. They do not change the
    // charge state and they do not invent a bus-off recovery.
    Event note_transport_error(std::string code);

    bool reset_fault(std::string& reason, Clock::time_point now = Clock::now());

    // Planned stop of the monitor. A session that is open at that moment is
    // closed with stop_reason MONITOR_SHUTDOWN and quality INTERRUPTED, keeping
    // the energy accepted so far EXACTLY (no energy is invented for the time the
    // monitor was not looking). The record is energy_lower_bound and is NEVER
    // billing_complete: it is held for review, and the charge may continue as
    // the next session after the restart. time_upper_bound stays false (that
    // flag is for communication timeouts). Without this call a restart leaves an
    // orphan whose energy is unknown. With no session open it returns no events
    // and changes nothing; calling it twice is harmless. A crash or a power loss
    // cannot run it: the open line then has no close and the session is an
    // orphan, as before.
    std::vector<Event> shutdown(Clock::time_point now = Clock::now());

    const Config& config() const noexcept { return config_; }

   private:
    struct Sample {
        std::int64_t voltage_mv{0};
        std::int64_t current_ma{0};
        std::int64_t power_uw{0};
        Clock::time_point at{};
    };

    Config config_;
    std::int64_t min_voltage_mv_{0};
    std::int64_t max_voltage_mv_{0};
    std::int64_t min_current_ma_{0};
    std::int64_t max_current_ma_{0};
    Snapshot data_;
    std::optional<Clock::time_point> last_measurement_at_;
    std::optional<Clock::time_point> last_status_at_;
    std::optional<std::int8_t> last_status_value_;
    std::optional<Sample> last_sample_;
    std::optional<Sample> last_integrated_;
    bool fault_latched_{false};
    bool measurement_timeout_reported_{false};
    bool status_timeout_reported_{false};
    bool session_open_{false};
    SessionRecord open_{};
    std::optional<SessionRecord> last_closed_;
    Clock::time_point session_started_at_{};
    std::uint64_t energy_rem_{0};
    std::uint64_t next_session_id_{1};
    AssetKpi kpi_{};
    std::optional<Clock::time_point> dwell_from_;
    ChargeState dwell_state_{ChargeState::Unknown};

    // Diagnostic bookkeeping: last accepted sample and the moment the value
    // last changed, used to detect a frozen measurement channel.
    std::optional<Sample> diag_last_;
    Clock::time_point diag_last_change_{};
    Diagnostics diag_{};
    std::optional<Clock::time_point> diag_stuck_since_{};
    bool diag_stuck_reported_{false};
    std::uint32_t diag_hunt_runs_{0};
    std::int64_t diag_hunt_ref_ma_{0};
    std::int64_t diag_hunt_extreme_ma_{0};

    std::vector<Event> enter_fault(std::string code, std::string message, Clock::time_point now);
    std::vector<Event> ingest_measurement(const CanFrame& frame);
    std::vector<Event> ingest_power_status(const CanFrame& frame);
    void recompute_state(Clock::time_point now, std::vector<Event>& events);
    void set_operational_state(ChargeState next, Clock::time_point now, const std::string& leave_reason,
                               std::vector<Event>& events);
    void account_until(Clock::time_point now);
    Event open_session(Clock::time_point now);
    Event close_session(Clock::time_point now, const std::string& reason);
    void integrate(const Sample& sample, std::vector<Event>& events);
    void diagnose(const Sample& sample, std::vector<Event>& events);
    void price(SessionRecord& rec) const;
    bool fresh(const std::optional<Clock::time_point>& at, Clock::time_point now,
               std::chrono::milliseconds timeout) const;
    AssetKpi kpi_at(Clock::time_point now) const;
    SessionRecord priced_open(Clock::time_point now) const;
};

std::string to_string(ChargeState state);
std::string to_string(EventLevel level);
const char* recommended_action(const std::string& code) noexcept;
std::string json_snapshot(const Snapshot& snapshot);

// A self-contained evidence block for the tariff that priced a session, in
// exact integer micro-EUR. Embedded in the session JSON and in the journal
// close line so a dispute can be re-verified years later, even when the
// running configuration has changed in between.
std::string json_tariff_block(const Tariff& tariff);

std::string json_session_line(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                              const std::string& closed_at_utc, const std::string& tariff_json = {});
std::string utc_timestamp();

// "1.25" with scale 1_000_000 -> 1250000. Scale must be a power of ten.
// Extra fractional digits are rejected (no silent truncation).
std::optional<std::int64_t> parse_fixed_point(const std::string& text, std::int64_t scale);
std::string format_fixed_point(std::int64_t value, std::int64_t scale);

std::optional<CanFrame> parse_hex_frame(const std::string& line, Clock::time_point now = Clock::now());

// Chained Anchor implementation
std::string json_chained_anchor(const ChainedAnchor& anchor);
std::optional<ChainedAnchor> parse_chained_anchor(const std::string& json);
// Full anchor verification. Four checks, in order:
//   1. chain: the anchor points at the anchor that preceded it
//      (empty prev_anchor_sha256 expected for the genesis anchor);
//   2. journal binding: the journal hash the anchor claims matches the
//      journal as read now — a rewritten journal fails here, which is the
//      tamper signal the anchor exists to raise;
//   3. self-consistency: schema and timestamp are present and valid;
//   4. signature: when hmac_secret is non-empty, the HMAC must match
//      sign_hmac(). Leave it empty for anchors signed by an external TSA
//      (RFC3161): the token is then verified outside this library.
bool verify_chained_anchor(const ChainedAnchor& current, const std::string& prev_anchor_sha256,
                           const std::string& journal_sha256_expected, const std::string& hmac_secret = "");
std::string compute_anchor_sha256(const ChainedAnchor& anchor);

// Webhook implementation
std::string json_webhook_event(const WebhookEvent& event);
bool send_webhook(const std::string& url, const WebhookEvent& event, const std::string& secret, std::string& error);

// Advanced Analytics
SessionTrend compute_session_trend(const std::vector<SessionRecord>& sessions, const std::string& asset_id);
FleetSummary compute_fleet_summary(const std::vector<Snapshot>& bay_snapshots);
std::vector<AnomalyResult> detect_anomalies(const std::vector<SessionRecord>& sessions, const std::string& asset_id);

// Cross-bay drift: per-asset closed-billable history in, anomalies out. For
// every asset with at least MIN_DRIFT_SESSIONS closed billable sessions the
// mean of its most recent DRIFT_RECENT_SESSIONS is compared against the
// fleet median of those means. When at least MIN_DRIFT_ASSETS such assets
// exist and an asset's mean deviates from the fleet median by more than
// DRIFT_THRESHOLD, a "fleet_drift" anomaly is reported: one bay delivering
// systematically less (or more) than its twins is a maintenance signal that
// per-asset baselines cannot see, because each bay looks normal against
// itself. Constants are declared so the gates are visible in the header and
// stable in tests.
inline constexpr std::size_t DRIFT_RECENT_SESSIONS = 5;
inline constexpr std::size_t MIN_DRIFT_SESSIONS = 3;
inline constexpr std::size_t MIN_DRIFT_ASSETS = 3;
inline constexpr double DRIFT_THRESHOLD = 0.35;  // 35 % off the fleet median
std::vector<AnomalyResult> detect_fleet_drift(const std::map<std::string, std::vector<SessionRecord>>& per_asset);

DegradationAssessment assess_degradation(const std::vector<SessionRecord>& sessions, const std::string& asset_id);
std::string json_fleet_summary(const FleetSummary& summary);
std::string json_session_trend(const SessionTrend& trend);
std::string json_anomalies(const std::vector<AnomalyResult>& anomalies);
std::string json_degradation(const DegradationAssessment& assessment);

// OCPI CDR Export
struct OcpiCdrRecord {
    std::string cdr_id;
    std::string start_utc;
    std::string stop_utc;
    std::uint64_t meter_start_wh;
    std::uint64_t meter_stop_wh;
    std::string auth_uid;
    std::string auth_type;  // "RFID", "APP", "PLUG_AND_CHARGE", etc.
    std::string currency;
    std::int64_t total_cost_micro_eur;
    std::int64_t energy_cost_micro_eur;
    std::int64_t time_cost_micro_eur;
    std::int64_t flat_cost_micro_eur;
    std::string tariff_json;
    std::string ocmf_signed_data;
};
std::string json_ocpi_cdr(const OcpiCdrRecord& cdr);
std::vector<OcpiCdrRecord> generate_ocpi_cdrs(const std::vector<SessionRecord>& sessions, const Config& config);

// Multi-Bay Monitor
class MultiBayMonitor {
   public:
    explicit MultiBayMonitor(MultiBayConfig config = MultiBayConfig{});
    ~MultiBayMonitor();

    // Ingest a CAN frame for a specific bay
    std::vector<Event> ingest(const std::string& asset_id, const CanFrame& frame);

    // Tick all bays
    std::vector<Event> tick(Clock::time_point now = Clock::now());

    // Get aggregate fleet snapshot
    FleetSummary fleet_snapshot(Clock::time_point now = Clock::now()) const;

    // Get individual bay snapshot
    std::optional<Snapshot> bay_snapshot(const std::string& asset_id, Clock::time_point now = Clock::now()) const;

    // Get all bay snapshots
    std::vector<Snapshot> all_bay_snapshots(Clock::time_point now = Clock::now()) const;

    // Hot-reload configuration for a specific bay
    bool reload_bay_config(const std::string& asset_id, const Config& new_config, std::string& error);

    // Enable/disable a bay
    void set_bay_enabled(const std::string& asset_id, bool enabled);

    // Feed the analytics history with a session the caller has closed and
    // written to its journal. The monitor keeps a bounded copy (last 256 per
    // bay) for bay_trend()/bay_anomalies(). Nothing is invented: without
    // recorded sessions those analytics return empty, not fabricated data.
    void record_closed_session(const std::string& asset_id, const SessionRecord& record);

    // Get list of bay asset IDs
    std::vector<std::string> bay_asset_ids() const;

    // Get config for a bay
    std::optional<Config> bay_config(const std::string& asset_id) const;

    // Shutdown all bays gracefully
    std::vector<Event> shutdown(Clock::time_point now = Clock::now());

    // Analytics
    SessionTrend bay_trend(const std::string& asset_id) const;
    std::vector<AnomalyResult> bay_anomalies(const std::string& asset_id) const;

    // Webhook streaming
    void set_webhook(const std::string& url, const std::string& secret);

    const MultiBayConfig& config() const noexcept { return config_; }

   private:
    struct BayInstance {
        Config config;
        std::unique_ptr<Monitor> monitor;
        bool enabled{true};
        int priority{0};
        std::vector<SessionRecord> closed_history;
    };

    MultiBayConfig config_;
    std::unordered_map<std::string, BayInstance> bays_;
    std::string webhook_url_;
    std::string webhook_secret_;
    mutable std::mutex mutex_;

    void maybe_send_webhook(const std::string& asset_id, const Event& event);
    void emit_fleet_webhook(const FleetSummary& summary, Clock::time_point now);
};

}  // namespace charger
