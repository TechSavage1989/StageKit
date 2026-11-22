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
# The inside of the pack's little screen: its frame's lines are at x 14 and
# 129, y 17 and 81 (centre 71.5, 49), with the pack's small camera in its
# bottom right corner (the pack's own pictures of this screen have it too).
# Drawings are 64 x 36, centred on the frame's centre: SCREEN is a box of that
# centre, clear of the camera. (Up to beta164 it was (22, 27, 106, 67), the
# drawings sat 7.5 px left of the frame's centre; the owner: "all the
# standalone icons appear to have the images offset to the left. make them
# all centered".) ERASE is what is cleared of the pack's own drawing.
FRAME = (14, 17, 129, 81)
SCREEN = (30, 29, 113, 69)
ERASE = (22, 27, 113, 69)
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
# ("dot", cx, cy, r), ("arc", cx, cy, r, start, end) (degrees, clockwise from
# the right), ("tri", x0, y0, x1, y1, x2, y2), ("text", words).
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
    # OBS Stats: rising bars and a circular reset arrow.
    "reset-stats": [("box", 5, 22, 11, 31), ("box", 15, 14, 21, 31), ("box", 25, 6, 31, 31),
                    ("arc", 48, 19, 13, -60, 235), ("tri", 40.5, 8.4, 33, 10, 39, 16)],
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
    # Fit to space: the room beside a column, and the item filling it
    "fit-space": [("frame", 4, 3, 40, 33), ("box", 10, 9, 34, 27), ("box", 46, 3, 58, 16), ("box", 46, 20, 58, 33)],
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
# Pictures for groups of the pack's own, found by the words of their label
# (src/pictures.ts): a Switch Scene key's scene ("scenes", by the scene's
# name) and a Show in Scenes key's source ("show hide", by the source's
# name). The owner asked for the Standby, Live and Chat Live scenes and for
# the holding modes' and the columns' show buttons to have pictures "that are
# representative of their function". Name: (group, label); each has an on and
# an off look.
GROUPED = {
    "scene-live": ("scenes", "live"),
    "scene-chat-live": ("scenes", "chat live"),
    "scene-standby": ("scenes", "standby"),
    "mode-starting": ("show hide", "starting"),
    "mode-brb": ("show hide", "brb"),
    "mode-intermission": ("show hide", "intermission"),
    "mode-ending": ("show hide", "ending"),
    "mode-column-chat": ("show hide", "column chat"),
    "mode-column-gba": ("show hide", "column gba"),
}
_BUBBLES = [("box", 38, 5, 58, 11), ("tri", 40, 11, 45, 11, 39, 15), ("box", 38, 16, 58, 22), ("tri", 40, 22, 45, 22, 39, 26),
            ("box", 38, 27, 58, 33)]
# Live: a screen with a record button filling it.
DRAWINGS["scene-live"] = [("frame", 4, 2, 60, 34), ("ring", 32, 18, 11), ("dot", 32, 18, 6)]
# Chat Live: the record button beside a column of chat bubbles.
DRAWINGS["scene-chat-live"] = [("frame", 2, 2, 34, 34), ("ring", 18, 18, 10), ("dot", 18, 18, 5)] + _BUBBLES
# Standby: the standby (power) sign on a screen.
DRAWINGS["scene-standby"] = [("frame", 4, 2, 60, 34), ("arc", 32, 19, 11, -55, 235), ("line", 32, 5, 32, 18)]
# The holding modes: starting (play), be right back (a cup), intermission (an
# hourglass), ending (stop).
DRAWINGS["mode-starting"] = [("ring", 32, 18, 15), ("tri", 27, 9, 27, 27, 42, 18)]
DRAWINGS["mode-brb"] = [("frame", 18, 13, 38, 31), ("arc", 39, 21, 6, -90, 90), ("line", 12, 34, 46, 34),
                        ("line", 23, 3, 23, 9), ("line", 28, 2, 28, 9), ("line", 33, 3, 33, 9)]
DRAWINGS["mode-intermission"] = [("line", 20, 2, 44, 2), ("line", 20, 34, 44, 34), ("line", 22, 3, 42, 33), ("line", 42, 3, 22, 33),
                                 ("tri", 27, 9, 37, 9, 32, 16), ("tri", 32, 24, 24, 31, 40, 31)]
DRAWINGS["mode-ending"] = [("ring", 32, 18, 15), ("box", 25, 11, 39, 25)]
# The columns: a game with a chat column, or with a handheld's column.
DRAWINGS["mode-column-chat"] = [("frame", 2, 2, 62, 34), ("box", 6, 6, 30, 30), ("line", 34, 2, 34, 34)] + [
    ("box", 38, 6, 58, 11), ("box", 38, 15.5, 58, 20.5), ("box", 38, 25, 58, 30)]
DRAWINGS["mode-column-gba"] = [("frame", 2, 2, 62, 34), ("box", 6, 6, 30, 30), ("line", 34, 2, 34, 34),
                               ("frame", 40, 3, 56, 33), ("box", 43, 6, 53, 17), ("dot", 45, 25, 2), ("dot", 51, 25, 2)]
# The Switch Scene key: a scene, its sources laid out in it.
DRAWINGS["scene"] = [("frame", 6, 3, 58, 33), ("box", 11, 8, 35, 28), ("box", 39, 8, 53, 16), ("box", 39, 20, 53, 28)]
SWITCHES_EXTRA = ["scene"]

# Aspect ratios: the ratio as a frame of that shape.
RATIOS = ["4:3", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1", "64:49", "16:15"]
for ratio in RATIOS:
    a, b = (int(n) for n in ratio.split(":"))
    h = 30.0
    w = min(58.0, h * a / b)
    h = w * b / a
    DRAWINGS["ratio-" + ratio.replace(":", "x")] = [("frame", 32 - w / 2, 18 - h / 2, 32 + w / 2, 18 + h / 2)]
DRAWINGS["ratio-auto"] = [("frame", 6, 4, 58, 32), ("text", "A")]

# The pictures that came from the pack in another style (the arrange tools,
# the camera places, the hook keys, the plain keys), drawn again in this one.
# The owner: "all keys for the plugin must be of consistent design language
# and styling".
DRAWINGS["copy-all"] = [("box", 4, 11, 20, 25)] + arrow(22, 18, 34, 18, 4) + [
    ("frame", 38, 2, 58, 12), ("frame", 38, 13, 58, 23), ("frame", 38, 24, 58, 34)]
DRAWINGS["copy-chosen"] = [("box", 4, 11, 20, 25)] + arrow(22, 18, 34, 18, 4) + [
    ("box", 38, 2, 58, 12), ("frame", 38, 13, 58, 23), ("box", 38, 24, 58, 34)]
DRAWINGS["snap"] = [("box", 8, 8, 28, 28), ("frame", 36, 8, 56, 28), ("line", 32, 2, 32, 34)]
DRAWINGS["guides"] = [("frame", 6, 3, 58, 33), ("line", 32, 3, 32, 33), ("line", 6, 18, 58, 18), ("box", 22, 10, 30, 16)]
DRAWINGS["snap-now"] = [("box", 8, 8, 28, 28), ("frame", 36, 8, 56, 28)] + arrow(30, 18, 34, 18, 3)
# A box with an arrow to the edge it slides to.
for _name, (_dx, _dy) in {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0), "up-left": (-1, -1),
                          "up-right": (1, -1), "down-left": (-1, 1), "down-right": (1, 1)}.items():
    _ex, _ey = 32 + _dx * 26, 18 + _dy * 15
    _edge = []
    if _dx:
        _edge.append(("line", _ex, 3, _ex, 33))
    if _dy:
        _edge.append(("line", 6, _ey, 58, _ey))
    _bx, _by = 32 - _dx * 10, 18 - _dy * 6
    DRAWINGS["slide-" + _name] = _edge + [("box", _bx - 5, _by - 5, _bx + 5, _by + 5)] + arrow(
        _bx + _dx * 7, _by + _dy * 7, _ex - _dx * 5, _ey - _dy * 4, 4)
