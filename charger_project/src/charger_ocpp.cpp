#include "charger_ocpp.hpp"

#include <iomanip>
#include <limits>
#include <sstream>

#include "charger_monitor.hpp"

namespace charger {
namespace {

constexpr std::uint64_t kMaxExactJsonInteger = 9007199254740992ull;  // 2^53
constexpr std::size_t kMaxTransactionIdLength = 36;                  // OCPP 2.0.1 TransactionType.transactionId

std::string json_escape_local(const std::string& s) {
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

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Howard Hinnant's civil-date algorithms: proleptic Gregorian, no libc timezone state.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = floor_div(y, 400);
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = floor_div(z, 146097);
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

bool is_leap(std::int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned days_in_month(std::int64_t y, unsigned m) {
    static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) return 29;
    return kDays[m - 1];
}

std::string two(unsigned v) {
    std::string s = std::to_string(v);
    return s.size() < 2 ? "0" + s : s;
}

// One OCPP meterValue entry: the session-relative register in uWh.
std::string meter_value_json(const std::string& timestamp, const char* context, std::uint64_t micro_wh) {
    std::ostringstream o;
    o << "[{\"timestamp\":\"" << timestamp << "\",\"sampledValue\":[{\"value\":" << micro_wh << ",\"context\":\""
      << context << "\",\"measurand\":\"Energy.Active.Import.Register\",\"unitOfMeasure\":{\"unit\":\"Wh\","
      << "\"multiplier\":-6}}]}]";
    return o.str();
}

}  // namespace

const char* ocpp_stopped_reason(const std::string& stop_reason) noexcept {
    if (stop_reason == "STATUS_OFF" || stop_reason == "STATUS_READY") return "Local";
    return "Other";
}

const char* ocpp_ended_trigger_reason(const std::string& stop_reason) noexcept {
    if (stop_reason == "STATUS_OFF" || stop_reason == "STATUS_READY") return "ChargingStateChanged";
    return "AbnormalCondition";
}

std::optional<std::int64_t> parse_utc_epoch(const std::string& s) {
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') {
        return std::nullopt;
    }
    const auto digits = [&](std::size_t pos, std::size_t n, int& out) {
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0;
    if (!digits(0, 4, Y) || !digits(5, 2, M) || !digits(8, 2, D) || !digits(11, 2, h) || !digits(14, 2, mi) ||
        !digits(17, 2, sec)) {
        return std::nullopt;
    }
    if (M < 1 || M > 12 || D < 1 || h > 23 || mi > 59 || sec > 59) return std::nullopt;
    if (static_cast<unsigned>(D) > days_in_month(Y, static_cast<unsigned>(M))) return std::nullopt;
    const std::int64_t days = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
    return days * 86400 + h * 3600 + mi * 60 + sec;
}

std::optional<std::string> format_utc_epoch(std::int64_t epoch_seconds) {
    const std::int64_t days = floor_div(epoch_seconds, 86400);
    const std::int64_t sod = epoch_seconds - days * 86400;
    std::int64_t y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, y, m, d);
    if (y < 0 || y > 9999) return std::nullopt;
    std::string year = std::to_string(y);
    year.insert(0, 4 - year.size(), '0');
    return year + "-" + two(m) + "-" + two(d) + "T" + two(static_cast<unsigned>(sod / 3600)) + ":" +
           two(static_cast<unsigned>((sod % 3600) / 60)) + ":" + two(static_cast<unsigned>(sod % 60)) + "Z";
}

std::optional<std::string> derive_start_utc(const std::string& closed_at_utc, std::int64_t duration_ms) {
    if (duration_ms < 0) return std::nullopt;
    const auto close = parse_utc_epoch(closed_at_utc);
    if (!close) return std::nullopt;
    // 128-bit: close * 1000 - duration cannot overflow, and the floor keeps the
    // result on the second the session had already started in.
    const WideInt start_ms = static_cast<WideInt>(*close) * 1000 - static_cast<WideInt>(duration_ms);
    WideInt start_s = start_ms / 1000;
    if (start_ms % 1000 != 0 && start_ms < 0) --start_s;
    if (start_s < static_cast<WideInt>(std::numeric_limits<std::int64_t>::min()) ||
        start_s > static_cast<WideInt>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return format_utc_epoch(static_cast<std::int64_t>(start_s));
}

std::string json_ocpp_events(const SettledSession& s, std::string& error) {
    error.clear();
    const auto& r = s.record;
    const std::string transaction_id = s.asset_id + "-" + std::to_string(s.id);
    if (transaction_id.size() > kMaxTransactionIdLength) {
        error =
            "transactionId '" + transaction_id + "' exceeds the 36 characters OCPP 2.0.1 allows; refusing to export";
        return {};
    }
    if (r.duration_ms < 0 || r.charging_s < 0) {
        error = "the session duration is negative; refusing to derive a start time";
        return {};
    }
    if (r.energy_uwh > kMaxExactJsonInteger) {
        error = "the session energy exceeds 2^53 uWh and would not survive a JSON number exactly; refusing to export";
        return {};
    }
    if (!parse_utc_epoch(s.closed_at_utc)) {
        error = "closed_at_utc '" + s.closed_at_utc +
                "' is not strict YYYY-MM-DDTHH:MM:SSZ; refusing to export a made-up time";
        return {};
    }
    const auto start = derive_start_utc(s.closed_at_utc, r.duration_ms);
    if (!start) {
        error = "the derived start time is outside the year range 0000..9999; refusing to export";
        return {};
    }
    const std::string& end = s.closed_at_utc;
    const std::string tx = json_escape_local(transaction_id);

    std::ostringstream o;
    o << "{\"schema\":\"" << OCPP_EVENTS_SCHEMA << "\""
      << ",\"note\":\"OCPP 2.0.1 TransactionEventRequest payloads rebuilt from the local ledger (a file export, no "
         "transport). The energy is a session-relative integral in microwatt-hours (unit Wh, multiplier -6) that "
         "starts at 0: it is not a meter register and carries no signature (no OCMF). The Started timestamp is "
         "derived as close minus the measured duration; the Ended timestamp is the wall-clock label written at "
         "close.\""
      << ",\"session\":{\"asset_id\":\"" << json_escape_local(s.asset_id) << "\",\"session_id\":" << s.id
      << ",\"billing_complete\":" << (r.billing_complete ? "true" : "false") << ",\"quality\":\""
      << json_escape_local(r.quality) << "\",\"stop_reason\":\"" << json_escape_local(r.stop_reason)
      << "\",\"energy_lower_bound\":" << (r.energy_lower_bound ? "true" : "false")
      << ",\"time_upper_bound\":" << (r.time_upper_bound ? "true" : "false") << "}" << ",\"events\":[";
    // Started
    o << "{\"eventType\":\"Started\",\"timestamp\":\"" << *start << "\",\"triggerReason\":\"ChargingStateChanged\""
      << ",\"seqNo\":0,\"transactionInfo\":{\"transactionId\":\"" << tx << "\",\"chargingState\":\"Charging\"}"
      << ",\"meterValue\":" << meter_value_json(*start, "Transaction.Begin", 0) << "}";
    // Ended
    o << ",{\"eventType\":\"Ended\",\"timestamp\":\"" << end << "\",\"triggerReason\":\""
      << ocpp_ended_trigger_reason(r.stop_reason) << "\",\"seqNo\":1,\"transactionInfo\":{\"transactionId\":\"" << tx
      << "\",\"chargingState\":\"Idle\",\"timeSpentCharging\":" << r.charging_s << ",\"stoppedReason\":\""
      << ocpp_stopped_reason(r.stop_reason) << "\"}"
      << ",\"meterValue\":" << meter_value_json(end, "Transaction.End", r.energy_uwh) << "}";
    o << "]}";
    return o.str();
}

}  // namespace charger
