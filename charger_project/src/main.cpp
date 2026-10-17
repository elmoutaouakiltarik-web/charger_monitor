#include "charger_config.hpp"
#include "charger_dbc.hpp"
#include "charger_ledger.hpp"
#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"
#include "charger_tou.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
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

void log_event(const charger::Event& e) {
    std::cerr << charger::utc_timestamp() << " level=" << charger::to_string(e.level) << " code=" << e.code
              << " message=\"" << e.message << "\"";
    const char* action = charger::recommended_action(e.code);
    if (action != nullptr && action[0] != '\0') std::cerr << " action=\"" << action << "\"";
    std::cerr << std::endl;
}

struct Options {
    bool simulate{false};
    bool replay{false};
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
    std::cout << "Usage: " << argv0 << " --simulate | --replay | <can-interface> | --help | --version [options]\n"
              << "Bay monitor: it listens, it does not command the power stage. Not a fiscal meter.\n"
              << "\nListening modes (choose exactly one)\n"
              << "  --replay               read cansend-style frames (ID#DATA) from stdin, one per line\n"
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
    if (is_action) {
        if (opt.simulate || opt.replay || !opt.iface.empty() || opt.help || opt.version) {
            error =
                "--portfolio, --verify-journal, --anchor-journal and --verify-anchor are standalone actions: do not "
                "combine with a listening mode";
            return std::nullopt;
        }
        return opt;
    }
    const int modes = static_cast<int>(opt.simulate) + static_cast<int>(opt.replay) +
                      static_cast<int>(!opt.iface.empty()) + static_cast<int>(opt.help) + static_cast<int>(opt.version);
    if (modes != 1) {
        error = "specify exactly one mode: --simulate, --replay, a CAN interface, --help or --version";
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
        if (opened && journal_enabled_ && snap.session) {
            ledger_.append_session_open(*snap.session, snap.asset_id, charger::utc_timestamp());
        }
        if (!closed) return;
        // The tariff of record travels with the session so a dispute can be
        // verified years later, even when the running configuration changed.
        const std::string tariff = charger::json_tariff_block(monitor.config().tariff);
        if (journal_enabled_ && snap.last_session) {
            ledger_.append_session_close(*snap.last_session, snap.asset_id, snap.currency, charger::utc_timestamp(),
                                         tariff);
        }
        if (jsonl_enabled_ && snap.last_session) {
            out_ << charger::json_session_line(*snap.last_session, snap.asset_id, snap.currency,
                                               charger::utc_timestamp(), tariff)
                 << '\n';
            out_.flush();
            if (!out_) std::cerr << "SESSION_LOG_FAILED\n";
        }
    }

   private:
    std::ofstream out_;
    bool jsonl_enabled_{false};
    charger::Ledger ledger_;
    bool journal_enabled_{false};
};

void publish(charger::Monitor& monitor, const std::vector<charger::Event>& events, SessionLog& log,
             charger::Clock::time_point now) {
    for (const auto& e : events) log_event(e);
    log.on_events(events, monitor, now);
    std::cout << "{\"timestamp\":\"" << charger::utc_timestamp()
              << "\",\"type\":\"snapshot\",\"data\":" << charger::json_snapshot(monitor.snapshot(now)) << "}"
              << std::endl;
}

void emit(charger::Monitor& monitor, const charger::CanFrame& frame, SessionLog& log) {
    auto events = monitor.ingest(frame);
    const auto tick_events = monitor.tick(frame.received_at);
    events.insert(events.end(), tick_events.begin(), tick_events.end());
    publish(monitor, events, log, frame.received_at);
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

void run_replay(charger::Monitor& monitor, SessionLog& log, int interval_ms) {
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
        emit(monitor, *frame, log);
        clock += step;
    }
}

void run_simulation(charger::Monitor& monitor, SessionLog& log) {
    std::cerr << "SIMULATION mode: synthetic CAN frames, no hardware is controlled. Ctrl+C to quit.\n";
    unsigned n = 0;
    while (running.load()) {
        emit(monitor, measurement_frame(48000, static_cast<std::uint16_t>(1000 + (n % 500))), log);
        emit(monitor, status_frame((n % 12 < 2) ? 1 : 2), log);
        for (int i = 0; i < 10 && running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto now = charger::Clock::now();
            auto events = monitor.tick(now);
            for (const auto& e : events) log_event(e);
            log.on_events(events, monitor, now);
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

void run_can(charger::Monitor& monitor, const std::string& iface, SessionLog& log) {
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
            for (const auto& e : events) log_event(e);
            log.on_events(events, monitor, now);
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
        emit(monitor, f, log);
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
        if (parsed->simulate) {
            run_simulation(monitor, log);
        } else if (parsed->replay) {
            run_replay(monitor, log, parsed->replay_interval_ms.value_or(0));
        } else {
#if defined(__linux__)
            run_can(monitor, parsed->iface, log);
#else
            std::cerr << "Live CAN listening requires Linux SocketCAN. Use --simulate or --replay.\n";
            return 2;
#endif
        }
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
