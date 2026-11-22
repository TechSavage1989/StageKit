#!/usr/bin/env python3
"""The small corner badges a key's image carries when it has both a type and
a function.

The owner, of the Manual Window / Source key: "all keys must have the
options". A key whose type list is set (src/key-types.ts) shows that type's
picture; what the key does (its step, tool, slide direction, aspect ratio,
dock option, kind of capture, blur style, destination) is then drawn as a
badge in the top right corner, in the pack's style: a little screen of the
key's own colours with the drawing in it, no words. The top left corner is
the on/off mark's (tools/make-state-icons.py); a Move Source or Move Scene
Item key, which has no on/off mark, carries its second badge there (a linked
source brought along, flipped or not; this scene or a chosen scene only).

The plugin lays a badge over the type's picture when it shows the key
(src/compose.ts): the type pictures and the badges are drawn here, the
combinations (too many to draw beforehand: some 80 types times 80 functions
times six palettes, on and off) are made when needed.

Writes imgs/pack/<palette>/badges/<name>.png (BADGE_W x BADGE_H), the list of
names in src/badges.json, and svg/badges/<palette>/<name>.svg (the same
drawing as shapes, through tools/svgdraw.py).
Run from the streamdeck folder, after make-option-icons.py:
    python3 tools/make-badges.py
"""
import importlib.util
import json
import os
import shutil
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
PACK = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin", "imgs", "pack")
LIST = os.path.join(ROOT, "src", "badges.json")
SVG = os.path.join(ROOT, "svg", "badges")
sys.path.insert(0, HERE)
from svgdraw import SvgDraw  # noqa: E402

_spec = importlib.util.spec_from_file_location("make_option_icons", os.path.join(HERE, "make-option-icons.py"))
OPTIONS = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(OPTIONS)
DRAWINGS = OPTIONS.DRAWINGS
arrow = OPTIONS.arrow

# A badge's size on a 144 px key, and where the plugin puts it (src/compose.ts
# has the same numbers): clear of the on/off mark and of most of the picture's
# own screen.
BADGE_W, BADGE_H = 54, 36
EDGE = (12, 12, 14, 255)
# The drawing's field (64 x 36, as every drawing of make-option-icons.py) is
# drawn at this scale inside the badge's screen, with a bolder pen than on a
# key: a badge is small.
FIELD = 0.68
PEN = 2.4
SS = 4  # drawn large and shrunk, for smooth edges

import math  # noqa: E402
_HEX = [(math.cos(math.radians(a)), math.sin(math.radians(a)) * 0.9) for a in range(30, 390, 60)]

