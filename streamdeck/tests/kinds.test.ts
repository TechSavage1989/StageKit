import assert from "node:assert/strict";
import { test } from "node:test";

import { afterPress, filterItems, isMove, isSwitch, sourceItems, type FilterInfo } from "../src/kinds.ts";

const filters: FilterInfo[] = [
	{ source: "camera", name: "Top right", kind: "stagekit_send_to", enabled: false },
	{ source: "camera", name: "Shadow, Glow & Outline", kind: "stagekit_shadow_backdrop", enabled: true },
	{ source: "camera", name: "Soft", kind: "stagekit_ramp_blur", enabled: false },
	{ source: "Game", name: "Ramp Blur", kind: "stagekit_ramp_blur", enabled: false },
	{ source: "Scene", name: "Fit the game", kind: "stagekit_fit_item", enabled: true },
	{ source: "Other", name: "Color Correction", kind: "color_filter", enabled: true },
];

test("a move is not a switch, and something that is not StageKit's is neither", () => {
	assert.equal(isMove("stagekit_send_to"), true);
	assert.equal(isSwitch("stagekit_send_to"), false);
	assert.equal(isSwitch("stagekit_ramp_blur"), true);
	assert.equal(isSwitch("color_filter"), false);
	assert.equal(isMove("color_filter"), false);
});

test("the source list holds only sources with a filter of the wanted sort, by name", () => {
	assert.deepEqual(
		sourceItems(filters, isSwitch).map((i) => i.value),
		["camera", "Game", "Scene"],
	);
	assert.deepEqual(
		sourceItems(filters, isMove).map((i) => i.value),
		["camera"],
	);
});

test("the filter list is one source's, and a renamed filter says what it is", () => {
	assert.deepEqual(filterItems(filters, "camera", isSwitch), [
		{ label: "Shadow, Glow & Outline", value: "Shadow, Glow & Outline" },
		{ label: "Soft (Ramp Blur)", value: "Soft" },
	]);
	assert.deepEqual(filterItems(filters, "camera", isMove), [{ label: "Top right (Send To)", value: "Top right" }]);
	assert.deepEqual(filterItems(filters, "nobody", isSwitch), []);
});

test("a press switches over, or only on, or only off", () => {
	assert.equal(afterPress(undefined, false), true);
	assert.equal(afterPress("toggle", true), false);
	assert.equal(afterPress("on", true), true);
	assert.equal(afterPress("off", false), false);
});
