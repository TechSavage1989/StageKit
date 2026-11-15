"""A stand-in for Pillow's ImageDraw that writes SVG shapes instead of pixels.

The drawing tools (make-action-icons.py, make-option-icons.py) draw with
ImageDraw calls. Handed one of these instead, the same code makes an editable
SVG of the same picture: tools/make-svgs.py does that. Only the calls those
tools use are here, with Pillow's meaning: an outline is drawn inside the
shape's box, angles are degrees clockwise from three o'clock.
"""
import math


def colour(c):
    """An (r, g, b[, a]) of Pillow's as an SVG colour and opacity."""
    if c is None:
        return "none", 1.0
    if isinstance(c, str):
        return c, 1.0
    r, g, b = c[:3]
    a = c[3] if len(c) > 3 else 255
    return f"#{r:02x}{g:02x}{b:02x}", round(a / 255, 3)


def n(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


class SvgDraw:
    def __init__(self, scale=1.0):
        self.k = scale  # from the drawing's own units to the SVG's
        self.items = []

    def _paint(self, fill=None, outline=None, width=0):
        f, fo = colour(fill)
        o, oo = colour(outline)
        out = f'fill="{f}"' + (f' fill-opacity="{fo}"' if fo < 1 else "")
        if outline is not None and width:
            out += f' stroke="{o}" stroke-width="{n(width * self.k)}"' + (f' stroke-opacity="{oo}"' if oo < 1 else "")
        return out

    def _box(self, xy, width, outline):
        x0, y0, x1, y1 = (v * self.k for v in xy)
        if outline is not None and width:  # (Pillow draws the outline inside)
            h = width * self.k / 2
            x0, y0, x1, y1 = x0 + h, y0 + h, x1 - h, y1 - h
        return x0, y0, x1, y1

    def rectangle(self, xy, fill=None, outline=None, width=1):
        xy = [c for p in xy for c in (p if isinstance(p, tuple) else (p,))]
        x0, y0, x1, y1 = self._box(xy, width, outline)
        self.items.append(f'<rect x="{n(x0)}" y="{n(y0)}" width="{n(x1 - x0)}" height="{n(y1 - y0)}" {self._paint(fill, outline, width)}/>')

    def rounded_rectangle(self, xy, radius=0, fill=None, outline=None, width=1):
        x0, y0, x1, y1 = self._box(xy, width, outline)
        r = max(0.0, radius * self.k - (width * self.k / 2 if outline is not None and width else 0))
        self.items.append(
            f'<rect x="{n(x0)}" y="{n(y0)}" width="{n(x1 - x0)}" height="{n(y1 - y0)}" rx="{n(r)}" {self._paint(fill, outline, width)}/>'
        )

    def ellipse(self, xy, fill=None, outline=None, width=1):
        xy = [c for p in xy for c in (p if isinstance(p, tuple) else (p,))]
        x0, y0, x1, y1 = self._box(xy, width, outline)
        self.items.append(
            f'<ellipse cx="{n((x0 + x1) / 2)}" cy="{n((y0 + y1) / 2)}" rx="{n((x1 - x0) / 2)}" ry="{n((y1 - y0) / 2)}" {self._paint(fill, outline, width)}/>'
        )

    def line(self, xy, fill=None, width=1, joint=None):
        if xy and not isinstance(xy[0], tuple):
            xy = [(xy[i], xy[i + 1]) for i in range(0, len(xy), 2)]
        pts = " ".join(f"{n(x * self.k)},{n(y * self.k)}" for x, y in xy)
        c, o = colour(fill)
        join = ' stroke-linejoin="round"' if joint == "curve" else ""
        self.items.append(
            f'<polyline points="{pts}" fill="none" stroke="{c}" stroke-width="{n(width * self.k)}"{join}'
            + (f' stroke-opacity="{o}"' if o < 1 else "")
            + "/>"
        )

    def polygon(self, xy, fill=None, outline=None, width=1):
        if xy and not isinstance(xy[0], tuple):
            xy = [(xy[i], xy[i + 1]) for i in range(0, len(xy), 2)]
        pts = " ".join(f"{n(x * self.k)},{n(y * self.k)}" for x, y in xy)
        self.items.append(f'<polygon points="{pts}" {self._paint(fill, outline, width)}/>')

    def _arc_points(self, xy, start, end, width=0):
        x0, y0, x1, y1 = (v * self.k for v in xy)
        h = width * self.k / 2
        cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 - h, (y1 - y0) / 2 - h
        a0, a1 = math.radians(start), math.radians(end)
        p0 = (cx + rx * math.cos(a0), cy + ry * math.sin(a0))
        p1 = (cx + rx * math.cos(a1), cy + ry * math.sin(a1))
        large = 1 if (end - start) % 360 > 180 else 0
        return cx, cy, rx, ry, p0, p1, large

    def arc(self, xy, start, end, fill=None, width=1):
        cx, cy, rx, ry, p0, p1, large = self._arc_points(xy, start, end, width)
        c, _ = colour(fill)
        self.items.append(
            f'<path d="M{n(p0[0])},{n(p0[1])} A{n(rx)},{n(ry)} 0 {large} 1 {n(p1[0])},{n(p1[1])}" fill="none" stroke="{c}" stroke-width="{n(width * self.k)}"/>'
        )

    def chord(self, xy, start, end, fill=None, outline=None, width=1):
        cx, cy, rx, ry, p0, p1, large = self._arc_points(xy, start, end, width if outline is not None else 0)
        self.items.append(
            f'<path d="M{n(p0[0])},{n(p0[1])} A{n(rx)},{n(ry)} 0 {large} 1 {n(p1[0])},{n(p1[1])} Z" {self._paint(fill, outline, width)}/>'
        )

    def pieslice(self, xy, start, end, fill=None, outline=None, width=1):
        cx, cy, rx, ry, p0, p1, large = self._arc_points(xy, start, end, 0)
        self.items.append(
            f'<path d="M{n(cx)},{n(cy)} L{n(p0[0])},{n(p0[1])} A{n(rx)},{n(ry)} 0 {large} 1 {n(p1[0])},{n(p1[1])} Z" {self._paint(fill, outline, width)}/>'
        )

    def text(self, xy, text, font=None, fill=None, size=22):
        c, _ = colour(fill)
        x, y = xy
        self.items.append(
            f'<text x="{n(x * self.k)}" y="{n(y * self.k)}" font-family="DejaVu Sans, sans-serif" font-weight="bold" '
            f'font-size="{n(size * self.k)}" text-anchor="middle" dominant-baseline="central" fill="{c}">{text}</text>'
        )

    def body(self):
        return "\n".join(self.items)
