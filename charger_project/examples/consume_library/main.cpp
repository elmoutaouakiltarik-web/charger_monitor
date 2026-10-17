// Downstream consumer example.
//
// This is not part of the library. It is built the way a third-party project
// would build it: install the package, then find_package and link the exported
// target. If this compiles and prints exact figures, the public API is usable.

#include <charger_ledger.hpp>
#include <charger_monitor.hpp>
#include <charger_portfolio.hpp>

#include <iostream>

using namespace charger;

int main() {
    Config cfg;
    cfg.asset_id = "bay-consumer";
    cfg.tariff.energy_micro_eur_per_kwh = 250000;   // 0.25 EUR/kWh
    cfg.tariff.time_micro_eur_per_hour = 3600000;   // 3.60 EUR/h
    cfg.tariff.flat_micro_eur = 500000;             // 0.50 EUR per session
    // One hour of constant current, integrated as a single trapezoid.
    cfg.max_integration_step = std::chrono::hours(1);
    cfg.measurement_timeout = std::chrono::hours(1);
    cfg.status_timeout = std::chrono::hours(1);

    Monitor monitor(cfg);

    // Demo protocol: 0x180 carries voltage in centivolts and current in
    // centiamps, both unsigned 16-bit little-endian.
    const auto t0 = Clock::now();
    CanFrame measurement;
    measurement.id = 0x180;
    measurement.length = 8;
    measurement.received_at = t0;
    measurement.data[0] = 0x80;
    measurement.data[1] = 0xBB;  // 0xBB80 = 48000 centivolts = 480.00 V
    measurement.data[2] = 0xE8;
    measurement.data[3] = 0x03;  // 0x03E8 = 1000 centiamps = 10.00 A

    CanFrame status;
    status.id = 0x181;
    status.length = 1;
    status.data[0] = 2;  // CHARGING
    status.received_at = t0;

    monitor.ingest(measurement);
    monitor.ingest(status);
    monitor.ingest([&] {
        auto f = measurement;
        f.received_at = t0 + std::chrono::hours(1);
        return f;
    }());
    monitor.ingest([&] {
        auto f = status;
        f.received_at = t0 + std::chrono::hours(1);
        f.data[0] = 0;  // OFF closes the session
        return f;
    }());

    const auto record = monitor.snapshot(t0 + std::chrono::hours(1)).last_session;
    if (!record || record->energy_wh != 4800 || !record->billing_complete) {
        std::cerr << "consumer: unexpected ledger present=" << (record ? 1 : 0);
        if (record) {
            std::cerr << " energy_wh=" << record->energy_wh << " quality=" << record->quality
                      << " billable=" << record->billing_complete;
        }
        std::cerr << '\n';
        return 1;
    }

    Portfolio portfolio;
    portfolio.add(*record, "bay-consumer");
    const auto kpi = portfolio.summary();

    std::cout << "consumer OK: " << record->energy_wh << " Wh, " << record->total_cost_micro_eur
              << " micro-EUR total, fleet revenue " << kpi.revenue_micro_eur << " micro-EUR\n";
    // Matches the published NIST vector, so the chain is auditable externally.
    std::cout << "sha256(\"abc\") = " << sha256_hex("abc") << '\n';
    return 0;
}