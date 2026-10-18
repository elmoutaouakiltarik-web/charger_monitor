// Browser tests for the single-file dashboard. Needs Playwright and a Chromium:
//   npm i -g playwright   (or PLAYWRIGHT_DIR=/path/to/node_modules/playwright)
//   CHARGER_CHROMIUM=/path/to/chrome  (optional; Playwright's own download otherwise)
// Skipped, not failed, when Playwright is not installed: the core tests are the
// ones that gate CI; these guard what a person actually sees.

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync, existsSync } from 'node:fs';
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { C, rechain } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const HTML = join(here, '..', 'charger-control-room.html');
const demo = readFileSync(join(here, '..', 'data', 'demo.journal'));
const require = createRequire(import.meta.url);

let pw = null;
for (const cand of [process.env.PLAYWRIGHT_DIR, 'playwright', '/opt/node22/lib/node_modules/playwright'].filter(Boolean)) {
  try { pw = require(cand); break; } catch { /* try the next */ }
}
const chromiumPath = process.env.CHARGER_CHROMIUM
  || ['/opt/pw-browsers/chromium-1194/chrome-linux/chrome'].find((p) => existsSync(p));
const skip = !pw || !existsSync(HTML) ? 'Playwright or the built bundle is not available' : false;

let browser;
before(async () => {
  if (skip) return;
  browser = await pw.chromium.launch({ executablePath: chromiumPath, args: ['--no-sandbox'] });
});
after(async () => { if (browser) await browser.close(); });

async function open(opts = {}, url = 'file://' + HTML) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 }, acceptDownloads: true, ...opts });
  const page = await ctx.newPage();
  page.problems = [];
  page.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warning') page.problems.push(m.text()); });
  page.on('pageerror', (e) => page.problems.push('pageerror: ' + e.message));
  await page.goto(url);
  return page;
}

test('the bundle pins its own script with a CSP hash and makes no external request', { skip }, async () => {
  const html = readFileSync(HTML, 'utf8');
  const csp = /Content-Security-Policy" content="([^"]+)"/.exec(html)[1];
  assert.match(csp, /default-src 'none'/);
  assert.match(csp, /connect-src 'self'/);
  const m = /<script>\n([\s\S]*?)\n<\/script>/.exec(html);
  const digest = createHash('sha256').update('\n' + m[1] + '\n').digest('base64');
  assert.ok(csp.includes(`'sha256-${digest}'`), 'CSP hash equals the inline script');
  assert.doesNotMatch(html, /<script[^>]+src=/i);
  assert.doesNotMatch(html, /<link[^>]+href="https?:/i);
  const page = await open();
  const requests = [];
  page.on('request', (r) => requests.push(r.url()));
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  assert.deepEqual(requests.filter((u) => !u.startsWith('file:') && !u.startsWith('data:')), []);
  assert.deepEqual(page.problems, []);
  await page.context().close();
});

test('demo shift verifies and shows the exact settlement numbers', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  const summary = readFileSync(join(here, 'golden', 'summary.txt'), 'utf8');
  const total = /invoice total\s+([0-9.]+) EUR/.exec(summary)[1];
  const hero = await page.textContent('.hero .v');
  assert.equal(hero.replace(/[\s ]/g, '').replace('EURexcl.VAT', ''), C.trimFixed(BigInt(total.replace('.', '')), 6, 2));
  await page.click('#tab-integrity');
  assert.match(await page.textContent('main'), new RegExp(C.sha256Hex(demo)));
  assert.deepEqual(page.problems, []);
  await page.context().close();
});

test('an altered record is named by line and the verified prefix only is shown', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo with one altered record');
  await page.waitForSelector('.banner.bad');
  assert.match(await page.textContent('.banner.bad'), /Chain broken at line 41/);
  assert.equal((await page.textContent('#tab-sessions .badge')).trim(), '20');
  await page.context().close();
});

test('a session opens its evidence: independent recalculation passes', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  await page.click('#tab-sessions');
  await page.click('tbody tr >> nth=0');
  await page.waitForSelector('#drawer');
  assert.equal(await page.locator('#drawer .chk .ico.bad').count(), 0);
  assert.ok(await page.locator('#drawer .chk .ico.ok').count() >= 8);
  assert.match(await page.textContent('#drawer'), /sha256sum/);
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('#drawer').count(), 0);
  await page.context().close();
});

