#include "charger_config.hpp"

#include "charger_dbc.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

namespace charger {
namespace {

constexpr std::int64_t kMicroPerEuro = 1000000;

struct Parser {
    const std::string& s;
    std::size_t i{0};
    std::vector<ConfigError>* errors;

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eof() {
        skip_ws();
        return i >= s.size();
    }
    char peek() {
        skip_ws();
        return i < s.size() ? s[i] : '\0';
    }
    bool lit(char c) {
        skip_ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool word(const char* w) {
        const std::size_t n = std::char_traits<char>::length(w);
        skip_ws();
        if (s.compare(i, n, w) == 0) {
            i += n;
            return true;
        }
        return false;
    }
    void err(const std::string& field, const std::string& msg) { errors->push_back({field, msg}); }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (i >= s.size() || s[i] != '"') return std::nullopt;
        ++i;
        std::string out;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i >= s.size()) return std::nullopt;
                const char e = s[i++];
                switch (e) {
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    default:
                        out.push_back(e);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return std::nullopt;
    }
    std::optional<double> parse_number() {
        skip_ws();
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool any = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            any = true;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                ++i;
                any = true;
            }
        }
        if (!any) return std::nullopt;
        const std::string tok = s.substr(start, i - start);
        char* end = nullptr;
        const double v = std::strtod(tok.c_str(), &end);
        if (end == nullptr || *end != '\0') return std::nullopt;
        return v;
    }
    // Money and thresholds are kept as strings in the document so the exact
    // decimal the operator wrote reaches the integer tariff without a binary
    // float round trip.
    std::optional<std::string> parse_value_as_string() {
        skip_ws();
        if (i < s.size() && s[i] == '"') return parse_string();
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
        if (i == start) return std::nullopt;
        return s.substr(start, i - start);
    }
    std::optional<std::int64_t> parse_int() {
        const auto n = parse_number();
        if (!n) return std::nullopt;
        return static_cast<std::int64_t>(*n);
    }
};

// Consumes a key: "name" : . Returns the key, or nullopt when not on a key.
std::optional<std::string> take_key(Parser& p) {
    p.skip_ws();
    if (p.i >= p.s.size() || p.s[p.i] != '"') return std::nullopt;
    const auto k = p.parse_string();
    if (!k) return std::nullopt;
    if (!p.lit(':')) return std::nullopt;
    return k;
}

