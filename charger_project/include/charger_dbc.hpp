#pragma once

// DBC (Vector CAN database) reader.
//
// A real fleet is described by a .dbc file, not by invented identifiers. This
// reader consumes the subset an industrial bay monitor actually needs:
//
//   BO_   message definition (CAN id, length)
//   SG_   signal definition with start bit, length, byte order, signedness,
//         factor and offset
//   VAL_  enumerated value tables, which is how an OEM states the power-stage
//         status codes
//
// Motorola (big-endian) bit numbering is implemented, because most industrial
// OEM signals use it and getting it wrong silently decodes nonsense.
//
// Scope: the message/signal/value core. Node lists, attributes, environment
// variables, value tables and multiplexing are recognised and skipped rather
// than rejected, so a real OEM file loads instead of failing on a feature this
// reader does not need yet.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "charger_monitor.hpp"

namespace charger {

// One signal inside a message, in DBC terms.
struct DbcSignal {
    std::string name;
    std::uint32_t start_bit{0};
    std::uint32_t length_bits{0};
    bool little_endian{true};
    bool is_signed{false};
    double factor{1.0};
    double offset{0.0};
    double minimum{0.0};
    double maximum{0.0};
    std::string unit;
    // Enumerated raw value -> label, from VAL_ tables.
    std::vector<std::pair<std::int64_t, std::string>> values;

    // Decodes the raw integer carried by this signal out of a CAN payload.
    // Returns nullopt when the payload is too short for the signal.
    std::optional<std::int64_t> decode_raw(const std::uint8_t* data, std::size_t length) const;

    // physical = raw * factor + offset
    double to_physical(std::int64_t raw) const { return static_cast<double>(raw) * factor + offset; }

    // Looks up an enumerated label. Returns empty when the value is not listed.
    std::string label_for(std::int64_t raw) const;
};

struct DbcMessage {
    std::uint32_t can_id{0};
    std::string name;
    std::size_t length{0};
    std::vector<DbcSignal> signals;

    const DbcSignal* find_signal(const std::string& signal_name) const;
};

struct DbcDatabase {
    std::string version;
    std::vector<DbcMessage> messages;
    std::vector<std::string> nodes;

    const DbcMessage* find_by_id(std::uint32_t can_id) const;
    const DbcMessage* find_by_name(const std::string& message_name) const;
};

struct DbcLoadResult {
    bool ok{false};
    DbcDatabase database;
    // Non-fatal notes: sections recognised and skipped.
    std::vector<std::string> skipped_sections;
    // Fatal errors, located by line number (1-based).
    std::vector<std::pair<int, std::string>> errors;
};

// The extended-id bit that Vector uses in DBC files. Strip it before matching
// a standard 11-bit CAN id.
constexpr std::uint32_t kDbcExtendedFlag = 0x80000000u;

DbcLoadResult parse_dbc(const std::string& text);

// Names a message that carries exactly one voltage and one current signal.
// This is the adoption path: point it at a vendor DBC and the monitor decodes
// that protocol instead of the demo one.
struct DbcBinding {
    std::uint32_t measurement_id{0};
    std::uint32_t status_id{0};
    std::size_t measurement_length{0};
    std::string voltage_signal;
    std::string current_signal;
    std::string status_signal;
};

std::optional<DbcBinding> auto_bind(const DbcDatabase& db, double min_voltage_v, double max_voltage_v,
                                    double min_current_a, double max_current_a);

// DBC-agnostic shape of auto_bind: needed when the operator names the signals
// explicitly instead of trusting the range heuristic.
bool bind_signals(const DbcDatabase& db, const std::string& voltage_signal, const std::string& current_signal,
                  DbcBinding& out, std::string& error);

// Names a message whose signal carries the power-stage status as an
// enumerated value table (VAL_). Such a table is how an OEM states the status
// codes, so the labels are translated into the kStatus* meanings below.
struct DbcStatusBinding {
    std::uint32_t can_id{0};
    std::size_t length{0};
    std::string signal;
};

// Picks the best status carrier: any message whose signal has a value table
// that maps to at least two distinct meanings (OFF/READY/CHARGING/FAULT).
std::optional<DbcStatusBinding> auto_bind_status(const DbcDatabase& db);

// Outcome of binding a DBC onto the wire-level decode parameters.
struct DbcProfileResult {
    bool ok{false};
    ProtocolProfile profile{};
    // True when the status signal was bound and its layer map was built from
    // the value table. False means the profile kept the default demo status
    // layout, which is an adoption decision the caller must make knowingly.
    bool status_bound{false};
    std::string error;
};

// Converts a DBC binding into a ProtocolProfile: CAN ids, byte offsets,
// endianness and exact rational scales from the declared factor, and the
// status map from the enumerated value table. Refuses rather than guesses:
// an approximate decode of a billing signal silently bills wrong numbers.
DbcProfileResult profile_from_dbc(const DbcDatabase& db, const DbcBinding& binding);

}  // namespace charger