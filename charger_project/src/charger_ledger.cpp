#include "charger_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "charger_portfolio.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace charger {
namespace {

constexpr const char* kTab = "\t";
constexpr std::int64_t kMicroPerEuro = 1000000;

std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

// FIPS 180-4 constants.
constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

void sha256_block(std::uint32_t state[8], const unsigned char block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// Makes the bytes written so far durable. Returns false if any step failed, so
// the caller can refuse to call a line recorded. An unchecked fsync would let a
// full disk or a revoked permission pass for a durable commit.
bool sync_file(std::FILE* f) {
    if (f == nullptr) return false;
    // Flush the userspace buffer first: fsync only covers what reached the
    // kernel, so syncing before the flush would persist nothing.
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

// Size of the file in bytes, or -1 when it cannot be established.
std::int64_t file_size_of(const std::string& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    if (ec) return -1;
    return static_cast<std::int64_t>(n);
}

// Drops everything past `size` bytes, so a torn write leaves no half line for
// the next append to build on. Best effort: the caller has already lost the
// write and must report it either way.
bool truncate_file(std::FILE* f, std::int64_t size) {
    if (f == nullptr || size < 0) return false;
#if defined(_WIN32)
    return _chsize_s(_fileno(f), static_cast<std::int64_t>(size)) == 0;
#else
    return ::ftruncate(::fileno(f), static_cast<::off_t>(size)) == 0;
#endif
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (v > (0xffffffffffffffffull - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

// Journal payloads and dispute documents must be exact even for hand-edited
// text fields, so strings are escaped, not trusted.
std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o << "\\\"";
                break;
            case '\\':
                o << "\\\\";
                break;
            case '\b':
                o << "\\b";
                break;
            case '\f':
                o << "\\f";
                break;
            case '\n':
                o << "\\n";
                break;
            case '\r':
                o << "\\r";
                break;
            case '\t':
                o << "\\t";
                break;
            default:
                if (c < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    o << static_cast<char>(c);
                }
        }
    }
    return o.str();
}

}  // namespace

std::string sha256_hex(const std::string& data) {
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t full = data.size() / 64;
    for (std::size_t i = 0; i < full; ++i) sha256_block(state, bytes + i * 64);

    unsigned char tail[128] = {0};
    const std::size_t rest = data.size() % 64;
    if (rest != 0) std::memcpy(tail, bytes + full * 64, rest);
    tail[rest] = 0x80;
    const std::size_t tail_len = (rest < 56) ? 64 : 128;
    const std::uint64_t bitlen = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i)
        tail[tail_len - 1 - static_cast<std::size_t>(i)] = static_cast<unsigned char>(bitlen >> (8 * i));
    for (std::size_t off = 0; off < tail_len; off += 64) sha256_block(state, tail + off);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t v : state) {
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(v >> shift) & 0xfu]);
    }
    return out;
}

Ledger::Ledger(Ledger&& other) noexcept
    : path_(std::move(other.path_)),
      last_hash_(std::move(other.last_hash_)),
      lines_written_(other.lines_written_),
      energy_uwh_(other.energy_uwh_),
      billable_(other.billable_),
      revenue_(other.revenue_),
      closes_(other.closes_),
      opens_(other.opens_),
      open_sessions_(std::move(other.open_sessions_)),
      per_asset_(std::move(other.per_asset_)),
      sessions_(std::move(other.sessions_)),
      context_currency_(std::move(other.context_currency_)),
      context_timestamp_(std::move(other.context_timestamp_)),
      context_tariff_(std::move(other.context_tariff_)),
      fsync_on_commit_(other.fsync_on_commit_) {
    other.path_.clear();
    other.lines_written_ = 0;
}

