"""Rebuilds the owner's OBS scene collection on StageKit tools.

Reads the original collection (never modified) and writes a new one. Each
production scene is flattened: the nested "Overlay"/"Parts" scenes, source
clones and Move-filter chains are replaced by the real sources placed directly
in the scene with StageKit filters on them.

Usage: python3 -I rebuild.py <original.json> <output.json> <report.txt>
"""
import copy
import json
import os
import sys
import uuid

SRC, OUT, REPORT = sys.argv[1], sys.argv[2], sys.argv[3]
old = json.load(open(SRC, encoding="utf-8"))
by = {s["name"]: s for s in old["sources"]}
for g in old.get("groups", []):
    by[g["name"]] = g

CANVAS_W, CANVAS_H = old["resolution"]["x"], old["resolution"]["y"]

PRODUCTION = [
    "Stream Start", "Game-Capture-Scene", "Game-Component-Capture-Scene", "Console Scene",
    "Game-Capture-HD60S-Scene", "Game-Component-HD60S-Capture-Scene", "Window-Capture-Scene",
    "Window-Component-Capture-Scene", "Main Display Scene", "Tablet Scene", "Dolphin-GBA-Scene",
    "Camera Only", "Intermission", "Be Right Back", "Stream Ending",
]

# Filters the old setup needed and StageKit replaces.
OLD_FILTERS = {
    "move_action_filter", "move_value_filter", "move_source_filter", "move_transition_override_filter",
    "obs_composite_blur", "filter-blur", "shader_filter", "streamfx-filter-sdf-effects", "filter-dropshadow",
    "scroll_filter",
}
GAME_CAPTURES = {"Game Capture", "Windowed Game Capture"}
# The one game capture in the rebuilt collection, and how far an emulator's squarer picture keeps from
# the edges of a full-screen scene (the owner placed one by hand at about this).
GAME = "Game Capture Framed"
FULL_SCREEN_MARGIN = 90.0
# Everything under these was the hand-built blurred background.
BACKGROUND_SCENES = ("Live Background", "Start Background Scene", "Background Scene", "Tablet Background Scene")
# Show/hide fades are left off: a collection generated with them crashed the owner's OBS on import and the
# same collection without them loaded. Why is not known yet. Set STAGEKIT_REBUILD_FADES=1 to put them back.
KEEP_FADES = os.environ.get("STAGEKIT_REBUILD_FADES", "0") == "1"
SKIP_LEAVES = {"Purple"}
# The owner's choice: where the cropped "Amuse Main" widget sat (start, intermission, break and ending
# scenes), show the "Amuse Component" widget instead, whole and uncropped, in the same place.
SWAP_WIDGET = {"Amuse Main": "Amuse Component"}

report = []
new_sources = {}  # name -> source json (leaf sources, wrappers, groups, scenes)
new_groups = {}
shadowed = set()  # names of sources that get a Shadow & Backdrop filter
blurred = set()   # names of sources that get a Ramp Blur filter


def uid():
    return str(uuid.uuid4())


def make_filter(filter_id, name, settings, enabled=True):
    return {
        "prev_ver": old["sources"][0].get("prev_ver", 0), "name": name, "uuid": uid(), "id": filter_id,
        "versioned_id": filter_id, "settings": settings, "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0,
        "balance": 0.5, "enabled": enabled, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
        "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {},
    }


def has_shadow(source):
    for f in source.get("filters", []):
        if f["id"] == "shader_filter" and f.get("enabled", True) and \
                "drop_shadow" in f.get("settings", {}).get("shader_file_name", ""):
            return f["settings"]
    return None


def had_blur_chain(source):
    ids = [f["id"] for f in source.get("filters", [])]
    return "obs_composite_blur" in ids and "move_value_filter" in ids


def carry(name):
    """A leaf source carried over, with the old helper filters taken off."""
    if name in new_sources:
        return new_sources[name]
    s = copy.deepcopy(by[name])
    if had_blur_chain(s):
        blurred.add(name)
    removed = [f["name"] for f in s.get("filters", []) if f["id"] in OLD_FILTERS]
    s["filters"] = [f for f in s.get("filters", []) if f["id"] not in OLD_FILTERS]
    if removed:
        report.append(f"  {name}: removed {len(removed)} old filter(s)")
    new_sources[name] = s
    return s