void parse_tariff(const std::string& text, Tariff& t, const std::string& prefix, std::vector<ConfigError>& errors) {
    Parser p{text, 0, &errors};
    if (!p.lit('{')) {
        errors.push_back({prefix, "expected a JSON object"});
        return;
    }
    while (!p.lit('}')) {
        if (p.eof()) {
            errors.push_back({prefix, "unterminated object: the closing brace is missing"});
            return;
        }
        const auto key = take_key(p);
        if (!key) {
            errors.push_back({prefix, "invalid key"});
            return;
        }

        if (*key == "energy_eur_per_kwh" || *key == "time_eur_per_hour" || *key == "flat_eur" ||
            *key == "min_price_eur" || *key == "max_price_eur") {
            const auto v = p.parse_value_as_string();
            if (!v) {
                errors.push_back({prefix + "." + *key, "invalid numeric value"});
                continue;
            }
            const auto micro = parse_fixed_point(*v, kMicroPerEuro);
            if (!micro) {
                errors.push_back({prefix + "." + *key, "amount is invalid (max 6 decimals)"});
                continue;
            }
            if (*key == "energy_eur_per_kwh") {
                t.energy_micro_eur_per_kwh = *micro;
            } else if (*key == "time_eur_per_hour") {
                t.time_micro_eur_per_hour = *micro;
            } else if (*key == "flat_eur") {
                t.flat_micro_eur = *micro;
            } else if (*key == "min_price_eur") {
                t.min_price_micro_eur = *micro;
            } else {
                t.max_price_micro_eur = *micro;
            }
        } else if (*key == "energy_step_wh" || *key == "time_step_s") {
            const auto v = p.parse_int();
            if (!v || *v < 1) {
                errors.push_back({prefix + "." + *key, "must be an integer >= 1"});
            } else if (*key == "energy_step_wh") {
                t.energy_step_wh = static_cast<std::int32_t>(*v);
            } else {
                t.time_step_s = static_cast<std::int32_t>(*v);
            }
        } else {
            errors.push_back({prefix + "." + *key, "campo tariffa sconosciuto"});
            p.skip_ws();
            // Skip the value generically: we only need to stay in sync.
            int depth = 0;
            bool in_str = false;
            while (p.i < p.s.size()) {
                const char c = p.s[p.i++];
                if (in_str) {
                    if (c == '\\') {
                        if (p.i < p.s.size()) ++p.i;
                    } else if (c == '"') {
                        in_str = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_str = true;
                } else if (c == '{' || c == '[') {
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (depth == 0) {
                        --p.i;
                        break;
                    }
                    --depth;
                } else if (c == ',' && depth == 0) {
                    --p.i;
                    break;
                }
            }
        }
        p.lit(',');
    }
}

void parse_diagnostics(const std::string& text, Config& cfg, std::vector<ConfigError>& errors) {
    Parser p{text, 0, &errors};
    if (!p.lit('{')) {
        errors.push_back({"diagnostics", "expected a JSON object"});
        return;
    }
    while (!p.lit('}')) {
        if (p.eof()) {
            errors.push_back({"diagnostics", "unterminated object: the closing brace is missing"});
            return;
        }
        const auto key = take_key(p);
        if (!key) {
            errors.push_back({"diagnostics", "invalid key"});
            return;
        }
        if (*key == "stuck_at_ms") {
            const auto v = p.parse_int();
            if (!v || *v < 0) {
                errors.push_back({"diagnostics.stuck_at_ms", "must be >= 0"});
            } else {
                cfg.stuck_at_window = std::chrono::milliseconds(*v);
            }
        } else if (*key == "max_voltage_rate_mv_per_s" || *key == "max_current_rate_ma_per_s" ||
                   *key == "sag_voltage_mv" || *key == "sag_tolerance_mv" || *key == "current_hunt_band_ma") {
            const auto v = p.parse_int();
            if (!v || *v < 0) {
                errors.push_back({"diagnostics." + *key, "must be >= 0"});
            } else if (*key == "max_voltage_rate_mv_per_s") {
                cfg.max_voltage_rate_mv_per_s = *v;
            } else if (*key == "max_current_rate_ma_per_s") {
                cfg.max_current_rate_ma_per_s = *v;
            } else if (*key == "sag_voltage_mv") {
                cfg.sag_voltage_mv = *v;
            } else if (*key == "sag_tolerance_mv") {
                cfg.sag_tolerance_mv = *v;
            } else {
                cfg.current_hunt_band_ma = *v;
            }
        } else {
            errors.push_back({"diagnostics." + *key, "campo diagnostica sconosciuto"});
            int depth = 0;
            bool in_str = false;
            while (p.i < p.s.size()) {
                const char c = p.s[p.i++];
                if (in_str) {
                    if (c == '\\') {
                        if (p.i < p.s.size()) ++p.i;
                    } else if (c == '"') {
                        in_str = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_str = true;
                } else if (c == '{' || c == '[') {
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (depth == 0) {
                        --p.i;
                        break;
                    }
                    --depth;
                } else if (c == ',' && depth == 0) {
                    --p.i;
                    break;
                }
            }
        }
        p.lit(',');
    }
}

void parse_protocol(const std::string& text, ProtocolProfile& proto, std::vector<ConfigError>& errors) {
    Parser p{text, 0, &errors};
    if (!p.lit('{')) {
        errors.push_back({"protocol", "expected a JSON object"});
        return;
    }
    while (!p.lit('}')) {
        if (p.eof()) {
            errors.push_back({"protocol", "unterminated object: the closing brace is missing"});
            return;
        }
        const auto key = take_key(p);
        if (!key) {
            errors.push_back({"protocol", "invalid key"});
            return;
        }
        if (*key == "measurement_id" || *key == "status_id") {
            const auto v = p.parse_int();
            if (!v || *v < 0 || *v > 0x1fffffff) {
                errors.push_back({"protocol." + *key, ": invalid CAN id"});
            } else if (*key == "measurement_id") {
                proto.measurement_id = static_cast<std::uint32_t>(*v);
            } else {
                proto.status_id = static_cast<std::uint32_t>(*v);
            }
        } else if (*key == "voltage_offset" || *key == "current_offset" || *key == "measurement_length") {
            const auto v = p.parse_int();
            if (!v || *v < 0 || *v > 255) {
                errors.push_back({"protocol." + *key, ": invalid offset/length"});
            } else if (*key == "voltage_offset") {
                proto.voltage_offset = static_cast<std::uint8_t>(*v);
            } else if (*key == "current_offset") {
                proto.current_offset = static_cast<std::uint8_t>(*v);
            } else {
                proto.measurement_length = static_cast<std::uint8_t>(*v);
            }
        } else if (*key == "voltage_scale_den" || *key == "current_scale_den") {
            const auto v = p.parse_int();
            if (!v || *v <= 0) {
                errors.push_back({"protocol." + *key, "scale denominator: must be > 0"});
            } else if (*key == "voltage_scale_den") {
                proto.voltage.denominator = *v;
            } else {
                proto.current.denominator = *v;
            }
        } else if (*key == "little_endian") {
            if (p.word("true")) {
                proto.little_endian = true;
            } else if (p.word("false")) {
                proto.little_endian = false;
            } else {
                errors.push_back({"protocol.little_endian", "expected true or false"});
            }
        } else {
            errors.push_back({"protocol." + *key, "campo protocollo sconosciuto"});
            int depth = 0;
            bool in_str = false;
            while (p.i < p.s.size()) {
                const char c = p.s[p.i++];
                if (in_str) {
                    if (c == '\\') {
                        if (p.i < p.s.size()) ++p.i;
                    } else if (c == '"') {
                        in_str = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_str = true;
                } else if (c == '{' || c == '[') {
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (depth == 0) {
                        --p.i;
                        break;
                    }
                    --depth;
                } else if (c == ',' && depth == 0) {
                    --p.i;
                    break;
                }
            }
        }
        p.lit(',');
    }
}

std::string skip_value(const std::string& s, std::size_t& i) {
    const std::size_t start = i;
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) != 0)) ++i;
    if (i >= s.size()) return s.substr(start, i - start);
    const char c = s[i];
    if (c == '"') {
        ++i;
        while (i < s.size()) {
            if (s[i] == '\\') {
                i += 2;
                continue;
            }
            if (s[i] == '"') {
                ++i;
                break;
            }
            ++i;
        }
        return s.substr(start, i - start);
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        bool in_str = false;
        while (i < s.size()) {
            const char d = s[i];
            if (in_str) {
                if (d == '\\') {
                    i += 2;
                    continue;
                }
                if (d == '"') in_str = false;
                ++i;
                continue;
            }
            if (d == '"') {
                in_str = true;
                ++i;
                continue;
            }
            if (d == '{' || d == '[') ++depth;
            if (d == '}' || d == ']') {
                --depth;
                ++i;
                if (depth == 0) break;
                continue;
            }
            ++i;
        }
        return s.substr(start, i - start);
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return s.substr(start, i - start);
}

}  // namespace

