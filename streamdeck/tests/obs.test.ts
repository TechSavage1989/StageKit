// The link to OBS, against a stand-in for OBS's WebSocket server that speaks
// just enough of the protocol (obs-websocket 5, JSON).

import assert from "node:assert/strict";
import { once } from "node:events";
import { after, before, test } from "node:test";

import { WebSocketServer } from "ws";

import { obs } from "../src/obs.ts";

type Filter = { filterName: string; filterKind: string; filterEnabled: boolean };
const filters: Record<string, Filter[]> = {
	camera: [
		{ filterName: "Top right", filterKind: "stagekit_send_to", filterEnabled: false },
		{ filterName: "Ramp Blur", filterKind: "stagekit_ramp_blur", filterEnabled: false },
		{ filterName: "Colour", filterKind: "color_filter", filterEnabled: true },
	],
	"Game Scene": [{ filterName: "Fit the game", filterKind: "stagekit_fit_item", filterEnabled: true }],
	"A Group": [],
	// Not an input of the stand-in's list, so not in the filter lists above.
	DSLR: [
		{ filterName: "Standby Image", filterKind: "stagekit_standby", filterEnabled: true },
		{ filterName: "Show in Scenes-Camera", filterKind: "stagekit_show_in_scenes", filterEnabled: true },
	],
};

// The DSLR's Standby Image over it on purpose, and the presses StageKit was asked for.
let dslrCovered = false;
const coverRequests: Array<Record<string, unknown>> = [];
const coverEvents: Array<Record<string, unknown>> = [];
const showPresses: Array<Record<string, unknown>> = [];

function standbyAndShow(request: string, data: Record<string, unknown>): Record<string, unknown> {
	if (request === "ShowInScenesPress") {
		showPresses.push(data);
		const found = filters[String(data.sourceName)]?.find((f) => f.filterName === data.filterName && f.filterKind === "stagekit_show_in_scenes");
		if (!found) return { success: false, error: "No Show in Scenes filter of that name on the source" };
		found.filterEnabled = data.how === "toggle" ? !found.filterEnabled : data.how === "on";
		return { success: true, enabled: found.filterEnabled };
	}
	if (data.sourceName !== "DSLR") return { success: false, error: "The source has no Standby Image filter" };
	if (request === "StandbyCover") {
		coverRequests.push(data);
		const next = data.how === "toggle" ? !dslrCovered : data.how === "on";
		if (next !== dslrCovered) coverEvents.push({ sourceName: "DSLR", filterName: "Standby Image", covered: next });
		dslrCovered = next;
	}
	return { success: true, filterName: "Standby Image", covered: dslrCovered, enabled: true };
}

const pressed: string[] = [];
let blurred = false;

// Scene Modes filters, as the StageKit plugin answers for them (modes.md §7).
type ModeFilter = { scene: string; filter: string; enabled: boolean; current: string; previous: string; modes: Array<{ id: string; name: string; type: string }> };
const modeFilters: ModeFilter[] = [
	{
		scene: "Standby",
		filter: "Standby modes",
		enabled: true,
		current: "m1",
		previous: "",
		modes: [
			{ id: "m1", name: "Starting", type: "starting" },
			{ id: "m2", name: "Be Right Back", type: "" },
			{ id: "m3", name: "Lunch", type: "" },
		],
	},
	{ scene: "Chat Live", filter: "Captures", enabled: true, current: "", previous: "", modes: [{ id: "c1", name: "Game", type: "game" }] },
	{ scene: "Chat Live", filter: "Column", enabled: true, current: "", previous: "", modes: [{ id: "k1", name: "GBA", type: "" }] },
];
const modeRequests: Array<Record<string, unknown>> = [];
let modeEvent: Record<string, unknown> | undefined;

function sceneModes(request: string, data: Record<string, unknown>): Record<string, unknown> {
	if (request === "ListSceneModes")
		return { success: true, filters: modeFilters.filter((f) => !data.scene || f.scene === data.scene) };
	const f = modeFilters.find((m) => m.scene === data.scene && (!data.filter || m.filter === data.filter));
	if (!f) return { success: false, error: "No Scene Modes on that scene" };
	if (request === "GetSceneMode") return { success: true, ...f };
	modeRequests.push(data);
	const was = f.current;
	if (data.how === "next" || data.how === "none") {
		// (As the filter's How::Next: the mode after the one on, round again; from none, the first.)
		const at = f.modes.findIndex((m) => m.id === was);
		f.current = data.how === "none" ? "" : f.modes[(at + 1) % f.modes.length].id;
		f.previous = was;
		modeEvent = { scene: f.scene, filter: f.filter, mode: f.current, previous: was };
		return { success: true };
	}
	const mode = f.modes.find((m) => m.id === data.mode || m.name === data.mode);
	if (!mode) return { success: false, error: "No such mode" };
	f.current = data.how === "toggle" && was === mode.id ? f.previous : mode.id;
	f.previous = was;
	modeEvent = { scene: f.scene, filter: f.filter, mode: f.current, previous: was };
	return { success: true };
}

// Scene Blurs, as the StageKit plugin answers for them: one name on two
// scenes, another on one.
const sceneBlurs = [
	{ scene: "Live", filter: "Blur", type: "gaussian", style: "gaussian", active: false },
	{ scene: "Chat Live", filter: "Blur", type: "gaussian", style: "gaussian", active: false },
	{ scene: "Standby", filter: "Mosaic", type: "pixelate", style: "mosaic_voronoi", active: false },
];
const blurRequests: Array<Record<string, unknown>> = [];
const blurEvents: Array<Record<string, unknown>> = [];

