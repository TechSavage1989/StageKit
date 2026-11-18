#!/usr/bin/env python3
"""Writes the plugin's manifest, its settings pages and each key's default
pictures from src/features.json and src/tools.ts, so that a feature is added
in one place. Needs Pillow, and the icon pack already imported
(tools/import-icons.py) and the small icons drawn (tools/make-icons.py).

Run from the streamdeck folder:  python3 tools/make-plugin-files.py
"""
import json
import os
import html
import re
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
PLUGIN = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin")
BASE = "com.techsavage1989.stagekit"
VERSION = "0.25.0.0"

# The pack picture each feature's key starts with (its "on" and "off" look).
PICTURES = {
    "stagekit_ramp_blur": ("blur-types/gaussian-on", "blur-types/gaussian-off"),
    "stagekit_shadow_backdrop": ("shadow-glow-outline/soft-on", "shadow-glow-outline/soft-off"),
    "stagekit_corners": ("corners/round-on", "corners/round-off"),
    "stagekit_auto_capture": ("filters/auto-capture-on", "filters/auto-capture-off"),
    "stagekit_auto_frame": ("filters/auto-frame-on", "filters/auto-frame-off"),
    "stagekit_standby": ("filters/standby-picture-on", "filters/standby-picture-off"),
    "stagekit_show_in_scenes": ("filters/show-in-scenes-on", "filters/show-in-scenes-off"),
    "stagekit_send_to": ("options/camera-top-right", None),
    "stagekit_place_item": ("options/camera-centre", None),
    "stagekit_ramp_zoom": ("options/zoom-on", "options/zoom-off"),
    "stagekit_fit_item": ("options/fit-on", "options/fit-off"),
    "stagekit_active_app": ("options/source-switcher-on", "options/source-switcher-off"),
    "stagekit_app_capture": ("options/window-switcher-on", "options/window-switcher-off"),
    "stagekit_program_trigger": ("options/trigger-on", "options/trigger-off"),
    "stagekit_visibility_link": ("options/link-on", "options/link-off"),
    "stagekit_show_hide": ("options/show-hide-on", "options/show-hide-off"),
    "stagekit_layout": ("options/layout", None),
    "stagekit_scene_fit": ("options/scene-fit-on", "options/scene-fit-off"),
    "stagekit_scene_shadow": ("options/scene-shadow-on", "options/scene-shadow-off"),
    "stagekit_scene_corners": ("options/scene-corners-on", "options/scene-corners-off"),
    "stagekit_scene_standby": ("filters/standby-picture-on", "filters/standby-picture-off"),
    "stagekit_scene_blur": ("blur-types/gaussian-on", "blur-types/gaussian-off"),
}


PALETTE = """		<sdpi-item label="Palette">
			<sdpi-select setting="palette" default="mono">
				<option value="mono">Mono</option>
				<option value="violet">Violet</option>
				<option value="ocean">Ocean</option>
				<option value="forest">Forest</option>
				<option value="ember">Ember</option>
				<option value="neon">Neon</option>
			</sdpi-select>
		</sdpi-item>
"""

CONNECTION = """		<details>
			<summary>OBS connection</summary>
			<sdpi-item label="Address">
				<sdpi-textfield setting="host" global placeholder="127.0.0.1"></sdpi-textfield>
			</sdpi-item>
			<sdpi-item label="Port">
				<sdpi-textfield setting="port" global placeholder="4455"></sdpi-textfield>
			</sdpi-item>
			<sdpi-item label="Password">
				<sdpi-password setting="password" global></sdpi-password>
			</sdpi-item>
			<p>
				In OBS: Tools &rarr; WebSocket Server Settings. Tick &ldquo;Enable WebSocket server&rdquo;, then
				&ldquo;Show Connect Info&rdquo; for the port and password. One connection serves every StageKit key.
			</p>
		</details>
"""

