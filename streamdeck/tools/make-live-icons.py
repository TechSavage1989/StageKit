#!/usr/bin/env python3
"""The Stream and Record keys' pictures: one still picture or a short loop of
frames for each state OBS reports, in every palette (needs Pillow).

The owner: "Id like animated stream Start buttons and stream Record buttons
... have animations that are relevant, and match the existing color themes".
Each frame is the pack's framed screen (as tools/make-option-icons.py draws
its options) with a drawing in it, no words:

  Stream   idle          an antenna, still
           starting      its waves appearing one by one
           live          waves pulsing out from its tip
           reconnecting  the waves flickering
           stopping      the waves going one by one
  Record   idle          a hollow record dot beside a still filmstrip
           starting      the dot filling, a ring of light turning round it
           recording     the dot pulsing, the filmstrip running past
           paused        pause bars pulsing slowly in the dot
           stopping      the dot emptying, the ring turning back

The record dot is red in every palette; the waves, the ring of light and the
pause bars are the palette's accent (the brightest strong colour of the
pack's own key frame); the rest is the palette's ink, as on every other key.

Writes imgs/pack/<palette>/live/<key>-<state>-<n>.png (a still state has
one frame, 0), the marked copies tools/make-state-icons.py would make under
status/on/ and status/off/ (with its own `marked`), and adds each frame to
src/pack.json in the group "live" (on and off are the same file: whether the
key is on is its mark). The frame counts are what src/live.ts plays; a test
checks the two agree. tools/make-svgs.py writes the same drawings as SVG
(`svg_frame` here). Run from the streamdeck folder, after
make-option-icons.py:
    python3 tools/make-live-icons.py
"""
import colorsys
import importlib.util
import json
import math
import os
import shutil

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
PACK = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin", "imgs", "pack")
LIST = os.path.join(ROOT, "src", "pack.json")
# What src/live.ts reads: the frames of each state and each palette's accent
# (the colour a long press's bar fills in).
LIVE = os.path.join(ROOT, "src", "live.json")
GROUP = "live"


def _load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OPTIONS = _load("make-option-icons")
STATE = _load("make-state-icons")
SCREEN, SCALE = OPTIONS.SCREEN, OPTIONS.SCALE
PEN = 2.6
# The record dot's red: the same in every palette.
RED = (232, 54, 54)

# Frames of each state (still states: 1). src/live.ts has the same numbers
# and plays them at its own rate (the paused loop is longer: a slow pulse).
FRAMES = {
    "stream": {"idle": 1, "starting": 8, "live": 8, "reconnecting": 8, "stopping": 8},
    "record": {"idle": 1, "starting": 8, "recording": 8, "paused": 12, "stopping": 8},
}

# --------------------------------------------------------------- drawings
# Shapes on the 64 x 36 field of make-option-icons.py, each with its colour
# ("ink", "accent" or "red") and how opaque it is (0 to 1):
#   ("line", x0, y0, x1, y1, c, a)   ("ring", cx, cy, r, c, a)
#   ("dot", cx, cy, r, c, a)         ("box", x0, y0, x1, y1, c, a)
#   ("arc", cx, cy, r, start, end, c, a)   (degrees clockwise from the right)

TIP = (32, 15)  # the antenna's tip, where its waves come from
WAVES = (7, 12, 17)  # the three waves' radii


def mast(tip_fill, colour="ink"):
    """The antenna: a mast on a foot, and its tip. (Not two legs and a
    crossbar: drawn that small, that reads as the letter A.)"""
    x, y = TIP
    shapes = [("line", x, y + 3, x, 33, "ink", 1), ("line", x - 6, 34, x + 6, 34, "ink", 1)]
    shapes.append(("dot", x, y, 3.2, colour, 1) if tip_fill else ("ring", x, y, 3.4, colour, 1))
    return shapes


def wave(r, colour, alpha):
    """One wave each side of the tip."""
    x, y = TIP
    return [("arc", x, y, r, -45, 45, colour, alpha), ("arc", x, y, r, 135, 225, colour, alpha)]


def stream(state, f, n):
    if state == "idle":
        return mast(False) + wave(WAVES[0], "ink", 0.45)
    if state == "live":
        shapes = mast(True, "accent")
        for k in range(3):
            t = (f / n + k / 3) % 1  # each wave's way out, 0 at the tip
            r = 5 + 13 * t
            alpha = (1 - t * 0.8) * min(1, t / 0.15)
            shapes += wave(r, "accent", alpha)
        return shapes
    if state in ("starting", "stopping"):
        # Two frames for each of none, one, two and three waves.
        shown = f // 2 if state == "starting" else 3 - f // 2
        shapes = mast(f % 2 == 0, "accent")
        for k in range(shown):
            fresh = state == "starting" and k == shown - 1 and f % 2 == 0
            shapes += wave(WAVES[k], "accent", 0.5 if fresh else (0.95 if state == "starting" else 0.6))
        return shapes
    if state == "reconnecting":
        pattern = ((1, 0.25, 0.9), (0.2, 1, 0.15), (0.9, 0.1, 1), (0.15, 0.85, 0.3), (1, 0.3, 0.1), (0.25, 0.1, 0.95), (0.1, 1, 0.2), (0.85, 0.2, 0.7))
        shapes = mast(f % 2 == 0, "accent")
        for k in range(3):
            shapes += wave(WAVES[k], "accent", pattern[f % len(pattern)][k])
        return shapes
    raise ValueError(state)