function sceneBlur(request: string, data: Record<string, unknown>): Record<string, unknown> {
	if (request === "ListSceneBlurs") {
		const names = [...new Set(sceneBlurs.map((b) => b.filter))];
		return {
			success: true,
			filters: names.map((filter) => {
				const mine = sceneBlurs.filter((b) => b.filter === filter);
				return { filter, type: mine[0].type, style: mine[0].style, active: mine.some((b) => b.active), scenes: mine.map((b) => ({ scene: b.scene, active: b.active })) };
			}),
			blurs: sceneBlurs,
		};
	}
	blurRequests.push(data);
	const mine = sceneBlurs.filter((b) => b.filter === data.filter && (!data.scene || b.scene === data.scene));
	if (mine.length === 0) return { success: false, error: "No scene has a Scene Blur with that name" };
	const anyOn = mine.some((b) => b.active);
	const active = data.how === "on" || (data.how === "toggle" && !anyOn);
	for (const b of mine) {
		if (b.active !== active) blurEvents.push({ scene: b.scene, filter: b.filter, active });
		b.active = active;
	}
	return { success: true, active, blurs: mine };
}

function answer(requestType: string, data: Record<string, unknown> = {}): { ok: boolean; data?: unknown } {
	const list = filters[String(data.sourceName)];
	switch (requestType) {
		case "GetInputList":
			return {
				ok: true,
				data: {
					inputs: [
						{ inputName: "camera", unversionedInputKind: "dshow_input" },
						{ inputName: "Window", unversionedInputKind: "window_capture" },
						{ inputName: "Game", unversionedInputKind: "game_capture" },
					],
				},
			};
		case "GetSceneList":
			return { ok: true, data: { scenes: [{ sceneName: "Game Scene" }] } };
		case "GetGroupList":
			return { ok: true, data: { groups: ["A Group"] } };
		case "GetSourceFilterList":
			return list ? { ok: true, data: { filters: list } } : { ok: false };
		case "GetSourceFilter": {
			const found = list?.find((f) => f.filterName === data.filterName);
			return found ? { ok: true, data: found } : { ok: false };
		}
		case "SetSourceFilterEnabled": {
			const found = list?.find((f) => f.filterName === data.filterName);
			if (!found) return { ok: false };
			found.filterEnabled = data.filterEnabled === true;
			return { ok: true };
		}
		case "GetTransitionKindList":
			return { ok: true, data: { transitionKinds: ["cut_transition", "fade_transition", "swipe_transition"] } };
		case "TriggerHotkeyByName":
			pressed.push(String(data.hotkeyName));
			return { ok: true };
		case "CallVendorRequest": {
			if (data.vendorName !== "StageKit") return { ok: false };
			if (data.requestType === "ListSceneBlurs" || data.requestType === "SceneBlur") {
				const responseData = sceneBlur(String(data.requestType), (data.requestData ?? {}) as Record<string, unknown>);
				return { ok: true, data: { vendorName: "StageKit", requestType: data.requestType, responseData } };
			}
			if (String(data.requestType).includes("SceneMode")) {
				const responseData = sceneModes(String(data.requestType), (data.requestData ?? {}) as Record<string, unknown>);
				return { ok: true, data: { vendorName: "StageKit", requestType: data.requestType, responseData } };
			}
			if (["StandbyCover", "GetStandbyCover", "ShowInScenesPress"].includes(String(data.requestType))) {
				const responseData = standbyAndShow(String(data.requestType), (data.requestData ?? {}) as Record<string, unknown>);
				return { ok: true, data: { vendorName: "StageKit", requestType: data.requestType, responseData } };
			}
			if (data.requestType === "BlurAll") {
				blurred = !blurred;
				return { ok: true, data: { vendorName: "StageKit", requestType: "BlurAll", responseData: { success: true, active: blurred } } };
			}
			return { ok: true, data: { vendorName: "StageKit", requestType: data.requestType, responseData: { success: false, error: "no such request" } } };
		}
		default:
			return { ok: false };
	}
}

let server: WebSocketServer;
let port = 0;

before(async () => {
	server = new WebSocketServer({ port: 0, handleProtocols: () => "obswebsocket.json" });
	await once(server, "listening");
	port = (server.address() as { port: number }).port;
	server.on("connection", (socket) => {
		const send = (op: number, d: unknown) => socket.send(JSON.stringify({ op, d }));
		send(0, { obsWebSocketVersion: "5.5.0", rpcVersion: 1 });
		socket.on("message", (raw) => {
			const { op, d } = JSON.parse(String(raw));
			const status = (ok: boolean) => ({ result: ok, code: ok ? 100 : 600 });
			if (op === 1) send(2, { negotiatedRpcVersion: 1 });
			if (op === 6) {
				const got = answer(d.requestType, d.requestData);
				send(7, {
					requestType: d.requestType,
					requestId: d.requestId,
					requestStatus: status(got.ok),
					responseData: got.data,
				});
				if (got.ok && d.requestType === "SetSourceFilterEnabled")
					send(5, { eventType: "SourceFilterEnableStateChanged", eventIntent: 32, eventData: d.requestData });
				for (const event of coverEvents.splice(0)) {
					const eventData = { vendorName: "StageKit", eventType: "StandbyCoverChanged", eventData: event };
					setTimeout(() => send(5, { eventType: "VendorEvent", eventIntent: 512, eventData }), 5);
				}
				for (const event of blurEvents.splice(0)) {
					// As each filter says it from its next tick.
					const eventData = { vendorName: "StageKit", eventType: "SceneBlurChanged", eventData: event };
					setTimeout(() => send(5, { eventType: "VendorEvent", eventIntent: 512, eventData }), 5);
				}
				if (modeEvent) {
					// As the filter says it from its next tick.
					const eventData = { vendorName: "StageKit", eventType: "SceneModeChanged", eventData: modeEvent };
					modeEvent = undefined;
					setTimeout(() => send(5, { eventType: "VendorEvent", eventIntent: 512, eventData }), 5);
				}
			}
			if (op === 8)
				send(9, {
					requestId: d.requestId,
					results: d.requests.map((r: { requestType: string; requestData: Record<string, unknown> }) => {
						const got = answer(r.requestType, r.requestData);
						return { requestType: r.requestType, requestStatus: status(got.ok), responseData: got.data };
					}),
				});
		});
	});
});

