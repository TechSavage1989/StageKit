#!/usr/bin/env python3
"""Writes a synthetic scene collection for tests/collection-perf: the shape of a large StageKit setup.

Usage: python3 -I tests/make-perf-collection.py <out.json> [--scale S]

--scale draws the same collection on a canvas S times the size (0.25: 640x360), every place and size with it:
headless, llvmpipe draws a 2560x1440 frame in a few tenths of a second, and every filter's timers then come round
at every tick, which a real graphics card's 60 frames a second never do.

Nothing in it is anyone's: the names are made up and every capture, camera and browser is a colour source
(or a text) of the size it would have. What it mirrors is the number and kind of StageKit filters a big
collection carries: 18 scenes at 2560x1440, of which three are layout scenes (Live, Chat Live, Standby)
and fifteen per-game scenes; about 36 Fit to Space (7 or 8 in a layout scene), 10 Link Visibility
(one per layout scene over the captures with *Lock hidden sources*, the rest *Follow the first source*
over the holding modes), 14 Show in Scenes (five on the captures with *Follow what is shown*), 23 Shadow,
Glow & Outline, 4 Standby Image, an Auto Frame, an Auto Capture, three window switchers, an Ambient Background, groups, disabled Move Source and
Blur filters, and Morph as the scene transition.
"""
import json
import sys