def item_json(name, pos, scale=(1.0, 1.0), bounds_type=0, bounds=(0.0, 0.0), bounds_align=0, crop=(0, 0, 0, 0),
              visible=True, locked=False, item_id=1, show=None, hide=None, align=5):
    return {
        "name": name, "source_uuid": new_sources[name]["uuid"], "visible": visible, "locked": locked, "rot": 0.0,
        "pos": {"x": float(pos[0]), "y": float(pos[1])}, "scale": {"x": float(scale[0]), "y": float(scale[1])},
        "align": align, "bounds_type": bounds_type, "bounds_align": bounds_align, "bounds_crop": False,
        "bounds": {"x": float(bounds[0]), "y": float(bounds[1])}, "crop_left": crop[0], "crop_top": crop[2],
        "crop_right": crop[1], "crop_bottom": crop[3], "id": item_id, "group_item_backup": False,
        "scale_filter": "disable", "blend_method": "default", "blend_type": "normal",
        # The fade an item had when shown or hidden is kept.
        "show_transition": copy.deepcopy(show) if show and KEEP_FADES else {"duration": 0},
        "hide_transition": copy.deepcopy(hide) if hide and KEEP_FADES else {"duration": 0}, "private_settings": {},
    }


def scene_json(name, items, custom_size=None, filters=None):
    template = copy.deepcopy(by["Camera Only"])
    for i, it in enumerate(items):
        it["id"] = i + 1
    template.update({"name": name, "uuid": uid(), "filters": filters or [],
                     "hotkeys": {"OBSBasic.SelectScene": []}})
    template["settings"] = {"custom_size": bool(custom_size), "id_counter": len(items) + 1, "items": items}
    if custom_size:
        template["settings"]["cx"], template["settings"]["cy"] = custom_size
    return template


def source_size(name):
    """Best guess at a source's pixel size, for composing bounds."""
    s = by.get(name, {})
    st = s.get("settings", {})
    if s.get("id") in ("scene",):
        return (st.get("cx", CANVAS_W), st.get("cy", CANVAS_H)) if st.get("custom_size") else (CANVAS_W, CANVAS_H)
    if s.get("id") == "group":
        return st.get("cx", CANVAS_W), st.get("cy", CANVAS_H)
    return None


def leaf_size(name):
    """The pixel size of a plain source, from its settings."""
    src = by[name]
    st = src.get("settings", {})
    if src["id"] == "browser_source":
        return int(st.get("width", 800)), int(st.get("height", 600))
    if src["id"] == "color_source":
        return int(st.get("width", 1920)), int(st.get("height", 1080))
    res = st.get("resolution") or st.get("last_resolution")
    if res:
        w, h = (int(v) for v in res.split("x"))
        return w, h
    report.append(f"  !! size of '{name}' ({src['id']}) is a guess (1920x1080): check its crop wrapper")
    return 1920, 1080


class T:
    """Where a nested scene's own coordinates land in the scene above it."""

    def __init__(self, ox=0.0, oy=0.0, sx=1.0, sy=1.0):
        self.ox, self.oy, self.sx, self.sy = ox, oy, sx, sy

    def child(self, it, size):
        sx, sy = it["scale"]["x"], it["scale"]["y"]
        ox, oy = it["pos"]["x"], it["pos"]["y"]
        if it.get("bounds_type", 0) and size:
            bw, bh = it["bounds"]["x"], it["bounds"]["y"]
            rx, ry = bw / size[0], bh / size[1]
            bt = it["bounds_type"]
            if bt == 1:
                sx, sy = rx, ry
            elif bt == 3:
                sx = sy = max(rx, ry)
            else:
                sx = sy = min(rx, ry)
            al = it.get("bounds_align", 0)
            fx = 0.0 if al & 1 else 1.0 if al & 2 else 0.5
            fy = 0.0 if al & 4 else 1.0 if al & 8 else 0.5
            ox += (bw - size[0] * sx) * fx
            oy += (bh - size[1] * sy) * fy
        return T(self.ox + ox * self.sx, self.oy + oy * self.sy, self.sx * sx, self.sy * sy)


