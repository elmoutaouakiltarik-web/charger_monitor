#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include "charger_ledger.hpp"

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace charger;

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

// Runs the synthesiser and returns its exit status (0 on success).
int run(const std::string& synth, const std::string& args) {
    const std::string cmd = "\"" + synth + "\" " + args + " 2>/dev/null";
    return std::system(cmd.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: charger_synth_tests <path to charger_synth>");
    const std::string synth = argv[1];
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "charger_synth_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string a = (dir / "a.journal").string(), b = (dir / "b.journal").string(),
                      c = (dir / "c.journal").string(), snaps = (dir / "s.ndjson").string();

    require(run(synth, "--out \"" + a + "\" --snapshots-out \"" + snaps + "\"") == 0, "the default scenario runs");

    // The journal is genuine: the real ledger accepts it end to end.
    std::string err;
    auto led = Ledger::open(a, false, err);
    require(led.has_value(), "journal opens: " + err);
    require(led->verify(err), "the synthetic journal must verify: " + err);
    const RecoveryReport rep = led->recover();
    require(rep.chain_intact, "chain intact");
    require(rep.session_closes > 100, "a week of five bays closes well over a hundred sessions");
    require(rep.orphan_sessions.size() == 1, "exactly one orphan");
    require(rep.billable_sessions > 0 && rep.billable_sessions < rep.session_closes,
            "some sessions are billable and some are held");
    require(rep.lines == rep.session_opens + rep.session_closes, "every line is an open or a close");

    std::set<std::string> assets;
    std::map<std::string, std::uint64_t> quality, reasons;
    for (const auto& s : led->sessions()) {
        assets.insert(s.asset_id);
        ++quality[s.record.quality];
        ++reasons[s.record.stop_reason];
        require(s.currency == "EUR", "currency recorded");
        require(!s.tariff_json.empty(), "the tariff of record travels with every close");
        require(s.closed_at_utc.size() == 20 && s.closed_at_utc[10] == 'T' && s.closed_at_utc.back() == 'Z',
                "closed_at_utc is ISO-8601 UTC");
        // The engine's own invariant, not the generator's: held means not billing_complete.
        if (s.record.quality != "OK") require(!s.record.billing_complete, "a non-OK quality is never billable");
        if (s.record.stop_reason == "COMMUNICATION_TIMEOUT") {
            require(s.record.time_upper_bound && !s.record.billing_complete, "a timeout is held");
        }
        require(s.record.energy_cost_micro_eur + s.record.time_cost_micro_eur + s.record.flat_cost_micro_eur ==
                    s.record.total_cost_micro_eur,
                "no clamp in this tariff: the parts sum to the total");
    }
    require(assets.size() == 5, "five bays appear");
    for (const char* q : {"OK", "GAPS", "NO_INTERVAL", "ZERO_ENERGY"}) {
        require(quality[q] > 0, std::string("quality ") + q + " must occur");
    }
    for (const char* r :
         {"STATUS_OFF", "STATUS_READY", "COMMUNICATION_TIMEOUT", "POWER_STAGE_FAULT", "MEASUREMENT_OUT_OF_RANGE"}) {
        require(reasons[r] > 0, std::string("stop reason ") + r + " must occur");
    }

    // The capture is a stream of snapshot envelopes in scenario time.
    const std::string capture = read_all(snaps);
    require(!capture.empty(), "the capture was written");
    require(capture.find("\"type\":\"snapshot\"") != std::string::npos, "snapshot envelope");
    require(capture.find("\"timestamp\":\"2026-10-06T00:00:00Z\"") != std::string::npos,
            "timestamps are scenario time, starting on the capture day");
    require(capture.find("\"state\":\"DISCONNECTED\"") != std::string::npos, "the capture contains the dropout");

    // Determinism: same arguments, same bytes; another seed, another journal. A
    // smaller scenario keeps this fast under the sanitizers.
    const std::string small = "--days 2 --bays 3";
    require(
        run(synth, "--out \"" + b + "\" " + small + " --snapshots-out \"" + (dir / "t.ndjson").string() + "\"") == 0,
        "small scenario with a capture");
    require(run(synth, "--out \"" + c + "\" " + small) == 0, "small scenario without a capture");
    require(read_all(b) == read_all(c), "the same seed must give a byte-identical journal, snapshots or not");
    const std::string d = (dir / "d.journal").string();
    require(run(synth, "--out \"" + d + "\" " + small + " --seed 2") == 0, "seed 2 runs");
    require(read_all(b) != read_all(d), "a different seed must give a different journal");

    // Refuses to clobber, unless forced.
    require(run(synth, "--out \"" + b + "\" " + small) != 0, "an existing output must be refused");
    require(run(synth, "--out \"" + b + "\" " + small + " --force") == 0, "--force replaces it");
    require(read_all(b) == read_all(c), "forced rewrite is still identical");
    require(run(synth, "--out \"" + (dir / "x.journal").string() + "\" --days 0") != 0, "bad --days is a usage error");
    require(run(synth, "--out \"" + (dir / "y.journal").string() + "\" --start 2026-02-30") != 0, "impossible date");
    require(run(synth, "--out \"" + (dir / "z.journal").string() + "\" --snapshots-every 15") != 0,
            "the capture cadence must be a multiple of the 10 s frame period");

    // A single bay still carries every quality.
    const std::string one = (dir / "one.journal").string();
    require(run(synth, "--out \"" + one + "\" --bays 1 --days 2") == 0, "one bay");
    auto led1 = Ledger::open(one, false, err);
    require(led1.has_value() && led1->verify(err), "one-bay journal verifies: " + err);
    led1->recover();
    require(!led1->sessions().empty(), "one-bay journal has sessions");

    fs::remove_all(dir);
    std::cout << "synth tests OK\n";
    return 0;
}
