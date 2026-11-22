import assert from "node:assert/strict";
import { test } from "node:test";
import { MARKS, OPERATIONS, timerPress } from "../src/timer.ts";
import { timerCycleTitle } from "../src/titles.ts";

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

test("cycle titles use the saved OBS clock time and keep mark-only support for older plugins", () => {
	for (const mark of [15, 30, 45, 0]) {
		const label = ":" + String(mark).padStart(2, "0");
		assert.equal(timerCycleTitle({ mark, deadlineTime: "9:45 PM" }), `${label}\n9:45 PM`);
		assert.equal(timerCycleTitle({ mark, deadlineTime: "21:45" }), `${label}\n21:45`);
		for (const deadlineTime of [undefined, null, "", "   ", 12345])
			assert.equal(timerCycleTitle({ mark, deadlineTime }), label);
	}
	assert.equal(timerCycleTitle({ mark: 0, deadlineTime: "12:00 AM" }), ":00\n12:00 AM");
	assert.equal(timerCycleTitle({ mark: 45, deadlineTime: " 9:45\u202fPM " }), ":45\n9:45\u202fPM", "preserve the host's time format");
	assert.equal(timerCycleTitle({ mark: -2, deadlineTime: "9:45 PM" }), "Cycle\n9:45 PM");
	for (const mark of [undefined, null, -2, 7, "15", NaN]) assert.equal(timerCycleTitle({ mark }), "Cycle");
	assert.equal(timerCycleTitle(), "Cycle");
});

test("the cycle key applies each tap, shows the selected mark and deadline, and holds start and stop once", { timeout: 8000 }, async (t) => {
	const { obs } = await import("../src/obs.ts");
	const { TimerKey } = await import("../src/actions/timer.ts");
	let running = false, minute = -2, deadlineTime = "";
	const operations: string[] = [];
	t.mock.method(obs, "stageKit", async (name: string, request: Record<string, unknown>) => {
		assert.equal(name, "TimerControl");
		assert.equal(request.timer, "private");
		const op = String(request.operation);
		operations.push(op);
		if (op === "cycle") {
			minute = minute === 15 ? 30 : minute === 30 ? 45 : minute === 45 ? 0 : 15;
			deadlineTime = new Map([[15, "10:15 PM"], [30, "10:30 PM"], [45, "9:45 PM"], [0, "10:00 PM"]]).get(minute)!;
		} else if (op === "cycleToggle") {
			running = !running;
			if (running) deadlineTime = "11:15 PM";
		}
		else assert.fail(op);
		return { running, mark: minute, deadlineTime };
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
	for (const title of [":15\n10:15 PM", ":30\n10:30 PM", ":45\n9:45 PM", ":00\n10:00 PM", ":15\n10:15 PM"]) {
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
		assert.equal(shown, ":15\n11:15 PM", "hold displays the returned deadline without advancing the mark");
		assert.equal(state, expected);
	}
	assert.deepEqual(operations, ["cycle", "cycle", "cycle", "cycle", "cycle", "cycleToggle", "cycleToggle"]);
});

test("cycle keys restore and refresh the selected timer's deadline through normal state events", async (t) => {
	const { obs } = await import("../src/obs.ts");
	const { TimerKey } = await import("../src/actions/timer.ts");
	let current: Record<string, unknown> | undefined = { running: false, mark: 45, deadlineTime: "9:45 PM" };
	let settings = { target: "private", timerPress: "cycle", timerMark: "15", palette: "mono", layout: "auto" };
	let title = "", state = -1, delivered: (() => void) | undefined;
	const images: Array<{ image: string; state?: number }> = [];
	const requests: Record<string, unknown>[] = [];
	t.mock.method(obs, "stageKit", async (name: string, request: Record<string, unknown>) => {
		assert.equal(name, "TimerControl");
		assert.deepEqual(request, { timer: settings.target, operation: "state" });
		requests.push(request);
		return current;
	});
	const key = {
		id: "cycle-state-test", isKey: () => true,
		getSettings: async () => settings,
		setImage: async (image: string, options?: { state?: number }) => { images.push({ image, ...options }); },
		setState: async (value: number) => { state = value; },
		setTitle: async (value: string) => { title = value; delivered?.(); },
	};
	const action = new TimerKey() as unknown as { onWillAppear(e: unknown): Promise<void>; onDidReceiveSettings(e: unknown): Promise<void> };
	Object.defineProperty(action, "actions", { get: () => [key] });
	const event = () => ({ action: key, payload: { settings } });
	await action.onWillAppear(event());
	assert.equal(title, ":45\n9:45 PM");
	assert.equal(state, 0);
	assert.deepEqual(images.map((image) => image.state), [0, 1], "both existing artwork states remain present");
	const refresh = async (eventName: string, timer?: string) => {
		const applied = new Promise<void>((done) => { delivered = done; });
		obs.emit(eventName, timer ? { timer } : undefined);
		await applied;
	};
	current = { running: true, mark: 0, deadlineTime: "12:00 AM" };
	await refresh("timer", "private");
	assert.equal(title, ":00\n12:00 AM");
	assert.equal(state, 1);
	settings = { ...settings, target: "fixture-scene-timer" };
	current = { running: false, mark: 15, deadlineTime: "00:15" };
	await action.onDidReceiveSettings(event());
	assert.equal(title, ":15\n00:15", "changing targets uses the new timer's state");
	const before = requests.length;
	obs.emit("timer", { timer: "unrelated-timer" });
	await new Promise<void>((done) => setImmediate(done));
	assert.equal(requests.length, before, "another timer cannot change this key");
	current = { running: false, mark: 30, deadlineTime: "00:30" };
	await refresh("connection");
	assert.equal(title, ":30\n00:30");
	current = { running: false, mark: 30 };
	await refresh("changed");
	assert.equal(title, ":30", "older servers or a duration timer cannot leave a stale deadline");
	current = undefined;
	await refresh("connection");
	assert.equal(title, "Cycle");
	assert.equal(state, 0);
	settings = { ...settings, timerPress: "set" };
	current = { running: false, mark: 45, deadlineTime: "9:45 PM" };
	await action.onDidReceiveSettings(event());
	assert.equal(title, ":15", "other timer operations keep their existing titles");
});
