#include "charger_candump.hpp"
#include "charger_config.hpp"
#include "charger_dbc.hpp"
#include "charger_export.hpp"
#include "charger_ledger.hpp"
#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"
#include "charger_tou.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace {

constexpr std::int64_t kMicroPerEuro = 1000000;

std::atomic<bool> running{true};

void on_signal(int) { running.store(false); }

// A request the operator can fix by changing the command line (exit code 2),
// as opposed to a failure while running (exit code 1).
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The wall-clock label written next to every journal line and snapshot. Live it
// is today's UTC; when a recording with absolute timestamps is replayed it is the
// RECORDED UTC, so the journal says when the session happened, not when the file
// was replayed. Durations and timeouts never use it: they use the monotonic
// clock of the Monitor.
class WallClock {
   public:
    void anchor(charger::Clock::time_point monitor_time, std::int64_t epoch_us) {
        monitor_time_ = monitor_time;
        epoch_us_ = epoch_us;
        anchored_ = true;
    }

    std::string utc(charger::Clock::time_point at) const {
        if (!anchored_) return charger::utc_timestamp();
        const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(at - monitor_time_).count();
        return charger::utc_from_epoch_us(epoch_us_ + delta);
    }

    std::string utc() const { return charger::utc_timestamp(); }

   private:
    charger::Clock::time_point monitor_time_{};
    std::int64_t epoch_us_{0};
    bool anchored_{false};
};

WallClock wall;

void log_event(const charger::Event& e, std::optional<charger::Clock::time_point> at = std::nullopt) {
    std::cerr << (at ? wall.utc(*at) : wall.utc()) << " level=" << charger::to_string(e.level) << " code=" << e.code
              << " message=\"" << e.message << "\"";
    const char* action = charger::recommended_action(e.code);
    if (action != nullptr && action[0] != '\0') std::cerr << " action=\"" << action << "\"";
    std::cerr << std::endl;
}

struct Options {
    bool simulate{false};
    bool replay{false};
    std::string replay_candump;
    bool help{false};
    bool version{false};
    std::string iface;
    std::string session_log;
    std::string asset_id;
    std::string currency;
    std::optional<std::string> energy_eur_per_kwh;
    std::optional<std::string> time_eur_per_hour;
    std::optional<std::string> flat_fee_eur;
    std::optional<std::string> min_price_eur;
    std::optional<std::string> max_price_eur;
    std::optional<int> energy_step_wh;
    std::optional<int> time_step_s;
    std::optional<int> max_integration_ms;
    std::optional<int> replay_interval_ms;
    std::string journal;
    std::string snapshot_file;
    std::string metrics_file;
    std::string portfolio_from;
    std::string config_file;
    std::string dbc_file;
    bool dump_dbc{false};
    bool print_tou{false};
    bool verify_journal{false};
    bool anchor_journal{false};
    bool verify_anchor{false};
    std::string anchor_file;
    bool print_config{false};
    std::optional<std::string> stuck_at_s;
    std::optional<std::string> max_v_rate;
    std::optional<std::string> max_i_rate;
    std::optional<std::string> sag_v;
    std::optional<std::string> sag_tol;
    std::optional<std::string> hunt_band;
    // OEM DBC adoption path.
    std::optional<std::string> dbc_path;
    std::optional<std::string> dbc_voltage_signal;
    std::optional<std::string> dbc_current_signal;
    std::optional<std::string> dbc_status_signal;
};

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0
        << " --simulate | --replay | --replay-candump FILE | <can-interface> | --help | --version [options]\n"
        << "Bay monitor: it listens, it does not command the power stage. Not a fiscal meter.\n"
        << "\nListening modes (choose exactly one)\n"
        << "  --replay               read cansend-style frames (ID#DATA) from stdin, one per line\n"
        << "  --replay-candump FILE  replay a `candump -L` recording (or - for stdin) with its REAL timing and its\n"
        << "                         recorded UTC in the journal; no sleeping, as fast as possible. The default\n"
        << "                         candump format has no time: add --replay-interval-ms N for it\n"
        << "  --simulate             synthetic frames, no hardware\n"
        << "  <can-interface>        live SocketCAN interface, e.g. can0 or vcan0\n"
        << "\nPricing (OCPI 2.2.1 subset, VAT excluded)\n"
        << "  --tariff-eur-per-kwh   ENERGY price per kWh\n"
        << "  --tariff-eur-per-hour  TIME price per charging hour\n"
        << "  --flat-fee-eur         FLAT price once per session\n"
        << "  --min-price-eur        optional minimum session price\n"
        << "  --max-price-eur        optional maximum session price\n"
        << "  --energy-step-wh N     ENERGY step_size in Wh (default 1)\n"
        << "  --time-step-s N        TIME step_size in seconds (default 1)\n"
        << "\nDurability and audit\n"
        << "  --journal PATH         durable hash-chained journal, survives a crash\n"
        << "  --session-log PATH     append-only JSONL of closed sessions\n"
        << "  --verify-journal --journal PATH   verify chain integrity (exit 3 if altered)\n"
        << "  --anchor-journal --journal PATH --anchor-file PATH   pin the prefix into an external anchor file\n"
        << "  --verify-anchor --journal PATH --anchor-file PATH    prove the anchored prefix was not rewritten\n"
        << "  --portfolio PATH       site rollup across assets, rebuilt from a journal\n"
        << "  --replay-interval-ms N in --replay, advance time by N ms per line (default 0)\n"
        << "\nSupervision files (read-only exports; the monitor opens no listening socket)\n"
        << "  --snapshot-file PATH   atomically rewrite PATH with the newest snapshot (same JSON line as stdout)\n"
        << "  --metrics-file PATH    atomically rewrite PATH with Prometheus text, e.g. for the node_exporter\n"
        << "                         textfile collector (/var/lib/node_exporter/textfile/charger.prom)\n"
        << "                         Both refresh at most once per report interval, also while the bus is\n"
        << "                         silent, so a stale or disconnected bay becomes visible. Prefer tmpfs.\n"
        << "\nConfiguration\n"
        << "  --config PATH          versioned JSON configuration; CLI flags override it\n"
        << "  --dbc PATH             vendor DBC database to validate the config against\n"
        << "  --dump-dbc --dbc PATH  print a parsed vendor database\n"
        << "  --print-config         print a configuration example\n"
        << "  --print-tou            print a TOU tariff file example (charger.tou.v1)\n"
        << "  --asset-id ID          asset identity in the ledger (default demo-bay-1)\n"
        << "  --currency EUR         ISO-4217 label only (default EUR)\n"
        << "  --max-integration-ms   longest integratable sample interval (default 2000, max 3600000)\n"
        << "\nVendor DBC adoption (decode the OEM wire format, not the demo one)\n"
        << "  --dbc PATH             vendor DBC database; validated against the config\n"
        << "  --voltage-signal NAME  explicit voltage signal, rewires the decode from the DBC\n"
        << "  --current-signal NAME  explicit current signal, rewires the decode from the DBC\n"
        << "  --status-signal NAME   explicit power-stage status carrier (default: auto-match)\n"
        << "  --dump-dbc --dbc PATH  print a parsed vendor database\n"
        << "\nPredictive diagnostics (disabled unless thresholds are given)\n"
        << "  --stuck-at-s S         frozen measurement window under load\n"
        << "  --max-v-rate MV/s      maximum plausible voltage slope\n"
        << "  --max-i-rate MA/s      maximum plausible current slope\n"
        << "  --sag-v MV             nominal voltage for the sag check\n"
        << "  --sag-tol MV           tolerance allowed on the sag\n"
        << "  --hunt-band MA         current hunting band under load\n"
        << "\nInvoice automatically only sessions with billing_complete=true.\n"
        << "PARKING_TIME is not priced: this protocol cannot prove connector occupancy.\n"
        << "Demo protocol: 0x180 V/I centi-units LE, 0x181 status 0..3.\n"
        << "Output: diagnostic events on stderr, JSON snapshots on stdout.\n";
}

