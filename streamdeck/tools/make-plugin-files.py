#!/usr/bin/env python3
"""Writes the plugin's manifest, its settings pages and each key's default
pictures from src/features.json and src/tools.ts, so that a feature is added
in one place. Needs Pillow, and the icon pack already imported
(tools/import-icons.py) and the small icons drawn (tools/make-icons.py).

Run from the streamdeck folder:  python3 tools/make-plugin-files.py
"""
import json
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
PLUGIN = os.path.join(ROOT, "com.techsavage1989.stagekit.sdPlugin")
BASE = "com.techsavage1989.stagekit"
VERSION = "0.6.0.0"

# The pack picture each feature's key starts with (its "on" and "off" look).
PICTURES = {
    "stagekit_ramp_blur": ("blur-types/gaussian-on", "blur-types/gaussian-off"),
    "stagekit_shadow_backdrop": ("shadow-glow-outline/soft-on", "shadow-glow-outline/soft-off"),
    "stagekit_corners": ("corners/round-on", "corners/round-off"),
    "stagekit_auto_capture": ("filters/auto-capture-on", "filters/auto-capture-off"),
    "stagekit_auto_frame": ("filters/auto-frame-on", "filters/auto-frame-off"),
    "stagekit_standby": ("filters/standby-picture-on", "filters/standby-picture-off"),
    "stagekit_show_in_scenes": ("filters/show-in-scenes-on", "filters/show-in-scenes-off"),
    "stagekit_send_to": ("camera/camera-top-right", None),
    "stagekit_place_item": ("camera/camera-centre", None),
}

PICTURE_AND_PALETTE = """		<sdpi-item label="Picture">
			<sdpi-select setting="picture" datasource="getPictures" default=""></sdpi-select>
		</sdpi-item>
		<sdpi-item label="Palette">
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


PRESS = options("On press", "press", "toggle", [("toggle", "Toggle"), ("on", "Switch on"), ("off", "Switch off")])


def page(name, title, body, script=""):
    html = (
        "<!doctype html>\n<html>\n	<head lang=\"en\">\n"
        f"		<title>{title}</title>\n"
        '		<meta charset="utf-8" />\n'
        '		<script src="https://sdpi-components.dev/releases/v4/sdpi-components.js"></script>\n'
        + STYLE +
        "	</head>\n\n	<body>\n" + body + PICTURE_AND_PALETTE + "\n" + CONNECTION + script + "	</body>\n</html>\n"
    )
    with open(os.path.join(PLUGIN, "ui", name + ".html"), "w", encoding="utf-8") as out:
        out.write(html)


def key_pictures(folder, on, off):
    """A key's default pictures, from the mono pack, at 72 and 144 px."""
    target = os.path.join(PLUGIN, "imgs", "actions", folder)
    os.makedirs(target, exist_ok=True)
    for name, pack in (("on", on), ("off", off)):
        if not pack:
            continue
        image = Image.open(os.path.join(PLUGIN, "imgs", "pack", "mono", pack + ".png")).convert("RGBA")
        image.save(os.path.join(target, f"{name}@2x.png"))
        image.resize((72, 72), Image.LANCZOS).save(os.path.join(target, f"{name}.png"))


def tools_list():
    text = open(os.path.join(ROOT, "src", "tools.ts"), encoding="utf-8").read()
    return re.findall(r'\{ value: "([^"]+)", label: "([^"]+)" \}', text)


def main():
    features = json.load(open(os.path.join(ROOT, "src", "features.json"), encoding="utf-8"))
    actions = []

    def add(key, name, tip, page_name, icon, states, visible=True):
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
        actions.append(action)

    def two(folder):
        return [
            {"Image": f"imgs/actions/{folder}/off", "Name": "Off", "TitleAlignment": "bottom"},
            {"Image": f"imgs/actions/{folder}/on", "Name": "On", "TitleAlignment": "bottom"},
        ]

    def one(folder, image="on"):
        return [{"Image": f"imgs/actions/{folder}/{image}", "TitleAlignment": "bottom"}]

    for feature in features["switches"]:
        on, off = PICTURES.get(feature["kind"], ("plain/switch-on", "plain/switch-off"))
        key_pictures(feature["key"], on, off)
        page(feature["key"], f"StageKit {feature['name']}", one_list("Source") + PRESS)
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/switch/icon", two(feature["key"]))
    for feature in features["moves"]:
        on, _ = PICTURES.get(feature["kind"], ("plain/move", None))
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

    key_pictures("hookgame", "capture/game-on", None)
    page(
        "hook",
        "StageKit hook capture",
        one_list("Capture", "getCaptures", "source", "Choose a capture")
        + '		<sdpi-item label="Change with">\n'
        '			<sdpi-select setting="transition" datasource="getTransitions" default="fade_transition"></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Lasting (ms)">\n'
        '			<sdpi-textfield setting="lasting" placeholder="300" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n",
    )
    add("hook", "Capture Front Window", "Makes a Game Capture or a Window Capture take the window or game that is in front right now.", "hook", "imgs/actions/hook/icon", one("hookgame"))

    key_pictures("appswitch", "filters/next-capture", None)
    page(
        "appswitch",
        "StageKit next manual capture",
        one_list("Scene") + options("On press", "step", "next", [("next", "Next capture"), ("back", "Back to automatic")]),
    )
    add("appswitch", "Next Manual Capture", "Steps a Follow Active App scene to the next of its captures by hand, or back to following the apps.", "appswitch", "imgs/actions/move/icon", one("appswitch"))

    key_pictures("toolkey", "plain/tool", None)
    page("tool", "StageKit arrange tool", options("Tool", "tool", "left", tools_list()))
    add("tool", "Arrange tool", "Lines up, spaces out, snaps or resizes the items selected in OBS, or copies a transform to other scenes.", "tool", "imgs/actions/tool/icon", one("toolkey"))

    key_pictures("blurall", "blur-types/gaussian-on", "blur-types/gaussian-off")
    page("blur-all", "StageKit blur all", "")
    add("blurall", "Blur All", "Switches every Blur on, or all of them off again. Shows on while any is on.", "blur-all", "imgs/actions/blur/icon", two("blurall"))

    key_pictures("option", "tools/snap-on", "tools/snap-off")
    switches = [
        ("snap", "Snap on release"),
        ("guides", "Guides"),
        ("limit", "Stop resizing at edges"),
        ("slide", "Slide to the next edge"),
        ("everyScene", "Send to: in every scene"),
        ("keepShape", "Keep shape when resizing"),
    ]
    page("option", "StageKit dock option", options("Option", "option", "snap", switches) + PRESS)
    add("option", "Dock Option", "One of the StageKit dock's options on a key: snap on release, guides, stop resizing at edges. Shows which way it is.", "option", "imgs/actions/switch/icon", two("option"))

    key_pictures("slide", "tools/slide-left", None)
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
    key_pictures("anyswitch", "plain/switch-on", "plain/switch-off")
    key_pictures("anymove", "plain/move", None)
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
        "Description": "Keys for StageKit, the OBS Studio plugin: one for each of its features, each showing its state, plus keys that hook a capture, set a picture shape, run the arrange tools and blur everything. Talks to OBS through OBS's own WebSocket server.",
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