def make_view(view_name, source_name, settings):
    """A Cropped View (StageKit): part of a source as a plain source, so no wrapper scene is needed."""
    carry(source_name)
    v = copy.deepcopy(by["Start Image"])
    v.update({"name": view_name, "uuid": uid(), "id": "stagekit_cropped_view",
              "versioned_id": "stagekit_cropped_view", "filters": [],
              "settings": dict(settings, source=source_name)})
    new_sources[view_name] = v


def view_for_crop(name, crop):
    """A source cropped by its scene item and shadowed: the crop moves into a Cropped View."""
    left, right, top, bottom = crop
    w, h = leaf_size(name)
    vname = f"{name} Crop {w - left - right}x{h - top - bottom}"
    if vname not in new_sources:
        make_view(vname, name, {"crop_to": 0, "left": left, "right": right, "top": top, "bottom": bottom})
        report.append(f"  '{vname}': a Cropped View of {name} (no wrapper scene)")
    return vname


def fitted_view(name):
    """A Cropped View that trims to whatever the source draws."""
    vname = f"{name} Fitted"
    if vname not in new_sources:
        make_view(vname, name, {"crop_to": 6})
        report.append(f"  '{vname}': a Cropped View of {name} that trims to whatever it draws, "
                      "pinned by its bottom-left corner")
    return vname


def view_for_crop_scene(scene_name):
    """An old one-item scene with a Crop filter (Square Camera) becomes a Cropped View.

    Returns how much bigger the old scene drew the source, so the item keeps its size on screen."""
    scene = by[scene_name]
    inner = [it for it in scene["settings"]["items"]
             if it["name"] not in SKIP_LEAVES and it.get("visible", True) and not it.get("group_item_backup")]
    crop = next(f["settings"] for f in scene["filters"] if f["id"] == "crop_filter" and f.get("enabled", True))
    if len(inner) != 1 or not crop.get("relative", True) or inner[0]["pos"]["x"] or inner[0]["pos"]["y"]:
        return None
    it = inner[0]
    sx, sy = it["scale"]["x"], it["scale"]["y"]
    if scene_name not in new_sources:
        make_view(scene_name, it["name"], {
            "crop_to": 0, "left": round(crop.get("left", 0) / sx), "right": round(crop.get("right", 0) / sx),
            "top": round(crop.get("top", 0) / sy), "bottom": round(crop.get("bottom", 0) / sy)})
        report.append(f"  '{scene_name}': was a scene with a Crop filter, now a Cropped View of {it['name']}")
    return sx, sy