bool take_value(int& i, int argc, char** argv, std::string& out) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

// Overload for optional values, so a flag that is "maybe present" is read
// without a temporary at every call site.
bool take_value(int& i, int argc, char** argv, std::optional<std::string>& out) {
    if (i + 1 >= argc) return false;
    out = std::string(argv[++i]);
    return true;
}

std::optional<int> parse_positive_int(const std::string& text) {
    if (text.empty()) return std::nullopt;
    int value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        const int dig = c - '0';
        if (value > (2147483647 - dig) / 10) return std::nullopt;
        value = value * 10 + dig;
    }
    return value;
}

std::optional<Options> parse_args(int argc, char** argv, std::string& error) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") {
            opt.help = true;
        } else if (a == "--version") {
            opt.version = true;
        } else if (a == "--simulate") {
            opt.simulate = true;
        } else if (a == "--replay") {
            opt.replay = true;
        } else if (a == "--replay-candump") {
            if (!take_value(i, argc, argv, opt.replay_candump) || opt.replay_candump.empty()) {
                error = "--replay-candump needs a candump recording, or - for stdin";
                return std::nullopt;
            }
        } else if (a == "--asset-id") {
            if (!take_value(i, argc, argv, opt.asset_id)) {
                error = ": missing value for --asset-id";
                return std::nullopt;
            }
        } else if (a == "--currency") {
            if (!take_value(i, argc, argv, opt.currency)) {
                error = ": missing value for --currency";
                return std::nullopt;
            }
        } else if (a == "--session-log") {
            if (!take_value(i, argc, argv, opt.session_log)) {
                error = ": missing value for --session-log";
                return std::nullopt;
            }
        } else if (a == "--tariff-eur-per-kwh") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --tariff-eur-per-kwh";
                return std::nullopt;
            }
            opt.energy_eur_per_kwh = v;
        } else if (a == "--tariff-eur-per-hour") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --tariff-eur-per-hour";
                return std::nullopt;
            }
            opt.time_eur_per_hour = v;
        } else if (a == "--flat-fee-eur") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --flat-fee-eur";
                return std::nullopt;
            }
            opt.flat_fee_eur = v;
        } else if (a == "--min-price-eur") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --min-price-eur";
                return std::nullopt;
            }
            opt.min_price_eur = v;
        } else if (a == "--max-price-eur") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --max-price-eur";
                return std::nullopt;
            }
            opt.max_price_eur = v;
        } else if (a == "--energy-step-wh") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --energy-step-wh";
                return std::nullopt;
            }
            const auto n = parse_positive_int(v);
            if (!n) {
                error = "--energy-step-wh is not an integer";
                return std::nullopt;
            }
            opt.energy_step_wh = *n;
        } else if (a == "--time-step-s") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --time-step-s";
                return std::nullopt;
            }
            const auto n = parse_positive_int(v);
            if (!n) {
                error = "--time-step-s is not an integer";
                return std::nullopt;
            }
            opt.time_step_s = *n;
        } else if (a == "--max-integration-ms") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --max-integration-ms";
                return std::nullopt;
            }
            const auto n = parse_positive_int(v);
            if (!n) {
                error = "--max-integration-ms is not an integer";
                return std::nullopt;
            }
            opt.max_integration_ms = *n;
        } else if (a == "--replay-interval-ms") {
            std::string v;
            if (!take_value(i, argc, argv, v)) {
                error = ": missing value for --replay-interval-ms";
                return std::nullopt;
            }
            const auto n = parse_positive_int(v);
            if (!n) {
                error = "--replay-interval-ms is not an integer";
                return std::nullopt;
            }
            opt.replay_interval_ms = *n;
        } else if (a == "--journal") {
            if (!take_value(i, argc, argv, opt.journal)) {
                error = ": missing value for --journal";
                return std::nullopt;
            }
        } else if (a == "--snapshot-file") {
            if (!take_value(i, argc, argv, opt.snapshot_file) || opt.snapshot_file.empty()) {
                error = "--snapshot-file needs a non-empty path";
                return std::nullopt;
            }
        } else if (a == "--metrics-file") {
            if (!take_value(i, argc, argv, opt.metrics_file) || opt.metrics_file.empty()) {
                error = "--metrics-file needs a non-empty path";
                return std::nullopt;
            }
        } else if (a == "--portfolio") {
            if (!take_value(i, argc, argv, opt.portfolio_from)) {
                error = ": missing value for --portfolio";
                return std::nullopt;
            }
        } else if (a == "--dump-dbc") {
            opt.dump_dbc = true;
        } else if (a == "--dbc") {
            if (!take_value(i, argc, argv, opt.dbc_file)) {
                error = ": missing value for --dbc";
                return std::nullopt;
            }
        } else if (a == "--voltage-signal") {
            if (!take_value(i, argc, argv, opt.dbc_voltage_signal)) {
                error = ": missing value for --voltage-signal";
                return std::nullopt;
            }
        } else if (a == "--current-signal") {
            if (!take_value(i, argc, argv, opt.dbc_current_signal)) {
                error = ": missing value for --current-signal";
                return std::nullopt;
            }
        } else if (a == "--status-signal") {
            if (!take_value(i, argc, argv, opt.dbc_status_signal)) {
                error = ": missing value for --status-signal";
                return std::nullopt;
            }
        } else if (a == "--verify-journal") {
            opt.verify_journal = true;
        } else if (a == "--anchor-journal") {
            opt.anchor_journal = true;
        } else if (a == "--verify-anchor") {
            opt.verify_anchor = true;
        } else if (a == "--anchor-file") {
            if (!take_value(i, argc, argv, opt.anchor_file)) {
                error = ": missing value for --anchor-file";
                return std::nullopt;
            }
        } else if (a == "--config") {
            if (!take_value(i, argc, argv, opt.config_file)) {
                error = ": missing value for --config";
                return std::nullopt;
            }
        } else if (a == "--print-tou") {
            opt.print_tou = true;
        } else if (a == "--print-config") {
            opt.print_config = true;
        } else if (a == "--stuck-at-s") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --stuck-at-s";
                return std::nullopt;
            }
        } else if (a == "--max-v-rate") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --max-v-rate";
                return std::nullopt;
            }
        } else if (a == "--max-i-rate") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --max-i-rate";
                return std::nullopt;
            }
        } else if (a == "--sag-v") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --sag-v";
                return std::nullopt;
            }
        } else if (a == "--sag-tol") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --sag-tol";
                return std::nullopt;
            }
        } else if (a == "--hunt-band") {
            std::string tmp_value;
            if (!take_value(i, argc, argv, tmp_value)) {
                error = ": missing value for --hunt-band";
                return std::nullopt;
            }
        } else if (!a.empty() && a.rfind("--", 0) != 0 && opt.iface.empty()) {
            opt.iface = a;
        } else {
            error = ": unrecognised argument: " + a;
            return std::nullopt;
        }
    }
    const bool is_action = !opt.portfolio_from.empty() || opt.verify_journal || opt.print_config || opt.dump_dbc ||
                           opt.anchor_journal || opt.verify_anchor || opt.print_tou;
    const bool exports = !opt.snapshot_file.empty() || !opt.metrics_file.empty();
    if (is_action && exports) {
        error = "--snapshot-file and --metrics-file belong to a listening mode, not to a standalone action";
        return std::nullopt;
    }
    if (is_action) {
        if (opt.simulate || opt.replay || !opt.replay_candump.empty() || !opt.iface.empty() || opt.help ||
            opt.version) {
            error =
                "--portfolio, --verify-journal, --anchor-journal and --verify-anchor are standalone actions: do not "
                "combine with a listening mode";
            return std::nullopt;
        }
        return opt;
    }
    // An export must never share a file with the evidence: the journal is
    // append-only, and an atomic rewrite of it would destroy the chain.
    const std::string* targets[] = {&opt.snapshot_file, &opt.metrics_file};
    const std::string* protected_paths[] = {&opt.journal, &opt.session_log};
    for (const std::string* t : targets) {
        if (t->empty()) continue;
        for (const std::string* p : protected_paths) {
            if (*t == *p) {
                error = "an export file must not be the journal or the session log: " + *t;
                return std::nullopt;
            }
        }
    }
    if (!opt.snapshot_file.empty() && opt.snapshot_file == opt.metrics_file) {
        error = "--snapshot-file and --metrics-file must be different files";
        return std::nullopt;
    }
    const int modes = static_cast<int>(opt.simulate) + static_cast<int>(opt.replay) +
                      static_cast<int>(!opt.replay_candump.empty()) + static_cast<int>(!opt.iface.empty()) +
                      static_cast<int>(opt.help) + static_cast<int>(opt.version);
    if (modes != 1) {
        error =
            "specify exactly one mode: --simulate, --replay, --replay-candump, a CAN interface, --help or --version";
        return std::nullopt;
    }
    return opt;
}