# The page's own text (the fold-out's heading and its note) in the same
# font, size and quiet colours as Stream Deck's own controls: left alone, a
# browser shows it in a serif face.
STYLE = """		<style>
			body, summary, p {
				font-family: "Segoe UI", -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif;
				font-size: 9pt;
			}
			details { margin-top: 10px; border-top: 1px solid rgba(255, 255, 255, 0.08); padding-top: 8px; }
			summary { color: #969696; cursor: pointer; padding: 2px 0 8px 2px; outline: none; }
			summary:hover { color: #d8d8d8; }
			details p { color: #8a8a8a; line-height: 1.45; margin: 8px 4px 4px 4px; }
		</style>
"""

FOLLOW = """
		<script>
			// The second list depends on the first: ask for it again when the
			// source changes (after the new source has been saved).
			const source = document.getElementById("source");
			const filter = document.getElementById("filter");
			source.addEventListener("valuechange", () => setTimeout(() => filter.refresh(), 200));
			source.addEventListener("change", () => setTimeout(() => filter.refresh(), 200));
		</script>
"""


def one_list(label, source="getTargets", setting="target", placeholder="Choose"):
    return (
        f'		<sdpi-item label="{label}">\n'
        f'			<sdpi-select setting="{setting}" datasource="{source}" placeholder="{placeholder}" show-refresh></sdpi-select>\n'
        f"		</sdpi-item>\n"
    )


def options(label, setting, default, pairs):
    rows = "\n".join(f'				<option value="{value}">{text}</option>' for value, text in pairs)
    return (
        f'		<sdpi-item label="{label}">\n'
        f'			<sdpi-select setting="{setting}" default="{default}">\n{rows}\n			</sdpi-select>\n'
        f"		</sdpi-item>\n"
    )


FOLLOW_BLUR = """
		<script>
			// The scenes depend on the filter chosen: ask for them again when it
			// changes (after it has been saved).
			const filter = document.getElementById("filter");
			const scene = document.getElementById("scene");
			filter.addEventListener("valuechange", () => setTimeout(() => scene.refresh(), 200));
			filter.addEventListener("change", () => setTimeout(() => scene.refresh(), 200));
		</script>
"""

FOLLOW_MODE = """
		<script>
			// The modes offered are the chosen filter's: ask for them again when
			// the Scene Modes changes (after it has been saved).
			const modeFilter = document.getElementById("modeFilter");
			const lists = ["mode", "longMode", "modeFor"].map((id) => document.getElementById(id));
			const again = () => setTimeout(() => lists.forEach((l) => l.refresh()), 300);
			modeFilter.addEventListener("valuechange", again);
			modeFilter.addEventListener("change", again);
		</script>
"""

MODE_PRESSES = [("on", "Switch on"), ("off", "Switch off"), ("toggle", "Toggle"), ("next", "Next mode")]


def mode_page_html():
    """A Scene Mode key's settings (the owner: "the key should connect to the
    selected scene mode filter ... and it should allow configuration of the
    specific modes as desired, whether it's on, off, toggle, or next"): the
    filter, what a press and a long press do and the mode they act on."""
    e = lambda t: html.escape(t, quote=True)

    def select(label, setting, default, pairs, title):
        rows = "\n".join(f'				<option value="{v}">{e(t)}</option>' for v, t in pairs)
        return (
            f'		<sdpi-item label="{label}" title="{e(title)}">\n'
            f'			<sdpi-select setting="{setting}" default="{default}">\n{rows}\n			</sdpi-select>\n'
            f"		</sdpi-item>\n"
        )

    def modes(label, setting, title, placeholder="Choose a mode"):
        return (
            f'		<sdpi-item label="{label}" title="{e(title)}">\n'
            f'			<sdpi-select id="{setting}" setting="{setting}" datasource="getFilterModes" placeholder="{placeholder}" show-refresh></sdpi-select>\n'
            "		</sdpi-item>\n"
        )

    return (
        '		<sdpi-item label="Scene Modes" title="The Scene Modes filter this key works with, by its scene (and its name where a scene has two).">\n'
        '			<sdpi-select id="modeFilter" setting="modeFilter" datasource="getModeFilters" placeholder="Choose a Scene Modes" show-refresh></sdpi-select>\n'
        "		</sdpi-item>\n"
        + select("On press", "press", "on", MODE_PRESSES,
                 "Switch on: its mode. Switch off: no mode (the filter must allow no mode). Toggle: its mode, or back to the mode before. "
                 "Next mode: the next of the modes used, in the filter's order, round again after the last.")
        + modes("Mode", "mode", "The mode Switch on and Toggle act on.")
        + select("Long press", "longPress", "none", [("none", "Nothing")] + MODE_PRESSES,
                 "What holding the key does. Nothing (as it comes): the key acts the moment it is pressed. "
                 "Set, a press acts when the key is let go, and holding it for the hold time does this instead.")
        + modes("Long press mode", "longMode", "The mode a long press's Switch on or Toggle acts on. Unset: the Mode above.", "As the Mode above")
        + '		<sdpi-item label="Hold time (ms)" title="How long the key must be held for its long press: 300 to 5000 ms, 800 as it comes.">\n'
        '			<sdpi-textfield setting="holdMs" placeholder="800" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n"
    )


