#!/usr/bin/env python3
"""Adds two layout scenes to an OBS scene collection that uses StageKit, for a side-by-side trial.

Today each capture (game, capture card, window, two displays) has a scene of its own for each layout.
This writes a copy of the collection with two more scenes that each hold every capture, one shown at
a time:

    Live         in time replaces the five full-screen capture scenes
    Chat Live    in time replaces the three component capture scenes (the capture with the chat box,
                 camera and widget beside it)

Both were first called Full-Capture-Scene and Component-Capture-Scene; the owner renamed them
(2026-11-17: "Full capture should be called Live ... Component should be called Chat Live").

It reads one collection and writes another; the one it reads is never changed. Everything already in
the collection is kept exactly as it is, with two exceptions: each capture source gets one more filter
at the end of its list ("Show in Scenes-Capture"), and the collection's name gets " - Layout Scenes"
added. The script checks that itself before it writes anything.

What the new scenes are made of:

- Items are copies of the items in today's scenes, place and size included; nothing is placed by
  number here. Where today's scenes disagree (stacking order, where Alerts is), the report says so
  and what was chosen.
- One capture is shown; the others are hidden and locked.
- The Fit to Space filters of today's scenes are copied over, one per capture.
- "Link Visibility-Captures" on each new scene shows only one capture at a time, and
  "Show in Scenes-Capture" on each capture is its switch for both scenes. Both change with
  Morph (StageKit), 700 ms. Each switch is set to go to Live when it is pressed on (go_to): OBS
  changes to Live unless the scene on screen already shows that capture (Chat Live does).
- No show or hide transition is written on any new item: the filters set them when OBS runs.

The report lists both scenes item by item and filter by filter, every choice made, and a fidelity
table: each of today's eight scenes compared with its new scene as it looks with that capture shown.
A difference that neither an option nor the choice of one copy explains stops the script: it writes
the report, writes no collection and exits with an error.

Usage:
    python3 -I scripts/merge-capture-scenes.py <in.json> <out.json> <report.txt> [options]

Options:
    --alerts-full centred|off        Alerts in Live: centred as in the window and display
                                     scenes (default), or off the canvas as in the two game scenes
    --widget under|over              Amuse Square Camera under (default) or over Square Camera
    --pureref-full hidden|shown      Pureref in Live (default hidden)
    --displays-in-component yes|no   the two displays in Chat Live too (default yes)
    --shown NAME                     the capture shown in both scenes (default "Game Capture")
"""
import argparse
import copy
import json
import os
import sys
import uuid

# ---------------------------------------------------------------------------------------------- plan
CAPTURES = ["Game Capture", "HD60S", "Window Capture", "Main Display", "Tablet Display Capture"]
DISPLAYS = ["Main Display", "Tablet Display Capture"]
AFTER_SCENE = "Stream Start"  # the new scenes go straight after this one in the scene list
NAME_SUFFIX = " - Layout Scenes"
SWITCH_NAME = "Show in Scenes-Capture"
LINK_NAME = "Link Visibility-Captures"
FULL_NAME, COMPONENT_NAME = "Live", "Chat Live"
# What the two scenes were called before the owner renamed them: a collection holding one of these
# was merged by an earlier version of this script.
OLD_NAMES = ["Full-Capture-Scene", "Component-Capture-Scene"]
# The scene a capture's switch goes to when it is pressed on (Show in Scenes' go_to): OBS changes to
# it unless the scene on screen already shows the capture.
GO_TO = FULL_NAME
MORPH = "stagekit_morph"
CHANGE_MS = 700
FIT = "stagekit_fit_item"
SHOW_IN_SCENES = "stagekit_show_in_scenes"
LINK = "stagekit_visibility_link"
STANDBY = "stagekit_standby"
# What a Fit to Space filter writes down about its own item (src/placement/fit-engine.cpp, and
# kOwnSettings in src/dock/arrange.cpp): its home point, the size it keeps, the size from before
# the filter, and the place and size it was laid out at by hand (home_*). A copy made for another
# source starts without them.
FIT_OWN = ["have_anchor", "anchor_x", "anchor_y", "orig_item", "orig_k", "orig_w", "orig_h", "keep_item", "keep_k",
           "keep_w", "keep_h", "keep_sw", "keep_sh", "keep_pw", "keep_ph",
           "home_item", "home_x", "home_y", "home_w", "home_h", "home_space_x", "home_space_y", "home_space_w",
           "home_space_h", "home_cut_l", "home_cut_t", "home_cut_r", "home_cut_b"]
# How an item with no transition is written by OBS.
NO_TRANSITION = {"duration": 0}
# What is compared in the fidelity table.
LOOK = ["pos", "scale", "rot", "align", "bounds_type", "bounds_align", "bounds", "bounds_crop", "crop_left",
        "crop_top", "crop_right", "crop_bottom", "scale_filter", "blend_method", "blend_type"]

