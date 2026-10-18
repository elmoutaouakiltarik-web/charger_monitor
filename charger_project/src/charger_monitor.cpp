#include "charger_monitor.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace charger {
namespace {

constexpr std::uint64_t kUwhDivisor = 7200000000ull;
constexpr std::uint64_t kUwhPerWh = 1000000ull;
constexpr std::int64_t kMicroPerEuro = 1000000;
constexpr std::int64_t kPpm = 1000000;

bool finite_in_range(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

std::int64_t to_milli(double si) {
    if (!std::isfinite(si)) throw std::invalid_argument("Limit is not finite");
    const double scaled = si * 1000.0;
    if (scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
        scaled < static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        throw std::invalid_argument("Limit outside the representable integer range");
    }
    return std::llround(scaled);
}

std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o << "\\\"";
                break;
            case '\\':
                o << "\\\\";
                break;
            case '\b':
                o << "\\b";
                break;
            case '\f':
                o << "\\f";
                break;
            case '\n':
                o << "\\n";
                break;
            case '\r':
                o << "\\r";
                break;
            case '\t':
                o << "\\t";
                break;
            default:
                if (c < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    o << static_cast<char>(c);
                }
        }
    }
    return o.str();
}

const char* json_bool(bool v) { return v ? "true" : "false"; }

void append_si_2dp(std::ostringstream& o, std::int64_t milli) {
    const bool neg = milli < 0;
    std::int64_t abs_mv = neg ? -milli : milli;
    const std::int64_t centi = (abs_mv + 5) / 10;
    if (neg && centi != 0) o << '-';
    o << (centi / 100) << '.' << std::setw(2) << std::setfill('0') << (centi % 100);
}

std::uint64_t ceil_step_u64(std::uint64_t consumed, std::int32_t step) {
    if (consumed == 0 || step <= 1) return consumed;
    const auto s = static_cast<std::uint64_t>(step);
    const auto rem = consumed % s;
    if (rem == 0) return consumed;
    const auto headroom = std::numeric_limits<std::uint64_t>::max() - consumed;
    // Saturate rather than wrap: a rounded billable register must never become
    // a small number, which would silently under-invoice.
    if (headroom < s - rem) return std::numeric_limits<std::uint64_t>::max();
    return consumed + (s - rem);
}

std::int64_t ceil_step_i64(std::int64_t consumed, std::int32_t step) {
    if (consumed <= 0 || step <= 1) return consumed < 0 ? 0 : consumed;
    const auto s = static_cast<std::int64_t>(step);
    const auto rem = consumed % s;
    if (rem == 0) return consumed;
    const auto headroom = std::numeric_limits<std::int64_t>::max() - consumed;
    if (headroom < s - rem) return std::numeric_limits<std::int64_t>::max();
    return consumed + (s - rem);
}

std::optional<std::int64_t> mul_div_round(std::int64_t a, std::int64_t b, std::int64_t d) {
    if (d <= 0 || a < 0 || b < 0) return std::nullopt;
    const WideInt num = static_cast<WideInt>(a) * static_cast<WideInt>(b);
    const WideInt div = d;
    const WideInt rounded = (num + div / 2) / div;
    if (rounded > std::numeric_limits<std::int64_t>::max()) return std::nullopt;
    return static_cast<std::int64_t>(rounded);
}

bool currency_ok(const std::string& c) {
    if (c.size() != 3) return false;
    for (unsigned char ch : c) {
        if (ch < 'A' || ch > 'Z') return false;
    }
    return true;
}

bool asset_ok(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char ch : id) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                        ch == '_' || ch == ':' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

bool scale_ok(const Scale& s) {
    return s.numerator > 0 && s.denominator > 0 && s.numerator <= 1000000 && s.denominator <= 1000000000;
}

int scale_digits(std::int64_t scale) {
    if (scale <= 0) return -1;
    int digits = 0;
    for (std::int64_t s = scale; s > 1; s /= 10) {
        if (s % 10 != 0) return -1;
        ++digits;
    }
    return digits;
}

std::optional<std::int64_t> decode_milli(const CanFrame& frame, std::uint8_t offset, bool little_endian,
                                         const Scale& scale) {
    if (static_cast<std::size_t>(offset) + 1 >= frame.length) return std::nullopt;
    const std::uint16_t raw =
        little_endian
            ? static_cast<std::uint16_t>(frame.data[offset] | (static_cast<std::uint16_t>(frame.data[offset + 1]) << 8))
            : static_cast<std::uint16_t>(frame.data[offset + 1] |
                                         (static_cast<std::uint16_t>(frame.data[offset]) << 8));
    const WideInt num = static_cast<WideInt>(raw) * scale.numerator * 1000;
    return static_cast<std::int64_t>(num / scale.denominator);
}

void add_dwell(ChargeState state, std::int64_t ms, AssetKpi& kpi) {
    if (ms <= 0) return;
    switch (state) {
        case ChargeState::Unknown:
            kpi.unknown_ms += ms;
            break;
        case ChargeState::Off:
            kpi.off_ms += ms;
            break;
        case ChargeState::Ready:
            kpi.ready_ms += ms;
            break;
        case ChargeState::Charging:
            kpi.charging_ms += ms;
            break;
        case ChargeState::Fault:
            kpi.fault_ms += ms;
            break;
        case ChargeState::Disconnected:
            kpi.disconnected_ms += ms;
            break;
    }
}

std::string leave_reason(ChargeState next, const std::string& fault_code) {
    switch (next) {
        case ChargeState::Off:
            return "STATUS_OFF";
        case ChargeState::Ready:
            return "STATUS_READY";
        case ChargeState::Disconnected:
            return "COMMUNICATION_TIMEOUT";
        case ChargeState::Unknown:
            return "STATUS_UNKNOWN";
        case ChargeState::Fault:
            return fault_code.empty() ? "FAULT" : fault_code;
        case ChargeState::Charging:
            return "";
    }
    return "STATUS_UNKNOWN";
}

