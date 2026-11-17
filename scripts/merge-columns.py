#!/usr/bin/env python3
"""Adds a second column to the Component layout scene of a StageKit collection, for a side-by-side trial.

merge-capture-scenes.py made Component-Capture-Scene: every capture on the left, the chat box, camera
and widget in a column on the right. Dolphin-GBA-Scene is the same layout with another column: the two
GBA windows of Dolphin and a camera view between them. This writes a copy of the collection in which
Component-Capture-Scene holds both columns, one shown at a time:

    Chat   (as today)  Chat Box Group, DSLR Component, Amuse Component
    GBA                Dolphin-GBA-Link  1, Dolphin-ABG-Link 2, DSLR GBA Link

A column is a mode, as in the holding scene: a marker source leads it and Link Visibility follows it.

It reads one collection and writes another; the one it reads is never changed. Everything already in
the collection is kept exactly as it is, except Component-Capture-Scene, which gets the items and
filters below (its own items and filters are untouched), two new sources, and " + Columns" added to
the collection's name. The script checks that itself before it writes anything. Dolphin-GBA-Scene and
every other old scene stay as they are.

What is added:

- Two marker sources, "Column: Chat" and "Column: GBA": colour sources of 2x2 pixels, fully
  transparent, so they draw nothing. Each is an item of Component-Capture-Scene at 0,0, just above
  the background, locked; Chat's is shown, GBA's hidden. Showing a marker is choosing its column.
- The GBA column's items, copied from Dolphin-GBA-Scene with their places and bounding boxes, hidden:
  the two windows just under the captures (as in Dolphin-GBA-Scene, where they are under the game) and
  the camera view just above DSLR Component.
- Dolphin-GBA-Scene's Fit to Space for the camera view (renamed if its name is taken). Its two Fit to
  Space filters for the windows name sources that are not in the collection, so they stay out: the
  windows keep their fixed bounding boxes, as they have today.
- "Link Visibility-Columns" (Show only one at a time) over the two markers, and one Link Visibility
  per column ("Link Visibility-Column Chat", "-Column GBA") set to Follow the first source, the marker
  first. A camera view a camera switch has off stays hidden when its column is chosen, and a column
  not chosen is kept hidden whatever else shows its items.
- "Show in Scenes-Mode" on each marker source: its column's switch, for Component-Capture-Scene only,
  following what is shown; Chat's is on. All change with a fade, 500 ms.
- No show or hide transition is written on any new item: the filters set them when OBS runs.

The report lists what was added, every choice made and a fidelity table: Dolphin-GBA-Scene against
Component-Capture-Scene with the GBA column and Game Capture shown, and Component-Capture-Scene as it
was read against the new one with the Chat column shown. A difference the report does not explain
stops the script: it writes the report, writes no collection and exits with an error.

Usage:
    python3 -I scripts/merge-columns.py <in.json> <out.json> <report.txt>
"""
import copy
import json
import os
import sys
import uuid

# ---------------------------------------------------------------------------------------------- plan
SCENE = "Component-Capture-Scene"
FROM_SCENE = "Dolphin-GBA-Scene"
NAME_SUFFIX = " + Columns"
SWITCH_NAME = "Show in Scenes-Mode"
COLUMNS_LINK = "Link Visibility-Columns"
TRANSITION = "fade_transition"
CHANGE_MS = 500
FIT = "stagekit_fit_item"
SHOW_IN_SCENES = "stagekit_show_in_scenes"
LINK = "stagekit_visibility_link"
ONLY_ONE, FOLLOW = 0, 2  # LinkMode in src/placement/visibility-link.cpp
GAME = "Game Capture"
NO_TRANSITION = {"duration": 0}
LOOK = ["pos", "scale", "rot", "align", "bounds_type", "bounds_align", "bounds", "bounds_crop", "crop_left",
        "crop_top", "crop_right", "crop_bottom", "scale_filter", "blend_method", "blend_type"]
MARKER_SIZE = 2

