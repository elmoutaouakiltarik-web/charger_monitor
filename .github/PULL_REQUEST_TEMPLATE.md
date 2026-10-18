<!--
  Delete these instructions before you submit. A pull request whose body still
  contains them is not ready, and the ticked boxes below mean nothing on their
  own: they are claims, not evidence.
-->

## Title

`type(scope): one line, imperative, describing the change`

Conventional Commits (`fix`, `feat`, `docs`, `refactor`, `test`, `chore`).
Not a branch name, not "ai/agent-generated-branch-name", not "update".

## What this changes

<!-- One or two sentences. If it fixes a bug, link the issue. -->

## Why

<!-- The operational or economic reason. Not "it would be nice". -->

## How

<!-- The approach. Call out any change to a published schema or the public API. -->

## Verification

Run this and paste the summary line at the bottom:

```bash
bash scripts/verify.sh build
```

```
passed __   failed __   skipped __
```

Then tick only what applies:

- [ ] `scripts/verify.sh` reports 0 failed, and I pasted its summary above
- [ ] If the gate skipped a step, I say which and why below
- [ ] New behaviour has a test that would fail without the change

<!-- Anything you could not run, and what you did instead. Do not leave this
     silent: an unexplained gap reads as an oversight, not as diligence. -->

## Checklist

These are invariants of the project, not a formality. Leave a box unticked and
say why in the body.

- [ ] No external dependency added
- [ ] No floating point in the ledger, the tariff or the hash chain
- [ ] No interpolation across a communication dropout
- [ ] Diagnostics do not block charging
- [ ] Nothing gained a way to transmit on the bus or to write to a charger or a server
- [ ] An unknown quantity is `-1` / `null` / omitted, never exported as `0`
- [ ] If the journal layout, pricing or recovery rules changed: `dashboard/src/core.js`
      changed with them, and the C++/JS parity tests pass against a fresh build
- [ ] If the dashboard changed: `python3 scripts/build_dashboard.py` was re-run and the bundle committed
- [ ] Schemas changed only additively, or the major version was bumped
- [ ] `CHANGELOG.md` updated
