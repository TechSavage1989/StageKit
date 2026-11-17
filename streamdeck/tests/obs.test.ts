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
};

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
	const mode = f.modes.find((m) => m.id === data.mode || m.name === data.mode);
	if (!mode) return { success: false, error: "No such mode" };
	const was = f.current;
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
	const { modeItems, modeTarget } = await import("../src/modes.ts");
	const { modePicture } = await import("../src/pictures.ts");
	const { modeTitle } = await import("../src/titles.ts");
	const { SceneModeKey } = await import("../src/actions/mode.ts");
	if (!obs.isConnected) {
		const connected = once(obs, "connection");
		obs.use({ host: "127.0.0.1", port });
		await connected;
	}
	const sep = "\u001f";
	const items = modeItems(await obs.sceneModes());
	assert.deepEqual(
		items.map((i) => i.label),
		["Chat Live \u2014 Captures: Game", "Chat Live \u2014 Column: GBA", "Standby: Starting", "Standby: Be Right Back", "Standby: Lunch"],
		"the filter's name only where a scene has two",
	);
	const brb = items.find((i) => i.label === "Standby: Be Right Back")!.value;
	assert.equal(brb, `Standby${sep}Standby modes${sep}m2`, "the key keeps the mode's id, not its name");
	assert.deepEqual(modeTarget(brb), { scene: "Standby", filter: "Standby modes", mode: "m2" });
	assert.deepEqual(modeTarget("half"), { scene: "", filter: "", mode: "" });

	// A key, as Stream Deck gives one to an action.
	let settings: Record<string, unknown> = { target: brb, press: "on" };
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
