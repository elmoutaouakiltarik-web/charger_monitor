#pragma once

// Versioned JSON configuration.
//
// Real deployments cannot be configured by CLI flags alone: a site has dozens of
// bays with different tariffs, thresholds and protocol profiles. This module
// loads a versioned config file so a deployment is reviewable and diffable.
//
// The parser is deliberately small and strict. It accepts exactly the value
// shapes this schema needs and rejects anything else with a located error,
// rather than silently ignoring a typo in a tariff. A config that parses is a
// config that means what it says.

#include <optional>
#include <string>
#include <vector>
#include <chrono>

#include "charger_monitor.hpp"

namespace charger {

constexpr const char* CONFIG_SCHEMA = "charger.config.v1";
constexpr const char* MULTIBAY_CONFIG_SCHEMA = "charger.multibay.config.v1";

struct ConfigError {
    std::string field;
    std::string message;
};

struct ConfigLoadResult {
    bool ok{false};
    Config config{};
    std::string schema;
    std::vector<ConfigError> errors;
};

// Parses a config document. Every field is optional: anything absent keeps the
// library default, so a minimal file is valid. Unknown fields are reported as
// errors, because a misspelled tariff must not silently stay at zero.
//
// When `dbc_text` is non-empty the vendor DBC is parsed and bound, so a bay can
// be adopted by pointing at the manufacturer's database file. Signals named in
// the config must exist in the DBC, otherwise the config is rejected.
ConfigLoadResult parse_config(const std::string& text, const std::string& dbc_text = std::string());

// === NEW: Multi-Bay Configuration types ===
struct BayConfig {
    Config config;               // Per-bay configuration
    bool enabled{true};          // Whether this bay is active
    int priority{0};             // Processing priority (higher = more frequent)
};

struct MultiBayConfig {
    std::string schema{MULTIBAY_CONFIG_SCHEMA};
    std::vector<BayConfig> bays;
    // Global settings
    std::chrono::milliseconds global_report_interval{5000};
    bool enable_cross_bay_analytics{true};
    std::string webhook_url{ "" };
    std::string webhook_secret{ "" };
    // Fleet-level OCPI metadata
    std::string ocpi_operator_id{ "" };
    std::string ocpi_country_code{ "" };
    std::string ocpi_party_id{ "" };
};

struct MultiBayConfigLoadResult {
    bool ok{false};
    MultiBayConfig config{};
    std::vector<ConfigError> errors;
};
MultiBayConfigLoadResult parse_multibay_config(const std::string& text);
std::string json_multibay_config_example();

// === NEW: Power Module Profile types ===
struct PowerModuleProfile {
    std::string name;
    std::string vendor;
    std::string model;
    std::string dbc_text;        // Embedded DBC content
    // Signal mappings for power module telemetry
    std::string temp_signal;     // Temperature signal name
    std::string fan_speed_signal;
    std::string dc_link_voltage_signal;
    std::string interlock_signal;
    std::string fault_code_signal;
    // Thresholds
    std::int32_t max_temp_c{80};
    std::int32_t min_fan_rpm{1000};
    std::int32_t min_dc_link_v{300};
    std::int32_t max_dc_link_v{900};
};

struct PowerModuleProfileLoadResult {
    bool ok{false};
    PowerModuleProfile profile{};
    std::vector<ConfigError> errors;
};
PowerModuleProfileLoadResult parse_power_module_profile(const std::string& json_text);
std::vector<PowerModuleProfile> list_builtin_power_module_profiles();

std::string json_config_example();

// Human-readable summary of a loaded DBC, for the CLI.
std::string describe_dbc(const std::string& dbc_text);

}  // namespace charger