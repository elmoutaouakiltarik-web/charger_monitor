/* charger_monitor control room — core
 *
 * Everything the dashboard asserts about a journal is computed here, with no
 * DOM and no dependency, so the same file runs in the browser and under
 * `node --test`. The rules are the C++ library's rules, re-implemented
 * independently on purpose: an auditor who distrusts the binary can check the
 * journal with a second implementation they can read in one sitting.
 *
 *   - Money and energy are BigInt. Nothing in the ledger path touches a
 *     floating point number.
 *   - The hash chain is verified byte for byte (the journal is read as raw
 *     bytes, never round-tripped through a text decoder), exactly like
 *     Ledger::recover(): hash = SHA256(prev_hash TAB <bytes after 2nd tab>).
 *   - Nothing here can command a charger. It only reads files.
 *
 * Verified against charger_settle / charger_monitor --portfolio output in
 * dashboard/tests/core.test.mjs.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.ChargerCore = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var GENESIS = '0'.repeat(64);
  var LEDGER_SCHEMA = 'charger.ledger.v1';
  var ANCHOR_SCHEMA = 'charger.anchor.v1';
  var INT64_MAX = (1n << 63n) - 1n;
  var UINT64_MAX = (1n << 64n) - 1n;
  var UWH_PER_WH = 1000000n;
  var MICRO = 1000000n;

  // ------------------------------------------------------------------ SHA-256
  // FIPS 180-4. Streaming, so a line can be hashed as three byte ranges of the
  // original buffer without ever copying it.
  var K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]);

  function Sha256() {
    this.h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    this.buf = new Uint8Array(64);
    this.n = 0; // bytes currently in buf
    this.total = 0; // bytes hashed (a Number: safe far beyond any journal)
    this.w = new Uint32Array(64);
  }

  Sha256.prototype._block = function (b, o) {
    var w = this.w, h = this.h, i, t1, t2;
    for (i = 0; i < 16; i++, o += 4) {
      w[i] = ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
    }
    for (i = 16; i < 64; i++) {
      var x = w[i - 15], y = w[i - 2];
      var s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      var s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    var a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (i = 0; i < 64; i++) {
      var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      var ch = (e & f) ^ (~e & g);
      t1 = (hh + S1 + ch + K[i] + w[i]) >>> 0;
      var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      var maj = (a & bb) ^ (a & c) ^ (bb & c);
      t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = bb; bb = a; a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + bb) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0; h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
  };

  // update(bytes[, from, to]) hashes bytes[from, to).
  Sha256.prototype.update = function (bytes, from, to) {
    var i = from === undefined ? 0 : from;
    var end = to === undefined ? bytes.length : to;
    this.total += end - i;
    if (this.n > 0) {
      while (i < end && this.n < 64) this.buf[this.n++] = bytes[i++];
      if (this.n === 64) { this._block(this.buf, 0); this.n = 0; }
    }
    while (end - i >= 64) { this._block(bytes, i); i += 64; }
    while (i < end) this.buf[this.n++] = bytes[i++];
    return this;
  };

  Sha256.prototype.hex = function () {
    var bitsHi = Math.floor(this.total / 0x20000000) >>> 0;
    var bitsLo = (this.total << 3) >>> 0;
    var pad = new Uint8Array(this.n < 56 ? 64 - this.n : 128 - this.n);
    pad[0] = 0x80;
    var L = pad.length;
    pad[L - 8] = bitsHi >>> 24; pad[L - 7] = bitsHi >>> 16; pad[L - 6] = bitsHi >>> 8; pad[L - 5] = bitsHi;
    pad[L - 4] = bitsLo >>> 24; pad[L - 3] = bitsLo >>> 16; pad[L - 2] = bitsLo >>> 8; pad[L - 1] = bitsLo;
    var total = this.total;
    this.update(pad);
    this.total = total;
    var out = '';
    for (var i = 0; i < 8; i++) out += ('00000000' + this.h[i].toString(16)).slice(-8);
    return out;
  };

  var enc = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
  var dec = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8') : null;

  function toBytes(x) {
    if (typeof x === 'string') return enc.encode(x);
    if (x instanceof Uint8Array) return x;
    if (x instanceof ArrayBuffer) return new Uint8Array(x);
    throw new TypeError('expected string or bytes');
  }

  function sha256Hex(x) { return new Sha256().update(toBytes(x)).hex(); }

  // ASCII-only slice to string (hash fields). Anything non-ASCII simply fails
  // the comparison, exactly as it does in the C++ reader.
  function ascii(bytes, a, b) {
    var s = '';
    for (var i = a; i < b; i++) s += String.fromCharCode(bytes[i]);
    return s;
  }
  function utf8(bytes, a, b) { return dec.decode(bytes.subarray(a, b)); }

  // ------------------------------------------------------------ exact numbers
  // Mirrors parse_u64 in charger_ledger.cpp: digits only, no sign, no overflow.
  function parseU64(s) {
    if (!s || !/^[0-9]+$/.test(s)) return null;
    var v = BigInt(s);
    return v > UINT64_MAX ? null : v;
  }
  function u64(s) { var v = parseU64(s); return v === null ? 0n : v; }
  function i64(s) { return BigInt.asIntN(64, u64(s)); }

  // 1250000n, 6 -> "1.250000". Exact, sign-aware, no Number anywhere.
  function formatFixed(v, decimals) {
    var neg = v < 0n;
    var a = neg ? -v : v;
    var s = a.toString();
    if (decimals > 0) {
      if (s.length <= decimals) s = '0'.repeat(decimals - s.length + 1) + s;
      s = s.slice(0, s.length - decimals) + '.' + s.slice(s.length - decimals);
    }
    return (neg ? '-' : '') + s;
  }
  // Same value, trailing zeros trimmed down to `min` decimals: still exact.
  function trimFixed(v, decimals, min) {
    var s = formatFixed(v, decimals);
    if (s.indexOf('.') < 0) return s;
    var keep = min === undefined ? 2 : min;
    var dot = s.indexOf('.');
    var end = s.length;
    while (end > dot + 1 + keep && s.charAt(end - 1) === '0') end--;
    return s.slice(0, end);
  }
  function eur(micro, min) { return trimFixed(micro, 6, min); }
  function groupInt(s) { return s.replace(/\B(?=(\d{3})+(?!\d))/g, ' '); }
  // uWh -> kWh with 3 decimals floor (display), exact value in the tooltip.
  function kwhFromUwh(uwh) { return formatFixed(uwh / 1000000n, 3); }

  // ------------------------------------------------------------------- time
  // "YYYY-MM-DDTHH:MM:SSZ" -> epoch seconds, or null (never guess a clock).
  function parseUtc(s) {
    var m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z$/.exec(s || '');
    if (!m) return null;
    var Y = +m[1], M = +m[2], D = +m[3], h = +m[4], mi = +m[5], se = +m[6];
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || se > 60) return null;
    var y = M <= 2 ? Y - 1 : Y;
    var era = Math.floor(y / 400);
    var yoe = y - era * 400;
    var mp = (M + 9) % 12;
    var doy = Math.floor((153 * mp + 2) / 5) + D - 1;
    var doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
    var days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
  }
  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function formatUtc(epochSec) {
    var d = new Date(epochSec * 1000);
    return d.getUTCFullYear() + '-' + pad2(d.getUTCMonth() + 1) + '-' + pad2(d.getUTCDate()) + 'T' +
      pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes()) + ':' + pad2(d.getUTCSeconds()) + 'Z';
  }
  function fmtDuration(ms) {
    var s = Math.floor(Number(ms) / 1000);
    var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
    if (h > 0) return h + 'h ' + pad2(m) + 'm';
    if (m > 0) return m + 'm ' + pad2(sec) + 's';
    return sec + 's';
  }

  // ------------------------------------------------------------ journal read
  var FAULT_STOPS = { POWER_STAGE_FAULT: 1, MEASUREMENT_OUT_OF_RANGE: 1, ENERGY_COUNTER_SATURATED: 1 };

  function defer() { return new Promise(function (r) { setTimeout(r, 0); }); }

  /* parseJournal(bytes, opts) -> Promise<result>
   *
   * Mirrors Ledger::recover(): the first line that does not chain stops the
   * trust boundary; everything before it is the verified prefix. opts:
   *   onProgress(fraction)   called between slices of work
   *   sliceLines             lines per slice before yielding (default 1500)
   */
  async function parseJournal(input, opts) {
    opts = opts || {};
    var bytes = toBytes(input);
    var sliceLines = opts.sliceLines || 1500;
    var res = {
      bytes: bytes.length,
      fileSha256: '',
      rawLines: 0,
      lines: 0, // chained, non-empty lines (RecoveryReport::lines)
      chain: { intact: true, firstBrokenLine: 0, truncated: false, reason: '', lastHash: GENESIS, forensics: null },
      opens: 0,
      closes: 0,
      sessions: [],
      orphans: [],
      lineHashes: [] // [rawLineNo, hash] for each chained line, in order
    };

    var prev = GENESIS;
    var openLines = new Map(); // asset#id -> raw line of the open
    var openSessions = []; // [{asset,id,line,at}] mirrors Ledger::open_sessions_
    var lineno = 0;
    var pos = 0;
    var len = bytes.length;
    var sinceYield = 0;
    var broken = false;

    while (pos < len && !broken) {
      var nl = bytes.indexOf(10, pos);
      var start = pos;
      var end = nl === -1 ? len : nl;
      pos = nl === -1 ? len : nl + 1;
      lineno++;
      if (end > start && bytes[end - 1] === 13) end--;
      if (end === start) continue;

      // Field boundaries by tab, never by decoding.
      var tabs = [];
      for (var p = bytes.indexOf(9, start); p !== -1 && p < end; p = bytes.indexOf(9, p + 1)) tabs.push(p);
      var nparts = tabs.length + 1;
      if (nparts < 6) {
        res.chain.truncated = true;
        res.chain.intact = false;
        res.chain.firstBrokenLine = lineno;
        res.chain.reason = 'truncated or malformed record';
        broken = true;
        break;
      }
      var fieldEnd = function (i) { return i < tabs.length ? tabs[i] : end; };
      var fieldStart = function (i) { return i === 0 ? start : tabs[i - 1] + 1; };

      var h = new Sha256();
      h.update(bytes, start, tabs[0]);
      h.update(TAB);
      h.update(bytes, tabs[1] + 1, end);
      var recomputed = h.hex();
      var linePrev = ascii(bytes, start, tabs[0]);
      var lineHash = ascii(bytes, tabs[0] + 1, tabs[1]);
      if (linePrev !== prev) {
        res.chain.intact = false;
        res.chain.firstBrokenLine = lineno;
        res.chain.reason = 'broken link: prev_hash does not match the previous line';
        res.chain.forensics = classify(bytes, pos, lineno, linePrev, lineHash, recomputed, prev);
        broken = true;
        break;
      }
      if (lineHash !== recomputed) {
        res.chain.intact = false;
        res.chain.firstBrokenLine = lineno;
        res.chain.reason = 'hash mismatch: the record was altered';
        res.chain.forensics = classify(bytes, pos, lineno, linePrev, lineHash, recomputed, prev);
        broken = true;
        break;
      }
      prev = lineHash;
      res.lines++;
      res.lineHashes.push([lineno, lineHash]);

      var f = function (i) { return i < nparts ? utf8(bytes, fieldStart(i), fieldEnd(i)) : ''; };
      var type = f(2);
      var asset = f(3);
      var rec = {
        line: lineno,
        type: type,
        closed: type !== 'session_open',
        asset: asset,
        id: u64(f(4)),
        energyUwh: u64(f(5)),
        durationMs: i64(f(6)),
        totalCost: i64(f(7)),
        billable: nparts > 8 && f(8) === '1',
        stopReason: nparts > 9 && f(9) !== '-' ? f(9) : '',
        undersampled: nparts > 10 && f(10) === '1',
        energyWh: u64(f(11)),
        billedEnergyWh: u64(f(12)),
        chargingS: i64(f(13)),
        billedTimeS: i64(f(14)),
        energyCost: i64(f(15)),
        timeCost: i64(f(16)),
        flatCost: i64(f(17)),
        samples: Number(u64(f(18)) & 0xffffffffn),
        gaps: Number(u64(f(19)) & 0xffffffffn),
        timeUpper: u64(f(20)) === 1n,
        energyLower: u64(f(21)) === 1n,
        quality: nparts > 22 ? f(22) : '',
        currency: nparts > 23 ? f(23) : '',
        closedAt: nparts > 24 ? f(24) : '',
        tariffJson: nparts > 26 ? f(25) : '',
        prevHash: linePrev,
        hash: lineHash
      };

      var key = asset + '#' + rec.id;
      if (!rec.closed) {
        res.opens++;
        var dup = false;
        for (var k = 0; k < openSessions.length; k++) {
          if (openSessions[k].asset === asset && openSessions[k].id === rec.id) { dup = true; break; }
        }
        if (!dup) openSessions.push({ asset: asset, id: rec.id, line: lineno, at: nparts > 24 ? f(24) : '' });
        openLines.set(key, lineno);
      } else {
        res.closes++;
        for (var j = 0; j < openSessions.length; j++) {
          if (openSessions[j].asset === asset && openSessions[j].id === rec.id) { openSessions.splice(j, 1); break; }
        }
        rec.openLine = openLines.has(key) ? openLines.get(key) : 0;
        openLines.delete(key);
        rec.payload = nparts > 26 ? f(26) : nparts > 25 ? f(25) : '';
        rec.rawLine = null; // materialised on demand, see rawLineOf()
        res.sessions.push(rec);
      }

      if (++sinceYield >= sliceLines) {
        sinceYield = 0;
        if (opts.onProgress) opts.onProgress(pos / len);
        await defer();
      }
    }

    res.rawLines = lineno;
    res.orphans = openSessions;
    res.chain.lastHash = prev;
    // Whole-file digest: the first thing an auditor compares.
    var fh = new Sha256();
    for (var o = 0, n = 0; o < len; o += 1 << 20, n++) {
      fh.update(bytes, o, Math.min(len, o + (1 << 20)));
      if ((n & 7) === 7) await defer(); // keep the page responsive on a very large file
    }
    res.fileSha256 = fh.hex();
    res._bytes = bytes; // kept for rawLineOf(); not serialised anywhere
    if (opts.onProgress) opts.onProgress(1);
    return res;
  }

  var TAB = new Uint8Array([9]);

  // What kind of damage is this? Looks past the first bad line to tell an
  // edited record from removed/inserted lines. Forensic hint only: the trust
  // boundary stays at the first bad line, exactly like the C++ reader.
  function classify(bytes, pos, lineno, linePrev, lineHash, recomputed, expectedPrev) {
    var out = { kind: 'unknown', note: '', selfConsistentAfter: 0, checkedAfter: 0 };
    if (linePrev === expectedPrev && lineHash !== recomputed) {
      // Chain link intact, own digest wrong: the content of this line changed
      // (or its hash field was). Check whether the next line still links to the
      // hash that is *recorded* on this line.
      out.kind = 'record-altered';
      out.note = 'prev_hash links correctly but the line content no longer hashes to its recorded hash';
    } else {
      out.kind = 'link-broken';
      out.note = 'prev_hash differs from the previous line: lines were removed, inserted or reordered here';
    }
    // Look ahead (bounded) at how many following lines are internally
    // self-consistent and chain among themselves. Evidence about the shape of
    // the damage, never a licence to trust them.
    var checked = 0, ok = 0, p = pos, expect = lineHash;
    while (p < bytes.length && checked < 5000) {
      var nl = bytes.indexOf(10, p);
      var s = p, e = nl === -1 ? bytes.length : nl;
      p = nl === -1 ? bytes.length : nl + 1;
      if (e > s && bytes[e - 1] === 13) e--;
      if (e === s) continue;
      var t0 = bytes.indexOf(9, s);
      var t1 = t0 === -1 ? -1 : bytes.indexOf(9, t0 + 1);
      if (t0 === -1 || t1 === -1 || t0 >= e || t1 >= e) { checked++; continue; }
      checked++;
      var hh = new Sha256();
      hh.update(bytes, s, t0); hh.update(TAB); hh.update(bytes, t1 + 1, e);
      var own = hh.hex() === ascii(bytes, t0 + 1, t1);
      var links = ascii(bytes, s, t0) === expect;
      if (own && links) ok++;
      expect = ascii(bytes, t0 + 1, t1);
    }
    out.checkedAfter = checked;
    out.selfConsistentAfter = ok;
    return out;
  }

  // The exact bytes of a recorded line, for the dispute recipe. Needs the
  // original buffer (kept on the result).
  function rawLineOf(res, rec) {
    if (rec.rawLine) return rec.rawLine;
    var bytes = res._bytes, ln = 0, pos = 0;
    while (pos < bytes.length) {
      var nl = bytes.indexOf(10, pos);
      var s = pos, e = nl === -1 ? bytes.length : nl;
      pos = nl === -1 ? bytes.length : nl + 1;
      if (++ln === rec.line) {
        if (e > s && bytes[e - 1] === 13) e--;
        rec.rawLine = utf8(bytes, s, e);
        return rec.rawLine;
      }
    }
    return '';
  }

  // ------------------------------------------------------------------ rollup
  // Mirrors Ledger::track + Ledger::portfolio. Sums wrap at 2^64 like the C++
  // unsigned counters (they never will; the parity is deliberate).
  function rollup(sessions) {
    var byAsset = new Map();
    sessions.forEach(function (s) {
      if (!s.asset) return;
      var a = byAsset.get(s.asset);
      if (!a) {
        a = { assetId: s.asset, sessionsClosed: 0, sessionsBillable: 0, energyUwh: 0n, revenueMicro: 0n,
          chargingMs: 0n, faultStops: 0, timeoutStops: 0, undersampled: 0, heldValueMicro: 0n,
          billableValueMicro: 0n, heldEnergyUwh: 0n };
        byAsset.set(s.asset, a);
      }
      a.sessionsClosed++;
      if (s.billable) { a.sessionsBillable++; a.billableValueMicro += BigInt.asUintN(64, s.totalCost); }
      else { a.heldValueMicro += BigInt.asUintN(64, s.totalCost); a.heldEnergyUwh += s.energyUwh; }
      a.energyUwh = BigInt.asUintN(64, a.energyUwh + s.energyUwh);
      a.revenueMicro = BigInt.asUintN(64, a.revenueMicro + BigInt.asUintN(64, s.totalCost));
      a.chargingMs += s.durationMs;
      if (FAULT_STOPS[s.stopReason]) a.faultStops++;
      if (s.stopReason === 'COMMUNICATION_TIMEOUT') a.timeoutStops++;
      if (s.undersampled) a.undersampled++;
    });
    var per = Array.from(byAsset.values());
    per.sort(function (x, y) {
      if (x.revenueMicro !== y.revenueMicro) return x.revenueMicro > y.revenueMicro ? -1 : 1;
      return x.assetId < y.assetId ? -1 : x.assetId > y.assetId ? 1 : 0;
    });
    var t = { assets: per.length, sessionsClosed: 0, sessionsBillable: 0, sessionsHeld: 0, energyUwh: 0n,
      revenueMicro: 0n, chargingMs: 0n, faultStops: 0, timeoutStops: 0, meanPowerMw: -1n,
      revenuePerKwhMicro: -1n, bestAssetId: '', billableValueMicro: 0n, heldValueMicro: 0n, heldEnergyUwh: 0n,
      perAsset: per };
    per.forEach(function (a) {
      t.sessionsClosed += a.sessionsClosed; t.sessionsBillable += a.sessionsBillable;
      t.sessionsHeld += a.sessionsClosed - a.sessionsBillable;
      t.energyUwh += a.energyUwh; t.revenueMicro += a.revenueMicro; t.chargingMs += a.chargingMs;
      t.faultStops += a.faultStops; t.timeoutStops += a.timeoutStops;
      t.billableValueMicro += a.billableValueMicro; t.heldValueMicro += a.heldValueMicro;
      t.heldEnergyUwh += a.heldEnergyUwh;
    });
    if (per.length) t.bestAssetId = per[0].assetId;
    if (t.chargingMs > 0n) t.meanPowerMw = (t.energyUwh * 3600n) / t.chargingMs;
    if (t.energyUwh > 0n) t.revenuePerKwhMicro = (t.revenueMicro * 1000000000n) / t.energyUwh;
    return t;
  }

  // The same object `charger_monitor --portfolio` prints, as parsed JSON with
  // strings for the big integers: used by the parity tests.
  function portfolioJson(t) {
    return {
      schema: 'charger.portfolio.v1',
      assets: t.assets,
      sessions_closed: t.sessionsClosed,
      sessions_billable: t.sessionsBillable,
      sessions_held: t.sessionsHeld,
      energy_uwh: t.energyUwh.toString(),
      revenue_micro_eur: t.revenueMicro.toString(),
      revenue_excl_vat: formatFixed(t.revenueMicro, 6),
      charging_ms: Number(t.chargingMs),
      fault_stops: t.faultStops,
      timeout_stops: t.timeoutStops,
      mean_power_mw: Number(t.meanPowerMw),
      revenue_per_kwh_micro_eur: t.revenuePerKwhMicro < 0n ? null : Number(formatFixed(t.revenuePerKwhMicro, 6)),
      best_asset_id: t.bestAssetId,
      per_asset: t.perAsset.map(function (a) {
        return { asset_id: a.assetId, sessions_closed: a.sessionsClosed, sessions_billable: a.sessionsBillable,
          energy_uwh: a.energyUwh.toString(), revenue_micro_eur: a.revenueMicro.toString(),
          charging_ms: Number(a.chargingMs), fault_stops: a.faultStops, timeout_stops: a.timeoutStops,
          undersampled_sessions: a.undersampled };
      })
    };
  }

  // ------------------------------------------------- held reasons & settlement
  function heldReason(s) {
    if (s.billable) return '';
    if (s.quality && s.quality !== 'OK') return s.quality;
    if (s.timeUpper || s.stopReason === 'COMMUNICATION_TIMEOUT') return 'COMMUNICATION_TIMEOUT';
    return 'UNSPECIFIED';
  }

  var CSV_HEADER = 'asset_id,session_id,closed_at_utc,quality,billable,stop_reason,energy_wh,billed_energy_wh,' +
    'charging_s,billed_time_s,energy_eur,time_eur,flat_eur,total_eur,currency,energy_lower_bound,time_upper_bound';

  // Same quoting as charger_settle, plus one deliberate difference: a text cell
  // that a spreadsheet would execute as a formula is neutralised with a leading
  // apostrophe. (The C++ tool's asset ids are restricted to [A-Za-z0-9._:-],
  // so for any journal it wrote the output is byte-identical.)
  function csvCell(s) {
    var v = String(s);
    if (/^[=+@\t\r]/.test(v) || /^-[^0-9]/.test(v)) v = "'" + v;
    if (/[",\n\r]/.test(v)) return '"' + v.replace(/"/g, '""') + '"';
    return v;
  }
  function settlementCsv(sessions) {
    var rows = [CSV_HEADER];
    sessions.forEach(function (s) {
      rows.push([csvCell(s.asset), s.id.toString(), csvCell(s.closedAt), csvCell(s.quality), s.billable ? 'yes' : 'no',
        csvCell(s.stopReason), s.energyWh.toString(), s.billedEnergyWh.toString(), s.chargingS.toString(),
        s.billedTimeS.toString(), formatFixed(s.energyCost, 6), formatFixed(s.timeCost, 6),
        formatFixed(s.flatCost, 6), formatFixed(s.totalCost, 6), csvCell(s.currency),
        s.energyLower ? 'yes' : 'no', s.timeUpper ? 'yes' : 'no'].join(','));
    });
    return rows.join('\n') + '\n';
  }

  // What `charger_settle --summary` prints, as numbers.
  function settlementSummary(sessions) {
    var r = { closed: sessions.length, billable: 0, held: 0, billableEnergyWh: 0n, energyCost: 0n, timeCost: 0n,
      flatCost: 0n, invoiceTotal: 0n, heldTotal: 0n };
    sessions.forEach(function (s) {
      if (s.billable) {
        r.billable++;
        r.billableEnergyWh += s.energyWh;
        r.energyCost += BigInt.asUintN(64, s.energyCost);
        r.timeCost += BigInt.asUintN(64, s.timeCost);
        r.flatCost += BigInt.asUintN(64, s.flatCost);
        r.invoiceTotal += BigInt.asUintN(64, s.totalCost);
      } else {
        r.held++;
        r.heldTotal += BigInt.asUintN(64, s.totalCost);
      }
    });
    return r;
  }

  // ------------------------------------------------------- independent pricing
  function ceilStep(consumed, step) {
    if (consumed <= 0n || step <= 1n) return consumed < 0n ? 0n : consumed;
    var rem = consumed % step;
    if (rem === 0n) return consumed;
    return consumed + (step - rem);
  }
  function mulDivRound(a, b, d) { return (a * b + d / 2n) / d; }
  function bigOr(v, dflt) {
    try { return v === undefined || v === null ? dflt : BigInt(v); } catch (e) { return dflt; }
  }

  /* recompute(session) -> { available, ok, checks: [{name, journal, recomputed, ok}], note }
   *
   * Re-derives, from the meter registers and the tariff of record embedded in
   * the close line, every derived number the journal carries: the Wh register,
   * the OCPI step ceilings, the three cost dimensions, the clamp, the quality
   * label and the billing_complete decision. This is the check a dispute turns
   * on: "does the invoice follow from the meter and the tariff?"
   */
  function recompute(s) {
    if (!s.tariffJson) {
      return { available: false, ok: false, checks: [], note: 'no tariff of record in this close line (journal written before v1.4.0)' };
    }
    var t;
    try { t = JSON.parse(s.tariffJson); } catch (e) {
      return { available: false, ok: false, checks: [], note: 'the tariff of record is not valid JSON' };
    }
    var rate = bigOr(t.energy_micro_eur_per_kwh, null);
    var trate = bigOr(t.time_micro_eur_per_hour, null);
    var flat = bigOr(t.flat_micro_eur, null);
    var estep = BigInt(t.energy_step_wh === undefined ? 1 : t.energy_step_wh);
    var tstep = BigInt(t.time_step_s === undefined ? 1 : t.time_step_s);
    if (rate === null || trate === null || flat === null || estep < 1n || tstep < 1n) {
      return { available: false, ok: false, checks: [], note: 'the tariff of record is incomplete' };
    }
    var minP = bigOr(t.min_price_micro_eur, null);
    var maxP = bigOr(t.max_price_micro_eur, null);

    var wh = s.energyUwh / UWH_PER_WH;
    var chargingS = s.durationMs / 1000n;
    var billedWh = ceilStep(wh, estep);
    var billedS = ceilStep(chargingS, tstep);
    var interrupted = s.stopReason === 'MONITOR_SHUTDOWN'; // the monitor was stopped, not the power stage
    var quality = s.samples < 2 ? 'NO_INTERVAL' : s.gaps > 0 ? 'GAPS' : s.energyUwh === 0n ? 'ZERO_ENERGY' : interrupted ? 'INTERRUPTED' : 'OK';
    var timeUpper = s.stopReason === 'COMMUNICATION_TIMEOUT';
    var energyLower = s.samples < 2 || s.gaps > 0 || timeUpper || interrupted;
    var complete = quality === 'OK' && !timeUpper;
    var energy = 0n, time = 0n, flatC = 0n, total = 0n;
    if (billedWh > INT64_MAX) {
      complete = false; quality = 'PRICE_OVERFLOW';
    } else {
      energy = mulDivRound(billedWh, rate, 1000n);
      time = mulDivRound(billedS, trate, 3600n);
      if (energy > INT64_MAX || time > INT64_MAX) {
        energy = time = 0n; complete = false; quality = 'PRICE_OVERFLOW';
      } else {
        flatC = flat;
        total = energy + time + flatC;
        if (total > INT64_MAX) { total = 0n; complete = false; quality = 'PRICE_OVERFLOW'; }
        else {
          if (minP !== null && total < minP) total = minP;
          if (maxP !== null && total > maxP) total = maxP;
        }
      }
    }
    var checks = [];
    function add(name, journal, recomputed, kind) {
      checks.push({ name: name, journal: String(journal), recomputed: String(recomputed), kind: kind || 'int',
        ok: String(journal) === String(recomputed) });
    }
    add('energy_wh', s.energyWh, wh);
    add('charging_s', s.chargingS, chargingS);
    add('billed_energy_wh', s.billedEnergyWh, billedWh);
    add('billed_time_s', s.billedTimeS, billedS);
    add('energy_cost', s.energyCost, energy, 'eur');
    add('time_cost', s.timeCost, time, 'eur');
    add('flat_cost', s.flatCost, flatC, 'eur');
    add('total_cost', s.totalCost, total, 'eur');
    add('quality', s.quality, quality, 'text');
    add('billing_complete', s.billable ? 'yes' : 'no', complete ? 'yes' : 'no', 'text');
    add('time_upper_bound', s.timeUpper ? 'yes' : 'no', timeUpper ? 'yes' : 'no', 'text');
    add('energy_lower_bound', s.energyLower ? 'yes' : 'no', energyLower ? 'yes' : 'no', 'text');
    var sumOk = s.energyCost + s.timeCost + s.flatCost === s.totalCost;
    var clamped = (minP !== null && s.energyCost + s.timeCost + s.flatCost < minP) ||
      (maxP !== null && s.energyCost + s.timeCost + s.flatCost > maxP);
    return {
      available: true,
      ok: checks.every(function (c) { return c.ok; }),
      checks: checks,
      // energy + time + flat == total, unless a min/max clamp (declared in the
      // tariff of record) moved the total.
      parts_sum_ok: sumOk || clamped,
      clamped: clamped,
      tariff: { rate: rate, trate: trate, flat: flat, estep: estep, tstep: tstep, minP: minP, maxP: maxP },
      note: ''
    };
  }

  // ------------------------------------------------------------------ anchors
  function parseAnchor(text) {
    var out = { ok: false, error: '' };
    if (text.indexOf(ANCHOR_SCHEMA) < 0) { out.error = 'anchor schema must be ' + ANCHOR_SCHEMA; return out; }
    function grab(key) {
      var m = new RegExp('"' + key + '"\\s*:\\s*"([^"]*)"').exec(text);
      return m ? m[1] : null;
    }
    var jsha = grab('journal_sha256'), last = grab('last_hash'), at = grab('anchored_at_utc'), lines = grab('lines');
    if (jsha === null) { out.error = "anchor is missing 'journal_sha256'"; return out; }
    if (last === null) { out.error = "anchor is missing 'last_hash'"; return out; }
    if (at === null) { out.error = "anchor is missing 'anchored_at_utc'"; return out; }
    if (lines === null) { out.error = "anchor is missing 'lines'"; return out; }
    var n = parseU64(lines);
    if (n === null) { out.error = 'anchor lines is not a non-negative integer'; return out; }
    if (jsha.length !== 64 || last.length !== 64) { out.error = 'anchor hashes must be 64 hex characters'; return out; }
    return { ok: true, error: '', journalSha256: jsha, lastHash: last, lines: n, anchoredAtUtc: at };
  }

  // Same decision procedure as verify_journal_anchor(): the chain must be
  // intact, the journal must not be shorter than the anchor, and the line the
  // anchor names must still hash to the anchored value. Growth after the anchor
  // is reported, not refused.
  function verifyAnchor(res, anchor) {
    if (!res.chain.intact) {
      return { ok: false, error: 'chain broken at line ' + res.chain.firstBrokenLine, appended: 0 };
    }
    if (BigInt(res.lines) < anchor.lines) {
      return { ok: false, appended: 0, error: 'journal is shorter than the anchor (' + res.lines + ' < ' + anchor.lines + '): lines were removed' };
    }
    var at = GENESIS;
    if (anchor.lines > 0n) {
      var target = Number(anchor.lines);
      var found = null;
      for (var i = 0; i < res.lineHashes.length; i++) {
        if (res.lineHashes[i][0] === target) { found = res.lineHashes[i][1]; break; }
      }
      at = found === null ? GENESIS : found;
    }
    if (at !== anchor.lastHash) {
      return { ok: false, appended: 0, error: 'anchor mismatch: line ' + anchor.lines + ' no longer hashes to ' + anchor.lastHash + ': the prefix was rewritten' };
    }
    var appended = BigInt(res.lines) > anchor.lines ? BigInt(res.lines) - anchor.lines : 0n;
    var wholeFileSame = res.fileSha256 === anchor.journalSha256;
    return { ok: true, error: '', appended: Number(appended), wholeFileSame: wholeFileSame };
  }

  /* comparePrefix(current, earlier) -> {relation, ...}
   *
   * A poor man's anchor: is an EARLIER COPY of the journal an exact prefix of
   * the current one? Both chains must verify; then the hash at the earlier
   * copy's last line must still be the hash at that line today, and every
   * line before it follows, because each hash commits to its predecessor.
   * Relations: 'identical', 'prefix' (the current file only grew), 'diverged'
   * (they agree up to a line and differ after: the history was rewritten) and
   * 'broken' (one of the two chains does not verify, so nothing can be said).
   */
  function comparePrefix(cur, old) {
    if (!cur.chain.intact || !old.chain.intact) {
      return { relation: 'broken', error: !old.chain.intact ? 'earlier' : 'current', line: (!old.chain.intact ? old : cur).chain.firstBrokenLine };
    }
    var n = Math.min(cur.lineHashes.length, old.lineHashes.length);
    // First chained line whose hash differs (binary search is valid: hashes commit to their predecessors).
    var lo = 0, hi = n; // find the smallest i in [0,n) with different hash, else n
    while (lo < hi) {
      var mid = (lo + hi) >> 1;
      if (cur.lineHashes[mid][1] === old.lineHashes[mid][1]) lo = mid + 1; else hi = mid;
    }
    if (lo < n) return { relation: 'diverged', agreeLines: lo, firstDifferentLine: old.lineHashes[lo][0], earlierLines: old.lines, currentLines: cur.lines };
    if (old.lines === cur.lines) return { relation: 'identical', lines: cur.lines };
    if (old.lines < cur.lines) return { relation: 'prefix', earlierLines: old.lines, currentLines: cur.lines, appended: cur.lines - old.lines };
    return { relation: 'diverged', agreeLines: n, firstDifferentLine: 0, shorter: true, earlierLines: old.lines, currentLines: cur.lines };
  }

  // Byte-for-byte the document `charger_monitor --anchor-journal` writes
  // (lines is a quoted string in that format). Refuses a broken chain:
  // anchoring tampered history would launder it.
  function makeAnchor(res, nowIsoUtc) {
    if (!res.chain.intact) throw new Error('refusing to anchor a broken journal: line ' + res.chain.firstBrokenLine);
    return '{"schema":"' + ANCHOR_SCHEMA + '","journal_sha256":"' + res.fileSha256 + '","last_hash":"' +
      res.chain.lastHash + '","lines":"' + res.lines + '","anchored_at_utc":"' + nowIsoUtc + '"}\n';
  }

  // The two shell lines an auditor can run without any of our software.
  function auditorRecipe(res, rec, fileName) {
    var f = fileName || 'bay.journal';
    return [
      '# 1. the digest of the whole file (compare with journal_sha256 in an anchor or dispute pack)',
      'sha256sum ' + f,
      '# 2. the close line of ' + rec.asset + ' #' + rec.id + ' (line ' + rec.line + '): recompute its link',
      "sed -n '" + rec.line + "p' " + f + " | cut -f1,3- | tr -d '\\n' | sha256sum",
      '# must equal the second field of that line:',
      "sed -n '" + rec.line + "p' " + f + ' | cut -f2',
      '# expected: ' + rec.hash
    ].join('\n');
  }

  // ----------------------------------------------------- time-based breakdowns
  // Constant-power apportionment of a session's energy over the UTC hours it
  // spanned. The journal stores totals, not the per-second series, so this is
  // an estimate (the same stated assumption as `charger_settle --tou-settle`).
  // Integer arithmetic with a cumulative floor, so the slices sum to the total.
  function floorDiv(a, b) { return a >= 0n ? a / b : -((-a + b - 1n) / b); }

  function hourSlices(s) {
    var close = parseUtc(s.closedAt);
    if (close === null || s.durationMs <= 0n || s.energyUwh === 0n) return [];
    var dur = s.durationMs;
    var endMs = BigInt(close) * 1000n;
    var startMs = endMs - dur;
    var HOUR = 3600000n;
    var out = [];
    var t = startMs, prevCum = 0n;
    while (t < endMs) {
      var next = (floorDiv(t, HOUR) + 1n) * HOUR;
      var t2 = next < endMs ? next : endMs;
      var cum = (s.energyUwh * (t2 - startMs)) / dur;
      out.push({ hour: Math.floor(Number(t) / 3600000), uwh: cum - prevCum });
      prevCum = cum;
      t = t2;
    }
    return out;
  }

  function dayKey(epochSec) { return formatUtc(epochSec).slice(0, 10); }

  function dailyBuckets(sessions) {
    var m = new Map();
    sessions.forEach(function (s) {
      var c = parseUtc(s.closedAt);
      if (c === null) return;
      var k = dayKey(c);
      var b = m.get(k);
      if (!b) { b = { day: k, billableUwh: 0n, heldUwh: 0n, billable: 0, held: 0, billableMicro: 0n, heldMicro: 0n }; m.set(k, b); }
      if (s.billable) { b.billable++; b.billableUwh += s.energyUwh; b.billableMicro += BigInt.asUintN(64, s.totalCost); }
      else { b.held++; b.heldUwh += s.energyUwh; b.heldMicro += BigInt.asUintN(64, s.totalCost); }
    });
    return Array.from(m.values()).sort(function (a, b) { return a.day < b.day ? -1 : a.day > b.day ? 1 : 0; });
  }

  // 7 x 24 matrix (Mon=0 .. Sun=6) of estimated energy per UTC hour, uWh.
  function weekHourHeat(sessions) {
    var grid = [];
    for (var d = 0; d < 7; d++) { grid.push(new Array(24).fill(0n)); }
    sessions.forEach(function (s) {
      hourSlices(s).forEach(function (sl) {
        var hourOfDay = ((sl.hour % 24) + 24) % 24;
        var dayIdx = (((Math.floor(sl.hour / 24) + 3) % 7) + 7) % 7; // 1970-01-01 was a Thursday
        grid[dayIdx][hourOfDay] += sl.uwh;
      });
    });
    return grid;
  }

  function hourOfDayProfile(sessions) {
    var prof = new Array(24).fill(0n);
    sessions.forEach(function (s) {
      hourSlices(s).forEach(function (sl) { prof[((sl.hour % 24) + 24) % 24] += sl.uwh; });
    });
    return prof;
  }

  // ------------------------------------------------------- live snapshot read
  // Accepts the stdout envelope {"timestamp","type":"snapshot","data":{...}}
  // or a bare charger.snapshot.v1 document.
  function parseSnapshotText(text) {
    var o;
    try { o = JSON.parse(text); } catch (e) { return null; }
    if (!o || typeof o !== 'object') return null;
    var data = o.data && o.data.schema ? o.data : o;
    if (data.schema !== 'charger.snapshot.v1') return null;
    return { timestamp: o.timestamp || '', data: data };
  }
  function parseNdjson(text) {
    var out = [];
    text.split(/\r?\n/).forEach(function (line) {
      if (!line.trim()) return;
      var s = parseSnapshotText(line);
      if (s) out.push(s);
    });
    return out;
  }

  return {
    GENESIS: GENESIS, LEDGER_SCHEMA: LEDGER_SCHEMA, ANCHOR_SCHEMA: ANCHOR_SCHEMA,
    Sha256: Sha256, sha256Hex: sha256Hex, toBytes: toBytes,
    parseJournal: parseJournal, rawLineOf: rawLineOf,
    rollup: rollup, portfolioJson: portfolioJson,
    heldReason: heldReason, settlementCsv: settlementCsv, settlementSummary: settlementSummary, CSV_HEADER: CSV_HEADER,
    recompute: recompute,
    parseAnchor: parseAnchor, verifyAnchor: verifyAnchor, makeAnchor: makeAnchor, comparePrefix: comparePrefix, auditorRecipe: auditorRecipe,
    hourSlices: hourSlices, dailyBuckets: dailyBuckets, weekHourHeat: weekHourHeat, hourOfDayProfile: hourOfDayProfile,
    parseSnapshotText: parseSnapshotText, parseNdjson: parseNdjson,
    formatFixed: formatFixed, trimFixed: trimFixed, eur: eur, groupInt: groupInt, kwhFromUwh: kwhFromUwh,
    parseUtc: parseUtc, formatUtc: formatUtc, fmtDuration: fmtDuration, dayKey: dayKey,
    parseU64: parseU64
  };
});
