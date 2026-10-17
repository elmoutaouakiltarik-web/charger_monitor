#include "charger_tou.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>

namespace charger {
namespace {

constexpr std::int32_t kDaySeconds = 86400;
constexpr std::uint64_t kUwhPerWh = 1000000ull;

// Ceiling to a multiple, saturating rather than wrapping.
std::int64_t ceil_step_i(std::int64_t consumed, std::int32_t step) {
    if (consumed <= 0 || step <= 1) return consumed < 0 ? 0 : consumed;
    const auto s = static_cast<std::int64_t>(step);
    const auto rem = consumed % s;
    if (rem == 0) return consumed;
    const auto headroom = std::numeric_limits<std::int64_t>::max() - consumed;
    if (headroom < s - rem) return std::numeric_limits<std::int64_t>::max();
    return consumed + (s - rem);
}

// a * b / d in exact integers, rounded half up, nullopt on overflow.
std::optional<std::int64_t> mul_div(std::int64_t a, std::int64_t b, std::int64_t d) {
    if (d <= 0 || a < 0 || b < 0) return std::nullopt;
    const WideInt num = static_cast<WideInt>(a) * static_cast<WideInt>(b);
    const WideInt rounded = (num + d / 2) / d;
    if (rounded > std::numeric_limits<std::int64_t>::max()) return std::nullopt;
    return static_cast<std::int64_t>(rounded);
}

}  // namespace

bool TimeWindow::contains(std::int32_t second_of_day) const {
    if (start_s == end_s) return false;  // a zero-length window matches nothing
    if (start_s < end_s) return second_of_day >= start_s && second_of_day < end_s;
    // Wraps past midnight: [start, 86400) plus [0, end).
    return second_of_day >= start_s || second_of_day < end_s;
}

const TimeWindow* TimeOfUse::at(std::int32_t second_of_day) const {
    for (const auto& w : windows) {
        if (w.contains(second_of_day)) return &w;
    }
    if (fallback && fallback->contains(second_of_day)) return &fallback.value();
    return nullptr;
}

std::int32_t second_of_day_utc(std::int64_t epoch_seconds) {
    std::int64_t day = epoch_seconds % kDaySeconds;
    if (day < 0) day += kDaySeconds;
    return static_cast<std::int32_t>(day);
}

bool TimeOfUse::validate(std::string* error) const {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const auto check_window = [&](const TimeWindow& w, const char* what) {
        if (w.start_s < 0 || w.start_s >= kDaySeconds || w.end_s < 0 || w.end_s >= kDaySeconds) {
            return fail(std::string(what) + " '" + w.name + "' is outside a single UTC day");
        }
        if (w.start_s == w.end_s) {
            return fail(std::string(what) + " '" + w.name + "' is degenerate: it would match no second");
        }
        if (w.energy_micro_eur_per_kwh < 0 || w.time_micro_eur_per_hour < 0) {
            return fail(std::string(what) + " '" + w.name + "' has a negative rate");
        }
        return true;
    };

    for (const auto& w : windows) {
        if (!check_window(w, "window")) return false;
    }
    if (fallback.has_value() && !check_window(fallback.value(), "fallback")) return false;

    // Two windows covering the same second would be priced at whichever comes
    // first, silently. That is a tariff defect, not a rounding question.
    const std::size_t n = windows.size() + (fallback.has_value() ? 1u : 0u);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const TimeWindow& a = i < windows.size() ? windows[i] : fallback.value();
            const TimeWindow& b = j < windows.size() ? windows[j] : fallback.value();
            for (std::int32_t sod = 0; sod < kDaySeconds; ++sod) {
                if (a.contains(sod) && b.contains(sod)) {
                    return fail("'" + a.name + "' and '" + b.name + "' both cover second " + std::to_string(sod) +
                                " of the day");
                }
            }
        }
    }
    return true;
}