def mode_settings_html():
    """Each mode's own settings on the key: one Settings for list (the modes
    of the key's filter) and, for the mode chosen there, Use, Its layout and
    Its image. One set of lists, not one per mode: the page is plain
    sdpi-components bound to settings, and the plugin keeps what they say per
    mode (src/modes.ts `editModeSettings`), as Scene Fit's Settings for does in
    OBS."""
    script = "import { modePictureLists } from './src/key-types.ts'; console.log(JSON.stringify(modePictureLists()));"
    done = subprocess.run(["node", "--input-type=module", "-e", script], cwd=ROOT, capture_output=True, text=True, check=True)
    lists = json.loads(done.stdout)
    return (
        '		<sdpi-item label="Settings for" title="A mode of the Scene Modes: choose one, then whether the key uses it and its image. Each mode keeps its own. The key shows the mode that is on.">\n'
        '			<sdpi-select id="modeFor" setting="modeFor" datasource="getFilterModes" placeholder="Choose a mode" show-refresh></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Use" title="Whether Next mode goes to this mode. Every mode is used as it comes.">\n'
        '			<sdpi-select setting="modeUse" default="use">\n'
        '				<option value="use">Used</option>\n'
        '				<option value="skip">Left out of Next mode</option>\n'
        "			</sdpi-select>\n"
        "		</sdpi-item>\n"
        + "".join(type_list_html(l) for l in lists)
    )


PRESS = options("On press", "press", "toggle", [("toggle", "Toggle"), ("on", "Switch on"), ("off", "Switch off")])


_LISTS = None


def type_lists(key):
    """A key's type lists (src/key-types.ts, the one place they are made):
    what its image shows, in place of the Image list there was up to
    beta155. The owner: "have a similar source type, scene type, etc style
    list relative to the button function"."""
    global _LISTS
    if _LISTS is None:
        script = (
            "import { keysWithLists, layoutList, typeLists } from './src/key-types.ts';"
            "const out = {}; for (const k of keysWithLists()) {"
            # (The Layout list goes just before the Source type or Scene type
            # list: the owner, "a selector to choose the layout and a second
            # selector for the specific type".)
            "const lists = typeLists(k); const layout = layoutList(k);"
            "if (layout) lists.splice(lists.findIndex((l) => l.setting === 'sourceType' || l.setting === 'sceneType'), 0, layout);"
            "out[k] = lists; }"
            "console.log(JSON.stringify(out));"
        )
        done = subprocess.run(["node", "--input-type=module", "-e", script], cwd=ROOT, capture_output=True, text=True, check=True)
        _LISTS = json.loads(done.stdout)
    return _LISTS.get(key, [])


def type_list_html(lst):
    """One type list: Automatic first, then the types in their groups. Left
    unset it shows Automatic (and keeps an image chosen before beta156)."""
    e = lambda t: html.escape(t, quote=True)
    rows = [f'				<option value="auto">{e(lst["automatic"])}</option>']
    for group in lst["groups"]:
        options = [f'					<option value="{e(x["value"])}">{e(x["label"])}</option>' for x in group["entries"]]
        if group["label"]:
            rows.append(f'				<optgroup label="{e(group["label"])}">')
            rows += options
            rows.append("				</optgroup>")
        else:
            rows += [o[1:] for o in options]
    return (
        f'		<sdpi-item label="{e(lst["label"])}" title="{e(lst["tip"])}">\n'
        f'			<sdpi-select setting="{lst["setting"]}" placeholder="{e(lst["automatic"])}">\n'
        + "\n".join(rows)
        + "\n			</sdpi-select>\n		</sdpi-item>\n"
    )


