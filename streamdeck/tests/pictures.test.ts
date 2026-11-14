import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { test } from "node:test";

import { automatic, choose, COLOURS, file, pictureItems, PICTURES, plain, toolPicture } from "../src/pictures.ts";
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
	assert.equal(automatic("stagekit_send_to", { destination: 9 }), undefined, "a saved spot has no picture");
	assert.equal(automatic("stagekit_standby")?.id, "filters/standby-picture");
	assert.equal(automatic("stagekit_fit_item"), undefined);
	assert.equal(automatic("color_filter"), undefined);
});

test("a picture chosen by name wins; an unknown name falls back to the fitting one", () => {
	assert.equal(choose("show-hide/chat", "stagekit_ramp_blur")?.id, "show-hide/chat");
	assert.equal(choose("no/such", "stagekit_corners")?.id, "corners/round");
	assert.equal(choose("", "stagekit_fit_item"), undefined);
});

test("what the pack has no picture of gets a plain key, and tools get theirs", () => {
	assert.equal(plain("switch").on, "plain/switch-on");
	assert.equal(plain("switch").off, "plain/switch-off");
	assert.equal(plain("move").on, "plain/move");
	assert.equal(toolPicture(undefined, "same_everywhere").id, "tools/copy-transform-all");
	assert.equal(toolPicture(undefined, "left").id, "plain/tool");
	assert.equal(toolPicture("camera/camera-centre", "left").id, "camera/camera-centre");
	assert.equal(hotkeyOf("left"), "stagekit.arrange.left");
	assert.equal(hotkeyOf("nonsense"), undefined);
	assert.equal(hotkeyOf(undefined), undefined);
	assert.equal(TOOLS.length, 14);
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
