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
VERSION = "0.40.0.0"

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
    "stagekit_match_item": ("options/match-item-on", "options/match-item-off"),
    "stagekit_scene_transitions": ("options/scene-transitions-on", "options/scene-transitions-off"),
}


PALETTE = """		<sdpi-item label="Palette" title="The colors used to draw this key. Applies to its on and off states.">
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
			<sdpi-item label="Address" title="Computer running OBS. Use 127.0.0.1 when Stream Deck and OBS run on the same computer.">
				<sdpi-textfield setting="host" global placeholder="127.0.0.1"></sdpi-textfield>
			</sdpi-item>
			<sdpi-item label="Port" title="The port shown in OBS under Tools, WebSocket Server Settings.">
				<sdpi-textfield setting="port" global placeholder="4455"></sdpi-textfield>
			</sdpi-item>
			<sdpi-item label="Password" title="The password shown in OBS under WebSocket Server Settings, Show Connect Info. All StageKit keys share it.">
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
			#worksWith { line-height: 1.4; padding: 6px 0 4px 0; cursor: default; }
			.works-kind { color: #d8d8d8; }
			.works-chosen { color: #8a8a8a; }
			.works-chosen:empty { display: none; }
		</style>
"""

_WORKS = None


def works(key):
    """A key's *Works with* row as src/works-with.ts says it (the one table
    of what every key works with in OBS). A key missing there stops the
    build here."""
    global _WORKS
    if _WORKS is None:
        script = "import { worksWithPages } from './src/works-with.ts'; console.log(JSON.stringify(worksWithPages()));"
        done = subprocess.run(["node", "--input-type=module", "-e", script], cwd=ROOT, capture_output=True, text=True, check=True)
        _WORKS = json.loads(done.stdout)
    return _WORKS[key]


def works_row(key):
    """The first row of every page: what the key works with in OBS, as
    information, not a control. The first line is the kind of filter exactly
    as OBS's Filters window names it (or the feature); the second, filled in
    by the plugin when the page asks (WORKS_SCRIPT), what the key is set to,
    or its note. The owner (2026-11-18): "I want the stream deck keys to show
    the filter or feature they work with somewhere outside its [list].
    somewhere in the configuration area at the top for all keys"."""
    w = works(key)
    e = lambda t: html.escape(t, quote=True)
    return (
        f'		<sdpi-item class="works" label="Works with" title="{e(w["tip"])}">\n'
        f'			<div id="worksWith" data-follow="{" ".join(w["follow"])}">\n'
        f'				<div class="works-kind">{e(w["first"])}</div>\n'
        f'				<div class="works-chosen">{e(w["note"])}</div>\n'
        "			</div>\n"
        "		</sdpi-item>\n"
    )


# The row's second line (and, for a key that works several kinds, its first)
# from the plugin: the same request and answer a list's datasource uses
# (sendToPlugin with an event, sendToPropertyInspector back with that event),
# read by the page itself. Asked when the page opens and again when a list
# it goes by changes, after the change has been saved. Without an answer the
# row keeps what the page was written with.
WORKS_SCRIPT = """
		<script>
			// Works with: what this key works in OBS, and what it is set to.
			(() => {
				const row = document.getElementById("worksWith");
				const client = window.SDPIComponents && window.SDPIComponents.streamDeckClient;
				if (!row || !client || !client.sendToPropertyInspector || typeof client.send !== "function") return;
				const kind = row.querySelector(".works-kind");
				const chosen = row.querySelector(".works-chosen");
				client.sendToPropertyInspector.subscribe((ev) => {
					const answer = ev && ev.payload;
					if (!answer || answer.event !== "getWorksWith") return;
					if (typeof answer.kind === "string" && answer.kind !== "") kind.textContent = answer.kind;
					if (typeof answer.chosen === "string") chosen.textContent = answer.chosen;
				});
				const ask = () => Promise.resolve().then(() => client.send("sendToPlugin", { event: "getWorksWith" })).catch(() => {});
				const later = () => setTimeout(ask, 200);
				for (const setting of (row.dataset.follow || "").split(" ").filter(Boolean))
					for (const list of document.querySelectorAll(`[setting="${setting}"]`)) {
						list.addEventListener("valuechange", later);
						list.addEventListener("change", later);
					}
				ask();
			})();
		</script>
"""