test('hostile text in a journal is rendered as text, never as markup', { skip }, async () => {
  const lines = Buffer.from(demo).toString('utf8').split('\n').filter(Boolean);
  const evil = '<img src=x onerror="window.__pwned=1">';
  const forged = lines.map((l) => { const c = l.split('\t'); if (c[2] === 'session_close') { c[3] = evil; c[9] = '<b>boom</b>'; } else c[3] = evil; return c.join('\t'); });
  const page = await open();
  await page.setInputFiles('#file', { name: 'evil.journal', mimeType: 'application/octet-stream', buffer: Buffer.from(rechain(forged)) });
  await page.waitForSelector('.banner.ok');
  await page.click('#tab-sessions');
  await page.waitForSelector('tbody tr');
  assert.equal(await page.locator('main img, main b').count(), 0, 'no element was created from the data');
  assert.equal(await page.evaluate(() => window.__pwned), undefined);
  assert.match(await page.textContent('tbody'), /<img src=x/);
  assert.deepEqual(page.problems.filter((p) => !/Content Security Policy|violates/i.test(p)), []);
  await page.context().close();
});

test('the demo anchor verifies; a rewritten journal fails it', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  await page.click('#tab-integrity');
  // the embedded demo anchor was applied on load
  assert.match(await page.textContent('main'), /Anchor verified/);
  const dir = mkdtempSync(join(tmpdir(), 'ccr-'));
  try {
    const lines = Buffer.from(demo).toString('utf8').split('\n').filter(Boolean);
    const c = lines[5].split('\t'); c[5] = String(BigInt(c[5]) + 1n); lines[5] = c.join('\t');
    await page.setInputFiles('#file', { name: 'rewritten.journal', mimeType: 'text/plain', buffer: Buffer.from(rechain(lines)) });
    await page.waitForSelector('.banner.ok');
    await page.click('#tab-integrity');
    await page.waitForSelector('.banner.bad');
    assert.match(await page.textContent('main'), /Anchor does NOT verify/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
  await page.context().close();
});

test('an earlier copy proves the history: prefix is accepted, a rewritten past is named', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  await page.click('#tab-integrity');
  const lines = Buffer.from(demo).toString('utf8').split('\n').filter(Boolean);
  await page.setInputFiles('#compare-file', { name: 'yesterday.journal', mimeType: 'text/plain', buffer: Buffer.from(lines.slice(0, 200).join('\n') + '\n') });
  await page.waitForSelector('text=The earlier copy is an exact prefix');
  assert.match(await page.textContent('main'), /200 lines of the earlier copy are unchanged here; 183 lines were appended/);
  const c = lines[10].split('\t'); c[5] = String(BigInt(c[5]) + 1n); lines[10] = c.join('\t');
  await page.setInputFiles('#compare-file', { name: 'rewritten.journal', mimeType: 'text/plain', buffer: Buffer.from(rechain(lines.slice(0, 200))) });
  await page.waitForSelector('text=The histories DIVERGE');
  assert.match(await page.textContent('main'), /agree on the first 10 lines and differ from line 11/);
  await page.context().close();
});