FULL = {
    "name": FULL_NAME,
    # Today's scenes and the capture each is for.
    "members": [("Game-Capture-Scene", "Game Capture"), ("Game-Capture-HD60S-Scene", "HD60S"),
                ("Window-Capture-Scene", "Window Capture"), ("Main Display Scene", "Main Display"),
                ("Tablet Scene", "Tablet Display Capture")],
    # Bottom to top as wanted, and the scene each item is copied from. The order is only what decides
    # where today's scenes say nothing or disagree; what they agree on comes first.
    "items": [("Ambient Background", "Game-Capture-Scene"), ("Game Capture", "Game-Capture-Scene"),
              ("HD60S", "Game-Capture-HD60S-Scene"), ("Window Capture", "Window-Capture-Scene"),
              ("Main Display", "Main Display Scene"), ("Tablet Display Capture", "Tablet Scene"),
              ("Elgato Virtual Cam", "Window-Capture-Scene"), ("Amuse Square Camera", "Game-Capture-Scene"),
              ("Square Camera", "Game-Capture-Scene"), ("Discord", "Game-Capture-HD60S-Scene"),
              ("Alerts", "Window-Capture-Scene"), ("Pureref", "Window-Capture-Scene")],
    "like": {},
}
COMPONENT = {
    "name": COMPONENT_NAME,
    "members": [("Game-Component-Capture-Scene", "Game Capture"), ("Game-Component-HD60S-Capture-Scene", "HD60S"),
                ("Window-Component-Capture-Scene", "Window Capture")],
    "items": [("Ambient Background", "Game-Component-Capture-Scene"), ("Game Capture", "Game-Component-Capture-Scene"),
              ("HD60S", "Game-Component-HD60S-Capture-Scene"), ("Window Capture", "Window-Component-Capture-Scene"),
              ("Main Display", None), ("Tablet Display Capture", None),
              ("Chat Box Group", "Game-Component-Capture-Scene"), ("DSLR Component", "Game-Component-Capture-Scene"),
              ("Amuse Component", "Game-Component-Capture-Scene"), ("Discord", "Game-Component-Capture-Scene"),
              ("Elgato Virtual Cam", "Window-Component-Capture-Scene"), ("Alerts", "Game-Component-Capture-Scene"),
              ("Phone Cam", "Window-Component-Capture-Scene"), ("Pureref", "Window-Component-Capture-Scene")],
    # Not in a Component scene today: given a copy of this capture's item and of its Fit to Space.
    "like": {"Main Display": "Game Capture", "Tablet Display Capture": "Game Capture"},
}
# The name a capture's Fit to Space gets when its own is taken on the new scene.
FIT_NAMES = {"Main Display": "Fit Main Display", "Tablet Display Capture": "Fit Tablet"}
WIDGET, CAMERA = "Amuse Square Camera", "Square Camera"

parser = argparse.ArgumentParser(usage=__doc__, add_help=False)
parser.add_argument("src")
parser.add_argument("out")
parser.add_argument("report")
parser.add_argument("--alerts-full", choices=["centred", "off"], default="centred")
parser.add_argument("--widget", choices=["under", "over"], default="under")
parser.add_argument("--pureref-full", choices=["hidden", "shown"], default="hidden")
parser.add_argument("--displays-in-component", choices=["yes", "no"], default="yes")
parser.add_argument("--shown", default="Game Capture")
if len(sys.argv) < 4:
    sys.exit(__doc__)
opt = parser.parse_args()
SRC, OUT, REPORT = opt.src, opt.out, opt.report
for other in (OUT, REPORT):
    if os.path.realpath(other) == os.path.realpath(SRC):
        sys.exit("The collection that is read is never written to: give another file to write.")
if os.path.realpath(OUT) == os.path.realpath(REPORT):
    sys.exit("The collection and the report must be two files.")

# The options, turned into the plan.
if opt.alerts_full == "off":
    FULL["items"] = [(n, "Game-Capture-Scene" if n == "Alerts" else s) for n, s in FULL["items"]]
if opt.widget == "over":
    names = [n for n, _ in FULL["items"]]
    a, b = names.index(WIDGET), names.index(CAMERA)
    FULL["items"][a], FULL["items"][b] = FULL["items"][b], FULL["items"][a]
if opt.displays_in_component == "no":
    COMPONENT["items"] = [(n, s) for n, s in COMPONENT["items"] if n not in DISPLAYS]
    COMPONENT["like"] = {}
PLANS = [FULL, COMPONENT]

with open(SRC, encoding="utf-8") as f:
    text = f.read()