void append_record(std::ostringstream& o, const SessionRecord& s) {
    o << "{\"id\":" << s.id << ",\"closed\":" << json_bool(s.closed) << ",\"duration_ms\":" << s.duration_ms
      << ",\"energy_uwh\":\"" << s.energy_uwh << "\",\"energy_wh\":\"" << s.energy_wh << "\",\"billed_energy_wh\":\""
      << s.billed_energy_wh << "\",\"charging_s\":" << s.charging_s << ",\"billed_time_s\":" << s.billed_time_s
      << ",\"peak_power_mw\":" << s.peak_power_mw << ",\"min_voltage_mv\":" << s.min_voltage_mv
      << ",\"max_current_ma\":" << s.max_current_ma << ",\"samples\":" << s.samples
      << ",\"integration_gaps\":" << s.integration_gaps << ",\"undersampled\":" << json_bool(s.undersampled)
      << ",\"billing_complete\":" << json_bool(s.billing_complete)
      << ",\"energy_lower_bound\":" << json_bool(s.energy_lower_bound)
      << ",\"time_upper_bound\":" << json_bool(s.time_upper_bound)
      << ",\"price_clamped_min\":" << json_bool(s.price_clamped_min)
      << ",\"price_clamped_max\":" << json_bool(s.price_clamped_max) << ",\"quality\":\"" << json_escape(s.quality)
      << "\",\"stop_reason\":\"" << json_escape(s.stop_reason) << "\",\"energy_cost_micro_eur\":\""
      << s.energy_cost_micro_eur << "\",\"time_cost_micro_eur\":\"" << s.time_cost_micro_eur
      << "\",\"flat_cost_micro_eur\":\"" << s.flat_cost_micro_eur << "\",\"total_cost_micro_eur\":\""
      << s.total_cost_micro_eur << "\",\"total_cost_excl_vat\":\""
      << format_fixed_point(s.total_cost_micro_eur, kMicroPerEuro) << "\",\"vat\":\"omitted\"}";
}

}  // namespace

ProtocolProfile::ProtocolProfile() {
    status_map.fill(kStatusInvalid);
    status_map[0] = kStatusOff;
    status_map[1] = kStatusReady;
    status_map[2] = kStatusCharging;
    status_map[3] = kStatusFault;
}

ProtocolProfile ProtocolProfile::bare() {
    ProtocolProfile p;
    p.status_map.fill(kStatusInvalid);
    p.measurement_id = 0;
    p.status_id = 1;
    return p;
}

Monitor::Monitor(Config config) : config_(std::move(config)) {
    if (!finite_in_range(config_.min_voltage_v, -1.0e6, 1.0e6) ||
        !finite_in_range(config_.max_voltage_v, -1.0e6, 1.0e6) ||
        !finite_in_range(config_.min_current_a, -1.0e6, 1.0e6) ||
        !finite_in_range(config_.max_current_a, -1.0e6, 1.0e6) || config_.min_voltage_v > config_.max_voltage_v ||
        config_.min_current_a > config_.max_current_a) {
        throw std::invalid_argument("Invalid configuration: voltage or current range");
    }
    if (config_.measurement_timeout.count() <= 0 || config_.status_timeout.count() <= 0 ||
        config_.stale_timeout.count() <= 0 || config_.report_interval.count() <= 0 ||
        config_.max_integration_step.count() <= 0 || config_.max_integration_step > std::chrono::hours(1) ||
        config_.stuck_at_window < std::chrono::milliseconds(0) || config_.max_voltage_rate_mv_per_s < 0 ||
        config_.max_current_rate_ma_per_s < 0 || config_.sag_voltage_mv < 0 || config_.sag_tolerance_mv < 0 ||
        config_.current_hunt_band_ma < 0) {
        throw std::invalid_argument("Invalid configuration: timeout or integration step");
    }
    if (!scale_ok(config_.protocol.voltage) || !scale_ok(config_.protocol.current)) {
        throw std::invalid_argument("Invalid configuration: protocol scale");
    }
    const auto& p = config_.protocol;
    if (p.measurement_id == p.status_id || p.measurement_id > 0x1fffffffu || p.status_id > 0x1fffffffu ||
        p.measurement_length < 2 || p.measurement_length > 8 ||
        static_cast<unsigned>(p.voltage_offset) + 1 >= p.measurement_length ||
        static_cast<unsigned>(p.current_offset) + 1 >= p.measurement_length) {
        throw std::invalid_argument("Invalid configuration: protocol frame layout");
    }
    const unsigned v0 = p.voltage_offset;
    const unsigned i0 = p.current_offset;
    if (!(v0 + 1 < i0 || i0 + 1 < v0)) {
        throw std::invalid_argument("Invalid configuration: voltage and current overlap");
    }
    if (!currency_ok(config_.currency) || !asset_ok(config_.asset_id)) {
        throw std::invalid_argument("Invalid configuration: ISO-4217 currency or asset_id");
    }
    const auto& t = config_.tariff;
    if (t.energy_micro_eur_per_kwh < 0 || t.time_micro_eur_per_hour < 0 || t.flat_micro_eur < 0 ||
        t.energy_step_wh < 1 || t.time_step_s < 1 || (t.min_price_micro_eur && *t.min_price_micro_eur < 0) ||
        (t.max_price_micro_eur && *t.max_price_micro_eur < 0) ||
        (t.min_price_micro_eur && t.max_price_micro_eur && *t.min_price_micro_eur > *t.max_price_micro_eur)) {
        throw std::invalid_argument("Invalid configuration: tariff");
    }
    min_voltage_mv_ = to_milli(config_.min_voltage_v);
    max_voltage_mv_ = to_milli(config_.max_voltage_v);
    min_current_ma_ = to_milli(config_.min_current_a);
    max_current_ma_ = to_milli(config_.max_current_a);
    if (max_voltage_mv_ > 0 && max_current_ma_ > std::numeric_limits<std::int64_t>::max() / max_voltage_mv_) {
        throw std::invalid_argument("Invalid configuration: maximum power not representable");
    }
    data_.currency = config_.currency;
    data_.asset_id = config_.asset_id;
}

bool Monitor::fresh(const std::optional<Clock::time_point>& at, Clock::time_point now,
                    std::chrono::milliseconds timeout) const {
    if (!at || now < *at) return false;
    return (now - *at) <= timeout;
}

