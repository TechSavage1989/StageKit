#!/usr/bin/env python3
"""Marks every picture of the pack for keys that show whether something is on.

The owner: "the options that have an on and an off or toggle must represent
that current status on the key with the image". The pack's own "on" and "off"
pictures differ only in brightness, which is hard to read on a key. So each
gets a copy under imgs/pack/<palette>/status/on/ and status/off/ (apart:
some pictures use one file for both) with a mark that cannot be
missed: on, a green dot in the top left corner; off, the picture dimmed
further with a red slash across it. Keys with one state keep the plain ones.
Run after make-option-icons.py, from the streamdeck folder:
    python3 tools/make-state-icons.py
"""
import json
import os

from PIL import Image, ImageDraw, ImageEnhance

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PACK = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin", "imgs", "pack")
GREEN = (60, 205, 100, 255)
RED = (228, 64, 64, 255)
EDGE = (12, 12, 14, 255)


def marked(image, on):
    image = image.convert("RGBA")
    s = 4  # drawn large and shrunk, for smooth edges
    w, h = image.size
    big = image.resize((w * s, h * s), Image.LANCZOS)
    if not on:
        big = ImageEnhance.Brightness(big).enhance(0.55)
    d = ImageDraw.Draw(big)
    if on:
        r, cx, cy = 13 * s, 19 * s, 19 * s
        d.ellipse((cx - r - 3 * s, cy - r - 3 * s, cx + r + 3 * s, cy + r + 3 * s), fill=EDGE)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=GREEN)
    else:
        m = 14 * s
        d.line((m, m, w * s - m, h * s - m), fill=EDGE, width=17 * s)
        d.line((m, m, w * s - m, h * s - m), fill=RED, width=11 * s)
    return big.resize((w, h), Image.LANCZOS)


def main():
    pack = json.load(open(os.path.join(ROOT, "src", "pack.json"), encoding="utf-8"))
    made = 0
    for palette in pack["colours"]:
        for picture in pack["pictures"]:
            for name, on in ((picture["on"], True), (picture["off"], False)):
                source = os.path.join(PACK, palette, name + ".png")
                if not os.path.exists(source):
                    continue
                target = os.path.join(PACK, palette, "status", "on" if on else "off", name + ".png")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                # (256 colours: a quarter of the size, and no different to see.)
                image = marked(Image.open(source), on).quantize(colors=256, method=Image.Quantize.FASTOCTREE)
                image.save(target, optimize=True)
                made += 1
    print(made, "status pictures drawn")


if __name__ == "__main__":
    main()
