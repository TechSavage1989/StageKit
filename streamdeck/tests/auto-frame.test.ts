import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { AUTO_FRAME_COMMANDS, runAutoFrameCommand } from "../src/auto-frame.ts";
import { AutoFrameCommand } from "../src/actions/auto-frame-command.ts";
import { composed, decodePng } from "../src/compose.ts";
import { layoutList, typeLists } from "../src/key-types.ts";
import { obs } from "../src/obs.ts";
import { autoFrameCommandShown, COLOURS, laidOut, layersOf } from "../src/pictures.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const bytes = (path: string) => path.startsWith("data:image/png;base64,")
	? Buffer.from(path.slice("data:image/png;base64,".length), "base64")
	: readFileSync(new URL(path, plugin));
const pending = { success: true, requestId: 27, complete: false, status: "pending" };
const measured = { success: true, requestId: 27, complete: true, status: "measured", aspectRatio: "10:9", ratio: 10 / 9 };
const saved = { ...measured, status: "saved", saved: true };
const capability = { success: true, supportsRememberBy: true, requestId: 0, complete: true, status: "idle" };
const scoped = (reply: Record<string, unknown>, rememberBy = "window") => ({
	...reply, supportsRememberBy: true, rememberBy,
	rememberTarget: rememberBy === "application" ? "emulator.exe" : "Example game - Emulator",
});
const fakeClock = () => {
	let ms = 0;
	return { now: () => ms, wait: async (delay: number) => { ms += delay; } };
};

test("combined Auto Frame waits for its own fresh save without issuing a second Remember command", async () => {
	for (const rememberBy of ["window", "application"]) {
		const calls: Array<{ name: string; data: Record<string, unknown> }> = [];
		const completed = scoped(saved, rememberBy);
		const responses = [capability, scoped(pending, rememberBy), scoped(pending, rememberBy), completed];
		const send = async (name: string, data: Record<string, unknown>) => { calls.push({ name, data }); return responses.shift(); };
		const result = await runAutoFrameCommand(send, "Captured window", "Frame edges", "measure_and_remember", rememberBy, fakeClock());
		assert.equal(result, completed);
		assert.deepEqual(calls, [
			{ name: "GetAutoFrameState", data: { sourceName: "Captured window", filterName: "Frame edges" } },
			{ name: "AutoFrameCommand", data: { sourceName: "Captured window", filterName: "Frame edges", action: "measure_and_remember", rememberBy } },
			{ name: "GetAutoFrameState", data: { sourceName: "Captured window", filterName: "Frame edges", requestId: 27 } },
			{ name: "GetAutoFrameState", data: { sourceName: "Captured window", filterName: "Frame edges", requestId: 27 } },
		]);
	}
});

test("measurement and saving accept only their matching completed status", async () => {
	for (const feature of AUTO_FRAME_COMMANDS) {
		const completed = feature.action === "measure" ? measured : scoped(saved);
		const sender = (reply: Record<string, unknown> | undefined) => async (name: string, data: Record<string, unknown>) =>
			name === "GetAutoFrameState" && !("requestId" in data) ? capability : reply;
		assert.equal(await runAutoFrameCommand(sender(completed), "Capture", "Auto Frame", feature.action), completed);
		for (const wrong of [undefined, { ...completed, success: false }, { ...completed, success: undefined },
			{ ...completed, requestId: 0 }, { ...completed, requestId: "27" }, { ...completed, status: "failed" },
			feature.action === "measure" ? saved : scoped(measured),
			...(feature.action === "measure" ? [] : [{ ...completed, saved: false }])]) {
			assert.equal(await runAutoFrameCommand(sender(wrong), "Capture", "Auto Frame", feature.action), undefined);
		}
	}
});

test("saving keys default to Window name, support Application, and accept a capability after a previous failure", async () => {
	for (const action of ["remember", "measure_and_remember"] as const) {
		for (const setting of [undefined, "window", "application"]) {
			const rememberBy = setting ?? "window";
			const calls: Array<{ name: string; data: Record<string, unknown> }> = [];
			const completed = scoped(saved, rememberBy);
			const responses = [{ ...capability, success: false, status: "failed" }, scoped(pending, rememberBy), completed];
			const result = await runAutoFrameCommand(async (name, data) => {
				calls.push({ name, data });
				return responses.shift();
			}, "Capture", "Frame A", action, setting, fakeClock());
			assert.equal(result, completed);
			assert.deepEqual(calls[0], { name: "GetAutoFrameState", data: { sourceName: "Capture", filterName: "Frame A" } });
			assert.deepEqual(calls[1], { name: "AutoFrameCommand", data: { sourceName: "Capture", filterName: "Frame A", action, rememberBy } });
			assert.equal(calls.length, 3);
		}
	}
});