FOLLOW_SWITCHER = """
		<script>
			const target = document.querySelector('[setting="target"]');
			const step = document.querySelector('[setting="step"]');
			const refresh = () => setTimeout(() => step.refresh(), 200);
			target.addEventListener("valuechange", refresh);
			target.addEventListener("change", refresh);
		</script>
"""

CONDITIONAL_ROWS = """
		<script>
			(() => {
				const controls = Object.fromEntries(Array.from(document.querySelectorAll("[setting]"),
					(element) => [element.getAttribute("setting"), element]));
				const value = (setting) => controls[setting]?.value || controls[setting]?.getAttribute("default") || "";
				const visible = (setting, show) => {
					const row = controls[setting]?.closest("sdpi-item");
					if (row) { row.hidden = !show; row.style.display = show ? "" : "none"; }
				};
				const update = () => {
					const long = value("longPress");
					visible("holdMs", Boolean(long && long !== "none"));
					visible("longMode", long === "on" || long === "toggle");
					visible("modeUse", value("press") === "next" || long === "next");
					visible("hideAs", value("press") !== "on");
					visible("animate", !value("sceneType") || value("sceneType") === "auto");
				};
				for (const element of Object.values(controls)) {
					element.addEventListener("valuechange", update);
					element.addEventListener("change", update);
				}
				const client = window.SDPIComponents?.streamDeckClient;
				client?.didReceiveSettings?.subscribe(() => requestAnimationFrame(update));
				customElements.whenDefined("sdpi-select").then(() => requestAnimationFrame(update));
				update();
			})();
		</script>
"""


def one_list(label, source="getTargets", setting="target", placeholder="Choose one", tip=None):
    title = f' title="{html.escape(tip, quote=True)}"' if tip else ""
    return (
        f'		<sdpi-item label="{label}"{title}>\n'
        f'			<sdpi-select setting="{setting}" datasource="{source}" placeholder="{placeholder}" show-refresh></sdpi-select>\n'
        f"		</sdpi-item>\n"
    )


def obs_names(names):
    """The kinds of filter as OBS's Filters window lists them (src/kinds.ts
    `shownName`, through src/works-with.ts), quoted: "Source Filter: Blur
    (StageKit)", and "A or B" for a key that works several kinds."""
    quoted = [f"\u201c{n}\u201d" for n in names]
    return quoted[0] if len(quoted) == 1 else ", ".join(quoted[:-1]) + " or " + quoted[-1]


def target_list(key, label=None):
    """The list that picks the filter a key works, with the kind of filter to
    look for in OBS said in its tooltip. The owner: "The stream deck keys
    don't really say what filters they are associated with which makes it
    difficult for me to find the associated filters". Each entry reads
    "<source> \u2014 <filter>" (src/kinds.ts targetItems). The line under it
    that named the kind again (beta189) is the page's first row now, *Works
    with* (works_row), which also says the filter chosen."""
    w = works(key)
    where = w["where"]
    kinds = obs_names(w["kinds"])
    label = label or ("Scene and filter" if where == "scene" else "Source and filter")
    tip = (f"Each entry is a {where} and, after the dash, the name of its {kinds}. "
           f"In OBS: right-click the {where} and choose Filters to find that filter by its name.")
    return one_list(label, tip=tip)


