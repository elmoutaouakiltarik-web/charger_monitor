/* DOM and SVG helpers. Every piece of journal or snapshot text reaches the page
 * through createTextNode / textContent: there is no innerHTML anywhere in the
 * dashboard, so a hostile asset id or stop reason cannot become markup. */
(function (root) {
  'use strict';
  var SVGNS = 'http://www.w3.org/2000/svg';

  function apply(el, attrs) {
    if (!attrs) return;
    Object.keys(attrs).forEach(function (k) {
      var v = attrs[k];
      if (v === null || v === undefined || v === false) return;
      if (k === 'class') el.setAttribute('class', v);
      else if (k.slice(0, 2) === 'on' && typeof v === 'function') el.addEventListener(k.slice(2), v);
      else if (v === true) el.setAttribute(k, '');
      else el.setAttribute(k, String(v));
    });
  }
  function append(el, kids) {
    kids.forEach(function (k) {
      if (k === null || k === undefined || k === false) return;
      if (Array.isArray(k)) append(el, k);
      else if (k instanceof Node) el.appendChild(k);
      else el.appendChild(document.createTextNode(String(k)));
    });
  }
  function h(tag, attrs) {
    var el = document.createElement(tag);
    apply(el, attrs);
    append(el, Array.prototype.slice.call(arguments, 2));
    return el;
  }
  function s(tag, attrs) {
    var el = document.createElementNS(SVGNS, tag);
    apply(el, attrs);
    append(el, Array.prototype.slice.call(arguments, 2));
    return el;
  }

  var ICONS = {
    check: 'M5 12.5l4.5 4.5L19 7.5',
    cross: 'M6 6l12 12M18 6L6 18',
    warn: 'M12 3.5l9.5 16.5h-19L12 3.5zM12 10v4.5M12 17.2v.3',
    bolt: 'M13 2.5L5 13.5h6l-1 8 8-11h-6l1-8z',
    link: 'M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1',
    download: 'M12 3.5v12M7 11l5 5 5-5M4.5 20.5h15',
    copy: 'M9 9h11v11H9zM5.5 15H4V4h11v1.5',
    info: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 10.5v6M12 7.5v.3',
    off: 'M12 3v8M7 6.5a8 8 0 1 0 10 0',
    ready: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 12.3l2.7 2.7L16 9.5',
    unknown: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM9.5 9.5a2.5 2.5 0 1 1 3.6 2.2c-.7.4-1.1.9-1.1 1.8M12 16.8v.3',
    plug: 'M9 3v5M15 3v5M6 8h12v3a6 6 0 0 1-12 0V8zM12 17v4',
    play: 'M7 4.5v15l12-7.5-12-7.5z',
    pause: 'M7 5v14M17 5v14',
    print: 'M7 9V3.5h10V9M7 17H4.5V9h15v8H17M7 14h10v6.5H7z',
    sun: 'M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4',
    file: 'M6 3h8l4 4v14H6V3zM14 3v4h4'
  };
  function icon(name, cls) {
    return s('svg', { class: 'ico ' + (cls || ''), viewBox: '0 0 24 24', 'aria-hidden': 'true', fill: 'none',
      stroke: 'currentColor', 'stroke-width': '2', 'stroke-linecap': 'round', 'stroke-linejoin': 'round' },
    s('path', { d: ICONS[name] || ICONS.info }));
  }

  root.ChargerDom = { h: h, s: s, icon: icon };
})(typeof self !== 'undefined' ? self : this);
