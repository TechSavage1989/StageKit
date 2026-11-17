#!/usr/bin/env python3
"""Adds one holding scene to an OBS scene collection that uses StageKit, for a side-by-side trial.

Today the scenes shown before, between and after the game (Stream Start, Be Right Back, Stream Ending)
are one design: the same background, start image, chat box, widget and alerts, with a different title
and a few extras. This writes a copy of the collection with one more scene that holds all three, one
shown at a time:

    Holding-Scene    in time replaces Stream Start, Be Right Back and Stream Ending

Intermission stays a scene of its own: it shows the camera, and the camera's switch (Show in Scenes on
every scene) would show it in every mode of a shared scene.

It reads one collection and writes another; the one it reads is never changed. Everything already in
the collection is kept exactly as it is, with two exceptions: each mode's title (a group) gets one more
filter at the end of its list ("Show in Scenes-Mode"), and the collection's name gets " + Holding"
added. The script checks that itself before it writes anything. It does not depend on the layout
scenes of merge-capture-scenes.py: it works on a collection with or without them.

What the new scene is made of:

- Items are copies of the items in today's three scenes, place and size included; nothing is placed by
  number here. The items all three share are copied once, from Stream Start, except Alerts, which is
  copied from Be Right Back (centred, as the owner asked). Each mode's own items come from its scene.
- Each mode is a title and its extras:
      Start    Stream Starting Group + Countdown Timer, Virtical Line, Elgato Virtual Cam
      BRB      BRB Text + Break Text
      Ending   Ending Text + Thanks Text, Discord
  Stream Start's mode is shown in the file; the other titles and extras are hidden. Nothing is locked.
- Stream Start's Fit to Space for the start image is copied over.
- "Link Visibility-Titles" shows only one title at a time; "Link Visibility-Start", "-BRB" and
  "-Ending" each show and hide a mode's extras with its title (Together). "Show in Scenes-Mode" on each
  title is that mode's switch, for the holding scene only, following what is shown. All change with a
  fade, 500 ms.
- No show or hide transition is written on any new item: the filters set them when OBS runs.

The report lists the scene item by item and filter by filter, what differs between today's three
scenes, every choice made, and a fidelity table: each of today's three scenes compared with the new
scene as it looks with that mode shown. A difference that the choice of one copy does not explain
stops the script: it writes the report, writes no collection and exits with an error.

Usage:
    python3 -I scripts/merge-holding-scenes.py <in.json> <out.json> <report.txt>
"""
import copy
import json
import os
import sys
import uuid

# ---------------------------------------------------------------------------------------------- plan
NEW_SCENE = "Holding-Scene"
AFTER_SCENE = "Stream Start"  # the new scene goes straight after this one in the scene list
NAME_SUFFIX = " + Holding"
SWITCH_NAME = "Show in Scenes-Mode"
TITLES_LINK = "Link Visibility-Titles"
TRANSITION = "fade_transition"
CHANGE_MS = 500
FIT = "stagekit_fit_item"
SHOW_IN_SCENES = "stagekit_show_in_scenes"
LINK = "stagekit_visibility_link"
ONLY_ONE, TOGETHER = 0, 1  # LinkMode in src/placement/visibility-link.cpp
NO_TRANSITION = {"duration": 0}  # how OBS writes an item with no transition
LOOK = ["pos", "scale", "rot", "align", "bounds_type", "bounds_align", "bounds", "bounds_crop", "crop_left",
        "crop_top", "crop_right", "crop_bottom", "scale_filter", "blend_method", "blend_type"]

# (mode, its scene today, its title, its extras). The first is the one shown in the file.
MODES = [
    ("Start", "Stream Start", "Stream Starting Group", ["Countdown Timer", "Virtical Line", "Elgato Virtual Cam"]),
    ("BRB", "Be Right Back", "BRB Text", ["Break Text"]),
    ("Ending", "Stream Ending", "Ending Text", ["Thanks Text", "Discord"]),
]
# Items the three scenes share, and the scene each is copied from.
COMMON = {"Ambient Background": "Stream Start", "Start Image": "Stream Start", "Start Image Secondary": "Stream Start",
          "Chat Box Group": "Stream Start", "Amuse Component": "Stream Start", "Alerts": "Be Right Back"}