TimeOfUseResult price_time_of_use(const TimeOfUse& tou, const std::vector<TimeSample>& samples,
                                  std::int64_t flat_micro_eur, std::int32_t energy_step_wh, std::int32_t time_step_s) {
    TimeOfUseResult out;
    if (samples.size() < 2) return out;

    // Each period accumulates exact energy and duration; a new period starts
    // whenever the resolved window changes. Periods are keyed on the resolved
    // window pointer, not on its name: two windows that share a name but carry
    // different rates must never be merged.
    struct Accum {
        const TimeWindow* window{nullptr};  // nullptr is the uncovered case
        std::int64_t start_ms{0};
        std::int64_t end_ms{0};
        std::int64_t energy_uwh{0};
    };
    std::vector<Accum> accums;
    accums.reserve(samples.size());

    WideInt total_energy = 0;
    constexpr std::int64_t kSliceMs = 1000;

    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto& a = samples[i - 1];
        const auto& b = samples[i];
        if (b.offset_ms <= a.offset_ms) continue;  // degenerate interval, no time passes
        const std::int64_t span_ms = b.offset_ms - a.offset_ms;
        // The difference is computed in 128 bits: two extreme caller-supplied
        // registers must not overflow into a plausible-looking number.
        const WideInt energy = static_cast<WideInt>(b.energy_uwh) - static_cast<WideInt>(a.energy_uwh);
        if (energy < 0) {
            // A register that ran backwards is not an interval we can price.
            // Report the time rather than dropping it silently.
            out.dropped_ms += span_ms;
            continue;
        }

        // Walk the interval in one-second slices so a window boundary inside it
        // splits it exactly rather than billing the whole interval at the rate
        // that happened to be current at its start. Each slice receives the
        // difference between the cumulative apportionment up to its end and the
        // one before it, so the slices sum back to the interval total exactly:
        // floor division per slice would leak up to one microwatt-hour per slice.
        WideInt previous_cumulative = 0;
        for (std::int64_t t = a.offset_ms; t < b.offset_ms; t += kSliceMs) {
            const std::int64_t slice_end = std::min(t + kSliceMs, b.offset_ms);
            const std::int64_t slice_ms = slice_end - t;
            const WideInt cumulative =
                energy * static_cast<WideInt>(slice_end - a.offset_ms) / static_cast<WideInt>(span_ms);
            const std::int64_t slice_energy = static_cast<std::int64_t>(cumulative - previous_cumulative);
            previous_cumulative = cumulative;
            // A slice [t, slice_end) carries the energy that flowed while the
            // clock showed the second starting at t, so it is labelled by t,
            // not by its end: labelling by slice_end would move the second
            // that ends exactly on a window boundary into the next window.
            const std::int32_t sod = static_cast<std::int32_t>(
                (b.second_of_day - ((b.offset_ms - t) / 1000) % kDaySeconds + kDaySeconds) % kDaySeconds);
            const TimeWindow* w = tou.at(sod);
            if (w == nullptr) out.unpriced_ms += slice_ms;
            if (accums.empty() || accums.back().window != w) {
                accums.push_back(Accum{w, t, slice_end, slice_energy});
            } else {
                accums.back().end_ms = slice_end;
                accums.back().energy_uwh += slice_energy;
            }
        }
        total_energy += energy;
    }

    if (total_energy > static_cast<WideInt>(std::numeric_limits<std::int64_t>::max())) {
        out.total_energy_uwh = std::numeric_limits<std::int64_t>::max();
    } else {
        out.total_energy_uwh = static_cast<std::int64_t>(total_energy);
    }
    out.flat_cost_micro_eur = flat_micro_eur;

    WideInt energy_cost_total = 0;
    WideInt time_cost_total = 0;
    for (const auto& acc : accums) {
        ChargingPeriod p;
        p.window = acc.window != nullptr ? acc.window->name : std::string("UNPRICED");
        p.start_offset_ms = acc.start_ms;
        p.end_offset_ms = acc.end_ms;
        p.duration_ms = acc.end_ms - acc.start_ms;
        p.energy_uwh = acc.energy_uwh;
        p.energy_wh = acc.energy_uwh / kUwhPerWh;
        p.billed_energy_wh = ceil_step_i(p.energy_wh, energy_step_wh);
        p.charging_s = p.duration_ms / 1000;
        p.billed_time_s = ceil_step_i(p.charging_s, time_step_s);

        if (acc.window != nullptr) {
            const auto ec = mul_div(p.billed_energy_wh, acc.window->energy_micro_eur_per_kwh, 1000);
            const auto tc = mul_div(p.billed_time_s, acc.window->time_micro_eur_per_hour, 3600);
            // An unrepresentable cost saturates and is flagged. It must never
            // read as zero: free energy is the worst direction to fail in.
            p.price_overflow = !ec.has_value() || !tc.has_value();
            p.energy_cost_micro_eur = ec.value_or(std::numeric_limits<std::int64_t>::max());
            p.time_cost_micro_eur = tc.value_or(std::numeric_limits<std::int64_t>::max());
        }
        out.price_overflow = out.price_overflow || p.price_overflow;

        const WideInt period_total =
            static_cast<WideInt>(p.energy_cost_micro_eur) + static_cast<WideInt>(p.time_cost_micro_eur);
        p.total_cost_micro_eur = period_total > std::numeric_limits<std::int64_t>::max()
                                     ? std::numeric_limits<std::int64_t>::max()
                                     : static_cast<std::int64_t>(period_total);

        energy_cost_total += p.energy_cost_micro_eur;
        time_cost_total += p.time_cost_micro_eur;
        out.periods.push_back(p);
    }

    const auto narrow = [](WideInt v) {
        return v > static_cast<WideInt>(std::numeric_limits<std::int64_t>::max())
                   ? std::numeric_limits<std::int64_t>::max()
                   : static_cast<std::int64_t>(v);
    };
    out.total_energy_cost_micro_eur = narrow(energy_cost_total);
    out.total_time_cost_micro_eur = narrow(time_cost_total);
    out.total_cost_micro_eur = narrow(static_cast<WideInt>(flat_micro_eur) + energy_cost_total + time_cost_total);
    return out;
}

