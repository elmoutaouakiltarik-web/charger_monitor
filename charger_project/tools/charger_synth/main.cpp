// charger_synth — a reproducible, clearly SYNTHETIC multi-bay shift.
//
// Training, demos, acceptance dry-runs and the dashboard all need a journal
// that looks like a real site. Hand-written journals would not verify, and a
// recording of a real site is somebody's billing data. This tool drives the
// REAL Monitor with an injected clock and appends to the REAL Ledger with
// explicit UTC timestamps, so the result is a genuine, hash-chained journal:
// `charger_monitor --verify-journal`, `charger_settle` and `--portfolio` accept
// it exactly like a recorded one. Only the world is made up: the cars, the
// timing, the dropouts. Every figure in it was computed by the engine.
//
// The scenario is a pure function of its parameters: an integer xorshift
// generator, no <random> distributions, no wall clock. The same arguments give
// the same bytes on every machine.
//
//   charger_synth --out FILE [--seed N] [--days D] [--bays B] [--start YYYY-MM-DD]
//                 [--snapshots-out FILE.ndjson] [--snapshots-bay bay-NN]
//                 [--snapshots-day N] [--snapshots-every S] [--force]
//
// What the journal contains (all outcomes the engine can really produce):
//   clean closes (billable); a measurement hole over the integration step
//   (GAPS, held); COMMUNICATION_TIMEOUT dropouts (held, time_upper_bound);
//   single-sample aborted plug-ins (NO_INTERVAL); contactor closed with no
//   current (ZERO_ENERGY); a bay that latches POWER_STAGE_FAULT and one that
//   latches MEASUREMENT_OUT_OF_RANGE late in the run and stay down (a latched
//   fault is only cleared by a restart, which this scenario does not model);
//   and exactly one session left open by a process that died (an orphan).
//
// The tariff below is a PLACEHOLDER that exercises the arithmetic. It is not a
// market price and not a recommendation.
//
// Exit codes: 0 written, 2 usage error, 1 failure.

#include <charger_ledger.hpp>
#include <charger_monitor.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace charger;

namespace {

constexpr std::int64_t kPeriodS = 10;  // one measurement and one status frame every 10 s
constexpr std::int64_t kHourS = 3600;
constexpr std::int64_t kDayS = 86400;

// ------------------------------------------------------------------ calendar
// Howard Hinnant's civil-date algorithms: explicit, no libc timezone state.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool leap(std::int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool parse_date(const std::string& s, std::int64_t& epoch_days) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    int v[3] = {0, 0, 0};
    const std::size_t pos[3] = {0, 5, 8};
    const std::size_t len[3] = {4, 2, 2};
    for (int i = 0; i < 3; ++i) {
        for (std::size_t k = 0; k < len[i]; ++k) {
            const char c = s[pos[i] + k];
            if (c < '0' || c > '9') return false;
            v[i] = v[i] * 10 + (c - '0');
        }
    }
    static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (v[0] < 1970 || v[0] > 2100 || v[1] < 1 || v[1] > 12) return false;
    const int limit = mdays[v[1] - 1] + (v[1] == 2 && leap(v[0]) ? 1 : 0);
    if (v[2] < 1 || v[2] > limit) return false;
    epoch_days = days_from_civil(v[0], static_cast<unsigned>(v[1]), static_cast<unsigned>(v[2]));
    return true;
}

std::string format_utc(std::int64_t epoch_s) {
    const std::int64_t days = epoch_s >= 0 ? epoch_s / kDayS : -((-epoch_s + kDayS - 1) / kDayS);
    const std::int64_t sod = epoch_s - days * kDayS;
    std::int64_t y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, y, m, d);
    // Bounded ints keep the compiler's format-truncation analysis honest.
    const int hh = static_cast<int>(sod / 3600) % 24;
    const int mm = static_cast<int>(sod % 3600 / 60) % 60;
    const int ss = static_cast<int>(sod % 60);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(y) % 10000, m % 100, d % 100, hh,
                  mm, ss);
    return buf;
}

