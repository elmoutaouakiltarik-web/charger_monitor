/* Hand-rolled SVG charts. No library, no network. Marks follow the house specs:
 * columns <= 24px with a 4px rounded data end, a 2px surface gap between stacked
 * segments, hairline recessive grid, labels in ink tokens (never the series
 * colour), tooltips on every mark (data-tip), a table view beside every chart.
 * Values reaching this file are display numbers: the ledger itself stays BigInt. */
(function (root) {
  'use strict';
  var D = root.ChargerDom;
  var s = D.s;

  function niceMax(max) {
    if (!(max > 0)) return 1;
    var exp = Math.pow(10, Math.floor(Math.log10(max)));
    var f = max / exp;
    var nice = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10;
    return nice * exp;
  }
  function compact(n) {
    var a = Math.abs(n);
    if (a === 0) return '0';
    if (a >= 1e6) return (n / 1e6).toFixed(a >= 1e7 ? 0 : 1).replace(/\.0$/, '') + 'M';
    if (a >= 1e3) return (n / 1e3).toFixed(a >= 1e4 ? 0 : 1).replace(/\.0$/, '') + 'K';
    if (a >= 100) return String(Math.round(n));
    if (a >= 10) return n.toFixed(1).replace(/\.0$/, '');
    return n.toFixed(2).replace(/0$/, '').replace(/\.$/, '');
  }

  // Top-rounded path (4px data end), square at the baseline.
  function colPath(x, y, w, hgt, r) {
    r = Math.max(0, Math.min(r, w / 2, hgt));
    return 'M' + x + ',' + (y + hgt) + 'L' + x + ',' + (y + r) + 'Q' + x + ',' + y + ' ' + (x + r) + ',' + y +
      'L' + (x + w - r) + ',' + y + 'Q' + (x + w) + ',' + y + ' ' + (x + w) + ',' + (y + r) + 'L' + (x + w) + ',' + (y + hgt) + 'Z';
  }

  /* stackedColumns({w,h,data:[{label,tip,parts:[{v,cls}]}],yFmt,topLabel}) */
  function stackedColumns(o) {
    var W = Math.max(280, o.w), H = o.h || 220;
    var m = { l: 42, r: 8, t: 16, b: 24 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var data = o.data, n = data.length;
    var totals = data.map(function (d) { return d.parts.reduce(function (a, p) { return a + p.v; }, 0); });
    var max = niceMax(Math.max.apply(null, totals.concat([0])));
    var svg = s('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': o.aria || '' });
    var grid = s('g', { class: 'grid' }), axis = s('g', { class: 'axis' });
    for (var i = 0; i <= 4; i++) {
      var y = m.t + ph - (ph * i) / 4;
      grid.appendChild(s('line', { x1: m.l, x2: W - m.r, y1: y, y2: y, 'stroke-width': 1 }));
      axis.appendChild(s('text', { x: m.l - 6, y: y + 3.5, 'text-anchor': 'end' }, (o.yFmt || compact)((max * i) / 4)));
    }
    svg.appendChild(grid); svg.appendChild(axis);
    if (!n) return svg;
    var band = pw / n, bw = Math.min(24, Math.max(1.5, band - 2));
    var every = Math.max(1, Math.ceil(n / Math.max(1, Math.floor(pw / 52))));
    var maxIdx = totals.indexOf(Math.max.apply(null, totals));
    data.forEach(function (d, idx) {
      var x = m.l + idx * band + (band - bw) / 2;
      var cursor = m.t + ph, drawn = 0, lastSeg = null;
      d.parts.forEach(function (p) {
        if (p.v <= 0) return;
        var hh = Math.max(1, (p.v / max) * ph);
        var gap = drawn ? 2 : 0;
        cursor -= hh;
        lastSeg = { y: cursor, h: hh - gap, cls: p.cls, x: x };
        var seg = s('path', { class: p.cls, d: colPath(x, cursor, bw, hh - gap, 0) });
        seg.dataset.seg = '1';
        d._segs = d._segs || [];
        d._segs.push({ node: seg, y: cursor, h: hh - gap, cls: p.cls });
        svg.appendChild(seg);
        drawn++;
      });
      // 4px rounded data end on the topmost segment only.
      if (lastSeg && d._segs.length) {
        var top = d._segs[d._segs.length - 1];
        top.node.setAttribute('d', colPath(x, top.y, bw, top.h, 4));
      }
      var hit = s('rect', { x: m.l + idx * band, y: m.t, width: band, height: ph, fill: 'transparent', tabindex: '-1' });
      hit.setAttribute('data-tip', d.tip);
      svg.appendChild(hit);
      if (idx % every === 0) {
        svg.appendChild(s('text', { x: x + bw / 2, y: H - 7, 'text-anchor': 'middle' }, d.label));
      }
      if (idx === maxIdx && o.topLabel && totals[idx] > 0) {
        svg.appendChild(s('text', { class: 'lbl', x: x + bw / 2, y: Math.max(11, m.t + ph - (totals[idx] / max) * ph - 4), 'text-anchor': 'middle' }, o.topLabel(totals[idx])));
      }
    });
    svg.appendChild(s('line', { class: 'st-axis', x1: m.l, x2: W - m.r, y1: m.t + ph, y2: m.t + ph, 'stroke-width': 1 }));
    return svg;
  }

  /* hbars({w,rows:[{label,parts:[{v,cls}],value,tip}]}) */
  function hbars(o) {
    var rows = o.rows, rowH = o.rowH || 26;
    var W = Math.max(280, o.w);
    var labelW = Math.min(150, Math.max(54, Math.max.apply(null, rows.map(function (r) { return r.label.length; }).concat([4])) * 6.4 + 8));
    var valueW = 74;
    var pw = W - labelW - valueW - 4;
    var H = rows.length * rowH + 6;
    var svg = s('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': o.aria || '' });
    var totals = rows.map(function (r) { return r.parts.reduce(function (a, p) { return a + p.v; }, 0); });
    var max = Math.max.apply(null, totals.concat([1e-12]));
    rows.forEach(function (r, i) {
      var y = 3 + i * rowH, bh = Math.min(16, rowH - 8), by = y + (rowH - bh) / 2;
      svg.appendChild(s('text', { x: labelW - 8, y: y + rowH / 2 + 4, 'text-anchor': 'end', class: 'lbl' }, r.label));
      var x = labelW, parts = r.parts.filter(function (p) { return p.v > 0; });
      parts.forEach(function (p, pi) {
        var w = Math.max(2, (p.v / max) * pw);
        var gap = pi ? 2 : 0;
        var last = pi === parts.length - 1;
        var path = last
          ? 'M' + (x + gap) + ',' + by + 'L' + (x + w - 4) + ',' + by + 'Q' + (x + w) + ',' + by + ' ' + (x + w) + ',' + (by + 4) + 'L' + (x + w) + ',' + (by + bh - 4) + 'Q' + (x + w) + ',' + (by + bh) + ' ' + (x + w - 4) + ',' + (by + bh) + 'L' + (x + gap) + ',' + (by + bh) + 'Z'
          : 'M' + (x + gap) + ',' + by + 'L' + (x + w) + ',' + by + 'L' + (x + w) + ',' + (by + bh) + 'L' + (x + gap) + ',' + (by + bh) + 'Z';
        svg.appendChild(s('path', { class: p.cls, d: path }));
        x += w;
      });
      svg.appendChild(s('text', { class: 'lbl', x: x + 6, y: y + rowH / 2 + 4 }, r.value));
      var hit = s('rect', { x: 0, y: y, width: W, height: rowH, fill: 'transparent' });
      hit.setAttribute('data-tip', r.tip);
      svg.appendChild(hit);
    });
    return svg;
  }

  /* heat({w,grid:[7][24] numbers,rowLabels,tip(r,c,v)}) — sequential, one hue. */
  function heat(o) {
    var W = Math.max(300, o.w);
    var labelW = 34, topH = 16;
    var cw = Math.min(30, (W - labelW - 4) / 24), ch = Math.min(26, cw + 2);
    var H = topH + 7 * ch + 4;
    var svg = s('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': o.aria || '' });
    var max = 0;
    o.grid.forEach(function (row) { row.forEach(function (v) { if (v > max) max = v; }); });
    for (var c = 0; c < 24; c += 3) {
      svg.appendChild(s('text', { x: labelW + c * cw + cw / 2, y: 11, 'text-anchor': 'middle' }, (c < 10 ? '0' : '') + c));
    }
    o.grid.forEach(function (row, r) {
      svg.appendChild(s('text', { x: labelW - 6, y: topH + r * ch + ch / 2 + 4, 'text-anchor': 'end' }, o.rowLabels[r]));
      row.forEach(function (v, c) {
        var q = v <= 0 || max <= 0 ? -1 : Math.min(6, Math.floor((v / max) * 6.999));
        var rect = s('rect', { x: labelW + c * cw + 1, y: topH + r * ch + 1, width: cw - 2, height: ch - 2, rx: 3,
          class: q < 0 ? 'f-surface2' : 'f-q' + q });
        rect.setAttribute('data-tip', o.tip(r, c, v));
        svg.appendChild(rect);
      });
    });
    return svg;
  }

  /* spark(values, w, h) — 2px line, area wash at 10%. */
  function spark(values, w, hgt, cls) {
    var svg = s('svg', { class: 'spark', viewBox: '0 0 ' + w + ' ' + hgt, preserveAspectRatio: 'none', role: 'img' });
    if (values.length < 2) return svg;
    var max = Math.max.apply(null, values), min = Math.min.apply(null, values.concat([0]));
    var span = max - min || 1;
    var pts = values.map(function (v, i) {
      return [(i / (values.length - 1)) * w, hgt - 4 - ((v - min) / span) * (hgt - 8)];
    });
    var d = pts.map(function (p, i) { return (i ? 'L' : 'M') + p[0].toFixed(1) + ',' + p[1].toFixed(1); }).join('');
    svg.appendChild(s('path', { class: 'area', d: d + 'L' + w + ',' + hgt + 'L0,' + hgt + 'Z' }));
    svg.appendChild(s('path', { d: d, class: cls || '' }));
    return svg;
  }

  /* timeline({w,h,xs,ys,states,marker}) — a line over a capture with the state strip below. */
  function timeline(o) {
    var W = Math.max(300, o.w), H = o.h || 150;
    var m = { l: 44, r: 8, t: 10, b: 38 };
    var pw = W - m.l - m.r, ph = H - m.t - m.b;
    var svg = s('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': o.aria || '' });
    var n = o.ys.length;
    var max = niceMax(Math.max.apply(null, o.ys.concat([0])));
    for (var i = 0; i <= 3; i++) {
      var y = m.t + ph - (ph * i) / 3;
      svg.appendChild(s('line', { class: 'st-grid', x1: m.l, x2: W - m.r, y1: y, y2: y, 'stroke-width': 1 }));
      svg.appendChild(s('text', { x: m.l - 6, y: y + 3.5, 'text-anchor': 'end' }, compact((max * i) / 3)));
    }
    if (n > 1) {
      var step = Math.max(1, Math.floor(n / pw));
      var d = '';
      for (var k = 0; k < n; k += step) {
        var px = m.l + (k / (n - 1)) * pw, py = m.t + ph - (o.ys[k] / max) * ph;
        d += (k ? 'L' : 'M') + px.toFixed(1) + ',' + py.toFixed(1);
      }
      svg.appendChild(s('path', { d: d + 'L' + (m.l + pw) + ',' + (m.t + ph) + 'L' + m.l + ',' + (m.t + ph) + 'Z', class: 'f-s1', 'fill-opacity': '0.1' }));
      svg.appendChild(s('path', { d: d, fill: 'none', class: 'st-s1', 'stroke-width': 2, 'stroke-linejoin': 'round' }));
      // state strip
      var cells = Math.min(n, Math.floor(pw / 2));
      for (var c = 0; c < cells; c++) {
        var idx = Math.floor((c / cells) * n);
        svg.appendChild(s('rect', { x: m.l + (c / cells) * pw, y: m.t + ph + 6, width: pw / cells + 0.5, height: 10, class: o.stateCls(o.states[idx]) }));
      }
      if (o.marker !== undefined && o.marker !== null) {
        var mx = m.l + (o.marker / (n - 1)) * pw;
        svg.appendChild(s('line', { x1: mx, x2: mx, y1: m.t, y2: m.t + ph + 18, class: 'st-axis', 'stroke-width': 2 }));
        svg.appendChild(s('circle', { cx: mx, cy: m.t + ph - (o.ys[o.marker] / max) * ph, r: 4.5, class: 'f-s1', stroke: 'var(--surface)', 'stroke-width': 2 }));
      }
      (o.xLabels || []).forEach(function (xl) {
        svg.appendChild(s('text', { x: m.l + xl.at * pw, y: H - 6, 'text-anchor': xl.at < 0.05 ? 'start' : xl.at > 0.95 ? 'end' : 'middle' }, xl.text));
      });
    }
    return svg;
  }

  root.ChargerCharts = { niceMax: niceMax, compact: compact, stackedColumns: stackedColumns, hbars: hbars, heat: heat, spark: spark, timeline: timeline };
})(typeof self !== 'undefined' ? self : this);