SCALE = float(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1.0
W, H = round(2560 * SCALE), round(1440 * SCALE)
CAPTURES = ["Capture Game", "Capture Card", "Capture Window", "Capture Display", "Capture Tablet"]
sources = []
groups = []
_uid = [0]


def uuid():
    _uid[0] += 1
    return f"00000000-0000-4000-8000-{_uid[0]:012d}"


def colour(name, w, h, rgb):
    return {"id": "color_source", "versioned_id": "color_source_v3", "name": name, "uuid": uuid(),
            "settings": {"color": 0xFF000000 | rgb, "width": max(2, round(w * SCALE)),
                         "height": max(2, round(h * SCALE))}, "filters": []}


def text(name, words):
    return {"id": "text_ft2_source", "versioned_id": "text_ft2_source_v2", "name": name, "uuid": uuid(),
            "settings": {"text": words, "font": {"face": "Sans", "size": max(8, round(72 * SCALE)), "style": "Regular", "flags": 0}},
            "filters": []}


def filt(kind, name, settings=None, enabled=True):
    return {"id": kind, "versioned_id": kind, "name": name, "enabled": enabled, "settings": settings or {}}


def picks(names):
    return [{"value": n, "selected": False, "hidden": False} for n in names]


def item(name, x, y, w=None, h=None, visible=True, iid=0):
    it = {"name": name, "id": iid, "visible": visible, "locked": False, "rot": 0.0,
          "pos": {"x": x * SCALE, "y": y * SCALE}, "scale": {"x": 1.0, "y": 1.0}, "align": 5,
          "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
          "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0}
    if w and h:
        it["bounds_type"] = 2  # scale to inner bounds
        it["bounds"] = {"x": w * SCALE, "y": h * SCALE}
    return it


def scene(name, items, filters=(), group=False):
    for i, it in enumerate(items):
        it["id"] = i + 1
    s = {"id": "group" if group else "scene", "versioned_id": "group" if group else "scene", "name": name,
         "uuid": uuid(), "settings": {"items": items, "id_counter": len(items), "custom_size": group},
         "filters": list(filters)}
    if group:
        s["settings"]["cx"] = round(1200 * SCALE)
        s["settings"]["cy"] = round(400 * SCALE)
        groups.append(s)
    else:
        sources.append(s)
    return s


def fit(source, i, **extra):
    settings = {"item": source}
    settings.update(extra)
    return filt("stagekit_fit_item", f"Fit {source} {i}", settings)


def shadow(src, screen_px=False):
    src["filters"].append(filt("stagekit_shadow_backdrop", "Shadow (StageKit)",
                               {"screen_px": screen_px} if screen_px else {}))


# ------------------------------------------------------------------ sources
by_name = {}


def add(src):
    sources.append(src)
    by_name[src["name"]] = src
    return src


for n, cap in enumerate(CAPTURES):
    add(colour(cap, 1920, 1080, 0x203040 + n * 0x101808))
add(colour("Camera", 1920, 1080, 0x406080))
add(colour("Camera Square", 1080, 1080, 0x406090))
add(colour("Chat Box", 500, 1000, 0x302020))
add(colour("Music Widget", 500, 160, 0x203020))
add(colour("Alerts", 800, 300, 0x402040))
add(colour("Frame Panel", 2560, 1440, 0x101010))
add(colour("Console Feed", 1440, 1080, 0x504030))
for t in ("Stream Start", "Be Right Back", "Intermission", "Stream Ending", "Now Playing", "Socials", "Clock",
          "Ticker"):
    add(text(f"Text {t}", t))
add({"id": "stagekit_ambient_background", "versioned_id": "stagekit_ambient_background",
     "name": "Ambient Background", "uuid": uuid(),
     "settings": {"live_mode": 1, "blur": 60, "zoom": 120}, "filters": []})
add({"id": "stagekit_start_image", "versioned_id": "stagekit_start_image", "name": "Start Image",
     "uuid": uuid(), "settings": {}, "filters": []})

# Filters on sources: shadows on 23, Standby on four captures, an Auto Frame,
# Show in Scenes on the captures (follow) and the titles, disabled Move Source
# and Blur filters as a collection keeps them for keys.
shadowed = CAPTURES + ["Camera", "Camera Square", "Chat Box", "Music Widget", "Alerts", "Console Feed"] + \
    [f"Text {t}" for t in ("Stream Start", "Be Right Back", "Intermission", "Stream Ending", "Now Playing",
                           "Socials", "Clock", "Ticker")]
for i, name in enumerate(shadowed):
    shadow(by_name[name], screen_px=i in (5, 6, 7))
by_name["Capture Game"]["filters"].insert(0, filt("stagekit_auto_frame", "Auto Frame (StageKit)"))
by_name["Capture Game"]["filters"].insert(0, filt("stagekit_auto_capture", "Auto Capture (StageKit)"))
for cap in ("Capture Window", "Capture Display", "Capture Tablet"):
    by_name[cap]["filters"].insert(0, filt("stagekit_app_capture", "Automatic Window Switcher (StageKit)"))
for cap in CAPTURES[:4]:
    by_name[cap]["filters"].insert(1 if cap == "Capture Game" else 0,
                                   filt("stagekit_standby", "Standby Image (StageKit)"))
for cap in CAPTURES:
    by_name[cap]["filters"].append(filt("stagekit_show_in_scenes", "Show in Scenes (StageKit)", {
        "where": 1, "scenes": picks(["Live", "Chat Live"]), "follow": True}))
for t in ("Stream Start", "Be Right Back", "Intermission", "Stream Ending"):
    by_name[f"Text {t}"]["filters"].append(filt("stagekit_show_in_scenes", "Show in Scenes (StageKit)", {
        "where": 1, "scenes": picks(["Standby"])}))
for i, name in enumerate(["Camera", "Chat Box", "Alerts", "Music Widget", "Console Feed"]):
    by_name[name]["filters"].append(filt("stagekit_show_in_scenes", "Show in Scenes off (StageKit)", {}, False))
for i in range(21):
    target = ["Camera", "Camera Square", "Chat Box"][i % 3]
    by_name[target]["filters"].append(filt("stagekit_send_to", f"Move Source {i} (StageKit)", {}, False))
for i in range(10):
    target = CAPTURES[i % 5]
    by_name[target]["filters"].append(filt("stagekit_ramp_blur", f"Blur {i} (StageKit)", {}, False))

# ------------------------------------------------------------------ groups (titles of the holding modes)
for t in ("Stream Start", "Be Right Back", "Intermission", "Stream Ending"):
    shadow(scene(f"Title {t}", [item(f"Text {t}", 0, 0), item("Text Socials", 0, 200)], group=True))

# ------------------------------------------------------------------ scenes
# Fifteen per-game scenes, as a collection keeps them from before its layout
# scenes: each a capture, the camera, the chat box and a few overlays, with
# up to four Fit to Space (22 in all).
fits_per_old = [1, 1, 3, 0, 1, 3, 2, 3, 0, 0, 4, 0, 2, 1, 1]
for n, count in enumerate(fits_per_old):
    cap = CAPTURES[n % 5]
    items = [item("Ambient Background", 0, 0, 2560, 1440), item(cap, 30, 85, 1755, 1269),
             item("Camera", 1820, 85, 700, 394), item("Chat Box", 1820, 520, 700, 880),
             item("Text Now Playing", 40, 1380)]
    if n % 2:
        items.append(item("Music Widget", 1820, 1250))
    if n % 3 == 0:
        items.append(item("Alerts", 900, 40))
    names = [cap, "Camera", "Chat Box", "Music Widget"]
    fl = [fit(names[i % 4], i, resize=(i % 2 == 0)) for i in range(count)]
    scene(f"Game Scene {n + 1}", items, fl)

# Live: every capture stacked (one shown), the camera, the overlays.
live_items = [item("Ambient Background", 0, 0, 2560, 1440)] + \
    [item(c, 30, 85, 1755, 1269, visible=(i == 0)) for i, c in enumerate(CAPTURES)] + \
    [item("Camera", 1820, 85, 700, 394), item("Alerts", 900, 40), item("Text Clock", 2300, 1380),
     item("Text Ticker", 40, 1380), item("Frame Panel", 0, 0, 2560, 1440)]
live_items[-1]["visible"] = False
scene("Live", live_items,
      [fit(c, i, resize=True) for i, c in enumerate(CAPTURES[:3])] + [fit("Camera", 9, resize=False)] +
      [filt("stagekit_visibility_link", "Link Visibility (StageKit)",
            {"items": picks(CAPTURES), "mode": 0, "lock_hidden": True})])

# Chat Live: the captures, the camera, the chat box, the music widget.
chat_items = [item("Ambient Background", 0, 0, 2560, 1440)] + \
    [item(c, 30, 85, 1755, 1269, visible=(i == 0)) for i, c in enumerate(CAPTURES)] + \
    [item("Camera", 1820, 85, 700, 394), item("Camera Square", 1820, 85, 700, 700, visible=False),
     item("Chat Box", 1820, 520, 700, 880), item("Music Widget", 1820, 1250), item("Alerts", 900, 40),
     item("Text Now Playing", 40, 1380), item("Text Clock", 2300, 1380), item("Console Feed", 30, 85, 1755, 1269,
                                                                                visible=False)]
scene("Chat Live", chat_items,
      [fit(c, i, resize=True, visible_pixels=True) for i, c in enumerate(CAPTURES[:4])] +
      [fit("Camera", 5, resize=False), fit("Chat Box", 6, resize=True), fit("Music Widget", 7, resize=False),
       fit("Console Feed", 8)] +
      [filt("stagekit_visibility_link", "Link Visibility (StageKit)",
            {"items": picks(CAPTURES + ["Console Feed"]), "mode": 0, "lock_hidden": True}),
       filt("stagekit_visibility_link", "Link Visibility Cameras (StageKit)",
            {"items": picks(["Camera", "Camera Square"]), "mode": 0}),
       filt("stagekit_visibility_link", "Link Visibility Chat (StageKit)",
            {"items": picks(["Chat Box", "Music Widget"]), "mode": 2}),
       filt("stagekit_visibility_link", "Link Visibility Alerts (StageKit)",
            {"items": picks(["Alerts", "Text Now Playing"]), "mode": 2})])

# Standby: the holding modes, each a title group and its extras.
modes = ("Stream Start", "Be Right Back", "Intermission", "Stream Ending")
standby_items = [item("Ambient Background", 0, 0, 2560, 1440)] + \
    [item(f"Title {t}", 680, 400, visible=(i == 0)) for i, t in enumerate(modes)] + \
    [item("Camera", 1820, 85, 700, 394), item("Chat Box", 1820, 520, 700, 880), item("Text Clock", 2300, 1380),
     item("Text Socials", 40, 1380), item("Music Widget", 40, 1250), item("Start Image", 0, 0, 2560, 1440,
                                                                          visible=False)]
scene("Standby", standby_items,
      [fit("Camera", 1, resize=False), fit("Chat Box", 2, resize=True),
       filt("stagekit_visibility_link", "Link Visibility Titles (StageKit)",
            {"items": picks([f"Title {t}" for t in modes]), "mode": 0})] +
      [filt("stagekit_visibility_link", f"Link Visibility {t} (StageKit)",
            {"items": picks([f"Title {t}"] + (["Camera"] if t in ("Stream Start", "Intermission") else ["Chat Box"])),
             "mode": 2}) for t in modes])

collection = {
    "name": "StageKit perf (synthetic)",
    "current_scene": "Live", "current_program_scene": "Live",
    "scene_order": [{"name": s["name"]} for s in sources if s["id"] == "scene"],
    "sources": sources, "groups": groups,
    "current_transition": "Morph (StageKit)", "transition_duration": 600,
    "transitions": [{"id": "stagekit_morph", "name": "Morph (StageKit)", "settings": {}}],
    "resolution": {"x": W, "y": H},
}

if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        print(__doc__.strip())
        sys.exit(2)
    with open(sys.argv[1], "w", encoding="utf-8") as f:
        json.dump(collection, f, indent=1)
    kinds = {}
    for s in sources + groups:
        for fl in s["filters"]:
            kinds[fl["id"]] = kinds.get(fl["id"], 0) + 1
    print(f"{sys.argv[1]}: {sum(1 for s in sources if s['id'] == 'scene')} scenes, {len(groups)} groups, "
          + ", ".join(f"{k[9:] if k.startswith('stagekit_') else k} {v}" for k, v in sorted(kinds.items())))