// --------------------------------------------------------------------- RNG
// xorshift64*: tiny, fast, and identical everywhere.
class Rng {
   public:
    explicit Rng(std::uint64_t seed) {
        // splitmix64 spreads small seeds so bay 1 and bay 2 do not correlate.
        std::uint64_t z = seed + 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        state_ = z ^ (z >> 31);
        if (state_ == 0) state_ = 0x2545f4914f6cdd1dull;
    }
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545f4914f6cdd1dull;
    }
    // Uniform enough for a scenario; the modulo bias is irrelevant at these sizes.
    std::int64_t below(std::int64_t n) {
        return static_cast<std::int64_t>((next() >> 11) % static_cast<std::uint64_t>(n));
    }
    std::int64_t between(std::int64_t lo, std::int64_t hi) { return lo + below(hi - lo + 1); }

   private:
    std::uint64_t state_;
};

// ----------------------------------------------------------------- scenario
enum class Outcome { Clean, Gaps, Timeout, NoInterval, ZeroEnergy, PowerStageFault, OutOfRange, Orphan };

struct Planned {
    std::int64_t start_s{0};  // seconds since the scenario start, on the 10 s grid
    Outcome outcome{Outcome::Clean};
    std::int64_t samples{0};   // grid steps of CHARGING after the opening frame
    std::int64_t v0_cv{0};     // centivolts at the start of the charge
    std::int64_t vend_cv{0};   // centivolts at the end
    std::int64_t icc_ca{0};    // constant-current plateau, centiamps
    std::int64_t silent_s{0};  // dropout length for Timeout
    bool ends_ready{false};    // STATUS_READY instead of STATUS_OFF
};

struct Params {
    std::string out;
    std::uint64_t seed{1};
    int days{7};
    int bays{5};
    std::string start{"2026-10-05"};
    std::string snapshots_out;
    std::string snapshots_bay;
    std::optional<int> snapshots_day;
    int snapshots_every{60};
    bool force{false};
};

struct LedgerEvent {
    std::int64_t at_s{0};
    int bay{0};
    std::uint32_t seq{0};
    bool close{false};
    SessionRecord rec;
};

std::string bay_name(int bay) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "bay-%02d", bay + 1);
    return buf;
}

// Arrival weight per hour of day, percent chance per bay-hour: quiet at night,
// busy 07-10 and 17-21 UTC.
int hour_weight(int hod) {
    if (hod < 6) return 4;
    if (hod == 6) return 15;
    if (hod < 10) return 45;
    if (hod < 17) return 25;
    if (hod < 22) return 50;
    return 12;
}

Outcome forced_outcome(int bay, int bays, std::size_t ordinal) {
    // Guarantees every quality shows up even in a short run: a fixed session
    // ordinal on a fixed bay. A one-bay run carries all four on bay-01.
    if (bays == 1) {
        switch (ordinal) {
            case 3:
                return Outcome::Gaps;
            case 5:
                return Outcome::Timeout;
            case 7:
                return Outcome::NoInterval;
            case 9:
                return Outcome::ZeroEnergy;
            default:
                return Outcome::Clean;
        }
    }
    if (bay == 0 && ordinal == 3) return Outcome::Gaps;
    if (bay == 0 && ordinal == 5) return Outcome::NoInterval;
    if (bay == 1 && ordinal == 4) return Outcome::Timeout;
    if (bay == 1 && ordinal == 6) return Outcome::ZeroEnergy;
    return Outcome::Clean;
}

std::vector<Planned> plan_bay(const Params& p, int bay, std::int64_t total_s) {
    Rng rng(p.seed * 1000003ull + static_cast<std::uint64_t>(bay) * 7919ull + 1);
    const int popularity = 120 - 12 * (bay % 8);  // bay-01 is the busiest
    std::vector<Planned> out;
    std::int64_t free_at = 0;
    for (std::int64_t h = 0; h * kHourS < total_s; ++h) {
        const int chance = hour_weight(static_cast<int>(h % 24)) * popularity / 100;
        if (rng.below(100) >= chance) continue;
        Planned s;
        const std::int64_t arrival = h * kHourS + rng.below(kHourS);
        s.start_s = std::max(arrival, free_at) / kPeriodS * kPeriodS + kPeriodS;
        const std::int64_t roll = rng.below(1000);
        if (roll < 30) {
            s.outcome = Outcome::Gaps;
        } else if (roll < 60) {
            s.outcome = Outcome::Timeout;
        } else if (roll < 90) {
            s.outcome = Outcome::NoInterval;
        } else if (roll < 105) {
            s.outcome = Outcome::ZeroEnergy;
        }
        const Outcome forced = forced_outcome(bay, p.bays, out.size() + 1);
        if (forced != Outcome::Clean) s.outcome = forced;
        s.v0_cv = rng.between(34000, 41000);
        s.vend_cv = rng.between(44000, 48500);
        s.icc_ca = rng.between(6000, 18500);
        s.samples = rng.between(12, 55) * 60 / kPeriodS;
        s.ends_ready = rng.below(4) == 0;
        s.silent_s = rng.between(12, 36) * kPeriodS;
        if (s.outcome == Outcome::NoInterval) s.samples = 1;
        if (s.outcome == Outcome::ZeroEnergy) s.samples = rng.between(6, 18);
        const std::int64_t span = (s.samples + 1) * kPeriodS + (s.outcome == Outcome::Timeout ? s.silent_s : 0);
        if (s.start_s + span >= total_s) break;
        free_at = s.start_s + span + 120;
        out.push_back(s);
    }
    return out;
}