Ledger& Ledger::operator=(Ledger&& other) noexcept {
    if (this != &other) {
        path_ = std::move(other.path_);
        last_hash_ = std::move(other.last_hash_);
        lines_written_ = other.lines_written_;
        energy_uwh_ = other.energy_uwh_;
        billable_ = other.billable_;
        revenue_ = other.revenue_;
        closes_ = other.closes_;
        opens_ = other.opens_;
        open_sessions_ = std::move(other.open_sessions_);
        per_asset_ = std::move(other.per_asset_);
        sessions_ = std::move(other.sessions_);
        context_currency_ = std::move(other.context_currency_);
        context_timestamp_ = std::move(other.context_timestamp_);
        context_tariff_ = std::move(other.context_tariff_);
        fsync_on_commit_ = other.fsync_on_commit_;
        other.path_.clear();
        other.lines_written_ = 0;
    }
    return *this;
}

// No destructor work: every append() fcloses its own handle, so there is
// nothing buffered to flush. An fsync here would need a fresh fopen and would
// leak the descriptor.
Ledger::~Ledger() = default;

std::optional<Ledger> Ledger::open_memory_only() {
    Ledger l;
    l.last_hash_ = LEDGER_GENESIS_HASH;
    return l;
}

std::optional<Ledger> Ledger::open(const std::string& path, bool fsync_on_commit, std::string& error) {
    if (path.empty()) {
        error = "Empty ledger path";
        return std::nullopt;
    }
    Ledger l;
    l.path_ = path;
    l.fsync_on_commit_ = fsync_on_commit;
    l.last_hash_ = LEDGER_GENESIS_HASH;
    return l;
}

void Ledger::track(const std::string& asset, const SessionRecord& rec, const std::string& type) {
    if (type == "session_open") {
        ++opens_;
        for (const auto& o : open_sessions_) {
            if (o.first == asset && o.second == rec.id) return;
        }
        open_sessions_.emplace_back(asset, rec.id);
        return;
    }
    ++closes_;
    energy_uwh_ += rec.energy_uwh;
    if (rec.billing_complete) ++billable_;
    revenue_ += static_cast<std::uint64_t>(rec.total_cost_micro_eur);
    for (auto it = open_sessions_.begin(); it != open_sessions_.end(); ++it) {
        if (it->first == asset && it->second == rec.id) {
            open_sessions_.erase(it);
            break;
        }
    }
    Portfolio p;
    p.add(rec, asset);
    const auto s = p.summary();
    if (s.per_asset.empty()) return;
    const PortfolioAsset& incoming = s.per_asset.front();
    for (auto& existing : per_asset_) {
        if (existing.asset_id != incoming.asset_id) continue;
        existing.sessions_closed += incoming.sessions_closed;
        existing.sessions_billable += incoming.sessions_billable;
        existing.energy_uwh += incoming.energy_uwh;
        existing.revenue_micro_eur += incoming.revenue_micro_eur;
        existing.charging_ms += incoming.charging_ms;
        existing.fault_stops += incoming.fault_stops;
        existing.timeout_stops += incoming.timeout_stops;
        existing.undersampled_sessions += incoming.undersampled_sessions;
        return;
    }
    per_asset_.push_back(incoming);
}