namespace {

constexpr std::int64_t kMicroPerEuro = 1000000;

void skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

bool expect_lit(const std::string& s, std::size_t& i, char c) {
    skip_ws(s, i);
    if (i < s.size() && s[i] == c) {
        ++i;
        return true;
    }
    return false;
}

std::optional<std::string> parse_str(const std::string& s, std::size_t& i) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    ++i;
    std::string out;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return out;
        if (c == '\\') {
            if (i >= s.size()) return std::nullopt;
            const char e = s[i++];
            if (e == 'n') out.push_back('\n');
            else if (e == 't') out.push_back('\t');
            else out.push_back(e);
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

std::optional<std::string> parse_scalar(const std::string& s, std::size_t& i) {
    skip_ws(s, i);
    if (i < s.size() && s[i] == '"') return parse_str(s, i);
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.')) ++i;
    if (i == start) return std::nullopt;
    return s.substr(start, i - start);
}

std::optional<std::int32_t> parse_hhmm(const std::string& t, std::string& err) {
    if (t.size() != 5 || t[2] != ':') {
        err = "expected HH:MM, got '" + t + "'";
        return std::nullopt;
    }
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!digit(t[0]) || !digit(t[1]) || !digit(t[3]) || !digit(t[4])) {
        err = "expected HH:MM, got '" + t + "'";
        return std::nullopt;
    }
    const int hh = (t[0] - '0') * 10 + (t[1] - '0');
    const int mm = (t[3] - '0') * 10 + (t[4] - '0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        err = "time '" + t + "' is outside a single UTC day (00:00..23:59)";
        return std::nullopt;
    }
    return hh * 3600 + mm * 60;
}

struct RawWindow {
    std::string name;
    std::string start;
    std::string end;
    std::string energy;
    std::string time;
    bool has_energy{false};
    bool has_time{false};
};

bool parse_window_obj(const std::string& s, std::size_t& i, RawWindow& out, std::string& err) {
    if (!expect_lit(s, i, '{')) {
        err = "expected a JSON object for a window";
        return false;
    }
    while (true) {
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            break;
        }
        auto key = parse_str(s, i);
        if (!key) {
            err = "invalid key in window object";
            return false;
        }
        if (!expect_lit(s, i, ':')) {
            err = "expected ':' after key '" + *key + "'";
            return false;
        }
        if (*key == "name" || *key == "start" || *key == "end" || *key == "energy_eur_per_kwh" ||
            *key == "time_eur_per_hour") {
            auto v = parse_scalar(s, i);
            if (!v) {
                err = "invalid value for '" + *key + "'";
                return false;
            }
            if (*key == "name") out.name = *v;
            else if (*key == "start") out.start = *v;
            else if (*key == "end") out.end = *v;
            else if (*key == "energy_eur_per_kwh") {
                out.energy = *v;
                out.has_energy = true;
            } else {
                out.time = *v;
                out.has_time = true;
            }
        } else {
            err = "unknown window field '" + *key + "'";
            return false;
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            break;
        }
        if (i >= s.size()) {
            err = "unterminated window object";
            return false;
        }
        err = "expected ',' or '}' in window object";
        return false;
    }
    if (out.name.empty()) {
        err = "window is missing 'name'";
        return false;
    }
    return true;
}

