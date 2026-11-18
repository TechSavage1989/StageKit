import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { SOURCE_TYPES, typeLists } from "../src/key-types.ts";
import { modePicture } from "../src/pictures.ts";

// The Scene Modes filter's Mode type in OBS (src/placement/scene-modes.cpp,
// kTypes) is what a Scene Mode key on Automatic goes by.
const cpp = readFileSync(new URL("../../src/placement/scene-modes.cpp", import.meta.url), "utf8");
const block = /const TypeInfo kTypes\[\] = \{([\s\S]*?)\n\};/.exec(cpp);
const obsTypes = [...(block?.[1] ?? "").matchAll(/\{"([a-z0-9_]*)", "Modes\.Type\.\w+"\}/g)].map((m) => m[1]).filter((id) => id !== "");

test("the Scene Modes filter offers every source type the Scene Mode key offers, in the key's order", () => {
	const key = (typeLists("mode")[0] as unknown as { groups: Array<{ entries: Array<{ value: string }> }> }).groups
		.flatMap((g) => g.entries.map((e) => e.value))
		.filter((v) => !v.startsWith("scene:"));
	assert.ok(obsTypes.includes("handheld"));
	assert.deepEqual(obsTypes, key);
});

test("every Mode type set in OBS gives a Scene Mode key its own picture", () => {
	const plain = modePicture(undefined, undefined, undefined, undefined);
	for (const id of obsTypes) {
		assert.ok(SOURCE_TYPES.some((t) => t.id === id), id);
		assert.notEqual(modePicture(undefined, undefined, id, undefined), plain, id);
	}
});