bool Ledger::append(const std::string& type, const std::string& asset, const SessionRecord& rec,
                    const std::string& json_payload) {
    // Prefix layout, extended forward-compatibly: the first nine fields are the
    // v1.3.0 contract and never change, so an older reader still finds them in
    // place and a newer reader treats missing trailing fields as zero.
    // stop_reason, asset, quality, currency and the timestamp are tab-free by
    // construction, so they are safe in the prefix.
    std::ostringstream rest;
    rest << type << kTab << asset << kTab << rec.id << kTab << rec.energy_uwh << kTab << rec.duration_ms << kTab
         << rec.total_cost_micro_eur << kTab << (rec.billing_complete ? 1 : 0) << kTab
         << (rec.stop_reason.empty() ? std::string("-") : rec.stop_reason) << kTab << (rec.undersampled ? 1 : 0) << kTab
         << rec.energy_wh << kTab << rec.billed_energy_wh << kTab << rec.charging_s << kTab << rec.billed_time_s << kTab
         << rec.energy_cost_micro_eur << kTab << rec.time_cost_micro_eur << kTab << rec.flat_cost_micro_eur << kTab
         << rec.samples << kTab << rec.integration_gaps << kTab << (rec.time_upper_bound ? 1 : 0) << kTab
         << (rec.energy_lower_bound ? 1 : 0) << kTab << rec.quality << kTab << context_currency_ << kTab
         << context_timestamp_ << kTab << context_tariff_ << kTab << json_payload;
    context_currency_.clear();
    context_timestamp_.clear();
    // The tariff of record belongs to the line being closed only. Leaving it
    // set would stamp it onto the next session_open, which has no tariff of its
    // own, and that silently wrong line would still verify.
    context_tariff_.clear();

    const std::string prev = last_hash_;
    const std::string hash = sha256_hex(prev + kTab + rest.str());

    std::ostringstream line;
    line << prev << kTab << hash << kTab << rest.str() << '\n';

    if (path_.empty()) {
        last_hash_ = hash;
        ++lines_written_;
        track(asset, rec, type);
        return true;
    }
    std::FILE* f = std::fopen(path_.c_str(), "ab");
    if (f == nullptr) return false;
    const std::string data = line.str();

    // Where the valid journal ends right now. A write that stops halfway must be
    // rolled back to exactly this point: leaving the torn bytes in place would
    // make the next append start after a partial line, and the chain could then
    // never verify again.
    const std::int64_t good_end = file_size_of(path_);

    const std::size_t written = std::fwrite(data.data(), 1, data.size(), f);
    if (written != data.size()) {
        std::fflush(f);
        truncate_file(f, good_end);
        std::fclose(f);
        return false;
    }
    bool durable = true;
    if (fsync_on_commit_) durable = sync_file(f);
    // A failing close means the last bytes may never have reached the device.
    if (std::fclose(f) != 0) durable = false;
    if (!durable) {
        // The line is not trustworthy: take it back out rather than leaving a
        // recorded-but-not-durable row that a later reader would trust.
        if (std::FILE* rollback = std::fopen(path_.c_str(), "r+b")) {
            truncate_file(rollback, good_end);
            std::fclose(rollback);
        }
        return false;
    }
    last_hash_ = hash;
    ++lines_written_;
    track(asset, rec, type);
    return true;
}

// Returns false when the line did not reach the journal. A caller that ignores
// the result would go on believing the session was recorded when it was not.
bool Ledger::append_session_open(const SessionRecord& rec, const std::string& asset_id,
                                 const std::string& opened_at_utc) {
    context_currency_.clear();
    context_timestamp_ = opened_at_utc;
    std::ostringstream o;
    o << "{\"schema\":\"" << LEDGER_SCHEMA << "\",\"type\":\"session_open\",\"asset_id\":\"" << asset_id
      << "\",\"session_id\":" << rec.id << ",\"opened_at_utc\":\"" << opened_at_utc << "\"}";
    return append("session_open", asset_id, rec, o.str());
}

// Returns false when the close line did not reach the journal. On failure the
// session is deliberately NOT added to sessions_: the in-memory view must never
// claim a settlement the file does not contain, or a portfolio rollup would bill
// on energy that was never durably recorded.
bool Ledger::append_session_close(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                                  const std::string& closed_at_utc, const std::string& tariff_json) {
    context_currency_ = currency;
    context_timestamp_ = closed_at_utc;
    context_tariff_ = tariff_json;
    if (!append("session_close", asset_id, rec,
                json_session_line(rec, asset_id, currency, closed_at_utc, tariff_json))) {
        return false;
    }
    SettledSession s;
    s.asset_id = asset_id;
    s.id = rec.id;
    s.record = rec;
    s.currency = currency;
    s.closed_at_utc = closed_at_utc;
    s.tariff_json = tariff_json;
    sessions_.push_back(s);
    return true;
}

