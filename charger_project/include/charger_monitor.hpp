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
#include <optional>
#include <string>
#include <vector>

namespace charger {

constexpr const char* VERSION = "1.3.0";
constexpr const char* PROJECT_NAME = "charger_monitor";
constexpr const char* SNAPSHOT_SCHEMA = "charger.snapshot.v1";
constexpr const char* SESSION_SCHEMA = "charger.session.v1";

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
std::string json_session_line(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                              const std::string& closed_at_utc);
std::string utc_timestamp();

// "1.25" with scale 1_000_000 -> 1250000. Scale must be a power of ten.
// Extra fractional digits are rejected (no silent truncation).
std::optional<std::int64_t> parse_fixed_point(const std::string& text, std::int64_t scale);
std::string format_fixed_point(std::int64_t value, std::int64_t scale);

std::optional<CanFrame> parse_hex_frame(const std::string& line, Clock::time_point now = Clock::now());

}  // namespace charger