def output_lists():
    """The Stream and Record keys' On press and Long press lists, from
    src/live.ts (the one place they are made)."""
    script = (
        "import { PRESSES, longPresses } from './src/live.ts';"
        "console.log(JSON.stringify({ stream: [PRESSES.stream, longPresses('stream')], record: [PRESSES.record, longPresses('record')] }));"
    )
    done = subprocess.run(["node", "--input-type=module", "-e", script], cwd=ROOT, capture_output=True, text=True, check=True)
    return json.loads(done.stdout)


def output_page(output, presses, longs):
    """A Stream or Record key's settings: what a press and a long press do,
    the hold time, whether the image moves and whether the time shows."""
    e = lambda t: html.escape(t, quote=True)
    if output == "stream":
        tip = ("Toggle starts the stream, or stops it while live. There is no pause for a stream: "
               "OBS can pause a recording but not a stream, so this key has no Pause.")
    else:
        tip = "Toggle starts the recording, or stops it while recording. Pause / resume pauses it, or resumes it while paused."

    def select(label, setting, default, pairs, title):
        rows = "\n".join(f'				<option value="{v}">{e(t)}</option>' for v, t in pairs)
        return (
            f'		<sdpi-item label="{label}" title="{e(title)}">\n'
            f'			<sdpi-select setting="{setting}" default="{default}">\n{rows}\n			</sdpi-select>\n'
            f"		</sdpi-item>\n"
        )

    return (
        select("On press", "press", "toggle", presses, tip)
        + select("Long press", "longPress", "none", longs,
                 "What holding the key does. Nothing (as it comes): the key acts the moment it is pressed. "
                 "Set, a press acts when the key is let go, and holding it for the hold time does this instead: "
                 "for example On press Start and Long press Stop, so a stray tap never ends the " + ("stream." if output == "stream" else "recording."))
        + '		<sdpi-item label="Hold time (ms)" title="How long the key must be held for its long press: 300 to 5000 ms, 800 as it comes.">\n'
        '			<sdpi-textfield setting="holdMs" placeholder="800" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n"
        + select("Animate", "animate", "on", [("on", "On"), ("off", "Off")],
                 "On: the image moves while " + ("the stream starts, is live, reconnects or stops." if output == "stream" else "the recording starts, records, is paused or stops.") + " Off: it stays still.")
        + select("Show time", "showTime", "on", [("on", "On"), ("off", "Off")],
                 "On: the key's title shows how long the " + ("stream has been live." if output == "stream" else "recording has run."))
    )


def page(name, title, body, script="", key=None, after=""):
    html = (
        "<!doctype html>\n<html>\n	<head lang=\"en\">\n"
        f"		<title>{title}</title>\n"
        '		<meta charset="utf-8" />\n'
        '		<script src="https://sdpi-components.dev/releases/v4/sdpi-components.js"></script>\n'
        + STYLE +
        "	</head>\n\n	<body>\n" + body + "".join(type_list_html(l) for l in type_lists(key or name)) + after + PALETTE + "\n" + CONNECTION + script + "	</body>\n</html>\n"
    )
    with open(os.path.join(PLUGIN, "ui", name + ".html"), "w", encoding="utf-8") as out:
        out.write(html)


def key_pictures(folder, on, off, status=None):
    """A key's default pictures, from the mono pack, at 72 and 144 px (for a
    key with two states, the copies marked on and off)."""
    if status is None:
        status = off is not None
    target = os.path.join(PLUGIN, "imgs", "actions", folder)
    os.makedirs(target, exist_ok=True)
    for name, pack in (("on", on), ("off", off)):
        if not pack:
            continue
        marked = ("status/" + name + "/") if status else ""
        image = Image.open(os.path.join(PLUGIN, "imgs", "pack", "mono", marked + pack + ".png")).convert("RGBA")
        image.save(os.path.join(target, f"{name}@2x.png"))
        image.resize((72, 72), Image.LANCZOS).save(os.path.join(target, f"{name}.png"))


