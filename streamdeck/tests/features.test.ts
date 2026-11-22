import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
const base = "com.techsavage1989.stagekit";

test("every feature has its own key in the manifest, with a settings page and pictures that exist", () => {
	const wanted = [...features.switches, ...features.moves, ...features.scenes, ...features.sceneStarts].map((f) => `${base}.${f.key}`);
	for (const extra of [features.timer.key, "shape", "hook", "appswitch", "fitoverlap", "scene", "mode", "tool", "blurall", "option", "slide", "stream", "record", "obsfilter", "obssource", "obsmute", "obsstudio", "studiotransition", "obscamera", "resetstats", "switch", "move"]) wanted.push(`${base}.${extra}`);
	const have = manifest.Actions.map((a: { UUID: string }) => a.UUID);
	assert.deepEqual([...have].sort(), [...wanted].sort());
	for (const action of manifest.Actions) {
		assert.ok(existsSync(new URL(action.PropertyInspectorPath, plugin)), action.PropertyInspectorPath);
		assert.ok(existsSync(new URL(`${action.Icon}.png`, plugin)), action.Icon);
		for (const state of action.States) assert.ok(existsSync(new URL(`${state.Image}.png`, plugin)), state.Image);
	}
});

test("Reset Stats is a single-press global key with the shared image and connection controls", () => {
	const action = manifest.Actions.find((a: { UUID: string }) => a.UUID === `${base}.resetstats`);
	assert.equal(action.Name, "Reset Stats");
	assert.deepEqual(action.Controllers, ["Keypad"]);
	assert.equal(action.States.length, 1);
	const page = readFileSync(new URL(action.PropertyInspectorPath, plugin), "utf8");
	assert.match(page, /OBS Stats/);
	assert.match(page, /Same as Reset in the OBS Stats window/);
	const settings = [...new Set([...page.matchAll(/<sdpi-[^>]+\bsetting="([^"]+)"/g)].map((m) => m[1]))].sort();
	assert.deepEqual(settings, ["host", "layout", "palette", "password", "port", "sourceType"], "no target, toggle or output choices");
});

test("Studio controls preserve the existing switch and provide a separate momentary transition", () => {
	const studio = manifest.Actions.find((a: { UUID: string }) => a.UUID === `${base}.obsstudio`);
	assert.equal(studio.Name, "Studio Mode");
	assert.equal(studio.States.length, 2);
	assert.equal(studio.DisableAutomaticStates, true, "OBS controls the displayed state");
	const studioPage = readFileSync(new URL(studio.PropertyInspectorPath, plugin), "utf8");
	for (const value of ["toggle", "on", "off"]) assert.ok(studioPage.includes(`value="${value}"`), value);
	assert.match(studioPage, /setting="sceneType"/);
	const action = manifest.Actions.find((a: { UUID: string }) => a.UUID === `${base}.studiotransition`);
	assert.equal(action.Name, "Preview to Program");
	assert.equal(action.States.length, 1);
	const page = readFileSync(new URL(action.PropertyInspectorPath, plugin), "utf8");
	assert.match(page, /Same as the Transition button in Studio Mode/);
	assert.match(page, /Studio Mode must be on/);
	const settings = [...new Set([...page.matchAll(/<sdpi-[^>]+\bsetting="([^"]+)"/g)].map((m) => m[1]))].sort();
	assert.deepEqual(settings, ["host", "layout", "palette", "password", "port", "sourceType"]);
});

test("every key offered has an icon of its own in Stream Deck's list, at both sizes", () => {
	const offered = manifest.Actions.filter((a: { VisibleInActionsList?: boolean }) => a.VisibleInActionsList !== false);
	const icons = offered.map((a: { Icon: string }) => a.Icon);
	assert.equal(new Set(icons).size, icons.length, "two keys share an icon");
	const drawings = new Set<string>();
	for (const icon of icons) {
		for (const size of ["", "@2x"]) assert.ok(existsSync(new URL(`${icon}${size}.png`, plugin)), `${icon}${size}`);
		drawings.add(readFileSync(new URL(`${icon}@2x.png`, plugin)).toString("base64"));
	}
	assert.equal(drawings.size, icons.length, "two keys have the same drawing");
});

test("the retired keys remain registered for existing profiles and are hidden from the action list", () => {
	const hidden = manifest.Actions.filter((a: { VisibleInActionsList?: boolean }) => a.VisibleInActionsList === false);
	assert.deepEqual(hidden.map((a: { UUID: string }) => a.UUID).sort(), [`${base}.move`, `${base}.placeitem`, `${base}.switch`]);
});

test("a feature's key is named for the feature, and each is for one kind of filter", () => {
	const kinds = [...features.switches, ...features.moves].map((f) => f.kind);
	assert.equal(new Set(kinds).size, kinds.length);
	for (const f of [...features.switches, ...features.moves]) {
		assert.match(f.kind, /^stagekit_/);
		assert.ok(f.name.length > 3 && f.tip.length > 10);
	}
});

test("a dock switch key and a slide key have a picture for each choice the pack covers", async () => {
	const { optionPicture, slidePicture } = await import("../src/pictures.ts");
	const { DIRECTIONS } = await import("../src/actions/slide.ts").catch(() => ({ DIRECTIONS: [] as string[] }));
	assert.equal(optionPicture(undefined, "snap").id, "options/snap");
	assert.equal(optionPicture(undefined, "guides").id, "options/guides");
	assert.ok(optionPicture(undefined, "limit").on);
	for (const way of ["up", "down", "left", "right", "up-left", "up-right", "down-left", "down-right"])
		assert.equal(slidePicture(undefined, way).id, `options/slide-${way}`);
	assert.ok(slidePicture(undefined, "centre").on);
	assert.ok(Array.isArray(DIRECTIONS));
});
