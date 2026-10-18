// charger_probe — what did the bus actually do?
//
// A commissioning analyser for a `candump -L` recording. Before you set a stream
// timeout or an integration step, look at the real periods of your cabinet:
//
//   candump -L can0 > bay.log                       (on the cabinet, for a representative shift)
//   charger_probe bay.log --config bay-07.json      (anywhere)
//
// For every CAN id it prints the count, the first and last recorded time, and the
// inter-frame period as min / mean / median / p95 / p99 / max in microseconds
// (integer arithmetic, nearest-rank percentiles), plus how many gaps were longer
// than three times the median. It marks the ids that the configuration's protocol
// profile treats as measurement and status, and for those two it states, as plain
// facts, the longest silence observed next to the configured timeouts.
//
// It never suggests a value. Thresholds come from the cabinet's own specification
// and from an engineer, not from a tool that has seen one recording.
//
// Exit codes: 0 report written, 2 usage, unreadable file or a recording without
// timestamps (periods cannot be measured from a recording that has none).

#include <charger_candump.hpp>
#include <charger_config.hpp>
#include <charger_monitor.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace charger;

namespace {

struct IdStats {
    std::uint64_t count{0};
    std::int64_t first_us{0};
    std::int64_t last_us{0};
    std::vector<std::int64_t> periods;
};

struct Summary {
    std::int64_t min{0}, mean{0}, median{0}, p95{0}, p99{0}, max{0};
    std::uint64_t gaps_over_3x_median{0};
    bool has_periods{false};
};

// Nearest rank: the smallest value with at least p percent of the data at or below it.
std::int64_t nearest_rank(const std::vector<std::int64_t>& sorted, std::int64_t p) {
    const auto n = static_cast<std::int64_t>(sorted.size());
    std::int64_t rank = (p * n + 99) / 100;
    rank = std::max<std::int64_t>(1, std::min(rank, n));
    return sorted[static_cast<std::size_t>(rank - 1)];
}

Summary summarise(const IdStats& s) {
    Summary out;
    if (s.periods.empty()) return out;
    std::vector<std::int64_t> sorted = s.periods;
    std::sort(sorted.begin(), sorted.end());
    WideInt sum = 0;
    for (auto v : sorted) sum += v;
    out.has_periods = true;
    out.min = sorted.front();
    out.max = sorted.back();
    out.mean = static_cast<std::int64_t>(sum / static_cast<WideInt>(sorted.size()));
    out.median = nearest_rank(sorted, 50);
    out.p95 = nearest_rank(sorted, 95);
    out.p99 = nearest_rank(sorted, 99);
    if (out.median > 0) {
        for (auto v : sorted) {
            if (v > 3 * out.median) ++out.gaps_over_3x_median;
        }
    }
    return out;
}

std::string hex_id(std::uint32_t id, bool extended) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), extended ? "0x%08X" : "0x%03X", id);
    return buf;
}

// 12345 -> "12.345" (milli-units of an integer), no floating point.
std::string fixed3(std::int64_t v) { return format_fixed_point(v, 1000); }

std::string us_and_ms(std::int64_t us) { return std::to_string(us) + " us (" + fixed3(us) + " ms)"; }

const char* role_of(const Config& cfg, std::uint32_t id, bool extended) {
    if (extended) return "-";
    if (id == cfg.protocol.measurement_id) return "measurement";
    if (id == cfg.protocol.status_id) return "status";
    return "-";
}

