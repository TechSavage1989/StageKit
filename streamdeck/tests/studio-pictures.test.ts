import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { composed, decodePng } from "../src/compose.ts";
import { sceneTypePicture } from "../src/key-types.ts";
import { COLOURS, file, laidOut, layersOf, studioModeShown, studioTransitionShown } from "../src/pictures.ts";
import { studioTransitionTitle } from "../src/titles.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const bytes = (path: string) => path.startsWith("data:image/png;base64,")
	? Buffer.from(path.slice("data:image/png;base64,".length), "base64")
	: readFileSync(new URL(path, plugin));

test("Studio Mode has different monitor drawings in every palette and image layout", () => {
	const off = studioModeShown(false);
	const on = studioModeShown(true);
	assert.equal(off.picture.id, "options/studio-off");
	assert.equal(on.picture.id, "options/studio-on");
	// Separate records keep the off-state generator from replacing one monitor with two.
	assert.equal(off.picture.on, off.picture.off);
	assert.equal(on.picture.on, on.picture.off);
	for (const palette of COLOURS) {
		assert.notDeepEqual(decodePng(bytes(file(off.picture, true, palette))).data,
			decodePng(bytes(file(on.picture, true, palette))).data, palette);
		for (const layout of ["auto", "standalone", "live", "chat"]) {
			const offImage = composed(layersOf(laidOut(off, { layout }), palette, false));
			const onImage = composed(layersOf(laidOut(on, { layout }), palette, true));
			assert.notDeepEqual(bytes(offImage), bytes(onImage), `${palette} ${layout}`);
			assert.equal(decodePng(bytes(offImage)).width, 144);
		}
	}
});

test("chosen Studio images retain their type and show the state or transition as a badge", () => {
	const off = studioModeShown(false, "gameplay");
	const on = studioModeShown(true, "gameplay");
	assert.equal(off.picture.id, sceneTypePicture("gameplay"));
	assert.equal(on.picture.id, off.picture.id);
	assert.equal(off.badge, "studio-off");
	assert.equal(on.badge, "studio-on");
	const transition = studioTransitionShown(undefined, "camera");
	assert.equal(transition.badge, "studio-transition");
	assert.equal(studioTransitionShown(undefined).picture.id, "options/studio-transition");
	assert.equal(studioTransitionTitle(), "Preview to\nProgram");
	assert.equal(studioTransitionShown("options/reset-stats").picture.id, "options/reset-stats", "kept images remain usable");
});