RecoveryReport Ledger::recover() {
    RecoveryReport rep;
    if (path_.empty()) {
        rep.chain_intact = true;
        return rep;
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        rep.chain_intact = true;
        return rep;
    }
    energy_uwh_ = 0;
    billable_ = 0;
    revenue_ = 0;
    closes_ = 0;
    opens_ = 0;
    open_sessions_.clear();
    per_asset_.clear();
    sessions_.clear();

    std::string prev = LEDGER_GENESIS_HASH;
    std::string line;
    std::uint64_t lineno = 0;  // Journal lines are tracked as they are seen, so each closing
    // session can point back to its own opening evidence.
    std::map<std::string, std::uint64_t> open_lines;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto parts = split(line, '\t');
        if (parts.size() < 6) {
            rep.truncated_last_line = true;
            rep.chain_intact = false;
            rep.first_broken_line = lineno;
            break;
        }
        std::ostringstream rest;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (i > 2) rest << kTab;
            rest << parts[i];
        }
        const std::string recomputed = sha256_hex(parts[0] + kTab + rest.str());
        if (parts[0] != prev || parts[1] != recomputed) {
            rep.chain_intact = false;
            rep.first_broken_line = lineno;
            break;
        }
        prev = parts[1];
        ++rep.lines;

        const std::string& type = parts[2];
        const std::string& asset = parts[3];
        SessionRecord rec;
        parse_u64(parts.size() > 4 ? parts[4] : std::string(), rec.id);
        parse_u64(parts.size() > 5 ? parts[5] : std::string(), rec.energy_uwh);
        std::uint64_t duration = 0, cost = 0;
        parse_u64(parts.size() > 6 ? parts[6] : std::string(), duration);
        parse_u64(parts.size() > 7 ? parts[7] : std::string(), cost);
        rec.duration_ms = static_cast<std::int64_t>(duration);
        rec.total_cost_micro_eur = static_cast<std::int64_t>(cost);
        rec.billing_complete = parts.size() > 8 && parts[8] == "1";
        rec.stop_reason = (parts.size() > 9 && parts[9] != "-") ? parts[9] : std::string();
        rec.undersampled = parts.size() > 10 && parts[10] == "1";
        rec.closed = type != "session_open";
        // Forward-compatible trailing fields (absent in journals written before
        // v1.3.0 are left at zero, which is the safe settlement default).
        const auto num = [&parts](std::size_t idx) -> std::uint64_t {
            if (idx >= parts.size()) return 0;
            std::uint64_t v = 0;
            parse_u64(parts[idx], v);
            return v;
        };
        rec.energy_wh = num(11);
        rec.billed_energy_wh = num(12);
        rec.charging_s = static_cast<std::int64_t>(num(13));
        rec.billed_time_s = static_cast<std::int64_t>(num(14));
        rec.energy_cost_micro_eur = static_cast<std::int64_t>(num(15));
        rec.time_cost_micro_eur = static_cast<std::int64_t>(num(16));
        rec.flat_cost_micro_eur = static_cast<std::int64_t>(num(17));
        rec.samples = static_cast<std::uint32_t>(num(18));
        rec.integration_gaps = static_cast<std::uint32_t>(num(19));
        rec.time_upper_bound = num(20) == 1;
        rec.energy_lower_bound = num(21) == 1;
        rec.quality = parts.size() > 22 ? parts[22] : std::string();
        const std::string currency = parts.size() > 23 ? parts[23] : std::string();
        const std::string closed_at = parts.size() > 24 ? parts[24] : std::string();
        // Forward-compatible tariff snapshot between v1.3.0 journals (which
        // have the JSON payload in this slot) and v1.4.0 ones (which carry the
        // tariff on it). Both are self-identifying, so the shape, not the
        // position, is what settles it.
        std::string tariff_json;
        if (parts.size() > 26) {
            tariff_json = parts[25];
        }

        track(asset, rec, type);
        if (rec.closed) {
            ++rep.session_closes;
            rep.energy_uwh += rec.energy_uwh;
            rep.revenue_micro_eur += cost;
            if (rec.billing_complete) ++rep.billable_sessions;
            SettledSession s;
            s.asset_id = asset;
            s.id = rec.id;
            s.record = rec;
            s.currency = currency;
            s.closed_at_utc = closed_at;
            s.tariff_json = tariff_json;
            s.close_line = lineno;
            s.close_prev_hash = parts[0];
            s.close_hash = parts[1];
            s.close_line_raw = line;
            const std::string key = asset + "#" + std::to_string(rec.id);
            const auto open_it = open_lines.find(key);
            s.open_line = open_it != open_lines.end() ? open_it->second : 0;
            sessions_.push_back(s);
            open_lines.erase(key);
        } else {
            ++rep.session_opens;
            // Assigned, not emplaced: a session id reused for the same asset
            // after a restart must point at its own opening line, and the entry
            // is erased on close so a later reuse cannot inherit a stale one.
            open_lines[asset + "#" + std::to_string(rec.id)] = lineno;
        }
    }
    rep.orphan_sessions = open_sessions_;
    rep.last_hash = prev;
    last_hash_ = prev;
    lines_written_ = rep.lines;
    return rep;
}

