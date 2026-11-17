import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { test } from "node:test";

import {
	automatic,
	choose,
	COLOURS,
	file,
	pictureItems,
	PICTURES,
	hookPicture,
	optionPicture,
	plain,
	shapePicture,
	slidePicture,
	stepPicture,
	toolPicture,
} from "../src/pictures.ts";
import { moveTitle, optionTitle, shapeTitle, slideTitle, stepTitle, switchTitle, toolTitle } from "../src/titles.ts";
import { hotkeyOf, TOOLS } from "../src/tools.ts";

test("a Switch Scene key is called after its scene, over two lines when it is long", async () => {
	const { sceneTitle } = await import("../src/titles.ts");
	const { scenePicture } = await import("../src/pictures.ts");
	assert.equal(sceneTitle(""), "Scene");
	assert.equal(sceneTitle("Gaming"), "Gaming");
	assert.equal(sceneTitle("Be Right Back"), "Be Right\nBack");
	assert.ok(sceneTitle("Game-Component-HD60S-Capture-Scene").split("\n").every((line) => line.length <= 10));
	assert.equal(scenePicture(undefined).id, "options/scene");
	assert.notEqual(scenePicture(undefined).on, scenePicture(undefined).off);
});

test("the devices group offers capture cards and the rest, in every palette", () => {
	const devices = PICTURES.filter((p) => p.group === "devices").map((p) => p.id);
	for (const name of ["capture-card", "capture-box", "hdmi", "console", "webcam", "camcorder", "dslr", "microphone"])
		assert.ok(devices.includes(`options/${name}`), name);
});

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);

test("every picture of the pack is there, on and off, in every colour", () => {
	assert.ok(PICTURES.length > 150);
	for (const colour of COLOURS)
		for (const picture of PICTURES)
			for (const on of [true, false])
				assert.ok(existsSync(new URL(file(picture, on, colour), plugin)), file(picture, on, colour));
});

test("a filter gets the picture that fits how it is set", () => {
	assert.equal(automatic("stagekit_ramp_blur")?.id, "blur-types/gaussian", "the usual type when none is saved");
	assert.equal(automatic("stagekit_ramp_blur", { blur_type: 6 })?.id, "blur-types/hexagons");
	assert.notEqual(automatic("stagekit_ramp_blur")?.on, automatic("stagekit_ramp_blur")?.off);
	assert.equal(automatic("stagekit_corners", { style: 3 })?.id, "corners/scoop");
	assert.equal(automatic("stagekit_shadow_backdrop")?.id, "shadow-glow-outline/soft");
	assert.equal(automatic("stagekit_shadow_backdrop", { shadow_type: 1 })?.id, "shadow-glow-outline/glow");
	assert.equal(automatic("stagekit_shadow_backdrop", { shadow: false, outline: true })?.id, "shadow-glow-outline/outline");
	assert.equal(automatic("stagekit_send_to")?.id, "options/camera-top-right", "where a new Send To starts");
	assert.equal(automatic("stagekit_send_to", { destination: 6 })?.id, "options/camera-bottom-left");
	assert.equal(automatic("stagekit_send_to", { destination: 9 })?.id, "options/spot", "a saved spot has its own");
	assert.equal(automatic("stagekit_standby")?.id, "filters/standby-picture");
	assert.equal(automatic("stagekit_fit_item")?.id, "options/fit", "drawn for a kind the pack has none of");
	assert.equal(automatic("stagekit_app_capture")?.id, "options/window-switcher");
	assert.equal(automatic("color_filter"), undefined);
});

test("a picture chosen by name wins; an unknown name falls back to the fitting one", () => {
	assert.equal(choose("show-hide/chat", "stagekit_ramp_blur")?.id, "show-hide/chat");
	assert.equal(choose("no/such", "stagekit_corners")?.id, "corners/round");
	assert.equal(choose("", "color_filter"), undefined);
});

