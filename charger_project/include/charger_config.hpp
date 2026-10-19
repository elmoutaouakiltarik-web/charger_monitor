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

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations for types defined in charger_monitor.hpp
namespace charger {
struct Config;
struct MultiBayConfig;
struct PowerModuleProfile;
}  // namespace charger

#include "charger_monitor.hpp"

namespace charger {

constexpr const char* CONFIG_SCHEMA = "charger.config.v1";

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

// Multi-Bay Configuration parsing
struct MultiBayConfigLoadResult {
    bool ok{false};
    std::unique_ptr<MultiBayConfig> config;
    std::vector<ConfigError> errors;
};
MultiBayConfigLoadResult parse_multibay_config(const std::string& text);
std::string json_multibay_config_example();

// Power Module Profile management
struct PowerModuleProfileLoadResult {
    bool ok{false};
    std::unique_ptr<PowerModuleProfile> profile;
    std::vector<ConfigError> errors;
};
PowerModuleProfileLoadResult parse_power_module_profile(const std::string& json_text);
std::string json_power_module_profile_example();

std::string json_config_example();

// Human-readable summary of a loaded DBC, for the CLI.
std::string describe_dbc(const std::string& dbc_text);

}  // namespace charger