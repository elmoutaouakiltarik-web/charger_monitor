#include "charger_export.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace charger {
namespace {

constexpr std::int64_t kMilli = 1000;
constexpr std::int64_t kMicro = 1000000;

using Labels = std::vector<std::pair<std::string, std::string>>;

// Unsigned fixed point with a power-of-ten scale, for registers that may exceed
// INT64_MAX (microwatt-hours, micro-EUR). Integer arithmetic only.
std::string fixed_u64(std::uint64_t value, std::uint64_t scale, int digits) {
    std::ostringstream o;
    o << value / scale << '.' << std::setw(digits) << std::setfill('0') << value % scale;
    return o.str();
}

std::string micro_u64(std::uint64_t value) { return fixed_u64(value, 1000000ull, 6); }

std::string flag(bool v) { return v ? "1" : "0"; }

// Accumulates one exposition document. Each family is declared once, with its
// HELP and TYPE, before its samples, so the output is valid input for the
// node_exporter textfile collector.
class Exposition {
   public:
    void family(const char* name, const char* type, const char* help) {
        out_ << "# HELP " << name << ' ' << help << '\n' << "# TYPE " << name << ' ' << type << '\n';
        name_ = name;
    }

    void sample(const Labels& labels, const std::string& value) {
        out_ << name_;
        if (!labels.empty()) {
            out_ << '{';
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (i != 0) out_ << ',';
                out_ << labels[i].first << "=\"" << prometheus_escape_label(labels[i].second) << '"';
            }
            out_ << '}';
        }
        out_ << ' ' << value << '\n';
    }

    std::string str() const { return out_.str(); }

   private:
    std::ostringstream out_;
    const char* name_{""};
};

Labels with(const std::string& asset, const char* key, const std::string& value) {
    return {{"asset", asset}, {key, value}};
}

const char* const kStates[] = {"UNKNOWN", "OFF", "READY", "CHARGING", "FAULT", "DISCONNECTED"};

std::string state_name(ChargeState s) { return to_string(s); }

std::string error_text(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + std::strerror(errno);
}

}  // namespace