after(() => {
	// Let the link go quiet, then end the process: it would otherwise keep
	// trying to reach OBS for ever, which is its job.
	server.close();
	setTimeout(() => process.exit(process.exitCode ?? 0), 50);
});

test("it connects, lists only StageKit's filters, switches one and hears of it", async () => {
	assert.equal(obs.isConnected, false);
	assert.deepEqual(await obs.stageKitFilters(), [], "nothing while there is no OBS");

	const connected = once(obs, "connection");
	obs.use({ host: "127.0.0.1", port });
	assert.deepEqual(await connected, [true]);

	const found = await obs.stageKitFilters();
	assert.deepEqual(
		found.map((f) => `${f.source}/${f.name}/${f.kind}/${f.enabled}`).sort(),
		[
			"Game Scene/Fit the game/stagekit_fit_item/true",
			"camera/Ramp Blur/stagekit_ramp_blur/false",
			"camera/Top right/stagekit_send_to/false",
		],
		"inputs, scenes and groups are looked at; other plugins' filters are left out",
	);

	assert.equal(await obs.filterEnabled("camera", "Ramp Blur"), false);
	assert.equal(await obs.filterEnabled("camera", "No such filter"), undefined);
	assert.equal(await obs.filterEnabled("", ""), undefined);

	const heard = once(obs, "filter");
	await obs.setFilter("camera", "Ramp Blur", true);
	assert.deepEqual(await heard, ["camera", "Ramp Blur", true]);
	assert.equal(await obs.filterEnabled("camera", "Ramp Blur"), true);

	const now = await obs.filter("camera", "Ramp Blur");
	assert.equal(now?.kind, "stagekit_ramp_blur", "a key learns what kind its filter is, for its picture");

	await obs.pressHotkey("stagekit.arrange.left");
	assert.deepEqual(pressed, ["stagekit.arrange.left"], "an arrange tool is one of OBS's hotkeys, pressed by name");

	assert.equal((await obs.stageKit("BlurAll", { mode: "toggle" }))?.active, true, "StageKit's own requests are answered");
	assert.deepEqual(
		await obs.captures(),
		[
			{ name: "Game", kind: "game_capture" },
			{ name: "Window", kind: "window_capture" },
		],
		"a Hook key is offered the Game and Window Captures only",
	);
	assert.deepEqual(await obs.transitionKinds(), ["cut_transition", "fade_transition", "swipe_transition"]);
	assert.equal(await obs.stageKit("NoSuchRequest"), undefined, "and one it refuses comes back as nothing");
});

