#!/usr/bin/env python3
"""Brings the StageKit icon pack into the plugin (needs Pillow).

Usage, from the streamdeck folder:
    python3 -I tools/import-icons.py <pack folder>

<pack folder> holds one folder per colour (ember, forest, mono, neon, ocean,
violet), each with png/<group>/<name>.png at 288 px. Every picture is written
at key size (144 px, 255 colours) to
    com.techsavage1989.stagekit.sdPlugin/imgs/pack/<colour>/<group>/<name>.png
and src/pack.json lists them once, as pictures with an "on" and an "off" look.
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "com.techsavage1989.stagekit.sdPlugin", "imgs", "pack")
INDEX = os.path.join(HERE, "..", "src", "pack.json")
COLOURS = ["mono", "violet", "ocean", "forest", "ember", "neon"]
SIZE = 144


def words(name):
    return name.replace("-", " ").strip()


def build_index(groups):
    """groups: {group: [names without .png]} -> list of pictures."""
    pictures = []
    blur_off = set(groups.get("blur-off", []))
    for group in sorted(groups):
        if group == "blur-off":
            continue  # the "off" halves of the blur-<type> groups
        names = set(groups[group])
        done = set()
        for name in sorted(names):
            if name in done:
                continue
            on = off = f"{group}/{name}"
            label = words(name)
            for on_end, off_end in (("-on", "-off"), ("-shown", "-hidden")):
                if name.endswith(on_end) or name.endswith(off_end):
                    stem = name[: -len(on_end)] if name.endswith(on_end) else name[: -len(off_end)]
                    label = words(stem)
                    if stem + on_end in names:
                        on = f"{group}/{stem}{on_end}"
                        done.add(stem + on_end)
                    if stem + off_end in names:
                        off = f"{group}/{stem}{off_end}"
                        done.add(stem + off_end)
                    elif group.startswith("blur-") and stem + "-off" in blur_off:
                        off = f"blur-off/{stem}-off"
                    else:
                        off = on
                    if stem + on_end not in names:
                        on = off
                    break
            done.add(name)
            pictures.append({"id": f"{group}/{label.replace(' ', '-')}", "group": words(group), "label": label, "on": on, "off": off})
    return pictures


def accent_of(folder):
    """The palette's own bright colour, taken from one of its pictures."""
    image = Image.open(os.path.join(folder, "filters", "show-in-scenes-on.png")).convert("RGB").resize((64, 64))
    best, most = (255, 255, 255), -1.0
    for r, g, b in image.getdata():
        high, low = max(r, g, b), min(r, g, b)
        strength = (high - low) * high  # saturated and bright
        if strength > most:
            best, most = (r, g, b), strength
    return best if most > 2000 else (255, 255, 255)


def plain(kind, on, accent):
    """A plain key in the palette's colour, for what the pack has no picture of."""
    s = SIZE * 4
    ink = (255, 255, 255, 255) if on else (120, 126, 140, 255)
    lit = accent + (255,) if on else (86, 90, 104, 255)
    image = Image.new("RGBA", (s, s), (24, 26, 34, 255))
    draw = ImageDraw.Draw(image)
    w = s // 22
    frame = (s * 0.14, s * 0.22, s * 0.86, s * 0.70)
    draw.rounded_rectangle(frame, radius=s * 0.06, outline=ink, width=w)
    if kind == "switch":
        draw.rounded_rectangle((s * 0.52, s * 0.42, s * 0.78, s * 0.62), radius=s * 0.035, fill=lit)
        y0, y1 = s * 0.78, s * 0.90
        draw.rounded_rectangle((s * 0.36, y0, s * 0.64, y1), radius=(y1 - y0) / 2, fill=lit)
        knob, r = (s * 0.57 if on else s * 0.43), (y1 - y0) * 0.36
        draw.ellipse((knob - r, (y0 + y1) / 2 - r, knob + r, (y0 + y1) / 2 + r), fill=(255, 255, 255, 255))
    elif kind == "move":
        draw.rounded_rectangle((s * 0.22, s * 0.46, s * 0.42, s * 0.62), radius=s * 0.03, outline=(120, 126, 140, 255), width=w // 2)
        draw.rounded_rectangle((s * 0.58, s * 0.30, s * 0.78, s * 0.46), radius=s * 0.03, fill=lit)
        draw.line((s * 0.40, s * 0.48, s * 0.55, s * 0.41), fill=ink, width=w)
    else:  # tool: three items lined up on one edge
        draw.line((s * 0.26, s * 0.30, s * 0.26, s * 0.62), fill=lit, width=w)
        for i, length in enumerate((0.40, 0.26, 0.34)):
            y = s * (0.32 + i * 0.105)
            draw.rounded_rectangle((s * 0.30, y, s * (0.30 + length), y + s * 0.07), radius=s * 0.02, fill=ink)
    return image.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    pack = sys.argv[1]
    groups = None
    count = 0
    for colour in COLOURS:
        source = os.path.join(pack, colour, "png")
        if not os.path.isdir(source):
            print(f"missing: {source}")
            return 1
        found = {}
        for group in sorted(os.listdir(source)):
            folder = os.path.join(source, group)
            if not os.path.isdir(folder):
                continue
            for file in sorted(os.listdir(folder)):
                if not file.endswith(".png"):
                    continue
                image = Image.open(os.path.join(folder, file)).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
                target = os.path.join(OUT, colour, group)
                os.makedirs(target, exist_ok=True)
                image.quantize(255, method=Image.FASTOCTREE).save(os.path.join(target, file), "PNG", optimize=True)
                found.setdefault(group, []).append(file[:-4])
                count += 1
                if group == "blur-types":
                    # One picture per blur type, for any source: its "off"
                    # look is the same picture, greyed and darkened.
                    grey = ImageEnhance.Brightness(ImageEnhance.Color(image).enhance(0.0)).enhance(0.45)
                    grey.quantize(255, method=Image.FASTOCTREE).save(
                        os.path.join(target, file[:-4] + "-off.png"), "PNG", optimize=True
                    )
                    os.replace(os.path.join(target, file), os.path.join(target, file[:-4] + "-on.png"))
                    found[group][-1] = file[:-4] + "-on"
                    found[group].append(file[:-4] + "-off")
                    count += 1
        accent = accent_of(source)
        target = os.path.join(OUT, colour, "plain")
        os.makedirs(target, exist_ok=True)
        for name, kind, on in (
            ("switch-on", "switch", True),
            ("switch-off", "switch", False),
            ("move", "move", True),
            ("tool", "tool", True),
        ):
            plain(kind, on, accent).quantize(255, method=Image.FASTOCTREE).save(
                os.path.join(target, name + ".png"), "PNG", optimize=True
            )
            found.setdefault("plain", []).append(name)
            count += 1
        if groups is None:
            groups = found
        elif found != groups:
            print(f"{colour} does not hold the same pictures as {COLOURS[0]}")
            return 1
    pictures = build_index(groups)
    with open(INDEX, "w", encoding="utf-8") as out:
        json.dump({"colours": COLOURS, "pictures": pictures}, out, indent="\t")
        out.write("\n")
    print(f"{count} files, {len(pictures)} pictures in {len(COLOURS)} colours")
    return 0


if __name__ == "__main__":
    sys.exit(main())