bool Ledger::verify(std::string& error) const {
    if (path_.empty()) {
        error.clear();
        return true;
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        error = "Cannot read the ledger: " + path_;
        return false;
    }
    std::string prev = LEDGER_GENESIS_HASH;
    std::string line;
    std::uint64_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto parts = split(line, '\t');
        if (parts.size() < 4) {
            error = "line " + std::to_string(lineno) + ": malformed record";
            return false;
        }
        std::ostringstream rest;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (i > 2) rest << kTab;
            rest << parts[i];
        }
        const std::string recomputed = sha256_hex(parts[0] + kTab + rest.str());
        if (parts[0] != prev) {
            error = "line " + std::to_string(lineno) + ": broken link";
            return false;
        }
        if (parts[1] != recomputed) {
            error = "line " + std::to_string(lineno) + ": hash mismatch, record altered";
            return false;
        }
        prev = parts[1];
    }
    error.clear();
    return true;
}

std::string json_dispute(const SettledSession& s, const RecoveryReport& chain, const std::string& journal_path,
                         const std::string& journal_sha256_hex) {
    std::ostringstream o;
    const auto& r = s.record;
    const auto money = [](std::int64_t micro) { return format_fixed_point(micro, kMicroPerEuro); };
    const auto u64 = [](std::uint64_t v) { return std::to_string(v); };
    const auto flag = [](bool v) { return v ? "true" : "false"; };

    o << "{\"schema\":\"charger.dispute.v1\""
      << ",\"version\":\"" << VERSION << "\""
      << ",\"purpose\":\"complete meter, tariff and calculation evidence for one charging session\""
      << ",\"price_model\":\"ocpi-2.2.1-energy-time-flat\""
      << ",\"vat\":\"omitted\"";

    o << ",\"journal\":{\"path\":\"" << json_escape(journal_path) << "\""
      << ",\"sha256\":\"" << json_escape(journal_sha256_hex) << "\""
      << ",\"lines\":" << chain.lines << ",\"genesis\":\"" << LEDGER_GENESIS_HASH << "\""
      << ",\"last_hash\":\"" << json_escape(chain.last_hash) << "\""
      << ",\"chain_intact\":" << flag(chain.chain_intact);
    if (!chain.orphan_sessions.empty()) o << ",\"orphan_sessions\":" << chain.orphan_sessions.size();
    o << "}";

    o << ",\"session\":{\"asset_id\":\"" << json_escape(s.asset_id) << "\""
      << ",\"session_id\":" << s.id << ",\"currency\":\"" << json_escape(s.currency) << "\""
      << ",\"closed_at_utc\":\"" << json_escape(s.closed_at_utc) << "\""
      << ",\"quality\":\"" << json_escape(r.quality) << "\""
      << ",\"billable\":" << flag(r.billing_complete) << ",\"stop_reason\":\"" << json_escape(r.stop_reason) << "\""
      << ",\"energy_uwh\":\"" << u64(r.energy_uwh) << "\""
      << ",\"energy_wh\":\"" << u64(r.energy_wh) << "\""
      << ",\"billed_energy_wh\":\"" << u64(r.billed_energy_wh) << "\""
      << ",\"charging_s\":" << r.charging_s << ",\"billed_time_s\":" << r.billed_time_s << ",\"energy_cost\":\""
      << money(r.energy_cost_micro_eur) << "\""
      << ",\"time_cost\":\"" << money(r.time_cost_micro_eur) << "\""
      << ",\"flat_cost\":\"" << money(r.flat_cost_micro_eur) << "\""
      << ",\"total_cost\":\"" << money(r.total_cost_micro_eur) << "\""
      << ",\"total_cost_excl_vat\":\"" << money(r.total_cost_micro_eur) << "\""
      << ",\"peak_power_mw\":" << r.peak_power_mw << ",\"min_voltage_mv\":" << r.min_voltage_mv
      << ",\"max_current_ma\":" << r.max_current_ma << ",\"samples\":" << r.samples
      << ",\"integration_gaps\":" << r.integration_gaps << ",\"undersampled\":" << flag(r.undersampled)
      << ",\"energy_lower_bound\":" << flag(r.energy_lower_bound)
      << ",\"time_upper_bound\":" << flag(r.time_upper_bound) << "}";

    if (!s.tariff_json.empty()) o << ",\"tariff\":" << s.tariff_json;

    o << ",\"provenance\":{\"journal_open_line\":" << s.open_line << ",\"journal_close_line\":" << s.close_line
      << ",\"close_prev_hash\":\"" << json_escape(s.close_prev_hash) << "\""
      << ",\"close_hash\":\"" << json_escape(s.close_hash) << "\""
      << ",\"close_line_raw\":\"" << json_escape(s.close_line_raw) << "\"}";

    o << ",\"verification\":{"
      << "\"step_1\":\"verify the journal: each line hash chains as sha256(prev_hash TAB rest) and equals the next "
         "line prev_hash; close_hash is one such link\""
      << ",\"step_2\":\"verify this document against the file: sha256(journal file) equals journal.sha256\""
      << ",\"step_3\":\"recompute the tariff: energy_cost + time_cost + flat_cost must equal total_cost, priced from "
         "the embedded tariff block\""
      << "}}";
    return o.str();
}

