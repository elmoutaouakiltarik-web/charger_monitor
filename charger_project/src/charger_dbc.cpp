#include "charger_dbc.hpp"

#include "charger_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace charger {
namespace {

// Status meaning from a VAL_ label. Keywords are matched in the OEM's own
// vocabulary, case-insensitively; a negated label ("not ready") is refused
// rather than guessed. Returns kStatusInvalid when the label is not a match.
std::int8_t status_from_label(const std::string& label) {
    std::string low;
    low.reserve(label.size());
    for (unsigned char c : label) low.push_back(static_cast<char>(std::tolower(c)));
    if (low.find(" not ") == 0 || low.rfind("not ", 0) == 0 || low.find("not charg") != std::string::npos) {
        return kStatusInvalid;
    }
    const auto has = [&low](const char* word) { return low.find(word) != std::string::npos; };
    if (has("fault") || has("error") || has("fail") || has("trip")) return kStatusFault;
    if (has("charg") || has("deliver") || has("drive")) return kStatusCharging;
    if (has("ready") || has("idle") || has("standby")) return kStatusReady;
    if (has("off") || has("disabled") || has("stop")) return kStatusOff;
    return kStatusInvalid;
}

// Validates one measurement signal against what the monitor's wire codec can
// read exactly: a 16-bit field aligned to a byte pair, unsigned, with a plain
// zero offset and a factor expressible as an exact rational (< numerator
// / denominator). Returns an empty string on success, otherwise the reason.
std::string check_measurement_signal(const DbcSignal& s, const std::string& role, std::uint8_t& offset,
                                     bool& little_endian, Scale& scale) {
    if (s.is_signed) {
        return role + " signal " + s.name + " is signed (" + std::to_string(s.length_bits) +
               " bits): the monitor decodes unsigned physical measurements";
    }
    if (s.offset != 0.0) {
        return role + " signal " + s.name + " has a non-zero offset: the codec does not invent physical calibrations";
    }
    if (s.length_bits != 16) {
        return role + " signal " + s.name + " is " + std::to_string(s.length_bits) +
               " bits wide: the codec reads exactly 16-bit fields";
    }
    const bool ok_alignment = s.little_endian ? (s.start_bit % 8 == 0) : (s.start_bit % 8 == 7);
    if (!ok_alignment) {
        return role + " signal " + s.name + " starts at bit " + std::to_string(s.start_bit) +
               ": the signal must start on a byte boundary (Intel starts at bit 8n, Motorola at bit 8n+7)";
    }
    // Represent factor as num/den exactly. factor comes from the DBC decimal,
    // so a small power-of-ten denominator must close in; anything else is
    // refused rather than approximated on a tariff-bearing channel.
    for (int d = 0; d <= 9; ++d) {
        const double den = std::pow(10.0, d);
        const double scaled = s.factor * den;
        const double rounded = std::round(scaled);
        if (std::llabs(static_cast<long long>(rounded)) < 1) continue;
        if (std::fabs(scaled - rounded) > 1e-9 * std::max(1.0, std::fabs(scaled))) continue;
        auto num = static_cast<std::int64_t>(rounded);
        auto den_int = static_cast<std::int64_t>(den);
        const auto g = std::gcd(num, den_int);
        num /= g;
        den_int /= g;
        if (num > 0 && num <= 1000000 && den_int <= 1000000000 && den_int > 0) {
            scale.numerator = num;
            scale.denominator = den_int;
            break;
        }
    }
    if (scale.numerator <= 0) {
        return role + " signal " + s.name + ": factor " + std::to_string(s.factor) +
               " is not representable as an exact rational scale";
    }
    offset = static_cast<std::uint8_t>(s.start_bit / 8);
    little_endian = s.little_endian;
    return {};
}

std::optional<DbcStatusBinding> bind_status_message(const DbcDatabase& db, std::uint32_t can_id,
                                                    const std::string& signal_name) {
    const auto* m = db.find_by_id(can_id);
    if (m == nullptr) return std::nullopt;
    const auto* s = m->find_signal(signal_name);
    if (s == nullptr || s->values.empty()) return std::nullopt;
    DbcStatusBinding out;
    out.can_id = can_id;
    out.length = m->length;
    out.signal = s->name;
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    const auto blank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && blank(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && blank(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Reads a double the way a DBC file writes them: the locale-independent point,
// so a European vendor file with a comma decimal still parses.
double parse_dbc_number(const std::string& raw) {
    std::string t = trim(raw);
    if (t.empty()) return 0.0;
    // Some vendor tools emit a comma as the decimal separator.
    if (t.find(',') != std::string::npos && t.find('.') == std::string::npos) {
        t.replace(t.find(','), 1, ".");
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return 0.0;
    return v;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

// Extracts the text between the first pair of double quotes.
std::string quoted(const std::string& s) {
    const auto a = s.find('"');
    if (a == std::string::npos) return {};
    const auto b = s.find('"', a + 1);
    if (b == std::string::npos) return {};
    return s.substr(a + 1, b - a - 1);
}

// Sections this reader recognises but does not interpret. Reported so a caller
// knows the file loaded with less than full fidelity.
const char* const kSkippablePrefixes[] = {
    "NS_",        "CM_",       "BA_DEF_", "BA_DEF_REL_",  "BA_DEF_DEF_",  "BA_",      "BA_REL_", "BA_DEF_DEF_REL_",
    "BU_SG_REL_", "BO_TX_BU_", "EV_",     "ENVVAR_DATA_", "SIG_VALTYPE_", "CAT_DEF_", "CAT_",    "VAL_TABLE_",
    "SIG_GROUP_",
};

bool is_skippable(const std::string& t) {
    for (const char* p : kSkippablePrefixes) {
        if (t.rfind(p, 0) == 0) return true;
    }
    return false;
}

}  // namespace

std::optional<std::int64_t> DbcSignal::decode_raw(const std::uint8_t* data, std::size_t length) const {
    if (length_bits == 0 || length_bits > 64) return std::nullopt;
    if (data == nullptr) return std::nullopt;

    // DBC numbers payload bits with bit 0 at the most significant end of byte 0,
    // and a signal is always one contiguous ascending run of payload bits.
    //
    // Intel anchors that run at the least significant bit, so it starts exactly
    // at start_bit. Motorola anchors it one bit below the least significant
    // position of the starting byte, so the first bit of the run sits at
    // 7 - (start_bit % 8) inside byte start_bit / 8, and the value bits descend
    // from there. For a byte-aligned signal (start_bit % 8 == 7) this collapses
    // to plain big-endian assembly over the bytes it spans.
    //
    // Verified against cantools 45.x for both endiannesses, every start position
    // 0..63 and every length 1..64.
    const std::uint32_t first_bit = little_endian ? start_bit : (start_bit / 8) * 8 + (7 - (start_bit % 8));
    const std::uint64_t available_bits = static_cast<std::uint64_t>(length) * 8;
    // Bounds are checked against the whole run, not just its length: a signal
    // that starts high in a short payload must be refused, never read past.
    if (static_cast<std::uint64_t>(first_bit) + length_bits > available_bits) return std::nullopt;

    std::uint64_t raw = 0;
    for (std::uint32_t i = 0; i < length_bits; ++i) {
        const std::uint32_t position = first_bit + i;
        const std::uint8_t byte = data[position / 8];
        const std::uint32_t shift = little_endian ? (position % 8) : (7 - (position % 8));
        const std::uint32_t value_bit = little_endian ? i : (length_bits - 1 - i);
        if (((byte >> shift) & 1u) != 0) raw |= (1ull << value_bit);
    }

    if (is_signed && length_bits < 64) {
        const std::uint64_t sign = 1ull << (length_bits - 1);
        if ((raw & sign) != 0) {
            // Sign extend through a signed 64-bit intermediate.
            return static_cast<std::int64_t>(raw | ~((sign << 1) - 1));
        }
    }
    return static_cast<std::int64_t>(raw);
}

std::string DbcSignal::label_for(std::int64_t raw) const {
    for (const auto& v : values) {
        if (v.first == raw) return v.second;
    }
    return {};
}

const DbcSignal* DbcMessage::find_signal(const std::string& signal_name) const {
    for (const auto& s : signals) {
        if (s.name == signal_name) return &s;
    }
    return nullptr;
}

const DbcMessage* DbcDatabase::find_by_id(std::uint32_t can_id) const {
    for (const auto& m : messages) {
        if (m.can_id == can_id) return &m;
    }
    return nullptr;
}

const DbcMessage* DbcDatabase::find_by_name(const std::string& message_name) const {
    for (const auto& m : messages) {
        if (m.name == message_name) return &m;
    }
    return nullptr;
}

DbcLoadResult parse_dbc(const std::string& text) {
    DbcLoadResult res;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    bool in_message = false;

    while (std::getline(in, line)) {
        ++lineno;
        const auto t = trim(line);
        if (t.empty()) continue;
        if (t[0] == ';' || t.rfind("VERSION", 0) == 0) {
            if (t.rfind("VERSION", 0) == 0) res.database.version = quoted(t);
            continue;
        }
        if (t.rfind("BU_", 0) == 0) {
            const auto body = t.substr(t.find(':') == std::string::npos ? 3 : t.find(':') + 1);
            res.database.nodes = split_ws(body);
            continue;
        }

        if (t.rfind("VAL_", 0) == 0) {
            // VAL_ <id> <signal> <value> "<label>" [<value> "<label>" ...]
            // Labels may contain spaces, so split on quotes rather than spaces:
            // the quoted segments are the labels, the tokens between them the values.
            const auto head = split_ws(t.substr(4));
            if (head.size() < 2) continue;
            const std::uint32_t id =
                static_cast<std::uint32_t>(std::strtoul(head[0].c_str(), nullptr, 10)) & ~kDbcExtendedFlag;
            auto* m = res.database.find_by_id(id);
            if (m == nullptr) continue;
            auto* sig = const_cast<DbcSignal*>(m->find_signal(head[1]));
            if (sig == nullptr) continue;

            // Walk the raw line: number, quoted label, number, quoted label...
            const auto sig_at = line.find(head[1]);
            if (sig_at == std::string::npos) continue;
            const auto tail = line.substr(sig_at + head[1].size());
            std::istringstream nums(tail);
            std::string token;
            std::vector<std::string> labels;
            for (std::size_t i = 0; i < tail.size(); ++i) {
                if (tail[i] == '"') {
                    const auto close = tail.find('"', i + 1);
                    if (close == std::string::npos) break;
                    labels.push_back(tail.substr(i + 1, close - i - 1));
                    i = close;
                }
                (void)token;
            }
            // Collect the integers that sit outside quotes, in order.
            std::vector<std::int64_t> numbers;
            bool in_quote = false;
            std::string current;
            for (char c : tail) {
                if (c == '"') {
                    in_quote = !in_quote;
                    if (!current.empty()) {
                        numbers.push_back(std::strtoll(current.c_str(), nullptr, 10));
                        current.clear();
                    }
                    continue;
                }
                if (!in_quote && (std::isdigit(static_cast<unsigned char>(c)) || c == '-')) {
                    current.push_back(c);
                } else if (!current.empty()) {
                    numbers.push_back(std::strtoll(current.c_str(), nullptr, 10));
                    current.clear();
                }
            }
            if (!current.empty()) numbers.push_back(std::strtoll(current.c_str(), nullptr, 10));

            for (std::size_t i = 0; i < numbers.size() && i < labels.size(); ++i) {
                sig->values.emplace_back(numbers[i], labels[i]);
            }
            continue;
        }

        if (t.rfind("BO_", 0) == 0 && t.rfind("BO_TX_BU_", 0) != 0) {
            // BO_ <id> <Name>: <length> <transmitter>
            // The id may be preceded by whitespace, so skip to the first digit
            // rather than assuming the keyword ends exactly where it starts.
            std::size_t p = 3;
            while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p;
            const std::size_t id_start = p;
            while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p]))) ++p;
            if (p == id_start) {
                res.errors.emplace_back(lineno, "BO_ message has no CAN id");
                continue;
            }
            const std::uint32_t raw_id =
                static_cast<std::uint32_t>(std::strtoul(t.substr(id_start, p - id_start).c_str(), nullptr, 10));
            const auto colon = t.find(':', p);
            if (colon == std::string::npos) {
                res.errors.emplace_back(lineno, "BO_ message is missing ':'");
                continue;
            }
            DbcMessage m;
            m.can_id = raw_id & ~kDbcExtendedFlag;
            m.name = trim(t.substr(p, colon - p));
            const auto after_colon = split_ws(trim(t.substr(colon + 1)));
            if (after_colon.empty()) {
                res.errors.emplace_back(lineno, "BO_ message is missing its length");
                continue;
            }
            m.length = static_cast<std::size_t>(std::strtoul(after_colon[0].c_str(), nullptr, 10));
            res.database.messages.push_back(m);
            in_message = true;
            continue;
        }

        if (t.rfind("SG_", 0) == 0 && in_message && !res.database.messages.empty()) {
            // SG_ name [M|mN] : start|len@order sign (factor,offset) [min|max] "unit" receivers
            const auto colon = t.find(':');
            if (colon == std::string::npos) {
                res.errors.emplace_back(lineno, "SG_ signal is missing ':'");
                continue;
            }
            std::string name_part = t.substr(3, colon - 3);
            // Drop a multiplexer indicator such as [M] or [m3].
            const auto br = name_part.find('[');
            if (br != std::string::npos) {
                const auto br2 = name_part.find(']', br);
                if (br2 != std::string::npos) name_part = name_part.substr(0, br) + name_part.substr(br2 + 1);
            }
            DbcSignal s;
            s.name = trim(name_part);

            const auto body = trim(t.substr(colon + 1));
            const auto at = body.find('@');
            const auto bar = body.find('|');
            if (at == std::string::npos || bar == std::string::npos || bar > at) {
                res.errors.emplace_back(lineno, "SG_ signal is missing start|length@order");
                continue;
            }
            s.start_bit = static_cast<std::uint32_t>(std::strtoul(body.substr(0, bar).c_str(), nullptr, 10));
            // After '|' comes <length>@<order><sign>. Take two characters past
            // the '@': order is '1' (Intel/little-endian) or '0'
            // (Motorola/big-endian), then '+' unsigned or '-' signed.
            const auto order_pos = at + 1;
            if (order_pos + 1 < body.size()) {
                s.little_endian = (body[order_pos] == '1');
                s.is_signed = (body[order_pos + 1] == '-');
            }
            // The bit length is the digits between '|' and '@'.
            const std::string len_digits = trim(body.substr(bar + 1, at - bar - 1));
            if (len_digits.empty()) {
                res.errors.emplace_back(lineno, "SG_ signal has no bit length");
                continue;
            }
            s.length_bits = static_cast<std::uint32_t>(std::strtoul(len_digits.c_str(), nullptr, 10));

            const auto lp = body.find('(', at);
            const auto rp = body.find(')', lp == std::string::npos ? at : lp);
            if (lp != std::string::npos && rp != std::string::npos) {
                const auto comma = body.find(',', lp);
                if (comma != std::string::npos) {
                    s.factor = parse_dbc_number(body.substr(lp + 1, comma - lp - 1));
                    s.offset = parse_dbc_number(body.substr(comma + 1, rp - comma - 1));
                }
            }
            const auto lb = body.find('[', rp == std::string::npos ? at : rp);
            const auto rb = body.find(']', lb == std::string::npos ? at : lb);
            if (lb != std::string::npos && rb != std::string::npos) {
                const auto pipe = body.find('|', lb);
                if (pipe != std::string::npos) {
                    s.minimum = parse_dbc_number(body.substr(lb + 1, pipe - lb - 1));
                    s.maximum = parse_dbc_number(body.substr(pipe + 1, rb - pipe - 1));
                }
            }
            s.unit = quoted(body);
            res.database.messages.back().signals.push_back(s);
            continue;
        }

        if (is_skippable(t)) {
            if (std::find(res.skipped_sections.begin(), res.skipped_sections.end(), t.substr(0, 4)) ==
                res.skipped_sections.end()) {
                res.skipped_sections.push_back(t.substr(0, 4));
            }
            continue;
        }
        // Anything else is ignored rather than rejected: real vendor files
        // contain vendor extensions this reader does not model.
    }

    res.ok = res.errors.empty();
    return res;
}

bool bind_signals(const DbcDatabase& db, const std::string& voltage_signal, const std::string& current_signal,
                  DbcBinding& out, std::string& error) {
    out = DbcBinding{};
    const DbcMessage* vmsg = nullptr;
    const DbcSignal* vsig = nullptr;
    const DbcMessage* imsg = nullptr;
    const DbcSignal* isig = nullptr;
    for (const auto& m : db.messages) {
        if (const auto* s = m.find_signal(voltage_signal)) {
            vmsg = &m;
            vsig = s;
        }
        if (const auto* s = m.find_signal(current_signal)) {
            imsg = &m;
            isig = s;
        }
    }
    if (vsig == nullptr) {
        error = "voltage signal \"" + voltage_signal + "\" is not defined by the database";
        return false;
    }
    if (isig == nullptr) {
        error = "current signal \"" + current_signal + "\" is not defined by the database";
        return false;
    }
    if (vmsg->can_id != imsg->can_id) {
        error = "voltage signal \"" + voltage_signal + "\" and current signal \"" + current_signal +
                "\" must live in the same message";
        return false;
    }
    out.measurement_id = vmsg->can_id;
    out.measurement_length = vmsg->length;
    out.voltage_signal = voltage_signal;
    out.current_signal = current_signal;
    return true;
}

std::optional<DbcStatusBinding> auto_bind_status(const DbcDatabase& db) {
    // A status carrier is a 1-byte message with an enumerated signal whose
    // value table maps onto at least two distinct meanings; anything more
    // ambiguous would guess the charge state, which the session ledger is keyed
    // on.
    for (const auto& m : db.messages) {
        if (m.length != 1 || m.signals.size() != 1) continue;
        const auto& s = m.signals.front();
        if (s.values.empty() || s.length_bits > 8) continue;
        bool has_two = false;
        std::int8_t first = kStatusInvalid;
        for (const auto& v : s.values) {
            const auto meaning = status_from_label(v.second);
            if (meaning == kStatusInvalid) continue;
            if (!has_two) {
                has_two = true;
                first = meaning;
            } else if (meaning != first) {
                return bind_status_message(db, m.can_id, s.name);
            }
        }
    }
    return std::nullopt;
}

DbcProfileResult profile_from_dbc(const DbcDatabase& db, const DbcBinding& binding) {
    DbcProfileResult res;
    const auto* msg = db.find_by_id(binding.measurement_id);
    if (msg == nullptr) {
        res.error = "measurement message 0x" + [&] {
            std::ostringstream h;
            h << std::hex << binding.measurement_id;
            return h.str();
        }() + " is not in the database";
        return res;
    }
    const auto* vsig = msg->find_signal(binding.voltage_signal);
    const auto* isig = msg->find_signal(binding.current_signal);
    if (vsig == nullptr || isig == nullptr) {
        res.error = "voltage or current signal not found in message " + msg->name;
        return res;
    }
    if (msg->length < 2 || msg->length > 8) {
        res.error = "measurement message " + msg->name + " has a length of " + std::to_string(msg->length) +
                    " bytes; the codec reads 2-byte fields inside an 8-byte classic CAN frame";
        return res;
    }

    Scale v_scale{}, i_scale{};
    std::uint8_t v_off = 0, i_off = 0;
    bool v_le = true, i_le = true;
    if (auto e = check_measurement_signal(*vsig, "voltage", v_off, v_le, v_scale); !e.empty()) {
        res.error = e;
        return res;
    }
    if (auto e = check_measurement_signal(*isig, "current", i_off, i_le, i_scale); !e.empty()) {
        res.error = e;
        return res;
    }
    if (std::llabs(static_cast<long long>(v_off) - static_cast<long long>(i_off)) < 2) {
        res.error = "voltage and current fields overlap on " + msg->name;
        return res;
    }
    if (v_le != i_le) {
        // The profile carries a single byte order for both measurements. Reading
        // a big-endian field as little-endian silently yields a plausible wrong
        // current, so this is refused rather than resolved in favour of one.
        res.error = "voltage and current use different byte orders on " + msg->name + " ('" + vsig->name + "' and '" +
                    isig->name + "'); one profile carries one byte order, so this is refused";
        return res;
    }

    ProtocolProfile profile;
    profile.measurement_id = binding.measurement_id;
    profile.measurement_length = static_cast<std::uint8_t>(msg->length);
    profile.voltage_offset = v_off;
    profile.current_offset = i_off;
    profile.voltage = v_scale;
    profile.current = i_scale;
    profile.little_endian = v_le;
    profile.reserved_must_be_zero = false;  // other signals may share the frame

    if (binding.status_id == 0 && binding.status_signal.empty()) {
        res.ok = true;
        res.profile = profile;
        return res;
    }

    const auto* smsg = binding.status_id != 0 ? db.find_by_id(binding.status_id) : nullptr;
    const DbcSignal* ssig = nullptr;
    if (binding.status_id != 0 && smsg != nullptr) {
        ssig = smsg->find_signal(binding.status_signal);
    } else {
        for (const auto& m : db.messages) {
            if (const auto* s = m.find_signal(binding.status_signal)) {
                smsg = &m;
                ssig = s;
                break;
            }
        }
    }
    if (smsg == nullptr || ssig == nullptr) {
        res.error = "status signal \"" + binding.status_signal + "\" not found in the database";
        return res;
    }
    if (smsg->length != 1) {
        res.error = "status carrier " + smsg->name + " is " + std::to_string(smsg->length) +
                    " bytes; the monitor reads the power-stage status from a dedicated 1-byte frame";
        return res;
    }
    if (ssig->values.empty()) {
        res.error = "status signal " + ssig->name + " has no VAL_ value table";
        return res;
    }
    if (ssig->length_bits > 8) {
        // The status map is keyed by one byte. A wider signal would collide here
        // (values 0 and 256 mapping to the same key), so it is refused.
        res.error = "status signal " + ssig->name + " is " + std::to_string(ssig->length_bits) +
                    " bits wide; the monitor reads the status from a single byte";
        return res;
    }
    int distinct = 0;
    bool seen[4] = {false, false, false, false};
    for (const auto& v : ssig->values) {
        if (v.first < 0 || v.first > 0xff) {
            res.error = "status value " + std::to_string(v.first) + " on " + ssig->name +
                        " does not fit in the single status byte";
            return res;
        }
        const auto meaning = status_from_label(v.second);
        if (meaning < 0) continue;
        profile.status_map[static_cast<std::uint8_t>(v.first)] = meaning;
        if (meaning >= 0 && meaning <= 3) {
            if (!seen[meaning]) {
                seen[meaning] = true;
                ++distinct;
            }
        }
    }
    if (distinct < 2) {
        res.error = "status value table on " + ssig->name +
                    " does not map to at least two distinct meanings; refusing to guess";
        return res;
    }
    profile.status_id = smsg->can_id;
    res.ok = true;
    res.status_bound = true;
    res.profile = profile;
    return res;
}

std::optional<DbcBinding> auto_bind(const DbcDatabase& db, double min_voltage_v, double max_voltage_v,
                                    double min_current_a, double max_current_a) {
    // Choose the message that carries both measurements, then pick within it the
    // signal whose declared [min|max] best covers the configured window.
    // Matching on the declared range rather than the name keeps this working for
    // vendor files that call a signal "BusVoltage" or "HV_Meas".
    const DbcMessage* chosen = nullptr;
    const DbcSignal* voltage = nullptr;
    const DbcSignal* current_best = nullptr;
    double voltage_span = -1.0;

    for (const auto& m : db.messages) {
        if (m.signals.empty() || m.length == 0 || m.length > 8) continue;

        // Candidates are the signals whose declared range can contain the
        // configured voltage window. Several often qualify (a 1000 V and a
        // 500 A range both span 0..500), so the voltage is the widest one and
        // the current is the next widest that still reaches max_current_a.
        const DbcSignal* widest = nullptr;
        double widest_span = -1.0;
        for (const auto& s : m.signals) {
            if (s.factor <= 0.0) continue;
            const double lo = std::min(s.minimum, s.maximum);
            const double hi = std::max(s.minimum, s.maximum);
            if (hi <= lo) continue;
            if (!(lo <= min_voltage_v + 1e-9 && hi >= max_voltage_v - 1e-9)) continue;
            const double span = hi - lo;
            if (span > widest_span) {
                widest_span = span;
                widest = &s;
            }
        }
        if (widest == nullptr) continue;

        const DbcSignal* current = nullptr;
        double current_span = -1.0;
        for (const auto& s : m.signals) {
            if (s.factor <= 0.0 || &s == widest) continue;
            const double lo = std::min(s.minimum, s.maximum);
            const double hi = std::max(s.minimum, s.maximum);
            if (hi <= lo) continue;
            if (!(hi >= max_current_a - 1e-9)) continue;
            // Must start at or below zero and cover the configured minimum.
            if (lo > 1e-9 || hi < min_current_a - 1e-9) continue;
            const double span = hi - lo;
            if (span > current_span) {
                current_span = span;
                current = &s;
            }
        }
        if (current == nullptr) continue;

        const bool better = chosen == nullptr || widest_span < voltage_span;
        if (better) {
            chosen = &m;
            voltage = widest;
            voltage_span = widest_span;
            current_best = current;
        }
    }
    if (chosen == nullptr || voltage == nullptr || current_best == nullptr) return std::nullopt;
    DbcBinding binding;
    binding.measurement_id = chosen->can_id;
    binding.measurement_length = chosen->length;
    binding.voltage_signal = voltage->name;
    binding.current_signal = current_best->name;
    // Best-effort status binding: when a clear carrier exists the caller gets
    // it for free; otherwise the operator decides, not the tool.
    if (const auto st = auto_bind_status(db)) {
        binding.status_id = st->can_id;
        binding.status_signal = st->signal;
    }
    return binding;
}

}  // namespace charger