# The camera's nine places on the screen.
for _row, _y in (("top", 4), ("middle", 13), ("bottom", 22)):
    for _col, _x in (("left", 9), ("centre", 26), ("right", 43)):
        _place = "centre" if (_row, _col) == ("middle", "centre") else f"{_row}-{_col}"
        DRAWINGS["camera-" + _place] = [("frame", 6, 2, 58, 34), ("box", _x, _y, _x + 12, _y + 10)]
# Move Source's tenth way, a spot on the screen (Destination::Spot, the same
# spot in every scene): the screen with a box at a point measured from its
# corner, its two measures drawn to the edges. (The ninth, the item's own
# saved place, is "spot", a target.) The owner: "Move source needs plenty of
# relevant custom icons for all the potential options".
DRAWINGS["screen-spot"] = [("frame", 6, 2, 58, 34), ("box", 30, 16, 42, 26), ("line", 6, 12, 30, 12), ("line", 24, 2, 24, 16),
                           ("tri", 30, 12, 26, 9.5, 26, 14.5), ("tri", 24, 16, 21.5, 12, 26.5, 12)]
# The hook keys: a game, a window, inside the corners of a capture.
_CORNERS = [("line", 6, 4, 11, 4), ("line", 6, 4, 6, 9), ("line", 53, 4, 58, 4), ("line", 58, 4, 58, 9),
            ("line", 6, 32, 11, 32), ("line", 6, 27, 6, 32), ("line", 53, 32, 58, 32), ("line", 58, 27, 58, 32)]
DRAWINGS["hook-game"] = _CORNERS + [("frame", 16, 10, 48, 26), ("line", 22, 18, 30, 18), ("line", 26, 14, 26, 22),
                                    ("dot", 39, 15, 2), ("dot", 43, 20, 2)]
DRAWINGS["hook-window"] = _CORNERS + [("frame", 16, 9, 48, 27), ("box", 16, 9, 48, 14)]
# The plain keys, for a key whose target is not chosen yet.
DRAWINGS["plain-switch"] = [("frame", 14, 10, 50, 26), ("dot", 41, 18, 5)]
DRAWINGS["plain-move"] = [("frame", 6, 3, 58, 33), ("box", 12, 9, 24, 19)] + arrow(26, 21, 48, 27, 4)
DRAWINGS["plain-tool"] = [("line", 8, 3, 8, 33), ("box", 12, 7, 50, 15), ("box", 12, 21, 34, 29)]

# Source types: what a source is, for any key that acts on one, chosen in the
# key's settings (the owner: "instead of assigning a specific icon to my
# specific sources, ad an option in the key to choose a source type ... that
# way anyone with any source can pick it accordingly"). The ids are kept in
# keys' settings and are append-only; they are the same as the Scene Modes
# filter's *Mode type* in OBS. The last four are the holding modes' drawings.
SOURCE_TYPES = [
    "game", "window", "art_app", "code_editor", "browser", "camera", "capture_card", "display", "phone", "tablet",
    "chat", "music", "alerts", "title", "image", "video", "starting", "brb", "intermission", "ending",
]
TYPE_REUSES = {"starting": "mode-starting", "brb": "mode-brb", "intermission": "mode-intermission", "ending": "mode-ending"}
TYPES = {
    "game": [("frame", 10, 9, 54, 27), ("line", 17, 18, 27, 18), ("line", 22, 13, 22, 23), ("dot", 41, 15, 2.5),
             ("dot", 47, 21, 2.5)],
    "window": [("frame", 8, 4, 56, 32), ("box", 8, 4, 56, 10), ("frame", 14, 15, 30, 27), ("line", 36, 17, 50, 17),
               ("line", 36, 24, 46, 24)],
    "art_app": [("ring", 25, 18, 14), ("dot", 19, 12, 2.5), ("dot", 27, 10, 2.5), ("dot", 33, 15, 2.5), ("dot", 18, 21, 2.5),
                ("line", 43, 29, 57, 5), ("dot", 41, 31, 3.5)],
    "code_editor": [("line", 22, 9, 12, 18), ("line", 12, 18, 22, 27), ("line", 42, 9, 52, 18), ("line", 52, 18, 42, 27),
                    ("line", 36, 5, 28, 31)],
    "browser": [("frame", 8, 4, 56, 32), ("box", 8, 4, 56, 10), ("ring", 32, 21, 7), ("line", 25, 21, 39, 21),
                ("line", 32, 14, 32, 28)],
    "camera": DEVICES["webcam"],
    "capture_card": DEVICES["capture-card"],
    "display": DEVICES["display"],
    "phone": DEVICES["phone"],
    "tablet": [("frame", 8, 4, 56, 32), ("frame", 13, 8, 46, 28), ("dot", 51.5, 18, 2)],
    "chat": [("box", 6, 4, 40, 16), ("tri", 10, 16, 18, 16, 9, 22), ("frame", 24, 19, 58, 31), ("tri", 48, 31, 54, 31, 55, 35)],
    "music": [("dot", 20, 28, 5), ("dot", 44, 25, 5), ("line", 24, 28, 24, 7), ("line", 48, 25, 48, 4), ("line", 24, 7, 48, 4),
              ("line", 24, 12, 48, 9)],
    "alerts": [("arc", 32, 17, 11, 180, 360), ("line", 21, 17, 21, 26), ("line", 43, 17, 43, 26), ("line", 16, 27, 48, 27),
               ("dot", 32, 31, 3), ("line", 32, 3, 32, 6)],
    "title": [("box", 10, 7, 54, 14), ("line", 10, 21, 48, 21), ("line", 10, 28, 36, 28)],
    "image": [("frame", 8, 4, 56, 32), ("tri", 12, 30, 26, 13, 40, 30), ("tri", 32, 30, 43, 19, 53, 30), ("dot", 46, 11, 3)],
    "video": [("frame", 6, 4, 58, 32), ("line", 6, 9, 58, 9), ("line", 6, 27, 58, 27), ("tri", 27, 12, 27, 24, 38, 18)],
}


