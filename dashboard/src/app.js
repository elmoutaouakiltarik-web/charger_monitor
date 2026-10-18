/* charger_monitor control room — application.
 *
 * Read-only by construction: there is no code path in this file that can write
 * to a charger, a bus or a server. It reads files you give it (or same-origin
 * status files you name), computes everything locally, and draws it.
 */
(function () {
  'use strict';
  var C = window.ChargerCore, D = window.ChargerDom, X = window.ChargerCharts, I = window.ChargerI18n;
  var h = D.h, s = D.s, icon = D.icon;

  // ------------------------------------------------------------------ prefs
  function store(k, v) {
    try { if (v === undefined) return window.localStorage.getItem('ccr.' + k); window.localStorage.setItem('ccr.' + k, v); } catch (e) { /* private mode */ }
    return null;
  }
  var S = {
    lang: store('lang') || ((navigator.language || 'en').slice(0, 2) === 'it' ? 'it' : 'en'),
    theme: store('theme') || 'auto',
    tab: 'overview',
    journal: null, // {name, res, roll, sum, daily, heat}
    anchorDoc: null, anchorResult: null, compare: null,
    f: { asset: '', status: '', reason: '', q: '', from: '', to: '' },
    sort: { key: 'line', dir: -1 },
    page: 0,
    drawer: null,
    busy: null,
    live: { sources: [], timer: null, every: 2000, inflight: false, capture: null, idx: 0, playing: false, speed: 10, playTimer: null, mode: 'poll' },
    embedded: null
  };
  var PAGE = 50;
  var disposers = [];
  var main, tabsEl, tip, toastTimer;

  var t = function (k) { return I.t(S.lang, k); };

  // ---------------------------------------------------------------- format
  function groupInt(str) {
    var neg = str.charAt(0) === '-';
    var body = neg ? str.slice(1) : str;
    var parts = body.split('.');
    parts[0] = C.groupInt(parts[0]);
    return (neg ? '-' : '') + parts.join('.');
  }
  function money(micro, min) { return groupInt(C.trimFixed(BigInt(micro), 6, min === undefined ? 2 : min)); }
  function kwh(uwh) { return groupInt(C.formatFixed(BigInt(uwh) / 1000000n, 3)); }
  function kw(mw) { return groupInt(C.formatFixed(BigInt(mw) / 1000n, 3)); }
  function intg(n) { return groupInt(String(n)); }
  function pct1(num, den) {
    num = BigInt(num); den = BigInt(den);
    if (den === 0n) return '–';
    return C.formatFixed((num * 1000n) / den, 1) + ' %';
  }
  function stamp(s2) { return s2 ? s2.replace('T', ' ').replace('Z', '') : '–'; }
  function shortHash(x) { return x ? x.slice(0, 10) + '…' + x.slice(-6) : ''; }
  function fixedPpm(ppm) {
    if (ppm === undefined || ppm === null || ppm < 0) return '–';
    return C.formatFixed(BigInt(Math.floor(ppm / 100)), 2) + ' %';
  }
  var DAYS = { en: ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'], it: ['Lun', 'Mar', 'Mer', 'Gio', 'Ven', 'Sab', 'Dom'] };

  // ------------------------------------------------------------------ misc ui
  function toast(msg) {
    var old = document.querySelector('.toast');
    if (old) old.remove();
    var el = h('div', { class: 'toast', role: 'status' }, msg);
    document.body.appendChild(el);
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.remove(); }, 2600);
  }
  function copy(text) {
    var done = function () { toast(t('copied')); };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done, fallback);
    else fallback();
    function fallback() {
      var ta = h('textarea', { style: 'position:fixed;opacity:0' }, text);
      document.body.appendChild(ta); ta.select();
      try { document.execCommand('copy'); done(); } catch (e) { toast('Copy failed'); }
      ta.remove();
    }
  }
  function download(name, data, mime) {
    var blob = new Blob([data], { type: mime || 'application/octet-stream' });
    var url = URL.createObjectURL(blob);
    var a = h('a', { href: url, download: name });
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
  }
  function codeBlock(text, copyLabel) {
    return h('div', null,
      h('pre', { class: 'code' }, text),
      h('button', { class: 'btn sm', onclick: function () { copy(text); } }, icon('copy'), copyLabel || t('copy')));
  }

  // tooltip: one floating node, driven by data-tip on any mark
  function setupTip() {
    tip = h('div', { id: 'tip', role: 'tooltip' });
    document.body.appendChild(tip);
    var cur = null;
    function place(e) {
      var x = e.clientX + 14, y = e.clientY + 14;
      var w = tip.offsetWidth, hh = tip.offsetHeight;
      if (x + w > window.innerWidth - 8) x = e.clientX - w - 14;
      if (y + hh > window.innerHeight - 8) y = e.clientY - hh - 14;
      tip.style.left = Math.max(4, x) + 'px'; tip.style.top = Math.max(4, y) + 'px';
    }
    document.addEventListener('pointerover', function (e) {
      var n = e.target.closest ? e.target.closest('[data-tip]') : null;
      if (!n) { tip.classList.remove('on'); cur = null; return; }
      if (n !== cur) { cur = n; tip.textContent = n.getAttribute('data-tip'); }
      tip.classList.add('on'); place(e);
    });
    document.addEventListener('pointermove', function (e) { if (cur) place(e); });
    document.addEventListener('pointerleave', function () { tip.classList.remove('on'); cur = null; }, true);
  }

  // ----------------------------------------------------------- chart cards
  function simpleTable(headers, rows) {
    return h('div', { class: 'tablewrap' }, h('table', null,
      h('thead', null, h('tr', null, headers.map(function (x, i) { return h('th', { class: i ? 'r' : '' }, x); }))),
      h('tbody', null, rows.map(function (r) { return h('tr', null, r.map(function (c, i) { return h('td', { class: i ? 'r num' : '' }, c); })); }))));
  }
  function chartCard(o) {
    var mode = 'chart', lastW = 0;
    var body = h('div', { class: 'chart' });
    var btn = h('button', { class: 'btn sm swap', 'aria-pressed': 'false', onclick: function () {
      mode = mode === 'chart' ? 'table' : 'chart';
      btn.setAttribute('aria-pressed', mode === 'table' ? 'true' : 'false');
      btn.textContent = mode === 'chart' ? t('table_view') : t('chart_view');
      draw();
    } }, t('table_view'));
    function draw() {
      body.replaceChildren(mode === 'chart' ? o.build(Math.floor(body.clientWidth) || 560) : o.table());
      lastW = body.clientWidth;
    }
    var ro = new ResizeObserver(function () { if (mode === 'chart' && Math.abs(body.clientWidth - lastW) > 3) draw(); });
    ro.observe(body);
    disposers.push(function () { ro.disconnect(); });
    var card = h('section', { class: 'card' },
      h('header', null, h('h3', null, o.title), o.sub ? h('span', { class: 'sub' }, o.sub) : null, btn),
      o.legend ? h('div', { class: 'legend' }, o.legend) : null, body);
    requestAnimationFrame(draw);
    return card;
  }
  var legendItem = function (cls, label) { return h('span', null, h('i', { class: cls }), label); };

  // ----------------------------------------------------------- data loading
  function sniff(bytes) {
    var head = new TextDecoder().decode(bytes.subarray(0, 400));
    if (head.charAt(0) === '{') {
      if (head.indexOf('charger.anchor.v1') >= 0) return 'anchor';
      if (head.indexOf('charger.snapshot.v1') >= 0) return 'capture';
      return 'json';
    }
    return 'journal';
  }

  async function openJournal(name, bytes, note) {
    S.busy = { label: t('verifying'), p: 0 };
    renderBusy();
    try {
      var res = await C.parseJournal(bytes, { onProgress: function (p) { S.busy.p = p; renderBusy(); } });
      var roll = C.rollup(res.sessions);
      S.journal = { name: name, res: res, roll: roll, sum: C.settlementSummary(res.sessions), note: note || '',
        daily: C.dailyBuckets(res.sessions), heat: C.weekHourHeat(res.sessions) };
      S.anchorResult = null; S.compare = null; S.page = 0; S.drawer = null;
      S.f = { asset: '', status: '', reason: '', q: '', from: '', to: '' };
      if (S.anchorDoc) S.anchorResult = checkAnchor(S.anchorDoc);
    } finally {
      S.busy = null; renderBusy();
    }
    buildChrome();
    render();
  }

  function checkAnchor(doc) {
    if (!doc.ok) return { ok: false, error: doc.error };
    return C.verifyAnchor(S.journal.res, doc);
  }

  async function openFiles(files) {
    for (var i = 0; i < files.length; i++) {
      var f = files[i];
      var bytes = new Uint8Array(await f.arrayBuffer());
      var kind = sniff(bytes);
      if (kind === 'anchor') {
        S.anchorDoc = C.parseAnchor(new TextDecoder().decode(bytes));
        S.anchorDoc.name = f.name;
        if (S.journal) S.anchorResult = checkAnchor(S.anchorDoc);
        S.tab = 'integrity';
        render();
      } else if (kind === 'capture') {
        loadCapture(f.name, new TextDecoder().decode(bytes));
      } else if (kind === 'json') {
        toast(t('unknown_json'));
      } else {
        await openJournal(f.name, bytes);
        if (S.tab === 'live') { S.tab = 'overview'; render(); }
      }
    }
  }

  function b64ToBytes(b64) {
    var bin = atob(b64), out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  function bytesToB64(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i += 0x8000) out += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(out);
  }
  function demoData() {
    var el = document.getElementById('demo-data');
    if (!el || !el.textContent.trim()) return null;
    try { return JSON.parse(el.textContent); } catch (e) { return null; }
  }
  async function loadDemo(tampered) {
    var d = demoData();
    if (!d) { toast(t('no_demo')); return; }
    var bytes = b64ToBytes(d.journal);
    if (tampered) bytes = tamperInMemory(bytes);
    S.anchorDoc = null;
    if (!tampered && d.anchor) { S.anchorDoc = C.parseAnchor(d.anchor); S.anchorDoc.name = 'demo.anchor'; }
    await openJournal(tampered ? 'demo-altered.journal' : 'demo.journal', bytes, tampered ? 'tampered' : 'demo');
    S.tab = tampered ? 'integrity' : 'overview';
    render();
  }
  // Flips the last digit of the energy field of the 41st line, in memory only.
  function tamperInMemory(src) {
    var b = new Uint8Array(src);
    var pos = 0;
    for (var ln = 1; ln < 41; ln++) { var nl = b.indexOf(10, pos); if (nl < 0) return b; pos = nl + 1; }
    var t1 = b.indexOf(9, pos), t2 = b.indexOf(9, t1 + 1), t3 = b.indexOf(9, t2 + 1), t4 = b.indexOf(9, t3 + 1);
    var t5 = b.indexOf(9, t4 + 1), t6 = b.indexOf(9, t5 + 1);
    var p = t6 - 1; // last digit of energy_uwh (field 5)
    b[p] = b[p] >= 0x30 && b[p] < 0x39 ? b[p] + 1 : 0x30;
    return b;
  }
  function loadCapture(name, text) {
    var frames = C.parseNdjson(text);
    if (!frames.length) { toast(t('capture_empty')); return; }
    S.live.capture = { name: name, frames: frames };
    S.live.idx = 0; S.live.mode = 'capture'; stopPlay();
    S.tab = 'live';
    render();
  }

  // ------------------------------------------------------------- chrome
  function applyTheme() {
    var el = document.documentElement;
    if (S.theme === 'auto') el.removeAttribute('data-theme'); else el.setAttribute('data-theme', S.theme);
  }
  function brandMark() {
    return s('svg', { width: 32, height: 32, viewBox: '0 0 32 32', 'aria-hidden': 'true' },
      s('rect', { width: 32, height: 32, rx: 8, class: 'f-s1' }),
      s('path', { d: 'M17.5 5.5L8.5 18h6.2l-1.4 8.5 9.2-13h-6.2l1.2-8z', fill: '#fff' }));
  }
  var TABS = ['overview', 'fleet', 'sessions', 'integrity', 'live'];
  function buildChrome() {
    var app = document.getElementById('app');
    app.replaceChildren();
    var langSeg = h('div', { class: 'seg', role: 'group', 'aria-label': 'Language' },
      ['en', 'it'].map(function (l) {
        return h('button', { 'aria-pressed': String(S.lang === l), onclick: function () { S.lang = l; store('lang', l); document.documentElement.lang = l; buildChrome(); render(); } }, l.toUpperCase());
      }));
    var themeBtn = h('button', { class: 'btn', title: t('theme'), 'aria-label': t('theme'), onclick: function () {
      S.theme = S.theme === 'auto' ? 'dark' : S.theme === 'dark' ? 'light' : 'auto';
      store('theme', S.theme); applyTheme(); buildChrome(); render();
    } }, icon('sun'), h('span', { class: 'lbl' }, t('theme_' + S.theme)));
    var input = h('input', { type: 'file', id: 'file', class: 'hide', multiple: true, onchange: function (e) { openFiles(e.target.files); e.target.value = ''; } });
    var top = h('header', { class: 'top' }, h('div', { class: 'wrap' },
      h('div', { class: 'top-row' },
        h('div', { class: 'brand' }, brandMark(), h('div', null, h('h1', null, t('title')), h('small', null, t('subtitle')))),
        h('div', { class: 'spacer' }),
        h('button', { class: 'btn primary', onclick: function () { input.click(); } }, icon('file'), t('open_journal')),
        h('button', { class: 'btn', onclick: function () { loadDemo(false); } }, t('demo')),
        S.journal ? h('button', { class: 'btn no-print', onclick: saveReport, title: t('report_hint'), 'aria-label': t('save_report') }, icon('download'), h('span', { class: 'lbl' }, t('save_report'))) : null,
        S.journal ? h('button', { class: 'btn no-print', 'aria-label': t('print'), onclick: function () { window.print(); } }, icon('print'), h('span', { class: 'lbl' }, t('print'))) : null,
        langSeg, themeBtn, input),
      (tabsEl = h('nav', { class: 'tabs', role: 'tablist' }, TABS.map(function (k) {
        return h('button', { role: 'tab', id: 'tab-' + k, 'aria-selected': String(S.tab === k), onclick: function () { go(k); } },
          t('tab_' + k),
          k === 'sessions' && S.journal ? h('span', { class: 'badge' }, intg(S.journal.res.sessions.length)) : null,
          k === 'integrity' && S.journal ? h('span', { class: 'badge', title: S.journal.res.chain.intact ? '' : t('chain_broken') }, S.journal.res.chain.intact ? '✓' : '✕') : null);
      })))));
    main = h('main', { class: 'wrap', id: 'view', role: 'tabpanel', tabindex: '-1' });
    var foot = h('footer', { class: 'bottom' }, h('div', { class: 'wrap' },
      h('p', null, t('foot_readonly')),
      h('p', null, t('foot_math')),
      h('p', null, t('foot_limits'))));
    var busy = h('div', { id: 'busy', class: 'wrap hide' });
    app.appendChild(top); app.appendChild(busy); app.appendChild(main); app.appendChild(foot);
    document.title = t('title');
  }
  function renderBusy() {
    var el = document.getElementById('busy');
    if (!el) return;
    if (!S.busy) { el.classList.add('hide'); el.replaceChildren(); return; }
    el.classList.remove('hide');
    el.replaceChildren(h('div', { style: 'padding:8px 0' }, h('div', { class: 'note' }, S.busy.label + ' ' + Math.round(S.busy.p * 100) + ' %'),
      h('div', { class: 'progress' }, h('div', { style: 'width:' + Math.round(S.busy.p * 100) + '%' }))));
  }
  function go(tab) {
    S.tab = tab; S.drawer = null;
    if (location.hash !== '#' + tab) { try { history.replaceState(null, '', '#' + tab); } catch (e) { /* sandboxed frame */ } }
    render();
  }

  function render() {
    disposers.splice(0).forEach(function (d) { d(); });
    stopPoll();
    Array.prototype.forEach.call(tabsEl.children, function (b, i) { b.setAttribute('aria-selected', String(TABS[i] === S.tab)); });
    var view;
    if (S.tab === 'live') view = viewLive();
    else if (!S.journal) view = viewEmpty();
    else view = { overview: viewOverview, fleet: viewFleet, sessions: viewSessions, integrity: viewIntegrity }[S.tab]();
    main.replaceChildren(view);
    if (S.drawer) openDrawer(S.drawer, true);
  }

  // ------------------------------------------------------------- empty state
  function viewEmpty() {
    var input = document.getElementById('file');
    return h('div', null,
      h('div', { class: 'drop', id: 'dropzone' },
        h('h2', null, t('drop_title')),
        h('p', null, t('drop_body')),
        h('div', { class: 'actions' },
          h('button', { class: 'btn primary', onclick: function () { input.click(); } }, icon('file'), t('open_journal')),
          h('button', { class: 'btn', onclick: function () { loadDemo(false); } }, t('demo')),
          h('button', { class: 'btn', onclick: function () { loadDemo(true); } }, icon('warn'), t('demo_altered')),
          h('button', { class: 'btn', onclick: function () { go('live'); } }, icon('bolt'), t('tab_live')))),
      h('div', { class: 'promises' },
        [['promise1_t', 'promise1_b'], ['promise2_t', 'promise2_b'], ['promise3_t', 'promise3_b'], ['promise4_t', 'promise4_b']].map(function (p) {
          return h('div', null, h('b', null, t(p[0])), h('span', null, t(p[1])));
        })),
      h('p', { class: 'note', style: 'margin-top:16px' }, t('empty_hint')));
  }

  // ------------------------------------------------------------- shared bits
  function chainBanner(compact) {
    var res = S.journal.res, ch = res.chain;
    if (ch.intact) {
      return h('div', { class: 'banner ok', role: 'status' }, icon('check', 'ok'),
        h('div', null,
          h('h2', null, t('chain_ok')),
          h('p', null, t('chain_ok_body').replace('{n}', intg(res.lines)).replace('{b}', intg(res.bytes))),
          compact ? null : h('p', { class: 'hash mono' }, 'sha256 ' + res.fileSha256),
          h('p', { class: 'note' }, t('chain_scope'))));
    }
    return h('div', { class: 'banner bad', role: 'alert' }, icon('cross', 'bad'),
      h('div', null,
        h('h2', null, t('chain_bad').replace('{n}', intg(ch.firstBrokenLine))),
        h('p', null, t(ch.truncated ? 'chain_bad_trunc' : 'chain_bad_body')),
        h('p', { class: 'note' }, t('chain_prefix').replace('{n}', intg(Math.max(0, ch.firstBrokenLine - 1))).replace('{s}', intg(res.sessions.length)))));
  }
  function statusPill(sess) {
    if (sess.billable) return h('span', { class: 'pill ok' }, icon('check'), t('billable'));
    return h('span', { class: 'pill held' }, icon('warn'), t('held') + ' · ' + reasonLabel(C.heldReason(sess)));
  }
  function reasonLabel(code) {
    var k = 'reason_' + code;
    var v = I.t(S.lang, k);
    return v === k ? code : v;
  }
  function stopLabel(code) {
    if (!code) return '–';
    var k = 'stop_' + code;
    var v = I.t(S.lang, k);
    return v === k ? code : v;
  }

  // ------------------------------------------------------------- overview
  function viewOverview() {
    var J = S.journal, R = J.roll, sum = J.sum, res = J.res;
    var firstLast = periodOf(res.sessions);
    var heldShare = pct1(R.sessionsHeld, R.sessionsClosed);
    var tiles = [
      tile(t('k_energy'), kwh(R.energyUwh), 'kWh', t('k_energy_d').replace('{n}', kwh(R.heldEnergyUwh))),
      tile(t('k_sessions'), intg(R.sessionsClosed), '', pct1(R.sessionsBillable, R.sessionsClosed) + ' ' + t('billable_lc')),
      tile(t('k_mean_power'), R.meanPowerMw < 0n ? '–' : kw(R.meanPowerMw), 'kW', t('k_mean_power_d')),
      tile(t('k_rev_kwh'), R.revenuePerKwhMicro < 0n ? '–' : money(R.revenuePerKwhMicro), '€/kWh', t('k_rev_kwh_d')),
      tile(t('k_faults'), intg(R.faultStops), '', t('k_faults_d').replace('{n}', intg(R.timeoutStops)))
    ];
    var hero = h('section', { class: 'card hero' },
      h('div', { class: 'k' }, t('hero_k')),
      h('div', { class: 'v num' }, money(sum.invoiceTotal), h('small', null, res.sessions.length ? (res.sessions[0].currency || 'EUR') + ' ' + t('excl_vat') : '')),
      h('div', { class: 'd' }, t('hero_d').replace('{n}', intg(R.sessionsBillable)).replace('{kwh}', kwh(sum.billableEnergyWh * 1000000n)).replace('{a}', intg(R.assets))),
      h('div', { class: 'd' }, firstLast ? stamp(firstLast[0]) + ' → ' + stamp(firstLast[1]) + ' UTC' : ''),
      qualityBar(R),
      h('div', { style: 'display:flex;gap:28px;flex-wrap:wrap;margin-top:16px' },
        h('div', null, h('div', { class: 'k' }, t('k_held')), h('div', { class: 'num', style: 'font-size:22px;font-weight:600' }, money(R.heldValueMicro), ' ', h('small', { class: 'muted', style: 'font-size:12px;font-weight:400' }, '€')),
          h('div', { class: 'note' }, intg(R.sessionsHeld) + ' ' + t('sessions_lc') + ' · ' + heldShare)),
        h('div', null, h('div', { class: 'k' }, t('k_orphans')), h('div', { class: 'num', style: 'font-size:22px;font-weight:600' }, intg(res.orphans.length)),
          h('div', { class: 'note' }, t('k_orphans_d')))));

    return h('div', null, chainBanner(),
      h('div', { class: 'grid', style: 'grid-template-columns:repeat(auto-fit,minmax(min(100%,340px),1fr));margin-bottom:16px' }, hero,
        h('div', { class: 'tiles', style: 'align-content:start' }, tiles)),
      h('div', { class: 'grid cols-2', style: 'align-items:start' }, dailyCard(), bayCard()),
      h('div', { class: 'grid cols-2', style: 'margin-top:16px;align-items:start' }, heatCard(), attentionCard()),
      journalMeta());
  }
  // Share of closed sessions that may be invoiced vs held: counts, not money.
  function qualityBar(R) {
    var total = R.sessionsClosed || 1;
    var b = Math.round((R.sessionsBillable / total) * 1000) / 10, hh = Math.round((R.sessionsHeld / total) * 1000) / 10;
    var seg = function (cls, pct, tipText) {
      var el = h('span', { class: cls, style: 'display:block;height:10px;width:' + pct + '%' });
      el.setAttribute('data-tip', tipText); return el;
    };
    return h('div', { style: 'margin-top:18px' },
      h('div', { style: 'display:flex;gap:2px;border-radius:5px;overflow:hidden', role: 'img', 'aria-label': t('quality_aria') },
        seg('sw-s1', b, t('billable') + ': ' + R.sessionsBillable), seg('sw-s2', hh, t('held') + ': ' + R.sessionsHeld)),
      h('div', { class: 'legend', style: 'margin:6px 0 0' }, legendItem('sw-s1', t('billable') + ' ' + intg(R.sessionsBillable)), legendItem('sw-s2', t('held') + ' ' + intg(R.sessionsHeld))));
  }
  function tile(k, v, unit, d, flag) {
    return h('div', { class: 'tile' }, h('div', { class: 'k' }, k),
      h('div', { class: 'v num' }, v, unit ? h('small', null, unit) : null), d ? h('div', { class: 'd' }, d) : null);
  }
  function periodOf(sessions) {
    var first = null, last = null;
    sessions.forEach(function (x) { if (!x.closedAt) return; if (first === null || x.closedAt < first) first = x.closedAt; if (last === null || x.closedAt > last) last = x.closedAt; });
    return first ? [first, last] : null;
  }
  function journalMeta() {
    var J = S.journal;
    return h('p', { class: 'note', style: 'margin-top:16px' }, J.name + ' · ' + intg(J.res.bytes) + ' B · ' + intg(J.res.rawLines) + ' ' + t('lines_lc') + ' · ' + t('local_note'));
  }

  function dailyCard() {
    var J = S.journal;
    return chartCard({
      title: t('c_daily'), sub: t('c_daily_sub'),
      legend: [legendItem('sw-s1', t('billable')), legendItem('sw-s2', t('held'))],
      build: function (w) {
        var data = J.daily.map(function (d) {
          var b = Number(d.billableUwh) / 1e9, hh = Number(d.heldUwh) / 1e9;
          return { label: d.day.slice(5), parts: [{ v: b, cls: 'f-s1' }, { v: hh, cls: 'f-s2' }],
            tip: d.day + '\n' + t('billable') + ': ' + kwh(d.billableUwh) + ' kWh (' + d.billable + ')\n' + t('held') + ': ' + kwh(d.heldUwh) + ' kWh (' + d.held + ')' };
        });
        return X.stackedColumns({ w: w, h: 230, data: data, aria: t('c_daily'), yFmt: X.compact,
          topLabel: function (v) { return X.compact(v) + ' kWh'; } });
      },
      table: function () {
        return simpleTable([t('day'), t('billable') + ' kWh', t('held') + ' kWh', t('billable_lc'), t('held_lc')],
          J.daily.map(function (d) { return [d.day, kwh(d.billableUwh), kwh(d.heldUwh), d.billable, d.held]; }));
      }
    });
  }
  function bayCard() {
    var J = S.journal, per = J.roll.perAsset;
    var rows = per.slice(0, 14);
    return chartCard({
      title: t('c_bay'), sub: t('c_bay_sub') + (per.length > rows.length ? ' · ' + t('top_n').replace('{n}', rows.length) : ''),
      legend: [legendItem('sw-s1', t('billable')), legendItem('sw-s2', t('held'))],
      build: function (w) {
        return X.hbars({ w: w, aria: t('c_bay'), rows: rows.map(function (a) {
          var tot = a.billableValueMicro + a.heldValueMicro;
          return { label: a.assetId, parts: [{ v: Number(a.billableValueMicro) / 1e6, cls: 'f-s1' }, { v: Number(a.heldValueMicro) / 1e6, cls: 'f-s2' }],
            value: money(tot, 3), tip: a.assetId + '\n' + t('billable') + ': ' + money(a.billableValueMicro) + ' (' + a.sessionsBillable + ')\n' + t('held') + ': ' + money(a.heldValueMicro) + ' (' + (a.sessionsClosed - a.sessionsBillable) + ')\n' + kwh(a.energyUwh) + ' kWh' };
        }) });
      },
      table: function () {
        return simpleTable([t('asset'), t('billable') + ' €', t('held') + ' €', 'kWh', t('sessions')],
          per.map(function (a) { return [a.assetId, money(a.billableValueMicro), money(a.heldValueMicro), kwh(a.energyUwh), a.sessionsClosed]; }));
      }
    });
  }
  function heatCard() {
    var J = S.journal;
    return chartCard({
      title: t('c_heat'), sub: t('c_heat_sub'),
      legend: [h('span', null, h('i', { class: 'sw-q0' }), t('less')), h('span', null, h('i', { class: 'sw-q6' }), t('more'))],
      build: function (w) {
        var grid = J.heat.map(function (row) { return row.map(function (v) { return Number(v) / 1e9; }); });
        return X.heat({ w: w, aria: t('c_heat'), grid: grid, rowLabels: DAYS[S.lang],
          tip: function (r, c, v) { return DAYS[S.lang][r] + ' ' + (c < 10 ? '0' : '') + c + ':00–' + (c + 1 < 10 ? '0' : '') + (c + 1) + ':00 UTC\n≈ ' + X.compact(v) + ' kWh ' + t('estimated'); } });
      },
      table: function () {
        var prof = new Array(24).fill(0);
        J.heat.forEach(function (row) { row.forEach(function (v, c) { prof[c] += Number(v) / 1e9; }); });
        return simpleTable([t('hour_utc'), 'kWh ' + t('estimated')], prof.map(function (v, c) { return [(c < 10 ? '0' : '') + c + ':00', v.toFixed(3)]; }));
      }
    });
  }
  function attentionCard() {
    var J = S.journal, res = J.res;
    var held = res.sessions.filter(function (x) { return !x.billable; });
    held.sort(function (a, b) { return a.totalCost === b.totalCost ? 0 : a.totalCost > b.totalCost ? -1 : 1; });
    var reasons = new Map();
    held.forEach(function (x) { var k = C.heldReason(x); var r = reasons.get(k) || { n: 0, v: 0n }; r.n++; r.v += BigInt.asUintN(64, x.totalCost); reasons.set(k, r); });
    var rs = Array.from(reasons.entries()).sort(function (a, b) { return b[1].n - a[1].n; });
    var maxN = rs.length ? rs[0][1].n : 1;
    return h('section', { class: 'card' },
      h('header', null, h('h3', null, t('c_attention')), h('span', { class: 'sub' }, t('c_attention_sub'))),
      rs.length ? h('div', null, rs.map(function (r) {
        return h('div', { style: 'display:grid;grid-template-columns:minmax(140px,210px) 1fr auto;gap:10px;align-items:center;margin:4px 0' },
          h('span', null, reasonLabel(r[0])),
          h('span', { class: 'bar-wrap' }, h('span', { class: 'bar', style: 'display:block;height:8px;border-radius:0 4px 4px 0;background:var(--s2);width:' + Math.max(2, Math.round((r[1].n / maxN) * 100)) + '%' })),
          h('span', { class: 'num muted' }, r[1].n + ' · ' + money(r[1].v, 3)));
      })) : h('p', { class: 'empty' }, icon('check', 'ok'), ' ' + t('nothing_held')),
      held.length ? h('div', { style: 'margin-top:12px' }, h('div', { class: 'note' }, t('largest_held')),
        h('div', { class: 'tablewrap', style: 'margin-top:6px' }, h('table', null, h('tbody', null, held.slice(0, 5).map(function (x) {
          return h('tr', { class: 'click', tabindex: '0', onclick: function () { openDrawer(x); }, onkeydown: function (e) { if (e.key === 'Enter') openDrawer(x); } },
            h('td', null, x.asset + ' #' + x.id), h('td', null, reasonLabel(C.heldReason(x))), h('td', { class: 'r num' }, money(x.totalCost, 3)));
        }))))) : null,
      res.orphans.length ? h('div', { style: 'margin-top:12px' }, h('div', { class: 'note' }, t('orphans_note').replace('{n}', res.orphans.length)),
        h('div', { class: 'chips', style: 'margin-top:6px' }, res.orphans.slice(0, 8).map(function (o) { return h('span', { class: 'pill held' }, icon('warn'), o.asset + ' #' + o.id); }))) : null);
  }

  // ------------------------------------------------------------- fleet
  function viewFleet() {
    var J = S.journal, R = J.roll;
    var days = J.daily.map(function (d) { return d.day; });
    var perDay = new Map();
    J.res.sessions.forEach(function (x) {
      var c = C.parseUtc(x.closedAt); if (c === null) return;
      var key = x.asset + '|' + C.dayKey(c);
      perDay.set(key, (perDay.get(key) || 0n) + x.energyUwh);
    });
    var maxRev = R.perAsset.reduce(function (m, a) { return a.revenueMicro > m ? a.revenueMicro : m; }, 1n);
    var head = [['asset', 'asset'], ['sessions', 'sessions'], ['billable', 'billable'], ['held', 'held'], ['held_share', 'held_share'], ['energy', 'energy'], ['revenue', 'revenue'], ['charging', 'charging_time'],
      ['mean_power', 'mean_power'], ['faults', 'fault_stops'], ['timeouts', 'timeout_stops'], ['under', 'undersampled'], ['trend', 'daily_trend']];
    var rows = R.perAsset.map(function (a) {
      var held = a.sessionsClosed - a.sessionsBillable;
      var mean = a.chargingMs > 0n ? kw((a.energyUwh * 3600n) / a.chargingMs) : '–';
      var series = days.map(function (d) { return Number(perDay.get(a.assetId + '|' + d) || 0n) / 1e9; });
      return h('tr', { class: 'click', tabindex: '0', onclick: function () { S.f.asset = a.assetId; S.page = 0; go('sessions'); },
        onkeydown: function (e) { if (e.key === 'Enter') { S.f.asset = a.assetId; S.page = 0; go('sessions'); } } },
      h('td', null, a.assetId), h('td', { class: 'r num' }, intg(a.sessionsClosed)), h('td', { class: 'r num' }, intg(a.sessionsBillable)),
      h('td', { class: 'r num' }, intg(held)), h('td', { class: 'r num' }, pct1(held, a.sessionsClosed)),
      h('td', { class: 'r num' }, kwh(a.energyUwh) + ' kWh'),
      h('td', { class: 'r num' }, money(a.revenueMicro, 3), ' ', h('span', { class: 'bar', style: 'width:' + Math.max(2, Number((a.revenueMicro * 60n) / maxRev)) + 'px' })),
      h('td', { class: 'r num' }, C.fmtDuration(a.chargingMs)), h('td', { class: 'r num' }, mean + (mean === '–' ? '' : ' kW')),
      h('td', { class: 'r num' }, intg(a.faultStops)), h('td', { class: 'r num' }, intg(a.timeoutStops)), h('td', { class: 'r num' }, intg(a.undersampled)),
      h('td', null, series.length > 1 ? sparkCell(series) : '–'));
    });
    return h('div', null, chainBanner(true),
      h('div', { class: 'card', style: 'margin-bottom:12px' }, h('p', { class: 'note', style: 'margin:0' }, t('fleet_note'))),
      h('div', { class: 'tablewrap' }, h('table', null,
        h('thead', null, h('tr', null, head.map(function (c, i) { return h('th', { class: i && i < 12 ? 'r' : '' }, t('col_' + c[1])); }))),
        h('tbody', null, rows))),
      journalMeta());
  }
  function sparkCell(vals) {
    var w = 110, hh = 24;
    var wrap = h('span', { style: 'display:inline-block;width:' + w + 'px' });
    var sp = X.spark(vals, w, hh);
    sp.setAttribute('style', 'width:' + w + 'px;height:' + hh + 'px');
    wrap.appendChild(sp);
    return wrap;
  }

  // ------------------------------------------------------------- sessions
  var COLS = [
    ['asset', function (x) { return x.asset; }, 'str'],
    ['id', function (x) { return x.id; }, 'big'],
    ['closed', function (x) { return x.closedAt; }, 'str'],
    ['status', function (x) { return x.billable ? 0 : 1; }, 'num'],
    ['energy', function (x) { return x.energyUwh; }, 'big'],
    ['duration', function (x) { return x.durationMs; }, 'big'],
    ['stop', function (x) { return x.stopReason; }, 'str'],
    ['total', function (x) { return x.totalCost; }, 'big'],
    ['line', function (x) { return x.line; }, 'num']
  ];
  function filteredSessions() {
    var f = S.f, q = f.q.trim().toLowerCase();
    var out = S.journal.res.sessions.filter(function (x) {
      if (f.asset && x.asset !== f.asset) return false;
      if (f.status === 'billable' && !x.billable) return false;
      if (f.status === 'held' && x.billable) return false;
      if (f.reason && C.heldReason(x) !== f.reason) return false;
      if (f.from && x.closedAt.slice(0, 10) < f.from) return false;
      if (f.to && x.closedAt.slice(0, 10) > f.to) return false;
      if (q && (x.asset + ' ' + x.id + ' ' + x.stopReason + ' ' + x.quality + ' ' + x.closedAt).toLowerCase().indexOf(q) < 0) return false;
      return true;
    });
    var col = COLS.filter(function (c) { return c[0] === S.sort.key; })[0] || COLS[8];
    var dir = S.sort.dir;
    out.sort(function (a, b) {
      var x = col[1](a), y = col[1](b);
      var r = x < y ? -1 : x > y ? 1 : 0;
      return r * dir || (a.line - b.line) * dir;
    });
    return out;
  }
  function viewSessions() {
    var J = S.journal, list = filteredSessions();
    var pages = Math.max(1, Math.ceil(list.length / PAGE));
    if (S.page >= pages) S.page = pages - 1;
    var slice = list.slice(S.page * PAGE, S.page * PAGE + PAGE);
    var assets = J.roll.perAsset.map(function (a) { return a.assetId; }).sort();
    var reasons = Array.from(new Set(J.res.sessions.map(C.heldReason).filter(Boolean))).sort();
    function rerender() { S.page = 0; render(); }
    var q = h('input', { type: 'search', placeholder: t('search'), value: S.f.q, 'aria-label': t('search'),
      oninput: function (e) { S.f.q = e.target.value; S.page = 0; clearTimeout(q._t); q._t = setTimeout(function () { var pos = e.target.selectionStart; render(); var n = document.querySelector('input[type=search]'); if (n) { n.focus(); n.setSelectionRange(pos, pos); } }, 250); } });
    var sel = function (key, opts, all, label) {
      return h('select', { 'aria-label': label, onchange: function (e) { S.f[key] = e.target.value; rerender(); } },
        h('option', { value: '' }, all), opts.map(function (o) { return h('option', Object.assign({ value: o[0] }, S.f[key] === o[0] ? { selected: true } : {}), o[1]); }));
    };
    var filters = h('div', { class: 'filters' }, q,
      sel('asset', assets.map(function (a) { return [a, a]; }), t('all_assets'), t('asset')),
      sel('status', [['billable', t('billable')], ['held', t('held')]], t('all_status'), t('status')),
      sel('reason', reasons.map(function (r) { return [r, reasonLabel(r)]; }), t('all_reasons'), t('reason')),
      h('input', { type: 'date', value: S.f.from, 'aria-label': t('from'), onchange: function (e) { S.f.from = e.target.value; rerender(); } }),
      h('input', { type: 'date', value: S.f.to, 'aria-label': t('to'), onchange: function (e) { S.f.to = e.target.value; rerender(); } }),
      h('button', { class: 'btn sm', onclick: function () { S.f = { asset: '', status: '', reason: '', q: '', from: '', to: '' }; rerender(); } }, t('clear')),
      h('span', { class: 'spacer' }),
      h('button', { class: 'btn sm', onclick: function () { download(baseName() + '-settlement.csv', C.settlementCsv(list), 'text/csv'); } }, icon('download'), t('export_csv') + ' (' + intg(list.length) + ')'));
    var head = h('tr', null, COLS.map(function (c, i) {
      var active = S.sort.key === c[0];
      return h('th', { class: (c[2] !== 'str' && c[0] !== 'status') ? 'r' : '', 'aria-sort': active ? (S.sort.dir > 0 ? 'ascending' : 'descending') : 'none' },
        h('button', { onclick: function () { if (active) S.sort.dir *= -1; else { S.sort.key = c[0]; S.sort.dir = c[2] === 'str' ? 1 : -1; } S.page = 0; render(); } },
          t('col_' + c[0]), active ? (S.sort.dir > 0 ? ' ▲' : ' ▼') : ''));
    }));
    var rows = slice.map(function (x) {
      return h('tr', { class: 'click', tabindex: '0', onclick: function () { openDrawer(x); }, onkeydown: function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openDrawer(x); } } },
        h('td', null, x.asset), h('td', { class: 'r num' }, String(x.id)), h('td', { class: 'num', title: x.closedAt }, stamp(x.closedAt).slice(0, 16)),
        h('td', null, statusPill(x)), h('td', { class: 'r num' }, kwh(x.energyUwh)), h('td', { class: 'r num' }, C.fmtDuration(x.durationMs)),
        h('td', null, stopLabel(x.stopReason)), h('td', { class: 'r num' }, money(x.totalCost, 3)), h('td', { class: 'r num muted' }, String(x.line)));
    });
    var pager = h('div', { class: 'pager' },
      h('span', null, intg(list.length) + ' ' + t('sessions_lc') + ' · ' + t('page') + ' ' + (S.page + 1) + '/' + pages),
      h('button', { class: 'btn sm', disabled: S.page === 0, onclick: function () { S.page--; render(); } }, '‹'),
      h('button', { class: 'btn sm', disabled: S.page >= pages - 1, onclick: function () { S.page++; render(); } }, '›'));
    return h('div', null, filters,
      list.length ? h('div', { class: 'tablewrap' }, h('table', null, h('thead', null, head), h('tbody', null, rows))) : h('div', { class: 'card empty' }, t('no_match')),
      pager, h('p', { class: 'note' }, t('csv_note')));
  }
  function baseName() { return (S.journal ? S.journal.name : 'journal').replace(/\.[^.]+$/, ''); }

  // ------------------------------------------------------------- drawer
  function openDrawer(x, silent) {
    S.drawer = x;
    closeDrawerNode();
    var J = S.journal, res = J.res;
    var rc = C.recompute(x);
    var tariff = rc.available ? rc.tariff : null;
    var kv = function (k, v) { return [h('dt', null, k), h('dd', { class: 'num' }, v)]; };
    var eurLine = function (name, qty, formula, cost) {
      return h('tr', null, h('td', null, name), h('td', { class: 'num muted' }, formula), h('td', { class: 'r num' }, money(cost, 6)));
    };
    var costs = h('div', { class: 'tablewrap' }, h('table', null, h('tbody', null,
      eurLine('ENERGY', '', tariff ? intg(x.billedEnergyWh) + ' Wh × ' + C.trimFixed(tariff.rate, 6, 2) + ' €/kWh' : '', x.energyCost),
      eurLine('TIME', '', tariff ? intg(x.billedTimeS) + ' s × ' + C.trimFixed(tariff.trate, 6, 2) + ' €/h' : '', x.timeCost),
      eurLine('FLAT', '', tariff ? t('per_session') : '', x.flatCost),
      h('tr', null, h('td', null, h('b', null, t('total'))), h('td', { class: 'muted' }, rc.clamped ? t('clamped') : t('excl_vat')), h('td', { class: 'r num' }, h('b', null, money(x.totalCost, 6)))))));
    var checks = rc.available ? h('div', { class: 'chk' }, [
      h('span', { class: 'h' }, t('field')), h('span', { class: 'h' }, t('in_journal')), h('span', { class: 'h' }, t('recomputed')), h('span'),
      rc.checks.map(function (c) {
        return [h('span', null, c.name), h('span', { class: 'num mono' }, c.kind === 'eur' ? money(c.journal, 6) : c.journal),
          h('span', { class: 'num mono' }, c.kind === 'eur' ? money(c.recomputed, 6) : c.recomputed), c.ok ? icon('check', 'ok') : icon('cross', 'bad')];
      })]) : h('p', { class: 'note' }, rc.note);
    var recipe = C.auditorRecipe(res, x, J.name || 'bay.journal');
    var raw = C.rawLineOf(res, x);
    var node = h('aside', { class: 'drawer', role: 'dialog', 'aria-modal': 'true', 'aria-label': x.asset + ' #' + x.id, id: 'drawer' },
      h('button', { class: 'btn sm close', 'aria-label': t('close'), onclick: closeDrawer }, icon('cross')),
      h('h2', null, x.asset + ' #' + x.id), h('div', { style: 'margin-top:6px;display:flex;gap:8px;align-items:center;flex-wrap:wrap' }, statusPill(x),
        h('button', { class: 'btn sm no-print', onclick: function () { printEvidence(); } }, icon('print'), t('print_evidence'))),
      h('p', { class: 'note' }, J.name + ' · sha256 ' + res.fileSha256),
      !x.billable ? h('p', { class: 'note' }, t('why_' + C.heldReason(x))) : h('p', { class: 'note' }, t('why_billable')),
      h('section', null, h('h3', null, t('d_meter')), h('dl', { class: 'kv' },
        kv(t('closed_at'), x.closedAt + ' UTC'), kv(t('col_stop'), stopLabel(x.stopReason) + (x.stopReason ? ' (' + x.stopReason + ')' : '')),
        kv('energy_uwh', intg(x.energyUwh) + ' µWh'), kv('energy_wh', intg(x.energyWh) + ' Wh'), kv('billed_energy_wh', intg(x.billedEnergyWh) + ' Wh'),
        kv('duration_ms', intg(x.durationMs) + ' ms (' + C.fmtDuration(x.durationMs) + ')'), kv('charging_s / billed_time_s', intg(x.chargingS) + ' / ' + intg(x.billedTimeS) + ' s'),
        kv('samples / gaps', x.samples + ' / ' + x.gaps), kv('quality', x.quality), kv('energy_lower_bound', x.energyLower ? t('yes') : t('no')),
        kv('time_upper_bound', x.timeUpper ? t('yes') : t('no')), kv('undersampled', x.undersampled ? t('yes') : t('no')))),
      h('section', null, h('h3', null, t('d_cost')), costs),
      h('section', null, h('h3', null, t('d_recompute')), h('p', { class: 'note' }, rc.available ? (rc.ok ? t('recompute_ok') : t('recompute_bad')) : ''), checks,
        rc.available ? h('p', { class: 'note' }, t('recompute_scope')) : null),
      h('section', null, h('h3', null, t('d_chain')), h('dl', { class: 'kv' },
        kv(t('open_line'), x.openLine ? String(x.openLine) : '–'), kv(t('close_line'), String(x.line)),
        kv('prev_hash', x.prevHash), kv('close_hash', x.hash)),
      h('p', { class: 'note' }, t('d_chain_note'))),
      h('section', null, h('h3', null, t('d_verify')), h('p', { class: 'note' }, t('d_verify_note')), codeBlock(recipe),
        h('div', { class: 'note' }, t('d_cli')), codeBlock('charger_settle ' + (J.name || 'bay.journal') + ' --dispute ' + x.asset + ' ' + x.id)),
      h('section', null, h('h3', null, t('d_raw')), h('pre', { class: 'code wrap-all' }, raw)));
    var scrim = h('div', { class: 'scrim', id: 'scrim', onclick: closeDrawer });
    document.body.appendChild(scrim); document.body.appendChild(node);
    if (!silent) node.querySelector('.close').focus();
  }
  // Prints only the open evidence drawer: a one-session dispute sheet that names the file digest.
  function printEvidence() {
    document.body.classList.add('print-drawer');
    var done = function () { document.body.classList.remove('print-drawer'); window.removeEventListener('afterprint', done); };
    window.addEventListener('afterprint', done);
    window.print();
  }
  function closeDrawerNode() {
    var a = document.getElementById('drawer'), b = document.getElementById('scrim');
    if (a) a.remove(); if (b) b.remove();
  }
  function closeDrawer() { S.drawer = null; closeDrawerNode(); }

  // ------------------------------------------------------------- integrity
  function viewIntegrity() {
    var J = S.journal, res = J.res, ch = res.chain;
    var strip = h('div', { class: 'strip', role: 'img', 'aria-label': t('strip_aria') });
    var cells = Math.min(600, Math.max(1, res.rawLines));
    var per = res.rawLines / cells;
    for (var i = 0; i < cells; i++) {
      var from = Math.floor(i * per) + 1, to = Math.floor((i + 1) * per);
      var cls = '';
      if (!ch.intact) { if (ch.firstBrokenLine >= from && ch.firstBrokenLine <= Math.max(from, to)) cls = 'bad'; else if (from > ch.firstBrokenLine) cls = 'unk'; }
      var cell = h('i', { class: cls });
      cell.setAttribute('data-tip', t('lines_lc') + ' ' + from + (to > from ? '–' + to : '') + (cls === 'bad' ? '\n' + t('first_bad') : cls === 'unk' ? '\n' + t('unverified') : '\n' + t('verified')));
      strip.appendChild(cell);
    }
    var facts = h('dl', { class: 'kv' },
      h('dt', null, t('file')), h('dd', null, J.name), h('dt', null, t('size')), h('dd', { class: 'num' }, intg(res.bytes) + ' B'),
      h('dt', null, 'sha256 (' + t('whole_file') + ')'), h('dd', { class: 'mono' }, res.fileSha256),
      h('dt', null, t('lines')), h('dd', { class: 'num' }, intg(res.lines) + ' ' + t('chained') + ' / ' + intg(res.rawLines) + ' ' + t('raw')),
      h('dt', null, 'genesis'), h('dd', { class: 'mono' }, C.GENESIS),
      h('dt', null, 'last_hash'), h('dd', { class: 'mono' }, ch.lastHash),
      h('dt', null, t('sessions')), h('dd', { class: 'num' }, intg(res.closes) + ' ' + t('closed_lc') + ', ' + intg(res.opens) + ' ' + t('opened_lc') + ', ' + res.orphans.length + ' ' + t('orphans_lc')));
    var forensic = !ch.intact && ch.forensics ? h('section', { class: 'card', style: 'margin-top:16px' },
      h('header', null, h('h3', null, t('forensics'))),
      h('p', null, ch.reason + ' — ' + t('line') + ' ' + intg(ch.firstBrokenLine) + '.'),
      h('p', { class: 'note' }, ch.forensics.note),
      h('p', { class: 'note' }, t('forensics_tail').replace('{ok}', intg(ch.forensics.selfConsistentAfter)).replace('{n}', intg(ch.forensics.checkedAfter))),
      h('p', { class: 'note' }, t('forensics_scope'))) : null;
    var orphans = h('section', { class: 'card', style: 'margin-top:16px' },
      h('header', null, h('h3', null, t('orphans')), h('span', { class: 'sub' }, t('orphans_sub'))),
      res.orphans.length ? h('div', { class: 'tablewrap' }, h('table', null,
        h('thead', null, h('tr', null, [t('asset'), t('col_id'), t('open_line'), t('opened_at')].map(function (x) { return h('th', null, x); }))),
        h('tbody', null, res.orphans.map(function (o) { return h('tr', null, h('td', null, o.asset), h('td', { class: 'num' }, String(o.id)), h('td', { class: 'num' }, String(o.line)), h('td', { class: 'num' }, stamp(o.at))); }))))
        : h('p', { class: 'empty' }, icon('check', 'ok'), ' ' + t('no_orphans')));
    return h('div', null, chainBanner(true),
      h('section', { class: 'card' }, h('header', null, h('h3', null, t('i_chain')), h('span', { class: 'sub' }, t('i_chain_sub'))), strip,
        h('div', { class: 'legend', style: 'margin-top:8px' }, h('span', null, h('i', { class: 'sw-s1', style: 'background:var(--good)' }), t('verified')),
          !ch.intact ? [h('span', null, h('i', { style: 'background:var(--crit)' }), t('first_bad')), h('span', null, h('i', { style: 'background:var(--axis)' }), t('unverified'))] : null),
        h('div', { style: 'margin-top:12px' }, facts)),
      forensic, orphans, anchorCard(), compareCard(), proofCard());
  }
  function anchorCard() {
    var J = S.journal, res = J.res;
    var input = h('input', { type: 'file', class: 'hide', onchange: function (e) { openFiles(e.target.files); e.target.value = ''; } });
    var result = null;
    if (S.anchorResult) {
      var r = S.anchorResult;
      result = r.ok
        ? h('div', { class: 'banner ok', role: 'status', style: 'margin:12px 0 0' }, icon('check', 'ok'), h('div', null, h('h2', null, t('anchor_ok')),
          h('p', null, t('anchor_ok_body').replace('{n}', intg(S.anchorDoc.lines)).replace('{a}', intg(r.appended))),
          h('p', { class: 'note' }, r.wholeFileSame ? t('anchor_same') : t('anchor_grown'))))
        : h('div', { class: 'banner bad', role: 'alert', style: 'margin:12px 0 0' }, icon('cross', 'bad'), h('div', null, h('h2', null, t('anchor_bad')), h('p', null, r.error)));
    }
    var doc = res.chain.intact ? C.makeAnchor(res, C.formatUtc(Math.floor(Date.now() / 1000))) : '';
    return h('section', { class: 'card', style: 'margin-top:16px' },
      h('header', null, h('h3', null, t('anchor')), h('span', { class: 'sub' }, t('anchor_sub'))),
      h('p', null, t('anchor_body')),
      h('div', { class: 'filters' },
        h('button', { class: 'btn', disabled: !res.chain.intact, onclick: function () { download(baseName() + '.anchor', doc, 'application/json'); } }, icon('download'), t('anchor_create')),
        h('button', { class: 'btn', onclick: function () { input.click(); } }, icon('file'), t('anchor_verify')), input,
        S.anchorDoc ? h('span', { class: 'note' }, S.anchorDoc.name || '') : null),
      result, res.chain.intact ? h('details', { style: 'margin-top:8px' }, h('summary', null, t('anchor_preview')), codeBlock(doc.trim())) : null,
      h('p', { class: 'note' }, t('anchor_store')));
  }
  // Is an earlier copy of this journal an exact prefix of it? Works with no anchor at all:
  // two copies taken at different times are each other's witness.
  function compareCard() {
    var input = h('input', { type: 'file', id: 'compare-file', class: 'hide', onchange: async function (e) {
      var f = e.target.files[0]; e.target.value = '';
      if (!f) return;
      var bytes = new Uint8Array(await f.arrayBuffer());
      var earlier = await C.parseJournal(bytes);
      S.compare = { name: f.name, result: C.comparePrefix(S.journal.res, earlier), sha: earlier.fileSha256 };
      render();
    } });
    var r = S.compare && S.compare.result, out = null;
    if (r) {
      if (r.relation === 'prefix' || r.relation === 'identical') {
        out = h('div', { class: 'banner ok', role: 'status', style: 'margin:12px 0 0' }, icon('check', 'ok'), h('div', null,
          h('h2', null, t(r.relation === 'identical' ? 'cmp_identical' : 'cmp_prefix')),
          h('p', null, t(r.relation === 'identical' ? 'cmp_identical_body' : 'cmp_prefix_body').replace('{e}', intg(r.earlierLines || r.lines)).replace('{a}', intg(r.appended || 0))),
          h('p', { class: 'note mono' }, S.compare.name + ' · sha256 ' + S.compare.sha)));
      } else if (r.relation === 'diverged') {
        out = h('div', { class: 'banner bad', role: 'alert', style: 'margin:12px 0 0' }, icon('cross', 'bad'), h('div', null,
          h('h2', null, t('cmp_diverged')),
          h('p', null, r.shorter ? t('cmp_shorter') : t('cmp_diverged_body').replace('{n}', intg(r.firstDifferentLine)).replace('{k}', intg(r.agreeLines))),
          h('p', { class: 'note' }, t('cmp_diverged_note'))));
      } else {
        out = h('div', { class: 'banner bad', role: 'alert', style: 'margin:12px 0 0' }, icon('cross', 'bad'), h('div', null,
          h('h2', null, t('cmp_broken')), h('p', null, t(r.error === 'earlier' ? 'cmp_broken_earlier' : 'cmp_broken_current').replace('{n}', intg(r.line)))));
      }
    }
    return h('section', { class: 'card', style: 'margin-top:16px' },
      h('header', null, h('h3', null, t('cmp_title')), h('span', { class: 'sub' }, t('cmp_sub'))),
      h('p', null, t('cmp_body')),
      h('div', { class: 'filters' }, h('button', { class: 'btn', disabled: !S.journal.res.chain.intact, onclick: function () { input.click(); } }, icon('file'), t('cmp_pick')), input),
      out);
  }
  function proofCard() {
    return h('section', { class: 'card', style: 'margin-top:16px' },
      h('header', null, h('h3', null, t('proof'))),
      h('div', { class: 'two' },
        h('div', null, h('b', null, t('proves')), h('ul', null, ['p1', 'p2', 'p3'].map(function (k) { return h('li', null, t('proof_' + k)); }))),
        h('div', null, h('b', null, t('not_proves')), h('ul', null, ['n1', 'n2', 'n3'].map(function (k) { return h('li', null, t('proof_' + k)); })))),
      h('div', { class: 'note', style: 'margin-top:8px' }, t('verify_alone')),
      codeBlock("sha256sum " + (S.journal.name || 'bay.journal') + "\nsed -n '1p' " + (S.journal.name || 'bay.journal') + " | cut -f1,3- | tr -d '\\n' | sha256sum\nsed -n '1p' " + (S.journal.name || 'bay.journal') + ' | cut -f2'));
  }

  // ------------------------------------------------------------- live
  var STATE_UI = {
    CHARGING: { ico: 'bolt', dot: 'dot-s1' }, READY: { ico: 'ready', dot: 'dot-s3' }, OFF: { ico: 'off', dot: 'dot-muted' },
    FAULT: { ico: 'warn', dot: 'dot-crit' }, DISCONNECTED: { ico: 'plug', dot: 'dot-serious' }, UNKNOWN: { ico: 'unknown', dot: 'dot-muted' }
  };
  var STATE_FILL = { CHARGING: 'f-s1', READY: 'f-s3', OFF: 'f-muted', FAULT: 'f-crit', DISCONNECTED: 'f-serious', UNKNOWN: 'f-surface2' };
  var STALE_FILE_S = 10; // display threshold only: the monitor refreshes at least every report_interval

  function savedSources() {
    var v = store('sources');
    if (v === null || v === undefined) return 'status/bay-01.json';
    return v;
  }
  function viewLive() {
    var L = S.live;
    var capture = L.mode === 'capture' && L.capture;
    var input = h('input', { type: 'file', class: 'hide', onchange: function (e) { openFiles(e.target.files); e.target.value = ''; } });
    var ta = h('textarea', { rows: 3, id: 'sources', spellcheck: 'false', style: 'width:100%;font-family:ui-monospace,monospace;padding:6px 8px;border-radius:8px;border:1px solid var(--grid);background:var(--surface)',
      'aria-label': t('live_sources'), placeholder: 'status/bay-01.json\nstatus/bay-02.json' }, savedSources());
    var every = h('select', { 'aria-label': t('live_every'), onchange: function (e) { L.every = +e.target.value; if (L.timer) { stopPoll(); startPoll(); } } },
      [1000, 2000, 5000, 10000].map(function (ms) { return h('option', Object.assign({ value: ms }, L.every === ms ? { selected: true } : {}), ms / 1000 + ' s'); }));
    var grid = h('div', { class: 'grid cols-2', id: 'live-grid' });
    var summary = h('div', { class: 'chips', id: 'live-summary', style: 'margin-bottom:12px' });
    var ctl = h('section', { class: 'card', style: 'margin-bottom:16px' },
      h('header', null, h('h3', null, t('live_title')), h('span', { class: 'sub' }, t('live_sub'))),
      h('p', { class: 'note' }, t('live_howto')),
      ta,
      h('div', { class: 'filters', style: 'margin-top:8px' },
        h('button', { class: 'btn primary', id: 'poll-btn', onclick: function () { if (L.timer) { stopPoll(); } else { L.mode = 'poll'; L.sources = ta.value.split(/\r?\n/).map(function (x) { return x.trim(); }).filter(Boolean).map(function (u) { return { url: u, snaps: [], latest: null, err: '', at: 0 }; }); store('sources', ta.value); startPoll(); } renderLive(); } },
          icon(L.timer ? 'pause' : 'play'), L.timer ? t('live_stop') : t('live_start')),
        h('label', { class: 'note' }, t('live_every') + ' ', every),
        h('span', { class: 'spacer' }),
        h('button', { class: 'btn', onclick: function () { input.click(); } }, icon('file'), t('capture_load')), input,
        demoData() && demoData().capture ? h('button', { class: 'btn', onclick: function () { loadCapture('demo-capture.ndjson', demoData().capture); } }, t('capture_demo')) : null),
      location.protocol === 'file:' ? h('p', { class: 'note' }, icon('info'), ' ' + t('live_file_note')) : null);
    var root = h('div', null, ctl, summary, grid);
    L._grid = grid; L._summary = summary;
    if (capture) { renderCapture(root); } else { renderLive(); if (L.sources.length && !L.timer) startPoll(); }
    return root;
  }
  function renderLive() {
    var L = S.live, grid = L._grid, summary = L._summary;
    if (!grid || L.mode === 'capture') return;
    var btn = document.getElementById('poll-btn');
    if (btn) { btn.replaceChildren(icon(L.timer ? 'pause' : 'play'), L.timer ? t('live_stop') : t('live_start')); }
    var counts = {};
    var cards = L.sources.map(function (src) { if (src.latest) { var st = src.latest.data.state; counts[st] = (counts[st] || 0) + 1; } return liveCard(src); });
    grid.replaceChildren.apply(grid, cards.length ? cards : [h('div', { class: 'card empty' }, t('live_none'))]);
    summary.replaceChildren.apply(summary, Object.keys(counts).map(function (k) {
      var u = STATE_UI[k] || STATE_UI.UNKNOWN;
      return h('span', { class: 'state' }, h('span', { class: 'dot ' + u.dot }), icon(u.ico), stateLabel(k) + ' · ' + counts[k]);
    }));
  }
  function stateLabel(k) { var v = I.t(S.lang, 'state_' + k); return v === 'state_' + k ? k : v; }
  var DIAG = [['measurement_stuck', 'diag_stuck'], ['voltage_rate_exceeded', 'diag_vrate'], ['current_rate_exceeded', 'diag_irate'], ['voltage_sag', 'diag_sag'], ['current_hunting', 'diag_hunt']];

  function liveCard(src, frameOverride) {
    var snap = frameOverride || src.latest;
    if (!snap) {
      return h('section', { class: 'card live-card' }, h('div', { class: 'live-head' }, h('h3', { class: 'mono' }, src.url)),
        src.err ? h('div', { class: 'banner bad', role: 'alert' }, icon('cross', 'bad'), h('div', null, h('b', null, t('live_err')), h('p', null, src.err))) : h('p', { class: 'note' }, t('live_wait')));
    }
    var d = snap.data, u = STATE_UI[d.state] || STATE_UI.UNKNOWN;
    var fileAge = null;
    if (!frameOverride && snap.timestamp) {
      var ts = C.parseUtc(snap.timestamp);
      if (ts !== null) fileAge = Math.max(0, Math.round(Date.now() / 1000 - ts));
    }
    var stale = fileAge !== null && fileAge > STALE_FILE_S;
    var chips = [];
    DIAG.forEach(function (x) { if (d.diagnostics && d.diagnostics[x[0]]) chips.push(h('span', { class: 'pill held' }, icon('warn'), t(x[1]))); });
    if (d.measurements_stale) chips.push(h('span', { class: 'pill held' }, icon('warn'), t('meas_stale')));
    if (d.status_stale) chips.push(h('span', { class: 'pill held' }, icon('warn'), t('status_stale')));
    if (!chips.length) chips.push(h('span', { class: 'pill ok' }, icon('check'), t('no_advisories')));
    var sess = d.session;
    var series = (src.snaps || []).map(function (x) { return x.data.power_mw / 1e6; });
    var card = h('section', { class: 'card live-card' + (d.state === 'FAULT' ? ' fault' : '') },
      h('div', { class: 'live-head' },
        h('h3', null, d.asset_id || src.url),
        h('span', { class: 'state' }, h('span', { class: 'dot ' + u.dot }), icon(u.ico), stateLabel(d.state)),
        h('span', { class: 'spacer' }),
        frameOverride ? h('span', { class: 'note num' }, stamp(snap.timestamp) + ' UTC')
          : stale ? h('span', { class: 'pill bad' }, icon('cross', 'bad'), t('file_stale').replace('{s}', fileAge))
          : fileAge !== null ? h('span', { class: 'pill ok' }, icon('check'), t('file_fresh').replace('{s}', fileAge)) : null),
      d.fault_code ? h('div', { class: 'banner bad', role: 'alert', style: 'margin:0' }, icon('warn', 'bad'), h('div', null, h('b', null, d.fault_code), h('p', null, d.fault_message), d.fault_action ? h('p', { class: 'note' }, t('action_reported') + ' ' + d.fault_action) : null)) : null,
      h('div', { class: 'metrics' },
        h('div', null, h('div', { class: 'k' }, t('voltage')), h('div', { class: 'v num' }, C.formatFixed(BigInt(d.voltage_mv) / 10n, 2), h('small', null, ' V'))),
        h('div', null, h('div', { class: 'k' }, t('current')), h('div', { class: 'v num' }, C.formatFixed(BigInt(d.current_ma) / 10n, 2), h('small', null, ' A'))),
        h('div', null, h('div', { class: 'k' }, t('power')), h('div', { class: 'v num' }, kw(d.power_mw), h('small', null, ' kW')))),
      series.length > 1 ? X.spark(series, 320, 56) : null,
      h('div', { class: 'chips' }, chips),
      sess ? h('div', { class: 'note' }, h('b', null, t('session_open') + ' #' + sess.id), ' · ' + kwh(BigInt(sess.energy_uwh)) + ' kWh · ' + C.fmtDuration(sess.duration_ms) + ' · ' + sess.quality) : null,
      h('dl', { class: 'kv', style: 'grid-template-columns:1fr 1fr 1fr 1fr' },
        [[t('avail'), fixedPpm(d.availability_ppm)], [t('util'), fixedPpm(d.utilization_ppm)], [t('frames_ok'), intg(d.accepted_measurement_frames + d.accepted_status_frames)], [t('frames_rej'), intg(d.rejected_frames)],
          [t('sessions'), intg(d.sessions_closed) + ' / ' + intg(d.sessions_billable) + ' ' + t('billable_lc')], [t('timeouts'), intg(d.timeout_events)], [t('meas_age'), d.measurement_age_ms < 0 ? '–' : d.measurement_age_ms + ' ms'], ['v', d.version || '']].map(function (r) {
          return [h('dt', null, r[0]), h('dd', { class: 'num' }, r[1])];
        })));
    if (src.err && !frameOverride) card.insertBefore(h('div', { class: 'banner warn', role: 'status', style: 'margin:0' }, icon('warn', 'warn'), h('div', null, t('live_err') + ': ' + src.err)), card.children[1]);
    return card;
  }

  async function pollOnce() {
    var L = S.live;
    if (L.inflight) return;
    L.inflight = true;
    await Promise.all(L.sources.map(async function (src) {
      try {
        var r = await fetch(src.url, { cache: 'no-store' });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        var text = await r.text();
        var snap = C.parseSnapshotText(text.trim().split('\n').pop());
        if (!snap) throw new Error(t('not_snapshot'));
        src.latest = snap; src.err = ''; src.at = Date.now();
        src.snaps.push(snap); if (src.snaps.length > 120) src.snaps.shift();
      } catch (e) { src.err = String(e && e.message ? e.message : e); }
    }));
    L.inflight = false;
    if (S.tab === 'live') renderLive();
  }
  function startPoll() {
    var L = S.live;
    if (L.timer || !L.sources.length) return;
    pollOnce();
    L.timer = setInterval(pollOnce, L.every);
  }
  function stopPoll() { var L = S.live; if (L.timer) { clearInterval(L.timer); L.timer = null; } }
  function stopPlay() { var L = S.live; L.playing = false; if (L.playTimer) { clearInterval(L.playTimer); L.playTimer = null; } }

  function renderCapture(root) {
    var L = S.live, cap = L.capture, frames = cap.frames;
    var cardHost = h('div', { id: 'cap-card' });
    var chartHost = h('div', { class: 'chart' });
    var label = h('span', { class: 'num note' });
    var slider = h('input', { type: 'range', min: 0, max: frames.length - 1, value: L.idx, style: 'flex:1;min-width:160px', 'aria-label': t('capture_pos'),
      oninput: function (e) { L.idx = +e.target.value; draw(); } });
    function draw() {
      L.idx = Math.max(0, Math.min(frames.length - 1, L.idx));
      slider.value = L.idx;
      label.textContent = (L.idx + 1) + ' / ' + frames.length + ' · ' + stamp(frames[L.idx].timestamp) + ' UTC';
      var src = { url: cap.name, snaps: frames.slice(Math.max(0, L.idx - 119), L.idx + 1) };
      cardHost.replaceChildren(liveCard(src, frames[L.idx]));
      drawChart();
    }
    function drawChart() {
      var w = Math.floor(chartHost.clientWidth) || 640;
      var xs = frames.map(function (f) { return C.parseUtc(f.timestamp) || 0; });
      var span = xs[xs.length - 1] - xs[0];
      var xl = [0, 0.25, 0.5, 0.75, 1].map(function (a) { return { at: a, text: C.formatUtc(xs[0] + Math.round(span * a)).slice(5, 16).replace('T', ' ') }; });
      chartHost.replaceChildren(X.timeline({ w: w, h: 150, ys: frames.map(function (f) { return f.data.power_mw / 1e6; }), states: frames.map(function (f) { return f.data.state; }),
        stateCls: function (st) { return STATE_FILL[st] || 'f-surface2'; }, marker: L.idx, xLabels: xl, aria: t('capture_chart') }));
    }
    var ro = new ResizeObserver(function () { drawChart(); });
    ro.observe(chartHost); disposers.push(function () { ro.disconnect(); stopPlay(); });
    var playBtn = h('button', { class: 'btn', onclick: function () {
      if (L.playing) { stopPlay(); } else {
        L.playing = true;
        L.playTimer = setInterval(function () { if (L.idx >= frames.length - 1) { stopPlay(); playBtn.replaceChildren(icon('play'), t('live_start')); return; } L.idx = Math.min(frames.length - 1, L.idx + 1); draw(); }, Math.max(30, 1000 / L.speed));
      }
      playBtn.replaceChildren(icon(L.playing ? 'pause' : 'play'), L.playing ? t('live_stop') : t('live_start'));
    } }, icon(L.playing ? 'pause' : 'play'), L.playing ? t('live_stop') : t('live_start'));
    var speed = h('select', { 'aria-label': t('speed'), onchange: function (e) { L.speed = +e.target.value; if (L.playing) { stopPlay(); playBtn.click(); } } },
      [1, 5, 10, 30, 100].map(function (v) { return h('option', Object.assign({ value: v }, L.speed === v ? { selected: true } : {}), v + '×'); }));
    root.appendChild(h('section', { class: 'card', style: 'margin-bottom:16px' },
      h('header', null, h('h3', null, t('capture_title')), h('span', { class: 'sub' }, cap.name), h('button', { class: 'btn sm swap', onclick: function () { L.mode = 'poll'; stopPlay(); render(); } }, t('capture_close'))),
      h('div', { class: 'legend' }, ['CHARGING', 'READY', 'OFF', 'DISCONNECTED', 'FAULT'].map(function (k) {
        return h('span', null, h('i', { class: 'dot-' + (STATE_UI[k].dot.slice(4)), style: 'border-radius:2px' }), stateLabel(k)); }),
      h('span', null, t('capture_unit'))),
      chartHost,
      h('div', { class: 'filters', style: 'margin-top:8px' }, playBtn, speed, slider, label),
      h('p', { class: 'note' }, t('capture_note'))));
    root.appendChild(cardHost);
    requestAnimationFrame(draw);
  }

  // ------------------------------------------------------------- report
  function saveReport() {
    if (!S.journal) return;
    var bytes = S.journal.res._bytes;
    if (bytes.length > 24 * 1024 * 1024) { toast(t('report_big')); return; }
    var doc = document.documentElement.cloneNode(true);
    var app = doc.querySelector('#app'); while (app && app.firstChild) app.removeChild(app.firstChild);
    ['#tip', '.toast', '#drawer', '#scrim'].forEach(function (sel) { var n = doc.querySelector(sel); if (n) n.remove(); });
    var emb = doc.querySelector('#embedded');
    emb.textContent = JSON.stringify({ name: S.journal.name, savedAt: C.formatUtc(Math.floor(Date.now() / 1000)), b64: bytesToB64(bytes) });
    download(baseName() + '-report.html', '<!doctype html>\n' + doc.outerHTML, 'text/html');
  }

  // ------------------------------------------------------------- boot
  function boot() {
    document.documentElement.lang = S.lang;
    applyTheme();
    setupTip();
    var hash = (location.hash || '').slice(1);
    if (TABS.indexOf(hash) >= 0) S.tab = hash;
    buildChrome();
    main.replaceChildren(h('p', { class: 'empty' }, '…'));
    // drag & drop anywhere
    var dragDepth = 0;
    document.addEventListener('dragenter', function (e) { e.preventDefault(); dragDepth++; var z = document.getElementById('dropzone'); if (z) z.classList.add('over'); });
    document.addEventListener('dragleave', function () { dragDepth = Math.max(0, dragDepth - 1); var z = document.getElementById('dropzone'); if (z && !dragDepth) z.classList.remove('over'); });
    document.addEventListener('dragover', function (e) { e.preventDefault(); });
    document.addEventListener('drop', function (e) { e.preventDefault(); dragDepth = 0; if (e.dataTransfer && e.dataTransfer.files.length) openFiles(e.dataTransfer.files); });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && S.drawer) closeDrawer(); });
    window.addEventListener('hashchange', function () { var hh = location.hash.slice(1); if (TABS.indexOf(hh) >= 0 && hh !== S.tab) { S.tab = hh; render(); } });
    window.addEventListener('beforeprint', function () { /* charts already have fixed widths */ });
    // header actions that need the loaded journal
    var q = new URLSearchParams(location.search);
    var emb = document.getElementById('embedded');
    var embedded = null;
    if (emb && emb.textContent.trim()) { try { embedded = JSON.parse(emb.textContent); } catch (e) { embedded = null; } }
    var chain = Promise.resolve();
    if (embedded && embedded.b64) {
      chain = chain.then(function () { return openJournal(embedded.name || 'embedded.journal', b64ToBytes(embedded.b64), 'embedded:' + (embedded.savedAt || '')); });
    } else if (q.get('journal')) {
      chain = chain.then(function () { return fetch(q.get('journal'), { cache: 'no-store' }).then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.arrayBuffer(); }); })
        .then(function (buf) { return openJournal(q.get('journal').split('/').pop(), new Uint8Array(buf)); })
        .catch(function (e) { toast(String(e.message || e)); });
    }
    if (q.get('live')) { S.live.sources = q.get('live').split(',').map(function (u) { return { url: u, snaps: [], latest: null, err: '', at: 0 }; }); S.tab = 'live'; }
    chain.then(function () { render(); });
    window.ChargerApp = { state: S, openJournal: openJournal, saveReport: saveReport, loadDemo: loadDemo };
  }
  document.addEventListener('DOMContentLoaded', boot);

})();