test("what the pack has no picture of gets a plain key, and tools get theirs", () => {
	assert.equal(plain("switch").on, "options/plain-switch-on");
	assert.equal(plain("switch").off, "options/plain-switch-off");
	assert.equal(plain("move").on, "options/plain-move");
	assert.equal(toolPicture(undefined, "same_everywhere").id, "options/copy-all");
	assert.equal(toolPicture(undefined, "left").id, "options/align-left", "every tool has a picture of its own");
	for (const tool of TOOLS) assert.notEqual(toolPicture(undefined, tool.value).id, "options/plain-tool", tool.value);
	assert.equal(toolPicture(undefined, "nonsense").id, "options/plain-tool");
	assert.equal(toolPicture("camera/camera-centre", "left").id, "camera/camera-centre");
	assert.equal(hotkeyOf("left"), "stagekit.arrange.left");
	assert.equal(hotkeyOf("nonsense"), undefined);
	assert.equal(hotkeyOf(undefined), undefined);
	assert.equal(TOOLS.length, 14);
	assert.equal(hookPicture(undefined, "game_capture").id, "options/hook-game");
	assert.equal(hookPicture(undefined, "window_capture").id, "options/hook-window");
	assert.equal(hookPicture(undefined, undefined).id, "options/hook-game", "until OBS says which kind it is");
	assert.equal(hookPicture("show-hide/chat", "window_capture").id, "show-hide/chat");
});

test("files are per palette, mono unless another is asked for", () => {
	const blur = automatic("stagekit_ramp_blur")!;
	assert.equal(file(blur, true, undefined), "imgs/pack/mono/blur-types/gaussian-on.png");
	assert.equal(file(blur, false, "ocean"), "imgs/pack/ocean/blur-types/gaussian-off.png");
	assert.equal(file(blur, true, "no-such-colour"), "imgs/pack/mono/blur-types/gaussian-on.png");
});

test("the list to choose from starts with Automatic and holds every picture once", () => {
	const items = pictureItems();
	assert.deepEqual(items[0], { label: "Automatic (fits the filter)", value: "" });
	const count = items.slice(1).reduce((sum, item) => sum + ("children" in item ? item.children.length : 0), 0);
	assert.equal(count, PICTURES.length);
});