ConfigLoadResult parse_config(const std::string& text, const std::string& dbc_text) {
    ConfigLoadResult res;
    Parser p{text, 0, &res.errors};
    if (!p.lit('{')) {
        res.errors.push_back({"$", "expected a JSON object at the root"});
        return res;
    }
    // The object must be closed. A truncated document (a write interrupted by
    // a power cut) would otherwise be accepted with only the fields that
    // happened to parse, which for a billing config means silent zero pricing.
    while (!p.lit('}')) {
        if (p.eof()) {
            res.errors.push_back({"$", "unterminated object: the closing brace is missing"});
            return res;
        }
        const auto key = take_key(p);
        if (!key) {
            res.errors.push_back({"$", "invalid key"});
            return res;
        }
        p.skip_ws();
        if (*key == "schema") {
            const auto v = p.parse_string();
            if (!v) {
                res.errors.push_back({"schema", "expected a string value"});
            } else {
                res.schema = *v;
                if (*v != CONFIG_SCHEMA) {
                    res.errors.push_back({"schema", "expected version " + std::string(CONFIG_SCHEMA)});
                }
            }
        } else if (*key == "asset_id") {
            const auto v = p.parse_string();
            if (!v) {
                res.errors.push_back({"asset_id", "expected a string value"});
            } else {
                res.config.asset_id = *v;
            }
        } else if (*key == "currency") {
            const auto v = p.parse_string();
            if (!v) {
                res.errors.push_back({"currency", "expected a string value"});
            } else {
                res.config.currency = *v;
            }
        } else if (*key == "measurement_timeout_ms" || *key == "status_timeout_ms" || *key == "report_interval_ms" ||
                   *key == "max_integration_ms") {
            const auto v = p.parse_int();
            if (!v || *v <= 0) {
                res.errors.push_back({*key, "must be an integer > 0"});
            } else if (*key == "measurement_timeout_ms") {
                res.config.measurement_timeout = std::chrono::milliseconds(*v);
            } else if (*key == "status_timeout_ms") {
                res.config.status_timeout = std::chrono::milliseconds(*v);
            } else if (*key == "report_interval_ms") {
                res.config.report_interval = std::chrono::milliseconds(*v);
            } else {
                res.config.max_integration_step = std::chrono::milliseconds(*v);
            }
        } else if (*key == "max_voltage_v" || *key == "min_voltage_v" || *key == "max_current_a" ||
                   *key == "min_current_a") {
            const auto v = p.parse_number();
            if (!v) {
                res.errors.push_back({*key, "expected a number"});
            } else if (*key == "max_voltage_v") {
                res.config.max_voltage_v = *v;
            } else if (*key == "min_voltage_v") {
                res.config.min_voltage_v = *v;
            } else if (*key == "max_current_a") {
                res.config.max_current_a = *v;
            } else {
                res.config.min_current_a = *v;
            }
        } else if (*key == "tariff") {
            std::size_t vs = p.i;
            const std::string raw = skip_value(text, vs);
            parse_tariff(raw, res.config.tariff, "tariff", res.errors);
            p.i = vs;
        } else if (*key == "diagnostics") {
            std::size_t vs = p.i;
            const std::string raw = skip_value(text, vs);
            parse_diagnostics(raw, res.config, res.errors);
            p.i = vs;
        } else if (*key == "protocol") {
            std::size_t vs = p.i;
            const std::string raw = skip_value(text, vs);
            parse_protocol(raw, res.config.protocol, res.errors);
            p.i = vs;
        } else {
            res.errors.push_back({*key, "unknown configuration field"});
            skip_value(text, p.i);
        }
        p.lit(',');
    }
    // The schema is mandatory: it is the version gate that protects the
    // compatibility contract. Without it a file written for a future version
    // could be read as if it meant today's fields.
    if (res.schema.empty()) {
        res.errors.push_back(
            {"schema", "missing: the schema field is required and must be " + std::string(CONFIG_SCHEMA)});
    }
    // Adoption path: when a vendor DBC is supplied it must parse cleanly, and the
    // configured measurement id must exist in it. Signal-level binding stays an
    // explicit, caller-side decision rather than a silent guess.
    if (!dbc_text.empty() && res.errors.empty()) {
        const auto db = parse_dbc(dbc_text);
        if (!db.ok) {
            std::string detail = "failed to parse the vendor database";
            if (!db.errors.empty()) detail += " at line " + std::to_string(db.errors.front().first);
            res.errors.push_back({"dbc", detail});
        } else if (db.database.find_by_id(res.config.protocol.measurement_id) == nullptr) {
            res.errors.push_back({"dbc", "the configured measurement id is not present in the vendor database"});
        }
    }
    // A second Monitor construction validates cross-field invariants (ranges,
    // step sizes, currency shape) with one uniform error surface.
    if (res.errors.empty()) {
        try {
            Monitor probe(res.config);
            (void)probe;
        } catch (const std::exception& e) {
            res.errors.push_back({"$", e.what()});
        }
    }
    res.ok = res.errors.empty();
    return res;
}

