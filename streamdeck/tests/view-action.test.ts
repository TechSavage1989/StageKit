// The View action against an OBS WebSocket stand-in. Native command
// acceptance and application are separate, as they are in the plugin.
import assert from "node:assert/strict";
import { EventEmitter, once } from "node:events";
import { after, before, beforeEach, test, type TestContext } from "node:test";
import streamDeck from "@elgato/streamdeck";
import { WebSocketServer } from "ws";

import { ViewKey } from "../src/actions/view.ts";
import type { KeySettings } from "../src/actions/shared.ts";
import { obs } from "../src/obs.ts";
import { controllerSnapshot } from "./view-fixture.ts";

let server: WebSocketServer;
let port = 0;
let controllers = [controllerSnapshot()];
let available = true;
let accepted = true;
let mismatch = false;
let emptyAcceptance = false;
let holdNextGet: ((reply: () => void) => void) | undefined;
const requests: Array<{ name: string; data: Record<string, unknown> }> = [];
const commands = () => requests.filter((request) => request.name === "SceneControllerCommand").map((request) => request.data);

before(async () => {
	server = new WebSocketServer({ port: 0, handleProtocols: () => "obswebsocket.json" });
	await once(server, "listening");
	port = (server.address() as { port: number }).port;
	server.on("connection", (socket) => {
		const send = (op: number, d: unknown) => socket.send(JSON.stringify({ op, d }));
		send(0, { obsWebSocketVersion: "5.5.0", rpcVersion: 1 });
		socket.on("message", (raw) => {
			const { op, d } = JSON.parse(String(raw));
			if (op === 1) send(2, { negotiatedRpcVersion: 1 });
			if (op !== 6) return;
			assert.equal(d.requestType, "CallVendorRequest", "View uses only its native controller requests");
			assert.equal(d.requestData.vendorName, "StageKit");
			const name = d.requestData.requestType;
			const data = d.requestData.requestData ?? {};
			requests.push({ name, data });
			const controller = controllers.find((entry) => entry.controllerId === data.controllerId);
			let answer: Record<string, unknown> = { success: false, error: "No such request or controller" };
			if (available) {
				if (name === "ListSceneControllers") answer = { success: true, controllers };
				if (name === "GetSceneController" && controller) answer = { success: true, ...controller, controllerId: mismatch ? "wrong-controller" : controller.controllerId };
				if (name === "SceneControllerCommand" && controller) answer = emptyAcceptance ? {} : { success: accepted, error: accepted ? "" : "The native controller rejected this command" };
			}
			const snapshot = structuredClone(answer);
			const reply = () => send(7, {
				requestType: d.requestType, requestId: d.requestId, requestStatus: { result: true, code: 100 },
				responseData: { vendorName: "StageKit", requestType: name, responseData: snapshot },
			});
			const held = name === "GetSceneController" ? holdNextGet : undefined;
			if (held) {
				holdNextGet = undefined;
				held(reply);
			} else reply();
		});
	});
	const connected = once(obs, "connection");
	obs.use({ host: "127.0.0.1", port });
	assert.deepEqual(await connected, [true]);
});

beforeEach(() => {
	controllers = [controllerSnapshot()];
	available = accepted = true;
	mismatch = emptyAcceptance = false;
	holdNextGet = undefined;
	requests.length = 0;
});

after(() => {
	server.clients.forEach((socket) => socket.terminate());
	server.close();
	// ObsLink deliberately retries forever. Other companion WebSocket tests
	// use the same process cleanup after all assertions have completed.
	setTimeout(() => process.exit(process.exitCode ?? 0), 50);
});

const selected: KeySettings = { controllerId: "controller-a", viewTarget: "group", groupId: "mode-group", choiceId: "game", viewPress: "select" };