void Monitor::account_until(Clock::time_point now) {
    if (!dwell_from_) {
        dwell_from_ = now;
        dwell_state_ = data_.state;
        return;
    }
    if (now < *dwell_from_) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *dwell_from_).count();
    add_dwell(dwell_state_, ms, kpi_);
    dwell_from_ = now;
}

void Monitor::price(SessionRecord& rec) const {
    rec.energy_wh = rec.energy_uwh / kUwhPerWh;
    rec.charging_s = rec.duration_ms / 1000;
    rec.billed_energy_wh = ceil_step_u64(rec.energy_wh, config_.tariff.energy_step_wh);
    rec.billed_time_s = ceil_step_i64(rec.charging_s, config_.tariff.time_step_s);
    rec.undersampled = rec.samples < 2;
    if (rec.samples < 2) {
        rec.quality = "NO_INTERVAL";
    } else if (rec.integration_gaps > 0) {
        rec.quality = "GAPS";
    } else if (rec.energy_uwh == 0) {
        rec.quality = "ZERO_ENERGY";
    } else if (rec.stop_reason == STOP_MONITOR_SHUTDOWN) {
        // The monitor was stopped, not the power stage: what it counted is exact,
        // what happened afterwards is unknown. Never invoiced automatically.
        rec.quality = "INTERRUPTED";
    } else {
        rec.quality = "OK";
    }
    rec.time_upper_bound = rec.stop_reason == "COMMUNICATION_TIMEOUT";
    rec.energy_lower_bound =
        rec.undersampled || rec.integration_gaps > 0 || rec.time_upper_bound || rec.quality == "INTERRUPTED";
    rec.billing_complete = rec.closed && rec.quality == "OK" && !rec.time_upper_bound;

    // The overflow guard must precede the narrowing cast: converting a uint64_t
    // above INT64_MAX to int64_t is implementation-defined and can wrap to a
    // positive value, which would then slip past mul_div_round's negative check.
    const auto energy_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (rec.billed_energy_wh > energy_limit) {
        rec.energy_cost_micro_eur = 0;
        rec.time_cost_micro_eur = 0;
        rec.flat_cost_micro_eur = 0;
        rec.total_cost_micro_eur = 0;
        rec.billing_complete = false;
        rec.quality = "PRICE_OVERFLOW";
        return;
    }
    const auto energy =
        mul_div_round(static_cast<std::int64_t>(rec.billed_energy_wh), config_.tariff.energy_micro_eur_per_kwh, 1000);
    const auto time = mul_div_round(rec.billed_time_s, config_.tariff.time_micro_eur_per_hour, 3600);
    if (!energy || !time) {
        rec.energy_cost_micro_eur = 0;
        rec.time_cost_micro_eur = 0;
        rec.flat_cost_micro_eur = 0;
        rec.total_cost_micro_eur = 0;
        rec.billing_complete = false;
        rec.quality = "PRICE_OVERFLOW";
        return;
    }
    rec.energy_cost_micro_eur = *energy;
    rec.time_cost_micro_eur = *time;
    rec.flat_cost_micro_eur = config_.tariff.flat_micro_eur;
    WideInt total = static_cast<WideInt>(rec.energy_cost_micro_eur) + rec.time_cost_micro_eur + rec.flat_cost_micro_eur;
    if (total > std::numeric_limits<std::int64_t>::max()) {
        rec.total_cost_micro_eur = 0;
        rec.billing_complete = false;
        rec.quality = "PRICE_OVERFLOW";
        return;
    }
    rec.total_cost_micro_eur = static_cast<std::int64_t>(total);
    rec.price_clamped_min = false;
    rec.price_clamped_max = false;
    if (config_.tariff.min_price_micro_eur && rec.total_cost_micro_eur < *config_.tariff.min_price_micro_eur) {
        rec.total_cost_micro_eur = *config_.tariff.min_price_micro_eur;
        rec.price_clamped_min = true;
    }
    if (config_.tariff.max_price_micro_eur && rec.total_cost_micro_eur > *config_.tariff.max_price_micro_eur) {
        rec.total_cost_micro_eur = *config_.tariff.max_price_micro_eur;
        rec.price_clamped_max = true;
    }
}

Event Monitor::open_session(Clock::time_point now) {
    session_open_ = true;
    open_ = SessionRecord{};
    open_.id = next_session_id_++;
    session_started_at_ = now;
    energy_rem_ = 0;
    last_integrated_.reset();
    if (last_sample_) {
        Sample seed = *last_sample_;
        seed.at = now;
        last_integrated_ = seed;
        open_.samples = 1;
        open_.peak_power_mw = seed.power_uw / 1000;
        open_.min_voltage_mv = seed.voltage_mv;
        open_.max_current_ma = seed.current_ma;
    }
    ++kpi_.sessions_opened;
    return {EventLevel::Info, "SESSION_OPENED", "Session " + std::to_string(open_.id) + " opened"};
}

Event Monitor::close_session(Clock::time_point now, const std::string& reason) {
    open_.closed = true;
    open_.stop_reason = reason;
    if (now >= session_started_at_) {
        open_.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - session_started_at_).count();
    }
    price(open_);
    if (kpi_.energy_closed_uwh <= std::numeric_limits<std::uint64_t>::max() - open_.energy_uwh) {
        kpi_.energy_closed_uwh += open_.energy_uwh;
    }
    ++kpi_.sessions_closed;
    kpi_.integration_gaps += open_.integration_gaps;
    if (open_.undersampled) ++kpi_.sessions_undersampled;
    if (open_.billing_complete) ++kpi_.sessions_billable;
    last_closed_ = open_;
    session_open_ = false;
    last_integrated_.reset();
    energy_rem_ = 0;
    std::ostringstream msg;
    msg << "Session " << open_.id << " closed: " << open_.energy_wh << " Wh, "
        << format_fixed_point(open_.total_cost_micro_eur, kMicroPerEuro) << ' ' << config_.currency
        << " excluding VAT, reason " << reason << ", quality " << open_.quality;
    return {EventLevel::Info, "SESSION_CLOSED", msg.str()};
}