std::int64_t require_money(const std::string& text, const char* flag) {
    const auto v = charger::parse_fixed_point(text, kMicroPerEuro);
    if (!v) throw std::runtime_error(std::string(flag) + " is not a valid decimal amount (max 6 decimals)");
    return *v;
}

std::int64_t require_milli(const std::string& text, const char* flag) {
    const auto v = charger::parse_fixed_point(text, 1000);
    if (!v || *v < 0) throw std::runtime_error(std::string(flag) + " is not a valid integer");
    return *v;
}

charger::Config make_config(const Options& opt) {
    charger::Config cfg;
    std::string dbc_text;
    const bool explicit_signals = opt.dbc_voltage_signal.has_value() && opt.dbc_current_signal.has_value();
    if (!opt.dbc_file.empty()) {
        std::ifstream dbc(opt.dbc_file, std::ios::binary);
        if (!dbc) throw std::runtime_error("Cannot open the DBC file: " + opt.dbc_file);
        std::ostringstream db;
        db << dbc.rdbuf();
        dbc_text = db.str();
    }
    // The file is the base; explicit flags override it, so an operator can fix
    // one value on a running fleet without editing every file.
    if (!opt.config_file.empty()) {
        std::ifstream in(opt.config_file, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open the configuration: " + opt.config_file);
        std::ostringstream buf;
        buf << in.rdbuf();
        const auto loaded = charger::parse_config(buf.str(), dbc_text);
        if (!loaded.ok) {
            std::string msg = "Invalid configuration in " + opt.config_file + ":";
            for (const auto& e : loaded.errors) msg += "\n  " + e.field + ": " + e.message;
            throw std::runtime_error(msg);
        }
        cfg = loaded.config;
    } else if (!dbc_text.empty() && !explicit_signals) {
        throw std::runtime_error("--dbc requires --config, or both --voltage-signal and --current-signal");
    }
    if (explicit_signals) {
        // Adoption path: the operator names the signals, the DBC supplies the
        // wire layout. Every mapping is validated exactly; anything the codec
        // cannot decode bit-exactly aborts the start instead of guessing.
        const auto db = charger::parse_dbc(dbc_text);
        if (!db.ok) {
            std::string detail = "cannot parse the vendor database";
            if (!db.errors.empty()) detail += " at line " + std::to_string(db.errors.front().first);
            throw std::runtime_error(detail);
        }
        charger::DbcBinding binding;
        std::string bind_err;
        if (explicit_signals &&
            !charger::bind_signals(db.database, *opt.dbc_voltage_signal, *opt.dbc_current_signal, binding, bind_err)) {
            throw std::runtime_error(bind_err);
        }
        if (opt.dbc_status_signal) {
            binding.status_signal = *opt.dbc_status_signal;
            binding.status_id = 0;  // resolved by the signal name
        } else if (const auto st = charger::auto_bind_status(db.database)) {
            binding.status_id = st->can_id;
            binding.status_signal = st->signal;
        }
        const auto profile = charger::profile_from_dbc(db.database, binding);
        if (!profile.ok) throw std::runtime_error(profile.error);
        if (!profile.status_bound) {
            const auto& p = profile.profile;
            if (p.status_id == p.measurement_id) {
                throw std::runtime_error(
                    "no DBC status carrier could be bound; the default status id "
                    "0x181 collides with the measurement id. Pass --status-signal "
                    "or set status_id in the config");
            }
        }
        cfg.protocol = profile.profile;
    }
    if (!opt.asset_id.empty()) cfg.asset_id = opt.asset_id;
    if (!opt.currency.empty()) cfg.currency = opt.currency;
    if (opt.energy_eur_per_kwh)
        cfg.tariff.energy_micro_eur_per_kwh = require_money(*opt.energy_eur_per_kwh, "--tariff-eur-per-kwh");
    if (opt.time_eur_per_hour)
        cfg.tariff.time_micro_eur_per_hour = require_money(*opt.time_eur_per_hour, "--tariff-eur-per-hour");
    if (opt.flat_fee_eur) cfg.tariff.flat_micro_eur = require_money(*opt.flat_fee_eur, "--flat-fee-eur");
    if (opt.min_price_eur) cfg.tariff.min_price_micro_eur = require_money(*opt.min_price_eur, "--min-price-eur");
    if (opt.max_price_eur) cfg.tariff.max_price_micro_eur = require_money(*opt.max_price_eur, "--max-price-eur");
    if (opt.energy_step_wh) cfg.tariff.energy_step_wh = *opt.energy_step_wh;
    if (opt.time_step_s) cfg.tariff.time_step_s = *opt.time_step_s;
    if (opt.max_integration_ms) cfg.max_integration_step = std::chrono::milliseconds(*opt.max_integration_ms);
    if (opt.stuck_at_s) {
        const auto v = charger::parse_fixed_point(*opt.stuck_at_s, 1000);
        if (!v) throw std::runtime_error("--stuck-at-s is not a valid decimal value");
        cfg.stuck_at_window = std::chrono::milliseconds(*v);
    }
    if (opt.max_v_rate) cfg.max_voltage_rate_mv_per_s = require_milli(*opt.max_v_rate, "--max-v-rate");
    if (opt.max_i_rate) cfg.max_current_rate_ma_per_s = require_milli(*opt.max_i_rate, "--max-i-rate");
    if (opt.sag_v) cfg.sag_voltage_mv = require_milli(*opt.sag_v, "--sag-v");
    if (opt.sag_tol) cfg.sag_tolerance_mv = require_milli(*opt.sag_tol, "--sag-tol");
    if (opt.hunt_band) cfg.current_hunt_band_ma = require_milli(*opt.hunt_band, "--hunt-band");
    return cfg;
}

class SessionLog {
   public:
    explicit SessionLog(const std::string& jsonl_path, const std::string& journal_path) {
        if (!jsonl_path.empty()) {
            out_.open(jsonl_path, std::ios::app);
            if (!out_) throw std::runtime_error("Cannot open the session log: " + jsonl_path);
            jsonl_enabled_ = true;
        }
        if (!journal_path.empty()) {
            std::string err;
            auto led = charger::Ledger::open(journal_path, true, err);
            if (!led) throw std::runtime_error(err);
            const auto rep = led->recover();
            if (!rep.chain_intact) {
                std::cerr << "WARNING: ledger chain broken at line " << rep.first_broken_line
                          << "; data beyond that point is not trustworthy\n";
            }
            for (const auto& orphan : rep.orphan_sessions) {
                std::cerr << "ORPHAN_SESSION asset=" << orphan.first << " id=" << orphan.second
                          << ": the process ended without closing the session; energy is not billable\n";
            }
            if (!rep.orphan_sessions.empty()) {
                std::cerr << ": recovered " << rep.session_closes << " closed sessions, " << rep.energy_uwh << " uWh, "
                          << rep.billable_sessions << " billable\n";
            }
            ledger_ = std::move(*led);
            journal_enabled_ = true;
        }
    }

    void on_events(const std::vector<charger::Event>& events, const charger::Monitor& monitor,
                   charger::Clock::time_point now) {
        (void)now;
        bool opened = false;
        bool closed = false;
        for (const auto& e : events) {
            if (e.code == "SESSION_OPENED") opened = true;
            if (e.code == "SESSION_CLOSED") closed = true;
        }
        if (!opened && !closed) return;
        const auto snap = monitor.snapshot(now);
        const std::string stamp = wall.utc(now);
        if (opened && journal_enabled_ && snap.session) {
            const auto before = ledger_.lines_written();
            const bool written = ledger_.append_session_open(*snap.session, snap.asset_id, stamp);
            warn_if_not_written(written, before, "session open", snap.asset_id, snap.session->id);
        }
        if (!closed) return;
        // The tariff of record travels with the session so a dispute can be
        // verified years later, even when the running configuration changed.
        const std::string tariff = charger::json_tariff_block(monitor.config().tariff);
        bool close_recorded = true;
        if (journal_enabled_ && snap.last_session) {
            const auto before = ledger_.lines_written();
            close_recorded =
                ledger_.append_session_close(*snap.last_session, snap.asset_id, snap.currency, stamp, tariff);
            warn_if_not_written(close_recorded, before, "session close", snap.asset_id, snap.last_session->id);
        }
        if (jsonl_enabled_ && snap.last_session) {
            // A close line that never reached the journal must not reach the
            // session log either, or the two records would disagree.
            if (!close_recorded) {
                std::cerr << "SESSION_LOG_SKIPPED: " << snap.asset_id << " #" << snap.last_session->id
                          << " is absent from the journal, so it is absent here too" << std::endl;
            } else {
                out_ << charger::json_session_line(*snap.last_session, snap.asset_id, snap.currency, stamp, tariff)
                     << '\n';
                out_.flush();
                if (!out_) std::cerr << "SESSION_LOG_FAILED\n";
            }
        }
    }

   private:
    // The journal is the billing record: a line that did not reach it must never
    // pass in silence (a full disk, a revoked permission).
    void warn_if_not_written(bool written, std::uint64_t lines_before, const char* what, const std::string& asset,
                             std::uint64_t id) const {
        if (written && ledger_.lines_written() != lines_before) return;
        std::cerr << "JOURNAL_WRITE_FAILED: " << what << " of " << asset << " #" << id << " did not reach "
                  << ledger_.path() << "; the session is NOT recorded in the journal" << std::endl;
    }

    std::ofstream out_;
    bool jsonl_enabled_{false};
    charger::Ledger ledger_;
    bool journal_enabled_{false};
};

// One JSON line per snapshot: the exact envelope printed on stdout and written
// to --snapshot-file, so every consumer parses a single format.
std::string snapshot_line(const charger::Snapshot& snapshot, charger::Clock::time_point at) {
    return "{\"timestamp\":\"" + wall.utc(at) +
           "\",\"type\":\"snapshot\",\"data\":" + charger::json_snapshot(snapshot) + "}";
}

// Telemetry for whatever supervises the site: the newest snapshot and/or the
// Prometheus text, rewritten atomically. Writes are throttled on the monitor's
// own clock and also happen while the bus is silent, because a bay that stopped
// talking must show up as stale in the very file a dashboard polls. A failed
// write is reported at most once a minute and never stops the monitor or
// touches billing.
class StatusExport {
   public:
    StatusExport(std::string snapshot_path, std::string metrics_path, std::chrono::milliseconds report_interval)
        : snapshot_path_(std::move(snapshot_path)),
          metrics_path_(std::move(metrics_path)),
          interval_(std::max(report_interval, std::chrono::milliseconds(100))) {}

    bool enabled() const { return !snapshot_path_.empty() || !metrics_path_.empty(); }

    void write(const charger::Monitor& monitor, charger::Clock::time_point now, bool force = false) {
        last_now_ = now;
        if (!enabled()) return;
        const bool due = force || !last_write_ || now < *last_write_ || (now - *last_write_) >= interval_;
        if (!due) return;
        last_write_ = now;
        const auto snapshot = monitor.snapshot(now);
        bool ok = true;
        if (!snapshot_path_.empty()) ok = put(snapshot_path_, snapshot_line(snapshot, now) + '\n') && ok;
        if (!metrics_path_.empty())
            ok = put(metrics_path_, charger::prometheus_metrics(snapshot, charger::VERSION,
                                                                static_cast<std::int64_t>(std::time(nullptr)))) &&
                 ok;
        if (ok && failing_) {
            failing_ = false;
            std::cerr << "EXPORT_FILE_RECOVERED: supervision files are being written again" << std::endl;
        }
    }

    // The monitor clock of the last write() call (also recorded when no export is
    // configured: the shutdown path uses it as "the loop's last moment"), or
    // `fallback` before any.
    charger::Clock::time_point last_or(charger::Clock::time_point fallback) const {
        return last_now_.value_or(fallback);
    }

   private:
    bool put(const std::string& path, const std::string& content) {
        std::string err;
        // Status files are ephemeral: no fsync, the journal is the durable record.
        if (charger::write_file_atomic(path, content, err, false)) return true;
        const auto wall = charger::Clock::now();
        if (!last_warning_ || wall - *last_warning_ >= std::chrono::seconds(60)) {
            last_warning_ = wall;
            std::cerr << "EXPORT_FILE_FAILED: " << err << " (monitoring and billing continue)" << std::endl;
        }
        failing_ = true;
        return false;
    }

    std::string snapshot_path_;
    std::string metrics_path_;
    std::chrono::milliseconds interval_;
    std::optional<charger::Clock::time_point> last_write_;
    std::optional<charger::Clock::time_point> last_now_;
    std::optional<charger::Clock::time_point> last_warning_;
    bool failing_{false};
};

void publish(charger::Monitor& monitor, const std::vector<charger::Event>& events, SessionLog& log,
             StatusExport& status, charger::Clock::time_point now, bool print = true, bool log_routine = true) {
    for (const auto& e : events) {
        if (!log_routine && (e.code == "MEASUREMENT_ACCEPTED" || e.code == "POWER_STATUS_ACCEPTED")) continue;
        log_event(e, now);
    }
    log.on_events(events, monitor, now);
    if (print) std::cout << snapshot_line(monitor.snapshot(now), now) << std::endl;
    status.write(monitor, now);
}

void emit(charger::Monitor& monitor, const charger::CanFrame& frame, SessionLog& log, StatusExport& status,
          bool print = true) {
    auto events = monitor.ingest(frame);
    const auto tick_events = monitor.tick(frame.received_at);
    events.insert(events.end(), tick_events.begin(), tick_events.end());
    publish(monitor, events, log, status, frame.received_at, print);
}

// What the bay does when it is stopped on purpose: a session still open is closed
// as INTERRUPTED instead of being left as an orphan, and the supervision files are
// written one last time so they show the closed session. Runs after every
// listening mode, also after an error in the loop, and does not look at the stop
// flag: a second signal cannot skip it.
void finish_run(charger::Monitor& monitor, SessionLog& log, StatusExport& status) {
    const auto now = status.last_or(charger::Clock::now());
    const auto events = monitor.shutdown(now);
    if (!events.empty()) publish(monitor, events, log, status, now);
    status.write(monitor, now, true);
}

charger::CanFrame measurement_frame(std::uint16_t centivolts, std::uint16_t centiamps) {
    charger::CanFrame f;
    f.id = 0x180;
    f.length = 8;
    f.received_at = charger::Clock::now();
    f.data[0] = static_cast<std::uint8_t>(centivolts & 0xff);
    f.data[1] = static_cast<std::uint8_t>(centivolts >> 8);
    f.data[2] = static_cast<std::uint8_t>(centiamps & 0xff);
    f.data[3] = static_cast<std::uint8_t>(centiamps >> 8);
    return f;
}

charger::CanFrame status_frame(std::uint8_t status) {
    charger::CanFrame f;
    f.id = 0x181;
    f.length = 1;
    f.data[0] = status;
    f.received_at = charger::Clock::now();
    return f;
}

void run_replay(charger::Monitor& monitor, SessionLog& log, StatusExport& status, int interval_ms) {
    std::cerr << "REPLAY mode: cansend-style CAN frames (ID#DATA hex, one per line) on stdin. Ctrl+C to quit.\n";
    if (interval_ms > 0) {
        std::cerr << "Simulated time: each line advances " << interval_ms << " ms, to exercise energy and pricing.\n";
    }
    auto clock = charger::Clock::now();
    const auto step = std::chrono::milliseconds(interval_ms > 0 ? interval_ms : 0);
    std::string line;
    while (running.load() && std::getline(std::cin, line)) {
        auto frame = charger::parse_hex_frame(line, clock);
        if (!frame) {
            log_event({charger::EventLevel::Warning, "REPLAY_LINE_INVALID", "line is not in ID#DATA hexadecimal form"});
            continue;
        }
        emit(monitor, *frame, log, status);
        clock += step;
    }
}

// Replays a `candump` recording with its recorded timing.
//
// Time: with absolute timestamps (candump -L, or -ta) a frame's monitor time is its
// recorded time relative to the first frame, so every dropout, every period and
// every timeout is exactly what the bus did; the journal and the snapshots are
// labelled with the RECORDED UTC. The recording is read as fast as possible, but
// the silence between two frames is walked in 200 ms idle ticks (the cadence of the
// live loop, bounded to just past the longest stream timeout), because a bay that
// went quiet closes its session at the timeout, not at the moment the next frame
// finally arrives. Without timestamps (the default candump format) every frame
// advances time by --replay-interval-ms, or the replay is refused: guessing a
// period would invent energy.
void run_replay_candump(charger::Monitor& monitor, SessionLog& log, StatusExport& status, const std::string& path,
                        std::optional<int> interval_ms) {
    std::ifstream file;
    std::istream* in = &std::cin;
    if (path != "-") {
        file.open(path, std::ios::binary);
        if (!file) throw UsageError("cannot open the candump recording: " + path);
        in = &file;
    }
    std::cerr << "REPLAY-CANDUMP mode: " << (path == "-" ? "stdin" : path)
              << ". Read-only: nothing is transmitted. Ctrl+C to quit.\n";

    using namespace std::chrono;
    const auto& cfg = monitor.config();
    const auto horizon = std::max(cfg.measurement_timeout, cfg.status_timeout) + milliseconds(1000);
    const auto idle_step = milliseconds(200);
    // Snapshots on stdout are throttled to the report interval of the recording
    // (plus every frame that raised something besides the two routine "accepted"
    // events): a day at 3 500 frames/s must not become 300 million lines. For the
    // same reason the routine "accepted" events are not logged on stderr here.
    const auto print_every = std::max(cfg.report_interval, milliseconds(1));

    std::uint64_t line_no = 0, invalid = 0, used = 0, error_frames = 0, rtr = 0, fd = 0, extended = 0, no_time = 0,
                  other_iface = 0, regressions = 0;
    std::string first_iface;
    bool decided = false, timed = false, have_first = false;
    std::int64_t base_us = 0, last_us = 0, first_us = 0;
    charger::Clock::time_point base_time{}, prev_time{};
    std::optional<charger::Clock::time_point> last_print;
    const auto step = milliseconds(interval_ms.value_or(0));

    std::string line;
    while (running.load() && std::getline(*in, line)) {
        ++line_no;
        const auto rec = charger::parse_candump_line(line);
        if (!rec) {
            if (line.find_first_not_of(" \t\r\n") != std::string::npos) ++invalid;
            continue;
        }
        if (!decided) {
            decided = true;
            timed = rec->has_time;
            if (!timed && !interval_ms) {
                throw UsageError(
                    "the recording has no timestamps (default candump format): record with `candump -L` or "
                    "`candump -ta`, or pass --replay-interval-ms N to give every frame a fixed period");
            }
        }
        if (timed && !rec->has_time) {
            ++no_time;
            continue;
        }
        if (first_iface.empty()) first_iface = rec->iface;
        if (rec->iface != first_iface) ++other_iface;

        charger::Clock::time_point at;
        if (timed) {
            std::int64_t us = rec->time_us;
            if (!have_first) {
                have_first = true;
                base_time = charger::Clock::now();
                base_us = first_us = last_us = us;
                wall.anchor(base_time, base_us);
            }
            if (us < last_us) {
                // A step of the recording host's clock: the monitor needs time to move forward.
                if (++regressions == 1) {
                    log_event({charger::EventLevel::Warning, "REPLAY_TIME_REGRESSION",
                               "timestamp at line " + std::to_string(line_no) +
                                   " goes backwards; clamped to the previous frame (further ones are counted only)"});
                }
                us = last_us;
            }
            last_us = us;
            at = base_time + microseconds(us - base_us);
        } else if (!have_first) {
            have_first = true;
            base_time = charger::Clock::now();
            at = base_time;
        } else {
            at = prev_time + step;
        }

        // Silence between frames, walked the way the live loop would walk it.
        if (have_first && used > 0) {
            const auto limit = std::min(at, prev_time + horizon);
            for (auto t = prev_time + idle_step; t < limit; t += idle_step) {
                auto events = monitor.tick(t);
                for (const auto& e : events) log_event(e, t);
                log.on_events(events, monitor, t);
                status.write(monitor, t);
            }
        }
        prev_time = at;

        if (rec->error_frame) {
            ++error_frames;
            log_event(monitor.note_transport_error("CAN_ERROR_FRAME"), at);
            continue;
        }
        if (rec->rtr || rec->fd || rec->extended) {
            (rec->rtr ? rtr : rec->fd ? fd : extended)++;
            continue;
        }
        charger::CanFrame frame;
        frame.id = rec->id;
        frame.length = rec->length;
        frame.data = rec->data;
        frame.received_at = at;
        auto events = monitor.ingest(frame);
        const auto tick_events = monitor.tick(at);
        events.insert(events.end(), tick_events.begin(), tick_events.end());
        bool notable = false;
        for (const auto& e : events) {
            if (e.code != "MEASUREMENT_ACCEPTED" && e.code != "POWER_STATUS_ACCEPTED") notable = true;
        }
        const bool print = notable || !last_print || at < *last_print || (at - *last_print) >= print_every;
        if (print) last_print = at;
        publish(monitor, events, log, status, at, print, false);
        ++used;
    }
    std::cerr << "CANDUMP_REPLAY lines=" << line_no << " frames_used=" << used << " invalid_lines=" << invalid
              << " error_frames=" << error_frames << " rtr=" << rtr << " fd=" << fd << " extended=" << extended
              << " no_time=" << no_time << " other_iface=" << other_iface << " clock_regressions=" << regressions;
    if (timed && have_first) {
        std::cerr << " first_utc=" << charger::utc_from_epoch_us(first_us)
                  << " last_utc=" << charger::utc_from_epoch_us(last_us)
                  << " recorded_ms=" << (last_us - first_us) / 1000;
    } else {
        std::cerr << " first_utc=n/a last_utc=n/a recorded_ms=n/a";
    }
    std::cerr << std::endl;
}

void run_simulation(charger::Monitor& monitor, SessionLog& log, StatusExport& status) {
    std::cerr << "SIMULATION mode: synthetic CAN frames, no hardware is controlled. Ctrl+C to quit.\n";
    unsigned n = 0;
    while (running.load()) {
        emit(monitor, measurement_frame(48000, static_cast<std::uint16_t>(1000 + (n % 500))), log, status);
        emit(monitor, status_frame((n % 12 < 2) ? 1 : 2), log, status);
        for (int i = 0; i < 10 && running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = charger::Clock::now();
            auto events = monitor.tick(now);
            for (const auto& e : events) log_event(e, now);
            log.on_events(events, monitor, now);
            status.write(monitor, now);
        }
        ++n;
    }
}

#if defined(__linux__)
int open_can(const std::string& iface) {
    const int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) throw std::runtime_error(std::string("socket(3) failed: ") + std::strerror(errno));
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        const auto e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("CAN interface not found: ") + std::strerror(e));
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const auto e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("bind(2) failed: ") + std::strerror(e));
    }
    return fd;
}

