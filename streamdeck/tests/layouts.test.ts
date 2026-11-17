// A key's Layout: its image alone, in the live layout, or in the live + chat
// layout (src/key-types.ts `layoutList`, src/pictures.ts `laidOut`,
// tools/make-option-icons.py). The owner: "every icon should have a
// standalone version, live/no chat version, and a component/chat version",
// and "perhaps have a selector to choose the layout and a second selector for
// the specific type".

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };
import layouts from "../src/layouts.json" with { type: "json" };
import { composed, decodePng, type Raster } from "../src/compose.ts";
import { keysWithLists, LAYOUTS, layoutList, layoutOf, SCENE_TYPES, SOURCE_TYPES, typeLists } from "../src/key-types.ts";
import {
	chooseFor,
	COLOURS,
	file,
	laidOut,
	layersOf,
	modePicture,
	PICTURES,
	scenePicture,
	type Shown,
	stateFile,
	toolShown,
	stepShown,
	blurAllShown,
} from "../src/pictures.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const root = new URL("../", import.meta.url);
const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));
const KIND = new Map([...features.switches, ...features.moves].map((f) => [f.key, f.kind]));
const pageOf = (key: string) => (key === "blurall" ? "blur-all" : key);
const read = (f: string): Raster => decodePng(readFileSync(new URL(f, plugin)));
const fromUrl = (url: string): Raster => {
	assert.ok(url.startsWith("data:image/png;base64,"), url.slice(0, 40));
	return decodePng(Buffer.from(url.slice("data:image/png;base64,".length), "base64"));
};
const px = (r: Raster, x: number, y: number) => Array.from(r.data.subarray((y * r.width + x) * 4, (y * r.width + x) * 4 + 4));

/** What a Show in Scenes key shows with a Source type and a Layout. */
function showKey(sourceType: string, layout?: string): Shown {
	const picture = chooseFor(undefined, "stagekit_show_in_scenes", {}, "", undefined, { sourceType })!;
	return laidOut({ picture }, { layout, sourceType });
}

/** What a Switch Scene key shows with a Scene type and a Layout. */
function sceneKey(sceneType: string, layout?: string): Shown {
	return laidOut({ picture: scenePicture(undefined, "", sceneType) }, { layout, sceneType });
}

test("every key with a Source type or Scene type list has a Layout list, just before it", () => {
	const keys = keysWithLists();
	assert.ok(keys.length >= 31);
	for (const key of keys) {
		const lists = typeLists(key);
		const typed = lists.find((l) => l.setting === "sourceType" || l.setting === "sceneType");
		const layout = layoutList(key);
		if (!typed) {
			assert.equal(layout, undefined, key);
			continue;
		}
		assert.ok(layout, key);
		assert.equal(layout.setting, "layout");
		assert.equal(layout.label, "Layout");
		assert.equal(layout.automatic, "Automatic");
		const entries = layout.groups.flatMap((g) => g.entries);
		assert.deepEqual(
			entries.map((e) => [e.value, e.label]),
			[
				["standalone", "Standalone"],
				["live", "Live (no chat)"],
				["chat", "Live + chat"],
			],
		);
		// (Not one of the type lists: an image kept from the old Image list is not taken for a layout.)
		assert.ok(!lists.some((l) => l.setting === "layout"), key);
		const page = readFileSync(new URL(`ui/${pageOf(key)}.html`, plugin), "utf8");
		const at = page.indexOf('setting="layout"');
		assert.ok(at > 0, `${key}: the Layout list on its page`);
		assert.ok(at < page.indexOf(`setting="${typed.setting}"`), `${key}: Layout before ${typed.setting}`);
		assert.ok(page.includes('<sdpi-item label="Layout"'), key);
		for (const [value, label] of [
			["auto", "Automatic"],
			["standalone", "Standalone"],
			["live", "Live (no chat)"],
			["chat", "Live + chat"],
		])
			assert.ok(page.includes(`<option value="${value}">${label}</option>`), `${key}: ${value}`);
		// The values, names and pictures of the Layout list do not repeat.
		assert.equal(new Set(entries.map((e) => e.value)).size, entries.length);
		assert.equal(new Set(entries.map((e) => e.label)).size, entries.length);
	}
	assert.equal(layoutList("switch"), undefined);
	assert.equal(layoutList("move"), undefined);
	assert.deepEqual([...LAYOUTS], ["standalone", "live", "chat"]);
	for (const v of [undefined, "", "auto", "nonsense", 3]) assert.equal(layoutOf(v), undefined);
});