def flatten(scene_name, t, shadow, out, depth=0):
    """Appends (bottom first) the leaves of an old scene to `out`."""
    scene = by[scene_name]
    for it in scene["settings"]["items"]:
        name = it["name"]
        if it.get("group_item_backup") or name in SKIP_LEAVES:
            continue
        child = by.get(name)
        if not child:
            continue
        kind = child["id"]
        if kind in ("scene", "group") and name.startswith(BACKGROUND_SCENES):
            out.append({"ambient": True})
            continue
        if not it.get("visible", True) and kind in ("scene", "group"):
            # A hidden variant (another picture shape, another capture type).
            report.append(f"  dropped hidden variant '{name}'")
            continue
        own_shadow = shadow or bool(has_shadow(child))
        crop_filters = [f for f in child.get("filters", []) if f["id"] == "crop_filter" and f.get("enabled", True)]
        is_leaf_group = kind == "group" and all(
            by.get(c["name"], {}).get("id") not in ("scene", "group") for c in child["settings"]["items"])
        if kind in ("scene", "group") and not crop_filters and not is_leaf_group and depth < 10:
            flatten(name, t.child(it, source_size(name)), own_shadow, out, depth + 1)
            continue

        crop = (it.get("crop_left", 0), it.get("crop_right", 0), it.get("crop_top", 0), it.get("crop_bottom", 0))
        entry = {"name": name, "visible": it.get("visible", True), "locked": it.get("locked", False),
                 "pos": (t.ox + it["pos"]["x"] * t.sx, t.oy + it["pos"]["y"] * t.sy),
                 "scale": (it["scale"]["x"] * t.sx, it["scale"]["y"] * t.sy),
                 "bounds_type": it.get("bounds_type", 0), "bounds_align": it.get("bounds_align", 0),
                 "bounds": (it["bounds"]["x"] * t.sx, it["bounds"]["y"] * t.sy), "crop": crop,
                 "shadow": own_shadow, "show": it.get("show_transition"), "hide": it.get("hide_transition")}
        grew = view_for_crop_scene(name) if kind == "scene" and crop_filters else None
        if grew:
            entry["scale"] = (entry["scale"][0] * grew[0], entry["scale"][1] * grew[1])
        elif kind in ("scene", "group"):
            # Kept as it is: a group of plain sources.
            keep_container(name)
        elif name in GAME_CAPTURES:
            entry["game"] = True
        elif name in SWAP_WIDGET and kind == "browser_source":
            # Both widgets draw their card in the middle of their page, so the
            # new page is centred on the middle of the strip the old crop kept.
            new_name = SWAP_WIDGET[name]
            carry(new_name)
            ow, oh = leaf_size(name)
            nw, nh = leaf_size(new_name)
            sx, sy = entry["scale"]
            centre_x = entry["pos"][0] + (ow - crop[0] - crop[1]) * sx / 2.0
            centre_y = entry["pos"][1] + (oh - crop[2] - crop[3]) * sy / 2.0
            entry["name"] = new_name
            entry["pos"] = (centre_x - nw * sx / 2.0, centre_y - nh * sy / 2.0)
            entry["crop"] = (0, 0, 0, 0)
            report.append(f"  '{new_name}' shown whole where the cropped '{name}' was")
        elif own_shadow and any(crop) and kind == "browser_source":
            # A web widget cropped by hand: the crop numbers go stale when the
            # widget's layout changes (the owner found the album art cut off).
            # The view trims to whatever the widget draws instead, and the item
            # is pinned by its bottom-left corner so it grows up and to the right.
            entry["name"] = fitted_view(name)
            w, h = leaf_size(name)
            entry["pos"] = (entry["pos"][0], entry["pos"][1] + (h - crop[2] - crop[3]) * entry["scale"][1])
            entry["align"] = 9  # left + bottom
            entry["crop"] = (0, 0, 0, 0)
        elif own_shadow and any(crop):
            entry["name"] = view_for_crop(name, crop)
            entry["crop"] = (0, 0, 0, 0)
        else:
            carry(name)
        out.append(entry)


def keep_container(name):
    if name in new_sources:
        return
    c = copy.deepcopy(by[name])
    kept = []
    for it in c["settings"]["items"]:
        if it["name"] in SKIP_LEAVES or it.get("group_item_backup"):
            continue
        inner = by.get(it["name"], {})
        if inner.get("id") in ("scene", "group"):
            keep_container(it["name"])
        else:
            carry(it["name"])
        it["source_uuid"] = new_sources[it["name"]]["uuid"]
        kept.append(it)
    c["settings"]["items"] = kept
    c["settings"].pop("canvas", None)
    c["filters"] = [f for f in c.get("filters", []) if f["id"] not in OLD_FILTERS]
    c["hotkeys"] = {"OBSBasic.SelectScene": []} if c["id"] == "scene" else {}
    new_sources[name] = c
    if c["id"] == "group":
        new_groups[name] = c


# ---------------------------------------------------------------- new sources