// -------------------------------------------------------------- frame helpers
CanFrame measurement(std::int64_t cv, std::int64_t ca, Clock::time_point at) {
    CanFrame f;
    f.id = 0x180;
    f.length = 8;
    f.received_at = at;
    f.data[0] = static_cast<std::uint8_t>(cv & 0xff);
    f.data[1] = static_cast<std::uint8_t>((cv >> 8) & 0xff);
    f.data[2] = static_cast<std::uint8_t>(ca & 0xff);
    f.data[3] = static_cast<std::uint8_t>((ca >> 8) & 0xff);
    return f;
}

CanFrame status(std::uint8_t value, Clock::time_point at) {
    CanFrame f;
    f.id = 0x181;
    f.length = 1;
    f.data[0] = value;
    f.received_at = at;
    return f;
}

// PLACEHOLDER tariff: it exercises ENERGY, TIME, FLAT and both step sizes. It is
// not a market price.
Config bay_config(int bay) {
    Config cfg;
    cfg.asset_id = bay_name(bay);
    cfg.currency = "EUR";
    cfg.tariff.energy_micro_eur_per_kwh = 400000;  // 0.40 EUR/kWh
    cfg.tariff.time_micro_eur_per_hour = 2400000;  // 2.40 EUR/h
    cfg.tariff.flat_micro_eur = 500000;            // 0.50 EUR per session
    cfg.tariff.energy_step_wh = 10;
    cfg.tariff.time_step_s = 60;
    cfg.measurement_timeout = std::chrono::seconds(30);
    cfg.status_timeout = std::chrono::seconds(30);
    cfg.max_integration_step = std::chrono::seconds(12);
    return cfg;
}

// ------------------------------------------------------------ snapshot capture
class Capture {
   public:
    Capture(const std::string& path, std::int64_t from_s, std::int64_t to_s, std::int64_t every_s,
            std::int64_t start_epoch)
        : from_(from_s), to_(to_s), every_(every_s), epoch_(start_epoch) {
        if (!path.empty()) out_.open(path, std::ios::binary | std::ios::trunc);
    }
    bool ok() const { return !out_.is_open() || static_cast<bool>(out_); }
    bool enabled() const { return out_.is_open(); }
    std::uint64_t lines() const { return lines_; }
    std::uint64_t dropouts() const { return dropouts_; }

    // Writes a line on the cadence, and always when something happened: a state
    // change or a session event is exactly what a replay viewer wants to see.
    void consider(const Monitor& m, std::int64_t t, Clock::time_point now, bool event) {
        if (!out_.is_open() || t < from_ || t >= to_) return;
        const Snapshot snap = m.snapshot(now);
        const std::string state = to_string(snap.state);
        const bool due = (t - from_) % every_ == 0 || event || state != last_state_;
        if (!due || t == last_t_) return;
        if (state == "DISCONNECTED" && last_state_ != state) ++dropouts_;
        last_state_ = state;
        last_t_ = t;
        ++lines_;
        out_ << "{\"timestamp\":\"" << format_utc(epoch_ + t)
             << "\",\"type\":\"snapshot\",\"data\":" << json_snapshot(snap) << "}\n";
    }