function fixture(t: TestContext, settings: KeySettings = selected, id = "view-test", sharedAction?: ViewKey) {
	const watched = ["scenecontroller", "connection", "changed", "filter"];
	const listeners = new Map(watched.map((event) => [event, obs.listeners(event)]));
	const action = sharedAction ?? new ViewKey();
	const draws = new EventEmitter();
	const output = { state: -1, title: "", images: ["", ""], alerts: 0, settings: { ...settings } };
	const key = {
		id, isKey: () => true,
		getSettings: async () => output.settings,
		setSettings: async (next: KeySettings) => { output.settings = next; },
		setImage: async (image: string, options?: { state?: number }) => { output.images[options?.state ?? 0] = image; },
		setState: async (state: number) => { output.state = state; },
		setTitle: async (title: string) => { output.title = title; draws.emit("draw"); },
		showAlert: async () => { output.alerts++; },
	};
	t.after(() => {
		action.onWillDisappear({ action: key } as never);
		for (const event of watched)
			for (const listener of obs.listeners(event))
				if (!listeners.get(event)!.includes(listener)) obs.off(event, listener);
	});
	return {
		action, key, output, draws,
		appear: () => action.onWillAppear({ action: key, payload: { settings: output.settings } } as never),
		press: () => action.onKeyDown({ action: key, payload: { settings: output.settings } } as never),
		change: async (next: KeySettings) => {
			output.settings = next;
			await action.onDidReceiveSettings({ action: key, payload: { settings: next } } as never);
		},
	};
}

function nativeEvent(eventType: string, eventData: Record<string, unknown>) {
	for (const socket of server.clients) socket.send(JSON.stringify({ op: 5, d: {
		eventType, eventIntent: 512, eventData,
	} }));
}

function changed(controllerId = "controller-a") {
	nativeEvent("VendorEvent", { vendorName: "StageKit", eventType: "SceneControllerChanged", eventData: { controllerId } });
}

async function redraw(f: ReturnType<typeof fixture>) {
	const drawn = once(f.draws, "draw");
	changed();
	await drawn;
}

test("controller vendor requests preserve exact IDs and reject unknown, mismatched or unacknowledged responses", { timeout: 3000 }, async () => {
	assert.equal((await obs.sceneControllers())[0].controllerId, "controller-a");
	assert.equal((await obs.sceneController("controller-a"))?.groups[0].current, "game");
	assert.deepEqual(requests.slice(0, 2), [{ name: "ListSceneControllers", data: {} }, { name: "GetSceneController", data: { controllerId: "controller-a" } }]);
	mismatch = true;
	assert.equal(await obs.sceneController("controller-a"), undefined);
	mismatch = false;
	assert.equal(await obs.sceneController("missing"), undefined);
	const command = { controllerId: "controller-a", viewId: "stream-view", command: "select" as const };
	assert.equal(await obs.sceneControllerCommand(command), true);
	assert.deepEqual(commands().at(-1), command);
	accepted = false;
	assert.equal(await obs.sceneControllerCommand(command), false);
	emptyAcceptance = true;
	assert.equal(await obs.sceneControllerCommand(command), false);
	available = false;
	assert.deepEqual(await obs.sceneControllers(), []);
	assert.equal(await obs.sceneController("controller-a"), undefined);
	assert.equal(await obs.sceneControllerCommand(command), false);
});

test("View waits for actual native state, follows renamed choices and clears disabled or missing feedback", { timeout: 4000 }, async (t) => {
	const f = fixture(t);
	await f.appear();
	assert.equal(f.output.state, 1);
	await f.change({ ...selected, choiceId: "art" });
	assert.equal(f.output.state, 0);
	await f.press();
	assert.deepEqual(commands(), [{ controllerId: "controller-a", groupId: "mode-group", choiceId: "art", command: "select" }]);
	assert.equal(f.output.state, 0, "queued command acceptance does not predict application");
	const group = controllers[0].groups[0];
	group.current = "art";
	group.choices[0].active = false;
	group.choices[1].active = true;
	group.choices[1].name = "Art renamed";
	await redraw(f);
	assert.equal(f.output.state, 1);
	assert.equal(f.output.title, "Art\nrenamed");
	group.enabled = false;
	await redraw(f);
	assert.equal(f.output.state, 0);
	assert.equal(f.output.title, "Disabled");
	await f.press();
	assert.equal(commands().length, 1, "a disabled group is never sent a command");
	assert.equal(f.output.alerts, 1);
	controllers[0].groups = [];
	await redraw(f);
	assert.equal(f.output.title, "Missing\nGroup");
	controllers = [];
	await redraw(f);
	assert.equal(f.output.title, "Missing\nController");
	assert.equal(f.output.state, 0);
});

