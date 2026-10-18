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
    // Hash of the last chained line (genesis when the journal is empty).
    std::string last_hash;
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
    // Tariff snapshot embedded in the close line (journals written by v1.4.0
    // and later carry it by default). A dispute pack is verified against the
    // tariff that was actually in force, not the one running today.
    std::string tariff_json;
    // Provenance of the two journal lines that constitute the session, so an
    // auditor can recompute the chain links with standard tools. Zero or empty
    // for journals written before v1.4.0.
    std::uint64_t open_line{0};
    std::uint64_t close_line{0};
    std::string close_prev_hash;
    std::string close_hash;
    std::string close_line_raw;
};

// Rebuilds, for one recovered session, the complete evidence pack used to
// answer a billing dispute: meter values, the tariff of record, the
// calculation, and the chain provenance with the verification recipe. The
// journal_sha256_hex is the digest of the whole journal file, the first thing
// an auditor checks.
//
// What `chain.chain_intact` proves is internal consistency: that no line was
// edited, inserted, reordered or removed without breaking the chain. It is not
// an authenticity claim. The chain is anchored to a public genesis constant, so
// a wholesale rewrite recomputed from scratch would also verify. Detecting that
// needs an anchor outside this file.
std::string json_dispute(const SettledSession& session, const RecoveryReport& chain, const std::string& journal_path,
                         const std::string& journal_sha256_hex);

// External anchor for the hash chain: the chain alone proves internal
// consistency, not authenticity (a full rewrite recomputes cleanly). An anchor
// pins the state of the first N lines at a point in time into a separate file
// that lives on append-only storage (WORM share, object-lock bucket, printed
// and filed digest). Later verification proves the prefix was not rewritten
// while still allowing the journal to grow: line N must still hash to
// last_hash. An anchor is exact-state evidence, not a signature: key
// management stays outside this library.
constexpr const char* ANCHOR_SCHEMA = "charger.anchor.v1";

struct JournalAnchor {
    std::string journal_sha256;
    std::string last_hash;
    std::uint64_t lines{0};
    std::string anchored_at_utc;
};

// Verifies the chain, then writes the anchor. Fails rather than anchoring a
// broken journal: anchoring tampered history would launder it.
bool write_journal_anchor(const std::string& journal_path, const std::string& anchor_path, JournalAnchor& out,
                          std::string& error);

// Verifies the chain, then checks the anchor prefix: the line numbered
// anchor.lines must still hash to anchor.last_hash. Appends after the anchor
// are reported via appended_lines and are not a failure: the anchor proves
// the prefix, the chain proves the suffix links to it.
bool verify_journal_anchor(const std::string& journal_path, const std::string& anchor_path,
                           std::uint64_t& appended_lines, std::string& error);

// OCPI CDR subset exporter (charger.cdr.v1): the local record mapped onto the
// interchange vocabulary the backend bills from. session start is derived as
// close minus duration (the journal carries a wall-clock label only for the
// close); it is labelled derived_start_utc so a backend never mistakes it for
// a measured start. VAT omitted (not zero-rated). Signed exchange stays in
// the backend: this is the evidence, not the wire format.
std::string json_cdr(const SettledSession& session);

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

    // Appends a line and reports whether it reached the journal. false means the
    // bytes are NOT durable: a partial write has been rolled back to the last
    // good line, so the file is still verifiable and no valid data was removed.
    // Ignoring the result is how a caller ends up believing it recorded a
    // settlement it never wrote.
    bool append_session_open(const SessionRecord& rec, const std::string& asset_id, const std::string& opened_at_utc);
    bool append_session_close(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                              const std::string& closed_at_utc, const std::string& tariff_json = {});

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
    std::string context_tariff_;
    bool fsync_on_commit_{true};

    bool append(const std::string& type, const std::string& asset, const SessionRecord& rec,
                const std::string& json_payload);
    void track(const std::string& asset, const SessionRecord& rec, const std::string& type);
};

}  // namespace charger