   private:
    std::ofstream out_;
    std::int64_t from_, to_, every_, epoch_;
    std::string last_state_;
    std::int64_t last_t_{-1};
    std::uint64_t lines_{0};
    std::uint64_t dropouts_{0};
};

// -------------------------------------------------------------- bay simulation
class BaySim {
   public:
    BaySim(int bay, Clock::time_point base, Capture* capture, std::vector<LedgerEvent>& events)
        : bay_(bay), base_(base), monitor_(bay_config(bay)), capture_(capture), events_(events) {}

    bool dead() const { return dead_; }

    Clock::time_point at(std::int64_t t) const { return base_ + std::chrono::seconds(t); }

    // Feeds the engine exactly like the daemon does after each frame: ingest,
    // then tick, then look at what the engine says happened.
    void feed(std::int64_t t, const std::optional<CanFrame>& m, const std::optional<CanFrame>& s) {
        const auto now = at(t);
        std::vector<Event> events;
        if (m) {
            auto e = monitor_.ingest(*m);
            events.insert(events.end(), e.begin(), e.end());
        }
        if (s) {
            auto e = monitor_.ingest(*s);
            events.insert(events.end(), e.begin(), e.end());
        }
        auto tick = monitor_.tick(now);
        events.insert(events.end(), tick.begin(), tick.end());
        record(events, t, now);
    }

    void tick_only(std::int64_t t) {
        const auto now = at(t);
        record(monitor_.tick(now), t, now);
    }

    void idle(std::int64_t t) { feed(t, measurement(0, 0, at(t)), status(1, at(t))); }

    // One session from the opening frame to its end. Returns the last grid time used.
    std::int64_t run(const Planned& s, Rng& rng, std::int64_t start_t) {
        const std::int64_t n = s.samples;
        std::int64_t t = start_t;
        const auto charging_frames = [&](std::int64_t k) {
            const std::int64_t permille = n > 0 ? k * 1000 / n : 1000;
            std::int64_t v = s.v0_cv + (s.vend_cv - s.v0_cv) * permille / 1000;
            std::int64_t i = s.icc_ca;
            if (permille > 800) i = i * (1000 - (permille - 800) * 47 / 10) / 1000;
            const std::int64_t ramp = std::min<std::int64_t>(1000, 100 + 450 * k);
            i = i * ramp / 1000;
            if (s.outcome == Outcome::ZeroEnergy) i = 0;
            v += rng.between(-5, 5);
            i += i > 50 ? rng.between(-10, 10) : 0;
            return std::make_pair(std::clamp<std::int64_t>(v, 0, 50000), std::clamp<std::int64_t>(i, 0, 20000));
        };

        for (std::int64_t k = 0; k <= n; ++k, t += kPeriodS) {
            const auto frames = charging_frames(k);
            const auto m = measurement(frames.first, frames.second, at(t));
            const std::int64_t last = n;
            if (k == 0) {
                feed(t, m, status(2, at(t)));  // opens the session
                continue;
            }
            if (s.outcome == Outcome::Orphan && k == std::max<std::int64_t>(2, n / 2)) {
                dead_ = true;  // the process dies here; the open line stays in the journal
                return t;
            }
            if (s.outcome == Outcome::NoInterval) {
                // Unplugged before a second sample: close on the status alone.
                feed(t, std::nullopt, status(s.ends_ready ? 1 : 0, at(t)));
                return t;
            }
            if (s.outcome == Outcome::Gaps && k == std::max<std::int64_t>(2, n / 2)) {
                continue;  // one frame pair never arrives: 20 s between samples, over the 12 s step
            }
            if (s.outcome == Outcome::Timeout && k == std::max<std::int64_t>(2, n / 2)) {
                // The bus goes silent. The engine notices only through its own timeout.
                const std::int64_t last_frame = t - kPeriodS;
                for (std::int64_t sec = last_frame + 1; sec < last_frame + s.silent_s; ++sec) tick_only(sec);
                return last_frame + s.silent_s - kPeriodS;
            }
            if (s.outcome == Outcome::PowerStageFault && k == std::max<std::int64_t>(2, n / 2)) {
                feed(t, m, status(3, at(t)));
                dead_ = true;  // a latched fault stays until the process is restarted
                return t;
            }
            if (s.outcome == Outcome::OutOfRange && k == std::max<std::int64_t>(2, n / 2)) {
                feed(t, measurement(52000, frames.second, at(t)), std::nullopt);  // 520 V, over the 500 V limit
                dead_ = true;
                return t;
            }
            if (k == last) {
                feed(t, m, status(s.ends_ready ? 1 : 0, at(t)));
            } else {
                feed(t, m, status(2, at(t)));
            }
        }
        return t - kPeriodS;
    }

