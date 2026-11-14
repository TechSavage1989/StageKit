#!/usr/bin/env python3
"""Draws the plugin's icons into the .sdPlugin folder (needs Pillow).

Stream Deck wants each picture at two sizes, the second named @2x:
plugin icon 256/512, category icon 28/56, action icon 20/40, key 72/144.
Run from the streamdeck folder:  python3 tools/make-icons.py
"""
import os
import sys

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "com.techsavage1989.stagekit.sdPlugin", "imgs")
INK = (255, 255, 255, 255)
DIM = (150, 156, 170, 255)
ACCENT = (124, 92, 255, 255)
PANEL = (28, 30, 38, 255)


def canvas(size, background=None):
    scale = 4  # drawn large and shrunk, for smooth edges
    image = Image.new("RGBA", (size * scale, size * scale), background or (0, 0, 0, 0))
    return image, ImageDraw.Draw(image), size * scale


def save(image, size, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.resize((size, size), Image.LANCZOS).save(path)


def stage(draw, s, colour, fill=None):
    """A screen with a smaller picture in its corner: a scene with a camera."""
    w = max(2, s // 14)
    draw.rounded_rectangle((s * 0.14, s * 0.24, s * 0.86, s * 0.76), radius=s * 0.07, outline=colour, width=w)
    draw.rounded_rectangle((s * 0.52, s * 0.46, s * 0.78, s * 0.68), radius=s * 0.04, fill=fill or colour)


def switch(draw, s, on):
    """The same scene with a switch under it."""
    stage(draw, s, INK if on else DIM, ACCENT if on else DIM)
    y0, y1 = s * 0.82, s * 0.94
    draw.rounded_rectangle((s * 0.36, y0, s * 0.64, y1), radius=(y1 - y0) / 2, fill=ACCENT if on else (70, 74, 86, 255))
    knob = s * 0.57 if on else s * 0.43
    r = (y1 - y0) * 0.36
    draw.ellipse((knob - r, (y0 + y1) / 2 - r, knob + r, (y0 + y1) / 2 + r), fill=INK)


def move(draw, s, colour):
    """The camera with an arrow to its new place."""
    w = max(2, s // 14)
    draw.rounded_rectangle((s * 0.14, s * 0.24, s * 0.86, s * 0.76), radius=s * 0.07, outline=colour, width=w)
    draw.rounded_rectangle((s * 0.22, s * 0.50, s * 0.42, s * 0.68), radius=s * 0.03, outline=DIM, width=max(1, w // 2))
    draw.rounded_rectangle((s * 0.58, s * 0.32, s * 0.78, s * 0.50), radius=s * 0.03, fill=ACCENT)
    draw.line((s * 0.40, s * 0.52, s * 0.55, s * 0.44), fill=colour, width=w)


def tool(draw, s, colour, lit):
    """Three items lined up on one edge."""
    w = max(2, s // 14)
    draw.rounded_rectangle((s * 0.14, s * 0.24, s * 0.86, s * 0.76), radius=s * 0.07, outline=colour, width=w)
    draw.line((s * 0.27, s * 0.33, s * 0.27, s * 0.67), fill=lit, width=max(2, w * 2 // 3))
    for i, length in enumerate((0.40, 0.26, 0.34)):
        y = s * (0.35 + i * 0.11)
        draw.rounded_rectangle((s * 0.31, y, s * (0.31 + length), y + s * 0.07), radius=s * 0.02, fill=colour)


def blur(draw, s, on):
    """A screen whose picture is blurred (soft dots) or clear (one sharp block)."""
    colour = INK if on else DIM
    w = max(2, s // 14)
    draw.rounded_rectangle((s * 0.14, s * 0.24, s * 0.86, s * 0.76), radius=s * 0.07, outline=colour, width=w)
    if on:
        for cx, cy, r in ((0.34, 0.42, 0.07), (0.52, 0.52, 0.10), (0.68, 0.40, 0.06), (0.40, 0.62, 0.05), (0.66, 0.62, 0.07)):
            draw.ellipse((s * (cx - r), s * (cy - r), s * (cx + r), s * (cy + r)), fill=ACCENT)
    else:
        draw.rounded_rectangle((s * 0.30, s * 0.38, s * 0.70, s * 0.62), radius=s * 0.03, fill=DIM)


def main():
    for size, name in ((256, "marketplace.png"), (512, "marketplace@2x.png")):
        image, draw, s = canvas(size, PANEL)
        stage(draw, s, INK, ACCENT)
        save(image, size, os.path.join(OUT, "plugin", name))
    for size, name in ((28, "category-icon.png"), (56, "category-icon@2x.png")):
        image, draw, s = canvas(size)
        stage(draw, s, INK)
        save(image, size, os.path.join(OUT, "plugin", name))
    for size, suffix in ((20, ""), (40, "@2x")):
        image, draw, s = canvas(size)
        stage(draw, s, INK)
        save(image, size, os.path.join(OUT, "actions", "switch", f"icon{suffix}.png"))
        image, draw, s = canvas(size)
        move(draw, s, INK)
        save(image, size, os.path.join(OUT, "actions", "move", f"icon{suffix}.png"))
    for size, suffix in ((20, ""), (40, "@2x")):
        image, draw, s = canvas(size)
        tool(draw, s, INK, INK)
        save(image, size, os.path.join(OUT, "actions", "tool", f"icon{suffix}.png"))
        image, draw, s = canvas(size)
        blur(draw, s, True)
        save(image, size, os.path.join(OUT, "actions", "blur", f"icon{suffix}.png"))
    for size, suffix in ((72, ""), (144, "@2x")):
        image, draw, s = canvas(size, PANEL)
        tool(draw, s, INK, ACCENT)
        save(image, size, os.path.join(OUT, "actions", "tool", f"key{suffix}.png"))
        for on in (False, True):
            image, draw, s = canvas(size, PANEL)
            blur(draw, s, on)
            save(image, size, os.path.join(OUT, "actions", "blur", f"{'on' if on else 'off'}{suffix}.png"))
    for size, suffix in ((72, ""), (144, "@2x")):
        for on in (False, True):
            image, draw, s = canvas(size, PANEL)
            switch(draw, s, on)
            save(image, size, os.path.join(OUT, "actions", "switch", f"{'on' if on else 'off'}{suffix}.png"))
        image, draw, s = canvas(size, PANEL)
        move(draw, s, INK)
        save(image, size, os.path.join(OUT, "actions", "move", f"key{suffix}.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
