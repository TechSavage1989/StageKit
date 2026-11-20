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
		assert.equal(layout.label, "Image layout");
		assert.equal(layout.automatic, "Automatic (usual layout)");
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
		// (The Scene Mode key's Layout and image are each mode's own, under Mode settings.)
		const [layoutSetting, typedSetting] = key === "mode" ? ["modeLayout", "modeType"] : ["layout", typed.setting];
		const at = page.indexOf(`setting="${layoutSetting}"`);
		assert.ok(at > 0, `${key}: the Layout list on its page`);
		assert.ok(at < page.indexOf(`setting="${typedSetting}"`), `${key}: Layout before ${typedSetting}`);
		assert.ok(page.includes('<sdpi-item label="Image layout"'), key);
		for (const [value, label] of [
			["auto", "Automatic (usual layout)"],
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
				assert.ok(shown.picture.id.startsWith(`layouts/${layout}-`), `${t.id} ${layout}: ${shown.picture.id}`);
			} else assert.equal(shown.inset, layout, `${t.id}: the pack's picture laid into the panel`);
		}
	}
	for (const t of SCENE_TYPES)
		for (const layout of ["live", "chat"]) {
			const shown = sceneKey(t.id, layout);
			assert.equal(shown.inset, undefined, `${t.id} ${layout}`);
			assert.ok(shown.picture.id.startsWith(`layouts/${layout}-`), `${t.id} ${layout}: ${shown.picture.id}`);
		}
	// Within one layout, a type's picture is its own: no two drawings share one.
	const live = new Set(SOURCE_TYPES.filter((t) => t.base.startsWith("options/")).map((t) => laidOut({ picture: BY_ID.get(t.base)! }, { layout: "live" }).picture.id));
	assert.equal(live.size, new Set(SOURCE_TYPES.filter((t) => t.base.startsWith("options/")).map((t) => t.base)).size);
});