test("View sends saved views and Next directly to the native owner and refuses native failures", { timeout: 3000 }, async (t) => {
	const f = fixture(t, { controllerId: "controller-a", viewTarget: "view", viewId: "art-view" });
	await f.appear();
	await f.press();
	assert.deepEqual(commands().at(-1), { controllerId: "controller-a", viewId: "art-view", command: "select" });
	assert.equal(f.output.state, 0);
	await f.change({ ...selected, viewPress: "next" });
	// Changing the target resets On press to Select. A second edit selects Next.
	await f.change({ ...selected, viewPress: "next" });
	await f.press();
	assert.deepEqual(commands().at(-1), { controllerId: "controller-a", groupId: "mode-group", command: "next" });
	accepted = false;
	await f.press();
	assert.equal(f.output.alerts, 1);
	assert.equal(f.output.state, 1, "a failed press does not overwrite the actual selected choice");
});

test("a late snapshot cannot restore a previous target and a stale press cannot change it", { timeout: 4000 }, async (t) => {
	const f = fixture(t);
	await f.appear();
	let release!: () => void;
	let received!: () => void;
	let held = new Promise<void>((resolve) => { received = resolve; });
	holdNextGet = (reply) => { release = reply; received(); };
	const stale = f.change({ ...selected, palette: "ocean" });
	await held;
	const other = controllerSnapshot();
	other.controllerId = "controller-b";
	controllers.push(other);
	await f.change({ ...selected, controllerId: "controller-b" });
	assert.equal(f.output.settings.groupId, undefined, "identical child IDs on another controller still need explicit selection");
	assert.equal(f.output.settings.choiceId, undefined);
	assert.equal(f.output.title, "Choose\nGroup");
	release();
	await stale;
	assert.equal(f.output.title, "Choose\nGroup");
	assert.equal(f.output.state, 0);
	await f.change({ controllerId: "controller-a" });
	await f.change({ controllerId: "controller-a", viewTarget: "group", groupId: "mode-group" });
	await f.change(selected);
	held = new Promise<void>((resolve) => { received = resolve; });
	holdNextGet = (reply) => { release = reply; received(); };
	const pressed = f.press();
	await held;
	await f.change({ ...selected, choiceId: "art" });
	release();
	await pressed;
	assert.equal(commands().length, 0, "a changed key cannot submit its previous selection after preflight");
	assert.equal(f.output.state, 0);
	assert.equal(f.output.title, "Art");
});

test("one refresh requests each visible controller once while drawing all its keys", { timeout: 3000 }, async (t) => {
	const f = fixture(t, selected, "game");
	const art = fixture(t, { ...selected, choiceId: "art" }, "art", f.action);
	const otherController = controllerSnapshot();
	otherController.controllerId = "controller-b";
	controllers.push(otherController);
	const other = fixture(t, { ...selected, controllerId: "controller-b" }, "other", f.action);
	await Promise.all([f.appear(), art.appear(), other.appear()]);
	requests.length = 0;
	let drawn = Promise.all([once(f.draws, "draw"), once(art.draws, "draw")]);
	changed();
	await drawn;
	assert.deepEqual(requests, [{ name: "GetSceneController", data: { controllerId: "controller-a" } }]);
	assert.equal(f.output.state, 1);
	assert.equal(art.output.state, 0);
	requests.length = 0;
	drawn = Promise.all([once(f.draws, "draw"), once(art.draws, "draw"), once(other.draws, "draw")]);
	nativeEvent("SceneNameChanged", { sceneName: "Renamed scene", oldSceneName: "Old scene" });
	await drawn;
	assert.deepEqual(requests.map((entry) => entry.data.controllerId).sort(), ["controller-a", "controller-b"]);
	assert.ok(requests.every((entry) => entry.name === "GetSceneController"));
	assert.equal(other.output.state, 1);
});

test("a newer native event starts a fresh batch and an older shared response cannot overwrite it", { timeout: 3000 }, async (t) => {
	const f = fixture(t, selected, "game");
	const art = fixture(t, { ...selected, choiceId: "art" }, "art", f.action);
	await Promise.all([f.appear(), art.appear()]);
	requests.length = 0;
	let release!: () => void;
	const held = new Promise<void>((resolve) => { holdNextGet = (reply) => { release = reply; resolve(); }; });
	changed();
	await held;
	const group = controllers[0].groups[0];
	group.current = "art";
	group.choices[0].active = false;
	group.choices[1].active = true;
	group.choices[1].name = "Art renamed";
	const drawn = Promise.all([once(f.draws, "draw"), once(art.draws, "draw")]);
	changed();
	await drawn;
	assert.equal(requests.length, 2, "each event has one request, including while the previous event is pending");
	assert.equal(f.output.state, 0);
	assert.equal(art.output.state, 1);
	assert.equal(art.output.title, "Art\nrenamed");
	release();
	// This response follows the released old reply on the same WebSocket,
	// so its completion also establishes that the old reply was processed.
	await obs.sceneController("controller-a");
	assert.equal(f.output.state, 0);
	assert.equal(art.output.state, 1);
	assert.equal(art.output.title, "Art\nrenamed");
});

