"""Generate every image the repository ships.

Reproducible by design: change the text or the data below, run this script,
commit the result. No network access, no external assets, no fonts to download.

    python scripts/make_images.py

Outputs into docs/:
    social-preview.png   1280x640  repository card
    architecture.png     component map of the library and its tools
    session-lifecycle.png  state machine and the billing gate
    exactness.png        integer vs double accumulator, measured values
    fleet-kpi.png        what the operator sees per bay over time

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
           "Exact charging-session ledger for industrial DC charger bays",
           font=f_sub, fill=MUTED)

    # left panel: the invariant
    d.rounded_rectangle([64, 200, 604, 470], radius=12, fill=PANEL, outline=BORDER, width=1)
    d.text((92, 224), "THE INVARIANT", font=f_head, fill=GREEN)
    d.text((92, 264), "480 V * 10 A * 1 h", font=f_mono, fill=TEXT)
    d.text((92, 298), "=  exactly 4800 Wh", font=f_mono, fill=GREEN)
    d.text((92, 342), "integer microwatt-hours with a carried remainder,", font=f_body, fill=MUTED)
    d.text((92, 364), "so no float drift ever moves a Wh boundary.", font=f_body, fill=MUTED)

    # right panel: what it guarantees
    rx = 648
    d.rounded_rectangle([rx, 200, W - 64, 470], radius=12, fill=PANEL, outline=BORDER, width=1)
    d.text((rx + 28, 224), "GUARANTEED", font=f_head, fill=GREEN)
    rows = [
        ("dropouts", "never become invented energy"),
        ("restarts", "billing survives: hash-chained journal"),
        ("invoices", "billing_complete gates automation"),
        ("disputes", "meter + tariff + chain in one JSON"),
        ("OEM fleet", "vendor DBC rewires the decode safely"),
    ]
    y = 264
    for head, body in rows:
        d.text((rx + 28, y), head, font=f_body, fill=FAINT)
        d.text((rx + 128, y), body, font=f_body, fill=TEXT)
        y += 40

    d.text((64, H - 56),
           "C++17  ·  zero dependencies  ·  OCPI subset  ·  OCPP-mappable  ·  Apache-2.0",
           font=f_sub, fill=MUTED)
    return img, (W, H)


# ---------------------------------------------------------------- architecture
def architecture(path):
    W, H = 1280, 720
    img, d = canvas(W, H, top_accent=BLUE)
    f_h1 = font(28, "bold")
    f_box = font(17, "bold")
    f_body = font(15)
    f_small = font(13)
    f_mono = font(15, "mono")

    d.text((64, 46), "How the pieces fit", font=f_h1, fill=TEXT)
    d.text((64, 84), "CAN frames go in; an exact, verified ledger comes out.", font=f_body, fill=MUTED)

    # input row: two panels
    d.rounded_rectangle([64, 140, 564, 250], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((88, 160), "CAN bus / vendor DBC", font=f_box, fill=TEXT)
    d.text((88, 186), "SocketCAN live, or a .dbc file that rewires the wire layout exactly",
           font=f_small, fill=MUTED)
    d.text((88, 210), "decode validated bit-exactly against cantools 45.x", font=f_small, fill=FAINT)

    d.rounded_rectangle([636, 140, 1216, 250], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((660, 160), "Config + tariff", font=f_box, fill=TEXT)
    d.text((660, 186), "versioned JSON; malformed or misspelled fields abort startup,",
           font=f_small, fill=MUTED)
    d.text((660, 210), "because silent zero pricing is the bug nobody notices", font=f_small, fill=FAINT)

    # arrows into core
    for x in (314, 926):
        d.line([x, 250, x, 306], fill=BORDER, width=2)
        d.polygon([(x - 6, 296), (x + 6, 296), (x, 306)], fill=BORDER)

    # core
    d.rounded_rectangle([264, 306, 1016, 430], radius=12, fill=PANEL_ALT, outline=BLUE, width=2)
    d.text((296, 328), "charger_core  (C++17, no dependencies)", font=f_box, fill=BLUE)
    d.text((296, 358), "state machine  ·  exact trapezoid integration (128-bit)  ·  OCPI pricing",
           font=f_mono, fill=TEXT)
    d.text((296, 386), "predictive diagnostics (advisory, never blocking)  ·  deterministic, clock-injected",
           font=f_body, fill=MUTED)

    # three outputs
    outs = [
        (64,  "charger_ledger.hpp",   "hash-chained journal, fsync per line,\nrecovery, orphans, tamper detection"),
        (492, "charger_portfolio.hpp", "fleet rollup rebuilt from the journal\nalone: revenue, energy, ranking"),
        (920, "charger_settle --dispute", "meter + tariff of record + calculation\n+ chain provenance in one pack"),
    ]
    for x, title, body in outs:
        d.line([x + 250 if x != 920 else x + 148, 430, x + 250 if x != 920 else x + 148, 478],
               fill=BORDER, width=2)
        d.polygon([(x + 244 if x != 920 else x + 142, 468), (x + 256 if x != 920 else x + 154, 468),
                   (x + 250 if x != 920 else x + 148, 478)], fill=BORDER)
        d.rounded_rectangle([x, 478, x + 348, 606], radius=10, fill=PANEL, outline=BORDER, width=1)
        d.text((x + 20, 494), title, font=f_box, fill=BLUE)
        yy = 524
        for line in body.split("\n"):
            d.text((x + 20, yy), line, font=f_small, fill=MUTED)
            yy += 22

    # tools footer
    d.rounded_rectangle([64, 628, 1216, 694], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((88, 644), "CLI tools", font=f_box, fill=TEXT)
    d.text((230, 646), "charger_demo  ·  charger_settle (CSV, dispute pack)  ·  doorstep", font=f_small, fill=MUTED)
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
    # arrows
    for xa, xb in [(282, 332), (522, 588), (778, 900)]:
        d.line([xa, 193, xb, 193], fill=BORDER, width=2)
        d.polygon([(xb - 8, 187), (xb - 8, 199), (xb, 193)], fill=BORDER)
    d.text((307, 152), "both streams fresh", font=f_small, fill=FAINT)
    d.text((543, 152), "status = CHARGING", font=f_small, fill=FAINT)
    d.text((817, 152), "fault latched", font=f_small, fill=FAINT)

    d.rounded_rectangle([92, 290, 1188, 320], radius=8, fill=PANEL_ALT, outline=BORDER, width=1)
    d.text((116, 296), "Entering CHARGING opens the session.  Leaving it closes the session with a stop reason.",
           font=f_body, fill=TEXT)

    # the gate itself
    d.rounded_rectangle([92, 350, 1188, 590], radius=12, fill=PANEL, outline=GREEN, width=2)
    d.text((118, 370), "billing_complete = all of", font=f_state, fill=GREEN)
    conds = [
        ("closed cleanly", "the state left CHARGING on purpose, not by a crash mid-session"),
        ("two or more samples", "a single accepted frame cannot define an interval, let alone its energy"),
        ("no integration gap", "any interval beyond max_integration_step adds no energy: it is never interpolated"),
        ("not cut by a timeout", "a COMMUNICATION_TIMEOUT stop yields a time_upper_bound, not a bill"),
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
    W, H = 1180, 620
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

    ty = 420
    track_x, track_w = x0, W - 2 * x0
    d.text((track_x, ty), "ingest throughput", font=f_label, fill=TEXT)
    d.text((track_x, ty + 22), "a 500 kbit/s CAN bus at 1 Hz delivers about 3500 frames/s",
           font=f_small, fill=FAINT)
    scale_lbl = "scale: 1.2 M frames/s"
    lw = d.textlength(scale_lbl, font=f_small)
    d.text((track_x + track_w - lw, ty + 22), scale_lbl, font=f_small, fill=FAINT)

    by1, by2 = ty + 48, ty + 70
    d.rounded_rectangle([track_x, by1, track_x + track_w, by2], radius=4, fill=PANEL_ALT)
    scale = track_w / 1200000.0
    d.rounded_rectangle([track_x, by1, track_x + int(1117482 * scale), by2], radius=4, fill=PURPLE)
    d.text((track_x, by2 + 10), "1 117 142 frames/s  (~895 ns per frame, Release build on the reference machine)",
           font=f_small, fill=PURPLE)

    d.text((track_x, ty + 108), "Throughput is not a constraint. Exactness is, and it is structural:",
           font=f_small, fill=MUTED)
    d.text((track_x, ty + 128), "the ledger holds integers, so no amount of accumulated rounding moves a Wh boundary.",
           font=f_small, fill=MUTED)
    return img, (W, H)


# ---------------------------------------------------------------- fleet kpi
def fleet_kpi(path):
    W, H = 1180, 560
    img, d = canvas(W, H, top_accent=PURPLE)
    f_h1 = font(28, "bold")
    f_head = font(16, "bold")
    f_body = font(15)
    f_small = font(13)

    d.text((64, 40), "What the operator sees", font=f_h1, fill=TEXT)
    d.text((64, 78), "A held session is invisible nowhere: the reason travels with it.", font=f_body, fill=MUTED)

    d.rounded_rectangle([64, 126, 604, 536], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((88, 146), "Per bay over the shift", font=f_head, fill=TEXT)
    d.text((88, 176), "asset      status       verdict       energy         cost", font=f_small, fill=FAINT)
    rows = [
        ("bay-04", "CHARGING", "billable", "118 Wh", "good", GREEN),
        ("bay-07", "CHARGING", "billable", "4 800 Wh", "good", GREEN),
        ("bay-09", "TIMEOUT", "held", "2 250 Wh", "time is an upper bound", AMBER),
        ("bay-12", "FAULT", "held", "1 380 Wh", "fault kept energy", AMBER),
        ("bay-15", "ORPHAN", "held", "unknown", "process died mid-session", RED),
    ]
    y = 204
    for asset, status, verdict, energy, note, colour in rows:
        d.text((88, y), asset, font=f_body, fill=TEXT)
        d.text((196, y), status, font=f_body, fill=BLUE)
        d.text((330, y), verdict, font=f_body, fill=colour)
        d.text((430, y), energy, font=f_body, fill=TEXT)
        d.text((512, y), note, font=f_small, fill=FAINT)
        y += 34

    d.rounded_rectangle([648, 126, W - 64, 536], radius=10, fill=PANEL, outline=BORDER, width=1)
    d.text((672, 146), "Site rollup (rebuilt from the journal alone)", font=f_head, fill=TEXT)
    kpis = [
        ("revenue", "1 245.30 EUR", TEXT),
        ("energy", "4 982 kWh", TEXT),
        ("invoiced now", "4 711 EUR (93.8%)", GREEN),
        ("held", "318 EUR across 41 sessions", AMBER),
        ("availability", "99.2 ppm availability, energy lower bounds excluded", MUTED),
        ("best bay", "bay-07, highest revenue per hour", TEXT),
    ]
    y = 192
    for k, v, c in kpis:
        d.text((672, y), k, font=f_body, fill=FAINT)
        d.text((800, y), v, font=f_body, fill=c)
        y += 48

    d.text((64, H - 40), "Every figure is an exact integer at the source; the report is a view of that.",
           font=f_small, fill=MUTED)
    return img, (W, H)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "docs"
    os.makedirs(out, exist_ok=True)
    jobs = [
        ("social-preview.png", social_preview, GREEN),
        ("architecture.png", architecture, BLUE),
        ("session-lifecycle.png", session_lifecycle, AMBER),
        ("exactness.png", exactness, BLUE),
        ("fleet-kpi.png", fleet_kpi, PURPLE),
    ]
    for name, fn, accent in jobs:
        target = os.path.join(out, name)
        img, (w, h) = fn(target)
        img.save(target)
        audit(target, w, h, accent)
        print("wrote and audited " + target)


if __name__ == "__main__":
    main()