DOT = (16, 18)  # the record dot's centre
DOT_R = 10
STRIP = (31, 61, 9, 27)  # the filmstrip: x0, x1, top, bottom
PITCH = 8  # one picture of the filmstrip


def filmstrip(offset, alpha):
    """A strip of film running left, `offset` px along one picture."""
    x0, x1, top, bottom = STRIP
    shapes = [("line", x0, top, x1, top, "ink", alpha), ("line", x0, bottom, x1, bottom, "ink", alpha)]
    x = x0 + 1.5 - offset
    while x < x1:
        a, b = max(x, x0 + 1), min(x + 5, x1 - 1)
        if b - a > 0.6:
            shapes.append(("box", a, top + 4, b, bottom - 4, "ink", alpha))
        x += PITCH
    return shapes


def spinner(f, n, backwards=False):
    """A ring of light turning round the record dot."""
    turn = (f / n) * 360 * (-1 if backwards else 1)
    return [("arc", DOT[0], DOT[1], DOT_R + 3.6, turn - 50, turn + 50, "accent", 1),
            ("arc", DOT[0], DOT[1], DOT_R + 3.6, turn + 130, turn + 230, "accent", 0.45)]


def record(state, f, n):
    cx, cy = DOT
    if state == "idle":
        return [("ring", cx, cy, DOT_R, "red", 1)] + filmstrip(0, 0.55)
    if state == "recording":
        pulse = 0.5 - 0.5 * math.cos(2 * math.pi * f / n)  # 0 to 1 and back
        return ([("ring", cx, cy, DOT_R + 3.4, "red", 0.15 + 0.45 * pulse), ("dot", cx, cy, 7.4 + 1.8 * pulse, "red", 1)]
                + filmstrip(PITCH * f / n, 1))
    if state == "paused":
        pulse = 0.5 - 0.5 * math.cos(2 * math.pi * f / n)
        bars = 0.35 + 0.65 * pulse
        return ([("ring", cx, cy, DOT_R, "red", 1), ("box", cx - 4.6, cy - 5, cx - 1.4, cy + 5, "accent", bars),
                 ("box", cx + 1.4, cy - 5, cx + 4.6, cy + 5, "accent", bars)] + filmstrip(0, 0.7))
    if state in ("starting", "stopping"):
        fill = (f + 1) / n if state == "starting" else 1 - (f + 1) / n
        shapes = [("ring", cx, cy, DOT_R, "red", 1)] + spinner(f, n, state == "stopping")
        if fill > 0.05:
            shapes.append(("dot", cx, cy, 1 + 6.4 * fill, "red", 1))
        return shapes + filmstrip(PITCH * f / n if state == "starting" else 0, 0.7)
    raise ValueError(state)


def shapes_of(key, state, f):
    n = FRAMES[key][state]
    return (stream if key == "stream" else record)(state, f, n)


# --------------------------------------------------------------- painting
def colours(palette):
    """The palette's key background (the pack's key with its little screen
    emptied), its ink (as make-option-icons.py takes it) and its accent."""
    base = Image.open(os.path.join(PACK, palette, "filters", "next-capture.png")).convert("RGBA")
    inside = base.getpixel((30, 60))
    ink = max((base.getpixel((x, y)) for x in range(40, 90) for y in range(36, 62)), key=lambda c: sum(c[:3]))
    ImageDraw.Draw(base).rectangle(OPTIONS.ERASE, fill=inside)
    return base, ink[:3], accent(palette, ink[:3])


def accent(palette, ink):
    """The strongest colour of the pack's key frame (outside the little
    screen), brought up to a bright enough light; mono's frame is grey, so
    its accent is its ink."""
    image = Image.open(os.path.join(PACK, palette, "filters", "next-capture.png")).convert("RGB")
    best, score = None, 0.0
    for y in range(4, 92, 2):
        for x in range(2, 142, 2):
            if 12 <= x <= 131 and 15 <= y <= 83:
                continue
            r, g, b = (c / 255 for c in image.getpixel((x, y)))
            h, l, s = colorsys.rgb_to_hls(r, g, b)
            if l < 0.2:
                continue
            if s * min(l, 1 - l) * 2 > score:
                best, score = (h, l, s), s * min(l, 1 - l) * 2
    if best is None or score < 0.25:
        return ink
    h, l, s = best
    r, g, b = colorsys.hls_to_rgb(h, max(l, 0.62), max(s, 0.85))
    return (round(r * 255), round(g * 255), round(b * 255))


