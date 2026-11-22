// A key with a type and a function: the type's picture with what the key does
// as a badge in its corner (src/compose.ts, tools/make-badges.py).

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import { BADGE_AT, BADGE_H, BADGE_W, BADGES, badgeFile, composed, decodePng, encodePng, type Raster } from "../src/compose.ts";
import { BLUR_STYLES, typeLists } from "../src/key-types.ts";
import {
	blurAllPicture,
	blurAllShown,
	blurBadge,
	sceneBlurShown,
	COLOURS,
	file,
	hookPicture,
	hookShown,
	layersOf,
	moveExtra,
	movePicture,
	moveShown,
	optionPicture,
	optionShown,
	resetStatsShown,
	studioModeShown,
	studioTransitionShown,
	placeBadge,
	shapePicture,
	shapeShown,
	type Shown,
	shownId,
	slidePicture,
	slideShown,
	stateFile,
	stepPicture,
	stepShown,
	toolPicture,
	toolShown,
} from "../src/pictures.ts";
import { TOOLS } from "../src/tools.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const read = (f: string): Raster => decodePng(readFileSync(new URL(f, plugin)));
const fromUrl = (url: string): Raster => {
	assert.ok(url.startsWith("data:image/png;base64,"), url.slice(0, 40));
	return decodePng(Buffer.from(url.slice("data:image/png;base64,".length), "base64"));
};