test("a Scene Mode key lists every mode, turns one on by its id and is lit when the filter says so", async () => {
	const { modeFilterItems, modeChoiceItems, modeTarget } = await import("../src/modes.ts");
	const { modePicture } = await import("../src/pictures.ts");
	const { modeTitle } = await import("../src/titles.ts");
	const { SceneModeKey } = await import("../src/actions/mode.ts");
	if (!obs.isConnected) {
		const connected = once(obs, "connection");
		obs.use({ host: "127.0.0.1", port });
		await connected;
	}
	const sep = "\u001f";
	const filtersFound = await obs.sceneModes();
	assert.deepEqual(
		modeFilterItems(filtersFound),
		[
			{ label: "Chat Live \u2014 Captures", value: `Chat Live${sep}Captures` },
			{ label: "Chat Live \u2014 Column", value: `Chat Live${sep}Column` },
			{ label: "Standby \u2014 Standby modes", value: `Standby${sep}Standby modes` },
		],
		"the key chooses a filter, by its scene and the filter's name always, so it can be found in OBS",
	);
	assert.deepEqual(
		modeChoiceItems(filtersFound.find((f) => f.scene === "Standby")!.modes),
		[
			{ label: "Starting", value: "m1" },
			{ label: "Be Right Back", value: "m2" },
			{ label: "Lunch", value: "m3" },
		],
		"then a mode of that filter, kept by its id",
	);
	// (A key saved before beta175: one setting, a mode of one filter.)
	const brb = `Standby${sep}Standby modes${sep}m2`;
	assert.deepEqual(modeTarget(brb), { scene: "Standby", filter: "Standby modes", mode: "m2" });
	assert.deepEqual(modeTarget("half"), { scene: "", filter: "", mode: "" });

	// A key, as Stream Deck gives one to an action.
	let settings: Record<string, unknown> = { target: brb, press: "on" };
	const shown: { state?: number; title?: string; images: string[]; alerts: number } = { images: [], alerts: 0 };
	const key = {
		id: "mode-key-1",
		isKey: () => true,
		getSettings: async () => settings,
		setSettings: async (next: Record<string, unknown>) => {
			settings = next;
		},
		setImage: async (image?: string) => {
			shown.images.push(String(image));
		},
		setState: async (state: number) => {
			shown.state = state;
		},
		setTitle: async (title: string) => {
			shown.title = title;
		},
		showAlert: async () => {
			shown.alerts++;
		},
	};
	const action = new SceneModeKey() as unknown as {
		show(key: unknown, settings: Record<string, unknown>): Promise<void>;
		onKeyDown(ev: unknown): Promise<void>;
	};
	await action.show(key, settings);
	assert.equal(shown.state, 0, "off: another mode is on");
	assert.equal(shown.title, modeTitle("Be Right Back"));
	assert.ok(shown.images.at(-1)?.endsWith("/status/on/options/mode-brb-on.png"), "no type set in OBS: the name says be right back");
	assert.equal((settings.seen as { name?: string })?.name, "Be Right Back", "remembered for while OBS cannot be asked");

	// A press sends SetSceneMode with the mode's id; the filter's event lights the key.
	const heard = once(obs, "mode");
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(modeRequests.at(-1), { scene: "Standby", filter: "Standby modes", mode: "m2", how: "on" });
	const [event] = (await heard) as [Record<string, unknown>];
	assert.equal(event.mode, "m2");
	await action.show(key, settings);
	assert.equal(shown.state, 1, "lit while its mode is on");
	assert.equal(shown.alerts, 0);

	// Toggle on the mode that is on goes back to the one before.
	settings = { ...settings, press: "toggle" };
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.equal(modeRequests.at(-1)?.how, "toggle");
	await action.show(key, settings);
	assert.equal(shown.state, 0);

	// (Back to Switch on: the key's own mode, lit or dimmed.)
	settings = { ...settings, press: "on" };
	// The picture: Mode type chosen > an image kept from before > the mode's
	// type > its name > the plain mode key. A type is drawn as a mode: the
	// owner's scene layout of that kind where there is one.
	settings = { ...settings, sourceType: "camera" };
	await action.show(key, settings);
	assert.ok(shown.images.at(-1)?.endsWith("/scenes/12-camera-only.png"), shown.images.at(-1));
	settings = { ...settings, sourceType: "title" };
	await action.show(key, settings);
	assert.ok(shown.images.at(-1)?.endsWith("/options/type-title-on.png"), "no layout of that kind: the type's own");
	assert.equal(modePicture(undefined, "", "starting", "Lunch").id, "options/mode-starting", "the mode's own type");
	assert.equal(modePicture(undefined, "", "", "GBA").id, "options/type-handheld", "GBA counts as a handheld");
	assert.equal(modePicture(undefined, "", "", "GBA Link").id, "scenes/11-dolphin-gba", "a linked handheld: the console-and-handheld layout");
	assert.equal(modePicture(undefined, "", "", "Lunch").id, "options/mode", "nothing fits: the plain mode key");
	assert.equal(modePicture("options/browser", "", "game", "GBA").id, "options/browser", "an image kept from before wins over Automatic");
	assert.equal(modePicture("options/browser", "camera", "game", "GBA").id, "scenes/12-camera-only", "a type chosen wins over it");

	// A mode that is gone, or a press OBS refuses, shows an alert.
	settings = { target: `Standby${sep}Standby modes${sep}gone`, press: "on" };
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.equal(shown.alerts, 1);
	// OBS gone: the key keeps what it learned last.
	settings = { target: brb, seen: { for: brb, kind: "stagekit_scene_modes", enabled: true, settings: {}, name: "Be Right Back" } };
	server.clients.forEach((c) => c.terminate());
	await once(obs, "connection");
	await action.show(key, settings);
	assert.equal(shown.state, 1);
	assert.equal(shown.title, modeTitle("Be Right Back"));
});