def type_name(type_id):
    """The picture's name (options/<name>) for a source type."""
    return TYPE_REUSES.get(type_id) or "type-" + type_id.replace("_", "-")


def scaled(shapes, k, cx=32, cy=18):
    """A drawing made smaller about a point of the field."""
    out = []
    for shape in shapes:
        kind, v = shape[0], list(shape[1:])
        if kind in ("ring", "dot"):
            v = [cx + (v[0] - cx) * k, cy + (v[1] - cy) * k, v[2] * k]
        elif kind == "arc":
            v = [cx + (v[0] - cx) * k, cy + (v[1] - cy) * k, v[2] * k, v[3], v[4]]
        elif kind != "text":
            v = [(cx + (c - cx) * k) if i % 2 == 0 else (cy + (c - cy) * k) for i, c in enumerate(v)]
        out.append((kind, *v))
    return out


for _type, _shapes in TYPES.items():
    DRAWINGS[type_name(_type)] = _shapes
# Capture Active Window with a source type: the type inside the hook keys'
# corners of a capture, as hook-game has its game.
HOOK_TYPES = {}
for _type in SOURCE_TYPES:
    HOOK_TYPES["hook-type-" + _type.replace("_", "-")] = _CORNERS + scaled(DRAWINGS[type_name(_type)], 0.7)
DRAWINGS.update(HOOK_TYPES)


# More source types, for what no picture of the pack shows on its own (the
# owner: "make as many scene and source types as possible and provide the
# proper respective icons"). Generic, as every type is: no one's own sources.
# Where the owner's own sets have a picture of a type for a key's function
# (a show key's eye, a shadow, a blur, a scene layout), src/key-types.json
# gives the key that one; these are what is left. Ids append-only, in
# src/key-types.json's sourceTypes.
def _person(cx, cy, r):
    return [("dot", cx, cy - r * 0.55, r * 0.45), ("arc", cx, cy + r * 0.95, r * 0.9, 180, 360)]


