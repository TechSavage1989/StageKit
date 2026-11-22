import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import { AUTO_FRAME_COMMANDS, runAutoFrameCommand } from "../src/auto-frame.ts";
import { AutoFrameCommand } from "../src/actions/auto-frame-command.ts";
import { composed, decodePng } from "../src/compose.ts";
import { layoutList, typeLists } from "../src/key-types.ts";
import { autoFrameCommandShown, COLOURS, laidOut, layersOf } from "../src/pictures.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const bytes = (path: string) => path.startsWith("data:image/png;base64,")
	? Buffer.from(path.slice("data:image/png;base64,".length), "base64")
	: readFileSync(new URL(path, plugin));
const pending = { success: true, requestId: 27, complete: false, status: "pending" };
const measured = { success: true, requestId: 27, complete: true, status: "measured", aspectRatio: "10:9", ratio: 10 / 9 };
const saved = { ...measured, status: "saved", saved: true };
const fakeClock = () => {
	let ms = 0;
	return { now: () => ms, wait: async (delay: number) => { ms += delay; } };
};

test("combined Auto Frame waits for its own fresh save without issuing a second Remember command", async () => {
	const calls: Array<{ name: string; data: Record<string, unknown> }> = [];
	const responses = [pending, pending, saved];
	const send = async (name: string, data: Record<string, unknown>) => { calls.push({ name, data }); return responses.shift(); };
	const result = await runAutoFrameCommand(send, "Captured window", "Frame edges", "measure_and_remember", fakeClock());
	assert.equal(result, saved);
	assert.deepEqual(calls, [
		{ name: "AutoFrameCommand", data: { sourceName: "Captured window", filterName: "Frame edges", action: "measure_and_remember" } },
		{ name: "GetAutoFrameState", data: { sourceName: "Captured window", filterName: "Frame edges", requestId: 27 } },
		{ name: "GetAutoFrameState", data: { sourceName: "Captured window", filterName: "Frame edges", requestId: 27 } },
	]);
});

test("measurement and saving accept only their matching completed status", async () => {
	for (const feature of AUTO_FRAME_COMMANDS) {
		const completed = feature.action === "measure" ? measured : saved;
		assert.equal(await runAutoFrameCommand(async () => completed, "Capture", "Auto Frame", feature.action), completed);
		for (const wrong of [undefined, { ...completed, success: false }, { ...completed, success: undefined },
			{ ...completed, requestId: 0 }, { ...completed, requestId: "27" }, { ...completed, status: "failed" },
			feature.action === "measure" ? saved : measured,
			...(feature.action === "measure" ? [] : [{ ...saved, saved: false }])]) {
			assert.equal(await runAutoFrameCommand(async () => wrong, "Capture", "Auto Frame", feature.action), undefined);
		}
	}
});

test("pending Auto Frame rejects a superseded operation or lost connection", async () => {
	for (const terminal of [{ ...saved, requestId: 28 }, { success: false, requestId: 27, complete: true, status: "failed" }, undefined]) {
		const replies = [pending, terminal];
		assert.equal(await runAutoFrameCommand(async () => replies.shift(), "Capture", "Frame A", "measure_and_remember", fakeClock()), undefined);
	}
});

test("Auto Frame pending feedback has a five-second bound and missing targets send no request", async () => {
	const time = fakeClock();
	const calls: string[] = [];
	const send = async (name: string) => { calls.push(name); return pending; };
	assert.equal(await runAutoFrameCommand(send, "Capture", "Frame A", "measure", time), undefined);
	assert.equal(time.now(), 5000);
	assert.equal(calls.filter((name) => name === "AutoFrameCommand").length, 1);
	assert.ok(calls.filter((name) => name === "GetAutoFrameState").length > 1);
	calls.length = 0;
	assert.equal(await runAutoFrameCommand(send, "", "Frame A", "remember", time), undefined);
	assert.equal(await runAutoFrameCommand(send, "Capture", "", "remember", time), undefined);
	assert.deepEqual(calls, []);
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

test("a withheld initial Auto Frame reply times out and a late acceptance starts no status requests", { timeout: 7000 }, async () => {
	const calls: string[] = [];
	let release: ((reply: Record<string, unknown>) => void) | undefined;
	const result = await runAutoFrameCommand((name) => {
		calls.push(name);
		return new Promise((resolve) => { release = resolve; });
	}, "Capture", "Auto Frame", "measure_and_remember");
	assert.equal(result, undefined);
	assert.deepEqual(calls, ["AutoFrameCommand"]);
	release!(pending);
	await new Promise((resolve) => setTimeout(resolve, 20));
	assert.deepEqual(calls, ["AutoFrameCommand"], "late acceptance cannot start polling or a second command");
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