test("Measure Bars sends no remember scope and needs no capability preflight", async () => {
	for (const setting of [undefined, "window", "application", "invalid", null]) {
		const calls: Array<{ name: string; data: Record<string, unknown> }> = [];
		const responses = [pending, measured];
		assert.equal(await runAutoFrameCommand(async (name, data) => {
			calls.push({ name, data });
			return responses.shift();
		}, "Capture", "Frame A", "measure", setting, fakeClock()), measured);
		assert.deepEqual(calls, [
			{ name: "AutoFrameCommand", data: { sourceName: "Capture", filterName: "Frame A", action: "measure" } },
			{ name: "GetAutoFrameState", data: { sourceName: "Capture", filterName: "Frame A", requestId: 27 } },
		]);
	}
});

test("saving sends no command without an exact capability or with invalid saved settings", async () => {
	for (const action of ["remember", "measure_and_remember"] as const) {
		for (const state of [undefined, {}, { success: true }, { supportsRememberBy: false }, { supportsRememberBy: "true" }]) {
			const calls: string[] = [];
			assert.equal(await runAutoFrameCommand(async (name) => { calls.push(name); return state; }, "Capture", "Frame A", action), undefined);
			assert.deepEqual(calls, ["GetAutoFrameState"]);
		}
		for (const setting of [null, "", "Application", "title", 1, true, [], {}]) {
			const calls: string[] = [];
			assert.equal(await runAutoFrameCommand(async (name) => { calls.push(name); return capability; }, "Capture", "Frame A", action, setting), undefined);
			assert.deepEqual(calls, []);
		}
	}
});

test("saving rejects missing, mismatched, or changed scope and target echoes", async () => {
	for (const rememberBy of ["window", "application"]) {
		const accepted = scoped(pending, rememberBy);
		const completed = scoped(saved, rememberBy);
		for (const wrong of [
			{ ...completed, rememberBy: undefined },
			{ ...completed, rememberBy: rememberBy === "window" ? "application" : "window" },
			{ ...completed, rememberTarget: undefined },
			{ ...completed, rememberTarget: "" },
		]) {
			for (const responses of [[capability, wrong], [capability, accepted, wrong]])
				assert.equal(await runAutoFrameCommand(async () => responses.shift(), "Capture", "Frame A", "remember", rememberBy, fakeClock()), undefined);
		}
		const changed = [capability, accepted, { ...completed, rememberTarget: "Another target" }];
		assert.equal(await runAutoFrameCommand(async () => changed.shift(), "Capture", "Frame A", "measure_and_remember", rememberBy, fakeClock()), undefined);
	}
});

test("pending Auto Frame rejects a superseded operation or lost connection", async () => {
	for (const terminal of [{ ...scoped(saved), requestId: 28 }, { ...scoped(saved), success: false, status: "failed" }, undefined]) {
		const replies = [capability, scoped(pending), terminal];
		assert.equal(await runAutoFrameCommand(async () => replies.shift(), "Capture", "Frame A", "measure_and_remember", "window", fakeClock()), undefined);
	}
});

test("Auto Frame pending feedback has a five-second bound and missing targets send no request", async () => {
	const time = fakeClock();
	const calls: string[] = [];
	const send = async (name: string) => { calls.push(name); return pending; };
	assert.equal(await runAutoFrameCommand(send, "Capture", "Frame A", "measure", undefined, time), undefined);
	assert.equal(time.now(), 5000);
	assert.equal(calls.filter((name) => name === "AutoFrameCommand").length, 1);
	assert.ok(calls.filter((name) => name === "GetAutoFrameState").length > 1);
	calls.length = 0;
	assert.equal(await runAutoFrameCommand(send, "", "Frame A", "remember", undefined, time), undefined);
	assert.equal(await runAutoFrameCommand(send, "Capture", "", "remember", undefined, time), undefined);
	assert.deepEqual(calls, []);
});

test("capability, command and polling share one deadline and reject a response delivered after it", async () => {
	for (const delays of [[5000], [2500, 2500], [2000, 2000, 1000]]) {
		const time = fakeClock();
		const calls: string[] = [];
		const replies = [capability, scoped(pending), scoped(saved)];
		const result = await runAutoFrameCommand(async (name) => {
			const delay = delays[calls.length];
			assert.notEqual(delay, undefined, "no request starts after the deadline");
			calls.push(name);
			await time.wait(delay);
			return replies.shift();
		}, "Capture", "Frame A", "measure_and_remember", "window", time);
		assert.equal(result, undefined);
		assert.equal(calls.length, delays.length);
		assert.ok(time.now() >= 5000);
	}
});

