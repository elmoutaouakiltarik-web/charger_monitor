// Regenerates the dashboard screenshots used by the README.
//   node scripts/make_dashboard_screenshots.cjs
// Needs Playwright (npm i -g playwright) and a Chromium; set CHARGER_CHROMIUM
// to the browser binary if Playwright cannot find its own. Everything shown is
// the SYNTHETIC demo shift embedded in the bundle.
const path = require('path');
const fs = require('fs');
const root = path.resolve(__dirname, '..');
let pw;
for (const c of [process.env.PLAYWRIGHT_DIR, 'playwright', '/opt/node22/lib/node_modules/playwright'].filter(Boolean)) {
  try { pw = require(c); break; } catch (e) { /* next */ }
}
if (!pw) { console.error('Playwright not found'); process.exit(1); }
const exe = process.env.CHARGER_CHROMIUM || ['/opt/pw-browsers/chromium-1194/chrome-linux/chrome'].find((p) => fs.existsSync(p));

(async () => {
  const browser = await pw.chromium.launch({ executablePath: exe, args: ['--no-sandbox'] });
  const url = 'file://' + path.join(root, 'dashboard', 'charger-control-room.html');
  async function shot(name, scheme, steps, height) {
    const ctx = await browser.newContext({ viewport: { width: 1360, height }, colorScheme: scheme, deviceScaleFactor: 1 });
    const page = await ctx.newPage();
    await page.goto(url);
    await steps(page);
    await page.waitForTimeout(700);
    await page.screenshot({ path: path.join(root, 'docs', name) });
    await ctx.close();
  }
  const overview = async (p) => { await p.click('text=Demo shift (synthetic)'); await p.waitForSelector('.banner.ok'); };
  await shot('dashboard-overview-dark.png', 'dark', overview, 1010);
  await shot('dashboard-overview-light.png', 'light', overview, 1010);
  await shot('dashboard-integrity.png', 'dark', async (p) => { await p.click('text=Demo with one altered record'); await p.waitForSelector('.banner.bad'); }, 760);
  await shot('dashboard-session.png', 'dark', async (p) => {
    await overview(p); await p.click('#tab-sessions'); await p.click('tbody tr >> nth=1'); await p.waitForSelector('#drawer');
  }, 900);
  await shot('dashboard-live.png', 'dark', async (p) => {
    await p.goto(url + '#live'); await p.click('text=Demo capture (synthetic day)'); await p.waitForTimeout(400);
    await p.fill('input[type=range]', '156');
  }, 1060);
  await browser.close();
})();
