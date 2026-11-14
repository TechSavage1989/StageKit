import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
const base = "com.techsavage1989.stagekit";

test("every feature has its own key in the manifest, with a settings page and pictures that exist", () => {
	const wanted = [...features.switches, ...features.moves].map((f) => `${base}.${f.key}`);
	for (const extra of ["shape", "hook", "appswitch", "tool", "blurall", "switch", "move"]) wanted.push(`${base}.${extra}`);
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
