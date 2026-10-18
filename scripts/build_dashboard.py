#!/usr/bin/env python3
"""Bundle the control room into ONE offline HTML file.

    python3 scripts/build_dashboard.py            # write dashboard/charger-control-room.html (+ the Pages copy)
    python3 scripts/build_dashboard.py --check    # exit 1 if a committed copy is stale

Why one file: an operator on an air-gapped plant network, or an auditor you
email a report to, needs something they can read, hash and open with a double
click. There is no CDN, no font download, no framework and no build toolchain
beyond the Python standard library.

The Content-Security-Policy in the page pins the inline script by its SHA-256,
so a script that is not byte-identical to what this builder emitted will not
run. Embedding a journal into a saved report changes only a JSON data block,
never the script, so the pin keeps holding.
"""

import argparse
import base64
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "dashboard" / "src"
DATA = ROOT / "dashboard" / "data"
OUT = ROOT / "dashboard" / "charger-control-room.html"
# Byte-identical copy served by GitHub Pages (Settings -> Pages -> branch, /docs),
# so the control room can be tried in a browser without downloading anything.
PAGES_COPY = ROOT / "docs" / "control-room.html"

# Order matters: each file only uses what the ones before it export.
SCRIPTS = ["core.js", "dom.js", "charts.js", "i18n.js", "app.js"]

# A window of the synthetic capture is embedded, not the whole day: the page
# stays small and the window is chosen around the first communication drop-out
# so the replay shows something worth looking at.
CAPTURE_WINDOW = 360


def read(path):
    return path.read_text(encoding="utf-8")


def capture_window(text):
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if len(lines) <= CAPTURE_WINDOW:
        return "\n".join(lines) + "\n"
    first = None
    for i, ln in enumerate(lines):
        if '"state":"DISCONNECTED"' in ln and i > 0 and '"state":"CHARGING"' in lines[i - 1]:
            first = i
            break
    center = first if first is not None else len(lines) // 2
    start = max(0, min(center - CAPTURE_WINDOW // 2, len(lines) - CAPTURE_WINDOW))
    return "\n".join(lines[start : start + CAPTURE_WINDOW]) + "\n"


def build():
    js_parts = []
    for name in SCRIPTS:
        body = read(SRC / name)
        if "</script" in body.lower():
            sys.exit(f"{name} contains '</script': refuse to inline it")
        js_parts.append(f"/* ---- {name} ---- */\n{body}")
    js = "\n".join(js_parts)
    css = read(SRC / "style.css")
    if "</style" in css.lower():
        sys.exit("style.css contains '</style'")

    demo = {}
    journal = DATA / "demo.journal"
    if journal.exists():
        demo["journal"] = base64.b64encode(journal.read_bytes()).decode("ascii")
        anchor = DATA / "demo.anchor"
        if anchor.exists():
            demo["anchor"] = read(anchor)
        capture = DATA / "demo-capture.ndjson"
        if capture.exists():
            demo["capture"] = capture_window(read(capture))
    # json.dumps never emits a raw '<', but be explicit: a data block must not
    # be able to close itself.
    demo_json = json.dumps(demo, separators=(",", ":")).replace("<", "\\u003c")

    digest = base64.b64encode(hashlib.sha256(("\n" + js + "\n").encode("utf-8")).digest()).decode("ascii")
    csp = (
        "default-src 'none'; "
        f"script-src 'sha256-{digest}'; "
        "style-src 'unsafe-inline'; img-src data:; connect-src 'self'; "
        "base-uri 'none'; form-action 'none'"
    )
    html = read(SRC / "index.template.html")
    for marker, value in (("__CSP__", csp), ("__CSS__", css), ("__DEMO__", demo_json), ("__JS__", js)):
        if html.count(marker) != 1:
            sys.exit(f"template marker {marker} must appear exactly once")
        html = html.replace(marker, value)
    return html


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the committed bundle is out of date")
    args = ap.parse_args()
    html = build()
    targets = [OUT, PAGES_COPY]
    if args.check:
        stale = [t for t in targets if not t.exists() or t.read_text(encoding="utf-8") != html]
        if stale:
            sys.exit("stale: " + ", ".join(str(t.relative_to(ROOT)) for t in stale) + ": run scripts/build_dashboard.py")
        print("dashboard bundle is up to date")
        return
    for target in targets:
        target.write_text(html, encoding="utf-8")
        print(f"wrote {target.relative_to(ROOT)} ({len(html):,} bytes)")

if __name__ == "__main__":
    main()