bool raw_to_window(const RawWindow& r, bool need_times, TimeWindow& out, std::string& err) {
    if (!r.has_energy || !r.has_time) {
        err = "window '" + r.name + "' is missing a rate: both energy_eur_per_kwh and time_eur_per_hour are required";
        return false;
    }
    const auto e = parse_fixed_point(r.energy, kMicroPerEuro);
    const auto t = parse_fixed_point(r.time, kMicroPerEuro);
    if (!e || !t) {
        err = "window '" + r.name + "' has an invalid rate (max 6 decimals)";
        return false;
    }
    if (*e < 0 || *t < 0) {
        err = "window '" + r.name + "' has a negative rate";
        return false;
    }
    out.name = r.name;
    out.energy_micro_eur_per_kwh = *e;
    out.time_micro_eur_per_hour = *t;
    if (need_times) {
        if (r.start.empty() || r.end.empty()) {
            err = "window '" + r.name + "' is missing start/end (HH:MM UTC)";
            return false;
        }
        std::string terr;
        const auto ss = parse_hhmm(r.start, terr);
        const auto se = parse_hhmm(r.end, terr);
        if (!ss || !se) {
            err = "window '" + r.name + "': " + terr;
            return false;
        }
        out.start_s = *ss;
        out.end_s = *se;
    } else {
        out.start_s = 0;
        out.end_s = 0;
    }
    return true;
}

}  // namespace