def stagekit_sources():
    start = by["Start Image"]["settings"].get("file", "")
    # The start image: one setting for the whole collection.
    s = copy.deepcopy(by["Start Image"])
    s.update({"id": "stagekit_start_image", "versioned_id": "stagekit_start_image", "filters": [],
              "settings": {"file": start}})
    new_sources["Start Image"] = s
    old["modules"]["stagekit.start_image"] = start

    amb = copy.deepcopy(by["Start Image"])
    amb.update({"name": "Ambient Background", "uuid": uid(), "id": "stagekit_ambient_background",
                "versioned_id": "stagekit_ambient_background", "filters": [],
                "settings": {"base_type": 2, "live_mode": 1, "live_opacity": 70.0, "live_fit": 0,
                             "speed_x": 30.0, "speed_y": 15.0, "mirror": True}})
    new_sources["Ambient Background"] = amb

    # One game capture for every game scene. Auto Frame makes it just the trimmed picture; a Fit Item
    # filter on each scene then fills that scene's game area or floats the picture centred in it.
    g = copy.deepcopy(by["Game Capture"])
    g["name"] = GAME
    g["settings"]["capture_mode"] = "any_fullscreen"
    g["filters"] = [
        make_filter("stagekit_auto_capture", "Auto Capture", {"mode": 0, "fallback_fullscreen": True}),
        make_filter("stagekit_auto_frame", "Auto Frame",
                    {"frame_mode": 1, "frame_width": 1754, "frame_height": 1268, "fit": 0, "trim": 1,
                     "trim_wait": 3.0, "known_programs": True, "glide_ms": 350}),
    ]
    new_sources[GAME] = g
    blurred.add(GAME)
    shadowed.add(GAME)


def finish_filters():
    for name in sorted(shadowed):
        s = new_sources[name]
        if any(f["id"] == "stagekit_shadow_backdrop" for f in s.setdefault("filters", [])):
            continue
        s["filters"].append(make_filter("stagekit_shadow_backdrop", "Shadow, Glow & Outline", {
            "shadow": True, "shadow_color": 0xFF000000, "shadow_opacity": 100.0, "offset_x": 20.0, "offset_y": 20.0,
            "softness": 0.0, "spread": 0.0, "backdrop": False, "radius": 0.0, "keep_place": True,
            # Nothing has been adjusted for the shadow's room yet: the filter does it on first load.
            "adjusted_left": 0, "adjusted_top": 0, "adjusted_right": 0, "adjusted_bottom": 0}))
    for name in sorted(blurred):
        s = new_sources.get(name)
        if not s:
            continue
        # The old chain was a pixelate blur ramped from 1 to 5; off until its key is pressed.
        s.setdefault("filters", []).append(make_filter(
            "stagekit_ramp_blur", "Ramp Blur", {"blur_type": 3, "include_in_all": True}, enabled=False))


def camera_spots():
    """The seven Move filters that placed the square camera become Send To filters on it."""
    cam = new_sources.get("Square Camera")
    if not cam:
        return
    spots = {}
    for f in by["Square Camera Parts"].get("filters", []):
        st = f.get("settings", {})
        if f["id"] == "move_source_filter" and st.get("source") == "Square Camera" and "pos" in st:
            spots[f["name"]] = (st["pos"]["x"], st["pos"]["y"])
    for name, (x, y) in spots.items():
        cam["filters"].append(make_filter("stagekit_send_to", f"Camera {name}", {
            "destination": 9, "saved_x": x, "saved_y": y, "restore_size": False, "where": 1,
            "linked": "Amuse Square Camera", "flip_linked": True, "duration_ms": 500}, enabled=False))
    report.append(f"  Square Camera: {len(spots)} Send To filters (one per old camera position), "
                  "bringing 'Amuse Square Camera' along")


# --------------------------------------------------------------------- scenes