def options(label, setting, default, pairs, tip):
    rows = "\n".join(f'				<option value="{value}">{text}</option>' for value, text in pairs)
    return (
        f'		<sdpi-item label="{label}" title="{html.escape(tip, quote=True)}">\n'
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

MODE_PRESSES = [("toggle", "Toggle"), ("on", "Switch on"), ("off", "Switch off"), ("next", "Next mode")]


def mode_page_html():
    """A Scene Mode key's settings. First the filter, then one *Mode settings*
    list (the modes of the key's filter) with that mode's own Use in Next,
    Layout and Image right under it, then the key's behaviour in a *Key*
    fold-out. The owner (2026-11-18): "the scene modes key is very long in the
    configuration when the different modes are stacked. allow the mode to
    configure to be selected from another selector under the scene modes
    selector, and have it's configurations configurable". One set of lists,
    not one per mode: the page is plain sdpi-components bound to settings, and
    the plugin keeps what they say per mode (src/modes.ts `editModeSettings`),
    as Scene Fit's Settings for does in OBS. The key-wide Layout and Mode type
    rows are gone; keys saved with them still read them behind every mode's
    own (src/pictures.ts `followedModePicture`)."""
    e = lambda t: html.escape(t, quote=True)

    def select(label, setting, default, pairs, title):
        rows = "\n".join(f'					<option value="{v}">{e(t)}</option>' for v, t in pairs)
        return (
            f'			<sdpi-item label="{label}" title="{e(title)}">\n'
            f'				<sdpi-select setting="{setting}" default="{default}">\n{rows}\n				</sdpi-select>\n'
            f"			</sdpi-item>\n"
        )

    def modes(label, setting, title, placeholder="Choose a mode", indent="			"):
        return (
            f'{indent}<sdpi-item label="{label}" title="{e(title)}">\n'
            f'{indent}	<sdpi-select id="{setting}" setting="{setting}" datasource="getFilterModes" placeholder="{placeholder}" show-refresh></sdpi-select>\n'
            f"{indent}</sdpi-item>\n"
        )

    script = "import { modePictureLists } from './src/key-types.ts'; console.log(JSON.stringify(modePictureLists()));"
    done = subprocess.run(["node", "--input-type=module", "-e", script], cwd=ROOT, capture_output=True, text=True, check=True)
    lists = json.loads(done.stdout)
    return (
        '		<sdpi-item label="Scene and filter" title="Each entry is a scene and, after the dash, the name of its \u201cScene Filter: Scene Modes (StageKit)\u201d. '
        'In OBS: right-click the scene and choose Filters to find that filter by its name.">\n'
        '			<sdpi-select id="modeFilter" setting="modeFilter" datasource="getModeFilters" placeholder="Choose one" show-refresh></sdpi-select>\n'
        "		</sdpi-item>\n"
        + modes("Mode settings", "modeFor",
                "A mode of the Scene Modes: choose one to set, just below, whether Next mode uses it and the key's layout and image while it is on. "
                "Each mode keeps its own. Chosen for you: the key's Switch to mode, else the first mode.", indent="		")
        + '		<sdpi-item label="Use in Next" title="Whether Next mode goes to this mode. Every mode is used as it comes.">\n'
        '			<sdpi-select setting="modeUse" default="use">\n'
        '				<option value="use">Used</option>\n'
        '				<option value="skip">Left out of Next mode</option>\n'
        "			</sdpi-select>\n"
        "		</sdpi-item>\n"
        + "".join(type_list_html(l) for l in lists)
        + "		<details open>\n"
        "			<summary>Key</summary>\n"
        + select("On press", "press", "on", MODE_PRESSES,
                 "Toggle selects this mode. Press again to leave it: clear it if the filter allows no mode, or return to the previous mode if one is available. "
                 "Switch on selects this mode. Switch off clears it if the filter allows no mode. "
                 "Next mode: the next of the modes used, in the filter's order, round again after the last.")
        + modes("Switch to mode", "mode", "The mode selected by Switch on or Toggle. A new key starts with the mode shown in Mode settings. Switch on shows its own mode. The other actions show the active mode.")
        + select("Long press", "longPress", "none", [("none", "Nothing")] + MODE_PRESSES,
                 "What holding the key does. Nothing (as it comes): the key acts the moment it is pressed. "
                 "Set, a press acts when the key is let go, and holding it for the hold time does this instead.")
        + modes("Long press mode", "longMode", "The mode a long press's Switch on or Toggle acts on. Unset: Switch to mode.", "As Switch to mode")
        + '			<sdpi-item label="Hold time (ms)" title="How long the key must be held for its long press: 300 to 5000 ms, 800 as it comes.">\n'
        '				<sdpi-textfield setting="holdMs" placeholder="800" pattern="[0-9]*"></sdpi-textfield>\n'
        "			</sdpi-item>\n"
        + "		</details>\n"
    )


PRESS = options("On press", "press", "toggle", [("toggle", "Toggle"), ("on", "Switch on"), ("off", "Switch off")],
                "Toggle reverses the current state. Switch on and Switch off always request the selected state.")

# The Standby Image key: the filter switched, or (append-only, after them)
# its image shown over the source on purpose and taken away, for a camera,
# a capture card or a display capture ("StandbyCover"). The owner: "The Hide
# shows standby image I think should have its own filter, or be separate
# from that automatic window switcher somehow".
STANDBY_PRESS = (
    '		<sdpi-item label="On press" title="Toggle, Switch on and Switch off switch the Standby Image filter itself '
    '(off: no image at all). Cover / uncover, Cover with image and Uncover show its image over the source now, '
    'whatever the source shows, and take it away again, the way the filter is set to change; '
    'the key is then lit while the image covers the source.">\n'
    '			<sdpi-select setting="press" default="toggle">\n'
    + "\n".join(f'				<option value="{v}">{t}</option>' for v, t in (
        ("toggle", "Toggle"), ("on", "Switch on"), ("off", "Switch off"),
        ("cover", "Cover / uncover"), ("coveron", "Cover with image"), ("coveroff", "Uncover")))
    + "\n			</sdpi-select>\n		</sdpi-item>\n"
)

# The Show in Scenes key's own way of hiding (append-only), for one filter
# that several keys share: the owner prefers "ONE Show in Scenes filter
# where each Stream Deck key chooses".
HIDE_AS = (
    '		<sdpi-item label="When hiding" title="As the filter says: as its Hide shows Standby Image box is set. '
    'Hide the sources: they are hidden, whatever that box says. Cover with Standby Image: each source with a '
    'Source Filter: Standby Image (StageKit) is covered with its image instead, the others are hidden. '
    'The press that shows them again undoes it.">\n'
    '			<sdpi-select setting="hideAs" default="filter">\n'
    + "\n".join(f'				<option value="{v}">{t}</option>' for v, t in (
        ("filter", "As the filter says"), ("hide", "Hide the sources"), ("cover", "Cover with Standby Image")))
    + "\n			</sdpi-select>\n		</sdpi-item>\n"
)


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


def page(name, title, body, script="", key=None, lists=True):
    """A key's settings page. `key` is the key's name in the manifest when
    the page has another name (blur-all). Every page starts with the key's
    *Works with* row."""
    html = (
        "<!doctype html>\n<html>\n	<head lang=\"en\">\n"
        f"		<title>{title}</title>\n"
        '		<meta charset="utf-8" />\n'
        '		<script src="https://sdpi-components.dev/releases/v4/sdpi-components.js"></script>\n'
        + STYLE +
        "	</head>\n\n	<body>\n" + works_row(key or name) + body
        + "		<details open>\n			<summary>Image</summary>\n"
        + ("".join(type_list_html(l) for l in type_lists(key or name)) if lists else "")
        + PALETTE + "		</details>\n" + CONNECTION + script + WORKS_SCRIPT
        + (CONDITIONAL_ROWS if name in ("mode", "stream", "record", "showinscenes") else "") + "	</body>\n</html>\n"
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
        # (tools/make-action-icons.py); the hidden older keys keep theirs.
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
        body = target_list(feature["key"])
        if feature["kind"] == "stagekit_standby":
            body += STANDBY_PRESS
        else:
            body += PRESS
        if feature["kind"] == "stagekit_show_in_scenes":
            body += HIDE_AS
        page(feature["key"], f"StageKit {feature['name']}", body)
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/switch/icon", two(feature["key"]))
    for feature in features["moves"]:
        on, _ = PICTURES.get(feature["kind"], ("options/plain-move", None))
        key_pictures(feature["key"], on, None)
        page(feature["key"], f"StageKit {feature['name']}", target_list(feature["key"]))
        key = feature["key"]
        add(key, feature["name"], feature["tip"], key, f"imgs/actions/{key}/icon", one(key), visible=key != "placeitem")

    for feature in features["autoFrameCommands"]:
        key = feature["key"]
        key_pictures(key, "options/" + feature["image"], None)
        page(key, "StageKit " + feature["name"], target_list(key, label="Capture and filter"))
        add(key, feature["name"], feature["tip"], key, f"imgs/actions/{key}/icon", one(key))

    key_pictures("shape", "filters/auto-frame-on", None)
    ratio_labels = {"8:7": "8:7 (SNES / NES, square pixels)", "64:49": "64:49 (SNES / NES on a 4:3 TV)"}
    shapes = [("auto", "Automatic (work it out)")] + [(value, ratio_labels.get(value, value)) for value in
              ("4:3", "8:7", "64:49", "3:2", "10:9", "16:15", "16:9", "16:10", "5:4", "1:1")]
    page(
        "shape",
        "StageKit set aspect ratio",
        target_list("shape", label="Capture and filter")
        + options("Ratio", "shape", "4:3", shapes, "The displayed width and height. Automatic hands control back to Auto Frame.")
        + options("Keep ratio", "keep", "game", [("game", "This game only"), ("always", "Until changed")], "Keep the override for this game, or until another ratio is selected."),
    )
    add("shape", "Set Aspect Ratio", "Sets a game's aspect ratio by hand: 4:3, 8:7 and so on, or back to automatic. It can last just for the game being captured.", "shape", "imgs/actions/switch/icon", one("shape"))

    key_pictures("hookgame", "options/hook-game", None)
    page(
        "hook",
        "StageKit hook capture",
        one_list("Capture", "getCaptures", "source", tip="The Game Capture or Window Capture that will take the active window.")
        + '		<sdpi-item label="Transition" title="How the capture changes windows. An Automatic Window Switcher uses its own transition instead.">\n'
        '			<sdpi-select setting="transition" datasource="getTransitions" default="fade_transition"></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Duration (ms)" title="Length of the capture transition in milliseconds. Cut is instant. An Automatic Window Switcher uses its own timing.">\n'
        '			<sdpi-textfield setting="lasting" placeholder="300" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n",
    )
    add("hook", "Capture Active Window", "Makes a Game Capture or a Window Capture take the active window or game. With an Automatic Window Switcher on it, the window becomes that filter's manual window instead.", "hook", "imgs/actions/hook/icon", one("hookgame"))

    key_pictures("appswitch", "options/toggle", "options/toggle")
    page(
        "appswitch",
        "StageKit next manual capture",
        target_list("appswitch", label="Source or scene and filter")
        + one_list("On press", "getSwitcherSteps", "step", "Choose an action", "Only the hotkeys registered by the selected filter are available."),
        script=FOLLOW_SWITCHER,
    )
    add("appswitch", "Manual Window / Source", "For an Automatic Window Switcher, an Automatic Source Switcher or (Hide only) an Auto Capture: toggles between the manual window and automatic, steps to the next manual one, returns to automatic, hides and shows the capture, or clears the windows picked by hand.", "appswitch", "imgs/actions/move/icon", two("appswitch"))

    key_pictures("fitoverlap", "options/overlap-on", "options/overlap-off")
    page("fitoverlap", "StageKit Fit to Space overlap", target_list("fitoverlap") + PRESS)
    add("fitoverlap", "Fit to Space Overlap", "Switches a Fit to Space filter's 'Overlap only when it fills': on, the fitted window goes under the camera only while it fills its space, and beside it otherwise.", "fitoverlap", "imgs/actions/switch/icon", two("fitoverlap"))

    key_pictures("scene", "options/scene-on", "options/scene-off")
    page(
        "scene",
        "StageKit switch scene",
        one_list("Scene", "getScenes", "scene", tip="The OBS scene this key selects.")
        + options("Switch", "where", "program", [("program", "Program (live)"), ("preview", "Preview (Studio Mode)")], "Preview selects the preview in Studio Mode and the live scene when Studio Mode is off.")
        + '		<sdpi-item label="Transition" title="Choose the transition used when switching. Leave OBS\'s as it is uses the current OBS transition.">\n'
        '			<sdpi-select setting="transition" datasource="getSceneTransitions" default=""></sdpi-select>\n'
        "		</sdpi-item>\n"
        '		<sdpi-item label="Duration (ms)" title="Blank keeps the current OBS duration. A number changes the duration of a transition with adjustable timing.">\n'
        '			<sdpi-textfield setting="lasting" placeholder="As set in OBS" pattern="[0-9]*"></sdpi-textfield>\n'
        "		</sdpi-item>\n"
        + options("After the switch", "restoreTransition", "keep", [("keep", "Keep the selected transition"), ("restore", "Restore the previous transition")],
                  "Keep makes this transition and duration the OBS defaults. Restore puts the previous values back after the live switch completes."),
    )
    add("scene", "Switch Scene", "Switches OBS to a scene, on the program or (Studio Mode) the preview, with a transition if you choose one. Lit while that scene is on.", "scene", "imgs/actions/switch/icon", two("scene"))

    key_pictures("mode", "options/mode-on", "options/mode-off")
    # (Its lists are per mode, inside the body; its Palette is in the body's Key fold-out.)
    page("mode", "StageKit scene mode", mode_page_html(), FOLLOW_MODE, lists=False)
    add("mode", "Scene Mode", "Works one scene's Scene Modes: switches a mode on, off, toggles it or goes to the next mode. Shows the mode that is on.", "mode", "imgs/actions/switch/icon", two("mode"))

    # The keys for a filter on a scene that act through StageKit's own requests.
    for feature in features["scenes"]:
        on, off = PICTURES[feature["kind"]]
        key_pictures(feature["key"], on, off)
        page(
            feature["key"],
            f"StageKit {feature['name']}",
            '		<sdpi-item label="Blur filter" title="The name of the Scene Blur filter in OBS.">\n'
            '			<sdpi-select id="filter" setting="filter" datasource="getSceneBlurs" placeholder="Choose one" show-refresh></sdpi-select>\n'
            "		</sdpi-item>\n"
            '		<sdpi-item label="Scene" title="Use the chosen filter in one scene or every scene that has it.">\n'
            '			<sdpi-select id="scene" setting="scene" datasource="getSceneBlurScenes" default="" show-refresh></sdpi-select>\n'
            "		</sdpi-item>\n"
            + PRESS,
            FOLLOW_BLUR,
        )
        add(feature["key"], feature["name"], feature["tip"], feature["key"], "imgs/actions/switch/icon", two(feature["key"]))

    timer = features["timer"]
    timer_script = "import { MARKS, OPERATIONS } from './src/timer.ts'; console.log(JSON.stringify([MARKS.map(m => [m.value, m.label]), OPERATIONS]));"
    marks, operations = json.loads(subprocess.run(["node", "--input-type=module", "-e", timer_script], cwd=ROOT, capture_output=True, text=True, check=True).stdout)
    key_pictures("timer", "options/type-countdown-on", "options/type-countdown-off")
    page("timer", "StageKit Countdown Timer",
         one_list("Timer", "getTimers", "target", "Choose a timer", "The private timer or a named Countdown Timer source in this OBS collection.")
         + options("On press", "timerPress", "toggle", operations, "Cycle advances the selected mark when tapped. Hold for 0.8 seconds to start or stop.")
         + options("Clock shortcut", "timerMark", "next", marks, "Set the timer to the next occurrence on the OBS computer's clock.")
         + '<sdpi-item id="cycleHelp" label="Cycle" title="A tap changes the deadline. A hold starts or stops without advancing the mark."><span>Tap to select :15, :30, :45 or :00. Hold for 0.8 seconds to start or stop.</span></sdpi-item>\n',
         script="""<script>
            (() => {
                const select = document.querySelector('[setting="timerPress"]');
                const shortcut = document.querySelector('[setting="timerMark"]').closest('sdpi-item');
                const help = document.getElementById('cycleHelp');
                const update = () => {
                    const op = select.value || 'toggle';
                    shortcut.style.display = ['set', 'toggle', 'setStart'].includes(op) ? '' : 'none';
                    help.style.display = op === 'cycle' ? '' : 'none';
                };
                for (const e of ['change', 'valuechange']) select.addEventListener(e, update);
                window.SDPIComponents?.streamDeckClient?.didReceiveSettings?.subscribe(() => requestAnimationFrame(update));
                customElements.whenDefined('sdpi-select').then(() => requestAnimationFrame(update));
                update();
            })();
         </script>""")
    add("timer", timer["name"], timer["tip"], "timer", "imgs/actions/timer/icon", two("timer"))

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
    page("tool", "StageKit Arrange Tool", options("Tool", "tool", "left", tools_list(), "Runs this Arrange tool on the sources selected in OBS."))
    add("tool", "Arrange Tool", "Lines up, spaces out, snaps or resizes the items selected in OBS, or copies a transform to other scenes.", "tool", "imgs/actions/tool/icon", one("toolkey"))

    key_pictures("blurall", "blur-types/gaussian-on", "blur-types/gaussian-off")
    page("blur-all", "StageKit blur all", PRESS, key="blurall")
    add("blurall", "Blur All", "Switches every Blur on, or all of them off again. Shows on while any is on.", "blur-all", "imgs/actions/blur/icon", two("blurall"))

    key_pictures("option", "options/snap-on", "options/snap-off")
    switches = [
        ("snap", "Snap on release"),
        ("guides", "Guides in the preview"),
        ("limit", "Stop at Edges"),
        ("slide", "Slide to the next edge"),
        ("everyScene", "Move in every scene"),
        ("keepShape", "Keep shape when resizing"),
    ]
    page("option", "StageKit dock option", options("Option", "option", "snap", switches, "The dock setting this key controls. The key lights when that setting is on.") + PRESS)
    add("option", "Dock Option", "Controls Snap on release, Guides in the preview, Stop at Edges, Slide to the next edge, Move in every scene, or Keep shape when resizing.", "option", "imgs/actions/switch/icon", two("option"))

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
    page("slide", "StageKit slide", options("Slide", "direction", "left", ways, "Direction the selected item moves to reach the next edge."))
    add("slide", "Slide to Edge", "Slides the item selected in OBS to the next edge that way; the next press passes it.", "slide", "imgs/actions/move/icon", one("slide"))

    # OBS's regular controls and the Start hotkey published by the Move scene filters.
    key_pictures("obsfilter", "options/plain-switch-on", "options/plain-switch-off")
    page("obsfilter", "StageKit OBS Filter", one_list("Source or scene and filter", tip="Select the source or scene and its filter in OBS.") + PRESS)
    add("obsfilter", "OBS Filter", "Switches any OBS source or scene filter on and off.", "obsfilter", "imgs/actions/obsfilter/icon", two("obsfilter"))

    key_pictures("obssource", "options/source-switcher-on", "options/source-switcher-off")
    page("obssource", "StageKit Source Visibility", one_list("Scene or group and source", "getOrdinaryTargets", tip="Choose the scene or group and the source to show or hide.") + PRESS)
    add("obssource", "Source Visibility", "Shows or hides one scene item, including an item inside a group.", "obssource", "imgs/actions/obssource/icon", two("obssource"))

    key_pictures("obsmute", "options/plain-switch-on", "options/plain-switch-off")
    page("obsmute", "StageKit Audio Mixer Mute", one_list("Audio input", "getOrdinaryTargets", tip="Choose the OBS audio input whose mute button this key controls.") + options("On press", "press", "toggle", [("toggle", "Toggle mute"), ("on", "Mute"), ("off", "Unmute")], "How this key sets the mixer mute button."))
    add("obsmute", "Audio Mixer Mute", "Mutes or unmutes an OBS audio input.", "obsmute", "imgs/actions/obsmute/icon", two("obsmute"))

    key_pictures("obsstudio", "options/studio-on", "options/studio-off")
    page("obsstudio", "StageKit Studio Mode", PRESS
         + options("Long press", "longPress", "transition", [("transition", "Preview to Program"), ("none", "Nothing")],
                   "Preview to Program sends the current Preview using OBS's chosen transition and duration. Studio Mode must be on. "
                   "A short press acts on release. With Nothing selected, On press acts immediately.")
         + '\t\t<sdpi-item label="Hold time (ms)" title="How long the key must be held for its long press: 300 to 5000 ms, 800 as it comes.">\n'
         '\t\t\t<sdpi-textfield setting="holdMs" placeholder="800" pattern="[0-9]*"></sdpi-textfield>\n'
         '\t\t</sdpi-item>\n')
    add("obsstudio", "Studio Mode", "A short press switches Studio Mode on or off. Hold to send Preview to Program, or set Long press to Nothing. One monitor means off; two monitors mean on.", "obsstudio", "imgs/actions/obsstudio/icon", two("obsstudio"))

    key_pictures("studiotransition", "options/studio-transition", None)
    page("studiotransition", "StageKit Preview to Program", "")
    add("studiotransition", "Preview to Program", "Sends Preview to Program with OBS's current transition and duration. Requires Studio Mode.", "studiotransition", "imgs/actions/studiotransition/icon", one("studiotransition"))

    key_pictures("obscamera", "options/plain-switch-on", "options/plain-switch-off")
    page("obscamera", "StageKit Virtual Camera", PRESS)
    add("obscamera", "Virtual Camera", "Starts or stops the OBS virtual camera.", "obscamera", "imgs/actions/obscamera/icon", two("obscamera"))

    key_pictures("resetstats", "options/reset-stats", None)
    page("resetstats", "StageKit Reset Stats", "")
    add("resetstats", "Reset Stats", "Resets OBS Stats, the same as pressing Reset in the OBS Stats window.", "resetstats", "imgs/actions/resetstats/icon", one("resetstats"))

    key_pictures("movesourceport", "options/camera-centre", None)
    page("movesourceport", "StageKit Move Source in Scene", target_list("movesourceport"))
    add("movesourceport", "Move Source in Scene", "Starts a StageKit Move Source scene filter.", "movesourceport", "imgs/actions/movesourceport/icon", one("movesourceport"))

    key_pictures("moveswapport", "options/layout", None)
    page("moveswapport", "StageKit Swap Sources in Scene", target_list("moveswapport"))
    add("moveswapport", "Swap Sources in Scene", "Starts a StageKit Move Source Swap scene filter.", "moveswapport", "imgs/actions/moveswapport/icon", one("moveswapport"))

    # The first two keys, kept for the keys already made with them.
    legacy_target = one_list("Source or scene and filter", tip="Select the source or scene and the StageKit filter to control.")
    key_pictures("anyswitch", "options/plain-switch-on", "options/plain-switch-off")
    key_pictures("anymove", "options/plain-move", None)
    page("switch", "StageKit filter switch", legacy_target + PRESS)
    page("move", "StageKit move", legacy_target)
    add("switch", "Filter switch (older)", "Switches one StageKit filter that stays on or off.", "switch", "imgs/actions/switch/icon", two("anyswitch"), visible=False)
    add("move", "Move (older)", "Runs a Move Source, Move Scene Item, Apply Layout or Aspect Ratio Preset filter once.", "move", "imgs/actions/move/icon", one("anymove"), visible=False)

    groups = [
        ("scene", "mode", "scenetransitions", "showhide", "showinscenes", "visibilitylink"),
        ("hook", "autocapture", "appcapture", "activeapp", "appswitch", "autoframe", "measurebars", "rememberratio", "measureandremember", "shape", "standby", "scenestandby", "programtrigger"),
        ("sendto", "placeitem", "matchitem", "layout", "fititem", "scenefit", "fitoverlap", "slide", "tool", "option"),
        ("blur", "sceneblur", "blurall", "shadow", "sceneshadow", "corners", "scenecorners", "zoom"),
        ("stream", "record", "timer"),
        ("obsfilter", "obssource", "obsmute", "obsstudio", "studiotransition", "obscamera", "resetstats", "movesourceport", "moveswapport"), ("switch", "move"),
    ]
    rank = {key: index for index, key in enumerate(key for group in groups for key in group)}
    actions.sort(key=lambda item: rank[item["UUID"].rsplit(".", 1)[1]])
    manifest = {
        "Name": "StageKit",
        "Version": VERSION,
        "Author": "StageKit contributors",
        "Actions": actions,
        "Category": "StageKit",
        "CategoryIcon": "imgs/plugin/category-icon",
        "CodePath": "bin/plugin.js",
        "Description": "Control OBS scenes, capture sources, filters, layouts, streaming and recording from Stream Deck. Keys show the current state over OBS WebSocket.",
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