MORE_TYPES = {
    # A cartridge.
    "retro": [("frame", 18, 3, 46, 33), ("box", 22, 8, 42, 20), ("line", 23, 26, 23, 31), ("line", 29, 26, 29, 31),
              ("line", 35, 26, 35, 31), ("line", 41, 26, 41, 31)],
    # A handheld's screen linked to a console's picture by its cable, and the
    # second of two such screens (the owner's console-plus-handheld layout).
    "linked_handheld": [("frame", 4, 4, 34, 26), ("frame", 42, 8, 60, 30), ("box", 45, 11, 57, 20), ("dot", 48, 25, 1.6),
                        ("dot", 54, 25, 1.6), ("line", 34, 20, 42, 20)],
    "linked_handheld_2": [("frame", 4, 2, 22, 17), ("box", 7, 5, 19, 12), ("frame", 4, 19, 22, 34), ("box", 7, 22, 19, 29),
                          ("frame", 30, 19, 48, 34), ("box", 30, 19, 48, 34), ("line", 22, 26, 30, 26)],
    # A window with a gamepad in it.
    "emulator": [("frame", 6, 3, 58, 33), ("box", 6, 3, 58, 9), ("line", 18, 21, 28, 21), ("line", 23, 16, 23, 26),
                 ("dot", 40, 18, 2.5), ("dot", 46, 24, 2.5)],
    # A handheld: screen, cross pad and two buttons.
    "handheld": [("frame", 8, 5, 56, 31), ("frame", 21, 9, 43, 27), ("line", 11, 18, 18, 18), ("line", 14.5, 14.5, 14.5, 21.5),
                 ("dot", 50, 14, 2.2), ("dot", 50, 22, 2.2)],
    "second_display": [("frame", 3, 4, 29, 22), ("box", 35, 4, 61, 22), ("line", 16, 22, 16, 28), ("line", 48, 22, 48, 28),
                       ("line", 9, 30, 23, 30), ("line", 41, 30, 55, 30)],
    # A page of writing with its corner turned.
    "office": [("line", 20, 2, 38, 2), ("line", 20, 2, 20, 34), ("line", 20, 34, 44, 34), ("line", 44, 34, 44, 9),
               ("line", 38, 2, 44, 9), ("line", 25, 13, 39, 13), ("line", 25, 19, 39, 19), ("line", 25, 25, 34, 25)],
    "video_player": [("frame", 6, 2, 58, 25), ("tri", 27, 7, 27, 20, 38, 13.5), ("line", 6, 31, 58, 31), ("box", 6, 28, 28, 34)],
    # A board with pictures pinned up.
    "reference": [("frame", 4, 2, 60, 34), ("box", 9, 7, 25, 18), ("frame", 29, 7, 43, 20), ("box", 47, 7, 55, 16),
                  ("box", 9, 23, 20, 29), ("box", 25, 25, 55, 29)],
    # Cameras of several sorts: a face in a corner, the whole screen, at the side, on a phone, over a desk.
    "facecam": [("frame", 4, 2, 60, 34), ("frame", 38, 16, 58, 32)] + _person(48, 25, 6),
    "full_camera": [("frame", 4, 2, 60, 34)] + _person(32, 18, 13),
    "side_camera": [("frame", 4, 2, 60, 34), ("line", 42, 2, 42, 34), ("box", 8, 7, 37, 29)] + _person(51, 16, 7),
    "phone_camera": [("frame", 22, 1, 42, 35)] + _person(32, 18, 7),
    "workspace_camera": [("box", 24, 1, 40, 7), ("dot", 32, 9, 2.5), ("line", 4, 33, 60, 33), ("box", 16, 25, 30, 32),
                         ("frame", 36, 22, 50, 32)] + arrow(32, 12, 32, 20, 4),
    "virtual_camera": [("ring", 26, 18, 10), ("dot", 26, 18, 4), ("line", 48, 4, 48, 16), ("line", 42, 10, 54, 10),
                       ("line", 44, 6, 52, 14), ("line", 52, 6, 44, 14), ("dot", 52, 27, 2), ("dot", 44, 31, 1.5)],
    # A speech bubble with a voice in it.
    "voice_chat": [("frame", 8, 3, 50, 26), ("tri", 14, 26, 24, 26, 11, 34), ("line", 17, 11, 17, 18), ("line", 23, 8, 23, 21),
                   ("line", 29, 12, 29, 17), ("line", 35, 9, 35, 20), ("line", 41, 12, 41, 17)],
    # Apps, as what they make, never a program's logo or letter (the owner:
    # "App 3D, or any app type should not have a specific letter or anything.
    # it should only be images relevant to the source"): a wireframe cube, a
    # bust on its stand with a loop tool, a set square beside a ruler.
    "app_3d": [("line", 32, 2, 47, 10), ("line", 47, 10, 47, 26), ("line", 47, 26, 32, 34), ("line", 32, 34, 17, 26),
               ("line", 17, 26, 17, 10), ("line", 17, 10, 32, 2), ("line", 17, 10, 32, 18), ("line", 47, 10, 32, 18),
               ("line", 32, 18, 32, 34), ("dot", 32, 18, 2.5)],
    "sculpting": [("ring", 26, 10, 7), ("arc", 26, 31, 13, 180, 360), ("box", 11, 30, 41, 34), ("line", 46, 32, 54, 9),
                  ("ring", 56, 6, 3.5)],
    "cad": [("line", 6, 33, 6, 3), ("line", 6, 3, 36, 33), ("line", 36, 33, 6, 33), ("line", 12, 27, 12, 17), ("line", 12, 17, 22, 27),
            ("line", 22, 27, 12, 27), ("frame", 44, 2, 58, 34), ("line", 44, 8, 50, 8), ("line", 44, 14, 53, 14), ("line", 44, 20, 50, 20),
            ("line", 44, 26, 53, 26)],
    # Devices.
    "headset": [("arc", 32, 21, 15, 180, 360), ("box", 14, 19, 22, 31), ("box", 42, 19, 50, 31), ("line", 46, 31, 38, 34),
                ("dot", 36, 34, 2)],
    "pc": [("frame", 4, 4, 34, 24), ("line", 19, 24, 19, 30), ("line", 11, 31, 27, 31), ("frame", 42, 2, 58, 34), ("dot", 50, 9, 2),
           ("line", 46, 16, 54, 16), ("line", 46, 21, 54, 21)],
    "retro_console": [("frame", 6, 14, 58, 32), ("frame", 22, 3, 42, 14), ("line", 26, 8, 38, 8), ("dot", 13, 23, 2.5),
                      ("box", 44, 21, 52, 25), ("line", 6, 18, 58, 18)],
    "controller": [("frame", 12, 10, 52, 25), ("tri", 12, 25, 22, 25, 10, 33), ("tri", 52, 25, 42, 25, 54, 33), ("line", 18, 17.5, 26, 17.5),
                   ("line", 22, 13.5, 22, 21.5), ("dot", 41, 15, 2), ("dot", 46, 20, 2), ("line", 32, 10, 32, 2)],
    "stream_deck": [("frame", 8, 4, 56, 32), ("box", 13, 9, 23, 17), ("box", 27, 9, 37, 17), ("box", 41, 9, 51, 17),
                    ("box", 13, 20, 23, 28), ("box", 27, 20, 37, 28), ("box", 41, 20, 51, 28)],
    "pen_display": [("frame", 4, 4, 44, 28), ("line", 8, 33, 40, 33), ("line", 24, 28, 24, 33), ("line", 47, 31, 60, 6),
                    ("tri", 47, 31, 45, 35, 50, 33), ("arc", 18, 16, 6, 180, 360), ("arc", 30, 16, 6, 0, 180)],
    "vr_headset": [("frame", 12, 9, 52, 27), ("ring", 23, 18, 5), ("ring", 41, 18, 5), ("line", 4, 14, 12, 14), ("line", 52, 14, 60, 14),
                   ("tri", 28, 27, 36, 27, 32, 22)],
    "action_cam": [("frame", 18, 8, 46, 30), ("ring", 30, 19, 7), ("dot", 30, 19, 2.5), ("box", 38, 4, 44, 8), ("dot", 41, 13, 1.6)],
    "ptz_camera": [("box", 22, 29, 42, 33), ("line", 32, 23, 32, 29), ("ring", 32, 14, 9), ("dot", 32, 14, 3.5),
                   ("arc", 32, 14, 15, 200, 250), ("arc", 32, 14, 15, 290, 340)],
    "mixer": [("frame", 4, 3, 60, 33), ("line", 14, 8, 14, 28), ("line", 26, 8, 26, 28), ("line", 38, 8, 38, 28), ("line", 50, 8, 50, 28),
              ("box", 10, 20, 18, 24), ("box", 22, 11, 30, 15), ("box", 34, 17, 42, 21), ("box", 46, 13, 54, 17)],
    "key_light": [("box", 16, 3, 48, 17), ("line", 32, 17, 32, 30), ("line", 22, 33, 42, 33), ("line", 12, 22, 18, 19),
                  ("line", 52, 22, 46, 19), ("line", 24, 23, 26, 20), ("line", 40, 23, 38, 20)],
    "green_screen": [("box", 8, 3, 56, 27), ("line", 12, 27, 6, 34), ("line", 52, 27, 58, 34), ("line", 32, 27, 32, 34)],
    # Status.
    "live": [("dot", 32, 18, 6), ("arc", 32, 18, 12, -45, 45), ("arc", 32, 18, 19, -45, 45), ("arc", 32, 18, 12, 135, 225),
             ("arc", 32, 18, 19, 135, 225)],
    "recording": [("line", 6, 4, 13, 4), ("line", 6, 4, 6, 10), ("line", 51, 4, 58, 4), ("line", 58, 4, 58, 10),
                  ("line", 6, 32, 13, 32), ("line", 6, 26, 6, 32), ("line", 51, 32, 58, 32), ("line", 58, 26, 58, 32), ("dot", 32, 18, 9)],
    "paused": [("ring", 32, 18, 15), ("box", 25, 10, 30, 26), ("box", 34, 10, 39, 26)],
    "muted": [("frame", 27, 3, 37, 20), ("line", 22, 13, 22, 21), ("line", 42, 13, 42, 21), ("line", 22, 22, 42, 22),
              ("line", 32, 22, 32, 29), ("line", 24, 31, 40, 31), ("line", 16, 33, 48, 3)],
    "offline": [("frame", 10, 3, 54, 25), ("line", 32, 25, 32, 30), ("line", 22, 31, 42, 31), ("ring", 32, 14, 7), ("line", 27, 9, 37, 19)],
    "privacy": [("ring", 32, 15, 10), ("dot", 32, 15, 4), ("line", 32, 25, 32, 30), ("line", 22, 31, 42, 31), ("line", 16, 33, 48, 2)],
    "raid": [("dot", 9, 18, 3.5), ("dot", 19, 18, 3.5), ("dot", 29, 18, 3.5)] + arrow(36, 18, 58, 18, 6),
    # A bar filling up to a flag.
    "events": [("frame", 4, 16, 50, 26), ("box", 4, 16, 32, 26), ("line", 56, 4, 56, 32), ("tri", 56, 4, 63, 8, 56, 12)],
    # A stopwatch.
    "countdown": [("ring", 32, 20, 13), ("box", 29, 2, 35, 6), ("line", 32, 20, 32, 11), ("line", 32, 20, 39, 24)],
    # A badge with a star.
    "logo": [("ring", 32, 18, 16), ("tri", 32, 6, 42, 26, 22, 26), ("tri", 32, 30, 22, 12, 42, 12)],
    "technical": [("line", 32, 3, 13, 33), ("line", 13, 33, 51, 33), ("line", 51, 33, 32, 3), ("line", 32, 13, 32, 24),
                  ("dot", 32, 28.5, 2)],
}
for _type, _shapes in MORE_TYPES.items():
    DRAWINGS[type_name(_type)] = _shapes

