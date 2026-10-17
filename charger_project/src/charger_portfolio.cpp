#include "charger_portfolio.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace charger {
namespace {

constexpr std::int64_t kMicroPerEuro = 1000000;

bool is_fault_stop(const std::string& reason) {
    return reason == "POWER_STAGE_FAULT" || reason == "MEASUREMENT_OUT_OF_RANGE" ||
           reason == "ENERGY_COUNTER_SATURATED";
}

// -1 means "not measurable", never a fabricated number.
const char* nullable_fixed(std::int64_t v, std::int64_t scale) {
    static thread_local std::string buffer;
    if (v < 0) return "null";
    buffer = format_fixed_point(v, scale);
    return buffer.c_str();
}

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

Portfolio::Entry* Portfolio::find(const std::string& asset_id) {
    for (auto& e : entries_) {
        if (e.stats.asset_id == asset_id) return &e;
    }
    return nullptr;
}

const Portfolio::Entry* Portfolio::find(const std::string& asset_id) const {
    for (const auto& e : entries_) {
        if (e.stats.asset_id == asset_id) return &e;
    }
    return nullptr;
}

void Portfolio::add(const SessionRecord& rec, const std::string& asset_id) {
    if (asset_id.empty() || !rec.closed) return;
    Entry* e = find(asset_id);
    if (e == nullptr) {
        entries_.push_back(Entry{});
        e = &entries_.back();
        e->stats.asset_id = asset_id;
    }
    e->stats.sessions_closed += 1;
    if (rec.billing_complete) e->stats.sessions_billable += 1;
    e->stats.energy_uwh += rec.energy_uwh;
    e->stats.revenue_micro_eur += static_cast<std::uint64_t>(rec.total_cost_micro_eur);
    e->stats.charging_ms += rec.duration_ms;
    e->charging_ms += rec.duration_ms;
    if (is_fault_stop(rec.stop_reason)) e->stats.fault_stops += 1;
    if (rec.stop_reason == "COMMUNICATION_TIMEOUT") e->stats.timeout_stops += 1;
    if (rec.undersampled) e->stats.undersampled_sessions += 1;
}

PortfolioKpi Portfolio::summary() const {
    PortfolioKpi out;
    out.assets = entries_.size();
    for (const auto& e : entries_) {
        out.sessions_closed += e.stats.sessions_closed;
        out.sessions_billable += e.stats.sessions_billable;
        out.energy_uwh += e.stats.energy_uwh;
        out.revenue_micro_eur += e.stats.revenue_micro_eur;
        out.charging_ms += e.stats.charging_ms;
        out.fault_stops += e.stats.fault_stops;
        out.timeout_stops += e.stats.timeout_stops;
        out.sessions_held += e.stats.sessions_closed - e.stats.sessions_billable;
        PortfolioAsset copy = e.stats;
        // A single session carries no dwell history, so availability is not
        // derivable here. -1 means "not measurable from sessions alone".
        copy.availability_ppm = -1;
        out.per_asset.push_back(copy);
    }
    std::sort(out.per_asset.begin(), out.per_asset.end(), [](const PortfolioAsset& a, const PortfolioAsset& b) {
        if (a.revenue_micro_eur != b.revenue_micro_eur) return a.revenue_micro_eur > b.revenue_micro_eur;
        return a.asset_id < b.asset_id;
    });
    if (!out.per_asset.empty()) out.best_asset_id = out.per_asset.front().asset_id;

    // Mean power from the exact integer registers, no floating point.
    if (out.charging_ms > 0) {
        // 1 uWh/ms = 3600 J/ms = 3.6 W = 3600 mW.
        const WideInt num = static_cast<WideInt>(out.energy_uwh) * 3600;
        out.mean_power_mw = static_cast<std::int64_t>(num / static_cast<WideInt>(out.charging_ms));
    }
    if (out.energy_uwh > 0) {
        // revenue per kWh in micro-EUR esatti: revenue_micro_eur * 1e9 / energy_uwh.
        const WideInt num = static_cast<WideInt>(out.revenue_micro_eur) * 1000000000;
        const WideInt den = static_cast<WideInt>(out.energy_uwh);
        out.revenue_per_kwh_milli_micro_eur = static_cast<std::int64_t>(num / den);
    }
    return out;
}

std::vector<std::string> Portfolio::top_assets_by_revenue(std::size_t limit, const PortfolioKpi& summary) const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < summary.per_asset.size() && i < limit; ++i) {
        out.push_back(summary.per_asset[i].asset_id);
    }
    return out;
}

std::string json_portfolio(const PortfolioKpi& summary) {
    std::ostringstream o;
    o << "{\"schema\":\"charger.portfolio.v1\",\"assets\":" << summary.assets
      << ",\"sessions_closed\":" << summary.sessions_closed << ",\"sessions_billable\":" << summary.sessions_billable
      << ",\"sessions_held\":" << summary.sessions_held << ",\"energy_uwh\":\"" << summary.energy_uwh
      << "\",\"revenue_micro_eur\":\"" << summary.revenue_micro_eur << "\",\"revenue_excl_vat\":\""
      << format_fixed_point(static_cast<std::int64_t>(summary.revenue_micro_eur), kMicroPerEuro)
      << "\",\"charging_ms\":" << summary.charging_ms << ",\"fault_stops\":" << summary.fault_stops
      << ",\"timeout_stops\":" << summary.timeout_stops << ",\"mean_power_mw\":" << summary.mean_power_mw
      << ",\"revenue_per_kwh_micro_eur\":" << nullable_fixed(summary.revenue_per_kwh_milli_micro_eur, kMicroPerEuro)
      << ",\"best_asset_id\":\"" << json_escape(summary.best_asset_id) << "\",\"per_asset\":[";
    for (std::size_t i = 0; i < summary.per_asset.size(); ++i) {
        const auto& a = summary.per_asset[i];
        if (i != 0) o << ',';
        o << "{\"asset_id\":\"" << json_escape(a.asset_id) << "\",\"sessions_closed\":" << a.sessions_closed
          << ",\"sessions_billable\":" << a.sessions_billable << ",\"energy_uwh\":\"" << a.energy_uwh
          << "\",\"revenue_micro_eur\":\"" << a.revenue_micro_eur << "\",\"charging_ms\":" << a.charging_ms
          << ",\"fault_stops\":" << a.fault_stops << ",\"timeout_stops\":" << a.timeout_stops
          << ",\"undersampled_sessions\":" << a.undersampled_sessions << "}";
    }
    o << "]}";
    return o.str();
}

std::string Portfolio::json(const PortfolioKpi& summary) const { return json_portfolio(summary); }

}  // namespace charger