   private:
    void record(const std::vector<Event>& events, std::int64_t t, Clock::time_point now) {
        bool event = false;
        std::optional<Snapshot> snap;
        for (const Event& e : events) {
            const bool opened = e.code == "SESSION_OPENED";
            const bool closed = e.code == "SESSION_CLOSED";
            if (!opened && !closed) continue;
            event = true;
            if (!snap) snap = monitor_.snapshot(now);
            LedgerEvent le;
            le.at_s = t;
            le.bay = bay_;
            le.seq = seq_++;
            le.close = closed;
            if (opened && snap->session) le.rec = *snap->session;
            if (closed && snap->last_session) le.rec = *snap->last_session;
            events_.push_back(std::move(le));
        }
        if (capture_ != nullptr) capture_->consider(monitor_, t, now, event);
    }

    int bay_;
    Clock::time_point base_;
    Monitor monitor_;
    Capture* capture_;
    std::vector<LedgerEvent>& events_;
    std::uint32_t seq_{0};
    bool dead_{false};
};

// ------------------------------------------------------------------------ CLI
bool parse_int(const std::string& s, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    std::int64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

bool file_exists(const std::string& path) { return static_cast<bool>(std::ifstream(path, std::ios::binary)); }

void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " --out FILE [--seed N] [--days D] [--bays B] [--start YYYY-MM-DD]\n"
              << "          [--snapshots-out FILE.ndjson] [--snapshots-bay bay-NN] [--snapshots-day N]\n"
              << "          [--snapshots-every S] [--force]\n"
              << "Writes a reproducible SYNTHETIC journal produced by the real engine. Defaults: seed 1, 7 days,\n"
              << "5 bays, start 2026-10-05, snapshots from bay-02 on day 1 every 60 s plus every event.\n";
}