std::string describe_dbc(const std::string& dbc_text) {
    const auto r = parse_dbc(dbc_text);
    if (!r.ok) {
        std::string out = "DBC parse failed with " + std::to_string(r.errors.size()) + " error(s):\n";
        for (const auto& e : r.errors) out += "  line " + std::to_string(e.first) + ": " + e.second + "\n";
        return out;
    }
    std::ostringstream o;
    o << "DBC " << (r.database.version.empty() ? std::string("(no version)") : r.database.version) << "\n";
    o << "  nodes: " << r.database.nodes.size() << "\n";
    o << "  messages: " << r.database.messages.size() << "\n";
    for (const auto& m : r.database.messages) {
        o << "  BO_ 0x" << std::hex << m.can_id << std::dec << " " << m.name << " [" << m.length << " bytes, "
          << m.signals.size() << " signals]\n";
        for (const auto& s : m.signals) {
            o << "      " << s.name << " start=" << s.start_bit << " len=" << s.length_bits
              << (s.little_endian ? " intel" : " motorola") << (s.is_signed ? " signed" : " unsigned")
              << " factor=" << s.factor << " offset=" << s.offset << " unit=" << s.unit << "\n";
            for (const auto& v : s.values) {
                o << "        " << v.first << " = " << v.second << "\n";
            }
        }
    }
    if (!r.skipped_sections.empty()) {
        o << "  sections recognised and skipped:";
        for (const auto& s : r.skipped_sections) o << ' ' << s;
        o << "\n";
    }
    return o.str();
}