void run_can(charger::Monitor& monitor, const std::string& iface, SessionLog& log, StatusExport& status) {
    const int fd = open_can(iface);
    std::cerr << "Listening on CAN interface " << iface
              << ". Read-only monitor: it never sends power commands. Ctrl+C to quit.\n";
    while (running.load()) {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            const auto e = errno;
            ::close(fd);
            throw std::runtime_error(std::string("poll(2) failed: ") + std::strerror(e));
        }
        if (pr == 0) {
            const auto now = charger::Clock::now();
            auto events = monitor.tick(now);
            for (const auto& e : events) log_event(e, now);
            log.on_events(events, monitor, now);
            status.write(monitor, now);
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ::close(fd);
            throw std::runtime_error("CAN descriptor error");
        }
        if (!(pfd.revents & POLLIN)) continue;
        can_frame raw{};
        const auto n = ::read(fd, &raw, sizeof(raw));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            const auto e = errno;
            ::close(fd);
            throw std::runtime_error(std::string("read(2) failed: ") + std::strerror(e));
        }
        if (n != static_cast<ssize_t>(sizeof(raw))) {
            log_event({charger::EventLevel::Warning, "CAN_FRAME_SIZE", "Unexpected received frame size"});
            continue;
        }
        if ((raw.can_id & CAN_ERR_FLAG) != 0) {
            log_event(monitor.note_transport_error("CAN_ERROR_FRAME"));
            continue;
        }
        if ((raw.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) != 0) {
            log_event(
                {charger::EventLevel::Warning, "CAN_FRAME_FLAGS", "Frame esteso/RTR ignorato dal trasporto demo"});
            continue;
        }
        charger::CanFrame f;
        f.id = raw.can_id & CAN_SFF_MASK;
        f.length = raw.can_dlc;
        f.received_at = charger::Clock::now();
        if (f.length > 8) {
            log_event({charger::EventLevel::Warning, "CAN_DLC_INVALID", "DLC out of range"});
            continue;
        }
        for (std::size_t i = 0; i < f.length; ++i) f.data[i] = raw.data[i];
        emit(monitor, f, log, status);
    }
    ::close(fd);
}
#endif

}  // namespace

