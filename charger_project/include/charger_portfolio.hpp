#pragma once

// Fleet-level rollup over closed sessions.
//
// A single bay tells you nothing about the site. Operators pay for portfolio
// questions: which bay earns, which bay idles, which bay keeps faulting, and
// what the site actually returned per day. This module aggregates closed
// SessionRecords without needing a running process, so it can be rebuilt from
// the ledger after a restart.

#include <cstdint>
#include <string>
#include <vector>

#include "charger_monitor.hpp"

namespace charger {

struct PortfolioAsset {
    std::string asset_id;
    std::uint64_t sessions_closed{0};
    std::uint64_t sessions_billable{0};
    std::uint64_t energy_uwh{0};
    std::uint64_t revenue_micro_eur{0};
    std::int64_t charging_ms{0};
    std::uint64_t fault_stops{0};
    std::uint64_t timeout_stops{0};
    std::uint64_t undersampled_sessions{0};
    std::int64_t availability_ppm{-1};
};

struct PortfolioKpi {
    std::uint64_t assets{0};
    std::uint64_t sessions_closed{0};
    std::uint64_t sessions_billable{0};
    std::uint64_t sessions_held{0};
    std::uint64_t energy_uwh{0};
    std::uint64_t revenue_micro_eur{0};
    std::int64_t charging_ms{0};
    std::uint64_t fault_stops{0};
    std::uint64_t timeout_stops{0};
    // Mean delivered power over charging time; -1 when there is no charging time.
    std::int64_t mean_power_mw{-1};
    // Revenue per kWh actually delivered, in exact micro-EUR; -1 when no energy.
    std::int64_t revenue_per_kwh_milli_micro_eur{-1};
    std::string best_asset_id;
    std::vector<PortfolioAsset> per_asset;
};

class Portfolio {
   public:
    void add(const SessionRecord& rec, const std::string& asset_id);

    PortfolioKpi summary() const;

    // Top assets by revenue. Deterministic ordering: revenue desc, then id.
    std::vector<std::string> top_assets_by_revenue(std::size_t limit, const PortfolioKpi& summary) const;

    std::string json(const PortfolioKpi& summary) const;

    std::size_t asset_count() const noexcept { return entries_.size(); }

   private:
    struct Entry {
        PortfolioAsset stats{};
        std::uint64_t charging_ms{0};
    };
    std::vector<Entry> entries_;

    Entry* find(const std::string& asset_id);
    const Entry* find(const std::string& asset_id) const;
};

// Serialises a rollup. Free function so a caller that recovered the aggregate
// from a journal can render it without holding a Portfolio instance.
std::string json_portfolio(const PortfolioKpi& summary);

}  // namespace charger