void usage() {
    std::cerr << "Usage: charger_probe LOG.candump [--config FILE] [--dbc FILE] [--json]\n"
              << "  LOG.candump   a `candump -L` recording (or - for stdin); needs timestamps\n"
              << "  --config FILE configuration whose protocol profile and timeouts to compare against\n"
              << "                (the demo defaults when omitted)\n"
              << "  --dbc FILE    vendor DBC, when the configuration needs it\n"
              << "  --json        machine-readable report (charger.probe.v1)\n"
              << "Reports facts about the recording. It never suggests a threshold.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path, config_path, dbc_path;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            json = true;
        } else if (a == "--config" || a == "--dbc") {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a path\n";
                return 2;
            }
            (a == "--config" ? config_path : dbc_path) = argv[++i];
        } else if (a == "--help") {
            usage();
            return 0;
        } else if ((a.empty() || a[0] != '-' || a == "-") && log_path.empty()) {
            log_path = a;
        } else {
            std::cerr << "unrecognised argument: " << a << '\n';
            usage();
            return 2;
        }
    }
    if (log_path.empty()) {
        usage();
        return 2;
    }

    Config cfg;
    if (!config_path.empty()) {
        std::ifstream cf(config_path, std::ios::binary);
        if (!cf) {
            std::cerr << "cannot read the configuration: " << config_path << '\n';
            return 2;
        }
        std::ostringstream buf;
        buf << cf.rdbuf();
        std::string dbc_text;
        if (!dbc_path.empty()) {
            std::ifstream df(dbc_path, std::ios::binary);
            if (!df) {
                std::cerr << "cannot read the DBC: " << dbc_path << '\n';
                return 2;
            }
            std::ostringstream db;
            db << df.rdbuf();
            dbc_text = db.str();
        }
        const auto loaded = parse_config(buf.str(), dbc_text);
        if (!loaded.ok) {
            std::cerr << "invalid configuration " << config_path << ":\n";
            for (const auto& e : loaded.errors) std::cerr << "  " << e.field << ": " << e.message << '\n';
            return 2;
        }
        cfg = loaded.config;
    }

    std::ifstream file;
    std::istream* in = &std::cin;
    if (log_path != "-") {
        file.open(log_path, std::ios::binary);
        if (!file) {
            std::cerr << "cannot read the recording: " << log_path << '\n';
            return 2;
        }
        in = &file;
    }

    std::map<std::pair<bool, std::uint32_t>, IdStats> ids;  // (extended, id): std ids sort before 29-bit ones
    std::vector<std::string> ifaces;
    std::uint64_t lines = 0, unparsed = 0, frames = 0, error_frames = 0, rtr = 0, fd = 0, no_time = 0, regressions = 0;
    std::int64_t first_us = 0, last_us = 0;
    bool have_time = false;
    std::string line;
    while (std::getline(*in, line)) {
        ++lines;
        const auto r = parse_candump_line(line);
        if (!r) {
            if (line.find_first_not_of(" \t\r\n") != std::string::npos) ++unparsed;
            continue;
        }
        if (!r->has_time) {
            ++no_time;
            continue;
        }
        if (std::find(ifaces.begin(), ifaces.end(), r->iface) == ifaces.end()) ifaces.push_back(r->iface);
        if (!have_time) {
            have_time = true;
            first_us = r->time_us;
        } else if (r->time_us < last_us) {
            ++regressions;
        }
        last_us = std::max(last_us, r->time_us);
        ++frames;
        if (r->error_frame) {
            ++error_frames;
            continue;
        }
        if (r->rtr) ++rtr;
        if (r->fd) ++fd;
        auto& s = ids[{r->extended, r->id}];
        if (s.count == 0) {
            s.first_us = r->time_us;
        } else {
            // Out-of-order stamps are counted above; a period is never negative.
            s.periods.push_back(std::max<std::int64_t>(0, r->time_us - s.last_us));
        }
        s.last_us = std::max(s.last_us, r->time_us);
        ++s.count;
    }
    std::sort(ifaces.begin(), ifaces.end());
    if (!have_time) {
        std::cerr << "the recording has no timestamped frames: periods cannot be measured. Record with "
                     "`candump -L` (or `candump -ta`).\n";
        return 2;
    }
    const std::int64_t span_us = last_us - first_us;
    const std::int64_t fps_milli =
        span_us > 0 ? static_cast<std::int64_t>(static_cast<WideInt>(frames) * 1000000000 / span_us) : -1;

    struct Fact {
        const char* role;
        std::uint32_t id;
        bool seen;
        std::int64_t longest_us;
        std::int64_t timeout_ms;
    };
    std::vector<Fact> facts;
    for (const auto& role : {std::pair<const char*, std::uint32_t>{"measurement", cfg.protocol.measurement_id},
                             std::pair<const char*, std::uint32_t>{"status", cfg.protocol.status_id}}) {
        const auto it = ids.find({false, role.second});
        Fact f{role.first, role.second, it != ids.end(), 0, 0};
        if (f.seen) f.longest_us = summarise(it->second).max;
        f.timeout_ms =
            std::string(role.first) == "measurement" ? cfg.measurement_timeout.count() : cfg.status_timeout.count();
        facts.push_back(f);
    }
    const std::int64_t step_ms = cfg.max_integration_step.count();

    if (json) {
        std::cout << "{\"schema\":\"charger.probe.v1\",\"source\":\""
                  << (config_path.empty() ? "demo-defaults" : "config") << "\",\"lines\":" << lines
                  << ",\"frames\":" << frames << ",\"unparsed_lines\":" << unparsed
                  << ",\"lines_without_time\":" << no_time << ",\"clock_regressions\":" << regressions
                  << ",\"error_frames\":" << error_frames << ",\"rtr_frames\":" << rtr << ",\"fd_frames\":" << fd
                  << ",\"first_us\":" << first_us << ",\"last_us\":" << last_us << ",\"span_us\":" << span_us
                  << ",\"frames_per_second_milli\":" << fps_milli << ",\"interfaces\":[";
        for (std::size_t i = 0; i < ifaces.size(); ++i) std::cout << (i ? "," : "") << '"' << ifaces[i] << '"';
        std::cout << "],\"ids\":[";
        bool first = true;
        for (const auto& [key, st] : ids) {
            const auto sm = summarise(st);
            std::cout << (first ? "" : ",") << "{\"id\":\"" << hex_id(key.second, key.first)
                      << "\",\"extended\":" << (key.first ? "true" : "false") << ",\"role\":\""
                      << role_of(cfg, key.second, key.first) << "\",\"count\":" << st.count
                      << ",\"first_us\":" << st.first_us << ",\"last_us\":" << st.last_us;
            if (sm.has_periods) {
                std::cout << ",\"period_us\":{\"min\":" << sm.min << ",\"mean\":" << sm.mean
                          << ",\"median\":" << sm.median << ",\"p95\":" << sm.p95 << ",\"p99\":" << sm.p99
                          << ",\"max\":" << sm.max << "},\"gaps_over_3x_median\":" << sm.gaps_over_3x_median;
            } else {
                std::cout << ",\"period_us\":null,\"gaps_over_3x_median\":null";
            }
            std::cout << '}';
            first = false;
        }
        std::cout << "],\"facts\":{\"measurement_timeout_ms\":" << cfg.measurement_timeout.count()
                  << ",\"status_timeout_ms\":" << cfg.status_timeout.count()
                  << ",\"max_integration_step_ms\":" << step_ms << ",\"streams\":[";
        for (std::size_t i = 0; i < facts.size(); ++i) {
            const auto& f = facts[i];
            std::cout << (i ? "," : "") << "{\"role\":\"" << f.role << "\",\"id\":\"" << hex_id(f.id, false)
                      << "\",\"seen\":" << (f.seen ? "true" : "false") << ",\"longest_silence_us\":";
            if (f.seen)
                std::cout << f.longest_us;
            else
                std::cout << "null";
            std::cout << '}';
        }
        std::cout << "]},\"note\":\"facts only: no threshold is suggested\"}\n";
        return 0;
    }

    std::cout << "charger_probe " << VERSION << " — " << (log_path == "-" ? "stdin" : log_path) << '\n';
    std::cout << "profile      " << (config_path.empty() ? "demo defaults (no --config)" : config_path) << '\n';
    std::cout << "interfaces   ";
    for (std::size_t i = 0; i < ifaces.size(); ++i) std::cout << (i ? ", " : "") << ifaces[i];
    std::cout << '\n';
    std::cout << "recorded     " << utc_from_epoch_us(first_us) << " .. " << utc_from_epoch_us(last_us) << "  ("
              << span_us << " us)\n";
    std::cout << "frames       " << frames << " in " << lines << " lines";
    if (fps_milli >= 0) std::cout << ", " << fixed3(fps_milli) << " frames/s";
    std::cout << '\n';
    std::cout << "other        error=" << error_frames << " rtr=" << rtr << " fd=" << fd << " unparsed=" << unparsed
              << " without_time=" << no_time << " clock_regressions=" << regressions << "\n\n";

    std::cout << "ids in the recording (periods are microseconds between consecutive frames of the same id):\n";
    for (const auto& [key, st] : ids) {
        const auto sm = summarise(st);
        std::cout << hex_id(key.second, key.first) << "  " << role_of(cfg, key.second, key.first)
                  << "  count=" << st.count;
        if (sm.has_periods) {
            std::cout << "  period_us min=" << sm.min << " mean=" << sm.mean << " median=" << sm.median
                      << " p95=" << sm.p95 << " p99=" << sm.p99 << " max=" << sm.max
                      << "  gaps_over_3x_median=" << sm.gaps_over_3x_median;
        } else {
            std::cout << "  period_us n/a (one frame)";
        }
        std::cout << '\n';
    }

    std::cout << "\nWhat the bus did, next to what is configured:\n";
    for (const auto& f : facts) {
        std::cout << "  " << f.role << " " << hex_id(f.id, false) << ": ";
        if (!f.seen) {
            std::cout << "not seen in the recording\n";
            continue;
        }
        std::cout << "observed longest silence " << us_and_ms(f.longest_us) << "; your " << f.role << "_timeout is "
                  << f.timeout_ms << " ms; your max_integration_step is " << step_ms << " ms\n";
        std::cout << "    longest silence " << (f.longest_us > f.timeout_ms * 1000 ? "EXCEEDS" : "does not exceed")
                  << " the " << f.role << "_timeout; it "
                  << (f.longest_us > step_ms * 1000 ? "EXCEEDS" : "does not exceed") << " max_integration_step\n";
    }
    std::cout << "\nnote: facts only. No timeout or step is suggested: thresholds come from the cabinet's own\n"
                 "specification and an engineer, not from one recording.\n";
    return 0;
}