test("every layout picture is drawn in all six palettes, marked on and off, with its SVG", () => {
	const listed = PICTURES.filter((p) => p.group === "layouts");
	// Two frames and the two layouts of each drawing.
	assert.ok(listed.length >= 180, `${listed.length}`);
	assert.equal(COLOURS.length, 6);
	for (const palette of COLOURS)
		for (const p of listed) {
			assert.ok(existsSync(new URL(file(p, true, palette), plugin)), `${palette} ${p.id}`);
			assert.ok(existsSync(new URL(stateFile(p, true, palette), plugin)), `${palette} ${p.id} on`);
			assert.ok(existsSync(new URL(stateFile(p, false, palette), plugin)), `${palette} ${p.id} off`);
			if (!p.id.endsWith("-frame")) assert.ok(existsSync(new URL(`svg/keys/${palette}/${p.id}.svg`, root)), `${palette} ${p.id}.svg`);
		}
	for (const mark of ["on", "off"]) assert.ok(existsSync(new URL(`imgs/marks/${mark}.png`, plugin)), mark);
	// Every picture src/layouts.json names is listed.
	for (const pair of Object.values(layouts.byPicture)) for (const id of Object.values(pair)) assert.ok(BY_ID.has(id), id);
	for (const id of [...Object.keys(layouts.glyphOf), ...Object.values(layouts.glyphOf), ...layouts.standalone, ...Object.values(layouts.frames)])
		assert.ok(BY_ID.has(id), id);
});

test("every source type and scene type with a drawing has its own picture in each layout", () => {
	for (const t of SOURCE_TYPES) {
		const drawn = t.base.startsWith("options/");
		for (const layout of ["live", "chat"]) {
			const shown = laidOut({ picture: BY_ID.get(t.base)! }, { layout, sourceType: t.id });
			if (drawn) {
				assert.equal(shown.inset, undefined, `${t.id} ${layout}`);
				assert.ok(shown.picture.id.startsWith("layouts/") || shown.picture.id.startsWith("scenes/"), `${t.id} ${layout}: ${shown.picture.id}`);
			} else assert.equal(shown.inset, layout, `${t.id}: an app's picture laid into the panel`);
		}
	}
	for (const t of SCENE_TYPES)
		for (const layout of ["live", "chat"]) {
			const shown = sceneKey(t.id, layout);
			assert.equal(shown.inset, undefined, `${t.id} ${layout}`);
			assert.ok(shown.picture.id.startsWith("layouts/") || shown.picture.id.startsWith("scenes/0"), `${t.id} ${layout}: ${shown.picture.id}`);
		}
	// Within one layout, a type's picture is its own: no two drawings share one.
	const live = new Set(SOURCE_TYPES.filter((t) => t.base.startsWith("options/")).map((t) => laidOut({ picture: BY_ID.get(t.base)! }, { layout: "live" }).picture.id));
	assert.equal(live.size, new Set(SOURCE_TYPES.filter((t) => t.base.startsWith("options/")).map((t) => t.base)).size);
});