std::optional<PortfolioKpi> Ledger::portfolio() const {
    if (per_asset_.empty()) return std::nullopt;
    PortfolioKpi out;
    out.assets = per_asset_.size();
    for (const auto& a : per_asset_) {
        out.sessions_closed += a.sessions_closed;
        out.sessions_billable += a.sessions_billable;
        out.energy_uwh += a.energy_uwh;
        out.revenue_micro_eur += a.revenue_micro_eur;
        out.charging_ms += a.charging_ms;
        out.fault_stops += a.fault_stops;
        out.timeout_stops += a.timeout_stops;
        out.sessions_held += a.sessions_closed - a.sessions_billable;
        out.per_asset.push_back(a);
    }
    std::sort(out.per_asset.begin(), out.per_asset.end(), [](const PortfolioAsset& a, const PortfolioAsset& b) {
        if (a.revenue_micro_eur != b.revenue_micro_eur) return a.revenue_micro_eur > b.revenue_micro_eur;
        return a.asset_id < b.asset_id;
    });
    if (!out.per_asset.empty()) out.best_asset_id = out.per_asset.front().asset_id;
    if (out.charging_ms > 0) {
        const WideInt num = static_cast<WideInt>(out.energy_uwh) * 3600;
        out.mean_power_mw = static_cast<std::int64_t>(num / static_cast<WideInt>(out.charging_ms));
    }
    if (out.energy_uwh > 0) {
        const WideInt num = static_cast<WideInt>(out.revenue_micro_eur) * 1000000000;
        out.revenue_per_kwh_milli_micro_eur = static_cast<std::int64_t>(num / static_cast<WideInt>(out.energy_uwh));
    }
    return out;
}

namespace {

std::string read_whole_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    ok = true;
    return buf.str();
}

