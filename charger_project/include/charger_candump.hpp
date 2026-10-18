#pragma once

// Reader for `candump` recordings, so a bay recorded on site can be replayed in
// the office with the REAL timing:
//
//   candump -L can0 > bay.log          (log format, absolute timestamps)
//   candump -ta can0 > bay.txt         (default format with an absolute stamp)
//   candump can0 > bay.txt             (default format, no time)
//
// Log format:      (1436509052.249713) vcan0 044#2A366C2BBA
//                  (…) can0 18FEF100#0102        29-bit id: 8 hex digits
//                  (…) can0 123#R                remote frame
//                  (…) can0 123##1AABB           CAN FD (flagged, never decoded)
//                  (…) can0 20000004#00…         error frame (CAN_ERR_FLAG)
// Default format:  "  can0  180   [8]  80 BB E8 03 00 00 00 00", optionally
//                  preceded by "(1436509052.249713)" when recorded with -ta.
//
// Timestamps are parsed with integer arithmetic to microseconds: a bit-exact
// replay must not pass through a double. This module only reads text: nothing
// here can transmit on a bus.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace charger {

struct CandumpRecord {
    bool has_time{false};
    std::int64_t time_us{0};  // microseconds since the Unix epoch, when has_time
    std::string iface;
    std::uint32_t id{0};
    bool extended{false};     // 29-bit identifier
    bool rtr{false};          // remote frame: carries no data
    bool fd{false};           // CAN FD frame: recognised, never decoded (length stays 0)
    bool error_frame{false};  // kernel error frame (CAN_ERR_FLAG)
    std::uint8_t length{0};
    std::array<std::uint8_t, 8> data{};
};

// Returns nullopt for anything that is not a well-formed record: junk, a blank
// line, odd hex, more than 8 data bytes on a classic frame, an out-of-range
// identifier or timestamp. Never throws.
std::optional<CandumpRecord> parse_candump_line(const std::string& line);

// "2015-07-10T06:17:32Z" for 1436509052249713, using explicit calendar
// arithmetic (no libc time-zone state), the same style as utc_timestamp().
std::string utc_from_epoch_us(std::int64_t epoch_us);

}  // namespace charger