std::string prometheus_escape_label(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string prometheus_metrics(const Snapshot& s, const std::string& version, std::int64_t unix_seconds) {
    Exposition x;
    const Labels asset{{"asset", s.asset_id}};
    const auto& k = s.kpi;

    x.family("charger_info", "gauge", "Static facts about the monitored bay; the value is always 1.");
    x.sample({{"asset", s.asset_id}, {"version", version}, {"currency", s.currency}}, "1");

    if (unix_seconds >= 0) {
        // A frozen file keeps its old values forever under node_exporter, so the
        // render time is the only way to notice a dead monitor from the values.
        x.family("charger_export_timestamp_seconds", "gauge", "Unix time at which this file was rendered.");
        x.sample(asset, std::to_string(unix_seconds));
    }

    x.family("charger_state", "gauge", "Charge state of the bay, one-hot: exactly one state has the value 1.");
    for (const char* name : kStates) x.sample(with(s.asset_id, "state", name), flag(state_name(s.state) == name));

    x.family("charger_power_enabled", "gauge", "1 while the power stage reports it is delivering power.");
    x.sample(asset, flag(s.power_enabled));

    // Instantaneous values are exported only while the stream is fresh: a value
    // frozen from before a bus dropout would look like a live reading.
    if (s.measurements.valid && !s.measurements_stale) {
        x.family("charger_measurement_voltage_volts", "gauge", "Last accepted DC voltage.");
        x.sample(asset, format_fixed_point(s.voltage_mv, kMilli));
        x.family("charger_measurement_current_amperes", "gauge", "Last accepted DC current.");
        x.sample(asset, format_fixed_point(s.current_ma, kMilli));
        x.family("charger_measurement_power_watts", "gauge", "Delivered power; 0 unless the bay is CHARGING.");
        x.sample(asset, format_fixed_point(s.power_mw, kMilli));
    }

    x.family("charger_measurements_stale", "gauge", "1 when the measurement stream is older than its timeout.");
    x.sample(asset, flag(s.measurements_stale));
    x.family("charger_status_stale", "gauge", "1 when the power status stream is older than its timeout.");
    x.sample(asset, flag(s.status_stale));

    if (s.measurement_age_ms >= 0) {
        x.family("charger_measurement_age_seconds", "gauge", "Seconds since the last accepted measurement frame.");
        x.sample(asset, format_fixed_point(s.measurement_age_ms, kMilli));
    }
    if (s.status_age_ms >= 0) {
        x.family("charger_status_age_seconds", "gauge", "Seconds since the last accepted power status frame.");
        x.sample(asset, format_fixed_point(s.status_age_ms, kMilli));
    }

    x.family("charger_frames_accepted_total", "counter", "CAN frames accepted by the protocol profile.");
    x.sample(with(s.asset_id, "type", "measurement"), std::to_string(s.accepted_measurement_frames));
    x.sample(with(s.asset_id, "type", "status"), std::to_string(s.accepted_status_frames));
    x.family("charger_frames_rejected_total", "counter", "CAN frames rejected by validation.");
    x.sample(asset, std::to_string(s.rejected_frames));

    x.family("charger_sessions_opened_total", "counter", "Charging sessions opened.");
    x.sample(asset, std::to_string(k.sessions_opened));
    x.family("charger_sessions_closed_total", "counter", "Charging sessions closed.");
    x.sample(asset, std::to_string(k.sessions_closed));
    x.family("charger_sessions_billable_total", "counter", "Closed sessions with billing_complete true.");
    x.sample(asset, std::to_string(k.sessions_billable));
    x.family("charger_sessions_undersampled_total", "counter", "Closed sessions with fewer than two samples.");
    x.sample(asset, std::to_string(k.sessions_undersampled));
    x.family("charger_integration_gaps_total", "counter",
             "Measurement gaps over the integration step. Energy is never interpolated across them.");
    x.sample(asset, std::to_string(k.integration_gaps));
    x.family("charger_energy_closed_watt_hours_total", "counter",
             "Exact energy of closed sessions, from the microwatt-hour register.");
    x.sample(asset, micro_u64(k.energy_closed_uwh));

    x.family("charger_state_seconds_total", "counter", "Time spent in each charge state.");
    x.sample(with(s.asset_id, "state", "UNKNOWN"), format_fixed_point(k.unknown_ms, kMilli));
    x.sample(with(s.asset_id, "state", "OFF"), format_fixed_point(k.off_ms, kMilli));
    x.sample(with(s.asset_id, "state", "READY"), format_fixed_point(k.ready_ms, kMilli));
    x.sample(with(s.asset_id, "state", "CHARGING"), format_fixed_point(k.charging_ms, kMilli));
    x.sample(with(s.asset_id, "state", "FAULT"), format_fixed_point(k.fault_ms, kMilli));
    x.sample(with(s.asset_id, "state", "DISCONNECTED"), format_fixed_point(k.disconnected_ms, kMilli));
    x.family("charger_in_service_seconds_total", "counter",
             "Time the bay has been observed (every state but UNKNOWN).");
    x.sample(asset, format_fixed_point(k.in_service_ms, kMilli));

    // -1 ppm means undefined, which Prometheus expresses by the absence of the
    // sample. Exporting 0 would read as "never available".
    if (k.availability_ppm >= 0) {
        x.family("charger_availability_ratio", "gauge",
                 "Share of in-service time not in FAULT or DISCONNECTED (0..1).");
        x.sample(asset, format_fixed_point(k.availability_ppm, kMicro));
    }
    if (k.utilization_ppm >= 0) {
        x.family("charger_utilization_ratio", "gauge", "Share of in-service time spent CHARGING (0..1).");
        x.sample(asset, format_fixed_point(k.utilization_ppm, kMicro));
    }
    if (k.accept_ppm >= 0) {
        x.family("charger_frame_accept_ratio", "gauge", "Share of received frames that validation accepted (0..1).");
        x.sample(asset, format_fixed_point(k.accept_ppm, kMicro));
    }

    x.family("charger_fault_events_total", "counter", "Latched fault events.");
    x.sample(asset, std::to_string(k.fault_events));
    x.family("charger_timeout_events_total", "counter", "Stream timeouts detected.");
    x.sample(asset, std::to_string(k.timeout_events));
    x.family("charger_transport_errors_total", "counter", "CAN transport errors reported by the interface.");
    x.sample(asset, std::to_string(k.transport_errors));

    const auto& d = s.diagnostics;
    x.family("charger_diagnostic_active", "gauge",
             "Predictive advisory currently raised. An advisory never blocks charging.");
    x.sample(with(s.asset_id, "diagnostic", "measurement_stuck"), flag(d.measurement_stuck));
    x.sample(with(s.asset_id, "diagnostic", "voltage_rate_exceeded"), flag(d.voltage_rate_exceeded));
    x.sample(with(s.asset_id, "diagnostic", "current_rate_exceeded"), flag(d.current_rate_exceeded));
    x.sample(with(s.asset_id, "diagnostic", "voltage_sag"), flag(d.voltage_sag));
    x.sample(with(s.asset_id, "diagnostic", "current_hunting"), flag(d.current_hunting));
    x.family("charger_diagnostic_warnings_total", "counter", "Advisories raised since start.");
    x.sample(with(s.asset_id, "diagnostic", "measurement_stuck"), std::to_string(k.stuck_at_warnings));
    x.sample(with(s.asset_id, "diagnostic", "rate_of_change"), std::to_string(k.rate_of_change_warnings));
    x.sample(with(s.asset_id, "diagnostic", "voltage_sag"), std::to_string(k.sag_warnings));
    x.sample(with(s.asset_id, "diagnostic", "current_hunting"), std::to_string(k.current_hunt_warnings));

    if (!s.fault_code.empty()) {
        x.family("charger_fault_active", "gauge", "1 while a fault is latched; the code label says which.");
        x.sample(with(s.asset_id, "code", s.fault_code), "1");
    }

    x.family("charger_session_active", "gauge", "1 while a charging session is open.");
    x.sample(asset, flag(s.session.has_value()));
    if (s.session) {
        x.family("charger_session_energy_watt_hours", "gauge", "Energy integrated so far in the open session.");
        x.sample(asset, micro_u64(s.session->energy_uwh));
        x.family("charger_session_duration_seconds", "gauge", "Duration so far of the open session.");
        x.sample(asset, format_fixed_point(s.session->duration_ms, kMilli));
    }
    return x.str();
}

std::string prometheus_portfolio(const PortfolioKpi& p) {
    Exposition x;
    x.family("charger_portfolio_assets", "gauge", "Assets present in the journal.");
    x.sample({}, std::to_string(p.assets));
    x.family("charger_portfolio_sessions_closed", "gauge", "Closed sessions in the journal.");
    x.sample({}, std::to_string(p.sessions_closed));
    x.family("charger_portfolio_sessions_billable", "gauge", "Closed sessions with billing_complete true.");
    x.sample({}, std::to_string(p.sessions_billable));
    x.family("charger_portfolio_sessions_held", "gauge", "Closed sessions held for review, not invoiced.");
    x.sample({}, std::to_string(p.sessions_held));
    x.family("charger_portfolio_energy_watt_hours", "gauge", "Exact energy of closed sessions.");
    x.sample({}, micro_u64(p.energy_uwh));
    x.family("charger_portfolio_revenue_eur", "gauge", "Total session value in EUR excluding VAT, billable or held.");
    x.sample({}, micro_u64(p.revenue_micro_eur));
    x.family("charger_portfolio_charging_seconds", "gauge", "Charging time of closed sessions.");
    x.sample({}, format_fixed_point(p.charging_ms, kMilli));
    x.family("charger_portfolio_fault_stops", "gauge", "Sessions stopped by a fault.");
    x.sample({}, std::to_string(p.fault_stops));
    x.family("charger_portfolio_timeout_stops", "gauge", "Sessions stopped by a communication timeout.");
    x.sample({}, std::to_string(p.timeout_stops));
    if (p.mean_power_mw >= 0) {
        x.family("charger_portfolio_mean_power_watts", "gauge", "Delivered energy over charging time.");
        x.sample({}, format_fixed_point(p.mean_power_mw, kMilli));
    }
    if (p.revenue_per_kwh_milli_micro_eur >= 0) {
        x.family("charger_portfolio_revenue_per_kwh_eur", "gauge", "Session value per kWh actually delivered.");
        x.sample({}, format_fixed_point(p.revenue_per_kwh_milli_micro_eur, kMicro));
    }

    const auto per_asset = [&](const char* name, const char* help, const auto& value) {
        if (p.per_asset.empty()) return;
        x.family(name, "gauge", help);
        for (const auto& a : p.per_asset) x.sample({{"asset", a.asset_id}}, value(a));
    };
    per_asset("charger_asset_sessions_closed", "Closed sessions of the asset.",
              [](const PortfolioAsset& a) { return std::to_string(a.sessions_closed); });
    per_asset("charger_asset_sessions_billable", "Billable closed sessions of the asset.",
              [](const PortfolioAsset& a) { return std::to_string(a.sessions_billable); });
    per_asset("charger_asset_undersampled_sessions", "Closed sessions with fewer than two samples.",
              [](const PortfolioAsset& a) { return std::to_string(a.undersampled_sessions); });
    per_asset("charger_asset_energy_watt_hours", "Exact energy of the asset's closed sessions.",
              [](const PortfolioAsset& a) { return micro_u64(a.energy_uwh); });
    per_asset("charger_asset_revenue_eur", "Session value of the asset in EUR excluding VAT.",
              [](const PortfolioAsset& a) { return micro_u64(a.revenue_micro_eur); });
    per_asset("charger_asset_charging_seconds", "Charging time of the asset's closed sessions.",
              [](const PortfolioAsset& a) { return format_fixed_point(a.charging_ms, kMilli); });
    per_asset("charger_asset_fault_stops", "Sessions of the asset stopped by a fault.",
              [](const PortfolioAsset& a) { return std::to_string(a.fault_stops); });
    per_asset("charger_asset_timeout_stops", "Sessions of the asset stopped by a communication timeout.",
              [](const PortfolioAsset& a) { return std::to_string(a.timeout_stops); });
    return x.str();
}

bool write_file_atomic(const std::string& path, const std::string& content, std::string& error, bool durable) {
    if (path.empty()) {
        error = "empty path";
        return false;
    }
    const std::string tmp = path + ".tmp";
#if defined(_WIN32)
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        error = error_text("cannot create", tmp);
        return false;
    }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size() && std::fflush(f) == 0;
    if (ok && durable) ok = _commit(_fileno(f)) == 0;
    if (!ok) error = error_text("cannot write", tmp);
    if (std::fclose(f) != 0 && ok) {
        ok = false;
        error = error_text("cannot close", tmp);
    }
    if (ok && !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false;
        error = "cannot replace " + path + ": Win32 error " + std::to_string(GetLastError());
    }
    if (!ok) std::remove(tmp.c_str());
    return ok;
#else
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    // The temporary name is predictable; never write through a planted symlink.
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(tmp.c_str(), flags, 0644);
    if (fd < 0) {
        error = error_text("cannot create", tmp);
        return false;
    }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else {
            done += static_cast<std::size_t>(n);
        }
    }
    if (ok && durable && ::fsync(fd) != 0) ok = false;
    if (!ok) error = error_text("cannot write", tmp);
    if (::close(fd) != 0 && ok) {
        ok = false;
        error = error_text("cannot close", tmp);
    }
    if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) {
        ok = false;
        error = error_text("cannot replace", path);
    }
    if (!ok) ::unlink(tmp.c_str());
    return ok;
#endif
}

}  // namespace charger