# Drawings of badges alone: the steps the picture of a key had no drawing for,
# the blur styles (the pack's own are pictures, not drawings), the aspect
# ratio left to Auto Frame without its letter, the move's extra options.
OWN = {
    # Manual Window / Source: a switch (toggle), the next one (a play-to-bar),
    # back to automatic (an arrow turning back).
    "step-toggle": [("ring", 22, 18, 10), ("line", 22, 8, 42, 8), ("line", 22, 28, 42, 28), ("arc", 42, 18, 10, 270, 90),
                    ("dot", 42, 18, 6.5)],
    "step-next": [("tri", 14, 6, 14, 30, 38, 18), ("box", 42, 6, 49, 30)],
    "step-back": [("arc", 34, 18, 12, 270, 90), ("line", 34, 6, 18, 6), ("line", 34, 30, 22, 30), ("tri", 10, 6, 19, 0, 19, 12)],
    # Blur styles: a soft blot (gaussian), a square blot (box), rings of light
    # (dual kawase), rays from the middle (zoom), streaks (motion), squares
    # and hexagons (the mosaics).
    "blur-gaussian": [("dot", 32, 18, 6), ("ring", 32, 18, 11), ("ring", 32, 18, 16)],
    "blur-box": [("box", 26, 12, 38, 24), ("frame", 20, 6, 44, 30)],
    "blur-kawase": [("dot", 32, 18, 4), ("ring", 32, 18, 9), ("frame", 16, 3, 48, 33)],
    "blur-zoom": [("dot", 32, 18, 4)] + [("line", 32 + dx * 9, 18 + dy * 6, 32 + dx * 22, 18 + dy * 15) for dx, dy in
                                         ((1, 0), (-1, 0), (0, 1), (0, -1), (0.8, 0.8), (-0.8, 0.8), (0.8, -0.8), (-0.8, -0.8))],
    "blur-motion": [("box", 34, 10, 52, 26), ("line", 8, 12, 28, 12), ("line", 14, 18, 30, 18), ("line", 8, 24, 28, 24)],
    "blur-pixelate": [("box", x, y, x + 10, y + 9) for x, y in ((10, 4), (32, 4), (21, 14), (43, 14), (10, 24), (32, 24))],
    "blur-hexagons": [("line", cx + 9 * dx0, cy + 9 * dy0, cx + 9 * dx1, cy + 9 * dy1)
                      for cx, cy in ((22, 11), (42, 11), (32, 26))
                      for (dx0, dy0), (dx1, dy1) in zip(_HEX, _HEX[1:] + _HEX[:1])],
    # Set Aspect Ratio's Automatic: a frame with its corners pulled out (the
    # key's own picture writes an A in it; a badge has no words).
    "ratio-auto": [("frame", 16, 9, 48, 27)] + arrow(14, 7, 6, 2, 4) + arrow(50, 7, 58, 2, 4) + arrow(14, 29, 6, 34, 4) + arrow(50, 29, 58, 34, 4),
    # Move Source's extras: a linked source brought along (two boxes and a
    # link between them), the same flipped round the moved one (the two
    # swapped, arrows both ways), this scene only (one screen, on air) and
    # chosen scenes only (a stack with one picked out).
    "move-linked": [("box", 4, 10, 22, 26), ("frame", 42, 10, 60, 26), ("ring", 29, 18, 5), ("ring", 37, 18, 5)],
    "move-flip": [("box", 4, 10, 20, 26), ("frame", 44, 10, 60, 26), ("line", 32, 4, 32, 32)]
    + arrow(23, 13, 41, 13, 4) + arrow(41, 23, 23, 23, 4),
    "scene-this": [("frame", 10, 4, 54, 32), ("dot", 32, 18, 6)],
    "scene-chosen": [("frame", 6, 3, 40, 21), ("box", 15, 9, 49, 27), ("frame", 24, 15, 58, 33)],
    # The Stream and Record keys with a Scene type: what OBS is doing, still
    # (the key's own animated pictures are tools/make-live-icons.py's).
    "stream-off": [("line", 32, 15, 32, 33), ("line", 25, 34, 39, 34), ("ring", 32, 11, 4)],
    "stream-on": [("line", 32, 15, 32, 33), ("line", 25, 34, 39, 34), ("dot", 32, 11, 4), ("arc", 32, 11, 10, -45, 45),
                  ("arc", 32, 11, 10, 135, 225), ("arc", 32, 11, 17, -45, 45), ("arc", 32, 11, 17, 135, 225)],
    "record-off": [("ring", 32, 18, 14)],
    "record-on": [("ring", 32, 18, 15), ("dot", 32, 18, 10)],
    "record-paused": [("ring", 32, 18, 15), ("box", 25, 10, 30, 26), ("box", 34, 10, 39, 26)],
}

# The badges taken from the key pictures' own drawings: what a key's picture
# shows when it has no type.
TAKEN = (
    # Manual Window / Source: hide, clear.
    ["hide", "clear"]
    # OBS Stats reset.
    + ["reset-stats"]
    # Studio Mode's state and its momentary transition.
    + ["studio-off", "studio-on", "studio-transition"]
    # Arrange tools.
    + ["align-left", "align-center", "align-right", "align-top", "align-middle", "align-bottom", "space-across", "space-down",
       "snap-now", "same-width", "same-height", "same-size", "copy-all", "copy-chosen", "fit-space", "plain-tool"]
    # Dock options.
    + ["snap", "guides", "limit", "slide-to-edge", "every-scene", "keep-shape", "plain-switch"]
    # Slide to Edge.
    + ["slide-" + w for w in ("up-left", "up", "up-right", "left", "centre", "right", "down-left", "down", "down-right")] + ["plain-move"]
    # Set Aspect Ratio.
    + ["ratio-" + r.replace(":", "x") for r in OPTIONS.RATIOS]
    # Capture Active Window.
    + ["hook-game", "hook-window"]
    # Blur styles the pack has no picture of, drawn by make-option-icons.py.
    + ["style-trail", "style-mosaic-circle", "style-mosaic-triangle", "style-mosaic-voronoi", "style-mosaic-rhomboid",
       "style-mosaic-triakis", "style-glass"]
    # Move Source / Move Scene Item destinations.
    + ["camera-" + p for p in ("top-left", "top-centre", "top-right", "middle-left", "centre", "middle-right", "bottom-left",
                                "bottom-centre", "bottom-right")]
    + ["spot", "screen-spot"]
)