# (column, its marker, its items, the scene its items are copied from or None when already there).
# The first column is the one shown in the file.
COLUMNS = [
    ("Chat", "Column: Chat", ["Chat Box Group", "DSLR Component", "Amuse Component"], None),
    ("GBA", "Column: GBA", ["Dolphin-GBA-Link  1", "Dolphin-ABG-Link 2", "DSLR GBA Link"], FROM_SCENE),
]
# Where each new item goes: straight above the item named, bottom to top in this order.
PUT_ABOVE = [("Column: Chat", "Ambient Background"), ("Column: GBA", "Column: Chat"),
             ("Dolphin-ABG-Link 2", "Column: GBA"), ("Dolphin-GBA-Link  1", "Dolphin-ABG-Link 2"),
             ("DSLR GBA Link", "DSLR Component")]
MARKERS = [c[1] for c in COLUMNS]
COPIED = [n for c in COLUMNS if c[3] for n in c[2]]
COLUMN_OF = {n: c[0] for c in COLUMNS for n in [c[1]] + c[2]}

if len(sys.argv) != 4:
    sys.exit(__doc__)
SRC, OUT, REPORT = sys.argv[1:4]
for other in (OUT, REPORT):
    if os.path.realpath(other) == os.path.realpath(SRC):
        sys.exit("The collection that is read is never written to: give another file to write.")
if os.path.realpath(OUT) == os.path.realpath(REPORT):
    sys.exit("The collection and the report must be two files.")

with open(SRC, encoding="utf-8") as f:
    text = f.read()
data = json.loads(text)
before = json.loads(text)  # never touched: what the result is compared with at the end
everything = data.get("sources", []) + data.get("groups", [])
by_name = {s.get("name"): s for s in everything}


def top_items(scene):
    """A scene's own items, bottom to top, without the backup copies OBS writes of a group's items."""
    return [it for it in scene["settings"]["items"] if not it.get("group_item_backup")]


def item_of(scene_name, name):
    found = [it for it in top_items(by_name[scene_name]) if it.get("name") == name]
    return found[0] if len(found) == 1 else None


# ------------------------------------------------------------------------- is everything there?
missing = []
for scene_name in (SCENE, FROM_SCENE):
    if scene_name not in by_name or by_name[scene_name].get("id") != "scene":
        missing.append(f"the scene '{scene_name}' is not in the collection")
if not missing:
    for column, marker, names, from_scene in COLUMNS:
        if marker in by_name:
            missing.append(f"there is already something called '{marker}': this collection was merged before")
        for n in names:
            if n not in by_name:
                missing.append(f"the source '{n}' is not in the collection")
            elif from_scene is None and item_of(SCENE, n) is None:
                missing.append(f"{SCENE} does not hold '{n}' exactly once")
            elif from_scene is not None and item_of(from_scene, n) is None:
                missing.append(f"{from_scene} does not hold '{n}' exactly once")
            elif from_scene is not None and any(it.get("name") == n for it in by_name[SCENE]["settings"]["items"]):
                missing.append(f"{SCENE} already holds '{n}'")
            elif from_scene is not None and by_name[n].get("id") == "group":
                missing.append(f"'{n}' is a group, which this script does not copy")
    for name, anchor in PUT_ABOVE:
        if anchor not in MARKERS + COPIED and item_of(SCENE, anchor) is None:
            missing.append(f"{SCENE} does not hold '{anchor}' exactly once")
    if item_of(FROM_SCENE, GAME) is None or item_of(SCENE, GAME) is None:
        missing.append(f"'{GAME}' is not in both {SCENE} and {FROM_SCENE}")
    if not any(s.get("id") == "color_source" for s in data.get("sources", [])):
        missing.append("the collection has no colour source to take a colour source's fields from")
if sorted(n for n, _a in PUT_ABOVE) != sorted(MARKERS + COPIED):
    missing.append("the plan does not place every new item once (a fault in the script)")
if missing:
    sys.exit("Nothing was written. The plan cannot be carried out on this collection:\n  "
             + "\n  ".join(dict.fromkeys(missing)))

# ------------------------------------------------------------------------------------- helpers
report, choices, warnings, stripped = [], [], [], []
taken = {s.get("uuid") for s in everything}
taken |= {f.get("uuid") for s in everything for f in s.get("filters", [])}
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_DNS, "merge-columns.stagekit")