# Scene types the owner's scene pictures (the "scenes" group) have nothing
# for: each a scene of that kind laid out. Ids in src/key-types.json's
# sceneTypes; the picture is options/scene-type-<id>.
_BARS = [("box", 40, 7, 58, 11), ("box", 40, 15, 58, 19), ("box", 40, 23, 58, 27)]
SCENE_TYPES = {
    "countdown": [("frame", 2, 2, 62, 34), ("ring", 32, 19, 10), ("box", 30, 5, 34, 8), ("line", 32, 19, 32, 12), ("line", 32, 19, 37, 22)],
    "technical": [("frame", 2, 2, 62, 34), ("line", 32, 6, 18, 30), ("line", 18, 30, 46, 30), ("line", 46, 30, 32, 6),
                  ("line", 32, 13, 32, 22), ("dot", 32, 26, 1.8)],
    # A computer game: a monitor with a gamepad, and a keyboard.
    "pc_game": [("frame", 8, 2, 56, 24), ("line", 20, 13, 28, 13), ("line", 24, 9, 24, 17), ("dot", 38, 11, 2), ("dot", 43, 15, 2),
                ("box", 12, 28, 46, 33), ("dot", 53, 30.5, 3)],
    # Retro gaming: a sprite of big square pixels.
    "retro": [("box", 22, 4, 28, 9), ("box", 36, 4, 42, 9), ("box", 18, 9, 46, 15), ("box", 14, 15, 22, 21), ("box", 28, 15, 36, 21),
              ("box", 42, 15, 50, 21), ("box", 14, 21, 50, 27), ("box", 18, 27, 24, 33), ("box", 40, 27, 46, 33)],
    # Activities.
    "speedrun": [("ring", 24, 20, 12), ("box", 21, 3, 27, 6), ("line", 24, 20, 24, 12), ("line", 24, 20, 30, 24),
                 ("line", 46, 4, 46, 33), ("box", 46, 4, 52, 9), ("box", 52, 9, 58, 14), ("frame", 46, 4, 58, 14)],
    "music_production": [("frame", 4, 6, 60, 32), ("line", 12, 6, 12, 32), ("line", 20, 6, 20, 32), ("line", 28, 6, 28, 32),
                         ("line", 36, 6, 36, 32), ("line", 44, 6, 44, 32), ("line", 52, 6, 52, 32), ("box", 9, 6, 15, 20),
                         ("box", 17, 6, 23, 20), ("box", 33, 6, 39, 20), ("box", 41, 6, 47, 20), ("box", 49, 6, 55, 20)],
    "cooking": [("ring", 26, 23, 11), ("line", 37, 23, 60, 23), ("line", 20, 4, 22, 8), ("line", 22, 8, 20, 11),
                ("line", 27, 3, 29, 7), ("line", 29, 7, 27, 10), ("line", 32, 4, 34, 8)],
    "irl": [("ring", 32, 12, 8), ("dot", 32, 12, 3), ("tri", 25, 16, 39, 16, 32, 30), ("line", 12, 33, 52, 33)],
    "tutorial": [("frame", 4, 3, 44, 27), ("line", 10, 10, 30, 10), ("line", 10, 16, 36, 16), ("line", 10, 22, 26, 22),
                 ("line", 38, 33, 30, 18)] + _person(53, 22, 9),
    "unboxing": [("frame", 14, 16, 50, 33), ("line", 14, 16, 6, 8), ("line", 50, 16, 58, 8), ("line", 22, 16, 28, 6), ("line", 42, 16, 36, 6)],
    "fitness": [("line", 14, 18, 50, 18), ("box", 8, 8, 14, 28), ("box", 50, 8, 56, 28), ("box", 3, 12, 8, 24), ("box", 56, 12, 61, 24)],
    "reading": [("line", 32, 6, 32, 32), ("line", 32, 6, 8, 3), ("line", 8, 3, 8, 29), ("line", 8, 29, 32, 32), ("line", 32, 6, 56, 3),
                ("line", 56, 3, 56, 29), ("line", 56, 29, 32, 32), ("line", 13, 12, 27, 13), ("line", 37, 13, 51, 12)],
    "watch_party": [("frame", 10, 2, 54, 20), ("tri", 28, 6, 28, 16, 37, 11)] + _person(17, 30, 6) + _person(32, 30, 6) + _person(47, 30, 6),
    "charity": [("dot", 24, 14, 9), ("dot", 40, 14, 9), ("tri", 15.5, 17, 48.5, 17, 32, 33)],
    # The owner's console-and-handheld layout has its own picture; this is
    # a classic one.
    # An old television with a game on it.
    "classic": [("frame", 6, 7, 48, 34), ("frame", 10, 11, 38, 30), ("dot", 43, 15, 2), ("dot", 43, 22, 2), ("line", 22, 7, 16, 1),
                ("line", 30, 7, 36, 1), ("line", 17, 20, 23, 20), ("line", 20, 17, 20, 23), ("dot", 30, 19, 1.8)],
    "emulator": [("frame", 2, 2, 36, 34), ("box", 2, 2, 36, 7), ("line", 9, 20, 17, 20), ("line", 13, 16, 13, 24), ("dot", 25, 18, 2),
                 ("dot", 30, 22, 2)] + _BARS,
    "second_screen": [("frame", 2, 6, 30, 28), ("frame", 34, 6, 62, 28), ("box", 6, 10, 26, 24), ("line", 38, 12, 58, 12),
                      ("line", 38, 18, 54, 18), ("line", 38, 24, 50, 24)],
    "art": [("frame", 4, 3, 42, 33), ("arc", 15, 20, 6, 180, 360), ("arc", 27, 20, 6, 0, 180), ("line", 47, 31, 60, 5), ("dot", 46, 32, 3)],
    # A cube, as a 3D model is drawn.
    "modelling": [("frame", 14, 12, 36, 32), ("frame", 26, 4, 48, 24), ("line", 14, 12, 26, 4), ("line", 36, 12, 48, 4),
                  ("line", 36, 32, 48, 24), ("line", 14, 32, 26, 24)],
    "coding": [("frame", 2, 2, 62, 34), ("box", 2, 2, 62, 8), ("line", 22, 13, 14, 21), ("line", 14, 21, 22, 29), ("line", 42, 13, 50, 21),
               ("line", 50, 21, 42, 29), ("line", 36, 12, 28, 30)],
    "browsing": [("frame", 2, 2, 62, 34), ("box", 2, 2, 62, 8), ("ring", 32, 21, 9), ("line", 23, 21, 41, 21), ("line", 32, 12, 32, 30),
                 ("arc", 32, 21, 5, 270, 450)],
    "just_chatting": _person(18, 20, 13) + _BARS,
    "interview": _person(17, 19, 11) + _person(47, 19, 11) + [("line", 32, 4, 32, 32)],
    "podcast": [("frame", 27, 3, 37, 21), ("line", 22, 14, 22, 22), ("line", 42, 14, 42, 22), ("line", 22, 23, 42, 23),
                ("line", 32, 23, 32, 30), ("line", 24, 32, 40, 32), ("arc", 32, 12, 17, -35, 35), ("arc", 32, 12, 17, 145, 215)],
    "music": [("box", 6, 22, 12, 34), ("box", 15, 14, 21, 34), ("box", 24, 18, 30, 34), ("box", 33, 9, 39, 34), ("dot", 51, 28, 4.5),
              ("line", 55, 28, 55, 6), ("line", 55, 6, 61, 10)],
}
for _scene, _shapes in SCENE_TYPES.items():
    DRAWINGS["scene-type-" + _scene.replace("_", "-")] = _shapes