test('the saved report is one file that re-verifies itself', { skip }, async () => {
  const page = await open();
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  const [dl] = await Promise.all([page.waitForEvent('download'), page.click('button[aria-label="Save report"]')]);
  const out = join(mkdtempSync(join(tmpdir(), 'ccr-')), 'report.html');
  await dl.saveAs(out);
  const saved = readFileSync(out, 'utf8');
  const csp = /Content-Security-Policy" content="([^"]+)"/.exec(saved)[1];
  const m = /<script>\n([\s\S]*?)\n<\/script>/.exec(saved);
  assert.ok(csp.includes(createHash('sha256').update('\n' + m[1] + '\n').digest('base64')), 'script unchanged, pin still valid');
  const p2 = await open({}, 'file://' + out);
  await p2.waitForSelector('.banner.ok');
  assert.match(await p2.textContent('.banner.ok'), /Chain verified/);
  assert.deepEqual(p2.problems, []);
  await page.context().close(); await p2.context().close();
  rmSync(dirname(out), { recursive: true, force: true });
});

test('live view polls same-origin status files and flags a stale or missing one', { skip }, async () => {
  const root = mkdtempSync(join(tmpdir(), 'ccr-live-'));
  mkdirSync(join(root, 'status'));
  const snap = (asset, ts, state) => JSON.stringify({ timestamp: ts, type: 'snapshot', data: { schema: 'charger.snapshot.v1', version: '1.4.0', asset_id: asset, state,
    voltage_mv: 480000, current_ma: 10000, power_mw: 4800000, measurements_stale: false, status_stale: false, measurement_age_ms: 5, fault_code: '', diagnostics: {},
    availability_ppm: 990000, utilization_ppm: 500000, accepted_measurement_frames: 10, accepted_status_frames: 10, rejected_frames: 0, sessions_closed: 1, sessions_billable: 1, timeout_events: 0, session: null } });
  const now = C.formatUtc(Math.floor(Date.now() / 1000));
  writeFileSync(join(root, 'status', 'fresh.json'), snap('bay-fresh', now, 'CHARGING'));
  writeFileSync(join(root, 'status', 'stale.json'), snap('bay-stale', '2020-01-01T00:00:00Z', 'CHARGING'));
  writeFileSync(join(root, 'index.html'), readFileSync(HTML));
  const server = createServer((req, res) => {
    const f = join(root, req.url.split('?')[0]);
    if (req.url === '/' ) { res.end(readFileSync(join(root, 'index.html'))); return; }
    if (existsSync(f)) { res.end(readFileSync(f)); } else { res.statusCode = 404; res.end('no'); }
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  try {
    const page = await open({}, `http://127.0.0.1:${port}/#live`);
    await page.fill('#sources', 'status/fresh.json\nstatus/stale.json\nstatus/missing.json');
    await page.click('#poll-btn');
    await page.waitForSelector('text=bay-fresh');
    await page.waitForSelector('text=bay-stale');
    await page.waitForSelector('text=HTTP 404');
    assert.match(await page.textContent('main'), /status file not updated for/);
    assert.match(await page.textContent('main'), /updated \d+ s ago/);
    await page.context().close();
  } finally { server.close(); rmSync(root, { recursive: true, force: true }); }
});

test('phone width: no horizontal page scroll on any tab', { skip }, async () => {
  const page = await open({ viewport: { width: 375, height: 800 } });
  await page.click('text=Demo shift (synthetic)');
  await page.waitForSelector('.banner.ok');
  for (const tab of ['overview', 'fleet', 'sessions', 'integrity', 'live']) {
    await page.click('#tab-' + tab);
    await page.waitForTimeout(250);
    const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    assert.ok(over <= 1, `${tab} overflows by ${over}px`);
  }
  await page.context().close();
});

test('Italian and dark theme render without missing strings', { skip }, async () => {
  const page = await open({ colorScheme: 'dark' });
  await page.click('button:has-text("IT")');
  await page.click('text=Turno demo (sintetico)');
  await page.waitForSelector('.banner.ok');
  for (const tab of ['overview', 'fleet', 'sessions', 'integrity', 'live']) {
    await page.click('#tab-' + tab);
    await page.waitForTimeout(200);
    const text = await page.textContent('main');
    assert.doesNotMatch(text, /\b(tab|col|why|reason|stop|proof|diag|state|k|c|d|i|live|anchor)_[a-z_0-9]+\b/, `untranslated key on ${tab}`);
  }
  await page.context().close();
});