def uid(what):
    """A uuid for one new thing. The same collection gives the same ones each time the script is run."""
    made = str(uuid.uuid5(NAMESPACE, data.get("name", "") + "|" + what))
    if made in taken:
        sys.exit(f"Nothing was written: the uuid made for {what} is already in the collection.")
    taken.add(made)
    return made


def num(v):
    if v is None:
        return "not set"
    return ("%.4f" % v).rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def say(v):
    if isinstance(v, dict):
        return ",".join(num(v[k]) for k in sorted(v))
    return num(v)


def entries(names):
    """A list the way StageKit's PickList keeps one."""
    return [{"value": n, "selected": False, "hidden": False} for n in names]


def a_filter(kind):
    for s in everything:
        for f in s.get("filters", []):
            if f.get("id") == kind:
                return f
    sys.exit(f"Nothing was written: the collection has no {kind} filter to take a filter's fields from.")


def make_filter(like, filter_id, name, settings, enabled, what):
    """A filter entry with every field OBS writes, taken from a filter already in the file."""
    made = copy.deepcopy(like)
    made.update({"name": name, "uuid": uid(what), "id": filter_id, "versioned_id": filter_id, "settings": settings,
                 "enabled": enabled, "hotkeys": {}, "private_settings": {}})
    return made


scene = by_name[SCENE]
from_scene = by_name[FROM_SCENE]
ids = [it["id"] for it in top_items(scene)]  # (a group's backup entries carry the group's own ids)
next_id = max([scene["settings"].get("id_counter", 0)] + ids) + 1
new_items = {}

# --------------------------------------------------------------------------------- the markers
report.append("\nNew sources")
color_like = next(s for s in data["sources"] if s.get("id") == "color_source")
switch_like = a_filter(SHOW_IN_SCENES)
markers = []
for i, (column, marker, names, _from) in enumerate(COLUMNS):
    on = i == 0
    src = copy.deepcopy(color_like)
    switch = make_filter(switch_like, SHOW_IN_SCENES, SWITCH_NAME,
                         {"sources|apick": "", "scenes|apick": "", "where": 1, "scenes": entries([SCENE]),
                          "on_means": 0, "transition": TRANSITION, "transition_ms": CHANGE_MS, "follow": True},
                         on, f"{marker}|filter|{SWITCH_NAME}")
    src.update({"name": marker, "uuid": uid(f"{marker}|source"),
                "settings": {"color": 0, "width": MARKER_SIZE, "height": MARKER_SIZE},
                "hotkeys": {}, "private_settings": {}, "filters": [switch]})
    markers.append(src)
    report.append(f"  '{marker}': colour source, {MARKER_SIZE}x{MARKER_SIZE}, fully transparent; filter "
                  f"'{SWITCH_NAME}' (Show in Scenes, {'on' if on else 'off'}): this source, in {SCENE} only; on means "
                  f"shown; Fade, {CHANGE_MS} ms; follows what is shown")
    like = copy.deepcopy(item_of(SCENE, "Ambient Background"))  # an item at 0,0, scale 1, no box, no crop
    for k, v in {"rot": 0.0, "align": 5, "bounds_type": 0, "bounds_align": 0, "bounds_crop": False, "crop_left": 0,
                 "crop_top": 0, "crop_right": 0, "crop_bottom": 0}.items():
        if like.get(k) != v:
            sys.exit(f"Nothing was written: the background item in {SCENE} is not a plain item at 0,0 ({k}).")
    like.update({"name": marker, "source_uuid": src["uuid"], "visible": on, "locked": True,
                 "show_transition": dict(NO_TRANSITION), "hide_transition": dict(NO_TRANSITION),
                 "private_settings": {}})
    new_items[marker] = like

