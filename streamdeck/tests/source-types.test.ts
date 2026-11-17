// The Source type option: a picture for what a source is, chosen in any key
// that acts on a source, the same way for every key.

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };
import { isSceneKind } from "../src/kinds.ts";
import { chooseFor, COLOURS, file, hookPicture, inputPicture, PICTURES, stateFile, stepPicture } from "../src/pictures.ts";
import { remembered } from "../src/seen.ts";
import {
	hookTypePictureId,
	isSourceType,
	SOURCE_TYPES,
	sourceTypeItems,
	typeOfInput,
	typeOfName,
	typePictureId,
} from "../src/source-types.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const tools = new URL("../tools/", import.meta.url);

test("the types are listed once each, in the order asked for, with ids that never change", () => {
	assert.deepEqual(
		SOURCE_TYPES.map((t) => t.id),
		[
			"game",
			"window",
			"art_app",
			"code_editor",
			"browser",
			"camera",
			"capture_card",
			"display",
			"phone",
			"tablet",
			"chat",
			"music",
			"alerts",
			"title",
			"image",
			"video",
			"starting",
			"brb",
			"intermission",
			"ending",
		],
	);
	assert.equal(sourceTypeItems()[0].label, "Automatic");
	assert.equal(sourceTypeItems()[0].value, "");
	assert.equal(sourceTypeItems().length, SOURCE_TYPES.length + 1);
	// The drawing tool lists the same ids, in the same order.
	const drawn = readFileSync(new URL("make-option-icons.py", tools), "utf8").match(/SOURCE_TYPES = \[([^\]]+)\]/)?.[1] ?? "";
	assert.deepEqual([...drawn.matchAll(/"([a-z_]+)"/g)].map((m) => m[1]), SOURCE_TYPES.map((t) => t.id));
	assert.ok(isSourceType("camera") && !isSourceType("") && !isSourceType("Camera") && !isSourceType(undefined));
});

test("every type has a wordless picture of its own, on and off, marked, in every palette", () => {
	for (const colour of COLOURS) {
		const looks = new Set<string>();
		const hooks = new Set<string>();
		for (const type of SOURCE_TYPES) {
			const picture = PICTURES.find((p) => p.id === typePictureId(type.id));
			assert.ok(picture, type.id);
			assert.notEqual(picture.on, picture.off, `${type.id}: an on and an off look`);
			for (const on of [true, false]) {
				assert.ok(existsSync(new URL(file(picture, on, colour), plugin)), file(picture, on, colour));
				assert.ok(existsSync(new URL(stateFile(picture, on, colour), plugin)), stateFile(picture, on, colour));
			}
			looks.add(readFileSync(new URL(file(picture, true, colour), plugin)).toString("base64"));
			const hook = PICTURES.find((p) => p.id === hookTypePictureId(type.id));
			assert.ok(hook, `hook ${type.id}`);
			assert.ok(existsSync(new URL(file(hook, true, colour), plugin)));
			hooks.add(readFileSync(new URL(file(hook, true, colour), plugin)).toString("base64"));
		}
		assert.equal(looks.size, SOURCE_TYPES.length, `two types look the same in ${colour}`);
		assert.equal(hooks.size, SOURCE_TYPES.length, `two hook types look the same in ${colour}`);
	}
	// The last four are the holding modes' own pictures.
	assert.equal(typePictureId("brb"), "options/mode-brb");
	assert.equal(typePictureId("nonsense"), undefined);
});

test("Automatic goes by what OBS says the source is", () => {
	const cases: Record<string, string> = {
		game_capture: "game",
		window_capture: "window",
		monitor_capture: "display",
		xshm_input: "display",
		dshow_input: "camera",
		av_capture_input_v2: "camera",
		v4l2_input: "camera",
		browser_source: "browser",
		text_gdiplus_v3: "title",
		text_ft2_source_v2: "title",
		image_source: "image",
		ffmpeg_source: "video",
		vlc_source: "video",
	};
	for (const [kind, type] of Object.entries(cases)) {
		assert.equal(typeOfInput(kind), type, kind);
		assert.equal(inputPicture(kind)?.id, typePictureId(type), kind);
	}
	assert.equal(typeOfInput("color_source_v3"), undefined);
	assert.equal(typeOfInput(undefined), undefined);
});

test("a name speaks of a type by its words, the longest phrase first", () => {
	assert.equal(typeOfName("Be Right Back"), "brb");
	assert.equal(typeOfName("BRB Text"), "brb", "brb before text");
	assert.equal(typeOfName("Stream Starting Soon"), "starting");
	assert.equal(typeOfName("GBA"), "game");
	assert.equal(typeOfName("Capture Card"), "capture_card");
	assert.equal(typeOfName("Main Display"), "display");
	assert.equal(typeOfName("Weekend"), undefined, "whole words only");
	assert.equal(typeOfName(""), undefined);
});

