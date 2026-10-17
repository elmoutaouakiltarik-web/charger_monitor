# Contributing

Thanks for considering a contribution.

## What this project is

A dependency-free C++17 library and CLI that monitors a DC charger bay, produces
an exact session ledger and a durable tamper-evident journal. It does not
control hardware.

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

## Before opening a pull request

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/e2e_replay.sh
clang-format --dry-run --Werror src/*.cpp tests/*.cpp include/*.hpp
```

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

## Reporting security issues

See [SECURITY.md](SECURITY.md). Please do not open a public issue first.

## Code of conduct

Be direct and technical. Assume good faith. Review the code, not the person.