test("a key with a type and a layout shows that type in that layout", () => {
	const cases: Array<[Shown | undefined, string, string]> = [
		// The pack's own layouts, for the game, the capture card and the window.
		[showKey("game", "live"), "scenes/02-game-capture", "show game live"],
		[showKey("game", "chat"), "scenes/03-game-component", "show game chat"],
		[showKey("capture_card", "live"), "scenes/05-game-hd60s", "show capture card live"],
		[showKey("capture_card", "chat"), "scenes/06-game-component-hd60s", "show capture card chat"],
		[showKey("window", "live"), "scenes/07-window-capture", "show window live"],
		[showKey("window", "chat"), "scenes/08-window-component", "show window chat"],
		// Drawn: the Show in Scenes key's art (an eye) is the type's drawing in a layout.
		[showKey("chat"), "show-hide/chat", "show chat automatic"],
		[showKey("chat", "standalone"), "show-hide/chat", "show chat standalone"],
		[showKey("chat", "live"), "layouts/live-type-chat", "show chat live"],
		[showKey("chat", "chat"), "layouts/chat-type-chat", "show chat chat"],
		[showKey("retro", "live"), "layouts/live-type-retro", "show retro live"],
		[showKey("facecam", "chat"), "layouts/chat-type-facecam", "show facecam chat"],
		[showKey("console", "live"), "layouts/live-console", "show console live"],
		[showKey("brb", "chat"), "layouts/chat-mode-brb", "show brb chat"],
		[showKey("scene:art", "live"), "layouts/live-scene-type-art", "a scene type in a Source type list"],
		// A Switch Scene key: a scene that is a layout is its drawing alone when Standalone.
		[sceneKey("gameplay"), "scenes/02-game-capture", "scene gameplay automatic"],
		[sceneKey("gameplay", "standalone"), "options/type-game", "scene gameplay standalone"],
		[sceneKey("gameplay_chat", "standalone"), "options/type-game", "scene gameplay chat standalone"],
		[sceneKey("gameplay_chat", "live"), "scenes/02-game-capture", "scene gameplay chat live"],
		[sceneKey("gameplay", "chat"), "scenes/03-game-component", "scene gameplay chat"],
		[sceneKey("app_chat", "standalone"), "options/type-window", "scene app chat standalone"],
		[sceneKey("console", "standalone"), "scenes/04-console", "scene console standalone: not a layout itself"],
		[sceneKey("console", "chat"), "layouts/chat-console", "scene console chat"],
		[sceneKey("brb", "live"), "layouts/live-mode-brb", "scene brb live"],
		[sceneKey("art", "chat"), "layouts/chat-scene-type-art", "scene art chat"],
		[sceneKey("source:music", "live"), "layouts/live-type-music", "a source type in a Scene type list"],
		// A Scene Mode key: its type drawn as a mode, in a layout by its type.
		[laidOut({ picture: modePicture(undefined, "game") }, { layout: "chat", sourceType: "game" }), "scenes/03-game-component", "mode game chat"],
		[laidOut({ picture: modePicture(undefined, "music") }, { layout: "live", sourceType: "music" }), "layouts/live-type-music", "mode music live"],
	];
	for (const [shown, id, what] of cases) {
		assert.ok(shown, what);
		assert.equal(shown.inset, undefined, what);
		assert.equal(shown.picture.id, id, what);
	}
	// A Blur key's type, blurred in its style, goes by the type in a layout.
	const blur = chooseFor(undefined, "stagekit_ramp_blur", {}, "", undefined, { sourceType: "camera", look: "box" })!;
	assert.equal(laidOut({ picture: blur }, { layout: "live", sourceType: "camera" }).picture.id, "layouts/live-type-camera");
});

test("a key's badges and on/off mark stay on its layout", () => {
	// A tool key with a type: the type in the layout, the tool's badge in its corner.
	const tool = laidOut(toolShown(undefined, "left", "camera"), { layout: "live", sourceType: "camera" });
	assert.deepEqual([tool.picture.id, tool.badge, tool.inset], ["layouts/live-type-camera", "align-left", undefined]);
	const step = laidOut(stepShown(undefined, "hide", "browser"), { layout: "chat", sourceType: "browser" });
	assert.deepEqual([step.picture.id, step.badge], ["layouts/chat-type-browser", "hide"]);
	const blurAll = laidOut(blurAllShown(undefined, "pixelate", "art"), { layout: "chat", sceneType: "art" });
	assert.deepEqual([blurAll.picture.id, blurAll.badge], ["layouts/chat-scene-type-art", "blur-pixelate"]);
	// Two states: the drawn layout picture's own marked copies.
	for (const state of [true, false]) {
		const layers = layersOf(showKey("chat", "live"), "ember", state);
		assert.equal(layers.base, `imgs/pack/ember/status/${state ? "on" : "off"}/layouts/live-type-chat.png`);
		assert.ok(existsSync(new URL(layers.base, plugin)));
	}
	const url = composed(layersOf(tool, "neon"));
	const image = fromUrl(url);
	assert.deepEqual(px(image, 140, 60), px(read("imgs/pack/neon/layouts/live-type-camera.png"), 140, 60), "the layout as drawn outside the badge");
});