test("a Scene Mode key is tied to one filter: Switch on, Switch off, Toggle, Next mode over the modes used, and the image of the mode that is on", async () => {
	const { editModeSettings, migrateModeKey, modeRequest, shownMode } = await import("../src/modes.ts");
	const { SceneModeKey } = await import("../src/actions/mode.ts");
	const { modeTitle } = await import("../src/titles.ts");
	obs.use({ host: "127.0.0.1", port });
	while (!obs.isConnected) await once(obs, "connection");
	const sep = "\u001f";
	const standby = `Standby${sep}Standby modes`;
	const f = modeFilters.find((m) => m.scene === "Standby")!;
	f.current = "m1";
	f.previous = "";

	// A key saved before beta175 (a mode of one filter) is the same filter, that mode as its Mode, its press as it was.
	assert.deepEqual(migrateModeKey({ target: `${standby}${sep}m2`, press: "toggle" }), {
		target: `${standby}${sep}m2`,
		press: "toggle",
		modeFilter: standby,
		modeFilterWas: standby,
		mode: "m2",
	});
	assert.equal(migrateModeKey({ modeFilter: standby, mode: "m1" }), undefined, "a key already on a filter is left as it is");

	// What a press sends.
	const modes = { current: "m2", modes: [{ id: "m1" }, { id: "m2" }, { id: "m3" }] };
	assert.deepEqual(modeRequest("on", "m2", {}), { how: "on", mode: "m2" });
	assert.deepEqual(modeRequest("toggle", "m2", {}), { how: "toggle", mode: "m2" });
	assert.equal(modeRequest("on", "", {}), undefined, "no mode chosen: nothing to send");
	assert.deepEqual(modeRequest("off", "", {}), { how: "none" });
	assert.deepEqual(modeRequest("next", "", {}), { how: "next" }, "every mode used: the filter's own next");
	assert.deepEqual(modeRequest("next", "", { m3: { skip: true } }, modes), { how: "on", mode: "m1" }, "m3 left out: from m2 round to m1");
	assert.deepEqual(modeRequest("next", "", { m2: { skip: true } }, { ...modes, current: "m1" }), { how: "on", mode: "m3" });
	assert.deepEqual(modeRequest("next", "", { m1: { skip: true } }, { ...modes, current: "" }), { how: "on", mode: "m2" }, "none on: the first used");
	assert.equal(modeRequest("next", "", { m1: { skip: true }, m2: { skip: true }, m3: { skip: true } }, modes), undefined);
	assert.deepEqual(shownMode("on", "m2", "m1"), { shows: "m2", lit: false }, "Switch on: its own mode, dimmed");
	assert.deepEqual(shownMode("toggle", "m2", "m1"), { shows: "m1", lit: false }, "Toggle: the mode that is on");
	assert.deepEqual(shownMode("next", "", "m3"), { shows: "m3", lit: true });
	assert.deepEqual(shownMode("off", "", ""), { shows: "", lit: false });

	// The page's Settings for: one set of lists, kept per mode.
	let page: Record<string, unknown> = { modeFilter: standby, modeFilterWas: standby, modeFor: "m1" };
	page = editModeSettings(page)!;
	assert.deepEqual([page.modeUse, page.modeType, page.modeLayout], ["use", "auto", "auto"], "a mode chosen: its settings shown");
	page = editModeSettings({ ...page, modeType: "camera", modeLayout: "chat" })!;
	assert.deepEqual(page.perMode, { m1: { type: "camera", layout: "chat" } });
	page = editModeSettings({ ...page, modeFor: "m3" })!;
	assert.equal(page.modeType, "auto", "another mode: its own, Automatic");
	page = editModeSettings({ ...page, modeUse: "skip" })!;
	assert.deepEqual(page.perMode, { m1: { type: "camera", layout: "chat" }, m3: { skip: true } });
	page = editModeSettings({ ...page, modeFor: "m1" })!;
	assert.deepEqual([page.modeUse, page.modeType, page.modeLayout], ["use", "camera", "chat"], "back to the first: what it was given");
	assert.equal(editModeSettings(page), undefined, "nothing changed: nothing written");
	const other = editModeSettings({ ...page, modeFilter: `Chat Live${sep}Column` })!;
	assert.deepEqual([other.perMode, other.modeFor, other.mode], [{}, "", ""], "another filter: the old filter's modes are forgotten");

	// Mode settings is chosen for you (2026-11-18): the key's Switch to mode, else the first mode.
	page = editModeSettings({ modeFilter: standby, modeFilterWas: standby, mode: "m2" })!;
	assert.deepEqual([page.modeFor, page.modeForWas, page.modeType], ["m2", "m2", "auto"], "the key's own mode");
	page = editModeSettings({ modeFilter: standby, modeFilterWas: standby }, "m1")!;
	assert.equal(page.modeFor, "m1", "none to switch to: the filter's first mode");
	assert.equal(editModeSettings({ modeFilter: standby, modeFilterWas: standby }), undefined, "the first mode not known: left empty");
	page = editModeSettings({ modeFilter: `Chat Live${sep}Column`, modeFilterWas: standby, mode: "m2", modeFor: "m2", modeForWas: "m2" }, "k1")!;
	assert.deepEqual([page.mode, page.modeFor], ["", "k1"], "another filter: its first mode");

	// A key saved up to beta179 with the key-wide Layout and Mode type the page no longer shows: its own mode's
	// Image list shows that Mode type, nothing is written for looking, and editing that Image takes its place.
	const old = { modeFilter: standby, modeFilterWas: standby, mode: "m2", press: "toggle", layout: "chat", sourceType: "camera" };
	page = editModeSettings(old)!;
	assert.deepEqual([page.modeFor, page.modeType, page.modeLayout, page.perMode], ["m2", "camera", "auto", undefined], "shown as it is");
	assert.equal(editModeSettings(page), undefined, "looked at: nothing written");
	page = editModeSettings({ ...page, modeFor: "m1" })!;
	assert.equal(page.modeType, "auto", "another mode: the key-wide Mode type was never its image");
	page = editModeSettings({ ...page, modeFor: "m2" })!;
	page = editModeSettings({ ...page, modeType: "auto" })!;
	assert.deepEqual([page.sourceType, page.perMode, page.layout], ["auto", {}, "chat"], "Automatic chosen: the mode's own type from now on; the Layout untouched");
	page = editModeSettings({ ...old, modeFor: "m2", modeForWas: "m2", modeTypeKeyWide: "m2", modeType: "game", modeLayout: "auto", modeUse: "use" })!;
	assert.deepEqual([page.sourceType, page.perMode], ["auto", { m2: { type: "game" } }], "another image: the mode's own");

	// Switch to mode changed on such a key is no edit of the Image: the lists are loaded again for the new rule.
	page = editModeSettings({ ...old, modeFor: "m1" })!;
	assert.deepEqual([page.modeType, page.modeTypeKeyWide], ["auto", ""], "Mode settings on m1: not the key's mode");
	let moved = editModeSettings({ ...old, modeFor: "m2" }) ?? old;
	moved = editModeSettings(moved) ?? moved;
	assert.deepEqual([moved.modeType, moved.modeTypeKeyWide], ["camera", "m2"], "loaded for m2");
	moved = editModeSettings({ ...moved, mode: "m1" })!;
	assert.deepEqual([moved.perMode, moved.sourceType, moved.modeType], [undefined, "camera", "auto"], "Mode settings on the old mode, Switch to mode changed: nothing written, the list shows m2's own");
	assert.equal(editModeSettings(moved), undefined, "and then settled");
	moved = editModeSettings({ ...page, mode: "m1" })!;
	assert.deepEqual([moved.perMode, moved.sourceType, moved.modeType, moved.modeTypeKeyWide], [undefined, "camera", "camera", "m1"], "Mode settings already on the new mode: the key-wide image shown for it, nothing erased");
	assert.equal(editModeSettings(moved), undefined, "and then settled");

	// A key, as Stream Deck gives one to an action.
	let settings: Record<string, unknown> = {};
	const shown: { state?: number; title?: string; images: string[]; alerts: number } = { images: [], alerts: 0 };
	const key = {
		id: "mode-key-2",
		isKey: () => true,
		getSettings: async () => settings,
		setSettings: async (next: Record<string, unknown>) => {
			settings = next;
		},
		setImage: async (image?: string) => {
			shown.images.push(String(image));
		},
		setState: async (state: number) => {
			shown.state = state;
		},
		setTitle: async (title: string) => {
			shown.title = title;
		},
		showAlert: async () => {
			shown.alerts++;
		},
	};
	const action = new SceneModeKey() as unknown as {
		show(key: unknown, settings: Record<string, unknown>): Promise<void>;
		onKeyDown(ev: unknown): Promise<void>;
		onKeyUp(ev: unknown): void;
		onDidReceiveSettings(ev: unknown): Promise<void>;
	};
	const image = () => shown.images.at(-1) ?? "";
	const press = async () => {
		const heard = once(obs, "mode");
		await action.onKeyDown({ action: key, payload: { settings } });
		action.onKeyUp({ action: key });
		await heard;
		await action.show(key, settings);
	};

	// Next mode, every mode used: the filter's own next; the image and title follow the mode on.
	settings = { modeFilter: standby, press: "next" };
	await action.show(key, settings);
	assert.equal(shown.title, "Starting");
	assert.ok(image().includes("options/mode-starting"), image());
	assert.equal(shown.state, 1, "lit while a mode is on");
	await press();
	assert.deepEqual(modeRequests.at(-1), { scene: "Standby", filter: "Standby modes", how: "next" });
	assert.equal(f.current, "m2");
	assert.equal(shown.title, modeTitle("Be Right Back"));
	assert.ok(image().includes("options/mode-brb"), "Automatic: no type in OBS, the name says be right back");

	// Next mode with Lunch left out: from Be Right Back round to Starting, as "on".
	settings = { ...settings, perMode: { m3: { skip: true } } };
	await press();
	assert.deepEqual(modeRequests.at(-1), { scene: "Standby", filter: "Standby modes", mode: "m1", how: "on" });
	assert.equal(shown.title, "Starting");

	// A mode's own picture: its Mode type and Layout on the key.
	settings = { ...settings, perMode: { m1: { type: "camera" } } };
	await action.show(key, settings);
	assert.ok(image().endsWith("/scenes/12-camera-only.png") || image().includes("scenes/12-camera-only"), image());
	settings = { ...settings, perMode: { m1: { type: "game", layout: "chat" } } };
	await action.show(key, settings);
	assert.ok(image().includes("layouts/chat-type-game"), image());
	// Automatic: Lunch has no type and its name says nothing: the plain mode key.
	settings = { ...settings, perMode: {} };
	f.current = "m2";
	await press();
	assert.equal(f.current, "m3");
	assert.equal(shown.title, "Lunch");
	assert.ok(/options\/mode-on\.png$/.test(image()), image());

	// Switch off: no mode; the filter's picture and "No mode".
	settings = { modeFilter: standby, press: "off" };
	await press();
	assert.deepEqual(modeRequests.at(-1), { scene: "Standby", filter: "Standby modes", how: "none" });
	assert.equal(f.current, "");
	assert.equal(shown.title, "No mode");
	assert.ok(/options\/mode-off\.png$/.test(shown.images.at(-2) ?? ""), "dimmed: no mode on");
	assert.equal(shown.state, 0);

	// Toggle: its own mode on, the image follows; the key's Mode type is its own mode's.
	settings = { modeFilter: standby, mode: "m2", press: "toggle", sourceType: "camera" };
	await press();
	assert.deepEqual(modeRequests.at(-1), { scene: "Standby", filter: "Standby modes", mode: "m2", how: "toggle" });
	assert.equal(shown.state, 1);
	assert.ok(image().includes("scenes/12-camera-only"), "its own mode: the key's Mode type");
	await press();
	assert.equal(f.current, "", "back to the one before: none");
	assert.equal(shown.title, "No mode");
	f.current = "m1";
	await action.show(key, settings);
	assert.equal(shown.title, "Starting", "another mode on: its name");
	assert.ok(image().includes("options/mode-starting"), "and its own picture, not the key's Mode type");
	assert.equal(shown.state, 0, "dimmed: its own mode is not on");

	// Switch on is as before: its own mode, lit or dimmed.
	settings = { modeFilter: standby, mode: "m2", press: "on" };
	await action.show(key, settings);
	assert.equal(shown.title, modeTitle("Be Right Back"));
	assert.ok(image().includes("options/mode-brb"));
	assert.equal(shown.state, 0);

	// A key saved up to beta179 with the key-wide Layout and Mode type and no per-mode settings looks exactly as
	// before once its page has been brought up to date (Mode settings chosen, the lists filled), for its own mode
	// and for another mode on.
	for (const current of ["m2", "m1", ""]) {
		f.current = current;
		for (const keyWide of [
			{ layout: "chat", sourceType: "camera" },
			{ layout: "live" },
			{ sourceType: "game" },
		]) {
			const before = { modeFilter: standby, modeFilterWas: standby, mode: "m2", press: "toggle", ...keyWide };
			await action.show(key, before);
			const was = [image(), shown.title, shown.state];
			settings = { ...before };
			await action.onDidReceiveSettings({ action: key, payload: { settings } });
			assert.equal(settings.modeFor, "m2", "Mode settings: the key's own mode");
			assert.deepEqual([settings.layout, settings.sourceType, settings.perMode], [before.layout, before.sourceType, undefined], "the key-wide values kept");
			await action.show(key, settings);
			assert.deepEqual([image(), shown.title, shown.state], was, `${JSON.stringify(keyWide)}, ${current || "none"} on`);
		}
	}
	// (What that is: the key-wide Layout for every mode, the Mode type for its own mode only.)
	f.current = "m2";
	await action.show(key, { modeFilter: standby, mode: "m2", press: "toggle", layout: "chat", sourceType: "camera" });
	assert.ok(image().includes("layouts/chat-"), image());
	f.current = "m1";
	await action.show(key, { modeFilter: standby, mode: "m2", press: "toggle", layout: "chat", sourceType: "camera" });
	assert.ok(image().includes("layouts/chat-") && !image().includes("camera"), image());

	// An old key's settings are moved when they arrive.
	settings = { target: `${standby}${sep}m2`, press: "toggle" };
	await action.onDidReceiveSettings({ action: key, payload: { settings } });
	assert.equal(settings.modeFilter, standby);
	assert.equal(settings.mode, "m2");

	// A long press: Switch off when held, Next mode when tapped.
	settings = { modeFilter: standby, press: "next", longPress: "off", holdMs: 300 };
	f.current = "m1";
	let heard = once(obs, "mode");
	await action.onKeyDown({ action: key, payload: { settings } });
	await heard;
	action.onKeyUp({ action: key });
	assert.equal(modeRequests.at(-1)?.how, "none", "held: the long press");
	heard = once(obs, "mode");
	await action.onKeyDown({ action: key, payload: { settings } });
	action.onKeyUp({ action: key });
	await heard;
	assert.equal(modeRequests.at(-1)?.how, "next", "tapped: the press");

	// OBS gone: a Next mode key shows the mode it last saw on.
	f.current = "m2";
	settings = { modeFilter: standby, press: "next" };
	await action.show(key, settings);
	assert.equal(shown.title, modeTitle("Be Right Back"));
	server.clients.forEach((c) => c.terminate());
	await once(obs, "connection");
	const kept = settings;
	await action.show(key, kept);
	assert.equal(shown.title, modeTitle("Be Right Back"), "remembered while OBS cannot be asked");
	assert.equal(shown.state, 1);
	assert.equal(shown.alerts, 0);
});