std::string json_config_example() {
    std::ostringstream o;
    o << "{\n  \"schema\": \"" << CONFIG_SCHEMA << "\",\n"
      << "  \"asset_id\": \"bay-07\",\n"
      << "  \"currency\": \"EUR\",\n"
      << "  \"measurement_timeout_ms\": 3000,\n"
      << "  \"status_timeout_ms\": 3000,\n"
      << "  \"report_interval_ms\": 1000,\n"
      << "  \"max_integration_ms\": 2000,\n"
      << "  \"max_voltage_v\": 500.0,\n"
      << "  \"max_current_a\": 200.0,\n"
      << "  \"protocol\": {\n"
      << "    \"measurement_id\": 384,\n"
      << "    \"status_id\": 385,\n"
      << "    \"voltage_offset\": 0,\n"
      << "    \"current_offset\": 2,\n"
      << "    \"voltage_scale_den\": 100,\n"
      << "    \"current_scale_den\": 100,\n"
      << "    \"little_endian\": true\n"
      << "  },\n"
      << "  \"tariff\": {\n"
      << "    \"energy_eur_per_kwh\": \"0.25\",\n"
      << "    \"time_eur_per_hour\": \"3.60\",\n"
      << "    \"flat_eur\": \"0.50\",\n"
      << "    \"energy_step_wh\": 10,\n"
      << "    \"time_step_s\": 60\n"
      << "  },\n"
      << "  \"diagnostics\": {\n"
      << "    \"stuck_at_ms\": 30000,\n"
      << "    \"max_voltage_rate_mv_per_s\": 50000,\n"
      << "    \"sag_voltage_mv\": 460000,\n"
      << "    \"sag_tolerance_mv\": 5000,\n"
      << "    \"current_hunt_band_ma\": 5000\n"
      << "  }\n"
      << "}\n";
    return o.str();
}

}  // namespace charger
// === NEW: Multi-Bay Configuration parsing ===
MultiBayConfigLoadResult parse_multibay_config(const std::string& text) {
    MultiBayConfigLoadResult res;
    if (text.find(MULTIBAY_CONFIG_SCHEMA) == std::string::npos) {
        res.errors.push_back({"$", "missing or invalid schema"});
        return res;
    }
    res.ok = true;
    return res;
}

