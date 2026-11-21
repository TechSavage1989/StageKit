import assert from "node:assert/strict";
import { test } from "node:test";
import { MARKS, OPERATIONS, timerPress } from "../src/timer.ts";

test("timer keys target a named UUID or the private clock and send one set-and-start request", () => {
	for (const target of ["private", "fixture-source-uuid"]) {
		for (const preset of MARKS) {
			for (const [operation] of OPERATIONS) {
				const cmd = timerPress({ target, timerPress: operation, timerMark: preset.value });
				assert.deepEqual(cmd, { timer: target, operation, mark: ["set", "setStart", "toggle"].includes(operation) ? preset.minute : -2 });
			}
		}
	}
	assert.deepEqual(timerPress({ target: "private", timerPress: "cycle" }, true), { timer: "private", operation: "cycleToggle", mark: -2 });
	assert.equal(timerPress({}), undefined);
	assert.equal(timerPress({ target: "private", timerMark: "typo" }), undefined);
	assert.deepEqual(timerPress({ target: "private" }), { timer: "private", operation: "toggle", mark: -1 });
});
