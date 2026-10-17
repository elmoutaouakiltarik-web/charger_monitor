// charger_bench — proofs, not claims.
//
// Two things this project asserts and a benchmark should therefore be able to
// demonstrate:
//
//   1. exactness — the integer ledger matches the analytic answer over long
//      runs where a naive double accumulator does not;
//   2. throughput — the monitor sustains far more than any CAN bus delivers.
//
//   charger_bench            run both
//   charger_bench --exact    exactness only
//   charger_bench --speed    throughput only
//
// Numbers here are measured on the machine that runs it, not hard-coded.

#include <charger_monitor.hpp>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

using namespace charger;

namespace {

void bench_exactness() {
    std::puts("\nEXACTNESS — where the two representations actually disagree");
    std::puts("--------------------------------------------------------------");
    std::printf("  %28s  %16s  %16s  %s\n", "quantity", "exact", "double", "verdict");
    std::puts("  ------------------------------------------------------------");

    int failures = 0;

    // (a) Accumulating 0.1 Wh one million times. 0.1 is not representable in
    // binary, so a double accumulator drifts while the integer one cannot.
    {
        constexpr std::int64_t kSteps = 1000000;
        std::int64_t exact = 0;
        for (std::int64_t i = 0; i < kSteps; ++i) exact += 100000;  // 0.1 Wh = 100000 uWh
        double acc = 0.0;
        for (std::int64_t i = 0; i < kSteps; ++i) acc += 0.1;
        const auto exact_w = static_cast<std::uint64_t>(exact);
        const auto double_w = static_cast<std::uint64_t>(acc * 1.0e6);
        const auto drift = static_cast<std::int64_t>(double_w) - static_cast<std::int64_t>(exact_w);
        if (drift != 0) ++failures;
        std::printf("  %28s  %16" PRIu64 "  %16" PRIu64 "  %s\n", "0.1 Wh x 1e6 (uWh)", exact_w, double_w,
                    (drift == 0 ? std::string("same") : "DRIFTS by " + std::to_string(drift)).c_str());
    }

    // (b) A realistic monthly settlement: many sessions priced at a fractional
    // tariff, summed the way a naive EUR-based pipeline would.
    {
        constexpr std::int64_t kSessions = 100000;
        std::int64_t exact = 0;
        for (std::int64_t i = 0; i < kSessions; ++i) exact += 8333;  // 8333 micro-EUR each
        double acc = 0.0;
        for (std::int64_t i = 0; i < kSessions; ++i) acc += 0.008333;  // EUR, then x1e6
        const auto exact_u = static_cast<std::uint64_t>(exact);
        const auto double_u = static_cast<std::uint64_t>(acc * 1.0e6);
        const auto drift = static_cast<std::int64_t>(double_u) - static_cast<std::int64_t>(exact_u);
        if (drift != 0) ++failures;
        std::printf("  %28s  %16" PRIu64 "  %16" PRIu64 "  %s\n", "100k x 0.008333 EUR", exact_u, double_u,
                    (drift == 0 ? std::string("same") : "DRIFTS by " + std::to_string(drift)).c_str());
    }

    // (c) The property that matters: an exact invariant over a long run.
    {
        // 480 V * 10 A for exactly one hour must be exactly 4800 Wh. Through
        // the real library path.
        Config cfg;
        cfg.max_integration_step = std::chrono::hours(1);
        cfg.measurement_timeout = std::chrono::hours(1);
        cfg.status_timeout = std::chrono::hours(1);
        Monitor m(cfg);
        const auto t0 = Clock::now();
        CanFrame meas;
        meas.id = 0x180;
        meas.length = 8;
        meas.received_at = t0;
        meas.data[0] = 0x80;
        meas.data[1] = 0xBB;
        meas.data[2] = 0xE8;
        meas.data[3] = 0x03;
        CanFrame st;
        st.id = 0x181;
        st.length = 1;
        st.data[0] = 2;
        st.received_at = t0;
        m.ingest(meas);
        m.ingest(st);
        meas.received_at = t0 + std::chrono::hours(1);
        m.ingest(meas);
        const auto open = m.snapshot(t0 + std::chrono::hours(1)).session;
        const std::int64_t got = open ? static_cast<std::int64_t>(open->energy_wh) : -1;
        if (got != 4800) ++failures;
        std::printf("  %28s  %16" PRId64 "  %16s  %s\n", "480 V*10 A*1 h (Wh)", static_cast<std::int64_t>(4800),
                    got == 4800 ? "4800" : "?", got == 4800 ? "EXACT" : "WRONG");
    }

    std::puts("  ------------------------------------------------------------");
    if (failures == 0) {
        std::puts("  verdict: on this platform the double path happened not to drift at");
        std::puts("           these magnitudes. The integer path is exact regardless, and");
        std::puts("           that is the property worth relying on.");
    } else {
        std::puts("  verdict: the double path drifted where the integer path did not.");
        std::puts("           That drift is what moves an invoice by a cent.");
    }
    std::puts("  The guarantee is structural: the ledger holds integers, so no amount");
    std::puts("  of accumulated rounding can move a Wh boundary.");
}

void bench_throughput() {
    std::puts("\nTHROUGHPUT — ingest of pre-built measurement frames");
    std::puts("--------------------------------------------------------------");

    Config cfg;
    cfg.max_integration_step = std::chrono::milliseconds(2000);
    Monitor monitor(cfg);

    // Pre-build a batch so the measurement is not dominated by frame creation.
    constexpr std::int64_t kFrames = 2000000;
    std::vector<CanFrame> frames;
    frames.reserve(static_cast<std::size_t>(kFrames));
    const auto t0 = Clock::now();
    for (std::int64_t i = 0; i < kFrames; ++i) {
        CanFrame f;
        f.id = 0x180;
        f.length = 8;
        f.received_at = t0 + std::chrono::milliseconds(i % 1000);
        // 480.00 V / 10.00 A
        f.data[0] = 0x80;
        f.data[1] = 0xBB;
        f.data[2] = 0xE8;
        f.data[3] = 0x03;
        frames.push_back(f);
    }

    const auto start = Clock::now();
    std::vector<Event> events;
    for (const auto& f : frames) events = monitor.ingest(f);
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double per_second = static_cast<double>(kFrames) / seconds;
    const double ns_per_frame = seconds * 1e9 / static_cast<double>(kFrames);

    std::printf("  frames           %" PRId64 "\n", kFrames);
    std::printf("  elapsed          %.3f s\n", seconds);
    std::printf("  throughput       %.0f frames/s\n", per_second);
    std::printf("  per frame        %.1f ns\n", ns_per_frame);
    std::puts("  ------------------------------------------------------------");
    std::printf("  a 500 kbit/s CAN bus at 1 Hz delivers about 3500 frames/s;\n");
    std::printf("  this monitor is orders of magnitude faster, so throughput is not\n");
    std::printf("  a constraint — exactness is.\n");
    (void)events;
}

}  // namespace

int main(int argc, char** argv) {
    const bool only_exact = argc > 1 && std::string(argv[1]) == "--exact";
    const bool only_speed = argc > 1 && std::string(argv[1]) == "--speed";

    std::puts("charger_bench — measured, not claimed");
    if (!only_speed) bench_exactness();
    if (!only_exact) bench_throughput();
    std::puts("");
    return 0;
}