test("a key with a type and a layout shows that type in that layout", () => {
	const cases: Array<[Shown | undefined, string, string]> = [
		// One template for every type, game, capture card and window included (the owner: "they should all be consistent").
		[showKey("game", "live"), "layouts/live-type-game", "show game live"],
		[showKey("game", "chat"), "layouts/chat-type-game", "show game chat"],
		[showKey("capture_card", "live"), "layouts/live-type-capture-card", "show capture card live"],
		[showKey("capture_card", "chat"), "layouts/chat-type-capture-card", "show capture card chat"],
		[showKey("window", "live"), "layouts/live-type-window", "show window live"],
		[showKey("window", "chat"), "layouts/chat-type-window", "show window chat"],
		[showKey("app_3d", "live"), "layouts/live-type-app-3d", "show 3D app live"],
		[showKey("sculpting"), "options/type-sculpting", "show sculpting automatic: no program's letter"],
		[showKey("cad", "chat"), "layouts/chat-type-cad", "show CAD chat"],
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
		[sceneKey("gameplay_chat", "live"), "layouts/live-type-game", "scene gameplay chat live"],
		[sceneKey("gameplay", "chat"), "layouts/chat-type-game", "scene gameplay chat"],
		[sceneKey("app", "live"), "layouts/live-type-window", "scene app live"],
		[sceneKey("app_chat", "standalone"), "options/type-window", "scene app chat standalone"],
		[sceneKey("console", "standalone"), "scenes/04-console", "scene console standalone: not a layout itself"],
		[sceneKey("console", "chat"), "layouts/chat-console", "scene console chat"],
		[sceneKey("brb", "live"), "layouts/live-mode-brb", "scene brb live"],
		[sceneKey("art", "chat"), "layouts/chat-scene-type-art", "scene art chat"],
		[sceneKey("source:music", "live"), "layouts/live-type-music", "a source type in a Scene type list"],
		// A Scene Mode key: its type drawn as a mode, in a layout by its type.
		[laidOut({ picture: modePicture(undefined, "game") }, { layout: "chat", sourceType: "game" }), "layouts/chat-type-game", "mode game chat"],
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
	// A type whose picture is the pack's (the standby image): laid in, with the on/off mark over the whole.
	const app = showKey("standby_image", "chat");
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

test("no type's picture is a program's logo or letter", () => {
	// The owner: "App 3D, or any app type should not have a specific letter or anything. it should only be images relevant to the source".
	const art = JSON.parse(readFileSync(new URL("src/key-types.json", root), "utf8")).art as Record<string, Record<string, string>>;
	const branded = /^filters\/app-|blender|zbrush|fusion-360/;
	for (const t of SOURCE_TYPES) assert.ok(!branded.test(t.base), `${t.id}: ${t.base}`);
	for (const t of SCENE_TYPES) assert.ok(!branded.test(t.picture), `${t.id}: ${t.picture}`);
	for (const [family, pictures] of Object.entries(art))
		for (const [type, picture] of Object.entries(pictures)) assert.ok(!branded.test(picture), `${family} ${type}: ${picture}`);
	for (const key of keysWithLists())
		for (const list of typeLists(key))
			for (const group of list.groups) for (const e of group.entries) assert.ok(!branded.test(e.picture), `${key} ${e.value}: ${e.picture}`);
	for (const t of SOURCE_TYPES.filter((t) => t.group === "Apps")) assert.ok(t.base.startsWith("options/type-"), `${t.id}: a drawing of its own`);
	// A picture chosen by hand from the old Image list still loads.
	for (const id of ["filters/app-blender", "filters/app-zbrush", "filters/app-fusion-360"]) {
		assert.ok(BY_ID.has(id), id);
		assert.equal(chooseFor(id, "stagekit_ramp_zoom", {}, "")?.id, id);
	}
});

/** The middle of what is drawn in a part of a picture: pixels that differ from `ground` by more than a little. */
function middleOfInk(image: Raster, area: number[], ground: number[], skip?: number[]): [number, number] | undefined {
	let x0 = Infinity,
		y0 = Infinity,
		x1 = -Infinity,
		y1 = -Infinity;
	for (let y = area[1]; y < area[3]; y++)
		for (let x = area[0]; x < area[2]; x++) {
			if (skip && x >= skip[0] && y >= skip[1] && x < skip[2] && y < skip[3]) continue;
			const p = px(image, x, y);
			if (Math.abs(p[0] - ground[0]) + Math.abs(p[1] - ground[1]) + Math.abs(p[2] - ground[2]) < 90) continue;
			x0 = Math.min(x0, x);
			y0 = Math.min(y0, y);
			x1 = Math.max(x1, x + 1);
			y1 = Math.max(y1, y + 1);
		}
	return x1 > x0 ? [(x0 + x1) / 2, (y0 + y1) / 2] : undefined;
}

test("every drawing is in the middle of its frame, its panel and its badge", () => {
	// The owner: "all the standalone icons appear to have the images offset to the left. make them all centered. fix the icons universally".
	// The key's little screen: its frame's lines at x 14 and 129, y 17 and 81; the pack's small camera in its bottom right corner.
	const frameMiddle = [(14 + 130) / 2, (17 + 82) / 2];
	let looked = 0;
	for (const palette of ["mono", "ember"]) {
		for (const p of PICTURES.filter((p) => p.id.startsWith("options/"))) {
			const image = read(file(p, true, palette));
			const middle = middleOfInk(image, [18, 21, 127, 78], px(image, 20, 49), [110, 62, 127, 78]);
			assert.ok(middle, `${palette} ${p.id}: something drawn`);
			assert.ok(Math.abs(middle[0] - frameMiddle[0]) <= 2 && Math.abs(middle[1] - frameMiddle[1]) <= 2, `${palette} ${p.id}: middle at ${middle}, the frame's at ${frameMiddle}`);
			looked++;
		}
		for (const p of PICTURES.filter((p) => p.group === "layouts" && !p.id.endsWith("-frame"))) {
			const layout = p.id.startsWith("layouts/live-") ? "live" : "chat";
			const [x0, y0, x1, y1] = layouts.panel[layout];
			const image = read(file(p, true, palette));
			const middle = middleOfInk(image, [x0 + 3, y0 + 3, x1 - 3, y1 - 3], px(image, x0 + 3, y0 + 3));
			assert.ok(middle, `${palette} ${p.id}: something drawn`);
			assert.ok(Math.abs(middle[0] - (x0 + x1) / 2) <= 2 && Math.abs(middle[1] - (y0 + y1) / 2) <= 2, `${palette} ${p.id}: middle at ${middle}`);
			looked++;
		}
		for (const name of JSON.parse(readFileSync(new URL("src/badges.json", root), "utf8")).badges as string[]) {
			const image = read(`imgs/pack/${palette}/badges/${name}.png`);
			const middle = middleOfInk(image, [5, 5, image.width - 5, image.height - 5], px(image, 5, 5));
			assert.ok(middle, `${palette} badge ${name}`);
			assert.ok(Math.abs(middle[0] - image.width / 2) <= 2 && Math.abs(middle[1] - image.height / 2) <= 2, `${palette} badge ${name}: middle at ${middle}`);
			looked++;
		}
	}
	assert.ok(looked > 900, `${looked}`);
	// Every Live picture is one template: the camera top right, the same pixels for every type.
	const frame = read("imgs/pack/mono/layouts/live-frame.png");
	for (const p of PICTURES.filter((p) => p.id.startsWith("layouts/live-"))) {
		const image = read(file(p, true, "mono"));
		for (const [x, y] of [
			[128, 24],
			[125, 70],
			[5, 50],
		])
			assert.deepEqual(px(image, x, y), px(frame, x, y), `${p.id} at ${x}, ${y}`);
	}
});