test("an image chosen by hand comes first, then the Source type, then Automatic, on every key", () => {
	// A Show in Scenes key.
	assert.equal(chooseFor("options/browser", "stagekit_show_in_scenes", {}, "Overhead", "dshow_input", "game")?.id, "options/browser");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Overhead", "dshow_input", "game")?.id, "options/type-game");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Overhead", "dshow_input", "")?.id, "options/type-camera");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Overhead", "dshow_input", "no-such")?.id, "options/type-camera");
	// Automatic: the pack's picture of that source, then its name's type, then its kind in OBS.
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Square Camera", "dshow_input")?.id, "show-hide/square-camera");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Capture Card 2", "dshow_input")?.id, "options/type-capture-card");
	// Any other filter on a source: the type when chosen, its own picture otherwise.
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "Overhead", undefined, "art_app")?.id, "options/type-art-app");
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "Overhead", "dshow_input")?.id, "blur-types/gaussian");
	assert.equal(chooseFor(undefined, "stagekit_standby", {}, "Overhead", undefined, "video")?.id, "options/type-video");
	// A filter on a scene has no source: no type, no input kind.
	for (const kind of ["stagekit_fit_item", "stagekit_scene_fit", "stagekit_scene_shadow", "stagekit_scene_corners"]) {
		assert.ok(isSceneKind(kind), kind);
		assert.equal(chooseFor(undefined, kind, {}, "Live", "dshow_input", "camera")?.id.startsWith("options/type"), false, kind);
	}
	assert.equal(chooseFor(undefined, "stagekit_scene_fit", {}, "Live")?.id, "options/scene-fit");
	assert.equal(chooseFor(undefined, "stagekit_scene_shadow", {}, "Live")?.id, "options/scene-shadow");
	assert.equal(chooseFor(undefined, "stagekit_scene_corners", {}, "Live")?.id, "options/scene-corners");
	assert.equal(chooseFor(undefined, "stagekit_scene_standby", {}, "Live")?.group, "filters", "pictured as the standby filter");
	// Capture Active Window: the type inside the hook's corners.
	assert.equal(hookPicture(undefined, "game_capture", "browser").id, "options/hook-type-browser");
	assert.equal(hookPicture(undefined, "window_capture", "").id, "options/hook-window");
	assert.equal(hookPicture("options/display", "window_capture", "browser").id, "options/display");
	// Manual Window / Source.
	assert.equal(stepPicture(undefined, "hide", "chat").id, "options/type-chat");
	assert.equal(stepPicture(undefined, "hide", "").id, "options/hide");
	assert.equal(stepPicture("options/clear", "hide", "chat").id, "options/clear");
});

test("scene filters' keys are marked as such, and only keys on a source offer a Source type", () => {
	for (const f of features.switches) assert.equal((f as { scene?: boolean }).scene === true, isSceneKind(f.kind), f.kind);
	const offered = (page: string) => readFileSync(new URL(`ui/${page}.html`, plugin), "utf8").includes('setting="sourceType"');
	for (const f of features.switches) assert.equal(offered(f.key), !isSceneKind(f.kind), f.key);
	for (const page of ["hook", "appswitch", "mode"]) assert.ok(offered(page), page);
	for (const page of ["scene", "tool", "option", "slide", "fitoverlap", "blur-all", "shape"]) assert.ok(!offered(page), page);
	const page = readFileSync(new URL("ui/showinscenes.html", plugin), "utf8");
	for (const type of SOURCE_TYPES) assert.ok(page.includes(`<option value="${type.id}">${type.label}</option>`), type.id);
});

test("a key remembers the kind of source and the mode its image depends on", async () => {
	let stored: Record<string, unknown> = {};
	const key = { setSettings: async (s: Record<string, unknown>) => void (stored = s) };
	const seen = await remembered(key, { target: "Cam" }, "Cam\nShow", { kind: "stagekit_show_in_scenes", enabled: true, inputKind: "dshow_input" });
	assert.equal(seen?.inputKind, "dshow_input");
	const later = await remembered(key, stored as never, "Cam\nShow", undefined);
	assert.equal(later?.inputKind, "dshow_input", "while OBS cannot be asked");
	const mode = await remembered(key, {}, "S\u001fF\u001fm1", { kind: "stagekit_scene_modes", enabled: false, name: "Game", type: "game" });
	assert.deepEqual([mode?.name, mode?.type], ["Game", "game"]);
	const plain = await remembered(key, {}, "x", { kind: "stagekit_ramp_blur", enabled: true });
	assert.ok(!("inputKind" in (plain ?? {})) && !("name" in (plain ?? {})), "nothing kept that the key did not learn");
});