bool parse_args(int argc, char** argv, Params& p, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                err = a + " needs a value";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        std::int64_t n = 0;
        if (a == "--force") {
            p.force = true;
        } else if (a == "--out") {
            if (!value(p.out)) return false;
        } else if (a == "--snapshots-out") {
            if (!value(p.snapshots_out)) return false;
        } else if (a == "--snapshots-bay") {
            if (!value(p.snapshots_bay)) return false;
        } else if (a == "--start") {
            if (!value(p.start)) return false;
        } else if (a == "--seed") {
            if (!value(v)) return false;
            if (!parse_int(v, 0, 999999999999999ll, n)) {
                err = "--seed must be a non-negative integer";
                return false;
            }
            p.seed = static_cast<std::uint64_t>(n);
        } else if (a == "--days") {
            if (!value(v)) return false;
            if (!parse_int(v, 1, 31, n)) {
                err = "--days must be between 1 and 31";
                return false;
            }
            p.days = static_cast<int>(n);
        } else if (a == "--bays") {
            if (!value(v)) return false;
            if (!parse_int(v, 1, 16, n)) {
                err = "--bays must be between 1 and 16";
                return false;
            }
            p.bays = static_cast<int>(n);
        } else if (a == "--snapshots-day") {
            if (!value(v)) return false;
            if (!parse_int(v, 0, 30, n)) {
                err = "--snapshots-day must be between 0 and 30";
                return false;
            }
            p.snapshots_day = static_cast<int>(n);
        } else if (a == "--snapshots-every") {
            if (!value(v)) return false;
            if (!parse_int(v, 10, 3600, n) || n % kPeriodS != 0) {
                err = "--snapshots-every must be a multiple of 10 between 10 and 3600 seconds";
                return false;
            }
            p.snapshots_every = static_cast<int>(n);
        } else {
            err = "unrecognised argument: " + a;
            return false;
        }
    }
    if (p.out.empty()) {
        err = "--out FILE is required";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Params p;
    std::string err;
    if (!parse_args(argc, argv, p, err)) {
        std::cerr << err << '\n';
        usage(argv[0]);
        return 2;
    }
    std::int64_t start_days = 0;
    if (!parse_date(p.start, start_days)) {
        std::cerr << "--start must be a calendar date YYYY-MM-DD\n";
        return 2;
    }
    const std::int64_t start_epoch = start_days * kDayS;
    const std::int64_t total_s = static_cast<std::int64_t>(p.days) * kDayS;

    int capture_bay = p.bays >= 2 ? 1 : 0;
    if (!p.snapshots_bay.empty()) {
        capture_bay = -1;
        for (int b = 0; b < p.bays; ++b) {
            if (bay_name(b) == p.snapshots_bay) capture_bay = b;
        }
        if (capture_bay < 0) {
            std::cerr << "--snapshots-bay " << p.snapshots_bay << " is not one of the " << p.bays << " bays\n";
            return 2;
        }
    }
    const int capture_day = p.snapshots_day.value_or(std::min(1, p.days - 1));
    if (capture_day >= p.days) {
        std::cerr << "--snapshots-day is outside the " << p.days << "-day scenario\n";
        return 2;
    }
    if (p.snapshots_out.empty() && (!p.snapshots_bay.empty() || p.snapshots_day)) {
        std::cerr << "--snapshots-bay and --snapshots-day need --snapshots-out\n";
        return 2;
    }
    for (const std::string* path : {&p.out, &p.snapshots_out}) {
        if (!path->empty() && file_exists(*path) && !p.force) {
            std::cerr << "refusing to overwrite " << *path << " (pass --force)\n";
            return 2;
        }
    }
    if (!p.snapshots_out.empty() && p.snapshots_out == p.out) {
        std::cerr << "--out and --snapshots-out must be different files\n";
        return 2;
    }
    if (file_exists(p.out) && std::remove(p.out.c_str()) != 0) {
        std::cerr << "cannot replace " << p.out << '\n';
        return 1;
    }

    // The bay and day that always carry a showcase dropout (see below).
    const int showcase_bay = p.bays >= 2 ? 1 : 0;
    const std::int64_t showcase_day = std::min(1, p.days - 1);

    // Who goes down, and who dies mid-session. Bays fold onto the ones that exist.
    const int psf_bay = p.bays >= 4 ? 3 : (p.bays >= 2 ? 1 : -1);
    const int oor_bay = p.bays >= 5 ? 1 : -1;
    const int orphan_bay = p.bays >= 3 ? 2 : 0;

    // A fixed base keeps the engine's steady clock independent of the machine.
    const Clock::time_point base = Clock::time_point{} + std::chrono::hours(1);
    std::vector<LedgerEvent> events;
    std::uint64_t capture_lines = 0, capture_dropouts = 0;

    for (int b = 0; b < p.bays; ++b) {
        std::vector<Planned> plan = plan_bay(p, b, total_s);

        const auto cut_after = [&](double fraction, Outcome outcome) {
            const std::int64_t threshold = static_cast<std::int64_t>(static_cast<double>(total_s) * fraction);
            std::size_t pick = plan.size();
            for (std::size_t i = 0; i < plan.size(); ++i) {
                if (plan[i].start_s >= threshold) {
                    pick = i;
                    break;
                }
            }
            if (pick == plan.size() && !plan.empty()) pick = plan.size() - 1;
            if (pick == plan.size()) return;
            plan[pick].outcome = outcome;
            if (plan[pick].samples < 4) plan[pick].samples = 8;
            plan.resize(pick + 1);  // the bay does nothing after it stops
        };
        if (b == psf_bay) cut_after(0.78, Outcome::PowerStageFault);
        if (b == oor_bay) cut_after(0.88, Outcome::OutOfRange);
        if (b == orphan_bay) {
            if (!plan.empty()) {
                plan.back().outcome = Outcome::Orphan;
                if (plan.back().samples < 4) plan.back().samples = 8;
            }
        }
        if (b == showcase_bay) {
            // The showcase day always contains a dropout, whether or not it is
            // captured, so the journal never depends on the snapshot options.
            std::vector<std::size_t> today;
            bool has_timeout = false;
            for (std::size_t i = 0; i < plan.size(); ++i) {
                if (plan[i].start_s / kDayS != showcase_day) continue;
                today.push_back(i);
                if (plan[i].outcome == Outcome::Timeout) has_timeout = true;
            }
            if (!has_timeout && !today.empty()) {
                const std::size_t i = today[std::min<std::size_t>(2, today.size() - 1)];
                if (plan[i].outcome == Outcome::Clean || plan[i].outcome == Outcome::Gaps) {
                    plan[i].outcome = Outcome::Timeout;
                    if (plan[i].samples < 6) plan[i].samples = 12;
                }
            }
        }

        Capture capture(b == capture_bay ? p.snapshots_out : std::string(), capture_day * kDayS,
                        (capture_day + 1) * kDayS, p.snapshots_every, start_epoch);
        if (!capture.ok()) {
            std::cerr << "cannot write " << p.snapshots_out << '\n';
            return 1;
        }
        BaySim sim(b, base, capture.enabled() ? &capture : nullptr, events);
        Rng rng(p.seed * 7777ull + static_cast<std::uint64_t>(b) * 104729ull + 3);
        std::size_t next = 0;
        std::int64_t t = 0;
        while (t <= total_s && !sim.dead()) {
            if (next < plan.size() && plan[next].start_s <= t) {
                t = sim.run(plan[next], rng, std::max(t, plan[next].start_s)) + kPeriodS;
                ++next;
                continue;
            }
            sim.idle(t);
            t += kPeriodS;
        }
        if (b == capture_bay && !capture.ok()) {
            std::cerr << "cannot write " << p.snapshots_out << '\n';
            return 1;
        }
        if (capture.enabled()) {
            capture_lines = capture.lines();
            capture_dropouts = capture.dropouts();
        }
    }

    // One journal, one global order: the file is appended chronologically.
    std::stable_sort(events.begin(), events.end(), [](const LedgerEvent& a, const LedgerEvent& b) {
        if (a.at_s != b.at_s) return a.at_s < b.at_s;
        if (a.bay != b.bay) return a.bay < b.bay;
        return a.seq < b.seq;
    });

    std::string lerr;
    auto ledger = Ledger::open(p.out, false, lerr);
    if (!ledger) {
        std::cerr << "cannot open " << p.out << ": " << lerr << '\n';
        return 1;
    }
    const std::string tariff = json_tariff_block(bay_config(0).tariff);
    for (const LedgerEvent& e : events) {
        const std::string asset = bay_name(e.bay);
        const std::string stamp = format_utc(start_epoch + e.at_s);
        if (e.close) {
            ledger->append_session_close(e.rec, asset, "EUR", stamp, tariff);
        } else {
            ledger->append_session_open(e.rec, asset, stamp);
        }
    }
    if (ledger->lines_written() != events.size() || !ledger->verify(lerr)) {
        std::cerr << "the journal was not written completely: " << lerr << '\n';
        return 1;
    }

    // Deterministic summary, from the journal itself.
    auto reread = Ledger::open(p.out, false, lerr);
    const RecoveryReport rep = reread->recover();
    std::map<std::string, std::uint64_t> quality, reasons;
    for (const auto& s : reread->sessions()) {
        ++quality[s.record.quality];
        ++reasons[s.record.stop_reason];
    }
    std::cerr << "SYNTHETIC journal " << p.out << " (seed " << p.seed << ", " << p.days << " days from " << p.start
              << ", " << p.bays << " bays)\n"
              << "  lines " << rep.lines << ", closed sessions " << rep.session_closes << ", billable "
              << rep.billable_sessions << ", orphans " << rep.orphan_sessions.size() << ", chain "
              << (rep.chain_intact ? "intact" : "BROKEN") << "\n"
              << "  energy " << format_fixed_point(static_cast<std::int64_t>(rep.energy_uwh / 1000000ull), 1000)
              << " kWh, value " << format_fixed_point(static_cast<std::int64_t>(rep.revenue_micro_eur), 1000000)
              << " EUR (PLACEHOLDER tariff, excl. VAT)\n";
    if (!p.snapshots_out.empty()) {
        std::cerr << "  snapshots " << bay_name(capture_bay) << " day " << capture_day << ": " << capture_lines
                  << " lines, " << capture_dropouts << " dropout episode(s)\n";
    }
    for (const auto& q : quality) std::cerr << "  quality " << q.first << ": " << q.second << '\n';
    for (const auto& r : reasons) std::cerr << "  stop " << r.first << ": " << r.second << '\n';
    return rep.chain_intact ? 0 : 1;
}