# ------------------------------------------------------------------------------ the GBA column
for n in COPIED:
    item = copy.deepcopy(item_of(FROM_SCENE, n))
    for key in ("show_transition", "hide_transition"):
        if key in item and item[key] != NO_TRANSITION:
            stripped.append(f"'{n}' ({item[key].get('id', '?')} as its {key.replace('_', ' ')} in {FROM_SCENE})")
        if key in item:
            item[key] = dict(NO_TRANSITION)
    for key in ("show_transition_duration", "hide_transition_duration"):
        item.pop(key, None)
    if item.get("locked"):
        choices.append(f"'{n}' is locked in {FROM_SCENE}; it is not locked in {SCENE}.")
    item.update({"visible": False, "locked": False})
    new_items[n] = item

# Ids in the order the items are placed; then into the scene, each straight above its anchor.
items = scene["settings"]["items"]
for name, anchor in PUT_ABOVE:
    new_items[name]["id"] = next_id
    scene["hotkeys"][f"libobs.show_scene_item.{next_id}"] = []
    scene["hotkeys"][f"libobs.hide_scene_item.{next_id}"] = []
    next_id += 1
    at = [i for i, it in enumerate(items) if it.get("name") == anchor and not it.get("group_item_backup")][0]
    items.insert(at + 1, new_items[name])
scene["settings"]["id_counter"] = next_id - 1

# ------------------------------------------------------------------------------------ filters
added_filters = []
fit_names = {f.get("name") for f in scene.get("filters", [])}
left_out = []
for f in from_scene.get("filters", []):
    if f.get("id") != FIT:
        continue
    item = f["settings"].get("item", "")
    if item == GAME:
        continue
    if item not in COPIED:
        why = "names a source that is not in the collection" if item not in by_name else "is for another item"
        left_out.append(f"'{f['name']}' (fits '{item}', which {why})")
        continue
    name = f["name"]
    if name in fit_names:
        k = 2
        while f"{name} ({k})" in fit_names:
            k += 1
        choices.append(f"'{name}' is already a filter's name in {SCENE}; the copy is called '{name} ({k})'.")
        name = f"{name} ({k})"
    fit_names.add(name)
    added_filters.append(make_filter(f, FIT, name, copy.deepcopy(f["settings"]), f.get("enabled", True),
                                     f"{SCENE}|filter|{name}"))
link_like = a_filter(LINK)
links = [(COLUMNS_LINK, ONLY_ONE, MARKERS)] + [(f"Link Visibility-Column {c[0]}", FOLLOW, [c[1]] + c[2])
                                               for c in COLUMNS]
for link_name, mode, names in links:
    if link_name in fit_names:
        sys.exit(f"Nothing was written: {SCENE} already has a filter called '{link_name}'.")
    settings = {"mode": mode, "items": entries(names), "items|apick": "", "transition": TRANSITION,
                "transition_ms": CHANGE_MS, "lock_hidden": False}
    added_filters.append(make_filter(link_like, LINK, link_name, settings, True, f"{SCENE}|filter|{link_name}"))
scene["filters"] = scene.get("filters", []) + added_filters
data["sources"].extend(markers)
data["name"] = data.get("name", "") + NAME_SUFFIX

report.append(f"\n{SCENE}: items added (bottom to top among all its items; the number is the item's id)")
for it in top_items(scene):
    if it["name"] in new_items:
        box = f"box {say(it['bounds'])}" if it.get("bounds_type") else f"scale {say(it['scale'])}"
        role = "marker" if it["name"] in MARKERS else f"column {COLUMN_OF[it['name']]}"
        report.append(f"  {it['id']:>2}. {it['name']}: {'shown' if it['visible'] else 'hidden'}; {role}; "
                      f"{'locked; ' if it.get('locked') else ''}at {say(it['pos'])}, {box}"
                      + (f"; copied from {FROM_SCENE}" if it["name"] in COPIED else ""))