test("a Scene Blur key lists the blurs and their scenes, switches one scene or all, and is lit by the filter's event", async () => {
	const { blurFilterItems, blurSceneItems, readSceneBlurs, sceneBlurOn } = await import("../src/scene-blurs.ts");
	const { sceneBlurPicture } = await import("../src/pictures.ts");
	const { sceneBlurTitle } = await import("../src/titles.ts");
	const { SceneBlurKey } = await import("../src/actions/scene-blur.ts");
	obs.use({ host: "127.0.0.1", port });
	while (!obs.isConnected) await once(obs, "connection");

	// The two lists: each name once; every scene with it first, then each scene.
	const filters = await obs.sceneBlurs();
	assert.deepEqual(blurFilterItems(filters), [
		{ label: "Blur (2 scenes)", value: "Blur" },
		{ label: "Mosaic (Standby)", value: "Mosaic" },
	]);
	assert.deepEqual(blurSceneItems(filters, "Blur"), [
		{ label: "All scenes with this filter", value: "" },
		{ label: "Live", value: "Live" },
		{ label: "Chat Live", value: "Chat Live" },
	]);
	assert.deepEqual(blurSceneItems(filters, "Gone"), [{ label: "All scenes with this filter", value: "" }]);
	assert.deepEqual(readSceneBlurs(undefined), []);
	assert.equal(sceneBlurOn(filters, "Gone", ""), undefined, "a name OBS does not have is not known to be on or off");

	// A key, as Stream Deck gives one to an action.
	let settings: Record<string, unknown> = { filter: "Blur", scene: "Live", press: "toggle" };
	const shown: { state?: number; title?: string; images: string[]; alerts: number } = { images: [], alerts: 0 };
	const key = {
		isKey: () => true,
		getSettings: async () => settings,
		setSettings: async (next: Record<string, unknown>) => {
			settings = next;
		},
		setImage: async (image?: string) => {
			shown.images.push(String(image));
		},
		setState: async (state: number) => {
			shown.state = state;
		},
		setTitle: async (title: string) => {
			shown.title = title;
		},
		showAlert: async () => {
			shown.alerts++;
		},
	};
	const action = new SceneBlurKey() as unknown as {
		show(key: unknown, settings: Record<string, unknown>): Promise<void>;
		onKeyDown(ev: unknown): Promise<void>;
	};
	await action.show(key, settings);
	assert.equal(shown.state, 0, "off");
	assert.equal(shown.title, sceneBlurTitle("toggle"));
	assert.equal(shown.title, "Scene\nBlur");
	assert.ok(shown.images.at(-1)?.endsWith("/status/on/blur-types/gaussian-on.png"), shown.images.at(-1));

	// One scene: only that scene's blur is switched; the filter's event lights the key.
	const heard = once(obs, "sceneblur");
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(blurRequests.at(-1), { filter: "Blur", scene: "Live", how: "toggle" });
	const [event] = (await heard) as [Record<string, unknown>];
	assert.deepEqual(event, { scene: "Live", filter: "Blur", active: true });
	assert.deepEqual(
		sceneBlurs.map((b) => b.active),
		[true, false, false],
		"the same name on another scene is left alone",
	);
	await action.show(key, settings);
	assert.equal(shown.state, 1, "lit while that scene's blur is on");

	// All scenes with the name: lit while any is on; a toggle then clears them all.
	settings = { filter: "Blur", scene: "", press: "toggle" };
	await action.show(key, settings);
	assert.equal(shown.state, 1, "lit while any of them is on");
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(blurRequests.at(-1), { filter: "Blur", scene: "", how: "toggle" });
	assert.deepEqual(sceneBlurs.map((b) => b.active), [false, false, false]);
	await action.show(key, settings);
	assert.equal(shown.state, 0);
	settings = { ...settings, press: "on" };
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(sceneBlurs.map((b) => b.active), [true, true, false], "switch on: every scene with it");
	assert.equal(shown.title, "Scene\nBlur\nOn");
	settings = { ...settings, press: "off" };
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(sceneBlurs.map((b) => b.active), [false, false, false]);
	assert.equal(shown.alerts, 0);

	// The picture: the Blur style chosen > the style the filter is set to.
	settings = { filter: "Mosaic", scene: "Standby", press: "toggle" };
	await action.show(key, settings);
	assert.ok(shown.images.at(-1)?.endsWith("/options/style-mosaic-voronoi-on.png"), `the filter's own look: ${shown.images.at(-1)}`);
	settings = { ...settings, look: "motion" };
	await action.show(key, settings);
	assert.ok(shown.images.at(-1)?.endsWith("/blur-types/motion-on.png"), "a Blur style chosen wins");
	assert.equal(sceneBlurPicture(undefined, undefined, "trail").id, "options/style-trail", "a trail");
	assert.equal(sceneBlurPicture(undefined, undefined, "").id, "blur-types/gaussian", "a look not said: Gaussian");

	// A name no scene has: an alert. OBS gone: the key keeps what it learned last.
	settings = { filter: "Gone", scene: "", press: "on" };
	await action.onKeyDown({ action: key, payload: { settings } });
	assert.equal(shown.alerts, 1);
	settings = { filter: "Blur", scene: "Live", seen: { for: "Blur\u001fLive", kind: "stagekit_scene_blur", enabled: true, settings: {}, type: "box" } };
	server.clients.forEach((c) => c.terminate());
	await once(obs, "connection");
	await action.show(key, settings);
	assert.equal(shown.state, 1);
	assert.ok(shown.images.at(-1)?.endsWith("/blur-types/box-on.png"), shown.images.at(-1));
});

