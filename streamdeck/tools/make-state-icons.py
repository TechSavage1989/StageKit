#!/usr/bin/env python3
"""Marks every picture of the pack for keys that show whether something is on.

The owner: "the options that have an on and an off or toggle must represent
that current status on the key with the image". The pack's own "on" and "off"
pictures differ only in brightness, which is hard to read on a key. So each
gets a copy under imgs/pack/<palette>/status/on/ and status/off/ (apart:
some pictures use one file for both) with a mark that cannot be
missed: on, a green dot in the top left corner; off, a red "no" sign (a ring
with a bar across) in the same corner. Keys with one state keep the plain ones.

The off picture stays easy to read: it is brought to most of the brightness of
the on picture and only partly greyed. The first version dimmed the pack's off
picture (already dark) by half again and laid a thick red slash across the
middle; the owner: "the current 'off' keys are very dark and virtually
impossible to recognize the imagery to know what they control".
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


# How bright an off picture is, against its on picture, and how much colour
# it keeps.
OFF_BRIGHTNESS = 0.95
OFF_COLOUR = 0.5


def mean_light(image):
    grey = image.convert("RGBA")
    alpha = grey.getchannel("A")
    lum = grey.convert("L")
    seen = [l for l, a in zip(lum.getdata(), alpha.getdata()) if a > 32]
    return sum(seen) / len(seen) if seen else 0.0


def marked(image, on, on_light=None):
    image = image.convert("RGBA")
    s = 4  # drawn large and shrunk, for smooth edges
    w, h = image.size
    big = image.resize((w * s, h * s), Image.LANCZOS)
    r, cx, cy = 13 * s, 19 * s, 19 * s
    d = ImageDraw.Draw(big)
    if on:
        d.ellipse((cx - r - 3 * s, cy - r - 3 * s, cx + r + 3 * s, cy + r + 3 * s), fill=EDGE)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=GREEN)
        return big.resize((w, h), Image.LANCZOS)
    # Off: brought to most of the on picture's brightness (the pack's own off
    # pictures are darker), with part of its colour, and a "no" sign in the
    # corner where the on picture has its dot. The picture itself is left
    # uncovered.
    own = mean_light(image)
    lift = OFF_BRIGHTNESS * (on_light / own) if on_light and own > 1 else OFF_BRIGHTNESS
    alpha = big.getchannel("A")
    big = ImageEnhance.Brightness(big.convert("RGB")).enhance(min(max(lift, 0.5), 3.0))
    big = ImageEnhance.Color(big).enhance(OFF_COLOUR).convert("RGBA")
    big.putalpha(alpha)
    d = ImageDraw.Draw(big)
    d.ellipse((cx - r - 3 * s, cy - r - 3 * s, cx + r + 3 * s, cy + r + 3 * s), fill=EDGE)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=RED)
    d.ellipse((cx - r + 4 * s, cy - r + 4 * s, cx + r - 4 * s, cy + r - 4 * s), fill=EDGE)
    k = int((r - 4 * s) * 0.72)
    d.line((cx - k, cy - k, cx + k, cy + k), fill=RED, width=4 * s)
    return big.resize((w, h), Image.LANCZOS)


def main():
    pack = json.load(open(os.path.join(ROOT, "src", "pack.json"), encoding="utf-8"))
    made = 0
    for palette in pack["colours"]:
        for picture in pack["pictures"]:
            on_source = os.path.join(PACK, palette, picture["on"] + ".png")
            on_light = mean_light(Image.open(on_source)) if os.path.exists(on_source) else None
            for name, on in ((picture["on"], True), (picture["off"], False)):
                source = os.path.join(PACK, palette, name + ".png")
                if not os.path.exists(source):
                    continue
                target = os.path.join(PACK, palette, "status", "on" if on else "off", name + ".png")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                # (256 colours: a quarter of the size, and no different to see.)
                image = marked(Image.open(source), on, on_light).quantize(colors=256, method=Image.Quantize.FASTOCTREE)
                image.save(target, optimize=True)
                made += 1
    print(made, "status pictures drawn")


if __name__ == "__main__":
    main()