std::vector<Event> Monitor::shutdown(Clock::time_point now) {
    std::vector<Event> events;
    if (!session_open_) return events;
    account_until(now);
    events.push_back(close_session(now, STOP_MONITOR_SHUTDOWN));
    return events;
}

void Monitor::integrate(const Sample& sample, std::vector<Event>& events) {
    if (!last_integrated_) {
        last_integrated_ = sample;
        if (open_.samples == 0) {
            open_.samples = 1;
            open_.min_voltage_mv = sample.voltage_mv;
            open_.max_current_ma = sample.current_ma;
        }
        return;
    }
    if (sample.at < last_integrated_->at) return;
    const auto dt_us = std::chrono::duration_cast<std::chrono::microseconds>(sample.at - last_integrated_->at).count();
    const auto max_us = std::chrono::duration_cast<std::chrono::microseconds>(config_.max_integration_step).count();
    if (dt_us > 0 && dt_us <= max_us && last_integrated_->power_uw >= 0 && sample.power_uw >= 0) {
        const WideInt num = (static_cast<WideInt>(last_integrated_->power_uw) + sample.power_uw) * dt_us +
                            static_cast<WideInt>(energy_rem_);
        const WideInt add = num / static_cast<WideInt>(kUwhDivisor);
        energy_rem_ = static_cast<std::uint64_t>(num % static_cast<WideInt>(kUwhDivisor));
        if (add > static_cast<WideInt>(std::numeric_limits<std::uint64_t>::max() - open_.energy_uwh)) {
            events.push_back(
                {EventLevel::Error, "ENERGY_COUNTER_SATURATED", "Energy counter saturated; integration stopped"});
            last_integrated_ = sample;
            return;
        }
        open_.energy_uwh += static_cast<std::uint64_t>(add);
    } else if (dt_us > max_us) {
        ++open_.integration_gaps;
        events.push_back({EventLevel::Warning, "MEASUREMENT_GAP",
                          "Measurement interval beyond the maximum step: energy not interpolated"});
    }
    const auto power_mw = sample.power_uw / 1000;
    if (power_mw > open_.peak_power_mw) open_.peak_power_mw = power_mw;
    if (open_.samples == 0 || sample.voltage_mv < open_.min_voltage_mv) open_.min_voltage_mv = sample.voltage_mv;
    if (sample.current_ma > open_.max_current_ma) open_.max_current_ma = sample.current_ma;
    ++open_.samples;
    last_integrated_ = sample;
}

void Monitor::diagnose(const Sample& sample, std::vector<Event>& events) {
    const auto now = sample.at;
    diag_.voltage_rate_exceeded = false;
    diag_.current_rate_exceeded = false;
    diag_.voltage_sag = false;
    diag_.current_hunting = false;

    const bool charging = !fault_latched_ && data_.state == ChargeState::Charging;
    const std::int64_t dt_ms =
        diag_last_ ? std::chrono::duration_cast<std::chrono::milliseconds>(now - diag_last_->at).count() : 0;

    if (diag_last_ && dt_ms > 0) {
        // Rate of change against configured slopes; 0 disables the check.
        if (config_.max_voltage_rate_mv_per_s > 0) {
            const std::int64_t dv = std::llabs(sample.voltage_mv - diag_last_->voltage_mv);
            const WideInt allowed = static_cast<WideInt>(config_.max_voltage_rate_mv_per_s) * dt_ms / 1000;
            if (static_cast<WideInt>(dv) > allowed) {
                diag_.voltage_rate_exceeded = true;
                ++kpi_.rate_of_change_warnings;
                events.push_back(
                    {EventLevel::Warning, "VOLTAGE_RATE_EXCEEDED", "Voltage step beyond the configured slope"});
            }
        }
        if (config_.max_current_rate_ma_per_s > 0) {
            const std::int64_t di = std::llabs(sample.current_ma - diag_last_->current_ma);
            const WideInt allowed = static_cast<WideInt>(config_.max_current_rate_ma_per_s) * dt_ms / 1000;
            if (static_cast<WideInt>(di) > allowed) {
                diag_.current_rate_exceeded = true;
                ++kpi_.rate_of_change_warnings;
                events.push_back(
                    {EventLevel::Warning, "CURRENT_RATE_EXCEEDED", "Current step beyond the configured slope"});
            }
        }
    }

    // Voltage sag under load: a level condition, valid from the first sample.
    // A connector or contactor losing headroom shows up here before it faults.
    if (charging && config_.sag_voltage_mv > 0 && sample.voltage_mv < config_.sag_voltage_mv && sample.current_ma > 0) {
        if (config_.sag_voltage_mv - sample.voltage_mv > config_.sag_tolerance_mv) {
            diag_.voltage_sag = true;
            ++kpi_.sag_warnings;
            events.push_back({EventLevel::Warning, "VOLTAGE_SAG_UNDER_LOAD",
                              "Voltage collapse under load: check connector and contactor"});
        }
    }

    // Current hunting: repeated reversals inside a narrow band point to a loose
    // connection or an arcing contact.
    if (charging && config_.current_hunt_band_ma > 0 && dt_ms > 0) {
        if (std::llabs(sample.current_ma - diag_hunt_ref_ma_) > config_.current_hunt_band_ma) {
            diag_hunt_extreme_ma_ = sample.current_ma;
            ++diag_hunt_runs_;
        }
        diag_hunt_ref_ma_ = sample.current_ma;
        if (diag_hunt_runs_ >= 6 && diag_hunt_extreme_ma_ != 0) {
            diag_.current_hunting = true;
            ++kpi_.current_hunt_warnings;
            diag_hunt_runs_ = 0;
            events.push_back(
                {EventLevel::Warning, "CURRENT_HUNTING", "Current oscillation under load: suspect unstable contact"});
        }
    }

    // Stuck-at: a frozen channel is only suspicious when it contradicts what a
    // real charge looks like. Constant load at a plausible voltage and a real
    // current is normal operation, not a fault, so requiring both avoids
    // crying wolf on every steady charging profile.
    const bool plausible_load = sample.current_ma > 0 && sample.voltage_mv > 0;
    const bool unchanged =
        diag_last_ && sample.voltage_mv == diag_last_->voltage_mv && sample.current_ma == diag_last_->current_ma;
    if (unchanged && plausible_load) {
        if (!diag_stuck_since_) diag_stuck_since_ = now;
    } else {
        diag_stuck_since_.reset();
        diag_stuck_reported_ = false;
    }
    if (config_.stuck_at_window.count() > 0 && diag_stuck_since_ && !diag_stuck_reported_ && charging) {
        if ((now - *diag_stuck_since_) >= config_.stuck_at_window) {
            diag_.measurement_stuck = true;
            diag_stuck_reported_ = true;
            ++kpi_.stuck_at_warnings;
            events.push_back({EventLevel::Warning, "MEASUREMENT_STUCK",
                              "Measurements frozen at a plausible load: suspect a stuck sensor or cable"});
        }
    }
    diag_last_ = sample;
}

