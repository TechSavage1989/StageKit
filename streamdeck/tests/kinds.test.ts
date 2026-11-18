import assert from "node:assert/strict";
import { test } from "node:test";

import {
	afterPress,
	filterItems,
	isMove,
	isOwnName,
	isSwitch,
	shownName,
	sourceItems,
	targetItems,
	targetOf,
	TARGET_SEP,
	type FilterInfo,
} from "../src/kinds.ts";

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
		{ label: "Soft (Blur)", value: "Soft" },
	]);
	assert.deepEqual(filterItems(filters, "camera", isMove), [{ label: "Top right (Move Source)", value: "Top right" }]);
	assert.deepEqual(filterItems(filters, "nobody", isSwitch), []);
});

test("a press switches over, or only on, or only off", () => {
	assert.equal(afterPress(undefined, false), true);
	assert.equal(afterPress("toggle", true), false);
	assert.equal(afterPress("on", true), true);
	assert.equal(afterPress("off", false), false);
});

test("a feature's key chooses from one list: a source by its name, or with the filter's where that is needed", () => {
	const blur = (kind: string) => kind === "stagekit_ramp_blur";
	assert.deepEqual(targetItems(filters, blur), [
		{ label: "camera \u2014 Soft", value: `camera${TARGET_SEP}Soft` },
		{ label: "Game", value: `Game${TARGET_SEP}Ramp Blur` },
	]);
	assert.deepEqual(targetItems(filters, (kind) => kind === "stagekit_fit_item"), [
		{ label: "Scene \u2014 Fit the game", value: `Scene${TARGET_SEP}Fit the game` },
	]);
	assert.deepEqual(targetItems(filters, (kind) => kind === "stagekit_standby"), []);
});

test("a key's target is read from the one setting, or from the two older ones", () => {
	assert.deepEqual(targetOf({ target: `camera${TARGET_SEP}Soft` }), { source: "camera", filter: "Soft" });
	assert.deepEqual(targetOf({ source: "Game", filter: "Ramp Blur" }), { source: "Game", filter: "Ramp Blur" });
	assert.deepEqual(targetOf({ target: `a${TARGET_SEP}b`, source: "old", filter: "older" }), { source: "a", filter: "b" });
	assert.deepEqual(targetOf({}), { source: "", filter: "" });
});

test("a filter's own name is the one OBS shows now, or one it was given under an earlier name", () => {
	assert.equal(shownName("stagekit_fit_item"), "Scene Filter: Fit to Space (StageKit)");
	assert.equal(shownName("stagekit_ramp_blur"), "Source Filter: Blur (StageKit)");
	assert.equal(shownName("stagekit_standby"), "Source Filter: Standby Image (StageKit)");
	assert.equal(shownName("color_filter"), "color_filter");
	assert.equal(isOwnName("Scene Filter: Fit to Space (StageKit)", "stagekit_fit_item"), true);
	assert.equal(isOwnName("Fit Item (StageKit)", "stagekit_fit_item"), true);
	assert.equal(isOwnName("Standby Picture (StageKit)", "stagekit_standby"), true);
	assert.equal(isOwnName("Visibility Link (StageKit)", "stagekit_visibility_link"), true);
	assert.equal(isOwnName("Capture App in Use (StageKit)", "stagekit_app_capture"), true);
	assert.equal(isOwnName("Fit the game", "stagekit_fit_item"), false);
	// Old names are matched whole: a user's own name with a plain word in it is not the kind's.
	assert.equal(isOwnName("Chat Layout", "stagekit_layout"), false);
	assert.equal(isOwnName("Layout (StageKit)", "stagekit_layout"), true);
	assert.equal(isOwnName("My Active App", "stagekit_active_app"), false);
	const named: FilterInfo[] = [
		{ source: "Game", name: "Source Filter: Blur (StageKit)", kind: "stagekit_ramp_blur", enabled: false },
		{ source: "Cam", name: "Standby Picture (StageKit)", kind: "stagekit_standby", enabled: true },
	];
	assert.deepEqual(targetItems(named, (kind) => kind === "stagekit_ramp_blur"), [
		{ label: "Game", value: `Game${TARGET_SEP}Source Filter: Blur (StageKit)` },
	]);
	assert.deepEqual(filterItems(named, "Cam", isSwitch), [
		{ label: "Standby Picture (StageKit)", value: "Standby Picture (StageKit)" },
	]);
});