# Styles a key's image can show (src/key-types.json's looks): more blurs and
# mosaic cells than the pack's own, corner and edge looks, shadow looks and
# transitions. Each is options/style-<name>, in the "styles" group.
_GRID = [("frame", 4, 2, 60, 34)]
STYLES = {
    "mosaic-circle": _GRID + [("dot", x, y, 4.2) for x in (12, 24, 36, 48) for y in (10, 26)] + [("dot", x, 18, 4.2) for x in (18, 30, 42, 54)],
    "mosaic-triangle": _GRID + [("tri", x, 6, x + 12, 6, x + 6, 17) for x in (8, 22, 36)] + [("tri", x + 7, 19, x, 30, x + 14, 30) for x in (8, 22, 36)],
    "mosaic-voronoi": _GRID + [("line", 4, 14, 20, 10), ("line", 20, 10, 28, 2), ("line", 20, 10, 26, 22), ("line", 26, 22, 14, 34),
                               ("line", 26, 22, 42, 20), ("line", 42, 20, 46, 2), ("line", 42, 20, 50, 34), ("line", 42, 20, 60, 16)],
    "mosaic-rhomboid": _GRID + [("line", 4, 2, 36, 34), ("line", 22, 2, 54, 34), ("line", 40, 2, 60, 22), ("line", 4, 20, 18, 34),
                                ("line", 28, 2, 4, 26), ("line", 46, 2, 14, 34), ("line", 60, 6, 32, 34), ("line", 60, 24, 50, 34)],
    "mosaic-triakis": _GRID + [("line", 20, 2, 12, 18), ("line", 12, 18, 20, 34), ("line", 44, 2, 52, 18), ("line", 52, 18, 44, 34),
                               ("line", 20, 2, 44, 2), ("line", 20, 34, 44, 34), ("line", 12, 18, 52, 18), ("line", 20, 2, 44, 34),
                               ("line", 44, 2, 20, 34), ("dot", 32, 18, 2.5)],
    "glass": [("frame", 6, 3, 30, 17), ("frame", 34, 3, 58, 17), ("frame", 6, 19, 30, 33), ("frame", 34, 19, 58, 33),
              ("line", 10, 7, 16, 7), ("line", 38, 7, 44, 7), ("line", 10, 23, 16, 23), ("line", 38, 23, 44, 23)],
    "trail": [("frame", 6, 12, 18, 24), ("frame", 16, 10, 30, 26), ("box", 30, 7, 50, 29)] + arrow(52, 18, 60, 18, 3),
    # Corners & Edges
    "edge-soft": [("box", 22, 11, 42, 25), ("frame", 17, 7, 47, 29), ("frame", 12, 3, 52, 33)],
    "edge-outline": [("box", 16, 9, 48, 27), ("frame", 10, 4, 54, 32)],
    "edge-trim": [("box", 16, 9, 48, 27), ("line", 4, 9, 12, 9), ("line", 16, 1, 16, 5), ("line", 52, 9, 60, 9), ("line", 48, 1, 48, 5),
                  ("line", 4, 27, 12, 27), ("line", 16, 31, 16, 35), ("line", 52, 27, 60, 27), ("line", 48, 31, 48, 35)],
    # Shadow, Glow & Outline
    "shadow-hard": [("box", 22, 11, 52, 33), ("frame", 12, 3, 44, 25), ("box", 14, 5, 42, 23)],
    "shadow-long": [("frame", 10, 4, 32, 22), ("line", 32, 4, 56, 28), ("line", 32, 22, 46, 34), ("line", 10, 22, 22, 34),
                    ("line", 56, 28, 50, 34)],
    "shadow-neon": [("frame", 18, 10, 46, 26), ("frame", 12, 5, 52, 31), ("line", 6, 18, 10, 18), ("line", 54, 18, 58, 18),
                    ("line", 32, 1, 32, 3), ("line", 32, 33, 32, 35)],
    # Transitions
    "fade": [("frame", 6, 4, 38, 26), ("box", 26, 10, 58, 32)],
    "cut": [("frame", 4, 8, 26, 28), ("box", 38, 8, 60, 28), ("line", 32, 2, 32, 34)],
    "slide": [("frame", 4, 6, 60, 30), ("box", 4, 6, 26, 30)] + arrow(30, 18, 54, 18, 4),
    "swipe": [("frame", 4, 6, 60, 30), ("arc", 32, 34, 18, 210, 330)] + arrow(46, 23, 49, 26, 4),
    "wipe": [("frame", 4, 4, 60, 32), ("tri", 4, 4, 40, 4, 4, 32), ("line", 40, 4, 22, 32)],
    "zoom": [("frame", 24, 12, 40, 24), ("frame", 6, 2, 58, 34)] + arrow(22, 10, 12, 5, 4) + arrow(42, 26, 52, 31, 4),
    "morph": [("box", 6, 9, 24, 27), ("ring", 49, 18, 9)] + arrow(27, 18, 37, 18, 4),
    "make-room": [("frame", 3, 6, 61, 30), ("box", 7, 11, 23, 25), ("frame", 44, 11, 56, 25)] + arrow(26, 18, 40, 18, 4),
}
for _style, _shapes in STYLES.items():
    DRAWINGS["style-" + _style] = _shapes
DRAWINGS["match-item"] = [("frame", 5, 4, 31, 23), ("frame", 33, 13, 59, 32), ("line", 13, 27, 28, 27), ("line", 13, 31, 28, 31)]
DRAWINGS["scene-transitions"] = [("frame", 3, 4, 25, 24), ("frame", 39, 12, 61, 32)] + arrow(21, 18, 43, 18, 5)


def rounded(x0, y0, x1, y1, r):
    """A frame with round corners, as lines and arcs."""
    return [("line", x0 + r, y0, x1 - r, y0), ("line", x0 + r, y1, x1 - r, y1), ("line", x0, y0 + r, x0, y1 - r),
            ("line", x1, y0 + r, x1, y1 - r), ("arc", x0 + r, y0 + r, r, 180, 270), ("arc", x1 - r, y0 + r, r, 270, 360),
            ("arc", x1 - r, y1 - r, r, 0, 90), ("arc", x0 + r, y1 - r, r, 90, 180)]


# The Scene Mode key: modes stacked, one of them on.
DRAWINGS["mode"] = [("frame", 14, 2, 50, 10), ("box", 14, 14, 50, 22), ("frame", 14, 26, 50, 34)]
# The scene filters' keys: a scene with its sources, and what the filter does
# to all of them at once.
DRAWINGS["scene-fit"] = [("frame", 4, 2, 60, 34), ("box", 9, 7, 38, 29), ("box", 46, 7, 55, 29)] + arrow(39, 18, 44, 18, 3)
DRAWINGS["scene-shadow"] = [("frame", 2, 2, 62, 34), ("box", 12, 11, 31, 29), ("frame", 8, 7, 27, 25), ("box", 39, 11, 58, 29),
                            ("frame", 35, 7, 54, 25)]
