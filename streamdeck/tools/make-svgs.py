#!/usr/bin/env python3
"""Editable SVGs of every picture drawn here (needs Pillow).

The owner asked for "the full icon set with editable SVGs". This writes, into
streamdeck/svg/:

  action-icons/<key>.svg   the small icons in Stream Deck's list of actions
                           (tools/make-action-icons.py), white lines
  keys/<palette>/<group>/<name>.svg
                           every key picture drawn by tools/make-option-icons.py
                           (options, devices, the scene key), in each palette:
                           the drawing as shapes in a layer of its own, over the
                           pack's key background, with the off look and the on
                           and off marks (tools/make-state-icons.py) as layers
                           to show or hide
  marks/on.svg, off.svg    the two marks by themselves

The pictures from the original icon pack came here as PNG only, so they have
no SVG here: see svg/README.md. The drawings are made by the same code as the
PNGs, through tools/svgdraw.py, so the two always match.
Run from the streamdeck folder:  python3 tools/make-svgs.py
"""
import base64
import importlib.util
import io
import json
import os
import shutil

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
OUT = os.path.join(ROOT, "svg")
import sys

sys.path.insert(0, HERE)
from svgdraw import SvgDraw, colour, n  # noqa: E402


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ACTIONS = load("make-action-icons")
OPTIONS = load("make-option-icons")

HEAD = (
    '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
    'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
    'width="{w}" height="{h}" viewBox="0 0 {vw} {vh}">\n'
)


def layer(label, body, shown=True):
    style = "" if shown else ' style="display:none"'
    ident = "".join(c if c.isalnum() else "-" for c in label.lower()).strip("-")
    return f'<g id="{ident}" inkscape:groupmode="layer" inkscape:label="{label}"{style}>\n{body}\n</g>\n'


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def action_icons():
    made = 0
    for key, draw_fn in ACTIONS.ICONS.items():
        d = SvgDraw()
        inner = getattr(draw_fn, "inner", None)
        defs = ""
        if inner:
            inner(d)
            defs = (
                f'<defs><filter id="soft" x="-30%" y="-30%" width="160%" height="160%">'
                f'<feGaussianBlur stdDeviation="{n(draw_fn.radius * 0.6)}"/></filter></defs>\n'
            )
            body = f'<g filter="url(#soft)">\n{d.body()}\n</g>'
        else:
            draw_fn(d)
            body = d.body()
        svg = HEAD.format(w=40, h=40, vw=ACTIONS.S, vh=ACTIONS.S) + defs + layer("Icon", body) + "</svg>\n"
        write(os.path.join(OUT, "action-icons", key + ".svg"), svg)
        made += 1
    return made


def marks(size=144):
    """The two marks as SVG, as tools/make-state-icons.py draws them."""
    g, _ = colour((60, 205, 100, 255))
    r_, _ = colour((228, 64, 64, 255))
    e, _ = colour((12, 12, 14, 255))
    on = f'<circle cx="19" cy="19" r="16" fill="{e}"/>\n<circle cx="19" cy="19" r="13" fill="{g}"/>'
    k = round(9 * 0.72, 2)
    off = (
        f'<circle cx="19" cy="19" r="16" fill="{e}"/>\n'
        f'<circle cx="19" cy="19" r="13" fill="{r_}"/>\n'
        f'<circle cx="19" cy="19" r="9" fill="{e}"/>\n'
        f'<line x1="{19 - k}" y1="{19 - k}" x2="{19 + k}" y2="{19 + k}" stroke="{r_}" stroke-width="4"/>'
    )
    return on, off