std::optional<JournalAnchor> parse_anchor_json(const std::string& text, std::string& error) {
    JournalAnchor a;
    const auto grab = [&](const char* key, std::string& out) {
        const std::string k = std::string("\"") + key + "\"";
        const auto pos = text.find(k);
        if (pos == std::string::npos) {
            error = std::string("anchor is missing '") + key + "'";
            return false;
        }
        const auto c = text.find(':', pos + k.size());
        const auto q1 = text.find('"', c);
        const auto q2 = text.find('"', q1 + 1);
        if (c == std::string::npos || q1 == std::string::npos || q2 == std::string::npos) {
            error = std::string("anchor field '") + key + "' is malformed";
            return false;
        }
        out = text.substr(q1 + 1, q2 - q1 - 1);
        return true;
    };
    if (text.find(ANCHOR_SCHEMA) == std::string::npos) {
        error = "anchor schema must be charger.anchor.v1";
        return std::nullopt;
    }
    std::string lines_s;
    if (!grab("journal_sha256", a.journal_sha256) || !grab("last_hash", a.last_hash) ||
        !grab("anchored_at_utc", a.anchored_at_utc)) {
        return std::nullopt;
    }
    if (!grab("lines", lines_s)) return std::nullopt;
    std::uint64_t v = 0;
    if (!parse_u64(lines_s, v)) {
        error = "anchor lines is not a non-negative integer";
        return std::nullopt;
    }
    a.lines = v;
    if (a.journal_sha256.size() != 64 || a.last_hash.size() != 64) {
        error = "anchor hashes must be 64 hex characters";
        return std::nullopt;
    }
    return a;
}

}  // namespace

bool write_journal_anchor(const std::string& journal_path, const std::string& anchor_path, JournalAnchor& out,
                          std::string& error) {
    if (journal_path.empty() || anchor_path.empty()) {
        error = "journal and anchor paths are required";
        return false;
    }
    std::string verr;
    auto led = Ledger::open(journal_path, false, verr);
    if (!led) {
        error = verr;
        return false;
    }
    if (!led->verify(verr)) {
        error = "refusing to anchor a broken journal: " + verr;
        return false;
    }
    const auto rep = led->recover();
    if (!rep.chain_intact) {
        error = "refusing to anchor: chain broken at line " + std::to_string(rep.first_broken_line);
        return false;
    }
    bool ok = false;
    const std::string whole = read_whole_file(journal_path, ok);
    if (!ok) {
        error = "cannot read journal: " + journal_path;
        return false;
    }
    out.journal_sha256 = sha256_hex(whole);
    out.last_hash = rep.last_hash;
    out.lines = rep.lines;
    out.anchored_at_utc = utc_timestamp();
    std::ostringstream o;
    o << "{\"schema\":\"" << ANCHOR_SCHEMA << "\",\"journal_sha256\":\"" << out.journal_sha256 << "\"";
    o << ",\"last_hash\":\"" << out.last_hash << "\",\"lines\":\"" << out.lines << "\"";
    o << ",\"anchored_at_utc\":\"" << out.anchored_at_utc << "\"}";
    std::ofstream f(anchor_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot write anchor: " + anchor_path;
        return false;
    }
    f << o.str() << '\n';
    f.flush();
    if (!f) {
        error = "failed while writing anchor: " + anchor_path;
        return false;
    }
    return true;
}

