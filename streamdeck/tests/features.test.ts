import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
const base = "com.techsavage1989.stagekit";

test("every feature has its own key in the manifest, with a settings page and pictures that exist", () => {
	const wanted = [...features.switches, ...features.moves].map((f) => `${base}.${f.key}`);
	for (const extra of ["shape", "hook", "appswitch", "tool", "blurall", "option", "slide", "switch", "move"]) wanted.push(`${base}.${extra}`);
	const have = manifest.Actions.map((a: { UUID: string }) => a.UUID);
	assert.deepEqual([...have].sort(), [...wanted].sort());
	for (const action of manifest.Actions) {
		assert.ok(existsSync(new URL(action.PropertyInspectorPath, plugin)), action.PropertyInspectorPath);
		assert.ok(existsSync(new URL(`${action.Icon}.png`, plugin)), action.Icon);
		for (const state of action.States) assert.ok(existsSync(new URL(`${state.Image}.png`, plugin)), state.Image);
	}
});

test("the two catch-all keys are no longer offered, and the keys made with them are still known", () => {
	const hidden = manifest.Actions.filter((a: { VisibleInActionsList?: boolean }) => a.VisibleInActionsList === false);
	assert.deepEqual(hidden.map((a: { UUID: string }) => a.UUID).sort(), [`${base}.move`, `${base}.switch`]);
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
	assert.equal(optionPicture(undefined, "snap").id, "tools/snap");
	assert.equal(optionPicture(undefined, "guides").id, "tools/guides");
	assert.ok(optionPicture(undefined, "limit").on);
	for (const way of ["up", "down", "left", "right", "up-left", "up-right", "down-left", "down-right"])
		assert.equal(slidePicture(undefined, way).id, `tools/slide-${way}`);
	assert.ok(slidePicture(undefined, "centre").on);
	assert.ok(Array.isArray(DIRECTIONS));
});
