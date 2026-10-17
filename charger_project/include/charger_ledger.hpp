#pragma once

// Durable, tamper-evident session journal.
//
// The journal is the billing system of record. Losing it to a power blip or
// editing it after a dispute are the two failure modes that cost real money,
// so this module addresses both:
//   * every line is hash-chained (SHA-256) and can be verified offline;
//   * every committed line is flushed and fsynced, so a crash loses at most the
//     line being written, never an earlier one;
//   * on startup the journal is replayed to restore lifetime counters and to
//     report sessions that were opened but never closed.
//
// Line layout (tabs), chosen so recovery needs no JSON parser:
//   prev_hash \t hash \t type \t asset \t session_id \t energy_uwh \t
//   duration_ms \t cost_micro_eur \t billable \t json_payload
//   hash = SHA256(prev_hash "\t" <everything after the hash field>)
//
// NOT a safety function and NOT a legal-metrology record. NOT cryptographically
// non-repudiable: the chain detects casual edits and truncated histories, not an
// attacker who can rewrite the whole file.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"

namespace charger {

constexpr const char* LEDGER_SCHEMA = "charger.ledger.v1";
// First line links to this hash: it proves the journal was not prepended to.
constexpr const char* LEDGER_GENESIS_HASH = "0000000000000000000000000000000000000000000000000000000000000000";

// Compact SHA-256. Exposed because the chain verification must be reproducible
// by an auditor with any standard tool, not only inside this binary.
std::string sha256_hex(const std::string& data);

struct RecoveryReport {
    std::uint64_t lines{0};
    std::uint64_t session_opens{0};
    std::uint64_t session_closes{0};
    std::uint64_t energy_uwh{0};
    std::uint64_t billable_sessions{0};
    std::uint64_t revenue_micro_eur{0};
    // Sessions present in the journal with an open and no matching close: the
    // process died mid-session. Their delivered energy is unknown.
    std::vector<std::pair<std::string, std::uint64_t>> orphan_sessions;
    // First line whose hash does not chain, or 0 when the chain is intact.
    std::uint64_t first_broken_line{0};
    bool chain_intact{true};
    bool truncated_last_line{false};
};

// One closed session as recovered from the journal, with the fields a billing
// settlement needs. Kept separate from SessionRecord so a settlement tool never
// has to re-parse the JSON payload.
struct SettledSession {
    std::string asset_id;
    std::uint64_t id{0};
    SessionRecord record{};
    std::string currency;
    std::string closed_at_utc;
};

class Ledger {
   public:
    Ledger() = default;
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) noexcept;
    Ledger& operator=(Ledger&&) noexcept;

    ~Ledger();

    static std::optional<Ledger> open(const std::string& path, bool fsync_on_commit, std::string& error);
    static std::optional<Ledger> open_memory_only();

    // Replays the journal, restores the lifetime counters and reports orphans.
    RecoveryReport recover();

    // Recomputes every hash from the genesis link. Returns false and sets
    // error on the first mismatch, naming the line.
    bool verify(std::string& error) const;

    void append_session_open(const SessionRecord& rec, const std::string& asset_id, const std::string& opened_at_utc);
    void append_session_close(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                              const std::string& closed_at_utc);

    std::uint64_t lines_written() const noexcept { return lines_written_; }
    const std::string& path() const noexcept { return path_; }

    // Portfolio rollup rebuilt purely from the journal: survives restarts and
    // needs no running process.
    std::optional<PortfolioKpi> portfolio() const;

    // Every closed session, in journal order. This is the input a settlement or
    // invoicing run consumes, so the caller never parses JSON itself.
    const std::vector<SettledSession>& sessions() const noexcept { return sessions_; }

   private:
    std::string path_;
    std::string last_hash_{LEDGER_GENESIS_HASH};
    std::uint64_t lines_written_{0};
    std::uint64_t energy_uwh_{0};
    std::uint64_t billable_{0};
    std::uint64_t revenue_{0};
    std::uint64_t closes_{0};
    std::uint64_t opens_{0};
    std::vector<std::pair<std::string, std::uint64_t>> open_sessions_;
    std::vector<PortfolioAsset> per_asset_;
    std::vector<SettledSession> sessions_;
    // Set by the public append_* methods immediately before append() so the
    // tab-delimited prefix can carry them without parsing the JSON payload.
    std::string context_currency_;
    std::string context_timestamp_;
    bool fsync_on_commit_{true};

    bool append(const std::string& type, const std::string& asset, const SessionRecord& rec,
                const std::string& json_payload);
    void track(const std::string& asset, const SessionRecord& rec, const std::string& type);
};

}  // namespace charger