data = json.loads(text)
before = json.loads(text)  # never touched: what the result is compared with at the end
by_name = {s.get("name"): s for s in data.get("sources", []) + data.get("groups", [])}
scene_names = [s.get("name") for s in data.get("scene_order", [])]

# ------------------------------------------------------------------------- is everything there?
missing = []


def top_items(scene):
    """A scene's own items, bottom to top. OBS also writes a group's items into the scene as a backup
    it does not load; those are not items of the scene."""
    return [it for it in scene["settings"]["items"] if not it.get("group_item_backup")]


def item_of(scene_name, name):
    found = [it for it in top_items(by_name[scene_name]) if it.get("name") == name]
    return found[0] if len(found) == 1 else None


if opt.shown not in CAPTURES:
    missing.append(f"--shown '{opt.shown}' is not one of the captures: {', '.join(CAPTURES)}")
for name in CAPTURES:
    if name not in by_name or by_name[name].get("id") in ("scene", "group"):
        missing.append(f"the capture source '{name}' is not in the collection")
    elif any(f.get("name") == SWITCH_NAME for f in by_name[name].get("filters", [])):
        missing.append(f"'{name}' already has a filter called '{SWITCH_NAME}': this collection was merged before")
if AFTER_SCENE not in scene_names:
    missing.append(f"the scene '{AFTER_SCENE}' is not in the scene list")
for old_name in OLD_NAMES:
    if old_name in by_name:
        missing.append(f"there is already a scene called '{old_name}': this collection was merged before, by an "
                       f"earlier version of this script")
for plan in PLANS:
    if plan["name"] in by_name:
        missing.append(f"there is already something called '{plan['name']}'")
    planned = [n for n, _ in plan["items"]]
    if opt.shown in CAPTURES and opt.shown not in planned:
        missing.append(f"--shown '{opt.shown}' is not in {plan['name']} with --displays-in-component no")
    for scene_name, capture in plan["members"]:
        if scene_name not in by_name or by_name[scene_name].get("id") != "scene":
            missing.append(f"the scene '{scene_name}' is not in the collection")
        elif item_of(scene_name, capture) is None:
            missing.append(f"'{scene_name}' does not hold '{capture}' exactly once")
    for name, scene_name in plan["items"]:
        if name not in by_name:
            missing.append(f"the source '{name}' is not in the collection")
        elif scene_name is None:
            like = plan["like"].get(name)
            if like is None or like not in planned:
                missing.append(f"{plan['name']}: nothing to copy '{name}' from")
        elif scene_name in by_name and by_name[scene_name].get("id") == "scene" and item_of(scene_name, name) is None:
            missing.append(f"'{scene_name}' does not hold '{name}' exactly once")
if missing:
    sys.exit("Nothing was written. The plan cannot be carried out on this collection:\n  "
             + "\n  ".join(dict.fromkeys(missing)))

# ------------------------------------------------------------------------------------- helpers
report, choices, warnings, stripped = [], [], [], []
taken = {s.get("uuid") for s in data.get("sources", []) + data.get("groups", [])}
taken |= {f.get("uuid") for s in data.get("sources", []) + data.get("groups", []) for f in s.get("filters", [])}
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_DNS, "merge-capture-scenes.stagekit")


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


def fits_of(scene):
    return [f for f in scene.get("filters", []) if f.get("id") == FIT]


def make_filter(like, filter_id, name, settings, enabled, what):
    """A filter entry with every field OBS writes, taken from a StageKit filter already in the file."""
    made = copy.deepcopy(like)
    made.update({"name": name, "uuid": uid(what), "id": filter_id, "versioned_id": filter_id, "settings": settings,
                 "enabled": enabled, "hotkeys": {}, "private_settings": {}})
    return made


def a_stagekit_filter(kind):
    for s in data.get("sources", []) + data.get("groups", []):
        for f in s.get("filters", []):
            if f.get("id") == kind:
                return f
    for s in data.get("sources", []) + data.get("groups", []):
        for f in s.get("filters", []):
            if str(f.get("id", "")).startswith("stagekit_"):
                return f
    sys.exit("Nothing was written: the collection has no StageKit filter to take a filter's fields from.")