void run_portfolio(const std::string& path) {
    std::string err;
    auto led = charger::Ledger::open(path, false, err);
    if (!led) {
        std::cerr << "FATAL: " << err << '\n';
        return;
    }
    const auto rep = led->recover();
    if (!rep.chain_intact) {
        std::cerr << "WARNING: ledger chain broken at line " << rep.first_broken_line
                  << ". Data beyond that point is not trustworthy.\n";
        return;
    }
    const auto kpi = led->portfolio();
    if (!kpi) {
        std::cout << "{\"schema\":\"charger.portfolio.v1\",\"assets\":0,\"note\":\"empty ledger\"}\n";
        return;
    }
    std::cout << charger::json_portfolio(*kpi) << '\n';
}

int run_anchor_journal(const std::string& journal, const std::string& anchor) {
    if (journal.empty() || anchor.empty()) {
        std::cerr << "FATAL: --anchor-journal requires --journal PATH and --anchor-file PATH\n";
        return 2;
    }
    charger::JournalAnchor out;
    std::string err;
    if (!charger::write_journal_anchor(journal, anchor, out, err)) {
        std::cerr << "FATAL: " << err << '\n';
        return 1;
    }
    std::cout << "ANCHORED: " << out.lines << " lines, last_hash " << out.last_hash << " at " << out.anchored_at_utc
              << "\n  store " << anchor
              << " on append-only storage (WORM share, object-lock bucket); the journal may keep growing\n";
    return 0;
}

