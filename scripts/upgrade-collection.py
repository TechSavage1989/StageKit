#!/usr/bin/env python3
"""Brings an OBS scene collection that already uses StageKit up to date, without rebuilding it.

It reads one collection and writes another; the one it reads is never changed. Everything in the
collection is kept as it is except for these three things:

1. Window captures get a Fit Item filter on their scene, the way a game capture has one. Only a window
   capture that is fitted inside a centred bounding box is taken, and only if its scene has no Fit Item
   for it yet. A box that covers the canvas becomes "the whole screen" with a margin; any other box
   becomes "a box I set" with the same place and size and no margin.
2. An Auto Frame set to "Just the picture" gets the canvas height, so the picture is not scaled up from
   a smaller size.
3. A Send To or Place Item filter that brings an item along has "Flip sides to stay on screen" switched on.

Usage:
    python3 -I scripts/upgrade-collection.py <collection.json> <upgraded.json> <report.txt> [new name]
"""
import json
import sys
import uuid

if len(sys.argv) < 4:
    sys.exit(__doc__)
SRC, OUT, REPORT = sys.argv[1], sys.argv[2], sys.argv[3]
NEW_NAME = sys.argv[4] if len(sys.argv) > 4 else None

# How far a picture that does not have the screen's shape keeps from the screen's edges, when the
# collection has no whole-screen Fit Item of its own to take the number from.
DEFAULT_MARGIN = 90.0
# OBS's numbers: bounds type "scale to inner bounds", and the alignment bits.
SCALE_INNER = 2
LEFT, RIGHT, TOP, BOTTOM = 1, 2, 4, 8

data = json.load(open(SRC, encoding="utf-8"))
canvas_w = float(data.get("resolution", {}).get("x", 1920))
canvas_h = float(data.get("resolution", {}).get("y", 1080))
everything = data.get("sources", []) + data.get("groups", [])
kind_by_uuid = {s.get("uuid"): s.get("id") for s in everything}
kind_by_name = {s.get("name"): s.get("id") for s in everything}
report = []


def make_filter(filter_id, name, settings, like):
    """A filter entry written the way OBS writes one."""
    return {
        "prev_ver": like.get("prev_ver", 0), "name": name, "uuid": str(uuid.uuid4()), "id": filter_id,
        "versioned_id": filter_id, "settings": settings, "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0,
        "balance": 0.5, "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
        "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {},
    }


def box_corner(pos, size, align, low, high):
    """Where a bounding box starts, from the item's position and what that position refers to."""
    if align & low:
        return pos
    if align & high:
        return pos - size
    return pos - size / 2.0


# The margin already chosen for a whole-screen fit, if there is one.
margin = DEFAULT_MARGIN
for source in everything:
    for f in source.get("filters", []):
        if f.get("id") == "stagekit_fit_item" and f.get("settings", {}).get("box", 0) == 0:
            margin = float(f["settings"].get("margin", DEFAULT_MARGIN))

# 1. Fit Item for window captures.
for scene in data.get("sources", []):
    if scene.get("id") != "scene":
        continue
    fitted = {f.get("settings", {}).get("item") for f in scene.get("filters", [])
              if f.get("id") == "stagekit_fit_item"}
    added = []
    for item in scene.get("settings", {}).get("items", []):
        # Items inside a group are listed a second time in the scene, as a backup for old OBS versions.
        if item.get("group_item_backup"):
            continue
        name = item.get("name")
        kind = kind_by_uuid.get(item.get("source_uuid")) or kind_by_name.get(name)
        bounds = item.get("bounds", {})
        bw, bh = float(bounds.get("x", 0)), float(bounds.get("y", 0))
        if (kind != "window_capture" or name in fitted or item.get("bounds_type") != SCALE_INNER
                or item.get("bounds_align", 0) != 0 or bw < 2 or bh < 2 or item.get("rot", 0)):
            continue
        align = item.get("align", LEFT | TOP)
        x = box_corner(float(item["pos"]["x"]), bw, align, LEFT, RIGHT)
        y = box_corner(float(item["pos"]["y"]), bh, align, TOP, BOTTOM)
        whole = x <= 1 and y <= 1 and x + bw >= canvas_w - 1 and y + bh >= canvas_h - 1
        settings = ({"item": name, "box": 0, "margin": margin} if whole else
                    {"item": name, "box": 1, "box_x": float(round(x)), "box_y": float(round(y)),
                     "box_width": float(round(bw)), "box_height": float(round(bh)), "margin": 0.0})
        # A scene with no filters has no list for them yet; one is only made when it is needed.
        scene.setdefault("filters", []).append(make_filter("stagekit_fit_item", f"Fit {name}", settings, scene))
        fitted.add(name)
        added.append(f"'{name}' in the whole screen, {margin:.0f} px margin" if whole else
                     f"'{name}' in the box {bw:.0f}x{bh:.0f} at {x:.0f}, {y:.0f}")
    if added:
        report.append(f"{scene['name']}: Fit Item added for " + "; ".join(added))

# 2 and 3. Settings of filters that are already there.
for source in everything:
    for f in source.get("filters", []):
        settings = f.setdefault("settings", {})
        if f.get("id") == "stagekit_auto_frame" and settings.get("frame_mode") == 1:
            if float(settings.get("frame_height", 0)) < canvas_h:
                was = settings.get("frame_height")
                settings["frame_width"], settings["frame_height"] = int(canvas_w), int(canvas_h)
                report.append(f"{source['name']}: Auto Frame '{f['name']}' picture height {was} -> {int(canvas_h)}")
        if f.get("id") in ("stagekit_send_to", "stagekit_place_item"):
            if settings.get("linked") and settings.get("flip_linked") is False:
                settings["flip_linked"] = True
                report.append(f"{source['name']}: '{f['name']}' now flips '{settings['linked']}' to stay on screen")

if NEW_NAME:
    report.insert(0, f"Collection name: '{data.get('name')}' -> '{NEW_NAME}'")
    data["name"] = NEW_NAME
if not report:
    report.append("Nothing needed changing.")

json.dump(data, open(OUT, "w", encoding="utf-8"), indent=1)
open(REPORT, "w", encoding="utf-8").write("\n".join(report) + "\n")
print("\n".join(report))