def tools_list():
    text = open(os.path.join(ROOT, "src", "tools.ts"), encoding="utf-8").read()
    return re.findall(r'\{ value: "([^"]+)", label: "([^"]+)" \}', text)


def main():
    features = json.load(open(os.path.join(ROOT, "src", "features.json"), encoding="utf-8"))
    actions = []

    def add(key, name, tip, page_name, icon, states, visible=True):
        # Every key offered has its own icon in Stream Deck's list
        # (tools/make-action-icons.py); the two hidden older keys keep theirs.
        if visible:
            icon = f"imgs/actions/{key}/icon"
        action = {
            "Name": name,
            "UUID": f"{BASE}.{key}",
            "Icon": icon,
            "Tooltip": tip,
            "PropertyInspectorPath": f"ui/{page_name}.html",
            "Controllers": ["Keypad"],
            "States": states,
        }
        if not visible:
            action["VisibleInActionsList"] = False
        if len(states) == 2:
            # The plugin says which state a key is in (what OBS says);
            # flipped by Stream Deck on every press, it flashed the other one.
            action["DisableAutomaticStates"] = True
        actions.append(action)

    def two(folder):
        return [
            {"Image": f"imgs/actions/{folder}/off", "Name": "Off", "TitleAlignment": "bottom"},
            {"Image": f"imgs/actions/{folder}/on", "Name": "On", "TitleAlignment": "bottom"},
        ]

    def one(folder, image="on"):
        return [{"Image": f"imgs/actions/{folder}/{image}", "TitleAlignment": "bottom"}]

    for feature in features["switches"]:
        on, off = PICTURES.get(feature["kind"], ("options/plain-switch-on", "options/plain-switch-off"))
        key_pictures(feature["key"], on, off)
        scene = feature.get("scene", False)
        page(feature["key"], f"StageKit {feature['name']}", one_list("Scene" if scene else "Source") + PRESS)
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/switch/icon", two(feature["key"]))
    for feature in features["moves"]:
        on, _ = PICTURES.get(feature["kind"], ("options/plain-move", None))
        key_pictures(feature["key"], on, None)
        page(feature["key"], f"StageKit {feature['name']}", one_list("Move"))
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/move/icon", one(feature["key"]))

    key_pictures("shape", "filters/auto-frame-on", None)
    shapes = [("auto", "Automatic (hand it back)")] + [(s, s) for s in ("4:3", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1", "64:49")]
    page(
        "shape",
        "StageKit set aspect ratio",
        one_list("Capture")
        + options("Ratio", "shape", "4:3", shapes)
        + options("Keep ratio", "keep", "game", [("game", "This game only"), ("always", "Until changed")]),
    )
    add("shape", "Set Aspect Ratio", "Sets a game's aspect ratio by hand: 4:3, 8:7 and so on, or back to automatic. It can last just for the game being captured.", "shape", "imgs/actions/switch/icon", one("shape"))

    key_pictures("hookgame", "options/hook-game", None)
    page(
        "hook",
        "StageKit hook capture",
        one_list("Capture", "getCaptures", "source", "Choose a capture")
        + '		<sdpi-item label="Transition">\n'
        '			<sdpi-select setting="transition" datasource="getTransitions" default="fade_transition"></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Duration (ms)">\n'
        '			<sdpi-textfield setting="lasting" placeholder="300" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n",
    )
    add("hook", "Capture Active Window", "Makes a Game Capture or a Window Capture take the active window or game. An Automatic Window Switcher on it holds that window as its manual window.", "hook", "imgs/actions/hook/icon", one("hookgame"))

    key_pictures("appswitch", "options/toggle", "options/toggle")
    page(
        "appswitch",
        "StageKit next manual capture",
        one_list("Scene or capture") + options("On press", "step", "next", [("toggle", "Toggle manual / automatic"), ("next", "Next manual window or source"), ("back", "Return to automatic"), ("hide", "Hide or show the capture"), ("clear", "Clear picked windows")]),
    )
    add("appswitch", "Manual Window / Source", "For an Automatic Window Switcher or an Automatic Source Switcher: toggles between the manual window and automatic, steps to the next manual one, returns to automatic, hides and shows the capture, or clears the windows picked by hand.", "appswitch", "imgs/actions/move/icon", two("appswitch"))

    key_pictures("fitoverlap", "options/overlap-on", "options/overlap-off")
    page("fitoverlap", "StageKit Fit to Space overlap", one_list("Scene") + PRESS)
    add("fitoverlap", "Fit to Space Overlap", "Switches a Fit to Space filter's 'Overlap only when it fills': on, the fitted window goes under the camera only while it fills its space, and beside it otherwise.", "fitoverlap", "imgs/actions/switch/icon", two("fitoverlap"))

    key_pictures("scene", "options/scene-on", "options/scene-off")
    page(
        "scene",
        "StageKit switch scene",
        one_list("Scene", "getScenes", "scene", "Choose a scene")
        + options("Switch", "where", "program", [("program", "Program (live)"), ("preview", "Preview (Studio Mode)")])
        + '		<sdpi-item label="Transition">\n'
        '			<sdpi-select setting="transition" datasource="getSceneTransitions" default=""></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Duration (ms)">\n'
        '			<sdpi-textfield setting="lasting" placeholder="As set in OBS" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n",
    )
    add("scene", "Switch Scene", "Switches OBS to a scene, on the program or (Studio Mode) the preview, with a transition if you choose one. Lit while that scene is on.", "scene", "imgs/actions/switch/icon", two("scene"))

    key_pictures("mode", "options/mode-on", "options/mode-off")
    page("mode", "StageKit scene mode", mode_page_html(), FOLLOW_MODE, after=mode_settings_html())
    add("mode", "Scene Mode", "Works one scene's Scene Modes: switches a mode on, off, toggles it or goes to the next mode. Shows the mode that is on.", "mode", "imgs/actions/switch/icon", two("mode"))

    # The keys for a filter on a scene that act through StageKit's own requests.
    for feature in features["scenes"]:
        on, off = PICTURES[feature["kind"]]
        key_pictures(feature["key"], on, off)
        page(
            feature["key"],
            f"StageKit {feature['name']}",
            '		<sdpi-item label="Blur filter">\n'
            '			<sdpi-select id="filter" setting="filter" datasource="getSceneBlurs" placeholder="Choose a Scene Blur" show-refresh></sdpi-select>\n'
            "		</sdpi-item>\n"
            '		<sdpi-item label="Scene">\n'
            '			<sdpi-select id="scene" setting="scene" datasource="getSceneBlurScenes" default="" show-refresh></sdpi-select>\n'
            "		</sdpi-item>\n"
            + PRESS,
            FOLLOW_BLUR,
        )
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/switch/icon", two(feature["key"]))

    # Stream and Record: obs-websocket's own requests. Their pictures are
    # tools/make-live-icons.py's frames (the key's own code moves them).
    lists = output_lists()
    for output, name, tip in (
        ("stream", "Stream", "Starts and stops OBS's stream: toggle, start or stop, with a long press of its own if you like. Moves while live. A stream cannot be paused in OBS."),
        ("record", "Record", "Starts, stops, pauses and resumes OBS's recording, with a long press of its own if you like. Moves while recording."),
    ):
        presses, longs = lists[output]
        on_frame = "live/stream-live-0" if output == "stream" else "live/record-recording-0"
        key_pictures(output, on_frame, f"live/{output}-idle-0")
        page(output, f"StageKit {name.lower()}", output_page(output, presses, longs))
        add(output, name, tip, output, "imgs/actions/switch/icon", two(output))

    key_pictures("toolkey", "options/align-left", None)
    page("tool", "StageKit arrange tool", options("Tool", "tool", "left", tools_list()))
    add("tool", "Arrange tool", "Lines up, spaces out, snaps or resizes the items selected in OBS, or copies a transform to other scenes.", "tool", "imgs/actions/tool/icon", one("toolkey"))

    key_pictures("blurall", "blur-types/gaussian-on", "blur-types/gaussian-off")
    page("blur-all", "StageKit blur all", "", key="blurall")
    add("blurall", "Blur All", "Switches every Blur on, or all of them off again. Shows on while any is on.", "blur-all", "imgs/actions/blur/icon", two("blurall"))

    key_pictures("option", "options/snap-on", "options/snap-off")
    switches = [
        ("snap", "Snap on release"),
        ("guides", "Guides"),
        ("limit", "Stop resizing at edges"),
        ("slide", "Slide to the next edge"),
        ("everyScene", "Move to: in every scene"),
        ("keepShape", "Keep shape when resizing"),
    ]
    page("option", "StageKit dock option", options("Option", "option", "snap", switches) + PRESS)
    add("option", "Dock Option", "One of the StageKit dock's options on a key: snap on release, guides, stop resizing at edges. Shows which way it is.", "option", "imgs/actions/switch/icon", two("option"))

    key_pictures("slide", "options/slide-left", None)
    ways = [
        ("left", "Left"),
        ("right", "Right"),
        ("up", "Up"),
        ("down", "Down"),
        ("up-left", "Up and left"),
        ("up-right", "Up and right"),
        ("down-left", "Down and left"),
        ("down-right", "Down and right"),
        ("centre", "Centre in the free space"),
    ]
    page("slide", "StageKit slide", options("Slide", "direction", "left", ways))
    add("slide", "Slide to Edge", "Slides the item selected in OBS to the next edge that way; the next press passes it.", "slide", "imgs/actions/move/icon", one("slide"))

    # The first two keys, kept for the keys already made with them.
    two_lists = (
        '		<sdpi-item label="Source">\n'
        '			<sdpi-select id="source" setting="source" datasource="getSources" placeholder="Choose a source" show-refresh></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Filter">\n'
        '			<sdpi-select id="filter" setting="filter" datasource="getFilters" placeholder="Choose" show-refresh></sdpi-select>\n'
        "		</sdpi-item>\n"
    )
    key_pictures("anyswitch", "options/plain-switch-on", "options/plain-switch-off")
    key_pictures("anymove", "options/plain-move", None)
    page("switch", "StageKit filter switch", two_lists + PRESS, FOLLOW)
    page("move", "StageKit move", two_lists, FOLLOW)
    add("switch", "Filter switch (older)", "Switches any StageKit filter. Use the key named after the feature instead.", "switch", "imgs/actions/switch/icon", two("anyswitch"), visible=False)
    add("move", "Move (older)", "Runs any StageKit move. Use the key named after the feature instead.", "move", "imgs/actions/move/icon", one("anymove"), visible=False)

    manifest = {
        "Name": "StageKit",
        "Version": VERSION,
        "Author": "StageKit contributors",
        "Actions": actions,
        "Category": "StageKit",
        "CategoryIcon": "imgs/plugin/category-icon",
        "CodePath": "bin/plugin.js",
        "Description": "Keys for StageKit, the OBS Studio plugin: one for each of its features, each showing its state, plus keys that capture the active window, set an aspect ratio, switch scenes, run the arrange tools and blur everything. Talks to OBS through OBS's own WebSocket server.",
        "Icon": "imgs/plugin/marketplace",
        "SDKVersion": 3,
        "Software": {"MinimumVersion": "7.1"},
        "OS": [{"Platform": "mac", "MinimumVersion": "12"}, {"Platform": "windows", "MinimumVersion": "10"}],
        "Nodejs": {"Version": "24", "Debug": "enabled"},
        "UUID": BASE,
    }
    with open(os.path.join(PLUGIN, "manifest.json"), "w", encoding="utf-8") as out:
        json.dump(manifest, out, indent="\t")
        out.write("\n")
    print(f"{len(actions)} keys, {len(os.listdir(os.path.join(PLUGIN, 'ui')))} pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