test("a Standby Image key covers its source with the image and is lit while covered; a Show in Scenes key hides its own way", async () => {
	const { FilterSwitch } = await import("../src/actions/switch.ts");
	const { switchTitle } = await import("../src/titles.ts");
	obs.use({ host: "127.0.0.1", port });
	while (!obs.isConnected) await once(obs, "connection");
	const sep = "\u001f";
	let settings: Record<string, unknown> = { target: `DSLR${sep}Standby Image`, press: "cover" };
	const shown: { state?: number; title?: string; alerts: number } = { alerts: 0 };
	const key = {
		isKey: () => true,
		getSettings: async () => settings,
		setSettings: async (next: Record<string, unknown>) => {
			settings = next;
		},
		setImage: async () => {},
		setState: async (state: number) => {
			shown.state = state;
		},
		setTitle: async (title: string) => {
			shown.title = title;
		},
		showAlert: async () => {
			shown.alerts++;
		},
	};
	const standby = new FilterSwitch("com.techsavage1989.stagekit.standby", (kind) => kind === "stagekit_standby") as unknown as {
		show(key: unknown, settings: Record<string, unknown>): Promise<void>;
		onKeyDown(ev: unknown): Promise<void>;
	};
	await standby.show(key, settings);
	assert.equal(shown.state, 0, "not covered: not lit (although the filter is on)");
	assert.equal(shown.title, "Cover /\nUncover");

	// Cover: toggle, through StageKit's request; the filter is not switched.
	const heard = once(obs, "standbycover");
	await standby.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(coverRequests.at(-1), { sourceName: "DSLR", filterName: "Standby Image", how: "toggle" });
	assert.equal(dslrCovered, true);
	assert.equal(shown.state, 1, "lit while covered");
	assert.deepEqual((await heard)[0], { sourceName: "DSLR", filterName: "Standby Image", covered: true });
	assert.equal(filters.DSLR[0].filterEnabled, true, "the filter itself stays on");
	settings = { ...settings, press: "coveroff" };
	await standby.onKeyDown({ action: key, payload: { settings } });
	assert.equal(coverRequests.at(-1)?.how, "off");
	assert.equal(shown.state, 0);
	assert.equal(shown.title, switchTitle("stagekit_standby", "coveroff"));
	settings = { ...settings, press: "coveron" };
	await standby.onKeyDown({ action: key, payload: { settings } });
	assert.equal(dslrCovered, true);
	// A source without one: an alert.
	settings = { target: `camera${sep}Ramp Blur`, press: "coveron" };
	await standby.onKeyDown({ action: key, payload: { settings } });
	assert.equal(shown.alerts, 1);
	// Its filter's own presses still switch the filter.
	settings = { target: `DSLR${sep}Standby Image`, press: "off" };
	await standby.onKeyDown({ action: key, payload: { settings } });
	assert.equal(filters.DSLR[0].filterEnabled, false);
	filters.DSLR[0].filterEnabled = true;

	// Show in Scenes: "As the filter says" switches the filter as any key;
	// Hide or Cover press through ShowInScenesPress with that way.
	const sw = new FilterSwitch("com.techsavage1989.stagekit.showinscenes", (kind) => kind === "stagekit_show_in_scenes") as unknown as {
		onKeyDown(ev: unknown): Promise<void>;
	};
	settings = { target: `DSLR${sep}Show in Scenes-Camera`, press: "toggle" };
	await sw.onKeyDown({ action: key, payload: { settings } });
	assert.equal(showPresses.length, 0, "as the filter says (unset): no StageKit request");
	assert.equal(filters.DSLR[1].filterEnabled, false);
	settings = { ...settings, hideAs: "filter" };
	await sw.onKeyDown({ action: key, payload: { settings } });
	assert.equal(showPresses.length, 0);
	assert.equal(filters.DSLR[1].filterEnabled, true);
	settings = { ...settings, hideAs: "cover" };
	await sw.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(showPresses.at(-1), { sourceName: "DSLR", filterName: "Show in Scenes-Camera", how: "toggle", hideAs: "cover" });
	assert.equal(filters.DSLR[1].filterEnabled, false);
	settings = { ...settings, hideAs: "hide", press: "on" };
	await sw.onKeyDown({ action: key, payload: { settings } });
	assert.deepEqual(showPresses.at(-1), { sourceName: "DSLR", filterName: "Show in Scenes-Camera", how: "on", hideAs: "hide" });
	assert.equal(filters.DSLR[1].filterEnabled, true);
	assert.equal(shown.alerts, 1);
});
