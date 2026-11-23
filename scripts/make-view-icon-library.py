#!/usr/bin/env python3
"""Share existing Stream Deck artwork with the native Views icon picker.

Run from any directory, or use --check to verify the committed catalog, Qt
resource list and generated locale block. Only Python's standard library is
needed. The resource list references the original PNGs; it never copies or
redraws them. Qt embeds these files in StageKit, so using the native library
does not require the Stream Deck companion to be installed.
"""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
from xml.sax.saxutils import escape, quoteattr


ROOT = Path(__file__).resolve().parents[1]
DECK = ROOT / "streamdeck"
PACK = DECK / "com.techsavage1989.stagekit.sdPlugin/imgs/pack"
OUT = ROOT / "src/controller/view-icons"
LOCALE = ROOT / "data/locale/en-US.ini"
PREFIX = "/stagekit/view-icons"
BEGIN = "; ==== generated view icon library ===="
END = "; ==== end generated view icon library ===="

# These are general panel functions, not the full collection of filter looks,
# animations, key states or the owner's named scenes. Source and scene types
# below come directly from the companion's canonical generic type lists.
CONTROLS = [
    ("scene", "Scene", "options/scene"),
    ("mode", "Scene mode", "options/mode"),
    ("view", "Combined view", "options/view"),
    ("link", "Link Visibility", "options/link"),
    ("show-hide", "Show / hide", "options/show-hide"),
    ("show-in-scenes", "Show in Scenes", "filters/show-in-scenes"),
    ("layout", "Apply Layout", "options/layout"),
    ("fit", "Fit to Space", "options/fit"),
    ("scene-fit", "Scene Fit", "options/scene-fit"),
    ("source-switcher", "Automatic Source Switcher", "options/source-switcher"),
    ("window-switcher", "Automatic Window Switcher", "options/window-switcher"),
    ("standby", "Standby Image", "filters/standby-picture"),
    ("transitions", "Scene Transitions", "options/scene-transitions"),
    ("hide", "Hide", "options/hide"),
    ("clear", "Clear", "options/clear"),
]


def png_size(path):
    with path.open("rb") as image:
        header = image.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Not a PNG: {path.relative_to(ROOT)}")
    width, height = struct.unpack(">II", header[16:24])
    if not (0 < width <= 144 and 0 < height <= 144):
        raise ValueError(f"Unexpected key image size {width}x{height}: {path.relative_to(ROOT)}")