def build_scene(name):
    report.append(f"\n{name}")
    flat = []
    flatten(name, T(), False, flat)
    items, seen, ambient_done, game_done, fit = [], set(), False, False, None
    for e in flat:
        if e.get("ambient"):
            if not ambient_done:
                items.insert(0, item_json("Ambient Background", (0, 0)))
                ambient_done = True
            continue
        if e.get("game"):
            if game_done:
                continue
            game_done = True
            framed = e["bounds_type"] and e["bounds"][0] < CANVAS_W * 0.9
            items.append(item_json(GAME, e["pos"] if framed else (0, 0), visible=e["visible"]))
            if framed:
                fit = {"item": GAME, "box": 1, "box_x": float(round(e["pos"][0])), "box_y": float(round(e["pos"][1])),
                       "box_width": float(int(e["bounds"][0])), "box_height": float(int(e["bounds"][1])),
                       "margin": 0.0}
                report.append(f"  one game capture ('{GAME}'), fitted by Fit Item to the game box "
                              f"{fit['box_width']:.0f}x{fit['box_height']:.0f} at {fit['box_x']:.0f}, {fit['box_y']:.0f}")
            else:
                fit = {"item": GAME, "box": 0, "margin": FULL_SCREEN_MARGIN}
                report.append(f"  one game capture ('{GAME}'), fitted by Fit Item to the whole screen: a widescreen "
                              f"game fills it, other shapes float centred {FULL_SCREEN_MARGIN:.0f} px in")
            continue
        if e["name"] in seen:
            continue
        seen.add(e["name"])
        if e["shadow"]:
            shadowed.add(e["name"])
        items.append(item_json(e["name"], e["pos"], e["scale"], e["bounds_type"], e["bounds"], e["bounds_align"],
                               e["crop"], e["visible"], e["locked"], show=e.get("show"), hide=e.get("hide"),
                               align=e.get("align", 5)))
    filters = scene_filters(name, [i["name"] for i in items])
    if fit:
        filters.append(make_filter("stagekit_fit_item", "Fit the game", fit))
    new_sources[name] = scene_json(name, items, filters=filters)
    report.append(f"  {len(items)} items placed directly in the scene")


def scene_filters(name, item_names):
    """Visibility Link and Program Triggers for the window-capture scenes."""
    filters = []
    captures = [n for n in ("Window Capture", "Blender", "Zbrush", "Fusion 360") if n in item_names]
    if len(captures) > 1:
        filters.append(make_filter("stagekit_visibility_link", "One window capture at a time", {
            "mode": 0, "items": [{"value": n, "selected": False, "hidden": False} for n in captures]}))
        for item, program in (("Blender", "blender"), ("Zbrush", "zbrush"), ("Fusion 360", "fusion360")):
            if item in item_names:
                filters.append(make_filter("stagekit_program_trigger", f"Show {item} while it is open", {
                    "program": program, "when": 0,
                    "items": [{"value": item, "selected": False, "hidden": False}]}))
        report.append("  Visibility Link (one window capture at a time) and a Program Trigger per program")
    return filters


stagekit_sources()
for scene in PRODUCTION:
    build_scene(scene)
camera_spots()
finish_filters()

order = PRODUCTION + sorted(n for n, s in new_sources.items() if s["id"] == "scene" and n not in PRODUCTION)
out = copy.deepcopy(old)
out["name"] = "StageKit Rebuild"
out["sources"] = [s for n, s in new_sources.items() if s["id"] != "group"]
out["groups"] = list(new_groups.values())
out["scene_order"] = [{"name": n} for n in order]
out["current_scene"] = out["current_program_scene"] = "Stream Start"
# The old macros point at scenes that no longer exist.
out["modules"].pop("advanced-scene-switcher", None)
json.dump(out, open(OUT, "w", encoding="utf-8"), indent=1)

kinds = {}
for s in out["sources"]:
    kinds[s["id"]] = kinds.get(s["id"], 0) + 1
summary = [
    f"Original: {len(old['sources'])} sources ({sum(1 for s in old['sources'] if s['id'] == 'scene')} scenes), "
    f"{sum(len(s.get('filters', [])) for s in old['sources'])} filters",
    f"Rebuilt:  {len(out['sources'])} sources ({kinds.get('scene', 0)} scenes), "
    f"{sum(len(s.get('filters', [])) for s in out['sources'] + out['groups'])} filters",
    f"Scenes: {', '.join(order)}",
    f"Shadow, Glow & Outline on: {', '.join(sorted(shadowed))}",
    f"Ramp Blur on: {', '.join(sorted(n for n in blurred if n in new_sources))}",
]
open(REPORT, "w", encoding="utf-8").write("\n".join(summary) + "\n" + "\n".join(report) + "\n")
print("\n".join(summary))