void Monitor::set_operational_state(ChargeState next, Clock::time_point now, const std::string& leave_reason_text,
                                    std::vector<Event>& events) {
    account_until(now);
    if (fault_latched_) next = ChargeState::Fault;
    if (next != ChargeState::Charging) {
        // A diagnostic is meaningful only while charging. Leaving the state must
        // clear it, otherwise the HMI shows a stale condition.
        diag_ = Diagnostics{};
        diag_stuck_since_.reset();
        diag_stuck_reported_ = false;
        diag_hunt_runs_ = 0;
        diag_hunt_ref_ma_ = 0;
        diag_hunt_extreme_ma_ = 0;
    }
    if (session_open_ && data_.state == ChargeState::Charging && next != ChargeState::Charging) {
        events.push_back(close_session(now, leave_reason_text));
    }
    if (!fault_latched_ && !session_open_ && data_.state != ChargeState::Charging && next == ChargeState::Charging) {
        events.push_back(open_session(now));
    }
    data_.state = next;
    dwell_state_ = next;
}

std::vector<Event> Monitor::enter_fault(std::string code, std::string message, Clock::time_point now) {
    const bool repeat = fault_latched_ && data_.fault_code == code;
    fault_latched_ = true;
    data_.fault_code = std::move(code);
    data_.fault_message = std::move(message);
    std::vector<Event> ev;
    if (!repeat) {
        ++kpi_.fault_events;
        ev.push_back({EventLevel::Error, data_.fault_code, data_.fault_message});
    }
    set_operational_state(ChargeState::Fault, now, data_.fault_code, ev);
    return ev;
}

std::vector<Event> Monitor::ingest(const CanFrame& frame) {
    if (frame.length > 8) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "CAN_DLC_INVALID", "CAN frame longer than 8 bytes"}};
    }
    if (frame.id == config_.protocol.measurement_id) return ingest_measurement(frame);
    if (frame.id == config_.protocol.status_id) return ingest_power_status(frame);
    ++data_.rejected_frames;
    return {{EventLevel::Warning, "CAN_ID_UNKNOWN", "CAN identifier not defined by the protocol profile"}};
}

std::vector<Event> Monitor::ingest_measurement(const CanFrame& frame) {
    if (frame.length != config_.protocol.measurement_length) {
        ++data_.rejected_frames;
        return {
            {EventLevel::Warning, "MEASUREMENT_DLC_INVALID", "Measurement frame does not match the profile length"}};
    }
    if (config_.protocol.reserved_must_be_zero) {
        for (std::uint8_t i = 0; i < frame.length; ++i) {
            const bool voltage_byte = i == config_.protocol.voltage_offset || i == config_.protocol.voltage_offset + 1;
            const bool current_byte = i == config_.protocol.current_offset || i == config_.protocol.current_offset + 1;
            if (!voltage_byte && !current_byte && frame.data[i] != 0) {
                ++data_.rejected_frames;
                return {{EventLevel::Warning, "MEASUREMENT_RESERVED_NONZERO",
                         "Non-zero reserved bytes in the measurement frame"}};
            }
        }
    }
    if (last_measurement_at_ && frame.received_at < *last_measurement_at_) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "CLOCK_REGRESSION", "Non-monotonic measurement timestamp, frame rejected"}};
    }
    const auto mv =
        decode_milli(frame, config_.protocol.voltage_offset, config_.protocol.little_endian, config_.protocol.voltage);
    const auto ma =
        decode_milli(frame, config_.protocol.current_offset, config_.protocol.little_endian, config_.protocol.current);
    if (!mv || !ma) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "MEASUREMENT_DECODE_FAILED", "Measurement decoding failed"}};
    }
    if (*mv < min_voltage_mv_ || *mv > max_voltage_mv_ || *ma < min_current_ma_ || *ma > max_current_ma_) {
        ++data_.rejected_frames;
        return enter_fault("MEASUREMENT_OUT_OF_RANGE", "Voltage or current outside the configured range",
                           frame.received_at);
    }
    if (*mv > 0 && *ma > std::numeric_limits<std::int64_t>::max() / *mv) {
        ++data_.rejected_frames;
        return enter_fault("ENERGY_COUNTER_SATURATED", "Sample power not representable", frame.received_at);
    }
    Sample sample{*mv, *ma, (*mv) * (*ma), frame.received_at};
    data_.measurements = {static_cast<double>(*mv) / 1000.0, static_cast<double>(*ma) / 1000.0, frame.received_at,
                          true};
    last_measurement_at_ = frame.received_at;
    data_.measurements_stale = false;
    measurement_timeout_reported_ = false;
    ++data_.accepted_measurement_frames;
    last_sample_ = sample;
    std::vector<Event> ev{{EventLevel::Info, "MEASUREMENT_ACCEPTED", "CAN measurements updated"}};
    if (session_open_) integrate(sample, ev);
    if (!fault_latched_) recompute_state(frame.received_at, ev);
    diagnose(sample, ev);
    return ev;
}

