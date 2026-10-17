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
ConfigLoadResult parse_config(const std::string& text);

std::string json_config_example();

}  // namespace charger