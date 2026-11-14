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

from PIL import Image, ImageEnhance

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