test("every option of every key has a picture of its own, and a title", () => {
	const steps = ["toggle", "next", "back", "hide"].map((step) => stepPicture(undefined, step).id);
	assert.equal(new Set(steps).size, 4, steps.join(" "));
	const options = ["snap", "guides", "limit", "slide", "everyScene", "keepShape"].map((o) => optionPicture(undefined, o).id);
	assert.equal(new Set(options).size, 6, options.join(" "));
	assert.ok(!options.includes("plain/switch"));
	const ways = ["left", "right", "up", "down", "up-left", "up-right", "down-left", "down-right", "centre"];
	assert.equal(new Set(ways.map((w) => slidePicture(undefined, w).id)).size, 9);
	const shapes = ["auto", "4:3", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1", "64:49"];
	assert.equal(new Set(shapes.map((sh) => shapePicture(undefined, sh).id)).size, 10);
	assert.ok(!shapes.map((sh) => shapePicture(undefined, sh).id).includes("plain/switch"));

	assert.equal(stepTitle("toggle"), "Manual /\nAuto");
	assert.equal(stepTitle(undefined), "Next\nManual");
	assert.equal(switchTitle("stagekit_ramp_blur", "toggle"), "Blur");
	assert.equal(switchTitle("stagekit_ramp_blur", "off"), "Blur\nOff");
	assert.equal(switchTitle("stagekit_app_capture", undefined), "Window\nSwitcher");
	assert.equal(moveTitle("stagekit_send_to", { destination: 6 }), "Bottom\nLeft");
	assert.equal(moveTitle("stagekit_send_to"), "Top\nRight", "where a new one starts");
	assert.equal(moveTitle("stagekit_send_to", { destination: 9 }), "Saved\nSpot");
	assert.equal(toolTitle("same_width"), "Same\nWidth");
	assert.equal(optionTitle("guides", "on"), "Guides\nOn");
	assert.equal(slideTitle("up-left"), "Slide\nUp Left");
	assert.equal(shapeTitle("8:7"), "8:7");
});

test("the Fit to Space Overlap key has a picture of its own and says which way it switches", async () => {
	const { overlapPicture } = await import("../src/pictures.ts");
	const { overlapTitle } = await import("../src/titles.ts");
	assert.equal(overlapPicture(undefined).id, "options/overlap");
	assert.notEqual(overlapPicture(undefined).on, overlapPicture(undefined).off);
	assert.equal(overlapTitle("toggle"), "Overlap\nIf Fills");
	assert.equal(overlapTitle("off"), "Overlap\nIf Fills\nOff");
});

test("the Manual Window / Source key's Clear picked windows has its own picture and title", async () => {
	const { stepPicture } = await import("../src/pictures.ts");
	const { stepTitle } = await import("../src/titles.ts");
	assert.equal(stepPicture(undefined, "clear").id, "options/clear");
	assert.equal(stepTitle("clear"), "Clear\nPicked");
});

test("a key with two states shows a picture marked on or off, in every palette", async () => {
	const { stateFile } = await import("../src/pictures.ts");
	for (const colour of COLOURS)
		for (const picture of PICTURES)
			for (const on of [true, false]) {
				const path = stateFile(picture, on, colour);
				assert.ok(path.includes(`/status/${on ? "on" : "off"}/`), path);
				assert.ok(existsSync(new URL(path, plugin)), path);
			}
	// One file for both, as some pictures have: two marked copies all the same.
	const toggle = PICTURES.find((p) => p.on === p.off);
	if (toggle) assert.notEqual(stateFile(toggle, true, "mono"), stateFile(toggle, false, "mono"));
});

test("Stream Deck does not flip a two-state key by itself", async () => {
	const { readFileSync } = await import("node:fs");
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	for (const action of manifest.Actions)
		if (action.States.length === 2) assert.equal(action.DisableAutomaticStates, true, action.UUID);
	const app = manifest.Actions.find((a: { UUID: string }) => a.UUID.endsWith(".appswitch"));
	assert.equal(app.States.length, 2, "Manual Window / Source shows which way it is");
});

test("a Switch Scene key takes the pack's picture of its scene's layout, by name", async () => {
	const { scenePicture, scenePictureFor } = await import("../src/pictures.ts");
	assert.equal(scenePictureFor("Game-Capture-Scene")?.label.includes("game capture"), true);
	assert.equal(scenePictureFor("Game-Component-Capture-Scene")?.label.includes("game component"), true);
	assert.equal(scenePictureFor("Game-Component-HD60S-Capture-Scene")?.label.includes("game component hd60s"), true);
	assert.equal(scenePictureFor("Window-Component-Capture-Scene")?.label.includes("window component"), true);
	assert.equal(scenePictureFor("Window-Capture-Scene")?.label.includes("window capture"), true);
	assert.equal(scenePictureFor("Be Right Back")?.label.includes("be right back"), true);
	assert.equal(scenePictureFor("Stream Start")?.label.includes("stream start"), true);
	assert.equal(scenePictureFor("Something Else Entirely"), undefined);
	assert.equal(scenePicture(undefined, "Something Else Entirely").id, "options/scene");
	assert.equal(scenePicture(undefined, "Intermission").group, "scenes");
	assert.equal(scenePicture("options/scene", "Intermission").id, "options/scene", "a picture chosen by hand wins");
});

test("a filter key's picture fits the source it is on", async () => {
	const { chooseFor, sourcePictureFor, inputPicture } = await import("../src/pictures.ts");
	// The pack's own picture of that source, by the words of its name.
	assert.equal(sourcePictureFor("stagekit_show_in_scenes", {}, "Square Camera")?.id, "show-hide/square-camera");
	assert.equal(sourcePictureFor("stagekit_shadow_backdrop", {}, "Window Capture")?.id, "shadow-by-source/window-capture");
	assert.equal(sourcePictureFor("stagekit_ramp_blur", { blur_type: 1 }, "Game Capture")?.group, "blur box");
	assert.equal(sourcePictureFor("stagekit_show_in_scenes", {}, "Something Else"), undefined);
	assert.equal(sourcePictureFor("stagekit_corners", {}, "Square Camera"), undefined, "only kinds the pack drew by source");
	// Else what the source is: a camera, a window ...
	assert.equal(inputPicture("dshow_input")?.id, "options/webcam");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Overhead", "dshow_input")?.id, "options/webcam");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Notes", "window_capture")?.id, "capture/window");
	// Else the filter's own picture; and a picture chosen by hand always wins.
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Overhead", undefined)?.id, "filters/show-in-scenes");
	assert.equal(chooseFor(undefined, "stagekit_shadow_backdrop", {}, "Overhead", "dshow_input")?.group, "shadow glow outline");
	assert.equal(chooseFor("options/browser", "stagekit_show_in_scenes", {}, "Square Camera", "dshow_input")?.id, "options/browser");
});

