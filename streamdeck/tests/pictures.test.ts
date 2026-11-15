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
	assert.equal(automatic("stagekit_send_to")?.id, "camera/camera-top-right", "where a new Send To starts");
	assert.equal(automatic("stagekit_send_to", { destination: 6 })?.id, "camera/camera-bottom-left");
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
	assert.equal(plain("switch").on, "plain/switch-on");
	assert.equal(plain("switch").off, "plain/switch-off");
	assert.equal(plain("move").on, "plain/move");
	assert.equal(toolPicture(undefined, "same_everywhere").id, "tools/copy-transform-all");
	assert.equal(toolPicture(undefined, "left").id, "options/align-left", "every tool has a picture of its own");
	for (const tool of TOOLS) assert.notEqual(toolPicture(undefined, tool.value).id, "plain/tool", tool.value);
	assert.equal(toolPicture(undefined, "nonsense").id, "plain/tool");
	assert.equal(toolPicture("camera/camera-centre", "left").id, "camera/camera-centre");
	assert.equal(hotkeyOf("left"), "stagekit.arrange.left");
	assert.equal(hotkeyOf("nonsense"), undefined);
	assert.equal(hotkeyOf(undefined), undefined);
	assert.equal(TOOLS.length, 14);
	assert.equal(hookPicture(undefined, "game_capture").id, "capture/game");
	assert.equal(hookPicture(undefined, "window_capture").id, "capture/window");
	assert.equal(hookPicture(undefined, undefined).id, "capture/game", "until OBS says which kind it is");
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