DRAWINGS["scene-corners"] = [("frame", 2, 2, 62, 34)] + rounded(7, 7, 35, 29, 7) + rounded(39, 7, 57, 29, 5)



def bounds(shapes, pen=2.6):
    """Where a drawing's ink is, in its own units: (x0, y0, x1, y1). Outlines
    are drawn inside their box (as Pillow does); a line is a pen wide."""
    import math

    xs, ys = [], []
    h = pen / 2
    for shape in shapes:
        kind, v = shape[0], shape[1:]
        if kind in ("box", "frame"):
            xs += [v[0], v[2]]
            ys += [v[1], v[3]]
        elif kind == "line":
            xs += [v[0] - h, v[0] + h, v[2] - h, v[2] + h]
            ys += [v[1] - h, v[1] + h, v[3] - h, v[3] + h]
        elif kind in ("ring", "dot"):
            xs += [v[0] - v[2], v[0] + v[2]]
            ys += [v[1] - v[2], v[1] + v[2]]
        elif kind == "arc":
            cx, cy, r, start, end = v
            end = end if end >= start else end + 360
            for a in range(int(start), int(end) + 1):
                xs.append(cx + r * math.cos(math.radians(a)))
                ys.append(cy + r * math.sin(math.radians(a)))
        elif kind == "tri":
            xs += [v[0], v[2], v[4]]
            ys += [v[1], v[3], v[5]]
    return min(xs), min(ys), max(xs), max(ys)


def centred(shapes):
    """A drawing moved so its ink is in the middle of its 64 x 36 field (and
    so in the middle of whatever it is drawn in). Words stay where they are:
    they are always drawn in the middle."""
    x0, y0, x1, y1 = bounds([s for s in shapes if s[0] != "text"])
    dx, dy = 32 - (x0 + x1) / 2, 18 - (y0 + y1) / 2
    out = []
    for shape in shapes:
        kind, v = shape[0], list(shape[1:])
        if kind in ("ring", "dot", "arc"):
            v[0] += dx
            v[1] += dy
        elif kind != "text":
            v = [c + (dx if i % 2 == 0 else dy) for i, c in enumerate(v)]
        out.append((kind, *v))
    return out


# Every drawing centred (the owner: "make them all centered. fix the icons
# universally"); the hook keys' types again from the centred types.
for _name in list(DRAWINGS):
    DRAWINGS[_name] = centred(DRAWINGS[_name])
for _type in SOURCE_TYPES:
    HOOK_TYPES["hook-type-" + _type.replace("_", "-")] = _CORNERS + scaled(DRAWINGS[type_name(_type)], 0.7)
DRAWINGS.update(HOOK_TYPES)

# The ones that are a filter's key, with an "on" and an "off" look.
SWITCHES = ["scene", "zoom", "fit", "overlap", "source-switcher", "window-switcher", "trigger", "link", "show-hide", "limit", "slide-to-edge", "every-scene", "keep-shape", "snap", "guides", "plain-switch"]
SWITCHES += list(GROUPED)
SWITCHES += [type_name(t) for t in TYPES] + ["mode", "scene-fit", "scene-shadow", "scene-corners"]
SWITCHES += [type_name(t) for t in MORE_TYPES] + ["scene-type-" + s.replace("_", "-") for s in SCENE_TYPES]
SWITCHES += ["style-" + s for s in STYLES]
SWITCHES += ["match-item", "scene-transitions"]


def drawn(shapes, ink, box=SCREEN, size=1.0):
    """A drawing on a transparent layer the size of `box` (the key's little
    screen, or a layout's main panel), its 64 x 36 field in the middle of it
    at `size` times its own units."""
    s = SCALE
    w, h = (box[2] - box[0]) * s, (box[3] - box[1]) * s
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    # The field of 64 x 36 in the middle of the screen.
    k = s * size
    ox, oy = (w - 64 * k) / 2, (h - 36 * k) / 2
    pen = int(2.6 * k)

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
        elif kind == "arc":
            cx, cy, r, start, end = shape[1:]
            draw.arc((at(cx - r, cy - r), at(cx + r, cy + r)), start, end, fill=ink, width=pen)
        elif kind == "tri":
            draw.polygon((at(shape[1], shape[2]), at(shape[3], shape[4]), at(shape[5], shape[6])), fill=ink)
        elif kind == "text":
            font = ImageFont.truetype(FONT, int(22 * k))
            tb = draw.textbbox((0, 0), shape[1], font=font)
            draw.text(((w - tb[2] - tb[0]) / 2, (h - tb[3] - tb[1]) / 2), shape[1], font=font, fill=ink)
    return layer.resize((box[2] - box[0], box[3] - box[1]), Image.LANCZOS)

# ---------------------------------------------------------------- layouts
# Every type in the pack's two scene layouts (the owner: "every icon should
# have a standalone version, live/no chat version, and a component/chat
# version", and "perhaps have a selector to choose the layout and a second
# selector for the specific type"). The layouts are the pack's own: Live (no
# chat) is its game capture scene (a hexagon frame, the big panel, a small
# camera top right), Live + chat its game component scene (the panel on the
# left, a column of camera over chat on the right). Each is taken from the
# pack's picture in that palette with its gamepad taken out of the panel
# (the "frame"), and each type's drawing is put in the panel where the
# gamepad was, game included: one template for each layout, the same for
# every type but the drawing. src/layouts.json says which picture is which;
# src/pictures.ts (`laidOut`) chooses from it.
LAYOUTS = ("live", "chat")
LAYOUT_FROM = {"live": "scenes/02-game-capture", "chat": "scenes/03-game-component"}
# The main panel of each (the same in every palette), and the part of it the
# pack's gamepad (with its soft edge) takes.
PANEL = {"live": (27, 17, 117, 82), "chat": (8, 17, 98, 83)}
GLYPH = {"live": (45, 31, 99, 68), "chat": (25, 31, 80, 68)}
# A drawing in a panel is this much larger than on a key's little screen.
LAYOUT_SIZE = 1.1
# Every type is drawn in the same two layouts, game, window and capture card
# too (the owner: "they should all be consistent. for the live the square
# camera image should be in the same position for all"): the pack's own
# window and capture card scenes put the camera elsewhere or show a plain
# bar. The pack's scenes stay what they are as scene types.
# The pack's scene pictures, each by the drawing of what it shows: what goes
# in a layout's panel for that scene. Those that are a layout themselves are
# also that drawing alone when Standalone is chosen (STANDALONE).
GLYPH_OF = {
    "scenes/01-stream-start": "options/mode-starting",
    "scenes/02-game-capture": "options/type-game",
    "scenes/03-game-component": "options/type-game",
    "scenes/04-console": "options/console",
    "scenes/05-game-hd60s": "options/type-capture-card",
    "scenes/06-game-component-hd60s": "options/type-capture-card",
    "scenes/07-window-capture": "options/type-window",
    "scenes/08-window-component": "options/type-window",
    "scenes/09-main-display": "options/type-display",
    "scenes/10-tablet": "options/type-tablet",
    "scenes/11-dolphin-gba": "options/type-linked-handheld",
    "scenes/12-camera-only": "options/type-full-camera",
    "scenes/13-intermission": "options/mode-intermission",
    "scenes/14-be-right-back": "options/mode-brb",
    "scenes/15-stream-ending": "options/mode-ending",
}
STANDALONE = ["scenes/02-game-capture", "scenes/03-game-component", "scenes/05-game-hd60s", "scenes/06-game-component-hd60s",
              "scenes/07-window-capture", "scenes/08-window-component"]
