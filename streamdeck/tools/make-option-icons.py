#!/usr/bin/env python3
"""Key art without words, and a picture for every option of every key.

The icon pack came with each key's name written into the picture. The owner
asked for the names to go ("remove the embedded text names of the keys in
the art"; the key's own Title says it now) and for "icons specific to each
option, and this goes for every stream deck key".

This does both, in every palette:

 1. Takes the words out of every picture in imgs/pack/<palette>/.
 2. Draws the pictures the pack has none of, in its look: the pack's own
    little screen (from filters/next-capture) with a drawing in it. They go
    in imgs/pack/<palette>/options/ and are added to src/pack.json.

Run from the streamdeck folder, after tools/import-icons.py if that was run:
    python3 tools/make-option-icons.py
It can be run again: a picture with no words is left as it is.
"""
import json
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PACK = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin", "imgs", "pack")
LIST = os.path.join(ROOT, "src", "pack.json")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SIZE = 144
# The inside of the pack's little screen, clear of its frame and of the
# small camera in its corner; drawings are 64 x 36 in the middle of it.
SCREEN = (22, 27, 106, 67)
SCALE = 4


def near(a, b, by=10):
    return all(abs(x - y) <= by for x, y in zip(a[:3], b[:3]))


def without_words(image):
    """The picture with its name taken out: everything under the last clear
    band above the bottom is made background. (The background may shade from
    top to bottom, so a row is clear when it is all its own edge's colour,
    and is filled with that.)"""
    image = image.convert("RGBA")
    px = image.load()

    def clear(y):
        # (Not the outermost columns: a key's corners are rounded off.)
        return all(near(px[x, y], px[6, y], 14) for x in range(6, SIZE - 6))

    # The picture ends where the first clear band of some height begins
    # (some of the pack's files carry stray specks round the words, so the
    # words are not looked for themselves): everything from there down goes.
    start, run = None, 0
    for y in range(SIZE // 2, SIZE):
        run = run + 1 if clear(y) else 0
        if run == 5:
            start = y - 4
            break
    if start is None:
        return image  # the picture fills the key
    for row in range(start, SIZE - 2):
        edge = px[6, row]
        for x in range(6, SIZE - 6):
            px[x, row] = edge
    return image


# ---------------------------------------------------------------- drawings
# Each is a list of shapes on a 64 x 36 field: ("box", x0, y0, x1, y1) filled,
# ("frame", ...) an outline, ("line", x0, y0, x1, y1), ("ring", cx, cy, r),
# ("dot", cx, cy, r), ("tri", x0, y0, x1, y1, x2, y2), ("text", words).
def arrow(x0, y0, x1, y1, head=5):
    """A line with a head at its second end."""
    import math

    angle = math.atan2(y1 - y0, x1 - x0)
    left = (x1 - head * math.cos(angle - 0.6), y1 - head * math.sin(angle - 0.6))
    right = (x1 - head * math.cos(angle + 0.6), y1 - head * math.sin(angle + 0.6))
    return [("line", x0, y0, x1, y1), ("tri", x1, y1, left[0], left[1], right[0], right[1])]


EYE = [("ring", 32, 18, 11), ("dot", 32, 18, 5)]
WINDOW = [("frame", 6, 4, 34, 26), ("box", 6, 4, 34, 9)]
DRAWINGS = {
    # Manual window / source
    "toggle": arrow(14, 12, 50, 12) + arrow(50, 24, 14, 24),
    "hide": EYE + [("line", 16, 32, 48, 4)],
    "clear": WINDOW + [("line", 40, 12, 56, 28), ("line", 56, 12, 40, 28)],
    # Filters with no picture in the pack (each also in an "off" look)
    "zoom": [("ring", 27, 16, 11), ("line", 35, 24, 46, 33), ("line", 22, 16, 32, 16), ("line", 27, 11, 27, 21)],
    "fit": [("frame", 6, 3, 58, 33), ("box", 20, 9, 44, 27)],
    # Fit to Space Overlap: a window with a camera over its corner
    "overlap": [("frame", 6, 3, 50, 29), ("box", 38, 19, 58, 33)],
    "source-switcher": [("frame", 6, 8, 24, 28), ("box", 40, 8, 58, 28)] + arrow(27, 18, 37, 18, 4),
    "window-switcher": WINDOW + [("frame", 30, 12, 58, 34), ("box", 30, 12, 58, 17)],
    "trigger": WINDOW + [("tri", 42, 10, 42, 30, 58, 20)],
    "link": [("frame", 6, 10, 34, 26), ("frame", 30, 10, 58, 26)],
    "show-hide": EYE,
    # Moves
    "layout": [("box", 6, 4, 36, 32), ("box", 40, 4, 58, 16), ("box", 40, 20, 58, 32)],
    "spot": [("ring", 32, 18, 10), ("dot", 32, 18, 3), ("line", 32, 2, 32, 8), ("line", 32, 28, 32, 34), ("line", 14, 18, 20, 18), ("line", 44, 18, 50, 18)],
    # Arrange tools
    "align-left": [("line", 8, 3, 8, 33), ("box", 12, 7, 50, 15), ("box", 12, 21, 34, 29)],
    "align-right": [("line", 56, 3, 56, 33), ("box", 14, 7, 52, 15), ("box", 30, 21, 52, 29)],
    "align-center": [("line", 32, 3, 32, 33), ("box", 12, 7, 52, 15), ("box", 21, 21, 43, 29)],
    "align-top": [("line", 8, 4, 56, 4), ("box", 16, 8, 28, 32), ("box", 36, 8, 48, 22)],
    "align-bottom": [("line", 8, 32, 56, 32), ("box", 16, 4, 28, 28), ("box", 36, 14, 48, 28)],
    "align-middle": [("line", 8, 18, 56, 18), ("box", 16, 4, 28, 32), ("box", 36, 10, 48, 26)],
    "space-across": [("box", 6, 8, 16, 28), ("box", 27, 8, 37, 28), ("box", 48, 8, 58, 28)],
    "space-down": [("box", 14, 2, 50, 9), ("box", 14, 14, 50, 22), ("box", 14, 27, 50, 34)],
    "same-width": [("box", 10, 5, 54, 13), ("box", 10, 23, 54, 31)] + arrow(32, 18, 54, 18, 4) + arrow(32, 18, 10, 18, 4),
    "same-height": [("box", 10, 4, 22, 32), ("box", 42, 4, 54, 32)] + arrow(32, 18, 32, 4, 4) + arrow(32, 18, 32, 32, 4),
    "same-size": [("box", 8, 7, 28, 29), ("box", 36, 7, 56, 29)],
    # Dock options
    "limit": [("line", 54, 3, 54, 33), ("frame", 10, 9, 34, 27)] + arrow(36, 18, 50, 18, 4),
    "slide-to-edge": [("line", 56, 3, 56, 33), ("box", 8, 11, 24, 25)] + arrow(28, 18, 52, 18),
    "every-scene": [("frame", 6, 3, 40, 21), ("frame", 15, 9, 49, 27), ("box", 24, 15, 58, 33)],
    "keep-shape": [("frame", 8, 4, 56, 32), ("line", 8, 32, 56, 4), ("box", 5, 29, 11, 35), ("box", 53, 1, 59, 7)],
    # Slide to Edge: the middle of the free space
    "slide-centre": arrow(6, 18, 24, 18, 4) + arrow(58, 18, 40, 18, 4) + arrow(32, 2, 32, 12, 4) + arrow(32, 34, 32, 24, 4) + [("dot", 32, 18, 3)],
}
# Devices and sources, for any key's picture: capture cards inside and out,
# cameras, consoles and the like (the owner: "an additional set of streamdeck
# icons available for things like generic capture cards, external capture
# cards, etc"). Listed in the "devices" group.
DEVICES = {
    # A card that goes inside the computer: board, gold fingers, bracket, port.
    "capture-card": [("frame", 6, 8, 50, 26), ("box", 10, 26, 16, 30), ("box", 19, 26, 25, 30), ("box", 28, 26, 34, 30),
                     ("line", 55, 4, 55, 32), ("box", 51, 12, 59, 19), ("box", 14, 12, 30, 21)],
    # A box outside it, with its cable in and out.
    "capture-box": [("frame", 16, 9, 46, 27), ("dot", 40, 14, 2), ("line", 4, 18, 16, 18), ("line", 46, 18, 54, 18),
                    ("box", 54, 14, 60, 22), ("line", 21, 22, 34, 22)],
    "hdmi": [("frame", 18, 10, 46, 24), ("tri", 18, 24, 24, 30, 24, 24), ("tri", 46, 24, 40, 30, 40, 24), ("box", 24, 24, 40, 30),
             ("line", 32, 2, 32, 10)],
    "console": [("frame", 8, 12, 56, 28), ("line", 12, 20, 30, 20), ("dot", 48, 20, 2.5), ("line", 4, 30, 60, 30)],
    "webcam": [("ring", 32, 15, 10), ("dot", 32, 15, 4), ("line", 32, 25, 32, 31), ("line", 22, 32, 42, 32)],
    "camcorder": [("frame", 8, 10, 40, 28), ("tri", 40, 19, 56, 10, 56, 28), ("dot", 14, 15, 2)],
    "dslr": [("frame", 10, 10, 54, 31), ("box", 24, 5, 38, 10), ("ring", 32, 20, 7), ("dot", 47, 15, 2)],
    "microphone": [("frame", 27, 2, 37, 20), ("line", 22, 13, 22, 20), ("line", 42, 13, 42, 20), ("line", 22, 21, 42, 21),
                   ("line", 32, 21, 32, 30), ("line", 24, 32, 40, 32)],
    "phone": [("frame", 23, 2, 41, 34), ("line", 29, 6, 35, 6), ("dot", 32, 29, 2)],
    "display": [("frame", 10, 3, 54, 26), ("line", 32, 26, 32, 31), ("line", 22, 32, 42, 32)],
    "laptop": [("frame", 14, 5, 50, 24), ("box", 6, 26, 58, 30)],
    "browser": [("frame", 8, 4, 56, 32), ("box", 8, 4, 56, 11), ("line", 14, 18, 50, 18), ("line", 14, 25, 40, 25)],
    "speaker": [("box", 12, 13, 21, 23), ("tri", 21, 18, 33, 6, 33, 30), ("line", 38, 13, 42, 9), ("line", 38, 23, 42, 27),
                ("line", 39, 18, 45, 18)],
}
DRAWINGS.update(DEVICES)
# The Switch Scene key: a scene, its sources laid out in it.
DRAWINGS["scene"] = [("frame", 6, 3, 58, 33), ("box", 11, 8, 35, 28), ("box", 39, 8, 53, 16), ("box", 39, 20, 53, 28)]
SWITCHES_EXTRA = ["scene"]

# Aspect ratios: the ratio as a frame of that shape.
RATIOS = ["4:3", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1", "64:49"]
for ratio in RATIOS:
    a, b = (int(n) for n in ratio.split(":"))
    h = 30.0
    w = min(58.0, h * a / b)
    h = w * b / a
    DRAWINGS["ratio-" + ratio.replace(":", "x")] = [("frame", 32 - w / 2, 18 - h / 2, 32 + w / 2, 18 + h / 2)]
DRAWINGS["ratio-auto"] = [("frame", 6, 4, 58, 32), ("text", "A")]

# The ones that are a filter's key, with an "on" and an "off" look.
SWITCHES = ["scene", "zoom", "fit", "overlap", "source-switcher", "window-switcher", "trigger", "link", "show-hide", "limit", "slide-to-edge", "every-scene", "keep-shape"]


def drawn(shapes, ink):
    s = SCALE
    w, h = (SCREEN[2] - SCREEN[0]) * s, (SCREEN[3] - SCREEN[1]) * s
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    # The field of 64 x 36 in the middle of the screen.
    ox, oy = (w - 64 * s) / 2, (h - 36 * s) / 2
    k = s
    pen = int(2.6 * s)

    def at(x, y):
        return (ox + x * k, oy + y * k)

    for shape in shapes:
        kind = shape[0]
        if kind == "box":
            draw.rectangle((at(shape[1], shape[2]), at(shape[3], shape[4])), fill=ink)
        elif kind == "frame":
            draw.rectangle((at(shape[1], shape[2]), at(shape[3], shape[4])), outline=ink, width=pen)
        elif kind == "line":
            draw.line((at(shape[1], shape[2]), at(shape[3], shape[4])), fill=ink, width=pen)
        elif kind == "ring":
            cx, cy, r = shape[1:]
            draw.ellipse((at(cx - r, cy - r), at(cx + r, cy + r)), outline=ink, width=pen)
        elif kind == "dot":
            cx, cy, r = shape[1:]
            draw.ellipse((at(cx - r, cy - r), at(cx + r, cy + r)), fill=ink)
        elif kind == "tri":
            draw.polygon((at(shape[1], shape[2]), at(shape[3], shape[4]), at(shape[5], shape[6])), fill=ink)
        elif kind == "text":
            font = ImageFont.truetype(FONT, 22 * s)
            box = draw.textbbox((0, 0), shape[1], font=font)
            draw.text(((w - box[2] - box[0]) / 2, (h - box[3] - box[1]) / 2), shape[1], font=font, fill=ink)
    return layer.resize((SCREEN[2] - SCREEN[0], SCREEN[3] - SCREEN[1]), Image.LANCZOS)


def main():
    listed = json.load(open(LIST, encoding="utf-8"))
    palettes = listed["colours"]
    stripped = 0
    for palette in palettes:
        for folder, _, files in os.walk(os.path.join(PACK, palette)):
            # (Not the pictures drawn here, nor the marked copies of
            # tools/make-state-icons.py: their dot and slash are no words.)
            parts = os.path.relpath(folder, os.path.join(PACK, palette)).split(os.sep)
            if parts[0] in ("options", "status"):
                continue
            for name in files:
                if not name.endswith(".png"):
                    continue
                path = os.path.join(folder, name)
                before = Image.open(path)
                after = without_words(before)
                if after.tobytes() != before.convert("RGBA").tobytes():
                    after.save(path)
                    stripped += 1
    made = 0
    for palette in palettes:
        base = Image.open(os.path.join(PACK, palette, "filters", "next-capture.png")).convert("RGBA")
        # The colour of the screen's inside and of what is drawn on it.
        inside = base.getpixel((30, 60))
        ink = max((base.getpixel((x, y)) for x in range(40, 90) for y in range(36, 62)), key=lambda c: sum(c[:3]))
        ground = base.getpixel((2, 2))
        ImageDraw.Draw(base).rectangle(SCREEN, fill=inside)
        out = os.path.join(PACK, palette, "options")
        os.makedirs(out, exist_ok=True)
        for name, shapes in DRAWINGS.items():
            picture = base.copy()
            picture.alpha_composite(drawn(shapes, ink), (SCREEN[0], SCREEN[1]))
            if name in SWITCHES:
                picture.save(os.path.join(out, name + "-on.png"))
                # Switched off: the same, faded most of the way into the key.
                faded = Image.blend(picture, Image.new("RGBA", picture.size, ground), 0.6)
                faded.save(os.path.join(out, name + "-off.png"))
                made += 2
            else:
                picture.save(os.path.join(out, name + ".png"))
                made += 1
    pictures = [p for p in listed["pictures"] if not p["id"].startswith("options/")]
    for name in DRAWINGS:
        two = name in SWITCHES
        pictures.append(
            {
                "id": "options/" + name,
                "group": "devices" if name in DEVICES else "options",
                "label": name.replace("-", " ").replace("ratio ", "ratio ").replace("x", ":") if name.startswith("ratio-") else name.replace("-", " "),
                "on": f"options/{name}-on" if two else f"options/{name}",
                "off": f"options/{name}-off" if two else f"options/{name}",
            }
        )
    listed["pictures"] = pictures
    with open(LIST, "w", encoding="utf-8") as f:
        json.dump(listed, f, indent="\t", ensure_ascii=False)
        f.write("\n")
    print(f"{stripped} pictures had their words taken out; {made} pictures drawn; {len(pictures)} listed")


if __name__ == "__main__":
    main()