def drawing(shapes, ink):
    """A drawing of make-option-icons.py, as shapes, where it sits on the key."""
    sx0, sy0, sx1, sy1 = OPTIONS.SCREEN
    ox, oy = sx0 + ((sx1 - sx0) - 64) / 2, sy0 + ((sy1 - sy0) - 36) / 2
    pen = int(2.6 * OPTIONS.SCALE) / OPTIONS.SCALE
    d = SvgDraw()

    def at(x, y):
        return (ox + x, oy + y)

    for shape in shapes:
        kind = shape[0]
        if kind == "box":
            d.rectangle((*at(shape[1], shape[2]), *at(shape[3], shape[4])), fill=ink)
        elif kind == "frame":
            d.rectangle((*at(shape[1], shape[2]), *at(shape[3], shape[4])), outline=ink, width=pen)
        elif kind == "line":
            d.line((*at(shape[1], shape[2]), *at(shape[3], shape[4])), fill=ink, width=pen)
        elif kind == "ring":
            cx, cy, r = shape[1:]
            d.ellipse((*at(cx - r, cy - r), *at(cx + r, cy + r)), outline=ink, width=pen)
        elif kind == "dot":
            cx, cy, r = shape[1:]
            d.ellipse((*at(cx - r, cy - r), *at(cx + r, cy + r)), fill=ink)
        elif kind == "tri":
            d.polygon([at(shape[1], shape[2]), at(shape[3], shape[4]), at(shape[5], shape[6])], fill=ink)
        elif kind == "text":
            d.text(((sx0 + sx1) / 2, (sy0 + sy1) / 2), shape[1], fill=ink, size=22)
    return d.body()


def key_pictures():
    listed = json.load(open(OPTIONS.LIST, encoding="utf-8"))
    groups = {p["id"].split("/", 1)[1]: p["group"] for p in listed["pictures"] if p["id"].startswith("options/")}
    on_mark, off_mark = marks()
    made = 0
    for palette in listed["colours"]:
        # The key's background, as make-option-icons.py makes it: the pack's
        # own key with its little screen emptied.
        base = Image.open(os.path.join(OPTIONS.PACK, palette, "filters", "next-capture.png")).convert("RGBA")
        inside = base.getpixel((30, 60))
        ink = max((base.getpixel((x, y)) for x in range(40, 90) for y in range(36, 62)), key=lambda c: sum(c[:3]))
        ground = base.getpixel((2, 2))
        ImageDraw.Draw(base).rectangle(OPTIONS.SCREEN, fill=inside)
        buf = io.BytesIO()
        base.save(buf, format="PNG", optimize=True)
        href = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")
        background = f'<image width="{OPTIONS.SIZE}" height="{OPTIONS.SIZE}" xlink:href="{href}" href="{href}"/>'
        gc, _ = colour(ground)
        for name, shapes in OPTIONS.DRAWINGS.items():
            svg = HEAD.format(w=OPTIONS.SIZE, h=OPTIONS.SIZE, vw=OPTIONS.SIZE, vh=OPTIONS.SIZE)
            svg += layer("Background (the pack's key)", background)
            svg += layer("Drawing", drawing(shapes, ink))
            if name in OPTIONS.SWITCHES:
                svg += layer("Off look (show for the off picture)", f'<rect width="{OPTIONS.SIZE}" height="{OPTIONS.SIZE}" fill="{gc}" fill-opacity="0.2"/>', False)
            svg += layer("Mark: on (green dot)", on_mark, False)
            svg += layer("Mark: off (red no sign)", off_mark, False)
            svg += "</svg>\n"
            write(os.path.join(OUT, "keys", palette, groups.get(name, "options"), name + ".svg"), svg)
            made += 1
    head = HEAD.format(w=144, h=144, vw=144, vh=144)
    write(os.path.join(OUT, "marks", "on.svg"), head + layer("Mark: on", on_mark) + "</svg>\n")
    write(os.path.join(OUT, "marks", "off.svg"), head + layer("Mark: off", off_mark) + "</svg>\n")
    return made


def main():
    if os.path.isdir(OUT):
        for sub in ("action-icons", "keys", "marks"):
            shutil.rmtree(os.path.join(OUT, sub), ignore_errors=True)
    a = action_icons()
    k = key_pictures()
    print(f"{a} action icons and {k} key pictures written as SVG")


if __name__ == "__main__":
    main()