# ------------------------------------------------------------------------------- stacking order
def merged_order(plan):
    """The new scene's items bottom to top: what today's scenes agree on, then the plan's order."""
    wanted = [n for n, _ in plan["items"]]
    place = {n: i for i, n in enumerate(wanted)}
    votes = {}  # (lower, upper) -> the scenes that have them that way round
    for scene_name, _ in plan["members"]:
        there = [it["name"] for it in top_items(by_name[scene_name]) if it["name"] in place]
        for i, low in enumerate(there):
            for high in there[i + 1:]:
                votes.setdefault((low, high), []).append(scene_name)
        for it in top_items(by_name[scene_name]):
            if it["name"] not in place:
                warnings.append(f"{scene_name} holds '{it['name']}', which the plan does not carry into {plan['name']}.")
    below = {n: set() for n in wanted}  # name -> what must lie under it
    unsaid = []
    for i, a in enumerate(wanted):
        for b in wanted[i + 1:]:
            ab, ba = votes.get((a, b), []), votes.get((b, a), [])
            if ab and ba:
                # Today's scenes disagree: the plan's order decides, which is where the options act.
                why = f"--widget {opt.widget}" if {a, b} == {WIDGET, CAMERA} else "the plan's order"
                choices.append(f"{plan['name']}: '{a}' is under '{b}' in {', '.join(ab)} and over it in "
                               f"{', '.join(ba)}. Chosen: '{a}' under '{b}' ({why}).")
                below[b].add(a)
            elif ab:
                below[b].add(a)
            elif ba:
                below[a].add(b)
            else:
                unsaid.append((a, b))
    order, left = [], list(wanted)
    while left:
        ready = [n for n in left if not (below[n] & set(left))]
        if not ready:
            choices.append(f"{plan['name']}: today's scenes give no single order for {', '.join(left)}; "
                           f"the plan's order was used for these.")
            ready = left
        order.append(ready[0])
        left.remove(ready[0])
    if order != wanted:
        choices.append(f"{plan['name']}: today's scenes put the items in another order than the plan names. "
                       f"Used (bottom to top): {'; '.join(order)}. The plan said: {'; '.join(wanted)}.")
    else:
        report.append(f"  Stacking order: today's scenes agree with the order the plan names"
                      f"{', apart from the choices listed below' if any(c.startswith(plan['name']) for c in choices) else ''}.")
    extras = [(a, b) for a, b in unsaid
              if (a not in CAPTURES or b not in CAPTURES) and a not in plan["like"] and b not in plan["like"]]
    if extras:
        report.append("  No scene today holds these together, so the plan's order decided (lower first): "
                      + "; ".join(f"{a} / {b}" for a, b in extras) + ".")
    report.append("  The captures are never in one scene today; they are stacked in the plan's order, and only one is shown.")
    if plan["like"]:
        report.append(f"  {' and '.join(plan['like'])} are new here and lie where the plan's order puts them.")
    return order


# ---------------------------------------------------------------------------------- the scenes
new_scenes = []
origin = {}  # (new scene, new filter name) -> (old scene, old filter name) or None for a new one
copied_from = {}  # (new scene, item name) -> the scene its item was copied from
fit_template = a_stagekit_filter(FIT)

