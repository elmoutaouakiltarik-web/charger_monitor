#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "charger_export.hpp"
#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"

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

struct Sample {
    std::string name;
    std::map<std::string, std::string> labels;
    std::string value;
};

// A strict reader for the exposition format: it accepts only what a Prometheus
// scraper would accept, so a malformed line fails the test instead of passing.
bool parse_sample(const std::string& line, Sample& out) {
    std::size_t i = 0;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) != 0 || line[i] == '_' || line[i] == ':')) {
        ++i;
    }
    if (i == 0) return false;
    out.name = line.substr(0, i);
    out.labels.clear();
    if (i < line.size() && line[i] == '{') {
        ++i;
        while (true) {
            std::size_t k = i;
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) != 0 || line[i] == '_')) ++i;
            if (i == k || i + 1 >= line.size() || line[i] != '=' || line[i + 1] != '"') return false;
            const std::string key = line.substr(k, i - k);
            i += 2;
            std::string val;
            while (true) {
                if (i >= line.size()) return false;
                const char c = line[i++];
                if (c == '"') break;
                if (c == '\\') {
                    if (i >= line.size()) return false;
                    const char e = line[i++];
                    if (e == 'n') {
                        val += '\n';
                    } else if (e == '\\' || e == '"') {
                        val += e;
                    } else {
                        return false;
                    }
                } else {
                    val += c;
                }
            }
            out.labels[key] = val;
            if (i < line.size() && line[i] == ',') {
                ++i;
                continue;
            }
            if (i < line.size() && line[i] == '}') {
                ++i;
                break;
            }
            return false;
        }
    }
    if (i >= line.size() || line[i] != ' ') return false;
    out.value = line.substr(i + 1);
    if (out.value.empty()) return false;
    std::size_t j = out.value[0] == '-' ? 1 : 0;
    std::size_t digits = 0;
    while (j < out.value.size() && out.value[j] >= '0' && out.value[j] <= '9') ++j, ++digits;
    if (digits == 0) return false;
    if (j < out.value.size() && out.value[j] == '.') {
        ++j;
        std::size_t frac = 0;
        while (j < out.value.size() && out.value[j] >= '0' && out.value[j] <= '9') ++j, ++frac;
        if (frac == 0) return false;
    }
    return j == out.value.size();
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

// Checks the structure of a whole document and returns its samples.
std::vector<Sample> check_exposition(const std::string& text) {
    require(!text.empty() && text.back() == '\n', "the exposition must end with a newline");
    std::vector<Sample> samples;
    std::set<std::string> families;
    std::string help_for, type_for;
    std::map<std::string, std::string> types;
    std::set<std::string> series;
    for (const std::string& line : split_lines(text)) {
        require(!line.empty(), "no blank lines inside the exposition");
        if (line.rfind("# HELP ", 0) == 0) {
            const std::string name = line.substr(7, line.find(' ', 7) - 7);
            require(families.insert(name).second, "family declared twice: " + name);
            require(name.rfind("charger_", 0) == 0, "family must carry the charger_ prefix: " + name);
            help_for = name;
            type_for.clear();
        } else if (line.rfind("# TYPE ", 0) == 0) {
            const std::string rest = line.substr(7);
            const auto sp = rest.find(' ');
            require(sp != std::string::npos, "TYPE needs a type");
            const std::string name = rest.substr(0, sp), type = rest.substr(sp + 1);
            require(name == help_for, "TYPE must directly follow its HELP: " + name);
            require(type == "counter" || type == "gauge", "unexpected metric type: " + type);
            if (type == "counter") {
                require(name.size() > 6 && name.compare(name.size() - 6, 6, "_total") == 0,
                        "a counter must end with _total: " + name);
            } else {
                require(!(name.size() > 6 && name.compare(name.size() - 6, 6, "_total") == 0),
                        "only counters end with _total: " + name);
            }
            type_for = name;
            types[name] = type;
        } else {
            require(line[0] != '#', "unexpected comment: " + line);
            Sample s;
            require(parse_sample(line, s), "line does not parse as name{labels} value: " + line);
            require(s.name == type_for, "sample outside its family: " + line);
            require(series.insert(line.substr(0, line.rfind(' '))).second, "duplicate series: " + line);
            require(s.labels.count("asset") == 1 || s.name.rfind("charger_portfolio_", 0) == 0,
                    "every per-bay sample carries asset: " + line);
            samples.push_back(s);
        }
    }
    require(!families.empty(), "no family at all");
    return samples;
}