test("property-inspector lists use only native choices and offer Next only to a suitable group", { timeout: 3000 }, async (t) => {
	const f = fixture(t);
	await f.appear();
	const original = streamDeck.ui.sendToPropertyInspector;
	let answer: { event?: string; items?: Array<{ value: string; label: string }>; text?: string } = {};
	streamDeck.ui.sendToPropertyInspector = async (payload) => { answer = payload as typeof answer; };
	t.after(() => { streamDeck.ui.sendToPropertyInspector = original; });
	const ask = async (event: string) => {
		await f.action.onSendToPlugin({ action: f.key, payload: { event } } as never);
		return answer.items;
	};
	assert.equal((await ask("getSceneControllers"))?.[0].value, "controller-a");
	assert.deepEqual((await ask("getViewGroups"))?.map((entry) => entry.value), ["mode-group", "camera-group", "overlay-group"]);
	assert.deepEqual((await ask("getViewChoices"))?.map((entry) => entry.value), ["game", "art"]);
	assert.deepEqual((await ask("getViewCommands"))?.map((entry) => entry.value), ["select", "next"]);
	await f.change({ ...selected, groupId: "overlay-group" });
	assert.deepEqual((await ask("getViewCommands"))?.map((entry) => entry.value), ["select"]);
	assert.deepEqual((await ask("getViewChoices"))?.map((entry) => entry.value), ["on", "off"]);
	await f.change({ controllerId: "controller-a", viewTarget: "view", viewId: "stream-view" });
	assert.deepEqual((await ask("getSavedViews"))?.map((entry) => entry.value), ["stream-view", "art-view"]);
	assert.deepEqual((await ask("getViewCommands"))?.map((entry) => entry.value), ["select"]);
	available = false;
	await f.appear();
	assert.equal(f.output.state, 0);
	assert.equal(f.output.title, "Missing\nController");
	await f.press();
	assert.equal(commands().length, 0, "an older StageKit cannot receive a blind visibility fallback");
});

test("disconnect clears the selected light and reconnect reads the current native state", { timeout: 5000 }, async (t) => {
	const f = fixture(t, { ...selected, palette: "violet", sourceType: "camera", layout: "chat" }, "game");
	const art = fixture(t, { ...selected, choiceId: "art" }, "art", f.action);
	await Promise.all([f.appear(), art.appear()]);
	assert.equal(f.output.state, 1);
	assert.match(f.output.images[0], /^data:image\/png;base64,/);
	const held = new Promise<void>((resolve) => { holdNextGet = () => resolve(); });
	changed();
	await held;
	const disconnected = once(obs, "connection");
	const offline = Promise.all([once(f.draws, "draw"), once(art.draws, "draw")]);
	server.clients.forEach((socket) => socket.terminate());
	assert.deepEqual(await disconnected, [false]);
	await offline;
	assert.equal(f.output.state, 0);
	assert.equal(f.output.title, "OBS\nOffline");
	assert.equal(art.output.title, "OBS\nOffline");
	const count = requests.length;
	await f.press();
	assert.equal(requests.length, count);
	assert.equal(f.output.alerts, 1);
	controllers[0].enabled = false;
	const connected = once(obs, "connection");
	const online = Promise.all([once(f.draws, "draw"), once(art.draws, "draw")]);
	obs.use({ host: "127.0.0.1", port });
	assert.deepEqual(await connected, [true]);
	await online;
	assert.equal(f.output.state, 0);
	assert.equal(f.output.title, "Disabled");
	assert.equal(art.output.title, "Disabled");
	assert.equal(requests.length, count + 1, "the new connection requests one fresh snapshot for both keys");
	assert.equal(f.output.settings.palette, "violet");
	assert.equal(f.output.settings.layout, "chat");
	assert.equal(f.output.settings.sourceType, "camera");
});