for plan in PLANS:
    new_name = plan["name"]
    report.append(f"\n{new_name}")
    order = merged_order(plan)
    source_scene = dict(plan["items"])
    captures_here = [n for n in order if n in CAPTURES]

    items, hotkeys = [], {"OBSBasic.SelectScene": []}
    for name in order:
        like = plan["like"].get(name) if source_scene[name] is None else None
        from_scene = source_scene[like] if like else source_scene[name]
        old_items = by_name[from_scene]["settings"]["items"]
        old = item_of(from_scene, like or name)
        at = old_items.index(old)
        # A group's backup entries lie straight under the group's own item; they go with it, as they are.
        first = at
        while first > 0 and old_items[first - 1].get("group_item_backup") and by_name[name].get("id") == "group":
            first -= 1
        items.extend(copy.deepcopy(old_items[first:at]))
        item = copy.deepcopy(old)
        item["name"] = name
        item["source_uuid"] = by_name[name]["uuid"]
        item["id"] = len(hotkeys) // 2 + 1
        if like:
            # The capture copied from has a shadow, whose room is in its bounding box; this one has none. Its box
            # is the space its Fit to Space was given, so the room it holds before it is first fitted is that space.
            fit = [f for f in fits_of(by_name[from_scene]) if f["settings"].get("item") == like]
            fs = fit[0]["settings"] if fit else {}
            if item.get("bounds_type") and fs.get("box_width") and fs.get("box_height"):
                item["bounds"] = {"x": fs["box_width"], "y": fs["box_height"]}
        if name in CAPTURES:
            item["visible"] = name == opt.shown
            if name != opt.shown:
                item["locked"] = True
        if name == "Pureref" and plan is FULL:
            item["visible"] = opt.pureref_full == "shown"
        for key in ("show_transition", "hide_transition"):
            if key in item and item[key] != NO_TRANSITION:
                stripped.append(f"{new_name}: '{name}' ({item[key].get('id', '?')} as its {key.replace('_', ' ')} "
                                f"in {from_scene})")
            if key in item:
                item[key] = dict(NO_TRANSITION)
        for key in ("show_transition_duration", "hide_transition_duration"):
            item.pop(key, None)
        items.append(item)
        hotkeys[f"libobs.show_scene_item.{item['id']}"] = []
        hotkeys[f"libobs.hide_scene_item.{item['id']}"] = []
        copied_from[(new_name, name)] = from_scene
        box = f"box {say(item['bounds'])}" if item.get("bounds_type") else f"scale {say(item['scale'])}"
        note = f"copied from {from_scene}"
        if like:
            note = (f"new here: a copy of {like}'s item in {from_scene}, its box the space {like}'s Fit to Space "
                    f"was given (no shadow room)")
        report.append(f"  {item['id']:>2}. {name}: {'shown' if item['visible'] else 'hidden'}"
                      f"{', locked' if item.get('locked') else ''}; at {say(item['pos'])}, {box}; {note}")
        if like:
            choices.append(f"{new_name}: '{name}' is in no component scene today. It was given a copy of {like}'s "
                           f"item from {from_scene}, and a Fit to Space like {like}'s.")

    # Fit to Space: each capture's own from its own scene; the others' once, from the first scene that has one.
    filters, used, have_for, told = [], set(), {}, {}

    def add_fit(old, old_scene, name, item_name=None):
        settings = copy.deepcopy(old["settings"])
        if item_name:
            settings["item"] = item_name
            for key in FIT_OWN:
                settings.pop(key, None)
        made = make_filter(old, FIT, name, settings, old.get("enabled", True), f"{new_name}|filter|{name}")
        filters.append(made)
        used.add(name)
        origin[(new_name, name)] = None if item_name else (old_scene, old["name"])
        s = made["settings"]
        area = {0: "The whole screen", 1: "A box set in the filter", 2: "Free space around source"}.get(s.get("box", 0), "?")
        was = f"'{old['name']}' in {old_scene}"
        told[name] = [f"  filter '{name}' (Fit to Space, {'on' if made['enabled'] else 'off'}): {s.get('item')}; {area}; "
                      f"gap {num(s.get('margin', 30.0))}; "
                      + (f"a copy of {was} for another source, without its home point and kept sizes"
                         if item_name else f"copied from {was}")]
        if old["name"] != name and not item_name:
            choices.append(f"{new_name}: the Fit to Space '{old['name']}' of {old_scene} is called '{name}' here, "
                           f"because two filters on one scene need two names.")

    for scene_name, capture in plan["members"]:
        for old in fits_of(by_name[scene_name]):
            target = old["settings"].get("item", "")
            if target in CAPTURES:
                name = old["name"]
                if name in used:
                    name = FIT_NAMES.get(target, f"Fit {target}")
                n = 2
                while name in used:
                    name = f"{FIT_NAMES.get(target, 'Fit ' + target)} {n}"
                    n += 1
                add_fit(old, scene_name, name)
            elif target in have_for:
                told[have_for[target][1]].append(f"      (used for every capture; not copied: '{old['name']}' of "
                                                 f"{scene_name}, also for {target})")
            else:
                name, n = old["name"], 2
                while name in used:
                    name = f"{old['name']} {n}"
                    n += 1
                add_fit(old, scene_name, name)
                have_for[target] = (scene_name, name)
        for f in by_name[scene_name].get("filters", []):
            if f.get("id") != FIT:
                warnings.append(f"{scene_name} has the filter '{f.get('name')}' ({f.get('id')}), which is not carried "
                                f"into {new_name}.")
    for name, like in plan["like"].items():
        from_scene = source_scene[like]
        own = [f for f in fits_of(by_name[from_scene]) if f["settings"].get("item") == like]
        if not own:
            warnings.append(f"{from_scene} has no Fit to Space for {like}, so '{name}' has none in {new_name}.")
            continue
        fit_name, n = FIT_NAMES.get(name, f"Fit {name}"), 2
        while fit_name in used:
            fit_name = f"{FIT_NAMES.get(name, 'Fit ' + name)} {n}"
            n += 1
        add_fit(own[0], from_scene, fit_name, name)
    # The captures' first, in the order of their items; then the others in the order they had in the scene
    # they were copied from (a scene's filters tick in list order: the camera's before the chat box's, as today).
    filters.sort(key=lambda f: order.index(f["settings"]["item"]) if f["settings"].get("item") in CAPTURES else len(order))
    for f in filters:
        report.extend(told[f["name"]])

    link_settings = {"mode": 0, "items": entries(captures_here), "items|apick": "", "transition": MORPH,
                     "transition_ms": CHANGE_MS, "lock_hidden": True}
    filters.append(make_filter(fit_template, LINK, LINK_NAME, link_settings, True, f"{new_name}|filter|{LINK_NAME}"))
    origin[(new_name, LINK_NAME)] = None
    report.append(f"  filter '{LINK_NAME}' (Link Visibility, on): Show only one at a time; {', '.join(captures_here)}; "
                  f"Morph (StageKit), {CHANGE_MS} ms; hidden ones locked (lock_hidden)")

    scene = copy.deepcopy(by_name[plan["members"][0][0]])
    scene.update({"name": new_name, "uuid": uid(f"{new_name}|scene"), "hotkeys": hotkeys, "private_settings": {},
                  "filters": filters})
    scene["settings"] = {"custom_size": False, "id_counter": len(hotkeys) // 2, "items": items}
    new_scenes.append(scene)
    plan["order"], plan["captures"] = order, captures_here

# -------------------------------------------------------------------- the switch on each capture
report.append(f"\n'{SWITCH_NAME}' (Show in Scenes), added last on each capture")
switch_template = a_stagekit_filter(SHOW_IN_SCENES)
for name in CAPTURES:
    scenes = [p["name"] for p in PLANS if name in p["order"]]
    settings = {"sources|apick": "", "scenes|apick": "", "where": 1, "scenes": entries(scenes), "on_means": 0,
                "transition": MORPH, "transition_ms": CHANGE_MS, "follow": True, "go_to": GO_TO}
    by_name[name].setdefault("filters", []).append(
        make_filter(switch_template, SHOW_IN_SCENES, SWITCH_NAME, settings, name == opt.shown, f"{name}|filter|{SWITCH_NAME}"))
    report.append(f"  {name}: {'on' if name == opt.shown else 'off'}; this source, in {' and '.join(scenes)}; on means "
                  f"shown; Morph (StageKit), {CHANGE_MS} ms; follows what is shown (follow); pressed on, goes to "
                  f"{GO_TO} unless the scene on screen already shows it (go_to)")

data["sources"].extend(new_scenes)
at = scene_names.index(AFTER_SCENE) + 1
data["scene_order"][at:at] = [{"name": p["name"]} for p in PLANS]
data["name"] = data.get("name", "") + NAME_SUFFIX

# ------------------------------------------------------------------------------ fidelity table
fidelity, unexplained = [], 0
new_by_name = {s["name"]: s for s in new_scenes}
for plan in PLANS:
    new = new_by_name[plan["name"]]
    for scene_name, capture in plan["members"]:
        old_seen = [it for it in top_items(by_name[scene_name]) if it.get("visible")]
        new_seen = [it for it in top_items(new)
                    if (it["name"] == capture if it["name"] in CAPTURES else it.get("visible"))]
        old_at, new_at = {it["name"]: it for it in old_seen}, {it["name"]: it for it in new_seen}
        lines = []  # (explained, text)
        for it in old_seen:
            if it["name"] in new_at:
                continue
            if it["name"] == "Pureref" and plan is FULL and opt.pureref_full == "hidden":
                lines.append((True, "Pureref: shown here, hidden in the new scene [--pureref-full hidden]"))
            else:
                lines.append((False, f"{it['name']}: shown here, not shown in the new scene"))
        for it in new_seen:
            if it["name"] in old_at:
                continue
            if it["name"] == "Pureref" and plan is FULL and opt.pureref_full == "shown":
                lines.append((True, "Pureref: not shown here, shown in the new scene [--pureref-full shown]"))
            else:
                lines.append((False, f"{it['name']}: not shown here, shown in the new scene"))
        both_old = [it["name"] for it in old_seen if it["name"] in new_at]
        both_new = [it["name"] for it in new_seen if it["name"] in old_at]
        for i, a in enumerate(both_old):
            for b in both_old[i + 1:]:
                if both_new.index(a) > both_new.index(b):
                    widget = {a, b} == {WIDGET, CAMERA}
                    lines.append((widget, f"order: {a} is under {b} here, over it in the new scene"
                                  + (f" [--widget {opt.widget}]" if widget else "")))
        for name in both_old:
            diff = [f"{k} {say(old_at[name].get(k))} here, {say(new_at[name].get(k))} in the new scene"
                    for k in LOOK if old_at[name].get(k) != new_at[name].get(k)]
            if not diff:
                continue
            taken_from = copied_from[(plan["name"], name)]
            if name == "Alerts" and plan is FULL:
                lines.append((True, f"Alerts: {'; '.join(diff)} [--alerts-full {opt.alerts_full}]"))
            elif name not in CAPTURES and taken_from != scene_name:
                lines.append((True, f"{name}: {'; '.join(diff)} [one copy for every capture, taken from {taken_from}]"))
            else:
                lines.append((False, f"{name}: {'; '.join(diff)}"))
        # The Fit to Space filters, for the reader: they place the items once OBS runs.
        notes = []
        for old in fits_of(by_name[scene_name]):
            kept = [n for (s, n), o in origin.items() if s == plan["name"] and o == (scene_name, old["name"])]
            twin = [f for f in fits_of(new) if f["name"] in kept] or [
                f for f in fits_of(new) if f["settings"].get("item") == old["settings"].get("item")]
            if not twin:
                notes.append(f"Fit to Space '{old['name']}' has no counterpart in the new scene")
                continue
            a, b = old["settings"], twin[0]["settings"]
            keys = [k for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
            if old.get("enabled") != twin[0].get("enabled"):
                keys.append("on/off")
            name = f"'{old['name']}'" + (f" (now '{twin[0]['name']}')" if twin[0]["name"] != old["name"] else "")
            notes.append(f"Fit to Space {name}: " + ("same settings" if not keys else "the new scene's copy comes from "
                         f"{origin[(plan['name'], twin[0]['name'])][0]} and differs in " + ", ".join(
                             f"{k} ({say(a.get(k)) if not isinstance(a.get(k), list) else 'list'} here, "
                             f"{say(b.get(k)) if not isinstance(b.get(k), list) else 'list'} there)" for k in keys)))
        bad = [t for ok, t in lines if not ok]
        unexplained += len(bad)
        verdict = "same" if not lines else (f"{len(lines)} difference(s), all explained" if not bad else
                                            f"{len(bad)} UNEXPLAINED difference(s) of {len(lines)}")
        fidelity.append(f"  {scene_name}  ->  {plan['name']} with {capture} shown: {verdict}")
        fidelity.extend(f"      {'-' if ok else '!! UNEXPLAINED:'} {t}" for ok, t in lines)
        fidelity.extend(f"      . {t}" for t in notes)
        plan.setdefault("verdicts", []).append((scene_name, verdict))

# ---------------------------------------------------------------------------------- warnings
# Captures change into each other as a Morph only when the transition's links join them.
group_of = {}


def root(n):
    group_of.setdefault(n, n)
    while group_of[n] != n:
        n = group_of[n]
    return n


morphs = [t for t in data.get("transitions", []) if t.get("id") == MORPH]
for t in morphs:
    for e in t.get("settings", {}).get("links", []):
        ends = [p.strip() for p in str(e.get("value", "")).split("↔")]
        if len(ends) == 2 and all(ends):
            group_of[root(ends[0])] = root(ends[1])
if not morphs:
    warnings.append("The collection has no Morph (StageKit) transition, so no links: captures will fade and zoom, not morph.")
else:
    sets = {}
    for name in CAPTURES:
        sets.setdefault(root(name), []).append(name)
    if len(sets) > 1:
        warnings.append("Not every capture is joined to the others by the links of Morph (StageKit): "
                        + " | ".join(", ".join(v) for v in sets.values())
                        + ". A change between two that are not joined fades and zooms instead of morphing.")
    else:
        report.append("\nMorph links: all five captures are joined to each other through the links of Morph (StageKit).")
for name in CAPTURES:
    for f in by_name[name].get("filters", []):
        if f.get("id") == STANDBY and f.get("settings", {}).get("when_hidden"):
            warnings.append(f"'{name}' has the Standby Image filter '{f.get('name')}' with Show when source is hidden "
                            f"ticked: its image would stand in for the capture whenever another capture is shown.")
for s in data["sources"]:
    if s.get("id") != "scene":
        continue
    held = {it.get("name") for it in s["settings"].get("items", [])}
    for it in top_items(s):
        if by_name.get(it.get("name"), {}).get("id") == "group":
            held |= {g.get("name") for g in by_name[it["name"]].get("settings", {}).get("items", [])}
    for f in fits_of(s):
        if f["settings"].get("item") and f["settings"]["item"] not in held:
            warnings.append(f"{s['name']}: the Fit to Space '{f['name']}' names '{f['settings']['item']}', which is not "
                            f"in that scene. It does nothing. Left as it is.")

# --------------------------------------------------- check: nothing else in the collection changed
problems = []
for key in set(before) | set(data):
    if key not in ("sources", "scene_order", "name") and before.get(key) != data.get(key):
        problems.append(f"'{key}' changed")
old_sources = before.get("sources", [])
if data["sources"][len(old_sources):] != new_scenes or len(new_scenes) != len(PLANS):
    problems.append("the new scenes are not the only sources added")
for was, now in zip(old_sources, data["sources"]):
    if was.get("name") in CAPTURES:
        rest_was = {k: v for k, v in was.items() if k != "filters"}
        rest_now = {k: v for k, v in now.items() if k != "filters"}
        added = now.get("filters", [])[len(was.get("filters", [])):]
        if rest_was != rest_now or now.get("filters", [])[:-1] != was.get("filters", []) or len(added) != 1 \
                or added[0].get("name") != SWITCH_NAME:
            problems.append(f"the capture '{was.get('name')}' changed in more than its one new filter")
    elif was != now:
        problems.append(f"'{was.get('name')}' changed")
order_now = [s["name"] for s in data["scene_order"]]
if [n for n in order_now if n not in new_by_name] != scene_names or order_now[at:at + len(PLANS)] != [p["name"] for p in PLANS]:
    problems.append("the scene list is not the old one with the new scenes after " + AFTER_SCENE)
if data["name"] != before.get("name", "") + NAME_SUFFIX:
    problems.append("the collection's name is not the old one with the suffix")
for scene in new_scenes:
    ids = [it["id"] for it in top_items(scene)]
    if sorted(ids) != list(range(1, len(ids) + 1)) or scene["settings"]["id_counter"] != len(ids):
        problems.append(f"{scene['name']}: item ids and id counter do not match")
    if [it["name"] for it in top_items(scene) if it["name"] in CAPTURES and it["visible"]] != [opt.shown]:
        problems.append(f"{scene['name']}: not exactly '{opt.shown}' shown among the captures")
    for it in scene["settings"]["items"]:
        if not it.get("group_item_backup") and (it.get("show_transition", NO_TRANSITION) != NO_TRANSITION
                                                or it.get("hide_transition", NO_TRANSITION) != NO_TRANSITION):
            problems.append(f"{scene['name']}: '{it['name']}' carries a transition")
    names = [f["name"] for f in scene["filters"]]
    if len(names) != len(set(names)):
        problems.append(f"{scene['name']}: two filters share a name")

# ------------------------------------------------------------------------------------ write
ok = not problems and not unexplained
head = [
    f"Layout scenes, side-by-side trial: {before.get('name', '')}  ->  {data['name']}",
    f"Read: {SRC}",
    (f"Written: {OUT}" if ok else "NO COLLECTION WAS WRITTEN: see the problems below."),
    f"{len(scene_names)} scenes kept as they are, {len(PLANS)} added after '{AFTER_SCENE}': "
    + ", ".join(p["name"] for p in PLANS) + ".",
    "Checked by the script: every source, scene, group, transition and module entry that was in the collection is "
    "equal before and after, apart from one new last filter on each of the five captures: "
    + ("yes." if not problems else "NO: " + "; ".join(problems) + "."),
    "",
    "Options in force",
    f"  --alerts-full {opt.alerts_full}: Alerts in {FULL_NAME} is copied from "
    + ("Window-Capture-Scene (centred, as in the window and display scenes today)." if opt.alerts_full == "centred"
       else "Game-Capture-Scene (off the canvas, as in the two game scenes today).")
    + f" In {COMPONENT_NAME} it stays off the canvas, as in all three component scenes today.",
    f"  --widget {opt.widget}: Amuse Square Camera lies {opt.widget} Square Camera in {FULL_NAME}.",
    f"  --pureref-full {opt.pureref_full}: Pureref is {opt.pureref_full} in {FULL_NAME}.",
    f"  --displays-in-component {opt.displays_in_component}: Main Display and Tablet Display Capture are "
    + (f"in {COMPONENT_NAME} too." if opt.displays_in_component == "yes" else f"left out of {COMPONENT_NAME}."),
    f"  --shown \"{opt.shown}\": the capture shown in both new scenes; the others are hidden and locked.",
    "",
    f"Pressing a capture's Stream Deck key (Show in Scenes-Capture, On press: Switch on) goes to {GO_TO} as well, "
    f"unless the scene on screen already shows that capture: in {COMPONENT_NAME} the capture changes and the scene "
    "stays. The scene to go to is in the switch's own settings and can be changed or cleared there.",
]
tail = ["", "Choices between differing copies"] + [f"  {c}" for c in choices or ["none"]]
tail += ["", "Transitions left off the new items (the filters set them when OBS runs; a generated file with "
         "item transitions once crashed OBS on import)"] + [f"  {s}" for s in stripped or ["none"]]
tail += ["", "Not copied to the new scenes: the old scenes' private settings (transition overrides) and scene notes."]
tail += ["", "Fidelity: each of today's scenes against its new scene with only that capture shown",
         "  Compared: which items are shown, their order, and each one's place, scale, rotation, alignment, bounding "
         "box, crop and blending.",
         "  '-' is a difference an option or the choice of one copy explains; '.' is about a Fit to Space filter, for "
         "information."] + fidelity
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
for plan in PLANS:
    for scene_name, verdict in plan["verdicts"]:
        print(f"  {scene_name}: {verdict}")
print(f"{len(warnings)} warning(s), {len(choices)} choice(s). Read the report.")
