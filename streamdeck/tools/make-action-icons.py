#!/usr/bin/env python3
"""Draws each key's own icon for Stream Deck's list of actions (needs Pillow).

Stream Deck shows these small pictures beside each key's name in its list of
actions: white line drawings on nothing, 20 px and 40 px (@2x). Every key
that is offered has one of its own, so the keys can be told apart in the list
(the owner: "I'd like all of the stream deck buttons to get specific icons").
They go to imgs/actions/<key>/icon.png and icon@2x.png, where <key> is the
last part of the action's UUID. Run from the streamdeck folder:
    python3 tools/make-action-icons.py
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "com.techsavage1989.stagekit.sdPlugin", "imgs", "actions")
S = 160  # drawn at four times 40 px and shrunk, for smooth edges
W = 13  # line width at that size: about 3 px at 40, 1.5 at 20
INK = (255, 255, 255, 255)


def p(x, y):
    return (x * S, y * S)


def box(d, x0, y0, x1, y1, r=0.06, fill=False, width=W):
    if fill:
        d.rounded_rectangle((*p(x0, y0), *p(x1, y1)), radius=r * S, fill=INK)
    else:
        d.rounded_rectangle((*p(x0, y0), *p(x1, y1)), radius=r * S, outline=INK, width=width)


def line(d, x0, y0, x1, y1, width=W):
    d.line((*p(x0, y0), *p(x1, y1)), fill=INK, width=width)
    for x, y in ((x0, y0), (x1, y1)):
        r = width / 2
        d.ellipse((x * S - r, y * S - r, x * S + r, y * S + r), fill=INK)


def arrow(d, x0, y0, x1, y1, head=0.13, width=W):
    a = math.atan2(y1 - y0, x1 - x0)
    tip = p(x1, y1)
    left = (tip[0] - head * S * math.cos(a - 0.55), tip[1] - head * S * math.sin(a - 0.55))
    right = (tip[0] - head * S * math.cos(a + 0.55), tip[1] - head * S * math.sin(a + 0.55))
    back = (x1 - head * 0.6 * math.cos(a), y1 - head * 0.6 * math.sin(a))
    line(d, x0, y0, back[0], back[1], width)
    d.polygon([tip, left, right], fill=INK)


def ring(d, cx, cy, r, fill=False, width=W):
    xy = (*p(cx - r, cy - r), *p(cx + r, cy + r))
    if fill:
        d.ellipse(xy, fill=INK)
    else:
        d.ellipse(xy, outline=INK, width=width)


def eye(d, cx=0.5, cy=0.5, w=0.42, h=0.24, width=W):
    """An almond outline with a round pupil."""
    pts = []
    for i in range(41):
        t = -1 + i / 20
        pts.append(p(cx + t * w, cy - h * (1 - t * t)))
    for i in range(41):
        t = 1 - i / 20
        pts.append(p(cx + t * w, cy + h * (1 - t * t)))
    d.line(pts + [pts[0]], fill=INK, width=width, joint="curve")
    ring(d, cx, cy, h * 0.62, fill=True)


def person(d, x0, y0, x1, y1):
    """The camera: a person in a frame."""
    box(d, x0, y0, x1, y1, r=0.05, width=W - 3)
    cx, w, h = (x0 + x1) / 2, x1 - x0, y1 - y0
    ring(d, cx, y0 + h * 0.38, min(w, h) * 0.16, fill=True)
    d.pieslice((*p(cx - w * 0.3, y0 + h * 0.6), *p(cx + w * 0.3, y1 + h * 0.35)), 180, 360, fill=INK)


def blurred(draw_fn, radius=10):
    """Draws, then softens: for the blur keys."""
    def go(d, image):
        layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
        draw_fn(ImageDraw.Draw(layer))
        alpha = layer.split()[3].filter(ImageFilter.GaussianBlur(radius))
        alpha = alpha.point(lambda v: min(255, int(v * 1.5)))
        layer.putalpha(alpha)
        image.alpha_composite(layer)
    return go


def bars(d, x0, y0, y1, lengths):
    n = len(lengths)
    step = (y1 - y0) / n
    for i, length in enumerate(lengths):
        box(d, x0, y0 + step * i + 0.03, x0 + length, y0 + step * (i + 1) - 0.03, r=0.03, fill=True)


ICONS = {
    # One for each feature's switch.
    "blur": blurred(lambda d: (box(d, 0.14, 0.24, 0.86, 0.76, r=0.12, fill=True)), 14),
    "shadow": lambda d: (box(d, 0.28, 0.3, 0.9, 0.86, fill=True), box(d, 0.1, 0.12, 0.7, 0.68, r=0.06, fill=False)),
    "corners": lambda d: (d.arc((*p(0.12, 0.12), *p(0.62, 0.62)), 180, 270, fill=INK, width=W), line(d, 0.12, 0.37, 0.12, 0.88), line(d, 0.37, 0.12, 0.88, 0.12), line(d, 0.12, 0.88, 0.88, 0.88), line(d, 0.88, 0.12, 0.88, 0.88)),
    "zoom": lambda d: (ring(d, 0.42, 0.42, 0.28), line(d, 0.62, 0.62, 0.88, 0.88, W + 4), line(d, 0.3, 0.42, 0.54, 0.42), line(d, 0.42, 0.3, 0.42, 0.54)),
    "autocapture": lambda d: (ring(d, 0.5, 0.5, 0.38), ring(d, 0.5, 0.5, 0.2), ring(d, 0.5, 0.5, 0.07, fill=True)),
    "autoframe": lambda d: ([line(d, *seg) for seg in ((0.08, 0.22, 0.08, 0.4), (0.08, 0.22, 0.26, 0.22), (0.92, 0.22, 0.92, 0.4), (0.92, 0.22, 0.74, 0.22), (0.08, 0.78, 0.08, 0.6), (0.08, 0.78, 0.26, 0.78), (0.92, 0.78, 0.92, 0.6), (0.92, 0.78, 0.74, 0.78))], box(d, 0.3, 0.38, 0.7, 0.62, r=0.05, fill=True)),
    "standby": lambda d: (box(d, 0.08, 0.16, 0.92, 0.84), d.polygon([p(0.16, 0.76), p(0.42, 0.42), p(0.62, 0.76)], fill=INK), d.polygon([p(0.48, 0.76), p(0.66, 0.54), p(0.84, 0.76)], fill=INK), ring(d, 0.7, 0.34, 0.08, fill=True)),
    "showinscenes": lambda d: (box(d, 0.08, 0.06, 0.7, 0.44, width=W - 3), box(d, 0.3, 0.2, 0.92, 0.58, width=W - 3), eye(d, 0.5, 0.78, 0.36, 0.16, W - 3)),
    "fititem": lambda d: (box(d, 0.06, 0.14, 0.94, 0.86), box(d, 0.36, 0.38, 0.64, 0.62, r=0.03, fill=True), arrow(d, 0.33, 0.5, 0.14, 0.5, 0.1, W - 3), arrow(d, 0.67, 0.5, 0.86, 0.5, 0.1, W - 3)),
    "activeapp": lambda d: (box(d, 0.06, 0.28, 0.38, 0.72), box(d, 0.62, 0.28, 0.94, 0.72, fill=True), arrow(d, 0.42, 0.5, 0.6, 0.5, 0.1, W - 3)),
    "appcapture": lambda d: (box(d, 0.06, 0.1, 0.62, 0.58), line(d, 0.06, 0.24, 0.62, 0.24), d.rectangle((*p(0.36, 0.38), *p(0.96, 0.92)), fill=(0, 0, 0, 0)), box(d, 0.38, 0.42, 0.94, 0.9), line(d, 0.38, 0.56, 0.94, 0.56)),
    "programtrigger": lambda d: (box(d, 0.06, 0.16, 0.6, 0.84), line(d, 0.06, 0.32, 0.6, 0.32), d.polygon([p(0.68, 0.3), p(0.68, 0.7), p(0.96, 0.5)], fill=INK)),
    "visibilitylink": lambda d: (box(d, 0.04, 0.32, 0.56, 0.68, r=0.18), box(d, 0.44, 0.32, 0.96, 0.68, r=0.18)),
    "showhide": lambda d: (eye(d, 0.5, 0.4, 0.42, 0.24), arrow(d, 0.22, 0.86, 0.78, 0.86, 0.12, W - 3)),
    # Moves.
    "sendto": lambda d: (person(d, 0.06, 0.4, 0.42, 0.88), arrow(d, 0.42, 0.42, 0.88, 0.12, 0.14)),
    "placeitem": lambda d: (box(d, 0.36, 0.36, 0.64, 0.64, r=0.03, fill=True), arrow(d, 0.5, 0.3, 0.5, 0.06, 0.12, W - 3), arrow(d, 0.5, 0.7, 0.5, 0.94, 0.12, W - 3), arrow(d, 0.3, 0.5, 0.06, 0.5, 0.12, W - 3), arrow(d, 0.7, 0.5, 0.94, 0.5, 0.12, W - 3)),
    "layout": lambda d: (box(d, 0.06, 0.12, 0.56, 0.88, r=0.04, fill=True), box(d, 0.64, 0.12, 0.94, 0.46, r=0.04, fill=True), box(d, 0.64, 0.54, 0.94, 0.88, r=0.04, fill=True)),
    # The other keys.
    "shape": lambda d: (box(d, 0.04, 0.18, 0.96, 0.82), box(d, 0.3, 0.3, 0.7, 0.7, r=0.03, width=W - 4), arrow(d, 0.26, 0.5, 0.12, 0.5, 0.09, W - 4), arrow(d, 0.74, 0.5, 0.88, 0.5, 0.09, W - 4)),
    "hook": lambda d: (box(d, 0.06, 0.38, 0.94, 0.92), line(d, 0.06, 0.52, 0.94, 0.52), arrow(d, 0.5, 0.04, 0.5, 0.36, 0.14)),
    "appswitch": lambda d: (arrow(d, 0.1, 0.32, 0.88, 0.32, 0.15), arrow(d, 0.9, 0.68, 0.12, 0.68, 0.15)),
    "fitoverlap": lambda d: (box(d, 0.06, 0.1, 0.78, 0.7), box(d, 0.5, 0.5, 0.94, 0.9, r=0.04, fill=True)),
    "tool": lambda d: (line(d, 0.1, 0.08, 0.1, 0.92), bars(d, 0.2, 0.12, 0.88, [0.7, 0.42, 0.56])),
    "blurall": blurred(lambda d: (ring(d, 0.26, 0.34, 0.16, fill=True), ring(d, 0.74, 0.34, 0.16, fill=True), ring(d, 0.5, 0.72, 0.16, fill=True)), 7),
    "option": lambda d: (box(d, 0.06, 0.28, 0.94, 0.72, r=0.22), ring(d, 0.72, 0.5, 0.15, fill=True)),
    "slide": lambda d: (line(d, 0.9, 0.08, 0.9, 0.92), box(d, 0.06, 0.34, 0.34, 0.66, r=0.03, fill=True), arrow(d, 0.42, 0.5, 0.8, 0.5, 0.15)),
}


def main():
    for key, draw_fn in ICONS.items():
        image = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        if getattr(draw_fn, "__name__", "") == "go":
            draw_fn(None, image)
        else:
            draw_fn(ImageDraw.Draw(image))
        folder = os.path.join(OUT, key)
        os.makedirs(folder, exist_ok=True)
        image.resize((40, 40), Image.LANCZOS).save(os.path.join(folder, "icon@2x.png"))
        image.resize((20, 20), Image.LANCZOS).save(os.path.join(folder, "icon.png"))
    print(f"{len(ICONS)} action icons drawn")


if __name__ == "__main__":
    main()