const STEPS = ["toggle", "next", "back", "hide", "clear"];
const OPTIONS = ["snap", "guides", "limit", "slide", "everyScene", "keepShape"];
const WAYS = ["up-left", "up", "up-right", "left", "centre", "right", "down-left", "down", "down-right"];
const SHAPES = ["auto", "4:3", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1", "64:49"];
const BLURS = typeLists("blur")[0].groups.flatMap((g) => g.entries.map((e) => e.value));
const PLACES = typeLists("sendto")[0].groups.flatMap((g) => g.entries.map((e) => e.value));

/** Every key with a function, each function with a type chosen: what it shows. */
function everyFunction(type = "camera", scene = "gameplay"): Array<[string, Shown]> {
	const out: Array<[string, Shown]> = [["resetstats", resetStatsShown(undefined, type)],
		["studiotransition", studioTransitionShown(undefined, type)],
		["obsstudio off", studioModeShown(false, scene)], ["obsstudio on", studioModeShown(true, scene)]];
	for (const s of STEPS) out.push([`appswitch ${s}`, stepShown(undefined, s, type)]);
	for (const t of TOOLS) out.push([`tool ${t.value}`, toolShown(undefined, t.value, type)]);
	for (const o of OPTIONS) out.push([`option ${o}`, optionShown(undefined, o, type)]);
	for (const w of WAYS) out.push([`slide ${w}`, slideShown(undefined, w, type)]);
	for (const sh of SHAPES) out.push([`shape ${sh}`, shapeShown(undefined, sh, "game")]);
	for (const c of ["auto", "game", "window"]) out.push([`hook ${c}`, hookShown(undefined, "window_capture", "browser", c)]);
	for (const b of ["auto", ...BLURS]) out.push([`blurall ${b}`, blurAllShown(undefined, b, scene)]);
	for (const b of ["auto", ...BLURS]) out.push([`sceneblur ${b}`, sceneBlurShown(undefined, b, scene, "mosaic_voronoi")]);
	for (const p of ["auto", ...PLACES]) out.push([`sendto ${p}`, moveShown(undefined, "stagekit_send_to", {}, { sourceType: type, look: p })!]);
	for (let d = 0; d <= 10; d++) out.push([`sendto #${d}`, moveShown(undefined, "stagekit_send_to", { destination: d }, { sourceType: type })!]);
	for (const extra of [{ linked: "Chat" }, { linked: "Chat", flip_linked: false }, { apply_in: 1 }, { apply_in: 2 }])
		out.push([`placeitem ${JSON.stringify(extra)}`, moveShown(undefined, "stagekit_place_item", extra, { sourceType: type })!]);
	return out;
}

test("every badge a key can ask for is drawn, in every palette", () => {
	const asked = new Set<string>();
	for (const [, shown] of everyFunction()) {
		assert.ok(shown.badge, "a type chosen: what the key does is its badge");
		asked.add(shown.badge!);
		if (shown.extra) asked.add(shown.extra);
	}
	for (const style of [...BLUR_STYLES, ...BLURS]) asked.add(blurBadge(style));
	for (const name of asked) {
		assert.ok(BADGES.includes(name), `${name} is not in src/badges.json`);
		for (const colour of COLOURS) assert.ok(existsSync(new URL(badgeFile(name, colour), plugin)), badgeFile(name, colour));
	}
	for (const name of BADGES)
		for (const colour of COLOURS) {
			const badge = read(badgeFile(name, colour));
			assert.deepEqual([badge.width, badge.height], [BADGE_W, BADGE_H], name);
		}
	// Each function has a badge of its own.
	const distinct = (shown: Shown[]) => new Set(shown.map((s) => s.badge)).size;
	assert.equal(distinct(STEPS.map((s) => stepShown(undefined, s, "game"))), STEPS.length);
	assert.equal(distinct(TOOLS.map((t) => toolShown(undefined, t.value, "game"))), TOOLS.length);
	assert.equal(distinct(OPTIONS.map((o) => optionShown(undefined, o, "game"))), OPTIONS.length);
	assert.equal(distinct(WAYS.map((w) => slideShown(undefined, w, "game"))), WAYS.length);
	assert.equal(distinct(SHAPES.map((s) => shapeShown(undefined, s, "game"))), SHAPES.length);
	assert.equal(distinct(BLURS.map((b) => blurAllShown(undefined, b, "gameplay"))), BLURS.length);
	assert.equal(distinct(PLACES.map((p) => moveShown(undefined, "stagekit_send_to", {}, { sourceType: "camera", look: p })!)), PLACES.length);
	assert.deepEqual(
		[{}, { linked: "Chat" }, { linked: "Chat", flip_linked: false }, { apply_in: 1 }, { apply_in: 2 }].map((s) => moveExtra(s)),
		[undefined, "move-flip", "move-linked", "scene-this", "scene-chosen"],
	);
	assert.equal(placeBadge(9), "spot");
	assert.equal(placeBadge(10), "screen-spot");
	assert.equal(placeBadge(0), "camera-top-left");
});

test("a composed image is a valid PNG: the type's picture, the badge laid over its corner, nothing else changed", () => {
	const all = everyFunction();
	for (const colour of COLOURS)
		for (const [what, shown] of colour === "mono" ? all : all.filter((_, i) => i % 7 === 0)) {
			const twoState = what.startsWith("appswitch toggle") || what.startsWith("option") || what.startsWith("blurall") || what.startsWith("sceneblur");
			for (const state of twoState ? [false, true] : [undefined]) {
				const layers = layersOf(shown, colour, state);
				const out = fromUrl(composed(layers));
				const base = read(layers.base);
				assert.deepEqual([out.width, out.height], [144, 144], what);
				const rects = (["right", "left"] as const)
					.filter((side) => layers[side])
					.map((side) => ({ ...BADGE_AT[side], badge: read(badgeFile(layers[side]!, layers.palette)) }));
				for (let y = 0; y < 144; y++)
					for (let x = 0; x < 144; x++) {
						const o = (y * 144 + x) * 4;
						const inside = rects.find((r) => x >= r.x && x < r.x + BADGE_W && y >= r.y && y < r.y + BADGE_H);
						if (!inside) {
							assert.deepEqual(out.data.subarray(o, o + 4), base.data.subarray(o, o + 4), `${what} ${colour}: ${x},${y} changed`);
							continue;
						}
						const b = ((y - inside.y) * BADGE_W + (x - inside.x)) * 4;
						if (inside.badge.data[b + 3] === 255) assert.deepEqual(out.data.subarray(o, o + 4), inside.badge.data.subarray(b, b + 4), `${what} ${colour}: badge at ${x},${y}`);
					}
				// The on/off mark (top left) is still there on a key with two states.
				if (state !== undefined) {
					const mark = out.data.subarray((19 * 144 + 19) * 4, (19 * 144 + 19) * 4 + 3);
					if (state) assert.ok(mark[1] > 150 && mark[0] < 120, `${what}: the green dot`);
					else assert.ok(mark[0] > 150 && mark[1] < 120, `${what}: the red sign`);
				}
			}
		}
});

test("the PNG reader and writer agree, on the pack's 256-colour and full-colour files", () => {
	for (const f of [stateFile(stepPicture(undefined, "toggle"), true, "neon"), file(toolPicture(undefined, "left"), true, "ember"), "imgs/pack/mono/scenes/02-game-capture.png"]) {
		const image = read(f);
		const again = decodePng(encodePng(image));
		assert.deepEqual([again.width, again.height], [image.width, image.height]);
		assert.ok(Buffer.from(again.data).equals(Buffer.from(image.data)), f);
	}
});

test("Automatic, and every setting from before, shows exactly what it showed before: the same file, no badge", () => {
	for (const auto of [undefined, "", "auto"]) {
		const reset = resetStatsShown(undefined, auto);
		assert.equal(reset.picture.id, "options/reset-stats");
		assert.equal(reset.badge, undefined);
		assert.equal(composed(layersOf(reset, "mono")), "imgs/pack/mono/options/reset-stats.png");
		for (const s of STEPS) {
			const shown = stepShown(undefined, s, auto);
			assert.deepEqual(shown, { picture: stepPicture(undefined, s, auto) }, `appswitch ${s}`);
			assert.equal(composed(layersOf(shown, "mono")), file(shown.picture, true, "mono"), "a file, not a data URL");
		}
		for (const t of TOOLS) assert.deepEqual(toolShown(undefined, t.value, auto), { picture: toolPicture(undefined, t.value) });
		for (const o of OPTIONS) assert.deepEqual(optionShown(undefined, o, auto), { picture: optionPicture(undefined, o) });
		for (const w of WAYS) assert.deepEqual(slideShown(undefined, w, auto), { picture: slidePicture(undefined, w) });
		for (const sh of SHAPES) assert.deepEqual(shapeShown(undefined, sh, auto), { picture: shapePicture(undefined, sh) });
		for (const c of [undefined, "auto", "game", "window"])
			for (const kind of [undefined, "game_capture", "window_capture"]) assert.deepEqual(hookShown(undefined, kind, auto, c), { picture: hookPicture(undefined, kind, auto, c) });
		for (const b of ["auto", ...BLURS]) assert.deepEqual(blurAllShown(undefined, b, auto), { picture: blurAllPicture(undefined, b) });
		for (let d = 0; d <= 10; d++)
			for (const look of [undefined, "auto", ...PLACES])
				for (const kind of ["stagekit_send_to", "stagekit_place_item"]) {
					const settings = { destination: d, linked: "Chat", apply_in: 2 };
					assert.deepEqual(moveShown(undefined, kind, settings, { sourceType: auto, look }), { picture: movePicture(undefined, kind, settings, { look })! });
				}
		// Apply Layout has no Source type: a stray one changes nothing.
		assert.deepEqual(moveShown(undefined, "stagekit_layout", {}, { sourceType: "camera" }), { picture: movePicture(undefined, "stagekit_layout", {}, {})! });
	}
	// An image kept from the old Image list: still shown while no type is chosen.
	assert.equal(toolShown("show-hide/chat", "left").picture.id, "show-hide/chat");
	assert.equal(slideShown("show-hide/chat", "up", "auto").picture.id, "show-hide/chat");
	assert.equal(stepShown("options/clear", "hide", "").picture.id, "options/clear");
	// A Manual Window / Source or Capture Active Window key given a source type in beta155 shows that type, now with its step or capture as the badge.
	assert.deepEqual(stepShown(undefined, "hide", "chat"), { picture: stepPicture(undefined, "hide", "chat"), badge: "hide" });
	assert.equal(hookShown(undefined, "game_capture", "browser").badge, "hook-game");
	assert.equal(hookShown(undefined, "window_capture", "browser").badge, "hook-window");
	// A type chosen wins over an image kept, as on every key.
	assert.equal(toolShown("show-hide/chat", "left", "camera").badge, "align-left");
	// What `settle` compares: a picture with a badge is never taken for a kept image.
	assert.notEqual(shownId(toolShown(undefined, "left", "chat")), toolShown(undefined, "left", "chat").picture.id);
	assert.equal(shownId(toolShown(undefined, "left")), "options/align-left");
});
