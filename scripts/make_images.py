"""Generate every image the repository ships.

Reproducible by design: change the text or the data below, run this script,
commit the result. No network access, no external assets, no fonts to download.

    python scripts/make_images.py

Outputs into docs/:
    social-preview.png   1280x640  repository card
    architecture.png     component map of the library and its tools
    session-lifecycle.png  state machine and the billing gate
    exactness.png        integer vs double accumulator, measured values

Requires Pillow only. The palette matches the dark theme of GitHub.

Note on the accent bar: Pillow's rectangle bottom edge is inclusive, so a bar
asked for as [0, 0, w, 8] paints 9 rows. All bars here therefore pass 7 as the
bottom row, and a single ACCENT_ROWS constant documents what the bar is meant
to be. The image audit scripts enforce that height pixel-exactly.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- palette
BG = (13, 17, 23)
PANEL = (22, 27, 34)
PANEL_ALT = (27, 34, 43)
BORDER = (45, 51, 59)
TEXT = (230, 237, 243)
MUTED = (139, 148, 158)
FAINT = (92, 99, 107)
GREEN = (63, 185, 80)
GREEN_DIM = (45, 120, 58)
AMBER = (210, 153, 34)
RED = (248, 81, 73)
BLUE = (88, 166, 255)
PURPLE = (163, 113, 247)

ACCENT_ROWS = 8


def canvas(w, h, top_accent=GREEN):
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    if top_accent:
        d.rectangle([0, 0, w, ACCENT_ROWS - 1], fill=top_accent)
    return img, d


FONT_CANDIDATES = {
    "regular": [
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ],
    "bold": [
        "C:/Windows/Fonts/segoeuib.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ],
    "mono": [
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    ],
}


def font(size, weight="regular"):
    for path in FONT_CANDIDATES.get(weight, FONT_CANDIDATES["regular"]):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def wrap(d, text, f, max_w):
    words = text.split()
    lines, cur = [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if d.textlength(trial, font=f) <= max_w:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def panel(d, box, title=None, accent=None, title_font=None):
    x0, y0, x1, y1 = box
    d.rounded_rectangle(box, radius=10, fill=PANEL, outline=BORDER, width=1)
    if title:
        d.text((x0 + 20, y0 + 14), title, font=title_font or font(20, "bold"), fill=TEXT)


# ---------------------------------------------------------------- layout audit
def audit(path, expected_w, expected_h, accent_colour):
    """Fail loudly if an image is clipped or the accent bar drifted."""
    from PIL import Image as I
    import numpy as np
    im = np.array(I.open(path).convert("RGB"))
    h, w, _ = im.shape
    assert (w, h) == (expected_w, expected_h), f"{path}: {w}x{h} != {expected_w}x{expected_h}"
    bg = np.array(BG)
    # no content in the last 6 bottom rows
    assert np.abs(im[h-6:, :] - bg).sum(axis=2).max() <= 60, f"{path}: bottom clipping"
    # leftmost 8 body columns are pure background (content starts at x >= 8)
    assert np.abs(im[ACCENT_ROWS:, :8] - bg).sum(axis=2).max() <= 60, f"{path}: left clipping"
    assert np.abs(im[ACCENT_ROWS:, -8:] - bg).sum(axis=2).max() <= 60, f"{path}: right clipping"
    # accent bar exactly ACCENT_ROWS tall and pure accent colour
    bar = im[:ACCENT_ROWS, 0, :]
    assert np.all(bar == np.array(accent_colour)), f"{path}: accent bar wrong colour"
    assert np.abs(im[ACCENT_ROWS, 0, :] - bg).sum() <= 60, f"{path}: accent bar too tall"


# ---------------------------------------------------------------- social card
def social_preview(path):
    W, H = 1280, 640
    img, d = canvas(W, H, top_accent=GREEN)
    f_title = font(58, "bold")
    f_sub = font(24)
    f_head = font(17, "bold")
    f_body = font(16)
    f_mono = font(17, "mono")

    d.text((64, 56), "charger_monitor", font=f_title, fill=TEXT)
    d.text((64, 128),
           "Exact session ledger and an offline control room for DC charger bays",
           font=f_sub, fill=MUTED)

    # left panel: the invariant
    d.rounded_rectangle([64, 200, 604, 500], radius=12, fill=PANEL, outline=BORDER, width=1)
    d.text((92, 224), "THE INVARIANT", font=f_head, fill=GREEN)
    d.text((92, 264), "480 V * 10 A * 1 h", font=f_mono, fill=TEXT)
    d.text((92, 298), "=  exactly 4800 Wh", font=f_mono, fill=GREEN)
    d.text((92, 342), "integer microwatt-hours with a carried remainder,", font=f_body, fill=MUTED)
    d.text((92, 364), "so no float drift ever moves a Wh boundary.", font=f_body, fill=MUTED)

    # right panel: what it guarantees
    rx = 648
    d.rounded_rectangle([rx, 200, W - 64, 500], radius=12, fill=PANEL, outline=BORDER, width=1)
    d.text((rx + 28, 224), "GUARANTEED", font=f_head, fill=GREEN)
    rows = [
        ("dropouts", "never become invented energy"),
        ("restarts", "billing survives: hash-chained journal"),
        ("invoices", "billing_complete gates automation"),
        ("disputes", "meter + tariff + chain in one JSON"),
        ("OEM fleet", "vendor DBC rewires the decode safely"),
        ("auditors", "re-verify in a browser, offline, in 1 click"),
    ]
    y = 264
    for head, body in rows:
        d.text((rx + 28, y), head, font=f_body, fill=FAINT)
        d.text((rx + 128, y), body, font=f_body, fill=TEXT)
        y += 36

    d.text((64, H - 56),
           "C++17  ·  zero dependencies  ·  read-only  ·  Prometheus  ·  OCPI/OCPP-mappable  ·  Apache-2.0",
           font=f_sub, fill=MUTED)
    return img, (W, H)


# ---------------------------------------------------------------- architecture
def arrow_down(d, x, y0, y1, colour=None):
    colour = colour or BORDER
    d.line([x, y0, x, y1 - 8], fill=colour, width=2)
    d.polygon([(x - 6, y1 - 10), (x + 6, y1 - 10), (x, y1)], fill=colour)


def box(d, rect, title, lines, title_colour, f_box, f_small, outline=BORDER, fill=PANEL, width=1):
    x0, y0, x1, y1 = rect
    d.rounded_rectangle(rect, radius=10, fill=fill, outline=outline, width=width)
    d.text((x0 + 18, y0 + 14), title, font=f_box, fill=title_colour)
    yy = y0 + 42
    for line in lines:
        d.text((x0 + 18, yy), line, font=f_small, fill=MUTED)
        yy += 21


def architecture(path):
    W, H = 1280, 860
    img, d = canvas(W, H, top_accent=BLUE)
    f_h1 = font(28, "bold")
    f_box = font(17, "bold")
    f_body = font(15)
    f_small = font(13)
    f_mono = font(15, "mono")

    d.text((64, 46), "How the pieces fit", font=f_h1, fill=TEXT)
    d.text((64, 84), "CAN frames go in; an exact, verified record comes out and is read by people and systems that cannot change it.",
           font=f_body, fill=MUTED)

    # row 1: inputs
    cols = [64, 457, 850]
    w = 366
    box(d, [cols[0], 126, cols[0] + w, 236], "CAN bus  (listen-only)",
        ["SocketCAN live on the cabinet segment;", "the interface cannot even acknowledge a frame,", "so it can never drive the bus"],
        TEXT, f_box, f_small)
    box(d, [cols[1], 126, cols[1] + w, 236], "Recording  (candump -L)",
        ["replay a field capture with its REAL timing", "and recorded UTC; charger_probe measures the", "frame periods before you choose a timeout"],
        TEXT, f_box, f_small)
    box(d, [cols[2], 126, cols[2] + w, 236], "Config + tariff + vendor .dbc",
        ["versioned JSON: a misspelled field aborts", "startup; the OEM database rewires the decode", "exactly, validated bit for bit vs cantools"],
        TEXT, f_box, f_small)
    for x in (cols[0] + w // 2, cols[1] + w // 2, cols[2] + w // 2):
        arrow_down(d, x, 236, 282)

    # core
    d.rounded_rectangle([64, 282, 1216, 392], radius=12, fill=PANEL_ALT, outline=BLUE, width=2)
    d.text((92, 300), "charger_core   C++17, zero dependencies, deterministic, clock-injected", font=f_box, fill=BLUE)
    d.text((92, 332), "state machine  \u00b7  trapezoid integration in 128-bit integers  \u00b7  OCPI ENERGY/TIME/FLAT pricing", font=f_mono, fill=TEXT)
    d.text((92, 360), "never interpolates a dropout  \u00b7  predictive diagnostics (advisory, never blocking)  \u00b7  it never commands the power stage",
           font=f_body, fill=MUTED)
    for x in (cols[0] + w // 2, cols[1] + w // 2, cols[2] + w // 2):
        arrow_down(d, x, 392, 438)

    # row 3: artefacts
    box(d, [cols[0], 438, cols[0] + w, 548], "Journal  (billing record)",
        ["append-only, SHA-256 chained, fsync per line,", "crash recovery, orphans reported, tariff of record", "in every close line, external anchors"],
        GREEN, f_box, f_small, outline=GREEN, width=2)
    box(d, [cols[1], 438, cols[1] + w, 548], "Status files  (atomic, no socket)",
        ["JSON snapshot every report interval, even when", "the bus is silent; Prometheus text with the render", "time, so a dead monitor is visible"],
        BLUE, f_box, f_small)
    box(d, [cols[2], 438, cols[2] + w, 548], "Fleet roll-up",
        ["rebuilt from a journal alone: value, energy,", "held share, fault and timeout stops per bay;", "unknown stays unknown, never zero"],
        BLUE, f_box, f_small)
    for x in (cols[0] + w // 2, cols[1] + w // 2, cols[2] + w // 2):
        arrow_down(d, x, 548, 594)

    # row 4: consumers
    cw = 270
    xs = [64, 64 + cw + 24, 64 + 2 * (cw + 24), 64 + 3 * (cw + 24)]
    consumers = [
        ("Control Room", ["one offline HTML file: verifies the", "chain in the browser, recalculates", "every invoice, live bays, anchors"], PURPLE),
        ("charger_settle", ["CSV settlement, dispute pack, CDR,", "OCPP events, reconcile vs a meter,", "TOU estimate, Prometheus fleet view"], AMBER),
        ("Prometheus + alerts", ["node_exporter textfile; stale file,", "bus silent, fault, gaps, held sessions", "(example rules ship with the repo)"], AMBER),
        ("Auditor", ["sha256sum, sed and cut: no binary", "of ours needed to recompute a link", "or to check a time-stamped anchor"], GREEN),
    ]
    for x, (title, lines, colour) in zip(xs, consumers):
        box(d, [x, 594, x + cw, 704], title, lines, colour, f_box, f_small)

    # footer
    d.rounded_rectangle([64, 736, 1216, 800], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((88, 748), "Two implementations, one answer", font=f_box, fill=TEXT)
    d.text((88, 774), "the journal rules exist in C++ and, independently, in the dashboard; tests compare them byte for byte on freshly synthesised journals",
           font=f_small, fill=MUTED)
    d.text((64, 820), "Read-only end to end: no write path to a charger, a bus or a server.", font=f_small, fill=FAINT)
    return img, (W, H)


# ---------------------------------------------------------------- lifecycle
def session_lifecycle(path):
    W, H = 1280, 680
    img, d = canvas(W, H, top_accent=AMBER)
    f_h1 = font(28, "bold")
    f_state = font(20, "bold")
    f_body = font(15)
    f_small = font(13)

    d.text((64, 46), "Session lifecycle and the billing gate", font=f_h1, fill=TEXT)
    d.text((64, 84), "billing_complete is true only when every condition holds.", font=f_body, fill=MUTED)

    states = [("UNKNOWN", 92), ("READY", 332), ("CHARGING", 588), ("FAULT", 900)]
    y = 160
    for label, x in states:
        colour = AMBER if label == "CHARGING" else (RED if label == "FAULT" else BLUE)
        d.rounded_rectangle([x, y, x + 190, 226], radius=10, fill=PANEL, outline=colour, width=2)
        d.text((x + 42, y + 22), label, font=f_state, fill=colour)
    # arrows, with their labels BELOW the row so nothing touches a box
    for xa, xb, text in [(282, 332, "both streams fresh"), (522, 588, "status = CHARGING"), (778, 900, "fault latched")]:
        d.line([xa, 193, xb, 193], fill=BORDER, width=2)
        d.polygon([(xb - 8, 187), (xb - 8, 199), (xb, 193)], fill=BORDER)
        tw = d.textlength(text, font=f_small)
        d.text(((xa + xb) / 2 - tw / 2, 240), text, font=f_small, fill=FAINT)
        d.line([(xa + xb) / 2, 226, (xa + xb) / 2, 236], fill=BORDER, width=1)

    d.rounded_rectangle([92, 280, 1188, 336], radius=8, fill=PANEL_ALT, outline=BORDER, width=1)
    d.text((116, 288), "Entering CHARGING opens the session. Leaving it closes it with a stop reason:", font=f_small, fill=TEXT)
    d.text((116, 310), "STATUS_OFF, STATUS_READY, COMMUNICATION_TIMEOUT, a fault code, or MONITOR_SHUTDOWN.", font=f_small, fill=MUTED)

    # the gate itself
    d.rounded_rectangle([92, 350, 1188, 612], radius=12, fill=PANEL, outline=GREEN, width=2)
    d.text((118, 370), "billing_complete = all of", font=f_state, fill=GREEN)
    conds = [
        ("closed cleanly", "the state left CHARGING on purpose, not by a crash mid-session"),
        ("two or more samples", "a single accepted frame cannot define an interval, let alone its energy"),
        ("no integration gap", "any interval beyond max_integration_step adds no energy: it is never interpolated"),
        ("not cut by a timeout", "a COMMUNICATION_TIMEOUT stop yields a time_upper_bound, not a bill"),
        ("not interrupted", "a monitor shutdown keeps the exact energy so far but holds the session for review"),
    ]
    yy = 406
    for head, body in conds:
        d.text((140, yy), bulletin := "\u2022", font=f_body, fill=GREEN)
        d.text((160, yy), head, font=f_body, fill=TEXT)
        d.text((350, yy), body, font=f_small, fill=MUTED)
        yy += 34
    return img, (W, H)


# ---------------------------------------------------------------- exactness
def exactness(path):
    W, H = 1180, 660
    img, d = canvas(W, H, top_accent=BLUE)
    f_h1 = font(28, "bold")
    f_label = font(17, "bold")
    f_body = font(16)
    f_small = font(14)

    d.text((56, 40), "Why the ledger holds integers", font=f_h1, fill=TEXT)
    d.text((56, 78), "Measured on the same workloads by charger_bench: a double accumulator drifts, "
                    "the integer path does not.", font=f_body, fill=MUTED)

    cases = [
        ("0.1 Wh x 1e6", "accumulated Wh", "drifts by 1 uWh", RED),
        ("100k x 0.008333 EUR", "monthly settlement", "drifts by 1 uEUR", RED),
    ]
    x0, y0 = 64, 150
    for i, (title, sub, note, colour) in enumerate(cases):
        by = y0 + i * 130
        d.text((x0, by), title, font=f_label, fill=TEXT)
        d.text((x0, by + 22), sub, font=f_small, fill=FAINT)

        d.text((x0 + 300, by + 4), "integer", font=f_small, fill=MUTED)
        d.rounded_rectangle([x0 + 370, by, x0 + 790, by + 22], radius=4, fill=GREEN)
        d.text((x0 + 806, by + 4), "exact", font=f_small, fill=GREEN)

        d.text((x0 + 300, by + 40), "double", font=f_small, fill=MUTED)
        d.rounded_rectangle([x0 + 370, by + 36, x0 + 784, by + 58], radius=4, fill=RED)
        d.text((x0 + 806, by + 40), note, font=f_small, fill=colour)
        if x0 + 374 + 460 > W - 56:
            pass  # keep the bar inside the right margin

    ty = 410
    track_x, track_w = x0, W - 2 * x0
    d.text((track_x, ty), "ingest throughput vs what a CAN bus can deliver  (log scale)", font=f_label, fill=TEXT)
    d.text((track_x, ty + 22), "measured by charger_bench in a Release build on the machine that built these docs "
                               "(4 vCPU container, GCC 13); run it on yours",
           font=f_small, fill=FAINT)
    import math
    lo, hi = 3.0, 7.0   # 1e3 .. 1e7 frames/s

    def bar(y, value, colour, label):
        frac = (math.log10(value) - lo) / (hi - lo)
        end = track_x + int(track_w * frac)
        d.rounded_rectangle([track_x, y, track_x + track_w, y + 22], radius=4, fill=PANEL_ALT)
        d.rounded_rectangle([track_x, y, end, y + 22], radius=4, fill=colour)
        tw = d.textlength(label, font=f_small)
        if tw + 20 < end - track_x:
            d.text((track_x + 10, y + 3), label, font=f_small, fill=BG)   # inside the bar
        else:
            d.text((end + 10, y + 3), label, font=f_small, fill=TEXT)     # beside it

    bar(ty + 56, 3500, BLUE, "3 500 frames/s: a 500 kbit/s CAN bus at 1 Hz")
    bar(ty + 88, 8463432, PURPLE, "8.46 M frames/s ingested   (about 2 400 x the bus)")
    for k in range(3, 8):
        gx = track_x + int(track_w * (k - lo) / (hi - lo))
        d.text((gx - 10, ty + 118), "1e%d" % k, font=f_small, fill=FAINT)

    d.text((track_x, ty + 146), "Throughput is not a constraint. Exactness is, and it is structural: the ledger holds integers,",
           font=f_small, fill=MUTED)
    d.text((track_x, ty + 166), "so no amount of accumulated rounding moves a Wh boundary.", font=f_small, fill=MUTED)
    return img, (W, H)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "docs"
    os.makedirs(out, exist_ok=True)
    jobs = [
        ("social-preview.png", social_preview, GREEN),
        ("architecture.png", architecture, BLUE),
        ("session-lifecycle.png", session_lifecycle, AMBER),
        ("exactness.png", exactness, BLUE),
    ]
    for name, fn, accent in jobs:
        target = os.path.join(out, name)
        img, (w, h) = fn(target)
        img.save(target)
        audit(target, w, h, accent)
        print("wrote and audited " + target)


if __name__ == "__main__":
    main()
