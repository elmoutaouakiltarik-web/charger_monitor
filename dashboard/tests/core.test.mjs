// node --test dashboard/tests
//
// The dashboard's core is a second, independent implementation of the journal
// rules. These tests pin it to the C++ tools in three ways:
//   1. golden files produced by charger_settle / charger_monitor --portfolio on
//      the committed demo journal (always run);
//   2. tamper scenarios whose expected broken line is stated here (always run);
//   3. live parity against the built binaries on freshly synthesised journals
//      (runs when CHARGER_BUILD_DIR points at a build tree, as CI does).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { C, sha, rechain } from './helpers.mjs';
const here = dirname(fileURLToPath(import.meta.url));
const data = join(here, '..', 'data');
const golden = join(here, 'golden');
const demo = readFileSync(join(data, 'demo.journal'));
const BUILD = process.env.CHARGER_BUILD_DIR || '';
const haveBinaries = BUILD && existsSync(join(BUILD, 'charger_settle')) && existsSync(join(BUILD, 'charger_monitor'));


// ------------------------------------------------------------------ SHA-256
test('sha256 matches FIPS vectors and node:crypto on random splits', () => {
  assert.equal(C.sha256Hex(''), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(C.sha256Hex('abc'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(
    C.sha256Hex('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'),
    '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff);
  for (let n = 0; n < 200; n++) {
    const b = new Uint8Array(rnd() % 400);
    for (let i = 0; i < b.length; i++) b[i] = rnd() & 255;
    const h = new C.Sha256();
    for (let o = 0; o < b.length;) { const k = rnd() % 70; h.update(b, o, Math.min(b.length, o + k)); o += k; }
    assert.equal(h.hex(), sha(b));
  }
  // block-boundary lengths
  for (const len of [55, 56, 57, 63, 64, 65, 119, 120, 128]) {
    const b = new Uint8Array(len).fill(0x61);
    assert.equal(C.sha256Hex(b), sha(b), 'len ' + len);
  }
});

// -------------------------------------------------------------- exact number
test('formatFixed is exact and sign aware', () => {
  assert.equal(C.formatFixed(1250000n, 6), '1.250000');
  assert.equal(C.formatFixed(5n, 6), '0.000005');
  assert.equal(C.formatFixed(0n, 6), '0.000000');
  assert.equal(C.formatFixed(-1500000n, 6), '-1.500000');
  assert.equal(C.formatFixed(9223372036854775807n, 6), '9223372036854.775807');
  assert.equal(C.trimFixed(5380000n, 6, 2), '5.38');
  assert.equal(C.trimFixed(500000n, 6, 2), '0.50');
  assert.equal(C.trimFixed(501000n, 6, 2), '0.501');
  assert.equal(C.eur(1n), '0.000001');
});

test('parseU64 mirrors the C++ reader', () => {
  assert.equal(C.parseU64('0'), 0n);
  assert.equal(C.parseU64('18446744073709551615'), 18446744073709551615n);
  assert.equal(C.parseU64('18446744073709551616'), null);
  for (const bad of ['', '-1', '+1', '1 ', ' 1', '1e3', '0x10', '١']) assert.equal(C.parseU64(bad), null, bad);
});

test('UTC parsing refuses what it cannot read', () => {
  assert.equal(C.parseUtc('1970-01-01T00:00:00Z'), 0);
  assert.equal(C.parseUtc('2026-10-18T00:06:40Z'), Date.UTC(2026, 9, 18, 0, 6, 40) / 1000);
  assert.equal(C.parseUtc('2026-10-18 00:06:40'), null);
  assert.equal(C.parseUtc('2026-13-18T00:06:40Z'), null);
  assert.equal(C.parseUtc(''), null);
  assert.equal(C.formatUtc(C.parseUtc('2026-02-28T23:59:59Z')), '2026-02-28T23:59:59Z');
});

// ----------------------------------------------------------- demo journal
const res = await C.parseJournal(demo);

test('demo journal: chain verifies and the digest matches sha256sum', () => {
  assert.equal(res.chain.intact, true);
  assert.equal(res.chain.firstBrokenLine, 0);
  assert.equal(res.fileSha256, sha(demo));
  assert.equal(res.chain.lastHash, res.lineHashes.at(-1)[1]);
  assert.equal(res.lines, res.lineHashes.length);
  assert.equal(res.closes, res.sessions.length);
});

test('demo journal: orphans are reported, not billed', () => {
  assert.ok(res.orphans.length >= 1, 'the synthetic shift contains an orphan');
  for (const o of res.orphans) {
    assert.equal(res.sessions.some((s) => s.asset === o.asset && s.id === o.id), false);
  }
});

test('portfolio matches charger_monitor --portfolio exactly', () => {
  const want = JSON.parse(readFileSync(join(golden, 'portfolio.json'), 'utf8'));
  const got = C.portfolioJson(C.rollup(res.sessions));
  assert.deepEqual(got, want);
});

test('settlement CSV matches charger_settle byte for byte', () => {
  const want = readFileSync(join(golden, 'settle.csv'), 'utf8');
  assert.equal(C.settlementCsv(res.sessions), want);
});

test('settlement summary matches charger_settle --summary', () => {
  const want = readFileSync(join(golden, 'summary.txt'), 'utf8');
  const s = C.settlementSummary(res.sessions);
  const line = (label, v) => new RegExp('^\\s*' + label + '\\s+' + v + '(\\s|$)', 'm');
  assert.match(want, line('sessions closed', s.closed));
  assert.match(want, line('invoice now', s.billable));
  assert.match(want, line('held for review', s.held));
  assert.match(want, line('billable energy', s.billableEnergyWh + ' Wh'));
  assert.match(want, line('ENERGY', C.formatFixed(s.energyCost, 6) + ' EUR'));
  assert.match(want, line('TIME', C.formatFixed(s.timeCost, 6) + ' EUR'));
  assert.match(want, line('FLAT', C.formatFixed(s.flatCost, 6) + ' EUR'));
  assert.match(want, line('invoice total', C.formatFixed(s.invoiceTotal, 6) + ' EUR'));
  if (s.held > 0) assert.match(want, new RegExp('held \\(not invoiced\\) ' + C.formatFixed(s.heldTotal, 6).replace('.', '\\.') + ' EUR'));
});

test('every closed session re-prices from its tariff of record', () => {
  let checked = 0;
  for (const s of res.sessions) {
    const r = C.recompute(s);
    assert.equal(r.available, true, `${s.asset}#${s.id} has a tariff block`);
    const bad = r.checks.filter((c) => !c.ok);
    assert.deepEqual(bad, [], `${s.asset}#${s.id} line ${s.line}`);
    checked++;
  }
  assert.ok(checked > 50);
});

test('dispute pack facts equal the independently parsed session', () => {
  const files = ['dispute-index.json'];
  const idx = JSON.parse(readFileSync(join(golden, files[0]), 'utf8'));
  assert.ok(idx.length >= 3);
  for (const { asset, id } of idx) {
    const pack = JSON.parse(readFileSync(join(golden, `dispute-${asset}-${id}.json`), 'utf8'));
    const s = res.sessions.find((x) => x.asset === asset && x.id === BigInt(id));
    assert.ok(s, `${asset}#${id}`);
    assert.equal(pack.journal.sha256, res.fileSha256);
    assert.equal(pack.journal.lines, res.lines);
    assert.equal(pack.journal.last_hash, res.chain.lastHash);
    assert.equal(pack.provenance.journal_open_line, s.openLine);
    assert.equal(pack.provenance.journal_close_line, s.line);
    assert.equal(pack.provenance.close_hash, s.hash);
    assert.equal(pack.provenance.close_prev_hash, s.prevHash);
    assert.equal(pack.provenance.close_line_raw, C.rawLineOf(res, s));
    assert.equal(pack.session.energy_uwh, s.energyUwh.toString());
    assert.equal(pack.session.total_cost, C.formatFixed(s.totalCost, 6));
    assert.equal(pack.session.quality, s.quality);
    assert.equal(pack.session.billable, s.billable);
    assert.equal(pack.tariff.energy_micro_eur_per_kwh, JSON.parse(s.tariffJson).energy_micro_eur_per_kwh);
  }
});

test('anchor round trip: byte-compatible document, growth allowed, rewrite refused', async () => {
  const doc = C.makeAnchor(res, '2026-10-18T10:00:00Z');
  const a = C.parseAnchor(doc);
  assert.equal(a.ok, true);
  assert.equal(a.lines, BigInt(res.lines));
  assert.equal(C.verifyAnchor(res, a).ok, true);
  assert.equal(C.verifyAnchor(res, a).wholeFileSame, true);

  // The journal grows after anchoring: more lines are reported, not refused.
  const first = res._bytes.subarray(0, res._bytes.indexOf(10, 0) + 1);
  const half = Math.floor(res.rawLines / 2);
  let off = 0;
  for (let i = 0; i < half; i++) off = res._bytes.indexOf(10, off) + 1;
  const prefix = await C.parseJournal(res._bytes.subarray(0, off));
  const prefixAnchor = C.parseAnchor(C.makeAnchor(prefix, '2026-10-18T09:00:00Z'));
  const grown = C.verifyAnchor(res, prefixAnchor);
  assert.equal(grown.ok, true);
  assert.equal(grown.appended, res.lines - prefix.lines);
  assert.equal(grown.wholeFileSame, false);

  // A full rewrite that recomputes its own chain still fails the anchor.
  const text = Buffer.from(res._bytes).toString('utf8').split('\n');
  const cols = text[3].split('\t');
  cols[5] = String(BigInt(cols[5]) + 1n);
  const rewritten = rechain(text.filter(Boolean).map((l, i) => (i === 3 ? cols.join('\t') : l)));
  const rr = await C.parseJournal(Buffer.from(rewritten));
  assert.equal(rr.chain.intact, true, 'the rewritten journal is internally consistent');
  const refused = C.verifyAnchor(rr, a);
  assert.equal(refused.ok, false);
  assert.match(refused.error, /rewritten|shorter/);

  // Truncation below the anchor is refused with its own message.
  const short = await C.parseJournal(res._bytes.subarray(0, first.length));
  assert.match(C.verifyAnchor(short, a).error, /shorter than the anchor/);
});

test('an earlier copy: identical, a prefix, a rewrite, a removal, a broken chain', async () => {
  const half = lines.slice(0, 120);
  const earlier = await C.parseJournal(Buffer.from(half.join('\n') + '\n'));
  const cur = res;
  assert.deepEqual(C.comparePrefix(cur, cur).relation, 'identical');
  const p = C.comparePrefix(cur, earlier);
  assert.equal(p.relation, 'prefix');
  assert.equal(p.appended, cur.lines - earlier.lines);
  // a full rewrite of an early line that recomputes the chain: both verify, histories diverge at that line
  const c = lines[30].split('\t'); c[5] = String(BigInt(c[5]) + 1n);
  const rewritten = await C.parseJournal(Buffer.from(rechain(lines.map((l, i) => (i === 30 ? c.join('\t') : l)))));
  assert.equal(rewritten.chain.intact, true);
  const d = C.comparePrefix(rewritten, earlier);
  assert.equal(d.relation, 'diverged');
  assert.equal(d.agreeLines, 30);
  assert.equal(d.firstDifferentLine, 31);
  // the current journal is shorter than the earlier copy: lines were removed
  const sh = C.comparePrefix(earlier, cur);
  assert.equal(sh.relation, 'diverged');
  assert.equal(sh.shorter, true);
  // a broken chain on either side refuses to say anything
  const bad = Buffer.from(res._bytes); bad[300] ^= 1;
  const broken = await C.parseJournal(bad);
  assert.equal(C.comparePrefix(cur, broken).relation, 'broken');
  assert.equal(C.comparePrefix(cur, broken).error, 'earlier');
  assert.equal(C.comparePrefix(broken, cur).error, 'current');
});

test('anchor parser rejects malformed documents', () => {
  assert.equal(C.parseAnchor('{}').ok, false);
  assert.equal(C.parseAnchor('{"schema":"charger.anchor.v1"}').ok, false);
  const good = C.makeAnchor(res, '2026-10-18T10:00:00Z');
  assert.equal(C.parseAnchor(good.replace(/"lines":"\d+"/, '"lines":"x"')).ok, false);
  assert.equal(C.parseAnchor(good.replace(res.fileSha256, 'abc')).ok, false);
});

test('makeAnchor refuses a broken journal', async () => {
  const bad = Buffer.from(res._bytes);
  bad[200] ^= 1;
  const r = await C.parseJournal(bad);
  assert.throws(() => C.makeAnchor(r, '2026-10-18T10:00:00Z'), /refusing to anchor/);
});

// --------------------------------------------------------------- tampering

const lines = Buffer.from(demo).toString('utf8').split('\n').filter(Boolean);
const joinLines = (arr) => Buffer.from(arr.join('\n') + '\n');

const scenarios = [
  ['one field edited, hash untouched', () => joinLines(lines.map((l, i) => (i === 9 ? l.replace(/\t(\d+)\t/, (m, d) => `\t${+d + 1}\t`) : l))), 10, 'record-altered'],
  ['a line deleted', () => joinLines(lines.filter((_, i) => i !== 12)), 13, 'link-broken'],
  ['two lines swapped', () => joinLines(lines.map((l, i) => (i === 20 ? lines[21] : i === 21 ? lines[20] : l))), 21, 'link-broken'],
  ['a line inserted', () => joinLines([...lines.slice(0, 5), lines[3], ...lines.slice(5)]), 6, 'link-broken'],
  ['final line truncated mid-record', () => Buffer.from(lines.join('\n') + '\n' + lines[0].slice(0, 90)), lines.length + 1, null],
  ['first line prefix tampered', () => joinLines(lines.map((l, i) => (i === 0 ? l.replace('\t', '\t9') : l))), 1, 'record-altered']
];
for (const [name, build, wantLine, wantKind] of scenarios) {
  test('tamper detected: ' + name, async () => {
    const r = await C.parseJournal(build());
    assert.equal(r.chain.intact, false);
    assert.equal(r.chain.firstBrokenLine, wantLine);
    if (wantKind) assert.equal(r.chain.forensics.kind, wantKind);
    assert.ok(r.lines < r.rawLines || r.chain.truncated);
  });
}

test('CRLF line endings verify exactly like LF (the C++ reader strips the CR)', async () => {
  const crlf = Buffer.from(lines.join('\r\n') + '\r\n');
  const r = await C.parseJournal(crlf);
  assert.equal(r.chain.intact, true);
  assert.equal(r.lines, lines.length);
});

test('a journal without a trailing newline still counts its last line', async () => {
  const r = await C.parseJournal(Buffer.from(lines.join('\n')));
  assert.equal(r.chain.intact, true);
  assert.equal(r.lines, lines.length);
});

test('empty and garbage inputs never throw', async () => {
  const e = await C.parseJournal(new Uint8Array(0));
  assert.equal(e.chain.intact, true);
  assert.equal(e.lines, 0);
  const g = await C.parseJournal(Buffer.from('not a journal\nat all\n'));
  assert.equal(g.chain.intact, false);
  assert.equal(g.chain.firstBrokenLine, 1);
  const bin = await C.parseJournal(Buffer.from(Array.from({ length: 4096 }, (_, i) => (i * 37) & 255)));
  assert.equal(bin.chain.intact, false);
});

test('the verified prefix survives a break: sessions after it are not reported', async () => {
  const r = await C.parseJournal(joinLines(lines.filter((_, i) => i !== 40)));
  assert.equal(r.chain.firstBrokenLine, 41);
  assert.equal(r.lines, 40);
  assert.ok(r.sessions.every((s) => s.line < 41));
});

// ----------------------------------------------------- price re-derivation
test('recompute notices an edited cost even when the chain was recomputed', async () => {
  const idx = lines.findIndex((l) => l.split('\t')[2] === 'session_close');
  const cols = lines[idx].split('\t');
  cols[7] = String(BigInt(cols[7]) + 10000n); // total cost in the prefix
  const forged = lines.map((l, i) => (i === idx ? cols.join('\t') : l));
  const r = await C.parseJournal(Buffer.from(rechain(forged)));
  assert.equal(r.chain.intact, true, 'a full rewrite verifies as a chain');
  const s = r.sessions.find((x) => x.line === idx + 1);
  const rc = C.recompute(s);
  assert.equal(rc.ok, false);
  assert.ok(rc.checks.some((c) => c.name === 'total_cost' && !c.ok));
});

test('recompute covers clamps, step ceilings and overflow like Monitor::price', () => {
  const base = {
    energyUwh: 1234567890n, durationMs: 3601000n, samples: 5, gaps: 0, stopReason: 'STATUS_OFF', billable: true,
    quality: 'OK', timeUpper: false, energyWh: 1234n, chargingS: 3601n, billedEnergyWh: 1240n, billedTimeS: 3660n
  };
  const tariff = (extra) => JSON.stringify({ schema: 'charger.tariff.v1', energy_micro_eur_per_kwh: '400000',
    time_micro_eur_per_hour: '2400000', flat_micro_eur: '500000', energy_step_wh: 10, time_step_s: 60, ...extra });
  const mk = (extra, over) => {
    const t = JSON.parse(tariff(extra));
    const wh = 1234n, billedWh = 1240n, billedS = 3660n;
    const e = (billedWh * 400000n + 500n) / 1000n, tm = (billedS * 2400000n + 1800n) / 3600n;
    let total = e + tm + 500000n;
    if (t.min_price_micro_eur && total < BigInt(t.min_price_micro_eur)) total = BigInt(t.min_price_micro_eur);
    if (t.max_price_micro_eur && total > BigInt(t.max_price_micro_eur)) total = BigInt(t.max_price_micro_eur);
    return { ...base, energyWh: wh, energyCost: e, timeCost: tm, flatCost: 500000n, totalCost: total,
      tariffJson: tariff(extra), ...over };
  };
  assert.equal(C.recompute(mk({})).ok, true);
  assert.equal(C.recompute(mk({ min_price_micro_eur: '9000000' })).ok, true, 'min clamp');
  assert.equal(C.recompute(mk({ max_price_micro_eur: '1000000' })).ok, true, 'max clamp');
  assert.equal(C.recompute(mk({}, { totalCost: 1n })).ok, false);
  assert.equal(C.recompute({ ...base, tariffJson: '' }).available, false);
  assert.equal(C.recompute({ ...base, tariffJson: '{not json' }).available, false);
  // an exact multiple of the step is not increased
  const exact = mk({}, { energyUwh: 1230000000n, energyWh: 1230n, durationMs: 3600000n, chargingS: 3600n });
  const rc = C.recompute({ ...exact, billedEnergyWh: 1230n, billedTimeS: 3600n,
    energyCost: (1230n * 400000n + 500n) / 1000n, timeCost: 2400000n, totalCost: (1230n * 400000n + 500n) / 1000n + 2400000n + 500000n });
  assert.equal(rc.checks.find((c) => c.name === 'billed_energy_wh').recomputed, '1230');
  assert.equal(rc.checks.find((c) => c.name === 'billed_time_s').recomputed, '3600');
});

// -------------------------------------------------------------- aggregates
test('hour slices sum back to the session energy exactly', () => {
  for (const s of res.sessions.slice(0, 120)) {
    const slices = C.hourSlices(s);
    const sum = slices.reduce((a, x) => a + x.uwh, 0n);
    assert.equal(sum, s.energyUwh === 0n || s.durationMs <= 0n ? 0n : s.energyUwh, `${s.asset}#${s.id}`);
  }
});

test('daily buckets and heat map conserve energy', () => {
  const total = res.sessions.reduce((a, s) => a + s.energyUwh, 0n);
  const days = C.dailyBuckets(res.sessions);
  assert.equal(days.reduce((a, d) => a + d.billableUwh + d.heldUwh, 0n), total);
  const heat = C.weekHourHeat(res.sessions).flat().reduce((a, x) => a + x, 0n);
  const apportionable = res.sessions.filter((s) => s.durationMs > 0n && s.energyUwh > 0n && C.parseUtc(s.closedAt) !== null)
    .reduce((a, s) => a + s.energyUwh, 0n);
  assert.equal(heat, apportionable);
});

test('held reasons are explained for every held session', () => {
  for (const s of res.sessions) {
    if (s.billable) assert.equal(C.heldReason(s), '');
    else assert.notEqual(C.heldReason(s), '');
  }
});

// ----------------------------------------------------------- hostile input
test('hostile text in a journal never reaches the CSV as a formula', async () => {
  const idx = lines.findIndex((l) => l.split('\t')[2] === 'session_close');
  const cols = lines[idx].split('\t');
  cols[3] = '=HYPERLINK("http://x","y")';
  const r = await C.parseJournal(Buffer.from(rechain(lines.map((l, i) => (i === idx ? cols.join('\t') : l)))));
  const csv = C.settlementCsv(r.sessions);
  assert.ok(csv.includes('"\'=HYPERLINK(""http://x"",""y"")"'));
});

test('snapshot reader accepts the stdout envelope, bare documents and NDJSON', () => {
  const doc = { schema: 'charger.snapshot.v1', asset_id: 'bay-01', state: 'CHARGING' };
  assert.equal(C.parseSnapshotText(JSON.stringify(doc)).data.state, 'CHARGING');
  const env = C.parseSnapshotText(JSON.stringify({ timestamp: '2026-10-18T00:00:00Z', type: 'snapshot', data: doc }));
  assert.equal(env.timestamp, '2026-10-18T00:00:00Z');
  assert.equal(C.parseSnapshotText('{"schema":"other"}'), null);
  assert.equal(C.parseSnapshotText('nope'), null);
  assert.equal(C.parseNdjson(`${JSON.stringify(doc)}\n\nbroken\n${JSON.stringify(doc)}\n`).length, 2);
});

// ------------------------------------------------------ live parity vs C++
function run(bin, args, input) {
  return spawnSync(join(BUILD, bin), args, { input, encoding: 'utf8', maxBuffer: 1 << 28 });
}

test('live parity: JS and C++ agree on freshly synthesised journals', { skip: !haveBinaries || !existsSync(join(BUILD, 'charger_synth')) }, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'charger-parity-'));
  try {
    for (const seed of [1, 2, 3, 7, 42]) {
      const j = join(dir, `s${seed}.journal`);
      const gen = run('charger_synth', ['--out', j, '--seed', String(seed), '--days', '3', '--bays', '4']);
      assert.equal(gen.status, 0, gen.stderr);
      const bytes = readFileSync(j);
      const r = await C.parseJournal(bytes);
      assert.equal(r.chain.intact, true, `seed ${seed}`);
      assert.equal(r.fileSha256, sha(bytes));
      const cpp = run('charger_monitor', ['--portfolio', j]);
      assert.deepEqual(C.portfolioJson(C.rollup(r.sessions)), JSON.parse(cpp.stdout), `portfolio seed ${seed}`);
      const csv = run('charger_settle', [j]);
      assert.equal(C.settlementCsv(r.sessions), csv.stdout, `csv seed ${seed}`);
      for (const s of r.sessions) assert.ok(C.recompute(s).ok, `${s.asset}#${s.id} seed ${seed}`);

      // tamper one byte inside a random line: both implementations stop at the same line
      for (const lineNo of [1, 7, Math.max(1, r.rawLines - 1)]) {
        const b = Buffer.from(bytes);
        let off = 0;
        for (let i = 1; i < lineNo; i++) off = b.indexOf(10, off) + 1;
        const tab = b.indexOf(9, b.indexOf(9, off) + 1);
        b[tab + 6] = b[tab + 6] === 0x7a ? 0x79 : 0x7a; // flip a payload character
        const t = join(dir, `t${seed}-${lineNo}.journal`);
        writeFileSync(t, b);
        const tr = await C.parseJournal(b);
        const cppV = run('charger_settle', [t, '--verify-only']);
        assert.equal(cppV.status, 3, 'C++ flags the tamper');
        assert.match(cppV.stderr, new RegExp(`line ${tr.chain.firstBrokenLine}:`), `seed ${seed} line ${lineNo}`);
      }

      // the anchor the C++ tool writes is accepted by JS and vice versa
      const anchor = join(dir, `a${seed}.anchor`);
      const ar = run('charger_monitor', ['--anchor-journal', '--journal', j, '--anchor-file', anchor]);
      assert.equal(ar.status, 0, ar.stderr);
      const parsed = C.parseAnchor(readFileSync(anchor, 'utf8'));
      assert.equal(parsed.ok, true);
      assert.equal(parsed.lastHash, r.chain.lastHash);
      assert.equal(parsed.journalSha256, r.fileSha256);
      assert.equal(C.verifyAnchor(r, parsed).ok, true);
      const mine = join(dir, `m${seed}.anchor`);
      writeFileSync(mine, C.makeAnchor(r, '2026-10-18T10:00:00Z'));
      const vr = run('charger_monitor', ['--verify-anchor', '--journal', j, '--anchor-file', mine]);
      assert.equal(vr.status, 0, vr.stderr + vr.stdout);
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