def generate():
    types = json.loads((DECK / "src/key-types.json").read_text(encoding="utf-8"))
    pack = json.loads((DECK / "src/pack.json").read_text(encoding="utf-8"))
    pictures = {entry["id"]: entry for entry in pack["pictures"]}
    if len(pictures) != len(pack["pictures"]):
        raise ValueError("The Stream Deck picture catalog has duplicate IDs")
    locales = {}

    def label(key, value):
        if key in locales and locales[key] != value:
            raise ValueError(f"Conflicting locale label: {key}")
        locales[key] = value
        return key

    palettes = []
    for palette in pack["colours"]:
        if not re.fullmatch(r"[a-z][a-z0-9-]*", palette):
            raise ValueError(f"Unexpected palette ID: {palette}")
        palettes.append({"id": palette, "labelKey": label(f"ViewIcons.Palette.{palette}", palette.title())})
    if not palettes or palettes[0]["id"] != "mono":
        raise ValueError("The icon library must retain Mono as its default palette")
    groups = [
        {"id": group, "labelKey": label(f"ViewIcons.Group.{group}", title)}
        for group, title in (("scenes", "Scenes"), ("sources", "Sources"), ("controls", "Controls"))
    ]
    icons = []
    resources = {}
    ids = set()

    def add(kind, entry_id, title, group, picture_id, keywords):
        icon_id = f"{kind}:{entry_id}"
        if icon_id in ids:
            raise ValueError(f"Duplicate icon ID: {icon_id}")
        ids.add(icon_id)
        if picture_id not in pictures:
            raise ValueError(f"Missing picture {picture_id} for {icon_id}")
        # The normal bright key image has no on/off badge. Marked variants live
        # under status/ and are deliberately excluded: OBS's active highlight
        # remains the panel's state indicator.
        picture = PurePosixPath(pictures[picture_id]["on"])
        if picture.is_absolute() or ".." in picture.parts or "status" in picture.parts:
            raise ValueError(f"Unexpected picture path: {picture}")
        paths = {}
        for palette in pack["colours"]:
            relative = PurePosixPath(palette) / (str(picture) + ".png")
            source = PACK / relative
            png_size(source)
            resource = f":{PREFIX}/{relative}"
            paths[palette] = resource
            resources[str(relative)] = source
        icons.append({
            "id": icon_id,
            "labelKey": label(f"ViewIcons.{kind.title()}.{entry_id}", title),
            "group": group,
            "paths": paths,
            "keywords": sorted({str(word).strip().casefold() for word in keywords if str(word).strip()}),
        })

    for entry in types["sceneTypes"]:
        add("scene", entry["id"], entry["label"], "scenes", entry["picture"],
            [entry["id"].replace("_", " "), entry["group"], entry["label"]])
    for entry in types["sourceTypes"]:
        words = [word for phrase in entry.get("words", []) for word in phrase]
        add("source", entry["id"], entry["label"], "sources", entry["base"],
            [entry["id"].replace("_", " "), entry["group"], entry["label"], *words])
    for entry_id, title, picture in CONTROLS:
        add("control", entry_id, title, "controls", picture, [entry_id.replace("-", " "), title])

    catalog = {
        "_": "Generated by scripts/make-view-icon-library.py from the Stream Deck type lists and original unmarked key artwork.",
        "palettes": palettes,
        "groups": groups,
        "icons": icons,
    }
    qrc = [
        '<!DOCTYPE RCC><RCC version="1.0">',
        "  <!-- Generated by scripts/make-view-icon-library.py; references the original artwork. -->",
        f"  <qresource prefix={quoteattr(PREFIX)}>",
        '    <file alias="catalog.json">catalog.json</file>',
    ]
    for alias, path in sorted(resources.items()):
        relative = Path(os.path.relpath(path, OUT)).as_posix()
        qrc.append(f"    <file alias={quoteattr(alias)}>{escape(relative)}</file>")
    qrc.extend(["  </qresource>", "</RCC>", ""])
    locale_lines = [BEGIN, "; Written by scripts/make-view-icon-library.py. Edit the source catalogs instead."]
    for key, value in locales.items():
        locale_lines.append(f"{key}={json.dumps(value, ensure_ascii=False)}")
    locale_lines.extend([END, ""])
    locale_block = "\n".join(locale_lines)
    locale = LOCALE.read_text(encoding="utf-8")
    if BEGIN in locale or END in locale:
        if locale.count(BEGIN) != 1 or locale.count(END) != 1 or locale.index(BEGIN) > locale.index(END):
            raise ValueError("Malformed generated icon locale block")
        before, rest = locale.split(BEGIN, 1)
        _, after = rest.split(END, 1)
        locale = before + locale_block.rstrip("\n") + after
    else:
        locale = locale.rstrip("\n") + "\n\n" + locale_block
    outputs = {
        OUT / "catalog.json": json.dumps(catalog, ensure_ascii=False, indent=2) + "\n",
        OUT / "view-icons.qrc": "\n".join(qrc),
        LOCALE: locale,
    }
    return outputs, len(icons), len(resources), sum(path.stat().st_size for path in resources.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail if generated output is missing or stale")
    args = parser.parse_args()
    try:
        outputs, icons, images, byte_count = generate()
        stale = []
        for path, content in outputs.items():
            if args.check:
                if not path.exists() or path.read_bytes() != content.encode("utf-8"):
                    stale.append(str(path.relative_to(ROOT)))
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8", newline="\n")
        if stale:
            print("Stale icon library files: " + ", ".join(stale), file=sys.stderr)
            print("Run python3 scripts/make-view-icon-library.py", file=sys.stderr)
            return 1
        print(f"{'Checked' if args.check else 'Generated'} {icons} icon choices, {images} shared PNGs, {byte_count} image bytes")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"View icon library: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