test("the Live, Chat Live and Standby scenes have pictures of their own, the one with the most words first", async () => {
	const { scenePicture, scenePictureFor } = await import("../src/pictures.ts");
	assert.equal(scenePictureFor("Live")?.id, "options/scene-live");
	assert.equal(scenePictureFor("Chat Live")?.id, "options/scene-chat-live", "not the Live picture");
	assert.equal(scenePictureFor("Chat-Live-Scene")?.id, "options/scene-chat-live");
	assert.equal(scenePictureFor("Live Chat")?.id, "options/scene-chat-live", "words in any order");
	assert.equal(scenePictureFor("Standby")?.id, "options/scene-standby");
	assert.equal(scenePictureFor("Standby-Scene")?.id, "options/scene-standby");
	for (const id of ["options/scene-live", "options/scene-chat-live", "options/scene-standby"]) {
		const picture = scenePicture(id, "Anything");
		assert.equal(picture.group, "scenes");
		assert.notEqual(picture.on, picture.off, "an on and an off look");
	}
	assert.equal(scenePicture("options/scene", "Live").id, "options/scene", "a picture chosen by hand wins");
	assert.equal(scenePicture("options/scene-standby", "Chat Live").id, "options/scene-standby");
});

test("the holding modes' and the columns' show keys have pictures of their own", async () => {
	const { chooseFor, sourcePictureFor } = await import("../src/pictures.ts");
	const fits: Record<string, string> = {
		"Stream Starting Group": "options/mode-starting",
		"BRB Text": "options/mode-brb",
		"Intermission Text": "options/mode-intermission",
		"Ending Text": "options/mode-ending",
		"Column: Chat": "options/mode-column-chat",
		"Column: GBA": "options/mode-column-gba",
	};
	for (const [source, id] of Object.entries(fits)) {
		assert.equal(sourcePictureFor("stagekit_show_in_scenes", {}, source)?.id, id, source);
		assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, source, "text_gdiplus_v3")?.id, id, source);
	}
	// A plain chat source keeps the pack's own chat picture; the column takes its own.
	assert.equal(sourcePictureFor("stagekit_show_in_scenes", {}, "Chat")?.id, "show-hide/chat");
	// Only Show in Scenes keys go by these.
	assert.equal(sourcePictureFor("stagekit_shadow_backdrop", {}, "BRB Text"), undefined);
	// A picture chosen by hand wins.
	assert.equal(chooseFor("options/browser", "stagekit_show_in_scenes", {}, "BRB Text")?.id, "options/browser");
	assert.equal(chooseFor("options/mode-ending", "stagekit_show_in_scenes", {}, "Column: Chat")?.id, "options/mode-ending");
});

test("the new scene and mode pictures are all different, and marked on and off in every colour", async () => {
	const { readFileSync } = await import("node:fs");
	const { stateFile } = await import("../src/pictures.ts");
	const ids = [
		"options/scene-live",
		"options/scene-chat-live",
		"options/scene-standby",
		"options/mode-starting",
		"options/mode-brb",
		"options/mode-intermission",
		"options/mode-ending",
		"options/mode-column-chat",
		"options/mode-column-gba",
	];
	for (const colour of COLOURS) {
		const seen = new Set<string>();
		for (const id of ids) {
			const picture = PICTURES.find((p) => p.id === id);
			assert.ok(picture, id);
			for (const on of [true, false]) {
				const path = new URL(stateFile(picture, on, colour), plugin);
				assert.ok(existsSync(path), stateFile(picture, on, colour));
			}
			seen.add(readFileSync(new URL(file(picture, true, colour), plugin)).toString("base64"));
		}
		assert.equal(seen.size, ids.length, `two of the new pictures are the same in ${colour}`);
	}
});