std::vector<Event> Monitor::ingest_power_status(const CanFrame& frame) {
    if (frame.length != 1) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "POWER_STATUS_INVALID", "Invalid power status frame"}};
    }
    const auto meaning = config_.protocol.status_map[frame.data[0]];
    if (meaning < 0) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "POWER_STATUS_INVALID", "Invalid power status frame"}};
    }
    if (last_status_at_ && frame.received_at < *last_status_at_) {
        ++data_.rejected_frames;
        return {{EventLevel::Warning, "CLOCK_REGRESSION", "Non-monotonic status timestamp, frame rejected"}};
    }
    last_status_at_ = frame.received_at;
    data_.status_stale = false;
    status_timeout_reported_ = false;
    ++data_.accepted_status_frames;
    last_status_value_ = meaning;
    if (meaning == kStatusFault) {
        data_.power_enabled = false;
        return enter_fault("POWER_STAGE_FAULT", "The power stage reports a fault", frame.received_at);
    }
    data_.power_enabled = (meaning == kStatusCharging);
    if (fault_latched_) return {};
    std::vector<Event> ev{{EventLevel::Info, "POWER_STATUS_ACCEPTED", "Power status updated"}};
    recompute_state(frame.received_at, ev);
    return ev;
}

void Monitor::recompute_state(Clock::time_point now, std::vector<Event>& events) {
    if (fault_latched_) {
        set_operational_state(ChargeState::Fault, now, data_.fault_code, events);
        return;
    }
    const bool m_ok = fresh(last_measurement_at_, now, config_.measurement_timeout);
    const bool s_ok = fresh(last_status_at_, now, config_.status_timeout);
    ChargeState next = ChargeState::Disconnected;
    if (!last_measurement_at_ && !last_status_at_) {
        next = ChargeState::Unknown;
    } else if (m_ok && s_ok && last_status_value_) {
        switch (*last_status_value_) {
            case kStatusOff:
                next = ChargeState::Off;
                break;
            case kStatusReady:
                next = ChargeState::Ready;
                break;
            case kStatusCharging:
                next = ChargeState::Charging;
                break;
            default:
                next = ChargeState::Fault;
                break;
        }
    }
    set_operational_state(next, now, leave_reason(next, data_.fault_code), events);
}

std::vector<Event> Monitor::tick(Clock::time_point now) {
    std::vector<Event> events;
    data_.measurements_stale = !fresh(last_measurement_at_, now, config_.measurement_timeout);
    data_.status_stale = !fresh(last_status_at_, now, config_.status_timeout);
    if (fault_latched_) {
        data_.state = ChargeState::Fault;
        account_until(now);
        dwell_state_ = ChargeState::Fault;
        return events;
    }
    if (data_.measurements_stale && last_measurement_at_ && !measurement_timeout_reported_) {
        data_.measurements.valid = false;
        measurement_timeout_reported_ = true;
        ++kpi_.timeout_events;
        events.push_back({EventLevel::Warning, "MEASUREMENTS_TIMEOUT", "CAN measurement timeout"});
    }
    if (data_.status_stale && last_status_at_ && !status_timeout_reported_) {
        data_.power_enabled = false;
        status_timeout_reported_ = true;
        ++kpi_.timeout_events;
        events.push_back({EventLevel::Warning, "POWER_STATUS_TIMEOUT", "Power stage status timeout"});
    }
    ChargeState next = data_.state;
    if (!last_measurement_at_ && !last_status_at_) {
        next = ChargeState::Unknown;
    } else if (data_.measurements_stale || data_.status_stale) {
        next = ChargeState::Disconnected;
    }
    if (next != data_.state || session_open_) {
        set_operational_state(next, now, leave_reason(next, data_.fault_code), events);
    } else {
        account_until(now);
    }
    return events;
}

AssetKpi Monitor::kpi_at(Clock::time_point now) const {
    AssetKpi kpi = kpi_;
    if (dwell_from_ && now >= *dwell_from_) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *dwell_from_).count();
        add_dwell(dwell_state_, ms, kpi);
    }
    kpi.in_service_ms = kpi.off_ms + kpi.ready_ms + kpi.charging_ms + kpi.fault_ms + kpi.disconnected_ms;
    if (kpi.in_service_ms > 0) {
        const auto up = kpi.off_ms + kpi.ready_ms + kpi.charging_ms;
        kpi.availability_ppm = static_cast<std::int64_t>(static_cast<WideInt>(up) * kPpm / kpi.in_service_ms);
        kpi.utilization_ppm =
            static_cast<std::int64_t>(static_cast<WideInt>(kpi.charging_ms) * kPpm / kpi.in_service_ms);
    }
    kpi.accepted_frames = data_.accepted_measurement_frames + data_.accepted_status_frames;
    kpi.rejected_frames = data_.rejected_frames;
    const auto seen = kpi.accepted_frames + kpi.rejected_frames;
    if (seen > 0) {
        kpi.accept_ppm =
            static_cast<std::int64_t>(static_cast<WideInt>(kpi.accepted_frames) * kPpm / static_cast<WideInt>(seen));
    }
    return kpi;
}

SessionRecord Monitor::priced_open(Clock::time_point now) const {
    SessionRecord rec = open_;
    if (now >= session_started_at_) {
        rec.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - session_started_at_).count();
    }
    price(rec);
    rec.closed = false;
    rec.billing_complete = false;
    return rec;
}

Snapshot Monitor::snapshot(Clock::time_point now) const {
    Snapshot out = data_;
    out.measurements_stale = !fresh(last_measurement_at_, now, config_.measurement_timeout);
    out.status_stale = !fresh(last_status_at_, now, config_.status_timeout);
    const auto age = [now](const std::optional<Clock::time_point>& at) -> std::int64_t {
        if (!at || now < *at) return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - *at).count();
    };
    out.measurement_age_ms = age(last_measurement_at_);
    out.status_age_ms = age(last_status_at_);
    if (out.measurements_stale) out.measurements.valid = false;
    if (fault_latched_) out.state = ChargeState::Fault;
    out.voltage_mv = last_sample_ ? last_sample_->voltage_mv : 0;
    out.current_ma = last_sample_ ? last_sample_->current_ma : 0;
    out.power_mw = (out.state == ChargeState::Charging && out.measurements.valid && last_sample_)
                       ? last_sample_->power_uw / 1000
                       : 0;
    out.currency = config_.currency;
    out.asset_id = config_.asset_id;
    out.diagnostics = diag_;
    out.fault_action = recommended_action(out.fault_code);
    out.kpi = kpi_at(now);
    if (session_open_) out.session = priced_open(now);
    out.last_session = last_closed_;
    return out;
}

