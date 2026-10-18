## What this changes

<!-- One sentence. If it fixes a bug, link the issue. -->

## Why

<!-- The operational or economic reason. Not "it would be nice". -->

## How

<!-- The approach. Call out any change to a published schema or the public API. -->

## Verification

- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] `bash charger_project/scripts/e2e_replay.sh` passes
- [ ] `bash charger_project/scripts/consume_test.sh` passes (if the public API changed)
- [ ] `clang-format --dry-run --Werror` passes on the files you touched
- [ ] If the journal layout, pricing or recovery rules changed: `dashboard/src/core.js` changed with them, and `CHARGER_BUILD_DIR=build node --test dashboard/tests/core.test.mjs` passes
- [ ] If the dashboard changed: `python3 scripts/build_dashboard.py` was re-run and the bundle committed
- [ ] New behaviour has a test that would fail without the change

## Checklist

- [ ] No external dependency added
- [ ] No floating point in the ledger, the tariff or the hash chain
- [ ] No interpolation across a communication dropout
- [ ] Diagnostics do not block charging
- [ ] Nothing gained a way to transmit on the bus or to write to a charger or a server
- [ ] An unknown quantity is `-1` / `null` / omitted, never exported as `0`
- [ ] Schemas changed only additively, or the major version was bumped
- [ ] `CHANGELOG.md` updated