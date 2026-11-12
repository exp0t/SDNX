#!/usr/bin/env python3
"""Draw Switch-style button glyphs for Sleeping Dogs' UI textures (BUTTON_* in UI/SeekFreeBoot.big).

All shapes and letters are drawn here (a tiny stroke font, no font file and no copied artwork).

Mapping of the game's texture names (Xbox meaning in brackets) to the Switch label:
  default = by position (the NRO sets SDK input_xbox_layout = true: the Switch button in the Xbox button's place
  does its job), so the glyph names the Switch button in that place:
    BUTTON_ACCEPT [A, bottom] -> B   BUTTON_BACK [B, right] -> A   BUTTON_BUTTON1 [X, left] -> Y
    BUTTON_BUTTON2 [Y, top] -> X
  --by-letter (input_xbox_layout = false): A -> A, B -> B, X -> X, Y -> Y
  always: L1 [LB] -> L, R1 [RB] -> R, L2 [LT] -> ZL, R2 [RT] -> ZR, SELECT [Back] -> -, START -> +,
          L1R1 -> L R, L2R2 -> ZL ZR.  Sticks, stick clicks and the D-pad keep the original pictures.

Usage: make_switch_buttons.py out_dir [--by-letter]     (writes PNGs + a contact sheet)
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SS = 8  # supersampling

# ---------------------------------------------------------------------------- stroke font (unit box, y down)


def _arc(cx, cy, rx, ry, a0, a1, n=24):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


GLYPHS = {
    "A": [[(0.08, 1.0), (0.5, 0.0), (0.92, 1.0)], [(0.24, 0.66), (0.76, 0.66)]],
    "B": [[(0.16, 0.0), (0.16, 1.0)],
          [(0.16, 0.0), (0.55, 0.0)] + _arc(0.55, 0.24, 0.26, 0.24, -90, 90) + [(0.16, 0.48)],
          [(0.16, 0.48), (0.58, 0.48)] + _arc(0.58, 0.74, 0.28, 0.26, -90, 90) + [(0.16, 1.0)]],
    "X": [[(0.1, 0.0), (0.9, 1.0)], [(0.9, 0.0), (0.1, 1.0)]],
    "Y": [[(0.08, 0.0), (0.5, 0.52), (0.92, 0.0)], [(0.5, 0.52), (0.5, 1.0)]],
    "L": [[(0.2, 0.0), (0.2, 1.0), (0.86, 1.0)]],
    "R": [[(0.16, 1.0), (0.16, 0.0), (0.55, 0.0)] + _arc(0.55, 0.25, 0.27, 0.25, -90, 90) + [(0.16, 0.5)],
          [(0.46, 0.5), (0.86, 1.0)]],
    "Z": [[(0.12, 0.0), (0.88, 0.0), (0.12, 1.0), (0.88, 1.0)]],
    "+": [[(0.5, 0.0), (0.5, 1.0)], [(0.0, 0.5), (1.0, 0.5)]],
    "-": [[(0.0, 0.5), (1.0, 0.5)]],
}
ADVANCE = {"A": 1.0, "B": 0.95, "X": 1.0, "Y": 1.0, "L": 0.9, "R": 0.95, "Z": 1.0, "+": 1.0, "-": 1.0}


def draw_text(d: ImageDraw.ImageDraw, text, cx, cy, h, width, fill):
    """Centre a stroke string at (cx, cy); h = cap height in pixels (supersampled)."""
    gap = 0.18
    total = sum(ADVANCE[c] for c in text) + gap * (len(text) - 1)
    x = cx - total * h / 2
    for c in text:
        for stroke in GLYPHS[c]:
            pts = [(x + px * h, cy - h / 2 + py * h) for px, py in stroke]
            d.line(pts, fill=fill, width=int(width), joint="curve")
            r = width / 2
            for px, py in (pts[0], pts[-1]):
                d.ellipse([px - r, py - r, px + r, py + r], fill=fill)
        x += (ADVANCE[c] + gap) * h


# ---------------------------------------------------------------------------- shapes

BODY = (46, 46, 48, 255)
BODY_HI = (88, 88, 92, 255)
RIM = (20, 20, 22, 255)
LABEL = (244, 244, 244, 255)
HOLD = (232, 62, 40, 255)  # the originals mark "hold" with a red ring; keep the idea


def _canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def _finish(img, w, h, shadow=True):
    big = img
    if shadow:
        a = np.array(big)[:, :, 3]
        sh = Image.fromarray((a * 0.55).astype(np.uint8), "L").filter(ImageFilter.GaussianBlur(2.2 * SS))
        base = Image.new("RGBA", big.size, (0, 0, 0, 0))
        black = Image.new("RGBA", big.size, (0, 0, 0, 255))
        base.paste(black, (int(0.8 * SS), int(1.6 * SS)), sh)
        base.alpha_composite(big)
        big = base
    return big.resize((w, h), Image.LANCZOS)


def round_button(label, size=64, hold=False):
    img = _canvas(size, size)
    d = ImageDraw.Draw(img)
    s = SS
    c = size * s / 2
    r = size * s * 0.40
    if hold:
        rr = size * s * 0.475
        d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=HOLD)
        r = size * s * 0.39
    d.ellipse([c - r, c - r, c + r, c + r], fill=RIM)
    r2 = r - 1.6 * s
    d.ellipse([c - r2, c - r2, c + r2, c + r2], fill=BODY)
    # soft top highlight
    hl = _canvas(size, size)
    hd = ImageDraw.Draw(hl)
    hd.ellipse([c - r2 * 0.8, c - r2 * 0.95, c + r2 * 0.8, c + r2 * 0.1], fill=(255, 255, 255, 34))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(3 * s)))
    if label in ("+", "-"):
        draw_text(d, label, c, c, r * 0.95, 4.2 * s, LABEL)
    else:
        draw_text(d, label, c, c + 0.2 * s, r * 0.92, 4.6 * s, LABEL)
    return _finish(img, size, size)


def shoulder(label, w, h, trigger, box=None):
    """Rounded shoulder (L/R) or taller trigger (ZL/ZR) plate inside box=(x0, y0, x1, y1) of a w x h image."""
    img = _canvas(w, h)
    _shoulder_on(img, label, w, h, trigger, box or (0, 0, w, h))
    return _finish(img, w, h)


def _shoulder_on(img, label, w, h, trigger, box):
    d = ImageDraw.Draw(img)
    s = SS
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    if trigger:
        pw, ph = bw * 0.62, bh * 0.86
    else:
        pw, ph = bw * 0.90, bh * 0.56
    cx, cy = (x0 + bw / 2) * s, (y0 + bh / 2) * s
    rect = [cx - pw * s / 2, cy - ph * s / 2, cx + pw * s / 2, cy + ph * s / 2]
    rad = (ph if not trigger else pw) * s * 0.30
    d.rounded_rectangle(rect, radius=rad, fill=RIM)
    inner = [rect[0] + 1.6 * s, rect[1] + 1.6 * s, rect[2] - 1.6 * s, rect[3] - 1.6 * s]
    d.rounded_rectangle(inner, radius=max(1, rad - 1.6 * s), fill=BODY)
    hl = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(hl).rounded_rectangle([inner[0], inner[1], inner[2], (inner[1] + inner[3]) / 2],
                                         radius=max(1, rad - 1.6 * s), fill=(255, 255, 255, 30))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(2 * s)))
    if len(label) == 1:
        th = min(ph * 0.52, pw * 0.5)
    else:
        th = min(ph * 0.36, pw * 0.36)
    draw_text(d, label, cx, cy, th * s, 4.2 * s, LABEL)


def pair(labels, w, h, trigger):
    img = _canvas(w, h)
    _shoulder_on(img, labels[0], w, h, trigger, (0, 0, w / 2, h))
    _shoulder_on(img, labels[1], w, h, trigger, (w / 2, 0, w, h))
    return _finish(img, w, h)


# ---------------------------------------------------------------------------- set

def build(by_letter=False):
    face = {"BUTTON_ACCEPT": "B", "BUTTON_BACK": "A", "BUTTON_BUTTON1": "Y", "BUTTON_BUTTON2": "X"}
    if by_letter:
        face = {"BUTTON_ACCEPT": "A", "BUTTON_BACK": "B", "BUTTON_BUTTON1": "X", "BUTTON_BUTTON2": "Y"}
    out = {}
    for name, lab in face.items():
        out[name] = round_button(lab)
    out["BUTTON_BACK_HOLD"] = round_button(face["BUTTON_BACK"], hold=True)
    out["BUTTON_BUTTON1_HOLD"] = round_button(face["BUTTON_BUTTON1"], hold=True)
    out["BUTTON_BUTTON2_HOLD"] = round_button(face["BUTTON_BUTTON2"], hold=True)
    out["BUTTON_L1"] = shoulder("L", 64, 64, False)
    out["BUTTON_R1"] = shoulder("R", 64, 64, False)
    out["BUTTON_L2"] = shoulder("ZL", 64, 64, True)
    out["BUTTON_R2"] = shoulder("ZR", 64, 64, True)
    out["BUTTON_SELECT"] = round_button("-")
    out["BUTTON_START"] = round_button("+")
    out["BUTTON_L1R1"] = pair(("L", "R"), 128, 64, False)
    out["BUTTON_L2R2"] = pair(("ZL", "ZR"), 128, 64, True)
    return {k: np.array(v.convert("RGBA")) for k, v in out.items()}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    imgs = build("--by-letter" in sys.argv)
    for k, v in imgs.items():
        Image.fromarray(v, "RGBA").save(os.path.join(outdir, k + ".png"))
    cols, cell = 6, 136
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell, rows * cell), (90, 90, 90, 255))
    for i, (k, v) in enumerate(imgs.items()):
        sheet.alpha_composite(Image.fromarray(v, "RGBA"), ((i % cols) * cell + 4, (i // cols) * cell + 4))
    sheet.save(os.path.join(outdir, "_sheet.png"))
    print("%d glyphs -> %s" % (len(imgs), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
