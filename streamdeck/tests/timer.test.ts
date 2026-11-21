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

test("the cycle key applies each tap, shows the selected mark, and holds start and stop once", { timeout: 8000 }, async (t) => {
	const { obs } = await import("../src/obs.ts");
	const { TimerKey } = await import("../src/actions/timer.ts");
	let running = false, minute = -2;
	const operations: string[] = [];
	t.mock.method(obs, "stageKit", async (name: string, request: Record<string, unknown>) => {
		assert.equal(name, "TimerControl");
		assert.equal(request.timer, "private");
		const op = String(request.operation);
		operations.push(op);
		if (op === "cycle") minute = minute === 15 ? 30 : minute === 30 ? 45 : minute === 45 ? 0 : 15;
		else if (op === "cycleToggle") running = !running;
		else assert.fail(op);
		return { running, mark: minute };
	});
	let shown = "", state = -1, delivered: (() => void) | undefined;
	const event = {
		action: {
			id: "cycle-test", isKey: () => true,
			setState: async (n: number) => { state = n; },
			setTitle: async (title: string) => { shown = title; delivered?.(); },
			showAlert: async () => assert.fail("Unexpected key alert"),
		},
		payload: { settings: { target: "private", timerPress: "cycle" } },
	};
	const action = new TimerKey() as unknown as { onKeyDown(e: unknown): Promise<void>; onKeyUp(e: unknown): void };
	for (const title of [":15", ":30", ":45", ":00", ":15"]) {
		const applied = new Promise<void>((done) => { delivered = done; });
		await action.onKeyDown(event);
		action.onKeyUp(event);
		await applied;
		assert.equal(shown, title);
		assert.equal(state, 0);
	}
	for (const expected of [1, 0]) {
		const applied = new Promise<void>((done) => { delivered = done; });
		await action.onKeyDown(event);
		await applied;
		action.onKeyUp(event);
		assert.equal(shown, ":15");
		assert.equal(state, expected);
	}
	assert.deepEqual(operations, ["cycle", "cycle", "cycle", "cycle", "cycle", "cycleToggle", "cycleToggle"]);
});
