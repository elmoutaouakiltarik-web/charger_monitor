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

Run the whole gate in one command and paste its summary into the PR body:

```bash
bash scripts/verify.sh build
```

It covers what CI covers that does not need a special toolchain: a `-Werror`
Release build, `ctest`, the end-to-end replay, the `find_package` consumer,
journal tamper detection, synthetic-journal determinism, clang-format, and the
dashboard bundle plus the C++/JavaScript parity tests. It prints `PASS`, `FAIL`
or `SKIP` per step and a count at the end. A `SKIP` is not a pass: it means a
tool was missing, and the summary says so out loud.

The equivalent commands, if you prefer to run them yourself:

```bash
cmake -S charger_project -B build -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash charger_project/scripts/e2e_replay.sh
bash charger_project/scripts/consume_test.sh
clang-format --dry-run --Werror charger_project/src/*.cpp charger_project/tests/*.cpp \
    charger_project/include/*.hpp charger_project/tools/*/main.cpp
```

A change that touches the sanitizer-relevant paths deserves an ASan + UBSan run
too (`-DCHARGER_SANITIZERS=ON`); parsers deserve a libFuzzer run
(`-DCHARGER_BUILD_FUZZ=ON` with clang). Those two need toolchains CI installs and
`verify.sh` does not pretend to cover them.

If you changed a JSON field, bump nothing silently: the schemas in
`charger.snapshot.v1`, `charger.session.v1`, `charger.ledger.v1`,
`charger.portfolio.v1` and `charger.config.v1` are a compatibility contract.
Additive fields are fine, renaming or retyping is a breaking change and needs a
major version.

## Pull requests

**The pull request is the repository's shop window.** A reviewer, a prospective
integrator and a potential employer all read it before they read the code, so it
has to be as carefully made as the code.

- **Title**: `type(scope): one imperative line`. Never a branch name, never a
  generated identifier. `ai/agent-generated-branch-name` tells a reader nothing
  and looks automated because it is.
- **Body**: the template's guidance comments are instructions, not content.
  Delete them. A body that still contains `<!-- One sentence -->` says nobody
  read it.
- **Evidence over checkmarks.** A ticked box is a claim; the summary line from
  `scripts/verify.sh` is evidence. Paste the counts. If a step was skipped, name
  it.
- **Do not merge with failing checks.** Not "the failure looks unrelated" and
  not "it was green last week". If a check is red, the pull request is not ready.
  Re-run it or say why the result does not apply.
- **No self-merge of unreviewed work.** Changes touching the ledger, the pricing
  or the journal layout are the billing record and get a second pair of eyes.

### A note on CI on this repository

GitHub Actions is currently not running here: the account is locked over a
billing issue, so jobs fail to start rather than report a result. That is a
billing problem, not a green build, and the README says so.

Until it is resolved, `scripts/verify.sh` is the gate. Treat its summary as the
required evidence on every pull request, and do not read the absence of a red X
on a pull request as approval. Fixing the billing is what turns this from a
convention into an enforced check.

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

## AI-assisted development

This project uses AI-assisted tooling (large language models) to help draft code,
documentation and review comments. All AI-generated output is reviewed, tested
and verified by a human before it is merged. AI-generated code is subject to the
same review standards as human-written code: it must compile, pass the test
suite, respect the zero-dependency rule, and be explainable by a human maintainer.

If you use AI to generate code for this project, declare it in your pull request
and be prepared to explain every line. Do not paste AI-generated code that you
do not understand.

## Reporting security issues

See [SECURITY.md](SECURITY.md). Please do not open a public issue first.

## Code of conduct

Be direct and technical. Assume good faith. Review the code, not the person.