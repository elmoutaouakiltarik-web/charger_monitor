#include "charger_candump.hpp"
#include "charger_ledger.hpp"
#include "charger_monitor.hpp"
#include "charger_portfolio.hpp"

#include <cstdio>
#include <string>

using namespace charger;

// Any byte string must be rejected or ingested without a crash. A few capped
// lines also exercise the session ledger, the integer tariff path and the
// hash-chain journal, including verify() over a truncated or mutated file.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Config cfg;
    if (size > 0) {
        cfg.tariff.energy_micro_eur_per_kwh = static_cast<std::int64_t>(data[0]) * 1000;
        cfg.tariff.energy_step_wh = 1 + static_cast<std::int32_t>(data[0] % 20);
        cfg.tariff.flat_micro_eur = data[0];
    }
    Monitor monitor(cfg);
    const std::string blob(reinterpret_cast<const char*>(data), size);
    auto now = Clock::now();
    std::size_t begin = 0;
    std::string asset;
    for (int line = 0; line < 8 && begin < blob.size(); ++line) {
        const auto end = blob.find('\n', begin);
        const auto stop = end == std::string::npos ? blob.size() : end;
        const auto slice = blob.substr(begin, std::min<std::size_t>(stop - begin, 64));
        // The candump reader sees the same hostile bytes: it must refuse or parse, never crash.
        if (const auto rec = parse_candump_line(slice)) {
            static_cast<void>(utc_from_epoch_us(rec->time_us));
        }
        if (const auto frame = parse_hex_frame(slice, now)) {
            monitor.ingest(*frame);
            if (asset.empty() && monitor.snapshot(now).last_session) asset = "fuzz-bay";
        }
        monitor.tick(now);
        now += std::chrono::milliseconds(100);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    std::string reason;
    static_cast<void>(monitor.reset_fault(reason, now));
    const auto snapshot = monitor.snapshot(now);
    static_cast<void>(json_snapshot(snapshot));
    static_cast<void>(to_string(snapshot.state));
    if (snapshot.last_session) {
        static_cast<void>(
            json_session_line(*snapshot.last_session, snapshot.asset_id, snapshot.currency, "1970-01-01T00:00:00Z"));
    }

    // Journal round trip: append, verify, recover. Kept on a fixed path so the
    // fuzzer cannot exhaust the filesystem; the file is truncated each run.
    const char* path = "charger_fuzz.tmpjournal";
    std::FILE* f = std::fopen(path, "wb");
    if (f != nullptr) {
        std::fclose(f);
        std::string err;
        if (auto led = Ledger::open(path, false, err)) {
            SessionRecord rec;
            rec.id = 1;
            rec.closed = true;
            rec.energy_uwh = data[0];
            rec.duration_ms = 1000 + static_cast<std::int64_t>(data[0]);
            rec.total_cost_micro_eur = static_cast<std::int64_t>(data[0]) * 1000;
            rec.billing_complete = (data[0] & 1) != 0;
            rec.stop_reason = "STATUS_OFF";
            led->append_session_open(rec, "fuzz-bay", "1970-01-01T00:00:00Z");
            led->append_session_close(rec, "fuzz-bay", "EUR", "1970-01-01T00:00:01Z");
            led->recover();
            led->verify(err);
            if (const auto pf = led->portfolio()) static_cast<void>(Portfolio().json(*pf));
        }
        std::remove(path);
    }
    return 0;
}