def paint(d, shapes, palette_colours, ox, oy, k, pen):
    """Draws the shapes with d (Pillow's ImageDraw on a layer of its own per
    shape, or svgdraw's SvgDraw), the field's corner at (ox, oy), k px a unit."""
    ink, acc = palette_colours

    def at(x, y):
        return (ox + x * k, oy + y * k)

    def colour(c, a):
        rgb = {"ink": ink, "accent": acc, "red": RED}[c]
        return (*rgb, max(0, min(255, round(255 * a))))

    for shape in shapes:
        kind = shape[0]
        if kind == "line":
            x0, y0, x1, y1, c, a = shape[1:]
            d.line((*at(x0, y0), *at(x1, y1)), fill=colour(c, a), width=pen)
        elif kind == "box":
            x0, y0, x1, y1, c, a = shape[1:]
            d.rectangle((*at(x0, y0), *at(x1, y1)), fill=colour(c, a))
        elif kind in ("ring", "dot"):
            cx, cy, r, c, a = shape[1:]
            box = (*at(cx - r, cy - r), *at(cx + r, cy + r))
            if kind == "dot":
                d.ellipse(box, fill=colour(c, a))
            else:
                d.ellipse(box, outline=colour(c, a), width=pen)
        elif kind == "arc":
            cx, cy, r, start, end, c, a = shape[1:]
            d.arc((*at(cx - r, cy - r), *at(cx + r, cy + r)), start, end, fill=colour(c, a), width=pen)
        else:
            raise ValueError(shape)


def frame_image(base, palette_colours, shapes):
    """One frame: the key background with the drawing in its little screen,
    each shape laid over the ones before (so a faint wave over the mast is
    see-through, not a hole)."""
    s = SCALE
    w, h = (SCREEN[2] - SCREEN[0]) * s, (SCREEN[3] - SCREEN[1]) * s
    ox, oy = (w - 64 * s) / 2, (h - 36 * s) / 2
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for shape in shapes:
        one = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        paint(ImageDraw.Draw(one), [shape], palette_colours, ox, oy, s, int(PEN * s))
        layer.alpha_composite(one)
    picture = base.copy()
    picture.alpha_composite(layer.resize((SCREEN[2] - SCREEN[0], SCREEN[3] - SCREEN[1]), Image.LANCZOS), (SCREEN[0], SCREEN[1]))
    return picture


def svg_frame(palette_colours, shapes):
    """The same drawing as SVG shapes (tools/svgdraw.py), where it sits on the key."""
    import sys

    sys.path.insert(0, HERE)
    from svgdraw import SvgDraw

    d = SvgDraw()
    ox = SCREEN[0] + ((SCREEN[2] - SCREEN[0]) - 64) / 2
    oy = SCREEN[1] + ((SCREEN[3] - SCREEN[1]) - 36) / 2
    paint(d, shapes, palette_colours, ox, oy, 1, int(PEN * SCALE) / SCALE)
    return d.body()


def names():
    """Every frame's name: <key>-<state>-<n>."""
    return [(key, state, f, f"{key}-{state}-{f}") for key, states in FRAMES.items() for state, n in states.items() for f in range(n)]


def small(image):
    # (256 colours, as the marked copies: a quarter of the size, and no different to see.)
    return image.quantize(colors=256, method=Image.Quantize.FASTOCTREE)


def main():
    listed = json.load(open(LIST, encoding="utf-8"))
    made = 0
    accents = {}
    for palette in listed["colours"]:
        base, ink, acc = colours(palette)
        accents[palette] = list(acc)
        out = os.path.join(PACK, palette, GROUP)
        shutil.rmtree(out, ignore_errors=True)
        for mark in ("on", "off"):
            shutil.rmtree(os.path.join(PACK, palette, "status", mark, GROUP), ignore_errors=True)
            os.makedirs(os.path.join(PACK, palette, "status", mark, GROUP))
        os.makedirs(out)
        for key, state, f, name in names():
            picture = frame_image(base, (ink, acc), shapes_of(key, state, f))
            small(picture).save(os.path.join(out, name + ".png"), optimize=True)
            for mark, on in (("on", True), ("off", False)):
                small(STATE.marked(picture, on, STATE.mean_light(picture))).save(
                    os.path.join(PACK, palette, "status", mark, GROUP, name + ".png"), optimize=True)
            made += 3
    pictures = [p for p in listed["pictures"] if p["group"] != GROUP]
    for key, state, f, name in names():
        pictures.append({"id": f"{GROUP}/{name}", "group": GROUP, "label": f"{key} {state} {f}", "on": f"{GROUP}/{name}", "off": f"{GROUP}/{name}"})
    listed["pictures"] = pictures
    with open(LIST, "w", encoding="utf-8") as fh:
        json.dump(listed, fh, indent="\t", ensure_ascii=False)
        fh.write("\n")
    with open(LIVE, "w", encoding="utf-8") as fh:
        json.dump({"_": "Written by tools/make-live-icons.py: the frames of each state of the Stream and Record keys, and each palette's accent.",
                   "group": GROUP, "frames": FRAMES, "accents": accents}, fh, indent="\t")
        fh.write("\n")
    print(f"{made} pictures drawn: {len(names())} frames in each of {len(listed['colours'])} palettes, plain and marked on and off")


if __name__ == "__main__":
    main()