report.append(f"  Whole stack now, bottom to top: {', '.join(it['name'] for it in top_items(scene))}")
report.append(f"\n{SCENE}: filters added after its own")
for f in added_filters:
    st = f["settings"]
    if f["id"] == FIT:
        area = {0: "The whole screen", 1: "A box set in the filter", 2: "Free space around source"}.get(
            st.get("box", 0), "?")
        report.append(f"  '{f['name']}' (Fit to Space, {'on' if f.get('enabled', True) else 'off'}): {st.get('item')}; "
                      f"{area}; gap {num(st.get('margin', 30.0))}; resize {st.get('resize', True)}; towards "
                      f"{st.get('toward', 0)}; copied from {FROM_SCENE}")
    else:
        report.append(f"  '{f['name']}' (Link Visibility, on): "
                      f"{'Show only one at a time' if st['mode'] == ONLY_ONE else 'Follow the first source'}; "
                      f"{', '.join(e['value'] for e in st['items'])}; Fade, {CHANGE_MS} ms; hidden ones not locked")
report.append(f"  Not copied from {FROM_SCENE}: " + ("; ".join(left_out) if left_out else "nothing")
              + ". The two windows keep their fixed bounding boxes, as they have today.")

# Stacking order against Dolphin-GBA-Scene.
order_new = [it["name"] for it in top_items(scene)]
order_old = [it["name"] for it in top_items(from_scene) if it["name"] in order_new]
for i, a in enumerate(order_old):
    for b in order_old[i + 1:]:
        if order_new.index(a) > order_new.index(b):
            choices.append(f"'{a}' is under '{b}' in {FROM_SCENE} and over it in {SCENE} (kept where {SCENE} has it).")

# ------------------------------------------------------------------------------ fidelity table
fidelity, unexplained = [], 0


def compare(old_seen, new_seen, explain):
    """Lines of (explained, text) for two lists of shown items."""
    out = []
    old_at, new_at = {it["name"]: it for it in old_seen}, {it["name"]: it for it in new_seen}
    for it in old_seen:
        if it["name"] not in new_at:
            out.append(explain(it["name"], "missing", None) or (False, f"{it['name']}: shown here, not in the new scene"))
    for it in new_seen:
        if it["name"] not in old_at:
            out.append(explain(it["name"], "extra", None) or (False, f"{it['name']}: not shown here, shown in the new scene"))
    both_old = [it["name"] for it in old_seen if it["name"] in new_at]
    both_new = [it["name"] for it in new_seen if it["name"] in old_at]
    for i, a in enumerate(both_old):
        for b in both_old[i + 1:]:
            if both_new.index(a) > both_new.index(b):
                out.append((False, f"order: {a} is under {b} here, over it in the new scene"))
    for name in both_old:
        diff = [f"{k} {say(old_at[name].get(k))} here, {say(new_at[name].get(k))} in the new scene"
                for k in LOOK if old_at[name].get(k) != new_at[name].get(k)]
        if diff:
            out.append(explain(name, "look", "; ".join(diff)) or (False, f"{name}: {'; '.join(diff)}"))
    return out, both_new


def gba_explain(name, what, diff):
    if what == "extra" and name == "Column: GBA":
        return True, f"{name}: the column's marker, which draws nothing"
    if what == "missing" and name == "Amuse Component":
        return True, f"{name}: in the Chat column, so hidden with it ({FROM_SCENE} has no Amuse Component)"
    if what == "look" and name == "Alerts":
        return True, f"Alerts: {diff} [as {SCENE} has it today]"
    if what == "look" and name == GAME:
        return True, (f"{GAME}: {diff} [its bounding box is set by its Fit to Space each frame; {SCENE}'s copy of "
                      f"'Fit the game' is its own, and the space depends on which column is shown]")
    if what == "look" and name in ("Ambient Background", "Discord"):
        return True, f"{name}: {diff} [one copy in {SCENE}, as it has it today]"
    return None


def chat_explain(name, what, diff):
    if what == "extra" and name == "Column: Chat":
        return True, f"{name}: the column's marker, which draws nothing"
    return None


def shown_in_new(column):
    out = []
    for it in top_items(scene):
        n = it["name"]
        if n in COLUMN_OF:
            if COLUMN_OF[n] == column:
                out.append(it)
        elif n == GAME or (it.get("visible") and n not in ("HD60S", "Window Capture", "Main Display",
                                                             "Tablet Display Capture")):
            out.append(it)
    return out