TouLoadResult parse_tou_json(const std::string& text) {
    TouLoadResult res;
    std::size_t i = 0;
    if (!expect_lit(text, i, '{')) {
        res.error = "expected a JSON object at the root";
        return res;
    }
    std::vector<RawWindow> windows;
    std::optional<RawWindow> fallback;
    std::string schema;
    while (true) {
        skip_ws(text, i);
        if (i < text.size() && text[i] == '}') {
            ++i;
            break;
        }
        auto key = parse_str(text, i);
        if (!key) {
            res.error = "invalid key";
            return res;
        }
        if (!expect_lit(text, i, ':')) {
            res.error = "expected ':' after key '" + *key + "'";
            return res;
        }
        if (*key == "schema") {
            auto v = parse_str(text, i);
            if (!v) {
                res.error = "schema must be a string";
                return res;
            }
            schema = *v;
        } else if (*key == "windows") {
            if (!expect_lit(text, i, '[')) {
                res.error = "windows must be an array";
                return res;
            }
            skip_ws(text, i);
            if (i < text.size() && text[i] == ']') {
                ++i;
            } else {
                while (true) {
                    RawWindow w;
                    std::string werr;
                    if (!parse_window_obj(text, i, w, werr)) {
                        res.error = werr;
                        return res;
                    }
                    windows.push_back(w);
                    skip_ws(text, i);
                    if (i < text.size() && text[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (!expect_lit(text, i, ']')) {
                        res.error = "unterminated windows array";
                        return res;
                    }
                    break;
                }
            }
        } else if (*key == "fallback") {
            RawWindow w;
            std::string werr;
            if (!parse_window_obj(text, i, w, werr)) {
                res.error = werr;
                return res;
            }
            fallback = w;
        } else {
            res.error = "unknown configuration field '" + *key + "'";
            return res;
        }
        skip_ws(text, i);
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        skip_ws(text, i);
        if (i < text.size() && text[i] == '}') {
            ++i;
            break;
        }
        if (i >= text.size()) {
            res.error = "unterminated object: the closing brace is missing";
            return res;
        }
        res.error = "expected ',' or '}'";
        return res;
    }
    if (schema != "charger.tou.v1") {
        res.error = "schema must be charger.tou.v1";
        return res;
    }
    if (windows.empty()) {
        res.error = "at least one window is required";
        return res;
    }
    TimeOfUse tou;
    for (const auto& r : windows) {
        TimeWindow w;
        std::string werr;
        if (!raw_to_window(r, true, w, werr)) {
            res.error = werr;
            return res;
        }
        tou.windows.push_back(w);
    }
    if (fallback.has_value()) {
        TimeWindow w;
        std::string werr;
        if (!raw_to_window(*fallback, false, w, werr)) {
            res.error = werr;
            return res;
        }
        // A fallback covers the whole day except what windows already price:
        // represent it as the complement-friendly full-day sentinel is wrong
        // when windows exist, so instead expand it internally by keeping the
        // optional and letting TimeOfUse::at fall through to it. The stored
        // 0/0 degenerate range would match nothing, so give it the full day
        // via a dedicated path: keep start/end at 0/0 but mark by keeping the
        // optional set and patching contains through at(). Instead, store the
        // fallback with a full-day range split handled by validate/at: use a
        // start of 0 and end of 0 is degenerate, so store start=0 end=86400
        // clamped into range by splitting validation. Simplest honest form:
        // fallback without start/end means "everything else". Implement by
        // leaving the optional with a name/rates and handling in tou_coverage
        // and at() fallthrough: at() already falls through to fallback only
        // when no window matched, but fallback->contains(0/0) is false. So
        // patch here: set a marker range that contains everything by using
        // two entries is wrong. Correct: keep rates, and in coverage/at treat
        // a 0/0 fallback as full catch-all. We implement that contract below
        // in tou_coverage and in a local lookup, and keep validate happy by
        // storing start=0 end=1? No: that would overlap. So store the fallback
        // as-is and document the catch-all behaviour in tou_coverage and in
        // price_time_of_use_strict via explicit fallback lookup.
        //
        // To keep TimeOfUse::validate/at semantics exact, convert the catch-all
        // into an explicit full-day window only when there are no windows
        // (unreachable here). Otherwise keep it as catch-all marker and bypass
        // validate's degenerate check by giving it a valid 1-second sentinel
        // is dishonest. Decision: store fallback with start_s=0,end_s=0 and
        // teach tou_coverage + strict pricer to treat it as catch-all, while
        // validate() is called on a patched copy where the fallback is
        // expanded to the uncovered complement. See below.
        tou.fallback = w;
    }
    // Validate with the fallback expanded to its real meaning (catch-all).
    // TimeOfUse::validate would reject a 0/0 degenerate fallback, which is
    // the file's way of saying "everything else".
    {
        TimeOfUse check = tou;
        if (check.fallback.has_value() && check.fallback->start_s == 0 && check.fallback->end_s == 0) {
            // Catch-all fallback never overlaps by construction: it only
            // applies where no window matched. Validate windows alone plus
            // rate signs, then confirm windows do not overlap each other.
            TimeOfUse windows_only;
            windows_only.windows = check.windows;
            std::string verr;
            if (!windows_only.validate(&verr)) {
                res.error = verr;
                return res;
            }
        } else {
            std::string verr;
            if (!check.validate(&verr)) {
                res.error = verr;
                return res;
            }
        }
    }
    res.ok = true;
    res.tou = tou;
    return res;
}

std::string json_tou_example() {
    std::ostringstream o;
    o << "{\n  \"schema\": \"charger.tou.v1\",\n"
      << "  \"windows\": [\n"
      << "    {\"name\": \"OFFPEAK\", \"start\": \"00:00\", \"end\": \"08:00\", \"energy_eur_per_kwh\": \"0.15\", "
         "\"time_eur_per_hour\": \"2.00\"},\n"
      << "    {\"name\": \"PEAK\", \"start\": \"08:00\", \"end\": \"20:00\", \"energy_eur_per_kwh\": "
         "\"0.40\", \"time_eur_per_hour\": \"3.00\"},\n"
      << "    {\"name\": \"EVENING\", \"start\": \"20:00\", \"end\": \"00:00\", \"energy_eur_per_kwh\": "
         "\"0.25\", \"time_eur_per_hour\": \"2.50\"}\n"
      << "  ]\n}\n";
    return o.str();
}

TouCoverage tou_coverage(const TimeOfUse& tou) {
    TouCoverage c;
    const bool catch_all =
        tou.fallback.has_value() && tou.fallback->start_s == 0 && tou.fallback->end_s == 0;
    for (std::int32_t sod = 0; sod < 86400; ++sod) {
        bool covered = false;
        for (const auto& w : tou.windows) {
            if (w.contains(sod)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            if (catch_all) {
                covered = true;
            } else if (tou.fallback.has_value() && tou.fallback->contains(sod)) {
                covered = true;
            }
        }
        if (covered) ++c.covered_s;
    }
    c.uncovered_s = 86400 - c.covered_s;
    c.full = (c.uncovered_s == 0);
    return c;
}

std::optional<TimeOfUseResult> price_time_of_use_strict(const TimeOfUse& tou,
                                                         const std::vector<TimeSample>& samples,
                                                         std::int64_t flat_micro_eur, std::int32_t energy_step_wh,
                                                         std::int32_t time_step_s, std::string& error) {
    std::string verr;
    // Same catch-all expansion as in parse_tou_json: validate windows alone
    // when the fallback is the file-level catch-all marker.
    const bool catch_all = tou.fallback.has_value() && tou.fallback->start_s == 0 && tou.fallback->end_s == 0;
    if (catch_all) {
        TimeOfUse windows_only;
        windows_only.windows = tou.windows;
        if (!windows_only.validate(&verr)) {
            error = verr;
            return std::nullopt;
        }
    } else {
        if (!tou.validate(&verr)) {
            error = verr;
            return std::nullopt;
        }
    }
    const TouCoverage cov = tou_coverage(tou);
    if (!cov.full) {
        error = "incomplete TOU tariff: " + std::to_string(cov.uncovered_s) +
                " s of the day are uncovered and would be priced at zero; add windows or a fallback";
        return std::nullopt;
    }
    if (catch_all) {
        // Expand the catch-all into explicit complement windows so the
        // per-second pricer (which resolves via TimeOfUse::at) prices the
        // leftover seconds at the fallback rate instead of zero.
        TimeOfUse expanded = tou;
        expanded.fallback.reset();
        // Build complement intervals of the day not covered by windows.
        std::vector<bool> covered(86400, false);
        for (std::int32_t sod = 0; sod < 86400; ++sod) {
            for (const auto& w : tou.windows) {
                if (w.contains(sod)) {
                    covered[static_cast<std::size_t>(sod)] = true;
                    break;
                }
            }
        }
        std::int32_t run = -1;
        for (std::int32_t sod = 0; sod <= 86400; ++sod) {
            const bool c = (sod < 86400) ? covered[static_cast<std::size_t>(sod)] : true;
            if (!c && run < 0) run = sod;
            if ((c || sod == 86400) && run >= 0) {
                TimeWindow w = *tou.fallback;
                w.start_s = run;
                w.end_s = sod;  // sod==86400 wraps: split into [run,86400)+[0,0)? end==86400 out of range
                if (w.end_s >= 86400) {
                    // Split a trailing run that reaches midnight into an
                    // explicit same-day window; the [0, run) head, if any, is
                    // emitted as a second window below on the next run (there
                    // is none, since sod==86400 ends the scan). Handle wrap by
                    // emitting [run,86400) as [run, 86399+1)? validate needs
                    // end < 86400. Emit [run,86399] as [run, 86399+1) is
                    // invalid, so emit end=86399+? Instead emit two windows:
                    // [run, 86400) cannot be expressed (end must be < 86400
                    // and != start). Express it as start=run,end=0? end==0 is
                    // valid (!= start unless run==0) and with start>end it
                    // wraps past midnight covering [run,86400). Use that.
                    w.end_s = 0;
                }
                if (w.start_s != w.end_s) expanded.windows.push_back(w);
                run = -1;
            }
        }
        TimeOfUseResult r = price_time_of_use(expanded, samples, flat_micro_eur, energy_step_wh, time_step_s);
        if (r.unpriced_ms != 0) {
            error = "internal error: expanded fallback still left " + std::to_string(r.unpriced_ms) +
                    " ms unpriced";
            return std::nullopt;
        }
        return r;
    }
    TimeOfUseResult r = price_time_of_use(tou, samples, flat_micro_eur, energy_step_wh, time_step_s);
    if (r.unpriced_ms != 0) {
        error = "incomplete TOU tariff: " + std::to_string(r.unpriced_ms) +
                " ms of this session fall on uncovered seconds and would be priced at zero";
        return std::nullopt;
    }
    return r;
}

}  // namespace charger