test("a picture with no layout of its own is laid into the layout's panel", () => {
	// Automatic type on a Blur key: the blur's own picture in the panel.
	const blur = chooseFor(undefined, "stagekit_ramp_blur", { blur_type: 1 }, "")!;
	for (const layout of ["live", "chat"] as const) {
		const shown = laidOut({ picture: blur }, { layout });
		assert.equal(shown.inset, layout);
		assert.equal(shown.picture.id, blur.id);
		const frame = read(`imgs/pack/mono/layouts/${layout}-frame.png`);
		const image = fromUrl(composed(layersOf(shown, "mono")));
		assert.equal(image.width, 144);
		// The camera box (top right live; the column's top chat) is the frame's.
		const corner = layout === "live" ? [128, 24] : [118, 30];
		assert.deepEqual(px(image, corner[0], corner[1]), px(frame, corner[0], corner[1]), `${layout}: the camera`);
		// The panel holds the picture made smaller: not the empty panel any more.
		const [x0, y0, x1, y1] = layouts.panel[layout];
		let changed = 0;
		for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) if (px(image, x, y).join() !== px(frame, x, y).join()) changed++;
		assert.ok(changed > (x1 - x0) * (y1 - y0) * 0.3, `${layout}: ${changed} pixels of the panel changed`);
		// Outside the panel nothing else changes.
		assert.deepEqual(px(image, 2, 100), px(frame, 2, 100));
	}
	// An app's logo (no drawing): laid in, with the on/off mark over the whole.
	const app = showKey("app_3d", "chat");
	assert.equal(app.inset, "chat");
	const on = fromUrl(composed(layersOf(app, "ocean", true)));
	const off = fromUrl(composed(layersOf(app, "ocean", false)));
	const green = px(on, 19, 19);
	assert.ok(green[1] > 180 && green[0] < 100, `the on mark: ${green}`);
	const red = px(off, 19, 8);
	assert.ok(red[0] > 180 && red[1] < 100, `the off mark: ${red}`);
	// An image kept from the old Image list is laid in too (the key's own picture).
	const kept = chooseFor("blur-pixelate/game-capture", "stagekit_ramp_blur", {}, "")!;
	assert.equal(laidOut({ picture: kept }, { layout: "live" }).inset, "live");
});

test("a key without a Layout looks exactly as before", () => {
	const samples: Shown[] = [
		{ picture: chooseFor(undefined, KIND.get("blur")!, {}, "")! },
		showKey("chat"),
		toolShown(undefined, "left", "camera"),
		{ picture: scenePicture(undefined, "Game Component") },
		blurAllShown(undefined, "box", "gameplay"),
	];
	for (const shown of samples)
		for (const layout of [undefined, "", "auto"]) {
			const settings = layout === undefined ? {} : { layout };
			assert.equal(laidOut(shown, settings), shown, `${shown.picture.id} ${layout}`);
			assert.deepEqual(layersOf(laidOut(shown, settings), "mono"), layersOf(shown, "mono"));
		}
	// Without a badge or a layout, still the file's path as before.
	const plain = { picture: BY_ID.get("blur-types/gaussian")! };
	assert.equal(composed(layersOf(laidOut(plain, {}), "mono")), file(plain.picture, true, "mono"));
	assert.equal(composed(layersOf(laidOut(plain, {}), "mono", true)), stateFile(plain.picture, true, "mono"));
	// Standalone leaves every picture as it is but a scene that is a layout itself.
	assert.equal(laidOut(plain, { layout: "standalone" }), plain);
	assert.equal(showKey("game", "standalone").picture.id, showKey("game").picture.id);
});