rows = [(f"{FROM_SCENE}  ->  {SCENE} with the GBA column and {GAME} shown",
         [it for it in top_items(from_scene) if it.get("visible")], shown_in_new("GBA"), gba_explain),
        (f"{SCENE} as read  ->  {SCENE} with the Chat column shown",
         [it for it in top_items(by_name[SCENE]) if it.get("visible") and it["name"] not in new_items],
         shown_in_new("Chat"), chat_explain)]
game_notes = []
game_fits = {s: [f for f in by_name[s].get("filters", []) if f.get("id") == FIT and f["settings"].get("item") == GAME]
             for s in (FROM_SCENE, SCENE)}
for s, fs in game_fits.items():
    for f in fs:
        st = f["settings"]
        game_notes.append(f"      . '{f['name']}' in {s}: gap {num(st.get('margin', 30.0))}, resize "
                        f"{st.get('resize', True)}, towards {st.get('toward', 0)}, ignores "
                        f"{', '.join(e.get('value', '') for e in st.get('skip', [])) or 'nothing'}")

verdicts = []
for title, old_seen, new_seen, explain in rows:
    found, both = compare(old_seen, new_seen, explain)
    bad = [t for ok, t in found if not ok]
    unexplained += len(bad)
    verdict = "same" if not found else (f"{len(found)} difference(s), all explained" if not bad else
                                        f"{len(bad)} UNEXPLAINED difference(s) of {len(found)}")
    fidelity.append(f"  {title}: {verdict}")
    fidelity.extend(f"      {'-' if ok else '!! UNEXPLAINED:'} {t}" for ok, t in found)
    fidelity.append(f"      shown in both, bottom to top: {', '.join(both)}")
    if explain is gba_explain:
        fidelity.extend(game_notes)
        fidelity.append(f"      . Amuse Component is in neither: {FROM_SCENE} has none, and in {SCENE} it is in the "
                        "Chat column")
    verdicts.append((title, verdict))
# ---------------------------------------------------------------------------------- warnings
for s in everything:
    for f in s.get("filters", []):
        st = f.get("settings", {})
        if f.get("id") == SHOW_IN_SCENES and st.get("where", 0) == 0:
            listed = [e.get("value") for e in st.get("sources", [])] or [s.get("name")]
            here = [n for n in listed if n in COLUMN_OF and n not in MARKERS]
            if here:
                warnings.append(f"'{f.get('name')}' on '{s.get('name')}' acts in every scene: in {SCENE} it shows "
                                f"{', '.join(here)} only while its column is shown, and hides them in any column.")
for f in from_scene.get("filters", []):
    if f.get("id") not in (FIT,):
        warnings.append(f"{FROM_SCENE} has the filter '{f.get('name')}' ({f.get('id')}), not carried over.")

# --------------------------------------------------- check: nothing else in the collection changed
problems = []
for key in set(before) | set(data):
    if key not in ("sources", "name") and before.get(key) != data.get(key):
        problems.append(f"'{key}' changed")
if data["name"] != before.get("name", "") + NAME_SUFFIX:
    problems.append("the collection's name is not the old one with the suffix")
old_sources = before["sources"]
if len(data["sources"]) != len(old_sources) + len(markers) or data["sources"][len(old_sources):] != markers:
    problems.append("the new sources are not the two markers, added last")
for was, now in zip(old_sources, data["sources"]):
    if was.get("name") != SCENE and was != now:
        problems.append(f"'{was.get('name')}' changed")
    if was.get("name") == SCENE:
        rest = ("settings", "filters", "hotkeys")
        if {k: v for k, v in was.items() if k not in rest} != {k: v for k, v in now.items() if k not in rest}:
            problems.append(f"{SCENE} changed in more than its items, filters and hotkeys")
        if now["filters"][:len(was.get("filters", []))] != was.get("filters", []):
            problems.append(f"{SCENE}'s own filters changed")
        if [it for it in now["settings"]["items"] if it["name"] not in new_items] != was["settings"]["items"]:
            problems.append(f"{SCENE}'s own items changed")
        if {k: v for k, v in now["hotkeys"].items() if k in was["hotkeys"]} != was["hotkeys"]:
            problems.append(f"{SCENE}'s own hotkeys changed")
        all_ids = [it["id"] for it in top_items(now)]
        if len(all_ids) != len(set(all_ids)) or now["settings"]["id_counter"] != max(all_ids):
            problems.append("item ids are not unique or the id counter is wrong")
        want = {"OBSBasic.SelectScene"} | {f"libobs.{w}_scene_item.{i}" for i in all_ids for w in ("show", "hide")}
        if set(now["hotkeys"]) != want:
            problems.append(f"{SCENE}'s hotkeys do not match its items")
        names = [f["name"] for f in now["filters"]]
        if len(names) != len(set(names)):
            problems.append(f"two filters on {SCENE} share a name")
