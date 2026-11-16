import assert from "node:assert/strict";
import { test } from "node:test";

import { remembered } from "../src/seen.ts";

test("a key keeps what OBS last said, for while OBS cannot be asked", async () => {
	let stored: Record<string, unknown> | undefined;
	let writes = 0;
	const key = {
		setSettings: async (settings: Record<string, unknown>) => {
			stored = settings;
			writes++;
		},
	};
	// OBS answers: it is kept, with only what an image or a name depends on.
	const first = await remembered(key, { target: "Cam" }, "Cam\nBlur", {
		kind: "stagekit_ramp_blur",
		enabled: true,
		settings: { blur_type: 3, also: ["a", "b"], strength: 40 },
	});
	assert.deepEqual(first, { for: "Cam\nBlur", kind: "stagekit_ramp_blur", enabled: true, settings: { blur_type: 3 } });
	assert.equal(writes, 1);
	assert.equal((stored as { target: string }).target, "Cam", "the key's own settings stay");
	// OBS cannot say: the key goes by what it kept.
	const later = await remembered(key, stored as never, "Cam\nBlur", undefined);
	assert.equal(later?.kind, "stagekit_ramp_blur");
	assert.equal(writes, 1, "nothing is written while nothing is learned");
	// The same answer again is not written again.
	await remembered(key, stored as never, "Cam\nBlur", { kind: "stagekit_ramp_blur", enabled: true, settings: { blur_type: 3 } });
	assert.equal(writes, 1);
	// Pointed at something else, what was kept is not for it.
	assert.equal(await remembered(key, stored as never, "Cam\nShadow", undefined), undefined);
	// A key never told anything has nothing to go by.
	assert.equal(await remembered(key, {}, "Cam\nBlur", undefined), undefined);
});
