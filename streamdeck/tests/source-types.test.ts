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
	typeOfInput,
	typeOfName,
	typePictureId,
} from "../src/source-types.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const tools = new URL("../tools/", import.meta.url);

const BETA155 = [
	"game", "window", "art_app", "code_editor", "browser", "camera", "capture_card", "display", "phone", "tablet",
	"chat", "music", "alerts", "title", "image", "video", "starting", "brb", "intermission", "ending",
];

test("the types are listed once each, and no id a key may have saved is gone", () => {
	const ids = SOURCE_TYPES.map((t) => t.id);
	assert.equal(new Set(ids).size, ids.length);
	for (const id of BETA155) assert.ok(ids.includes(id), id);
	for (const t of SOURCE_TYPES) assert.match(t.id, /^[a-z0-9_]+$/);
	// The drawing tool's first twenty (each also drawn inside a capture's corners) are those.
	const drawn = readFileSync(new URL("make-option-icons.py", tools), "utf8").match(/SOURCE_TYPES = \[([^\]]+)\]/)?.[1] ?? "";
	assert.deepEqual([...drawn.matchAll(/"([a-z_]+)"/g)].map((m) => m[1]), BETA155);
	assert.ok(isSourceType("camera") && !isSourceType("") && !isSourceType("Camera") && !isSourceType(undefined));
});

test("every type has a wordless picture of its own, on and off, marked, in every palette", () => {
	for (const colour of COLOURS) {
		const looks = new Set<string>();
		for (const type of SOURCE_TYPES) {
			const picture = PICTURES.find((p) => p.id === typePictureId(type.id));
			assert.ok(picture, type.id);
			for (const on of [true, false]) {
				assert.ok(existsSync(new URL(file(picture, on, colour), plugin)), file(picture, on, colour));
				assert.ok(existsSync(new URL(stateFile(picture, on, colour), plugin)), stateFile(picture, on, colour));
			}
			looks.add(readFileSync(new URL(file(picture, true, colour), plugin)).toString("base64"));
		}
		assert.equal(looks.size, SOURCE_TYPES.length, `two types look the same in ${colour}`);
		const hooks = new Set<string>();
		for (const id of BETA155) {
			const hook = PICTURES.find((p) => p.id === hookTypePictureId(id));
			assert.ok(hook, `hook ${id}`);
			hooks.add(readFileSync(new URL(file(hook, true, colour), plugin)).toString("base64"));
		}
		assert.equal(hooks.size, BETA155.length);
	}
	assert.equal(typePictureId("brb"), "options/mode-brb");
	assert.equal(typePictureId("nonsense"), undefined);
	assert.equal(hookTypePictureId("retro"), undefined, "drawn in corners only for the first twenty");
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
	assert.equal(typeOfName("GBA"), "handheld");
	assert.equal(typeOfName("GBA Link 2"), "linked_handheld_2", "three words before two");
	assert.equal(typeOfName("Column: GBA"), "column_handheld", "two words before one");
	assert.equal(typeOfName("Green Screen"), "green_screen", "not a display");
	assert.equal(typeOfName("Side Camera"), "side_camera");
	assert.equal(typeOfName("Second Monitor"), "second_display");
	assert.equal(typeOfName("Capture Card"), "capture_card");
	assert.equal(typeOfName("Main Display"), "display");
	assert.equal(typeOfName("Weekend"), undefined, "whole words only");
	assert.equal(typeOfName(""), undefined);
});

test("the Source type comes first, then an image kept from before, then Automatic, on every key", () => {
	// A Show in Scenes key: the owner's own picture of that type, with the eye.
	assert.equal(chooseFor("options/browser", "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "game")?.id, "show-hide/game-capture");
	assert.equal(chooseFor("options/browser", "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "")?.id, "options/browser");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "game")?.id, "show-hide/game-capture");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "")?.id, "show-hide/square-camera");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "auto")?.id, "show-hide/square-camera");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Studio", "dshow_input", "no-such")?.id, "show-hide/square-camera");
	// Automatic: the pack's picture of that source, then its name's type, then its kind in OBS.
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Square Camera", "dshow_input")?.id, "show-hide/square-camera");
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Capture Card 2", "dshow_input")?.id, "show-hide/hd60s");
	// Any other filter on a source: the type when chosen, its own picture otherwise.
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "Studio", undefined, "art_app")?.id, "options/type-art-app");
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "Studio", "dshow_input")?.id, "blur-types/gaussian");
	assert.equal(chooseFor(undefined, "stagekit_standby", {}, "Studio", undefined, "video")?.id, "options/type-video");
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
	for (const f of features.switches) if (isSceneKind(f.kind)) assert.ok(!offered(f.key), f.key);
	for (const page of ["showinscenes", "shadow", "standby", "blur", "zoom", "mode", "tool", "option", "slide", "shape", "hook", "appswitch", "sendto", "placeitem"])
		assert.ok(offered(page), page);
	for (const page of ["scene", "fitoverlap", "blur-all", "layout"]) assert.ok(!offered(page), page);
	const page = readFileSync(new URL("ui/showinscenes.html", plugin), "utf8");
	for (const type of SOURCE_TYPES)
		assert.equal(page.includes(`<option value="${type.id}">${type.label.replace(/&/g, "&amp;")}</option>`), type.group !== "Activities", type.id);
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
