## What this changes

<!-- One sentence. If it fixes a bug, link the issue. -->

## Why

<!-- The operational or economic reason. Not "it would be nice". -->

## How

<!-- The approach. Call out any change to a published schema or the public API. -->

## Verification

- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] `bash scripts/e2e_replay.sh` passes
- [ ] `bash scripts/consume_test.sh` passes (if the public API changed)
- [ ] `clang-format --dry-run --Werror src/*.cpp tests/*.cpp include/*.hpp` passes
- [ ] New behaviour has a test that would fail without the change

## Checklist

- [ ] No external dependency added
- [ ] No floating point in the ledger, the tariff or the hash chain
- [ ] No interpolation across a communication dropout
- [ ] Diagnostics do not block charging
- [ ] Schemas changed only additively, or the major version was bumped
- [ ] `CHANGELOG.md` updated