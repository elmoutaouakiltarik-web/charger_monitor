"""Generate the social preview image for the GitHub repository.

Kept in the repository so the asset is reproducible: change the text, run the
script, commit the result. No network access, no external assets.
"""

from PIL import Image, ImageDraw, ImageFont
import glob
import os
import sys

W, H = 1280, 640

BG = (13, 17, 23)
PANEL = (22, 27, 34)
ACCENT = (63, 185, 80)      # exact green
ACCENT_DIM = (45, 120, 58)
TEXT = (230, 237, 243)
MUTED = (139, 148, 158)
WARN = (210, 153, 34)


def load_font(size, bold=False):
    patterns = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold
        else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for p in patterns:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    for p in glob.glob("/usr/share/fonts/**/*.ttf", recursive=True):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    f_title = load_font(64, bold=True)
    f_sub = load_font(26)
    f_mono = load_font(22)
    f_small = load_font(20)
    f_label = load_font(18, bold=True)

    # Accent rule at the top.
    d.rectangle([0, 0, W, 10], fill=ACCENT)

    x = 80
    d.text((x, 70), "charger_monitor", font=f_title, fill=TEXT)
    d.text((x, 150), "Exact charging-session ledger for industrial DC charger bays",
           font=f_sub, fill=MUTED)

    # Left panel: the invariant.
    d.rounded_rectangle([x, 225, x + 520, 470], radius=12, fill=PANEL, outline=(45, 51, 59), width=2)
    d.text((x + 28, 250), "THE INVARIANT", font=f_label, fill=ACCENT)
    d.text((x + 28, 285), "480 V x 10 A x 1 h", font=f_mono, fill=TEXT)
    d.text((x + 28, 325), "=", font=f_mono, fill=MUTED)
    d.text((x + 28, 355), "exactly 4800 Wh", font=f_mono, fill=ACCENT)
    d.text((x + 28, 400), "integer microwatt-hours, no float drift", font=f_small, fill=MUTED)

    # Right panel: what it guarantees.
    rx = x + 560
    d.rounded_rectangle([rx, 225, W - 80, 470], radius=12, fill=PANEL, outline=(45, 51, 59), width=2)
    d.text((rx + 28, 250), "GUARANTEES", font=f_label, fill=ACCENT)

    items = [
        ("dropouts", "never become invented energy", ACCENT_DIM),
        ("restarts", "billing survives via hash-chained journal", ACCENT_DIM),
        ("invoices", "billing_complete gates automation", ACCENT_DIM),
        ("diagnostics", "advise, never block charging", ACCENT_DIM),
    ]
    y = 292
    for head, body, color in items:
        d.text((rx + 28, y), head, font=f_small, fill=TEXT)
        d.text((rx + 150, y), body, font=f_small, fill=color)
        y += 42

    # Footer strip.
    d.rectangle([0, H - 56, W, H], fill=(18, 23, 29))
    d.text((x, H - 40), "C++17  ·  zero dependencies  ·  Apache-2.0  ·  ~1.1M frames/s",
           font=f_small, fill=MUTED)

    out = sys.argv[1] if len(sys.argv) > 1 else "docs/social-preview.png"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    img.save(out)
    print("wrote " + out)


if __name__ == "__main__":
    main()