const Sample* find(const std::vector<Sample>& samples, const std::string& name,
                   const std::map<std::string, std::string>& labels = {}) {
    for (const Sample& s : samples) {
        if (s.name != name) continue;
        bool match = true;
        for (const auto& kv : labels) {
            const auto it = s.labels.find(kv.first);
            if (it == s.labels.end() || it->second != kv.second) match = false;
        }
        if (match) return &s;
    }
    return nullptr;
}

std::string value_of(const std::vector<Sample>& samples, const std::string& name,
                     const std::map<std::string, std::string>& labels = {}) {
    const Sample* s = find(samples, name, labels);
    require(s != nullptr, "missing sample: " + name);
    return s->value;
}

}  // namespace

int main() {
    // --- The checker itself must reject what a scraper would reject ---
    {
        const auto rejects = [](const std::string& text) {
            try {
                check_exposition(text);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        };
        const std::string head = "# HELP charger_x_total h\n# TYPE charger_x_total counter\n";
        require(!rejects(head + "charger_x_total{asset=\"a\"} 1\n"), "the reference document is valid");
        require(rejects("# HELP charger_x h\n# TYPE charger_x counter\ncharger_x{asset=\"a\"} 1\n"),
                "a counter without _total is rejected");
        require(rejects(head + "charger_x_total{asset=\"a\"} 1.\n"), "a malformed number is rejected");
        require(rejects(head + "charger_x_total{asset=\"a\"} 1\ncharger_x_total{asset=\"a\"} 2\n"),
                "a duplicate series is rejected");
        require(rejects(head + "charger_y_total{asset=\"a\"} 1\n"), "a sample outside its family is rejected");
        require(rejects(head + "charger_x_total{asset=\"a} 1\n"), "an unterminated label is rejected");
    }

    // --- A fresh bay: nothing is known, so nothing undefined is exported ---
    {
        Monitor m;
        const auto t = Clock::now();
        const auto text = prometheus_metrics(m.snapshot(t), VERSION);
        const auto s = check_exposition(text);
        require(value_of(s, "charger_state", {{"state", "UNKNOWN"}}) == "1", "a fresh bay is UNKNOWN");
        require(value_of(s, "charger_state", {{"state", "CHARGING"}}) == "0", "one-hot: other states are 0");
        require(find(s, "charger_availability_ratio") == nullptr, "undefined availability must be absent, not 0");
        require(find(s, "charger_utilization_ratio") == nullptr, "undefined utilization must be absent, not 0");
        require(find(s, "charger_frame_accept_ratio") == nullptr, "no frames yet: the accept ratio is absent");
        require(find(s, "charger_measurement_age_seconds") == nullptr, "no frame yet: the age is absent");
        require(find(s, "charger_measurement_voltage_volts") == nullptr, "no fresh measurement: no voltage");
        require(find(s, "charger_session_energy_watt_hours") == nullptr, "no open session: no session energy");
        require(find(s, "charger_fault_active") == nullptr, "no fault: no fault series");
        require(value_of(s, "charger_session_active") == "0", "no session is open");
        require(value_of(s, "charger_measurements_stale") == "1", "no measurement yet counts as stale");
        require(value_of(s, "charger_info", {{"version", VERSION}, {"currency", "EUR"}}) == "1",
                "info carries version");
    }

    // --- One hour at 480 V / 10 A: the canonical vector, exact in the export ---
    {
        Config cfg;
        cfg.asset_id = "bay-07";
        cfg.measurement_timeout = std::chrono::minutes(3);
        cfg.status_timeout = std::chrono::minutes(3);
        cfg.max_integration_step = std::chrono::minutes(2);
        Monitor m(cfg);
        const auto t0 = Clock::now();
        const auto min = std::chrono::minutes(1);
        m.ingest(measurements(480.0, 10.0, t0));
        m.ingest(status(2, t0));
        for (int i = 1; i <= 60; ++i) {
            m.ingest(measurements(480.0, 10.0, t0 + min * i));
            m.ingest(status(2, t0 + min * i));
        }
        {
            const auto s = check_exposition(prometheus_metrics(m.snapshot(t0 + min * 60), VERSION));
            const std::map<std::string, std::string> a{{"asset", "bay-07"}};
            require(value_of(s, "charger_state", {{"state", "CHARGING"}}) == "1", "charging");
            require(value_of(s, "charger_measurement_voltage_volts", a) == "480.000", "exact voltage");
            require(value_of(s, "charger_measurement_current_amperes", a) == "10.000", "exact current");
            require(value_of(s, "charger_measurement_power_watts", a) == "4800.000", "exact power");
            require(value_of(s, "charger_session_active", a) == "1", "session active");
            require(value_of(s, "charger_session_energy_watt_hours", a) == "4800.000000", "exact in-progress energy");
            require(value_of(s, "charger_session_duration_seconds", a) == "3600.000", "duration of the open session");
            require(value_of(s, "charger_power_enabled", a) == "1", "power stage enabled");
            require(value_of(s, "charger_sessions_opened_total", a) == "1", "one session opened");
            require(value_of(s, "charger_sessions_closed_total", a) == "0", "none closed yet");
            require(value_of(s, "charger_measurement_age_seconds", a) == "0.000", "fresh frame has age 0");
            require(value_of(s, "charger_frames_accepted_total", {{"type", "measurement"}}) == "61", "frame counter");
            require(value_of(s, "charger_frames_accepted_total", {{"type", "status"}}) == "61", "frame counter");
            require(find(s, "charger_availability_ratio") != nullptr, "availability is defined once time has passed");
        }
        m.ingest(status(0, t0 + min * 60));
        const auto s = check_exposition(prometheus_metrics(m.snapshot(t0 + min * 60), VERSION));
        const std::map<std::string, std::string> a{{"asset", "bay-07"}};
        require(value_of(s, "charger_sessions_closed_total", a) == "1", "one session closed");
        require(value_of(s, "charger_energy_closed_watt_hours_total", a) == "4800.000000",
                "480 V x 10 A x 1 h is exactly 4800 Wh in the export");
        require(value_of(s, "charger_session_active", a) == "0", "session closed");
        require(find(s, "charger_session_energy_watt_hours") == nullptr, "no open session: no session energy");
        require(value_of(s, "charger_state", {{"state", "OFF"}}) == "1", "OFF after the status change");
        require(value_of(s, "charger_state_seconds_total", {{"state", "CHARGING"}}) == "3600.000",
                "dwell time is exported in exact seconds");
    }

    // --- A latched fault is a labelled series, and only while latched ---
    {
        Monitor m;
        const auto t = Clock::now();
        m.ingest(measurements(480, 10, t));
        m.ingest(status(3, t));
        const auto s = check_exposition(prometheus_metrics(m.snapshot(t), VERSION));
        require(value_of(s, "charger_fault_active", {{"code", "POWER_STAGE_FAULT"}}) == "1", "fault code label");
        require(value_of(s, "charger_state", {{"state", "FAULT"}}) == "1", "FAULT state");
        require(value_of(s, "charger_fault_events_total") == "1", "one fault event");
    }

    // --- Hostile label values are escaped, never allowed to break the line ---
    {
        Snapshot snap;
        snap.asset_id = "a\"b\\c\nd";
        snap.currency = "EUR";
        const std::string text = prometheus_metrics(snap, "1\"x");
        const auto s = check_exposition(text);
        require(find(s, "charger_session_active") != nullptr, "document still parses");
        require(s.front().labels.at("asset") == snap.asset_id, "the escaped label round-trips exactly");
        require(text.find("asset=\"a\\\"b\\\\c\\nd\"") != std::string::npos, "escape sequences are as specified");
        require(prometheus_escape_label("plain") == "plain", "plain text is untouched");
        require(prometheus_escape_label("\n") == "\\n" && prometheus_escape_label("\\") == "\\\\" &&
                    prometheus_escape_label("\"") == "\\\"",
                "the three escapes");
    }

    // --- The render time lets an alert catch a dead monitor (node_exporter keeps
    //     serving the last file forever); it is omitted when the caller has none ---
    {
        Snapshot snap;
        snap.asset_id = "bay-01";
        const auto with = check_exposition(prometheus_metrics(snap, VERSION, 1760745600));
        require(value_of(with, "charger_export_timestamp_seconds") == "1760745600", "render time is an exact integer");
        const auto without = check_exposition(prometheus_metrics(snap, VERSION));
        require(find(without, "charger_export_timestamp_seconds") == nullptr, "no timestamp unless the caller has one");
    }

    // --- Ratios are exact decimals built from ppm ---
    {
        Snapshot snap;
        snap.asset_id = "bay-01";
        snap.kpi.availability_ppm = 999999;
        snap.kpi.utilization_ppm = 0;  // defined and zero is NOT the same as undefined
        snap.kpi.accept_ppm = 1000000;
        const auto s = check_exposition(prometheus_metrics(snap, VERSION));
        require(value_of(s, "charger_availability_ratio") == "0.999999", "ppm to ratio, exact");
        require(value_of(s, "charger_utilization_ratio") == "0.000000", "a defined zero stays a zero");
        require(value_of(s, "charger_frame_accept_ratio") == "1.000000", "full ratio");
    }

    // --- The energy register can exceed INT64: unsigned formatting stays exact ---
    {
        Snapshot snap;
        snap.asset_id = "bay-01";
        snap.kpi.energy_closed_uwh = 18446744073709551615ull;
        const auto s = check_exposition(prometheus_metrics(snap, VERSION));
        require(value_of(s, "charger_energy_closed_watt_hours_total") == "18446744073709.551615",
                "UINT64_MAX uWh formats exactly");
    }

    // --- Portfolio rollup ---
    {
        PortfolioKpi k;
        PortfolioAsset a;
        a.asset_id = "bay-01";
        a.sessions_closed = 3;
        a.sessions_billable = 2;
        a.energy_uwh = 4800000000ull;
        a.revenue_micro_eur = 5300000ull;
        a.charging_ms = 3600000;
        a.fault_stops = 1;
        k.per_asset.push_back(a);
        k.assets = 1;
        k.sessions_closed = 3;
        k.sessions_billable = 2;
        k.sessions_held = 1;
        k.energy_uwh = 4800000000ull;
        k.revenue_micro_eur = 5300000ull;
        k.charging_ms = 3600000;
        k.fault_stops = 1;
        const auto s = check_exposition(prometheus_portfolio(k));
        require(value_of(s, "charger_portfolio_sessions_held") == "1", "held sessions");
        require(value_of(s, "charger_portfolio_energy_watt_hours") == "4800.000000", "exact fleet energy");
        require(value_of(s, "charger_portfolio_revenue_eur") == "5.300000", "exact fleet value");
        require(value_of(s, "charger_asset_revenue_eur", {{"asset", "bay-01"}}) == "5.300000", "per-asset value");
        require(value_of(s, "charger_asset_charging_seconds", {{"asset", "bay-01"}}) == "3600.000", "per-asset time");
        require(find(s, "charger_portfolio_mean_power_watts") == nullptr, "undefined mean power is absent");
        require(find(s, "charger_portfolio_revenue_per_kwh_eur") == nullptr, "undefined revenue/kWh is absent");

        k.mean_power_mw = 4800000;
        k.revenue_per_kwh_milli_micro_eur = 1104166;
        const auto s2 = check_exposition(prometheus_portfolio(k));
        require(value_of(s2, "charger_portfolio_mean_power_watts") == "4800.000", "mean power");
        require(value_of(s2, "charger_portfolio_revenue_per_kwh_eur") == "1.104166", "revenue per kWh");

        const auto empty = check_exposition(prometheus_portfolio(PortfolioKpi{}));
        require(value_of(empty, "charger_portfolio_assets") == "0", "an empty journal exports zero assets");
        require(find(empty, "charger_asset_sessions_closed") == nullptr, "no per-asset family without assets");
    }

    // --- Atomic replacement ---
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "charger_export_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string target = (dir / "status.prom").string();
        std::string err;
        require(write_file_atomic(target, "old\n", err), "first write: " + err);
        require(write_file_atomic(target, "new content\n", err, false), "replace: " + err);
        {
            // Scoped on purpose: an open stream keeps a Windows file locked, and
            // the remove_all below would then fail on a handle this test still
            // holds. Linux unlinks open files, so only Windows saw it.
            std::ifstream in(target, std::ios::binary);
            std::ostringstream got;
            got << in.rdbuf();
            require(got.str() == "new content\n", "the target holds the new content");
        }
        std::size_t files = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            (void)e;
            ++files;
        }
        require(files == 1, "no temporary file may be left behind");

        const std::string missing = (dir / "no" / "such" / "dir" / "x.prom").string();
        require(!write_file_atomic(missing, "x", err), "a missing directory must fail");
        require(!err.empty(), "the failure must say why");
        require(!fs::exists(missing) && !fs::exists(missing + ".tmp"), "a failed write leaves nothing");
        require(!write_file_atomic("", "x", err), "an empty path must fail");

        // A directory in the way of the target: the rename fails and the temp file is cleaned up.
        const fs::path blocker = dir / "blocked";
        fs::create_directories(blocker / "inner");
        require(!write_file_atomic(blocker.string(), "x", err), "replacing a directory must fail");
        require(!fs::exists(blocker.string() + ".tmp"), "the temporary file is removed after a failed rename");
        fs::remove_all(dir);
    }

    std::cout << "export tests OK\n";
    return 0;
}