test("all three Auto Frame commands have distinct images, badges, editable art and shared image choices", () => {
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	for (const palette of COLOURS) {
		const pictures = new Set<string>();
		for (const feature of AUTO_FRAME_COMMANDS) {
			const shown = autoFrameCommandShown(undefined, feature.image);
			assert.equal(shown.picture.id, "options/" + feature.image);
			const plain = bytes(composed(layersOf(shown, palette)));
			pictures.add(plain.toString("base64"));
			assert.equal(decodePng(plain).width, 144);
			const typed = autoFrameCommandShown(undefined, feature.image, "game");
			assert.equal(typed.badge, feature.image);
			for (const layout of ["auto", "standalone", "live", "chat"]) {
				const raster = decodePng(bytes(composed(layersOf(laidOut(typed, { layout }), palette))));
				assert.deepEqual([raster.width, raster.height], [144, 144]);
			}
			const svg = readFileSync(new URL(`../svg/keys/${palette}/options/${feature.image}.svg`, plugin), "utf8");
			assert.match(svg, /inkscape:label="Drawing"/);
			assert.match(readFileSync(new URL(`../svg/badges/${palette}/${feature.image}.svg`, plugin), "utf8"), /<svg/);
		}
		assert.equal(pictures.size, 3, palette + ": separate command drawings");
	}
	for (const feature of AUTO_FRAME_COMMANDS) {
		const action = manifest.Actions.find((a: { UUID: string }) => a.UUID.endsWith("." + feature.key));
		assert.equal(action.Name, feature.name);
		assert.equal(action.States.length, 1);
		assert.ok(typeLists(feature.key).some((list) => list.setting === "sourceType"));
		assert.ok(layoutList(feature.key));
		assert.match(readFileSync(new URL(action.PropertyInspectorPath, plugin), "utf8"), /Source Filter: Auto Frame \(StageKit\)/);
		assert.match(readFileSync(new URL(`../svg/action-icons/${feature.key}.svg`, plugin), "utf8"), /<svg/);
	}
});

