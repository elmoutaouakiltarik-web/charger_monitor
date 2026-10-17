#include "charger_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
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

void sync_file(std::FILE* f) {
    if (f == nullptr) return;
    std::fflush(f);
#if defined(_WIN32)
    _commit(_fileno(f));
#else
    ::fsync(::fileno(f));
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
    // stop_reason is tab-free by construction, so it is safe in the prefix.
    std::ostringstream rest;
    rest << type << kTab << asset << kTab << rec.id << kTab << rec.energy_uwh << kTab << rec.duration_ms << kTab
         << rec.total_cost_micro_eur << kTab << (rec.billing_complete ? 1 : 0) << kTab
         << (rec.stop_reason.empty() ? std::string("-") : rec.stop_reason) << kTab << (rec.undersampled ? 1 : 0) << kTab
         << json_payload;

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
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), f);
    const bool ok = written == data.size();
    if (ok && fsync_on_commit_) sync_file(f);
    std::fclose(f);
    if (!ok) return false;
    last_hash_ = hash;
    ++lines_written_;
    track(asset, rec, type);
    return true;
}

void Ledger::append_session_open(const SessionRecord& rec, const std::string& asset_id,
                                 const std::string& opened_at_utc) {
    std::ostringstream o;
    o << "{\"schema\":\"" << LEDGER_SCHEMA << "\",\"type\":\"session_open\",\"asset_id\":\"" << asset_id
      << "\",\"session_id\":" << rec.id << ",\"opened_at_utc\":\"" << opened_at_utc << "\"}";
    append("session_open", asset_id, rec, o.str());
}

void Ledger::append_session_close(const SessionRecord& rec, const std::string& asset_id, const std::string& currency,
                                  const std::string& closed_at_utc) {
    append("session_close", asset_id, rec, json_session_line(rec, asset_id, currency, closed_at_utc));
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

    std::string prev = LEDGER_GENESIS_HASH;
    std::string line;
    std::uint64_t lineno = 0;
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

        track(asset, rec, type);
        if (rec.closed) {
            ++rep.session_closes;
            rep.energy_uwh += rec.energy_uwh;
            rep.revenue_micro_eur += cost;
            if (rec.billing_complete) ++rep.billable_sessions;
        } else {
            ++rep.session_opens;
        }
    }
    rep.orphan_sessions = open_sessions_;
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

}  // namespace charger