# Contributing

Thanks for considering a contribution.

## What this project is

A dependency-free C++17 library and CLI that monitors a DC charger bay, produces
an arithmetically exact session ledger and a durable tamper-evident journal,
plus a read-only offline dashboard that verifies that journal independently.
It does not control hardware.

## Ground rules

1. **Zero external dependencies.** Everything is C++17 standard library. This is
   deliberate: industrial operators audit what they ship, and a dependency tree
   is a supply-chain liability. Do not add one without explaining the trade-off.
2. **Integers for money and energy.** Never introduce floating point into the
   ledger, the tariff or the hash chain. Prices are micro-EUR, energy is
   microwatt-hours, durations are milliseconds. Exact arithmetic is testable;
   binary floating point drift is a billing dispute.
3. **Never interpolate across a dropout.** A missing sample must produce
   `MEASUREMENT_GAP` and a lower-bound flag, never a plausible-looking number.
4. **Diagnostics never block charging.** Raising an advisory is safe; halting a
   charge on a heuristic is not.
5. **Diagnostics ship disabled.** Thresholds are site-specific. An engineer sets
   them from real electrical data; the library must not guess.
6. **Nothing here commands anything.** No transmit path on the CAN bus, no write
   path from the dashboard to a charger or a server, no listening socket in the
   monitor. Supervision is files written atomically and read by someone else.
7. **Absent is not zero.** A quantity the monitor cannot know (availability with
   no in-service time, a stale measurement) is `-1` / `null` in JSON and *omitted*
   from Prometheus. Never export an unknown as `0`.
8. **Two implementations, one answer.** The journal rules live in the C++ library
   and, independently, in `dashboard/src/core.js`. A change to the journal
   layout, the pricing or the recovery rules must change both, and the parity
   tests (`node --test dashboard/tests`) must pass against a fresh build.

## Before opening a pull request

```bash
cmake -S charger_project -B build -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash charger_project/scripts/e2e_replay.sh
bash charger_project/scripts/consume_test.sh
clang-format --dry-run --Werror charger_project/src/*.cpp charger_project/tests/*.cpp \
    charger_project/include/*.hpp charger_project/tools/*/main.cpp

# dashboard (Node >= 20, no packages to install)
python3 scripts/build_dashboard.py --check
CHARGER_BUILD_DIR=build node --test dashboard/tests/core.test.mjs
node --test dashboard/tests/ui.test.mjs      # needs Playwright; skipped without it
```

A change that touches the sanitizer-relevant paths deserves an ASan + UBSan run
too (`-DCHARGER_SANITIZERS=ON`); parsers deserve a libFuzzer run
(`-DCHARGER_BUILD_FUZZ=ON` with clang).

If you changed a JSON field, bump nothing silently: the schemas in
`charger.snapshot.v1`, `charger.session.v1`, `charger.ledger.v1`,
`charger.portfolio.v1` and `charger.config.v1` are a compatibility contract.
Additive fields are fine, renaming or retyping is a breaking change and needs a
major version.

## Tests

Every behavioural claim needs a test. Prefer exact integer assertions over
range checks: `require(s.last_session->energy_uwh == 4800000000ull, ...)` proves
the contract, `require(x > 0)` proves nothing.

Crash recovery, journal tampering and tariff rounding deserve explicit
negative tests. If you add a code path that can produce a wrong invoice, add the
test that proves it does not.

## Secure development practices

What is actually done, so an integrator can see it: `-Wall -Wextra -Wpedantic
-Werror` under GCC and Clang, ASan + UBSan builds, a libFuzzer harness over
malformed frames, tariffs and journal rows, a downstream-consumer build from the
installed package, byte-for-byte parity tests between the C++ tools and the
independent JavaScript verifier, no third-party code, and review of every
change. This is a list of practices, not a certification claim.

## Reporting security issues

See [SECURITY.md](SECURITY.md). Please do not open a public issue first.

## Code of conduct

Be direct and technical. Assume good faith. Review the code, not the person.