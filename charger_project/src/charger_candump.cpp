#include "charger_candump.hpp"

#include <cstdio>
#include <vector>

namespace charger {
namespace {

constexpr std::size_t kMaxLine = 4096;
constexpr std::uint32_t kErrFlag = 0x20000000u;
constexpr std::uint32_t kEffMask = 0x1FFFFFFFu;
constexpr std::uint32_t kSffMask = 0x7FFu;
// Seconds that still fit in int64 microseconds.
constexpr std::int64_t kMaxSeconds = 9223372036854LL;

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::vector<std::string> split_tokens(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_blank(s[i])) ++i;
        const std::size_t start = i;
        while (i < s.size() && !is_blank(s[i])) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Strict hex run of exactly `s.size()` digits, at most 8, into a uint32.
bool parse_hex_u32(const std::string& s, std::uint32_t& out) {
    if (s.empty() || s.size() > 8) return false;
    std::uint32_t v = 0;
    for (char c : s) {
        const int h = hex_val(c);
        if (h < 0) return false;
        v = (v << 4) | static_cast<std::uint32_t>(h);
    }
    out = v;
    return true;
}

// Even-length hex into at most `max` bytes. Anything longer is refused rather
// than truncated: a classic frame never carries more than 8 bytes.
bool parse_hex_bytes(const std::string& hex, std::size_t max, std::array<std::uint8_t, 8>& out, std::uint8_t& n) {
    if (hex.size() % 2 != 0 || hex.size() / 2 > max) return false;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_val(hex[i]);
        const int lo = hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i / 2 < out.size()) out[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    n = static_cast<std::uint8_t>(hex.size() / 2);
    return true;
}

// "(1436509052.249713)" -> microseconds. Integer arithmetic only; digits beyond
// the microsecond are dropped (candump prints exactly six).
bool parse_timestamp(const std::string& tok, std::int64_t& out_us) {
    if (tok.size() < 3 || tok.front() != '(' || tok.back() != ')') return false;
    const std::string inner = tok.substr(1, tok.size() - 2);
    const std::size_t dot = inner.find('.');
    const std::string sec_s = dot == std::string::npos ? inner : inner.substr(0, dot);
    const std::string frac_s = dot == std::string::npos ? std::string() : inner.substr(dot + 1);
    if (sec_s.empty() || sec_s.size() > 13 || frac_s.size() > 9) return false;
    if (dot != std::string::npos && frac_s.empty()) return false;
    std::int64_t sec = 0;
    for (char c : sec_s) {
        if (c < '0' || c > '9') return false;
        sec = sec * 10 + (c - '0');
    }
    if (sec > kMaxSeconds) return false;
    std::int64_t frac = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        const int digit = i < frac_s.size() ? frac_s[i] - '0' : 0;
        if (digit < 0 || digit > 9) return false;
        frac = frac * 10 + digit;
    }
    for (std::size_t i = 6; i < frac_s.size(); ++i) {
        if (frac_s[i] < '0' || frac_s[i] > '9') return false;
    }
    out_us = sec * 1000000 + frac;
    return true;
}

// Applies an identifier token: 3 digits is a standard id, 8 digits is a 29-bit
// id or, with CAN_ERR_FLAG set, an error frame. Anything else is refused.
bool apply_id(const std::string& tok, CandumpRecord& r) {
    std::uint32_t v = 0;
    if (!parse_hex_u32(tok, v)) return false;
    if (tok.size() == 3) {
        if (v > kSffMask) return false;
        r.id = v;
        return true;
    }
    if (tok.size() == 8) {
        // Bits 31/30 are the EFF/RTR flags of the kernel representation; the text
        // formats express those differently, so they never appear in an id.
        if ((v & 0xC0000000u) != 0) return false;
        if ((v & kErrFlag) != 0) {
            r.error_frame = true;
            r.id = v & kEffMask & ~kErrFlag;
        } else {
            r.extended = true;
            r.id = v & kEffMask;
        }
        return true;
    }
    return false;
}

// "044#2A366C2BBA", "123#R", "123#R5", "123##1AABB"
bool parse_log_frame(const std::string& tok, CandumpRecord& r) {
    const std::size_t hash = tok.find('#');
    if (hash == std::string::npos) return false;
    if (!apply_id(tok.substr(0, hash), r)) return false;
    const std::string rest = tok.substr(hash + 1);
    if (!rest.empty() && rest[0] == '#') {
        // CAN FD: one flags digit, then up to 64 data bytes. Recognised, not decoded.
        if (rest.size() < 2 || rest.size() > 2 + 128 || hex_val(rest[1]) < 0) return false;
        const std::string payload = rest.substr(2);
        if (payload.size() % 2 != 0) return false;
        for (char c : payload) {
            if (hex_val(c) < 0) return false;
        }
        r.fd = true;
        return true;
    }
    if (!rest.empty() && rest[0] == 'R') {
        if (rest.size() > 2) return false;
        if (rest.size() == 2 && hex_val(rest[1]) < 0) return false;
        r.rtr = true;
        return true;
    }
    return parse_hex_bytes(rest, 8, r.data, r.length);
}

// Default human format after the optional timestamp:
//   can0  180   [8]  80 BB E8 03 00 00 00 00        (optionally  ERRORFRAME)
//   can0  123   remote request
bool parse_human_frame(const std::vector<std::string>& t, std::size_t i, CandumpRecord& r) {
    if (t.size() < i + 2) return false;
    if (!apply_id(t[i + 1], r)) return false;
    std::size_t k = i + 2;
    if (t.size() == k + 2 && t[k] == "remote" && t[k + 1] == "request") {
        r.rtr = true;
        return true;
    }
    if (k >= t.size()) return false;
    const std::string& len_tok = t[k];
    if (len_tok.size() < 3 || len_tok.front() != '[' || len_tok.back() != ']') return false;
    std::size_t len = 0;
    for (std::size_t j = 1; j + 1 < len_tok.size(); ++j) {
        if (len_tok[j] < '0' || len_tok[j] > '9') return false;
        len = len * 10 + static_cast<std::size_t>(len_tok[j] - '0');
        if (len > 64) return false;
    }
    if (len > 8) return false;  // a classic frame never carries more; an FD frame is not decoded here
    ++k;
    for (std::size_t j = 0; j < len; ++j, ++k) {
        if (k >= t.size() || t[k].size() != 2) return false;
        const int hi = hex_val(t[k][0]);
        const int lo = hex_val(t[k][1]);
        if (hi < 0 || lo < 0) return false;
        r.data[j] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    r.length = static_cast<std::uint8_t>(len);
    if (k < t.size()) {
        if (t[k] == "ERRORFRAME" && k + 1 == t.size()) {
            r.error_frame = true;
        } else {
            return false;
        }
    }
    return true;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

bool plausible_iface(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-' || c == ':';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::optional<CandumpRecord> parse_candump_line(const std::string& line) {
    if (line.size() > kMaxLine) return std::nullopt;
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || is_blank(s.back()))) s.pop_back();
    const auto t = split_tokens(s);
    if (t.empty()) return std::nullopt;

    CandumpRecord r;
    std::size_t i = 0;
    if (t[0].front() == '(') {
        if (!parse_timestamp(t[0], r.time_us)) return std::nullopt;
        r.has_time = true;
        i = 1;
    }
    if (t.size() <= i + 1) return std::nullopt;
    if (!plausible_iface(t[i])) return std::nullopt;
    r.iface = t[i];
    if (t[i + 1].find('#') != std::string::npos) {
        if (t.size() != i + 2) return std::nullopt;
        if (!parse_log_frame(t[i + 1], r)) return std::nullopt;
        return r;
    }
    if (!parse_human_frame(t, i, r)) return std::nullopt;
    return r;
}

std::string utc_from_epoch_us(std::int64_t epoch_us) {
    const std::int64_t sec = floor_div(epoch_us, 1000000);
    const std::int64_t days = floor_div(sec, 86400);
    const std::int64_t sod = sec - days * 86400;
    // Howard Hinnant's civil_from_days: exact, no time-zone database involved.
    const std::int64_t z = days + 719468;
    const std::int64_t era = floor_div(z, 146097);
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(sod / 3600),
                  static_cast<long long>((sod % 3600) / 60), static_cast<long long>(sod % 60));
    return buf;
}

}  // namespace charger