FIT_FROM = ("Stream Start", "Fit Item - Start Image")
# Bottom to top as wanted. Only decides where today's scenes say nothing or disagree.
PLAN_ORDER = ["Ambient Background", "Break Text", "BRB Text", "Thanks Text", "Ending Text", "Start Image",
              "Elgato Virtual Cam", "Start Image Secondary", "Chat Box Group", "Stream Starting Group",
              "Countdown Timer", "Virtical Line", "Amuse Component", "Discord", "Alerts"]
SCENES = [m[1] for m in MODES]
TITLES = [m[2] for m in MODES]
MODE_ITEMS = {m[0]: [m[2]] + m[3] for m in MODES}
FROM = dict(COMMON)
for _mode, _scene, _title, _extras in MODES:
    for _n in [_title] + _extras:
        FROM[_n] = _scene

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
scene_names = [s.get("name") for s in data.get("scene_order", [])]


def top_items(scene):
    """A scene's own items, bottom to top. OBS also writes a group's items into the scene as a backup
    it does not load; those are not items of the scene."""
    return [it for it in scene["settings"]["items"] if not it.get("group_item_backup")]


def item_of(scene_name, name):
    found = [it for it in top_items(by_name[scene_name]) if it.get("name") == name]
    return found[0] if len(found) == 1 else None


# ------------------------------------------------------------------------- is everything there?
missing = []
if set(PLAN_ORDER) != set(FROM) or len(PLAN_ORDER) != len(FROM):
    missing.append("the plan's order does not name every planned item once (a fault in the script)")
if NEW_SCENE in by_name:
    missing.append(f"there is already something called '{NEW_SCENE}': this collection was merged before")
if AFTER_SCENE not in scene_names:
    missing.append(f"the scene '{AFTER_SCENE}' is not in the scene list")
for scene_name in SCENES:
    if scene_name not in by_name or by_name[scene_name].get("id") != "scene":
        missing.append(f"the scene '{scene_name}' is not in the collection")
for name, scene_name in FROM.items():
    if name not in by_name:
        missing.append(f"the source '{name}' is not in the collection")
    elif scene_name in by_name and by_name[scene_name].get("id") == "scene" and item_of(scene_name, name) is None:
        missing.append(f"'{scene_name}' does not hold '{name}' exactly once")
for title in TITLES:
    if title in by_name:
        if by_name[title].get("id") != "group" or by_name[title] not in data.get("groups", []):
            missing.append(f"the title '{title}' is not a group in the collection's groups")
        elif any(f.get("name") == SWITCH_NAME for f in by_name[title].get("filters", [])):
            missing.append(f"'{title}' already has a filter called '{SWITCH_NAME}': this collection was merged before")
if FIT_FROM[0] in by_name and not [f for f in by_name[FIT_FROM[0]].get("filters", []) if f.get("name") == FIT_FROM[1]
                                   and f.get("id") == FIT]:
    missing.append(f"'{FIT_FROM[0]}' has no Fit to Space called '{FIT_FROM[1]}'")
if missing:
    sys.exit("Nothing was written. The plan cannot be carried out on this collection:\n  "
             + "\n  ".join(dict.fromkeys(missing)))

# ------------------------------------------------------------------------------------- helpers
report, choices, warnings, stripped, differ = [], [], [], [], []
taken = {s.get("uuid") for s in everything}
taken |= {f.get("uuid") for s in everything for f in s.get("filters", [])}
NAMESPACE = uuid.uuid5(uuid.NAMESPACE_DNS, "merge-holding-scenes.stagekit")


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


def make_filter(like, filter_id, name, settings, enabled, what):
    """A filter entry with every field OBS writes, taken from a StageKit filter already in the file."""
    made = copy.deepcopy(like)
    made.update({"name": name, "uuid": uid(what), "id": filter_id, "versioned_id": filter_id, "settings": settings,
                 "enabled": enabled, "hotkeys": {}, "private_settings": {}})
    return made


def a_stagekit_filter(kind):
    for s in everything:
        for f in s.get("filters", []):
            if f.get("id") == kind:
                return f
    for s in everything:
        for f in s.get("filters", []):
            if str(f.get("id", "")).startswith("stagekit_"):
                return f
    sys.exit("Nothing was written: the collection has no StageKit filter to take a filter's fields from.")