for it in top_items(scene):
    if it["name"] in new_items:
        if it.get("source_uuid") != (by_name.get(it["name"]) or next(m for m in markers if m["name"] == it["name"]))[
                "uuid"]:
            problems.append(f"'{it['name']}' does not point at its source")
        if it.get("show_transition") != NO_TRANSITION or it.get("hide_transition") != NO_TRANSITION:
            problems.append(f"'{it['name']}' carries a transition")
    if it["name"].endswith(" (standby)") and it["name"] in new_items:
        problems.append(f"'{it['name']}' is a stand-in, which only StageKit makes")
if [it["name"] for it in top_items(scene) if it["name"] in MARKERS and it["visible"]] != [MARKERS[0]]:
    problems.append(f"not exactly '{MARKERS[0]}' shown among the markers")
if any(it["visible"] for it in top_items(scene) if it["name"] in COPIED):
    problems.append("an item of the GBA column is shown")
uuids = [s.get("uuid") for s in data["sources"] + data.get("groups", [])]
uuids += [f.get("uuid") for s in data["sources"] + data.get("groups", []) for f in s.get("filters", [])]
if len(uuids) != len(set(uuids)):
    problems.append("two things share a uuid")

# ------------------------------------------------------------------------------------ write
ok = not problems and not unexplained
head = [
    f"Columns in the Component layout scene, side-by-side trial: {before.get('name', '')}  ->  {data['name']}",
    f"Read: {SRC}",
    (f"Written: {OUT}" if ok else "NO COLLECTION WAS WRITTEN: see the problems below."),
    f"Checked by the script: everything that was in the collection is equal before and after, apart from {SCENE}'s "
    "new items, filters and hotkeys and the two new sources: "
    + ("yes." if not problems else "NO: " + "; ".join(problems) + "."),
    "",
    "Columns (marker + items; the first is shown in the file)",
] + [f"  {c}: {m} + {', '.join(n)}" + (f"  (copied from {s})" if s else "  (already in the scene)")
     for c, m, n, s in COLUMNS]
tail = ["", "Choices"] + [f"  {c}" for c in choices or ["none"]]
tail += ["", "Transitions left off the new items (the filters set them when OBS runs)"]
tail += [f"  {s}" for s in stripped or ["none"]]
tail += ["", "Set each column's Stream Deck key to On press: Switch on. A Toggle key pressed on the column showing",
         "hides it, and no column is left showing.",
         "Showing a column's item with the eye while its column is not shown is undone at once; hiding one while its",
         "column is shown leaves it hidden. Only a marker changes the column."]
tail += ["", "Fidelity",
         "  Compared: which items are shown, their order, and each one's place, scale, rotation, alignment, bounding "
         "box, crop and blending. '-' is explained; '.' is for information."] + fidelity
tail += ["", "Warnings (nothing was changed for these)"] + [f"  {w}" for w in warnings or ["none"]]
with open(REPORT, "w", encoding="utf-8") as f:
    f.write("\n".join(head + report + tail) + "\n")
if not ok:
    sys.exit(f"No collection was written. {len(problems)} problem(s) in the self-check, {unexplained} unexplained "
             f"difference(s) in the fidelity table. See {REPORT}")
with open(OUT, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
print(f"Wrote {OUT}")
print(f"Report: {REPORT}")
for title, verdict in verdicts:
    print(f"  {title}: {verdict}")
print(f"{len(warnings)} warning(s), {len(choices)} choice(s). Read the report.")