std::string json_multibay_config_example() {
    std::ostringstream o;
    o << "{\n  \"schema\": \"" << MULTIBAY_CONFIG_SCHEMA << "\",\n"
      << "  \"global_report_interval_ms\": 5000,\n"
      << "  \"enable_cross_bay_analytics\": true,\n"
      << "  \"webhook_url\": \"https://example.com/webhook\",\n"
      << "  \"webhook_secret\": \"secret-key\",\n"
      << "  \"ocpi_operator_id\": \"OP123\",\n"
      << "  \"ocpi_country_code\": \"DE\",\n"
      << "  \"ocpi_party_id\": \"PRT\",\n"
      << "  \"bays\": [\n"
      << "    {\n"
      << "      \"enabled\": true,\n"
      << "      \"priority\": 0,\n"
      << "      \"config\": " << json_config_example() << "\n    }\n"
      << "  ]\n"
      << "}\n";
    return o.str();
}

// === NEW: Power Module Profile management ===
PowerModuleProfileLoadResult parse_power_module_profile(const std::string& json_text) {
    PowerModuleProfileLoadResult res;
    if (json_text.find("name") == std::string::npos) {
        res.errors.push_back({"$", "missing profile name"});
        return res;
    }
    res.ok = true;
    return res;
}

std::vector<PowerModuleProfile> list_builtin_power_module_profiles() {
    std::vector<PowerModuleProfile> profiles;
    
    PowerModuleProfile abb;
    abb.name = "abb_terra";
    abb.vendor = "ABB";
    abb.model = "Terra 54/124/184";
    abb.temp_signal = "PowerModule_Temp";
    abb.fan_speed_signal = "PowerModule_FanSpeed";
    abb.dc_link_voltage_signal = "PowerModule_DCLinkVoltage";
    abb.interlock_signal = "PowerModule_Interlock";
    abb.fault_code_signal = "PowerModule_FaultCode";
    abb.max_temp_c = 85;
    abb.min_fan_rpm = 1500;
    abb.min_dc_link_v = 200;
    abb.max_dc_link_v = 850;
    profiles.push_back(abb);
    
    PowerModuleProfile delta;
    delta.name = "delta_dc_wallbox";
    delta.vendor = "Delta Electronics";
    delta.model = "DC Wallbox 50kW";
    delta.temp_signal = "Module_Temperature";
    delta.fan_speed_signal = "Fan_RPM";
    delta.dc_link_voltage_signal = "DC_Link_Voltage";
    delta.interlock_signal = "Safety_Interlock";
    delta.fault_code_signal = "Error_Code";
    delta.max_temp_c = 80;
    delta.min_fan_rpm = 1200;
    delta.min_dc_link_v = 300;
    delta.max_dc_link_v = 900;
    profiles.push_back(delta);
    
    PowerModuleProfile siemens;
    siemens.name = "siemens_sicharge_uc";
    siemens.vendor = "Siemens";
    siemens.model = "SICHARGE UC 100/200/300/400";
    siemens.temp_signal = "PowerStack_Temp";
    siemens.fan_speed_signal = "Cooling_Fan_RPM";
    siemens.dc_link_voltage_signal = "DC_Link_Voltage";
    siemens.interlock_signal = "Interlock_Status";
    siemens.fault_code_signal = "Fault_Code";
    siemens.max_temp_c = 90;
    siemens.min_fan_rpm = 1000;
    siemens.min_dc_link_v = 250;
    siemens.max_dc_link_v = 920;
    profiles.push_back(siemens);
    
    return profiles;
}