int run_verify_anchor(const std::string& journal, const std::string& anchor) {
    if (journal.empty() || anchor.empty()) {
        std::cerr << "FATAL: --verify-anchor requires --journal PATH and --anchor-file PATH\n";
        return 2;
    }
    std::uint64_t appended = 0;
    std::string err;
    if (!charger::verify_journal_anchor(journal, anchor, appended, err)) {
        std::cerr << "ANCHOR MISMATCH: " << err << '\n';
        return 3;
    }
    std::cout << "ANCHOR VERIFIED: prefix intact, " << appended << " line(s) appended since the anchor\n";
    return 0;
}

int run_verify_journal(const std::string& path) {
    std::string err;
    auto led = charger::Ledger::open(path, false, err);
    if (!led) {
        std::cerr << "FATAL: " << err << '\n';
        return 1;
    }
    if (!led->verify(err)) {
        std::cerr << "LEDGER TAMPERED: " << err << '\n';
        return 3;
    }
    const auto rep = led->recover();
    std::cout << "LEDGER VERIFIED: hash chain intact over " << rep.lines << " lines, " << rep.session_closes
              << " closed sessions, " << rep.billable_sessions << " billable, " << rep.energy_uwh << " uWh\n";
    if (!rep.orphan_sessions.empty()) {
        std::cout << "ORPHAN SESSIONS: " << rep.orphan_sessions.size() << " (energy not billable, manual review)\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::string error;
    const auto parsed = parse_args(argc, argv, error);
    if (!parsed) {
        std::cerr << error << "\nUsage: " << argv[0] << " --help\n";
        return 2;
    }
    if (parsed->help) {
        usage(argv[0]);
        return 0;
    }
    if (parsed->version) {
        std::cout << "charger_monitor " << charger::VERSION << "\n";
        return 0;
    }
    if (parsed->print_config) {
        std::cout << charger::json_config_example();
        return 0;
    }
    if (parsed->print_tou) {
        std::cout << charger::json_tou_example();
        return 0;
    }
    if (parsed->dump_dbc) {
        if (parsed->dbc_file.empty()) {
            std::cerr << "FATAL: --dump-dbc requires --dbc PATH\n";
            return 2;
        }
        std::ifstream dbc(parsed->dbc_file, std::ios::binary);
        if (!dbc) {
            std::cerr << "FATAL: cannot open " << parsed->dbc_file << '\n';
            return 2;
        }
        std::ostringstream buf;
        buf << dbc.rdbuf();
        std::cout << charger::describe_dbc(buf.str());
        return 0;
    }
    if (!parsed->portfolio_from.empty()) {
        run_portfolio(parsed->portfolio_from);
        return 0;
    }
    if (parsed->verify_journal) {
        if (parsed->journal.empty()) {
            std::cerr << "FATAL: --verify-journal requires --journal PATH\n";
            return 2;
        }
        return run_verify_journal(parsed->journal);
    }
    if (parsed->anchor_journal) {
        return run_anchor_journal(parsed->journal, parsed->anchor_file);
    }
    if (parsed->verify_anchor) {
        return run_verify_anchor(parsed->journal, parsed->anchor_file);
    }
    try {
        auto config = make_config(*parsed);
        auto monitor = charger::Monitor(config);
        SessionLog log(parsed->session_log, parsed->journal);
        StatusExport status(parsed->snapshot_file, parsed->metrics_file, config.report_interval);
        // The first write shows the bay as UNKNOWN before any frame has arrived.
        status.write(monitor, charger::Clock::now(), true);
        int code = 0;
        std::string failure;
        try {
            if (parsed->simulate) {
                run_simulation(monitor, log, status);
            } else if (parsed->replay) {
                run_replay(monitor, log, status, parsed->replay_interval_ms.value_or(0));
            } else if (!parsed->replay_candump.empty()) {
                run_replay_candump(monitor, log, status, parsed->replay_candump, parsed->replay_interval_ms);
            } else {
#if defined(__linux__)
                run_can(monitor, parsed->iface, log, status);
#else
                std::cerr << "Live CAN listening requires Linux SocketCAN. Use --simulate or --replay.\n";
                return 2;
#endif
            }
        } catch (const UsageError& e) {
            failure = e.what();
            code = 2;
        } catch (const std::exception& e) {
            failure = e.what();
            code = 1;
        }
        // Whatever ended the loop (a signal, the end of a recording, a bus error), a
        // session that is still open is closed and journalled before the process goes.
        finish_run(monitor, log, status);
        if (code != 0) {
            std::cerr << (code == 2 ? "ERROR: " : "FATAL: ") << failure << '\n';
            return code;
        }
    } catch (const UsageError& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