Event Monitor::note_transport_error(std::string code) {
    ++kpi_.transport_errors;
    return {EventLevel::Warning, std::move(code), "CAN transport error; the charge state is unchanged"};
}

bool Monitor::reset_fault(std::string& reason, Clock::time_point now) {
    if (!fault_latched_) {
        reason = "No latched fault to reset";
        return false;
    }
    if (!last_measurement_at_ || !last_status_at_) {
        reason = "Reset denied: measurements and power status were never received";
        return false;
    }
    if (!fresh(last_measurement_at_, now, config_.measurement_timeout) ||
        !fresh(last_status_at_, now, config_.status_timeout)) {
        reason = "Reset denied: data expired, restore communication first";
        return false;
    }
    if (data_.fault_code == "MEASUREMENT_OUT_OF_RANGE" || data_.fault_code == "POWER_STAGE_FAULT" ||
        data_.fault_code == "ENERGY_COUNTER_SATURATED") {
        reason = "Reset denied: this fault code requires external confirmation that the physical cause was removed";
        return false;
    }
    fault_latched_ = false;
    data_.fault_code.clear();
    data_.fault_message.clear();
    data_.state = ChargeState::Disconnected;
    dwell_state_ = ChargeState::Disconnected;
    reason = "Software latch cleared; the state stays DISCONNECTED until fresh valid data arrives";
    return true;
}

std::string to_string(ChargeState s) {
    switch (s) {
        case ChargeState::Unknown:
            return "UNKNOWN";
        case ChargeState::Off:
            return "OFF";
        case ChargeState::Ready:
            return "READY";
        case ChargeState::Charging:
            return "CHARGING";
        case ChargeState::Fault:
            return "FAULT";
        case ChargeState::Disconnected:
            return "DISCONNECTED";
    }
    return "UNKNOWN";
}