BADGES = {name: DRAWINGS[name] for name in TAKEN}
# (Each in the middle of its badge, as every drawing is on a key.)
BADGES.update({name: OPTIONS.centred(shapes) for name, shapes in OWN.items()})


def paint(d, shapes, ink, ox, oy, k, pen):
    """Draws a 64 x 36 drawing with d (Pillow's ImageDraw or svgdraw's SvgDraw)."""

    def at(x, y):
        return (ox + x * k, oy + y * k)

    def box(x0, y0, x1, y1):
        return (*at(x0, y0), *at(x1, y1))

    for shape in shapes:
        kind = shape[0]
        if kind == "box":
            d.rectangle(box(*shape[1:5]), fill=ink)
        elif kind == "frame":
            d.rectangle(box(*shape[1:5]), outline=ink, width=pen)
        elif kind == "line":
            d.line(box(*shape[1:5]), fill=ink, width=pen)
        elif kind == "ring":
            cx, cy, r = shape[1:]
            d.ellipse(box(cx - r, cy - r, cx + r, cy + r), outline=ink, width=pen)
        elif kind == "dot":
            cx, cy, r = shape[1:]
            d.ellipse(box(cx - r, cy - r, cx + r, cy + r), fill=ink)
        elif kind == "arc":
            cx, cy, r, start, end = shape[1:]
            d.arc(box(cx - r, cy - r, cx + r, cy + r), start, end, fill=ink, width=pen)
        elif kind == "tri":
            d.polygon([at(shape[1], shape[2]), at(shape[3], shape[4]), at(shape[5], shape[6])], fill=ink)
        else:
            raise ValueError(f"a badge has no words: {shape}")


def badge(d, shapes, ink, inside, s):
    """One badge at scale s: a dark edge, the pack's little screen (its
    inside colour, framed in ink) and the drawing."""
    d.rounded_rectangle((0, 0, BADGE_W * s - 1, BADGE_H * s - 1), radius=7 * s, fill=EDGE)
    d.rounded_rectangle((2 * s, 2 * s, (BADGE_W - 2) * s - 1, (BADGE_H - 2) * s - 1), radius=6 * s, fill=inside, outline=ink, width=2 * s)
    k = FIELD * s
    ox, oy = (BADGE_W * s - 64 * k) / 2, (BADGE_H * s - 36 * k) / 2
    paint(d, shapes, ink, ox, oy, k, PEN * s if isinstance(d, SvgDraw) else int(round(PEN * s)))


def colours(palette):
    """The palette's colours, as make-option-icons.py takes them."""
    base = Image.open(os.path.join(PACK, palette, "filters", "next-capture.png")).convert("RGBA")
    inside = base.getpixel((30, 60))
    ink = max((base.getpixel((x, y)) for x in range(40, 90) for y in range(36, 62)), key=lambda c: sum(c[:3]))
    return inside, ink


def main():
    palettes = json.load(open(os.path.join(ROOT, "src", "pack.json"), encoding="utf-8"))["colours"]
    shutil.rmtree(SVG, ignore_errors=True)
    made = 0
    for palette in palettes:
        inside, ink = colours(palette)
        out = os.path.join(PACK, palette, "badges")
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        os.makedirs(os.path.join(SVG, palette))
        for name, shapes in BADGES.items():
            big = Image.new("RGBA", (BADGE_W * SS, BADGE_H * SS), (0, 0, 0, 0))
            badge(ImageDraw.Draw(big), shapes, ink, inside, SS)
            big.resize((BADGE_W, BADGE_H), Image.LANCZOS).save(os.path.join(out, name + ".png"), optimize=True)
            d = SvgDraw()
            badge(d, shapes, ink, inside, 1)
            with open(os.path.join(SVG, palette, name + ".svg"), "w", encoding="utf-8") as f:
                f.write(
                    f'<svg xmlns="http://www.w3.org/2000/svg" width="{BADGE_W}" height="{BADGE_H}" viewBox="0 0 {BADGE_W} {BADGE_H}">\n'
                    + d.body()
                    + "\n</svg>\n"
                )
            made += 1
    with open(LIST, "w", encoding="utf-8") as f:
        json.dump({"_": "The corner badges of tools/make-badges.py, in imgs/pack/<palette>/badges/. Written by that tool.",
                   "width": BADGE_W, "height": BADGE_H, "badges": sorted(BADGES)}, f, indent="\t")
        f.write("\n")
    print(f"{made} badges drawn ({len(BADGES)} in each of {len(palettes)} palettes)")


if __name__ == "__main__":
    main()