test("only the two saving inspectors offer Remember by with Window name as the default", () => {
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	assert.equal(manifest.Version, "0.41.0.0");
	for (const feature of AUTO_FRAME_COMMANDS) {
		const page = readFileSync(new URL(`ui/${feature.key}.html`, plugin), "utf8");
		const settings = [...new Set([...page.matchAll(/<sdpi-[^>]+\bsetting="([^"]+)"/g)].map((match) => match[1]))].sort();
		const common = ["host", "layout", "palette", "password", "port", "sourceType", "target"];
		assert.deepEqual(settings, [...common, ...(feature.action === "measure" ? [] : ["rememberBy"])].sort());
		if (feature.action === "measure") {
			assert.doesNotMatch(page, /setting="rememberBy"/);
		} else {
			assert.match(page, /label="Remember by"/);
			assert.match(page, /setting="rememberBy" default="window"/);
			assert.match(page, /<option value="application">Application<\/option>/);
			assert.match(page, /<option value="window">Window name<\/option>/);
			assert.match(page, /Application sets the default for the app/);
			assert.match(page, /exact title within that app and overrides its default/);
		}
	}
});

test("a withheld initial Auto Frame reply times out and a late acceptance starts no status requests", { timeout: 7000 }, async () => {
	const calls: string[] = [];
	let release: ((reply: Record<string, unknown>) => void) | undefined;
	const result = await runAutoFrameCommand((name) => {
		calls.push(name);
		return new Promise((resolve) => { release = resolve; });
	}, "Capture", "Auto Frame", "measure");
	assert.equal(result, undefined);
	assert.deepEqual(calls, ["AutoFrameCommand"]);
	release!(pending);
	await new Promise((resolve) => setTimeout(resolve, 20));
	assert.deepEqual(calls, ["AutoFrameCommand"], "late acceptance cannot start polling or a second command");
});

test("a withheld capability reply times out and late support never starts a save", { timeout: 7000 }, async () => {
	const calls: string[] = [];
	let release: ((reply: Record<string, unknown>) => void) | undefined;
	const result = await runAutoFrameCommand((name) => {
		calls.push(name);
		return new Promise((resolve) => { release = resolve; });
	}, "Capture", "Auto Frame", "measure_and_remember", "application");
	assert.equal(result, undefined);
	assert.deepEqual(calls, ["GetAutoFrameState"]);
	release!(capability);
	await new Promise((resolve) => setTimeout(resolve, 20));
	assert.deepEqual(calls, ["GetAutoFrameState"]);
});

test("a withheld save acceptance or status reply uses only the time left after preflight", { timeout: 1500 }, async () => {
	for (const phase of ["command", "poll"]) {
		const time = fakeClock();
		const calls: string[] = [];
		let release: ((reply: Record<string, unknown>) => void) | undefined;
		const result = await runAutoFrameCommand(async (name) => {
			calls.push(name);
			if (calls.length === 1) {
				await time.wait(phase === "command" ? 4950 : 2400);
				return capability;
			}
			if (phase === "poll" && name === "AutoFrameCommand") {
				await time.wait(2400);
				return scoped(pending);
			}
			return new Promise<Record<string, unknown>>((resolve) => { release = resolve; });
		}, "Capture", "Auto Frame", "remember", "window", time);
		assert.equal(result, undefined);
		const expected = ["GetAutoFrameState", "AutoFrameCommand", ...(phase === "poll" ? ["GetAutoFrameState"] : [])];
		assert.deepEqual(calls, expected);
		release!(scoped(phase === "command" ? pending : saved));
		await new Promise((resolve) => setTimeout(resolve, 20));
		assert.deepEqual(calls, expected, "late acceptance or completion cannot restart an expired operation");
	}
});

test("saving key presses forward their scope once and keep pending feedback until completion", async (t) => {
	for (const feature of AUTO_FRAME_COMMANDS.filter((feature) => feature.action !== "measure")) {
		for (const rememberBy of [undefined, "window", "application"]) {
			const action = new AutoFrameCommand(feature);
			const calls: Array<{ name: string; data: Record<string, unknown> }> = [];
			let release: ((reply: Record<string, unknown>) => void) | undefined;
			t.mock.method(obs, "stageKit", async (name: string, data: Record<string, unknown>) => {
				calls.push({ name, data });
				if (name === "GetAutoFrameState") return new Promise((resolve) => { release = resolve; });
				return scoped(saved, rememberBy ?? "window");
			});
			let title = "";
			const feedback: string[] = [];
			const event = {
				action: {
					id: feature.key,
					setTitle: async (value: string) => { title = value; },
					showOk: async () => { feedback.push("ok"); },
					showAlert: async () => { feedback.push("alert"); },
				},
				payload: { settings: { source: "Capture", filter: "Auto Frame", rememberBy } },
			};
			const first = action.onKeyDown(event as never);
			await new Promise((resolve) => setImmediate(resolve));
			assert.equal(title, feature.pendingTitle);
			await action.onKeyDown(event as never);
			assert.equal(calls.length, 1, "a second press cannot duplicate a pending save");
			assert.deepEqual(feedback, []);
			release!(capability);
			await first;
			assert.deepEqual(feedback, ["ok"]);
			assert.equal(title, feature.title);
			assert.deepEqual(calls[1], { name: "AutoFrameCommand", data: {
				sourceName: "Capture", filterName: "Auto Frame", action: feature.action, rememberBy: rememberBy ?? "window",
			} });
			assert.equal(calls.length, 2);
			t.mock.restoreAll();
		}
	}
});

test("a saving key alerts without sending a command when OBS has no scope capability", async (t) => {
	const calls: string[] = [];
	t.mock.method(obs, "stageKit", async (name: string) => { calls.push(name); return { success: true }; });
	for (const feature of AUTO_FRAME_COMMANDS.filter((feature) => feature.action !== "measure")) {
		const feedback: string[] = [];
		const key = {
			id: feature.key, setTitle: async () => {},
			showOk: async () => { feedback.push("ok"); },
			showAlert: async () => { feedback.push("alert"); },
		};
		await new AutoFrameCommand(feature).onKeyDown({ action: key, payload: { settings: {
			source: "Capture", filter: "Auto Frame", rememberBy: "application",
		} } } as never);
		assert.deepEqual(feedback, ["alert"]);
	}
	assert.deepEqual(calls, ["GetAutoFrameState", "GetAutoFrameState"]);
});

test("Auto Frame commands keep their selected images offline and report an unsuccessful press", async () => {
	for (const feature of AUTO_FRAME_COMMANDS) {
		const action = new AutoFrameCommand(feature);
		let title = "", image = "";
		const feedback: string[] = [];
		const key = {
			id: feature.key,
			isKey: () => true,
			setImage: async (value: string) => { image = value; },
			setTitle: async (value: string) => { title = value; },
			showOk: async () => { feedback.push("ok"); },
			showAlert: async () => { feedback.push("alert"); },
		};
		await action.onWillAppear({ action: key, payload: { settings: { palette: "ocean", sourceType: "camera", layout: "chat" } } } as never);
		assert.match(image, /^data:image\/png;base64,/);
		assert.equal(title, feature.title);
		await action.onKeyDown({ action: key, payload: { settings: { source: "Capture", filter: "Auto Frame" } } } as never);
		assert.deepEqual(feedback, ["alert"]);
		assert.equal(title, feature.title);
	}
});