def mode_of(name):
    for mode, items in MODE_ITEMS.items():
        if name in items:
            return mode
    return None


# ------------------------------------------------------ the split, and what differs between today's scenes
report.append(f"\nWhat today's three scenes hold ({', '.join(SCENES)})")
for scene_name in SCENES:
    for it in top_items(by_name[scene_name]):
        if it["name"] not in FROM:
            warnings.append(f"{scene_name} holds '{it['name']}', which the plan does not carry into {NEW_SCENE}.")
for name in PLAN_ORDER:
    states = []
    for scene_name in SCENES:
        it = item_of(scene_name, name)
        states.append("-" if it is None else ("shown" if it.get("visible") else "hidden"))
    mode = mode_of(name)
    report.append(f"  {name:<24} " + "  ".join(f"{s:<7}" for s in states)
                  + (f"  -> mode {mode}" + (" (title)" if name in TITLES else "") if mode else "  -> shared"))
    if mode:
        own = SCENES[[m[0] for m in MODES].index(mode)]
        for scene_name, state in zip(SCENES, states):
            if scene_name == own and state != "shown":
                missing.append(f"'{name}' is planned for mode {mode} but is not shown in {own}")
            if scene_name != own and state == "shown":
                missing.append(f"'{name}' is planned for mode {mode} only but is shown in {scene_name} too")
    elif states != ["shown"] * 3 and states != ["hidden"] * 3:
        missing.append(f"'{name}' is planned as shared but is not the same (shown or hidden) in all three scenes")
report.append("  The split is confirmed: each mode's items are shown in its own scene only, and the shared ones "
              "are the same in all three." if not missing else "  THE SPLIT DOES NOT HOLD: see the problems.")
# Shared items whose place differs between the three scenes.
for name in COMMON:
    looks = {s: item_of(s, name) for s in SCENES}
    for k in LOOK:
        vals = {s: looks[s].get(k) for s in SCENES}
        if len({json.dumps(v, sort_keys=True) for v in vals.values()}) > 1:
            differ.append(f"'{name}' {k}: " + "; ".join(f"{say(v)} in {s}" for s, v in vals.items())
                          + f". Copied from {COMMON[name]}.")
for scene_name in SCENES:
    ps = by_name[scene_name].get("private_settings") or {}
    if ps:
        differ.append(f"{scene_name} has private settings (a transition override: "
                      + ", ".join(f"{k} {v}" for k, v in sorted(ps.items())) + "); the new scene has none.")
    others = [f"'{f.get('name')}' ({f.get('id')})" for f in by_name[scene_name].get("filters", [])
              if (scene_name, f.get("name")) != FIT_FROM and f.get("id") != FIT]
    if others:
        warnings.append(f"{scene_name} has the filter(s) {', '.join(others)}, not carried into {NEW_SCENE}.")