bool verify_journal_anchor(const std::string& journal_path, const std::string& anchor_path,
                           std::uint64_t& appended_lines, std::string& error) {
    appended_lines = 0;
    bool ok = false;
    const std::string anchor_text = read_whole_file(anchor_path, ok);
    if (!ok) {
        error = "cannot read anchor: " + anchor_path;
        return false;
    }
    std::string perr;
    const auto anchor = parse_anchor_json(anchor_text, perr);
    if (!anchor) {
        error = perr;
        return false;
    }
    std::string verr;
    auto led = Ledger::open(journal_path, false, verr);
    if (!led) {
        error = verr;
        return false;
    }
    if (!led->verify(verr)) {
        error = verr;
        return false;
    }
    const auto rep = led->recover();
    if (!rep.chain_intact) {
        error = "chain broken at line " + std::to_string(rep.first_broken_line);
        return false;
    }
    if (rep.lines < anchor->lines) {
        error = "journal is shorter than the anchor (" + std::to_string(rep.lines) + " < " +
                std::to_string(anchor->lines) + "): lines were removed";
        return false;
    }
    // Re-walk the prefix and confirm line N still hashes to the anchored hash.
    std::ifstream in(journal_path, std::ios::binary);
    std::string prev = LEDGER_GENESIS_HASH;
    std::string line;
    std::uint64_t lineno = 0;
    std::string hash_at_anchor = LEDGER_GENESIS_HASH;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto parts = split(line, '\t');
        if (parts.size() < 4) {
            error = "line " + std::to_string(lineno) + ": malformed record";
            return false;
        }
        std::ostringstream rest;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (i > 2) rest << kTab;
            rest << parts[i];
        }
        const std::string recomputed = sha256_hex(parts[0] + kTab + rest.str());
        if (parts[0] != prev || parts[1] != recomputed) {
            error = "line " + std::to_string(lineno) + ": chain mismatch";
            return false;
        }
        prev = parts[1];
        if (lineno == anchor->lines) hash_at_anchor = parts[1];
        if (lineno > anchor->lines && anchor->lines == 0) break;
    }
    if (hash_at_anchor != anchor->last_hash) {
        error = "anchor mismatch: line " + std::to_string(anchor->lines) + " no longer hashes to " + anchor->last_hash +
                ": the prefix was rewritten";
        return false;
    }
    appended_lines = rep.lines > anchor->lines ? rep.lines - anchor->lines : 0;
    return true;
}

std::string json_cdr(const SettledSession& s) {
    const auto& r = s.record;
    const auto money = [](std::int64_t micro) { return format_fixed_point(micro, kMicroPerEuro); };
    std::ostringstream o;
    o << "{\"schema\":\"charger.cdr.v1\"";
    o << ",\"cdr_id\":\"" << json_escape(s.asset_id) << "-" << s.id << "\"";
    o << ",\"session_id\":\"" << json_escape(s.asset_id) << "-" << s.id << "\"";
    o << ",\"asset_id\":\"" << json_escape(s.asset_id) << "\"";
    o << ",\"currency\":\"" << json_escape(s.currency) << "\"";
    // The journal stores a wall-clock label only for the close; the start is
    // close minus the measured duration, so it is derived, not measured.
    o << ",\"derived_start_utc\":\"close-minus-duration\"";
    o << ",\"end_utc\":\"" << json_escape(s.closed_at_utc) << "\"";
    o << ",\"duration_ms\":" << r.duration_ms;
    o << ",\"charging_s\":" << r.charging_s;
    o << ",\"energy_wh\":\"" << r.energy_wh << "\"";
    o << ",\"energy_uwh\":\"" << r.energy_uwh << "\"";
    o << ",\"billed_energy_wh\":\"" << r.billed_energy_wh << "\"";
    o << ",\"billed_time_s\":" << r.billed_time_s;
    o << ",\"costs\":{\"energy\":\"" << money(r.energy_cost_micro_eur) << "\",\"time\":\"";
    o << money(r.time_cost_micro_eur) << "\",\"flat\":\"" << money(r.flat_cost_micro_eur) << "\",\"total_excl_vat\":\"";
    o << money(r.total_cost_micro_eur) << "\"}";
    o << ",\"vat\":\"omitted\"";
    o << ",\"billable\":" << (r.billing_complete ? "true" : "false");
    o << ",\"stop_reason\":\"" << json_escape(r.stop_reason) << "\"";
    o << ",\"quality\":\"" << json_escape(r.quality) << "\"";
    if (!s.tariff_json.empty()) o << ",\"tariff\":" << s.tariff_json;
    o << ",\"provenance\":{\"journal_open_line\":" << s.open_line << ",\"journal_close_line\":" << s.close_line;
    o << ",\"close_hash\":\"" << json_escape(s.close_hash) << "\"}";
    o << ",\"note\":\"local evidence for an OCPI CDR; signed exchange stays in the backend\"}";
    return o.str();
}

}  // namespace charger