# Where a key's whole picture is (every picture of the pack is drawn there):
# what is laid into a layout's panel, made smaller, for a picture with no
# layout of its own (src/compose.ts).
CROP = (3, 11, 141, 88)
TYPES_LIST = os.path.join(ROOT, "src", "key-types.json")
LAYOUT_LIST = os.path.join(ROOT, "src", "layouts.json")


def layout_drawings():
    """The drawings that get a picture in each layout, by picture id
    (options/<name>): every source type's and scene type's that is a drawing
    here, and the drawings of the pack's scene pictures."""
    types = json.load(open(TYPES_LIST, encoding="utf-8"))
    ids = [t["base"] for t in types["sourceTypes"]] + [t["picture"] for t in types["sceneTypes"]] + list(GLYPH_OF.values())
    out = []
    for pid in ids:
        name = pid[len("options/"):] if pid.startswith("options/") else None
        if name and name in DRAWINGS and name not in out:
            out.append(name)
    return out


def layout_frame(palette, layout):
    """The pack's layout picture in a palette with its gamepad taken out, the
    colour of the panel and the colour the gamepad was drawn in."""
    image = Image.open(os.path.join(PACK, palette, LAYOUT_FROM[layout] + ".png")).convert("RGBA")
    x0, y0, x1, y1 = GLYPH[layout]
    ink = max((image.getpixel((x, y)) for x in range(x0, x1) for y in range(y0, y1)), key=lambda c: sum(c[:3]))
    panel = image.getpixel((PANEL[layout][0] + 3, PANEL[layout][1] + 3))
    ImageDraw.Draw(image).rectangle((x0, y0, x1 - 1, y1 - 1), fill=panel)
    return image, ink


def layout_pictures(palettes):
    """Draws imgs/pack/<palette>/layouts/: the two frames, and each drawing in
    each layout. Returns the pictures to list in src/pack.json and writes
    src/layouts.json."""
    names = layout_drawings()
    made = 0
    for palette in palettes:
        out = os.path.join(PACK, palette, "layouts")
        os.makedirs(out, exist_ok=True)
        for layout in LAYOUTS:
            frame, ink = layout_frame(palette, layout)
            frame.save(os.path.join(out, f"{layout}-frame.png"))
            box = PANEL[layout]
            for name in names:
                picture = frame.copy()
                picture.alpha_composite(drawn(DRAWINGS[name], ink, box, LAYOUT_SIZE), (box[0], box[1]))
                picture.save(os.path.join(out, f"{layout}-{name}.png"))
                made += 1
    pictures = []
    for layout in LAYOUTS:
        word = "live" if layout == "live" else "live chat"
        pictures.append({"id": f"layouts/{layout}-frame", "group": "layouts", "label": f"{word} frame",
                         "on": f"layouts/{layout}-frame", "off": f"layouts/{layout}-frame"})
        for name in names:
            pid = f"layouts/{layout}-{name}"
            pictures.append({"id": pid, "group": "layouts", "label": f"{word} {name.replace('-', ' ')}", "on": pid, "off": pid})
    by_picture = {}
    for name in names:
        by_picture["options/" + name] = {layout: f"layouts/{layout}-{name}" for layout in LAYOUTS}
    listed = {
        "_": "Written by tools/make-option-icons.py. Each picture's place in the two layouts (live: no chat, chat: live + chat), "
             "by the picture a key shows alone (byPicture); the drawing a scene picture of the pack shows (glyphOf); the scene pictures "
             "that are a layout themselves and are their drawing alone when Standalone is chosen (standalone); and, for a picture with "
             "no layout of its own, the empty layout (frames), its panel and the part of a key's picture laid into it (panel, crop).",
        "layouts": list(LAYOUTS),
        "frames": {layout: f"layouts/{layout}-frame" for layout in LAYOUTS},
        "panel": {layout: list(PANEL[layout]) for layout in LAYOUTS},
        "crop": list(CROP),
        "byPicture": by_picture,
        "glyphOf": GLYPH_OF,
        "standalone": STANDALONE,
    }
    with open(LAYOUT_LIST, "w", encoding="utf-8") as f:
        json.dump(listed, f, indent="\t", ensure_ascii=False)
        f.write("\n")
    return pictures, made


def main():
    listed = json.load(open(LIST, encoding="utf-8"))
    palettes = listed["colours"]
    stripped = 0
    for palette in palettes:
        for folder, _, files in os.walk(os.path.join(PACK, palette)):
            # (Not the pictures drawn here, nor the marked copies of
            # tools/make-state-icons.py: their dot and slash are no words.)
            parts = os.path.relpath(folder, os.path.join(PACK, palette)).split(os.sep)
            if parts[0] in ("options", "status", "layouts", "badges", "live"):
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
        ImageDraw.Draw(base).rectangle(ERASE, fill=inside)
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
    pictures = [p for p in listed["pictures"] if not p["id"].startswith(("options/", "layouts/"))]
    for name in DRAWINGS:
        two = name in SWITCHES
        if name in GROUPED:
            group, label = GROUPED[name]
        elif name in DEVICES:
            group, label = "devices", name.replace("-", " ")
        elif name.startswith("style-"):
            group, label = "styles", name[6:].replace("-", " ")
        elif name.startswith("scene-type-"):
            group, label = "scene types", name[11:].replace("-", " ")
        elif name.startswith("type-"):
            group, label = "source types", name[5:].replace("-", " ")
        elif name in HOOK_TYPES:
            group, label = "capture by type", name[10:].replace("-", " ")
        elif name.startswith("ratio-"):
            group, label = "options", name.replace("-", " ").replace("x", ":")
        else:
            group, label = "options", name.replace("-", " ")
        pictures.append(
            {
                "id": "options/" + name,
                "group": group,
                "label": label,
                "on": f"options/{name}-on" if two else f"options/{name}",
                "off": f"options/{name}-off" if two else f"options/{name}",
            }
        )
    laid, drawn_in_layouts = layout_pictures(palettes)
    pictures += laid
    made += drawn_in_layouts
    listed["pictures"] = pictures
    with open(LIST, "w", encoding="utf-8") as f:
        json.dump(listed, f, indent="\t", ensure_ascii=False)
        f.write("\n")
    print(f"{stripped} pictures had their words taken out; {made} pictures drawn; {len(pictures)} listed")


if __name__ == "__main__":
    main()