fits = {s: [f for f in by_name[s].get("filters", []) if f.get("id") == FIT] for s in SCENES}
fit_src = [f for f in fits[FIT_FROM[0]] if f.get("name") == FIT_FROM[1]][0]
for s in SCENES:
    if s == FIT_FROM[0]:
        continue
    for f in fits[s]:
        a, b = fit_src["settings"], f["settings"]
        keys = [k for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
        if f.get("enabled") != fit_src.get("enabled"):
            keys.append("on/off")
        differ.append(f"Fit to Space '{f['name']}' of {s} against {FIT_FROM[0]}'s: "
                      + ("same settings" if not keys else "differs in " + ", ".join(
                          f"{k} ({say(a.get(k)) if not isinstance(a.get(k), list) else 'list'} in {FIT_FROM[0]}, "
                          f"{say(b.get(k)) if not isinstance(b.get(k), list) else 'list'} in {s})" for k in keys))
                      + f". Only {FIT_FROM[0]}'s is copied.")
if missing:
    sys.exit("Nothing was written. The plan does not match the collection:\n  " + "\n  ".join(dict.fromkeys(missing)))


# ------------------------------------------------------------------------------- stacking order
def merged_order():
    """The new scene's items bottom to top: what today's scenes agree on, then the plan's order."""
    place = {n: i for i, n in enumerate(PLAN_ORDER)}
    votes = {}  # (lower, upper) -> the scenes that have them that way round
    for scene_name in SCENES:
        there = [it["name"] for it in top_items(by_name[scene_name]) if it["name"] in place]
        for i, low in enumerate(there):
            for high in there[i + 1:]:
                votes.setdefault((low, high), []).append(scene_name)
    below = {n: set() for n in PLAN_ORDER}
    unsaid = []
    for i, a in enumerate(PLAN_ORDER):
        for b in PLAN_ORDER[i + 1:]:
            ab, ba = votes.get((a, b), []), votes.get((b, a), [])
            if ab and ba:
                choices.append(f"'{a}' is under '{b}' in {', '.join(ab)} and over it in {', '.join(ba)}. "
                               f"Chosen: '{a}' under '{b}' (the plan's order).")
                below[b].add(a)
            elif ab:
                below[b].add(a)
            elif ba:
                below[a].add(b)
            else:
                unsaid.append((a, b))
    order, left = [], list(PLAN_ORDER)
    while left:
        ready = [n for n in left if not (below[n] & set(left))]
        if not ready:
            choices.append(f"today's scenes give no single order for {', '.join(left)}; the plan's order was used.")
            ready = left
        order.append(ready[0])
        left.remove(ready[0])
    if order != PLAN_ORDER:
        choices.append(f"today's scenes put the items in another order than the plan names. Used (bottom to top): "
                       f"{'; '.join(order)}.")
    else:
        report.append("  Stacking order: every pair that today's scenes hold together is in the order they have it.")
    if unsaid:
        report.append("  No scene today holds these together, so the plan's order decided (lower first): "
                      + "; ".join(f"{a} / {b}" for a, b in unsaid) + ".")
    return order


# ---------------------------------------------------------------------------------- the scene
report.append(f"\n{NEW_SCENE}")
order = merged_order()
shown_mode = MODES[0][0]
items, hotkeys, copied_from, unlocked = [], {"OBSBasic.SelectScene": []}, {}, []
for name in order:
    from_scene = FROM[name]
    old_items = by_name[from_scene]["settings"]["items"]
    old = item_of(from_scene, name)
    at = old_items.index(old)
    # A group's backup entries lie straight under the group's own item; they go with it, as they are.
    first = at
    while first > 0 and old_items[first - 1].get("group_item_backup") and by_name[name].get("id") == "group":
        first -= 1
    items.extend(copy.deepcopy(old_items[first:at]))
    item = copy.deepcopy(old)
    item["id"] = len(hotkeys) // 2 + 1
    if item.get("locked"):
        unlocked.append(f"'{name}' (locked in {from_scene})")
    item["locked"] = False
    mode = mode_of(name)
    if mode:
        item["visible"] = mode == shown_mode
    for key in ("show_transition", "hide_transition"):
        if key in item and item[key] != NO_TRANSITION:
            stripped.append(f"'{name}' ({item[key].get('id', '?')} as its {key.replace('_', ' ')} in {from_scene})")
        if key in item:
            item[key] = dict(NO_TRANSITION)
    for key in ("show_transition_duration", "hide_transition_duration"):
        item.pop(key, None)
    items.append(item)
    hotkeys[f"libobs.show_scene_item.{item['id']}"] = []
    hotkeys[f"libobs.hide_scene_item.{item['id']}"] = []
    copied_from[name] = from_scene
    box = f"box {say(item['bounds'])}" if item.get("bounds_type") else f"scale {say(item['scale'])}"
    role = (f"mode {mode}" + (" title" if name in TITLES else " extra")) if mode else "shared"
    report.append(f"  {item['id']:>2}. {name}: {'shown' if item['visible'] else 'hidden'}; {role}; at "
                  f"{say(item['pos'])}, {box}; copied from {from_scene}")
if unlocked:
    choices.append("Unlocked in the new scene (nothing is locked there): " + ", ".join(unlocked) + ".")

# Filters: the start image's Fit to Space, then the links.
fit_template = a_stagekit_filter(FIT)
filters = [make_filter(fit_src, FIT, fit_src["name"], copy.deepcopy(fit_src["settings"]), fit_src.get("enabled", True),
                       f"{NEW_SCENE}|filter|{fit_src['name']}")]
s = fit_src["settings"]
area = {0: "The whole screen", 1: "A box set in the filter", 2: "Free space around source"}.get(s.get("box", 0), "?")
report.append(f"  filter '{fit_src['name']}' (Fit to Space, {'on' if fit_src.get('enabled', True) else 'off'}): "
              f"{s.get('item')}; {area}; gap {num(s.get('margin', 30.0))}; ignores "
              f"{', '.join(e.get('value', '') for e in s.get('skip', [])) or 'nothing'}; copied from {FIT_FROM[0]}")
links = [(TITLES_LINK, ONLY_ONE, TITLES)] + [(f"Link Visibility-{m[0]}", TOGETHER, MODE_ITEMS[m[0]]) for m in MODES]
for link_name, mode, names in links:
    settings = {"mode": mode, "items": entries(names), "items|apick": "", "transition": TRANSITION,
                "transition_ms": CHANGE_MS, "lock_hidden": False}
    filters.append(make_filter(fit_template, LINK, link_name, settings, True, f"{NEW_SCENE}|filter|{link_name}"))
    report.append(f"  filter '{link_name}' (Link Visibility, on): "
                  f"{'Show only one at a time' if mode == ONLY_ONE else 'Together'}; {', '.join(names)}; "
                  f"Fade, {CHANGE_MS} ms; hidden ones not locked")

template_scene = by_name[SCENES[0]]
scene = copy.deepcopy(template_scene)
scene.update({"name": NEW_SCENE, "uuid": uid(f"{NEW_SCENE}|scene"), "hotkeys": hotkeys, "private_settings": {},
              "filters": filters})
scene["settings"] = copy.deepcopy(template_scene["settings"])
scene["settings"].update({"id_counter": len(hotkeys) // 2, "items": items})

# -------------------------------------------------------------------- the switch on each title
report.append(f"\n'{SWITCH_NAME}' (Show in Scenes), added last on each mode's title (a group)")
switch_template = a_stagekit_filter(SHOW_IN_SCENES)
for mode, scene_name, title, extras in MODES:
    settings = {"sources|apick": "", "scenes|apick": "", "where": 1, "scenes": entries([NEW_SCENE]), "on_means": 0,
                "transition": TRANSITION, "transition_ms": CHANGE_MS, "follow": True}
    on = mode == shown_mode
    by_name[title].setdefault("filters", []).append(
        make_filter(switch_template, SHOW_IN_SCENES, SWITCH_NAME, settings, on, f"{title}|filter|{SWITCH_NAME}"))
    report.append(f"  {title} (mode {mode}): {'on' if on else 'off'}; this source, in {NEW_SCENE} only; on means "
                  f"shown; Fade, {CHANGE_MS} ms; follows what is shown (follow)")

data["sources"].append(scene)
at = scene_names.index(AFTER_SCENE) + 1
data["scene_order"][at:at] = [{"name": NEW_SCENE}]
data["name"] = data.get("name", "") + NAME_SUFFIX

# ------------------------------------------------------------------------------ fidelity table
fidelity, unexplained, verdicts = [], 0, []
for mode, scene_name, title, extras in MODES:
    old_seen = [it for it in top_items(by_name[scene_name]) if it.get("visible")]
    new_seen = [it for it in top_items(scene)
                if (mode_of(it["name"]) == mode if mode_of(it["name"]) else it.get("visible"))]
    old_at, new_at = {it["name"]: it for it in old_seen}, {it["name"]: it for it in new_seen}
    lines = []  # (explained, text)
    for it in old_seen:
        if it["name"] not in new_at:
            lines.append((False, f"{it['name']}: shown here, not shown in the new scene"))
    for it in new_seen:
        if it["name"] not in old_at:
            lines.append((False, f"{it['name']}: not shown here, shown in the new scene"))
    both_old = [it["name"] for it in old_seen if it["name"] in new_at]
    both_new = [it["name"] for it in new_seen if it["name"] in old_at]
    for i, a in enumerate(both_old):
        for b in both_old[i + 1:]:
            if both_new.index(a) > both_new.index(b):
                lines.append((False, f"order: {a} is under {b} here, over it in the new scene"))
    for name in both_old:
        diff = [f"{k} {say(old_at[name].get(k))} here, {say(new_at[name].get(k))} in the new scene"
                for k in LOOK if old_at[name].get(k) != new_at[name].get(k)]
        if not diff:
            continue
        taken_from = copied_from[name]
        if name == "Alerts":
            lines.append((True, f"Alerts: {'; '.join(diff)} [centred, as the owner asked: copied from {taken_from}]"))
        elif name in COMMON and taken_from != scene_name:
            lines.append((True, f"{name}: {'; '.join(diff)} [one copy for every mode, taken from {taken_from}]"))
        else:
            lines.append((False, f"{name}: {'; '.join(diff)}"))
    notes = []
    for old in [f for f in by_name[scene_name].get("filters", []) if f.get("id") == FIT]:
        a, b = old["settings"], filters[0]["settings"]
        keys = [k for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
        notes.append(f"Fit to Space '{old['name']}': " + ("same settings" if not keys else
                     f"the new scene's copy comes from {FIT_FROM[0]} and differs in {', '.join(keys)}"))
    bad = [t for ok, t in lines if not ok]
    unexplained += len(bad)
    verdict = "same" if not lines else (f"{len(lines)} difference(s), all explained" if not bad else
                                        f"{len(bad)} UNEXPLAINED difference(s) of {len(lines)}")
    fidelity.append(f"  {scene_name}  ->  {NEW_SCENE} with mode {mode} shown: {verdict}")
    fidelity.extend(f"      {'-' if ok else '!! UNEXPLAINED:'} {t}" for ok, t in lines)
    fidelity.extend(f"      . {t}" for t in notes)
    fidelity.append(f"      shown, bottom to top: {', '.join(both_new) or 'nothing'}")
    verdicts.append((scene_name, verdict))

# ---------------------------------------------------------------------------------- warnings
for s in everything:
    for f in s.get("filters", []):
        st = f.get("settings", {})
        if f.get("id") == SHOW_IN_SCENES and s.get("name") not in TITLES and st.get("where", 0) == 0:
            listed = [e.get("value") for e in st.get("sources", [])] or [s.get("name")]
            here = [n for n in listed if n in FROM]
            if here:
                warnings.append(f"'{f.get('name')}' on '{s.get('name')}' acts in every scene, so it shows and hides "
                                f"{', '.join(here)} in {NEW_SCENE} too, as it does in today's three scenes.")
        if f.get("id") == LINK and s.get("name") != NEW_SCENE:
            elsewhere = [e.get("value") for e in st.get("elsewhere", [])]
            here = [n for n in elsewhere if n in FROM]
            if here:
                warnings.append(f"The Link Visibility '{f.get('name')}' of {s.get('name')} also acts on "
                                f"{', '.join(here)} in other scenes, {NEW_SCENE} included.")

# --------------------------------------------------- check: nothing else in the collection changed
problems = []
for key in set(before) | set(data):
    if key not in ("sources", "groups", "scene_order", "name") and before.get(key) != data.get(key):
        problems.append(f"'{key}' changed")
old_sources = before.get("sources", [])
if data["sources"][:len(old_sources)] != old_sources or data["sources"][len(old_sources):] != [scene]:
    problems.append("the new scene is not the only change to the sources")
old_groups = before.get("groups", [])
if len(old_groups) != len(data.get("groups", [])):
    problems.append("the number of groups changed")
for was, now in zip(old_groups, data.get("groups", [])):
    if was.get("name") in TITLES:
        rest_was = {k: v for k, v in was.items() if k != "filters"}
        rest_now = {k: v for k, v in now.items() if k != "filters"}
        added = now.get("filters", [])[len(was.get("filters", [])):]
        if rest_was != rest_now or now.get("filters", [])[:-1] != was.get("filters", []) or len(added) != 1 \
                or added[0].get("name") != SWITCH_NAME:
            problems.append(f"the title '{was.get('name')}' changed in more than its one new filter")
    elif was != now:
        problems.append(f"the group '{was.get('name')}' changed")
order_now = [s["name"] for s in data["scene_order"]]
if [n for n in order_now if n != NEW_SCENE] != scene_names or order_now[at:at + 1] != [NEW_SCENE]:
    problems.append("the scene list is not the old one with the new scene after " + AFTER_SCENE)
if data["name"] != before.get("name", "") + NAME_SUFFIX:
    problems.append("the collection's name is not the old one with the suffix")
ids = [it["id"] for it in top_items(scene)]
if sorted(ids) != list(range(1, len(ids) + 1)) or scene["settings"]["id_counter"] != len(ids):
    problems.append("item ids and id counter do not match")
if sorted(hotkeys) != sorted(["OBSBasic.SelectScene"] + [f"libobs.{w}_scene_item.{i}" for i in ids
                                                          for w in ("show", "hide")]):
    problems.append("the scene's hotkeys do not match its items")
if [it["name"] for it in top_items(scene)] != order or len(set(order)) != len(order):
    problems.append("the scene's items are not the planned ones, once each")
for it in top_items(scene):
    if it.get("source_uuid") != by_name[it["name"]].get("uuid"):
        problems.append(f"'{it['name']}' does not point at its source")
    if it.get("locked"):
        problems.append(f"'{it['name']}' is locked")
    if it.get("show_transition", NO_TRANSITION) != NO_TRANSITION or it.get("hide_transition", NO_TRANSITION) != NO_TRANSITION:
        problems.append(f"'{it['name']}' carries a transition")
if [it["name"] for it in top_items(scene) if it["name"] in TITLES and it["visible"]] != [MODES[0][2]]:
    problems.append(f"not exactly '{MODES[0][2]}' shown among the titles")
names = [f["name"] for f in scene["filters"]]
if len(names) != len(set(names)):
    problems.append("two filters on the new scene share a name")
uuids = [s.get("uuid") for s in data["sources"] + data.get("groups", [])]
uuids += [f.get("uuid") for s in data["sources"] + data.get("groups", []) for f in s.get("filters", [])]
if len(uuids) != len(set(uuids)):
    problems.append("two things share a uuid")

# ------------------------------------------------------------------------------------ write
ok = not problems and not unexplained
head = [
    f"Holding scene, side-by-side trial: {before.get('name', '')}  ->  {data['name']}",
    f"Read: {SRC}",
    (f"Written: {OUT}" if ok else "NO COLLECTION WAS WRITTEN: see the problems below."),
    f"{len(scene_names)} scenes kept as they are, {NEW_SCENE} added after '{AFTER_SCENE}'.",
    "Checked by the script: every source, scene, group, transition and module entry that was in the collection is "
    "equal before and after, apart from one new last filter on each of the three titles: "
    + ("yes." if not problems else "NO: " + "; ".join(problems) + "."),
    "",
    "Modes (title + extras; the first is shown in the file)",
] + [f"  {m}: {t} + {', '.join(e)}  (today: {s})" for m, s, t, e in MODES] + [
    "  Intermission stays a scene of its own: it shows the camera, and the camera's switch (Show in Scenes on "
    "every scene) would show the camera in every mode of a shared scene.",
]
tail = ["", "What differs between today's three scenes"] + [f"  {d}" for d in differ or ["nothing"]]
tail += ["", "Choices between differing copies"] + [f"  {c}" for c in choices or ["none"]]
tail += ["", "Transitions left off the new items (the filters set them when OBS runs; a generated file with "
         "item transitions once crashed OBS on import)"] + [f"  {s}" for s in stripped or ["none"]]
tail += ["", "Not copied to the new scene: the old scenes' private settings (Be Right Back's transition override).",
         "Be Right Back also has a Transition Table rule (any scene to it); Holding-Scene has none, so going to it uses",
         "OBS's current transition. Add a rule for Holding-Scene in the Transition Table if you want one.",
         "The scene's notes are copied from Stream Start (the same in all three).",
         "Set each mode's Stream Deck key to On press: Switch on. A Toggle key pressed on the mode showing hides it,",
         "and no title is left showing.",
         "Showing a mode's extra with the eye shows that whole mode."]
tail += ["", "Fidelity: each of today's scenes against the new scene with only that mode shown",
         "  Compared: which items are shown, their order, and each one's place, scale, rotation, alignment, bounding "
         "box, crop and blending.",
         "  '-' is a difference the choice of one copy explains; '.' is about a Fit to Space filter, for "
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
for scene_name, verdict in verdicts:
    print(f"  {scene_name}: {verdict}")
print(f"{len(warnings)} warning(s), {len(choices)} choice(s). Read the report.")