std::string to_string(EventLevel l) {
    switch (l) {
        case EventLevel::Info:
            return "INFO";
        case EventLevel::Warning:
            return "WARNING";
        case EventLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

const char* recommended_action(const std::string& code) noexcept {
    if (code == "MEASUREMENT_OUT_OF_RANGE")
        return "Check the scaling, the sensor and the limits; do not reset remotely";
    if (code == "POWER_STAGE_FAULT") return "Isolate the bay and follow the manufacturer's fault procedure";
    if (code == "ENERGY_COUNTER_SATURATED") return "Close the counting shift and archive the ledger";
    if (code == "MEASUREMENTS_TIMEOUT" || code == "POWER_STATUS_TIMEOUT")
        return "Check the bus, the termination, the connector and the gateway";
    if (code == "CAN_ID_UNKNOWN") return "The protocol profile does not match this asset";
    if (code == "CLOCK_REGRESSION") return "Check the time source of the replay or of the gateway";
    if (code == "MEASUREMENT_GAP") return "Do not bill the missing interval; raise the step only with evidence";
    if (code == "SESSION_CLOSED") return "Invoice automatically only when billing_complete is true";
    if (code == "MEASUREMENT_STUCK") return "Check the measurement channel's sensor and wiring";
    if (code == "VOLTAGE_RATE_EXCEEDED" || code == "CURRENT_RATE_EXCEEDED")
        return "Check transients and abnormal line loading";
    if (code == "VOLTAGE_SAG_UNDER_LOAD") return "Inspect the connector, the fuses and the contactor";
    if (code == "CURRENT_HUNTING") return "Check the terminal tightness and the contact resistance";
    return "";
}

std::string json_snapshot(const Snapshot& s) {
    std::ostringstream o;
    o << std::setfill('0') << "{\"schema\":\"" << SNAPSHOT_SCHEMA << "\",\"version\":\"" << VERSION
      << "\",\"asset_id\":\"" << json_escape(s.asset_id) << "\",\"currency\":\"" << json_escape(s.currency)
      << "\",\"state\":\"" << to_string(s.state) << "\",\"voltage_v\":";
    append_si_2dp(o, s.voltage_mv);
    o << ",\"current_a\":";
    append_si_2dp(o, s.current_ma);
    o << ",\"voltage_mv\":" << s.voltage_mv << ",\"current_ma\":" << s.current_ma << ",\"power_mw\":" << s.power_mw
      << ",\"measurements_valid\":" << json_bool(s.measurements.valid)
      << ",\"measurements_stale\":" << json_bool(s.measurements_stale)
      << ",\"status_stale\":" << json_bool(s.status_stale) << ",\"power_enabled\":" << json_bool(s.power_enabled)
      << ",\"measurement_age_ms\":" << s.measurement_age_ms << ",\"status_age_ms\":" << s.status_age_ms
      << ",\"fault_code\":\"" << json_escape(s.fault_code) << "\",\"fault_message\":\"" << json_escape(s.fault_message)
      << "\",\"fault_action\":\"" << json_escape(s.fault_action)
      << "\",\"accepted_measurement_frames\":" << s.accepted_measurement_frames
      << ",\"accepted_status_frames\":" << s.accepted_status_frames << ",\"rejected_frames\":" << s.rejected_frames
      << ",\"energy_closed_uwh\":\"" << s.kpi.energy_closed_uwh << "\",\"availability_ppm\":" << s.kpi.availability_ppm
      << ",\"utilization_ppm\":" << s.kpi.utilization_ppm << ",\"accept_ppm\":" << s.kpi.accept_ppm
      << ",\"in_service_ms\":" << s.kpi.in_service_ms << ",\"charging_ms\":" << s.kpi.charging_ms
      << ",\"fault_ms\":" << s.kpi.fault_ms << ",\"disconnected_ms\":" << s.kpi.disconnected_ms
      << ",\"sessions_opened\":" << s.kpi.sessions_opened << ",\"sessions_closed\":" << s.kpi.sessions_closed
      << ",\"sessions_billable\":" << s.kpi.sessions_billable << ",\"timeout_events\":" << s.kpi.timeout_events
      << ",\"transport_errors\":" << s.kpi.transport_errors << ",\"stuck_at_warnings\":" << s.kpi.stuck_at_warnings
      << ",\"rate_of_change_warnings\":" << s.kpi.rate_of_change_warnings << ",\"sag_warnings\":" << s.kpi.sag_warnings
      << ",\"current_hunt_warnings\":" << s.kpi.current_hunt_warnings
      << ",\"diagnostics\":{\"measurement_stuck\":" << json_bool(s.diagnostics.measurement_stuck)
      << ",\"voltage_rate_exceeded\":" << json_bool(s.diagnostics.voltage_rate_exceeded)
      << ",\"current_rate_exceeded\":" << json_bool(s.diagnostics.current_rate_exceeded)
      << ",\"voltage_sag\":" << json_bool(s.diagnostics.voltage_sag)
      << ",\"current_hunting\":" << json_bool(s.diagnostics.current_hunting) << "},\"session\":";
    if (s.session) {
        append_record(o, *s.session);
    } else {
        o << "null";
    }
    o << ",\"last_session\":";
    if (s.last_session) {
        append_record(o, *s.last_session);
    } else {
        o << "null";
    }
    o << "}";
    return o.str();
}

std::string json_tariff_block(const Tariff& t) {
    std::ostringstream o;
    o << "{\"schema\":\"charger.tariff.v1\",\"model\":\"ocpi-2.2.1-energy-time-flat\",\"vat\":\"omitted\""
         ",\"energy_micro_eur_per_kwh\":\""
      << t.energy_micro_eur_per_kwh << "\",\"time_micro_eur_per_hour\":\"" << t.time_micro_eur_per_hour
      << "\",\"flat_micro_eur\":\"" << t.flat_micro_eur << "\",\"energy_step_wh\":" << t.energy_step_wh
      << ",\"time_step_s\":" << t.time_step_s;
    if (t.min_price_micro_eur) o << ",\"min_price_micro_eur\":\"" << *t.min_price_micro_eur << '"';
    if (t.max_price_micro_eur) o << ",\"max_price_micro_eur\":\"" << *t.max_price_micro_eur << '"';
    o << "}";
    return o.str();
}

std::string json_session_line(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                              const std::string& closed_at_utc, const std::string& tariff_json) {
    std::ostringstream o;
    o << "{\"schema\":\"" << SESSION_SCHEMA << "\",\"version\":\"" << VERSION << "\",\"asset_id\":\""
      << json_escape(asset_id) << "\",\"currency\":\"" << json_escape(currency) << "\",\"closed_at_utc\":\""
      << json_escape(closed_at_utc)
      << "\",\"price_model\":\"ocpi-2.2.1-energy-time-flat\",\"vat\":\"omitted\",\"session\":";
    append_record(o, rec);
    if (!tariff_json.empty()) o << ",\"tariff\":" << tariff_json;
    o << "}";
    return o.str();
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return o.str();
}

std::optional<std::int64_t> parse_fixed_point(const std::string& text, std::int64_t scale) {
    const int digits = scale_digits(scale);
    if (digits < 0 || text.empty()) return std::nullopt;
    std::size_t i = 0;
    if (text[i] == '+') ++i;
    if (i >= text.size()) return std::nullopt;
    std::int64_t whole = 0;
    bool saw_digit = false;
    for (; i < text.size() && text[i] != '.'; ++i) {
        if (text[i] < '0' || text[i] > '9') return std::nullopt;
        saw_digit = true;
        const int dig = text[i] - '0';
        if (whole > (std::numeric_limits<std::int64_t>::max() - dig) / 10) return std::nullopt;
        whole = whole * 10 + dig;
    }
    std::int64_t frac = 0;
    int frac_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return std::nullopt;
            saw_digit = true;
            if (frac_digits >= digits) return std::nullopt;
            frac = frac * 10 + (text[i] - '0');
            ++frac_digits;
        }
    }
    if (!saw_digit || i != text.size()) return std::nullopt;
    for (int p = frac_digits; p < digits; ++p) {
        if (frac > std::numeric_limits<std::int64_t>::max() / 10) return std::nullopt;
        frac *= 10;
    }
    if (whole > std::numeric_limits<std::int64_t>::max() / scale) return std::nullopt;
    const std::int64_t whole_part = whole * scale;
    if (whole_part > std::numeric_limits<std::int64_t>::max() - frac) return std::nullopt;
    return whole_part + frac;
}

std::string format_fixed_point(std::int64_t value, std::int64_t scale) {
    const int digits = scale_digits(scale);
    if (digits < 0) return {};
    const bool neg = value < 0;
    const std::int64_t abs_v = neg ? -value : value;
    const std::int64_t whole = abs_v / scale;
    const std::int64_t frac = abs_v % scale;
    std::ostringstream o;
    if (neg) o << '-';
    o << whole << '.' << std::setw(digits) << std::setfill('0') << frac;
    return o.str();
}

namespace {

std::string trim(const std::string& s) {
    const auto is_blank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t b = 0, e = s.size();
    while (b < e && is_blank(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_blank(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

int hex_nibble(char c) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

}  // namespace

std::optional<CanFrame> parse_hex_frame(const std::string& line, Clock::time_point now) {
    const auto cleaned = trim(line);
    const auto hash = cleaned.find('#');
    if (hash == std::string::npos || hash == 0 || hash > 3) return std::nullopt;

    std::uint32_t id = 0;
    for (std::size_t i = 0; i < hash; ++i) {
        const int nib = hex_nibble(cleaned[i]);
        if (nib < 0) return std::nullopt;
        id = (id << 4) | static_cast<std::uint32_t>(nib);
    }
    if (id > 0x7ff) return std::nullopt;

    CanFrame f;
    f.id = id;
    f.received_at = now;
    const std::string data_hex = cleaned.substr(hash + 1);
    if (data_hex.empty()) return f;
    if (data_hex.size() % 2 != 0 || data_hex.size() > 16) return std::nullopt;
    for (std::size_t p = 0; p < data_hex.size(); p += 2) {
        const int hi = hex_nibble(data_hex[p]), lo = hex_nibble(data_hex[p + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        f.data[p / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    f.length = static_cast<std::uint8_t>(data_hex.size() / 2);
    return f;
}

}  // namespace charger
