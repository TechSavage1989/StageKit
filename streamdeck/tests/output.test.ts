// The Stream and Record keys (src/actions/output.ts, src/live.ts), against a
// stand-in for OBS's WebSocket server that streams and records as OBS does:
// each request answered, then its StreamStateChanged or RecordStateChanged
// events. The owner: "Id like animated stream Start buttons and stream Record
// buttons. both should be able to be set to toggle, start, pause or stop, and
// have animations that are relevant, and match the existing color themes",
// and then "the new keys also need a long press setting".

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { once } from "node:events";
import { after, before, test } from "node:test";

import { WebSocketServer } from "ws";

import { Animator, animator } from "../src/animate.ts";
import { decodePng, HOLD_BAR, withHoldBar } from "../src/compose.ts";
import { HoldPress } from "../src/hold.ts";
import { ACCENTS, FRAMES, framesOf, holdMs, isOn, longPresses, PRESSES, requestFor, shortTime, stateOfEvent, stateOfStatus } from "../src/live.ts";
import { obs } from "../src/obs.ts";
import { COLOURS, file, PICTURES, stateFile } from "../src/pictures.ts";
import { outputTitle } from "../src/titles.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// ------------------------------------------------------------ a stand-in OBS
const sent: string[] = [];
const out = { stream: false, record: false, paused: false, reconnecting: false };
let push: (type: string, data: Record<string, unknown>) => void = () => undefined;

function event(output: "stream" | "record", state: string, active: boolean): void {
	push(output === "stream" ? "StreamStateChanged" : "RecordStateChanged", { outputActive: active, outputState: `OBS_WEBSOCKET_OUTPUT_${state}` });
}

/** What OBS does with a request: false when it refuses (as obs-websocket does: starting what runs, say). */
function act(request: string): boolean {
	sent.push(request);
	const later = (f: () => void) => setTimeout(f, 5);
	const start = (o: "stream" | "record") => {
		if (out[o]) return false;
		later(() => {
			event(o, "STARTING", false);
			out[o] = true;
			event(o, "STARTED", true);
		});
		return true;
	};
	const stop = (o: "stream" | "record") => {
		if (!out[o]) return false;
		later(() => {
			event(o, "STOPPING", true);
			out[o] = false;
			out.paused = o === "record" ? false : out.paused;
			event(o, "STOPPED", false);
		});
		return true;
	};
	const pause = (to: boolean) => {
		if (!out.record || out.paused === to) return false;
		later(() => {
			out.paused = to;
			event("record", to ? "PAUSED" : "RESUMED", true);
		});
		return true;
	};
	switch (request) {
		case "StartStream":
			return start("stream");
		case "StopStream":
			return stop("stream");
		case "ToggleStream":
			return out.stream ? stop("stream") : start("stream");
		case "StartRecord":
			return start("record");
		case "StopRecord":
			return stop("record");
		case "ToggleRecord":
			return out.record ? stop("record") : start("record");
		case "PauseRecord":
			return pause(true);
		case "ResumeRecord":
			return pause(false);
		case "TogglePauseRecord":
			return pause(!out.paused);
	}
	return false;
}

let server: WebSocketServer;
let port = 0;

before(async () => {
	server = new WebSocketServer({ port: 0, handleProtocols: () => "obswebsocket.json" });
	await once(server, "listening");
	port = (server.address() as { port: number }).port;
	server.on("connection", (socket) => {
		const send = (op: number, d: unknown) => socket.send(JSON.stringify({ op, d }));
		push = (eventType, eventData) => send(5, { eventType, eventIntent: 64, eventData });
		send(0, { obsWebSocketVersion: "5.5.0", rpcVersion: 1 });
		socket.on("message", (raw) => {
			const { op, d } = JSON.parse(String(raw));
			if (op === 1) send(2, { negotiatedRpcVersion: 1 });
			if (op !== 6) return;
			let ok = true;
			let data: Record<string, unknown> | undefined;
			if (d.requestType === "GetStreamStatus")
				data = { outputActive: out.stream, outputReconnecting: out.reconnecting, outputTimecode: out.stream ? "01:02:03.400" : "00:00:00.000" };
			else if (d.requestType === "GetRecordStatus")
				data = { outputActive: out.record, outputPaused: out.paused, outputTimecode: out.record ? "00:04:05.000" : "00:00:00.000" };
			else ok = act(d.requestType);
			send(7, { requestType: d.requestType, requestId: d.requestId, requestStatus: { result: ok, code: ok ? 100 : 500 }, responseData: data });
		});
	});
	const connected = once(obs, "connection");
	obs.use({ host: "127.0.0.1", port });
	await connected;
});

after(() => {
	server.close();
	setTimeout(() => process.exit(process.exitCode ?? 0), 50);
});

// ----------------------------------------------------------------- a key
type Shown = { state?: number; title?: string; images: Array<{ image: string; state?: number }>; alerts: number };

function fakeKey(id: string, start: Record<string, unknown>) {
	let settings = start;
	const shown: Shown = { images: [], alerts: 0 };
	const key = {
		id,
		isKey: () => true,
		getSettings: async () => settings,
		setSettings: async (next: Record<string, unknown>) => {
			settings = next;
		},
		setImage: async (image?: string, options?: { state?: number }) => {
			shown.images.push({ image: String(image), state: options?.state });
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
	return { key, shown, settings: () => settings, set: (next: Record<string, unknown>) => (settings = next) };
}

type Action = {
	show(key: unknown, settings: Record<string, unknown>): Promise<void>;
	onWillAppear(ev: unknown): Promise<void>;
	onWillDisappear(ev: unknown): void;
	onKeyDown(ev: unknown): Promise<void>;
	onKeyUp(ev: unknown): void;
};

async function makeAction(output: "stream" | "record"): Promise<Action> {
	const { OutputKey } = await import("../src/actions/output.ts");
	return new OutputKey(output) as unknown as Action;
}

/** Waits for the stand-in OBS's events after a request to reach the link. */
function settled(output: "stream" | "record", until: string): Promise<void> {
	// (One listener for the whole wait: two events can come in one breath.)
	return new Promise((resolve) => {
		const heard = (which: string, state: string) => {
			if (which !== output || !String(state).endsWith(until)) return;
			obs.off("output", heard);
			resolve();
		};
		obs.on("output", heard);
	});
}

/** The image last given to the state the key is in. */
const last = (s: Shown) => s.images.filter((i) => i.state === undefined || i.state === s.state).at(-1)?.image ?? "";

// ------------------------------------------------------------------ tests
test("each On press sends its own obs-websocket request, and Pause is on the Record key only", () => {
	assert.deepEqual(
		PRESSES.stream.map(([v]) => [v, requestFor("stream", v)]),
		[
			["toggle", "ToggleStream"],
			["start", "StartStream"],
			["stop", "StopStream"],
		],
	);
	assert.deepEqual(
		PRESSES.record.map(([v]) => [v, requestFor("record", v)]),
		[
			["toggle", "ToggleRecord"],
			["start", "StartRecord"],
			["stop", "StopRecord"],
			["pause", "TogglePauseRecord"],
			["pauseonly", "PauseRecord"],
			["resume", "ResumeRecord"],
		],
	);
	// OBS cannot pause a stream: no request, nothing offered.
	for (const p of ["pause", "pauseonly", "resume"]) assert.equal(requestFor("stream", p), undefined, p);
	assert.equal(requestFor("stream", undefined), "ToggleStream", "an unset press is Toggle");
	assert.equal(requestFor("record", "none"), undefined, "a long press on Nothing sends nothing");
	assert.deepEqual(longPresses("stream")[0], ["none", "Nothing"]);
	const page = (k: string) => readFileSync(new URL(`ui/${k}.html`, plugin), "utf8");
	assert.ok(!/value="(pause|pauseonly|resume)"/.test(page("stream")), "the Stream key's page offers no pause");
	assert.match(page("stream"), /cannot pause a stream|not a stream/, "and says why in a tooltip");
	for (const v of ["pause", "pauseonly", "resume"]) assert.ok(page("record").includes(`value="${v}"`), v);
	for (const k of ["stream", "record"]) {
		for (const setting of ["press", "longPress", "holdMs", "animate", "showTime", "sceneType", "layout", "palette"])
			assert.ok(page(k).includes(`setting="${setting}"`), `${k}: ${setting}`);
		assert.ok(page(k).includes('setting="longPress" default="none"'), `${k}: a long press is Nothing as it comes`);
		assert.ok(page(k).includes('setting="animate" default="on"'), `${k}: animation on as it comes`);
	}
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	for (const k of ["stream", "record"]) {
		const action = manifest.Actions.find((a: { UUID: string }) => a.UUID === `com.techsavage1989.stagekit.${k}`);
		assert.ok(action, k);
		assert.equal(action.States.length, 2);
		assert.equal(action.DisableAutomaticStates, true, `${k}: the key shows what OBS says, never a flash`);
	}
});

test("OBS's states are read from its events and its status", () => {
	assert.equal(stateOfEvent("stream", "OBS_WEBSOCKET_OUTPUT_STARTED"), "live");
	assert.equal(stateOfEvent("stream", "OBS_WEBSOCKET_OUTPUT_RECONNECTING"), "reconnecting");
	assert.equal(stateOfEvent("stream", "OBS_WEBSOCKET_OUTPUT_RECONNECTED"), "live");
	assert.equal(stateOfEvent("record", "OBS_WEBSOCKET_OUTPUT_PAUSED"), "paused");
	assert.equal(stateOfEvent("record", "OBS_WEBSOCKET_OUTPUT_RESUMED"), "recording");
	assert.equal(stateOfEvent("record", "OBS_WEBSOCKET_OUTPUT_STOPPING"), "stopping");
	assert.equal(stateOfEvent("record", "OBS_WEBSOCKET_OUTPUT_UNKNOWN"), undefined);
	assert.equal(stateOfStatus("stream", { outputActive: true, outputReconnecting: true }), "reconnecting");
	assert.equal(stateOfStatus("record", { outputActive: true, outputPaused: true }), "paused");
	assert.equal(stateOfStatus("record", { outputActive: false }), "idle");
	for (const s of ["idle", "starting"]) assert.equal(isOn(s), false, s);
	for (const s of ["live", "recording", "paused", "reconnecting", "stopping"]) assert.equal(isOn(s), true, s);
	assert.equal(shortTime("01:02:03.400"), "1:02:03");
	assert.equal(shortTime("00:04:05.000"), "04:05");
	assert.equal(holdMs(undefined), 800);
	assert.equal(holdMs("50"), 300);
	assert.equal(holdMs("99999"), 5000);
});

test("every state's frames are drawn, in every palette, plain and marked on and off; the record dot is red in all", () => {
	assert.deepEqual(FRAMES.stream, { idle: 1, starting: 8, live: 8, reconnecting: 8, stopping: 8 });
	assert.deepEqual(FRAMES.record, { idle: 1, starting: 8, recording: 8, paused: 12, stopping: 8 });
	let n = 0;
	for (const output of ["stream", "record"] as const)
		for (const state of Object.keys(FRAMES[output])) {
			const frames = framesOf(output, state);
			assert.equal(frames.length, FRAMES[output][state]);
			for (const id of frames) {
				const picture = BY_ID.get(id);
				assert.ok(picture, id);
				for (const colour of COLOURS) {
					for (const f of [file(picture, true, colour), stateFile(picture, true, colour), stateFile(picture, false, colour)])
						assert.ok(existsSync(new URL(f, plugin)), f);
					n++;
				}
			}
			// Frames of a moving state change from each one to the next (a pulse comes back the way it went, so it may repeat itself).
			const looks = frames.map((id) => readFileSync(new URL(file(BY_ID.get(id)!, true, "ocean"), plugin)).toString("base64"));
			for (let i = 1; i < looks.length; i++) assert.notEqual(looks[i], looks[i - 1], `${output} ${state}: frames ${i - 1} and ${i} alike`);
			if (looks.length > 1) assert.ok(new Set(looks).size > looks.length / 2, `${output} ${state}: too few frames differ`);
		}
	assert.equal(n, 70 * COLOURS.length);
	for (const colour of COLOURS) {
		assert.ok(ACCENTS[colour]?.length === 3, colour);
		// The middle of the recording dot (the drawing's field is centred in the key's little screen).
		const image = decodePng(readFileSync(new URL(file(BY_ID.get("live/record-recording-4")!, true, colour), plugin)));
		const o = (49 * image.width + 55) * 4;
		const [r, g, b] = image.data.subarray(o, o + 3);
		assert.ok(r > 200 && g < 90 && b < 90, `${colour}: the dot is red (${r}, ${g}, ${b})`);
	}
	assert.notDeepEqual(ACCENTS.ember, ACCENTS.ocean, "the accents follow the palette");
});

test("the animator plays a loop, never more than its share at once, and stops", async () => {
	const a = new Animator(10, 2);
	const got: Record<string, string[]> = { k1: [], k2: [], k3: [] };
	for (const k of Object.keys(got)) a.play(k, ["f0", "f1", "f2"], 1, (image) => got[k].push(image));
	assert.equal(a.running, true);
	a.tick();
	assert.equal(got.k1.length + got.k2.length + got.k3.length, 2, "two keys a tick at most");
	a.tick();
	a.tick();
	assert.deepEqual([got.k1.length, got.k2.length, got.k3.length], [2, 2, 2], "taken in turn");
	assert.deepEqual(got.k1, ["f1", "f2"]);
	a.play("k1", ["f0", "f1", "f2"], 1, (image) => got.k1.push(image));
	while (got.k1.length < 3) a.tick();
	assert.equal(got.k1[2], "f0", "the same frames again carry on where they were, round the loop");
	a.rest("k2", true);
	const before = got.k2.length;
	a.tick();
	a.tick();
	assert.equal(got.k2.length, before, "a resting key holds still");
	a.play("k1", ["still"], 0, () => undefined);
	assert.equal(a.isPlaying("k1"), false, "one frame: stopped");
	a.stop("k2");
	a.stop("k3");
	assert.equal(a.running, false, "no key plays: no timer");
	// On its own timer.
	const b = new Animator(5, 4);
	const seen: string[] = [];
	b.play("x", ["a", "b"], 0, (image) => seen.push(image));
	await sleep(40);
	b.stop("x");
	const count = seen.length;
	assert.ok(count >= 3, `${count}`);
	await sleep(20);
	assert.equal(seen.length, count, "nothing after it stops");
});

test("a Stream key: a press sends its request, OBS's events change its state, image and title, and it moves only while on the deck", async () => {
	const action = await makeAction("stream");
	const k = fakeKey("stream-1", { press: "toggle", palette: "ember" });
	await action.onWillAppear({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(k.shown.state, 0, "off: not streaming");
	assert.ok(last(k.shown).endsWith("/ember/status/off/live/stream-idle-0.png"), last(k.shown));
	assert.equal(k.shown.title, "Stream");
	assert.equal(animator.isPlaying("stream-1"), false, "idle is still");

	const started = settled("stream", "STARTED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	action.onKeyUp({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(sent.at(-1), "ToggleStream");
	await started;
	await action.show(k.key, k.settings());
	assert.equal(k.shown.state, 1, "on while live");
	assert.ok(last(k.shown).endsWith("/ember/status/on/live/stream-live-0.png"), last(k.shown));
	assert.match(k.shown.title!, /^Stream\n\d\d:\d\d$/, `the time while live: ${k.shown.title}`);
	assert.equal(animator.isPlaying("stream-1"), true, "live moves");
	await sleep(400);
	// (Frame 0 is set for both states when the key is shown; the loop then sets the on state's.)
	const frames = k.shown.images.filter((i) => i.image.includes("/live/stream-live-") && !i.image.endsWith("-0.png"));
	assert.ok(frames.length >= 3, `frames shown: ${frames.length}`);
	assert.ok(frames.every((f) => f.state === 1 || f.state === undefined), "for the on state");
	assert.ok(new Set(frames.map((f) => f.image)).size >= 3, "different frames");

	// Off the deck: it stops.
	action.onWillDisappear({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(animator.isPlaying("stream-1"), false);
	const quiet = k.shown.images.length;
	await sleep(300);
	assert.equal(k.shown.images.length, quiet, "nothing sent to a key that is gone");

	// Animate Off: still.
	k.set({ ...k.settings(), animate: "off" });
	await action.onWillAppear({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(animator.isPlaying("stream-1"), false, "Animate Off holds the first frame");
	// The time can be left out.
	k.set({ ...k.settings(), showTime: "off" });
	await action.show(k.key, k.settings());
	assert.equal(k.shown.title, "Stream\nLIVE");

	// Start while live: OBS refuses, the key shows its alert.
	k.set({ ...k.settings(), press: "start" });
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	await sleep(50);
	assert.equal(sent.at(-1), "StartStream");
	assert.equal(k.shown.alerts, 1);

	// Stop: stopping, then idle and still.
	k.set({ ...k.settings(), press: "stop", animate: "on" });
	const stopped = settled("stream", "STOPPED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(sent.at(-1), "StopStream");
	await stopped;
	await action.show(k.key, k.settings());
	assert.equal(k.shown.state, 0);
	assert.equal(animator.isPlaying("stream-1"), false, "idle again: still");
	assert.equal(k.shown.title, "End\nStream");

	// A Scene type: the type's picture with whether it is live as a badge, not moving.
	k.set({ ...k.settings(), sceneType: "gameplay" });
	await action.show(k.key, k.settings());
	assert.ok(last(k.shown).startsWith("data:image/png;base64,"), "laid together with its badge");
	action.onWillDisappear({ action: k.key, payload: { settings: k.settings() } });
});

test("a Record key: start, pause and resume, stop; paused pulses, and OBS gone it keeps what it was told", async () => {
	const action = await makeAction("record");
	const k = fakeKey("record-1", { press: "start", palette: "neon" });
	await action.onWillAppear({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(k.shown.title, "Start\nRec");
	const started = settled("record", "STARTED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(sent.at(-1), "StartRecord");
	await started;
	await action.show(k.key, k.settings());
	assert.equal(k.shown.state, 1);
	assert.ok(last(k.shown).endsWith("/neon/status/on/live/record-recording-0.png"), last(k.shown));
	assert.match(k.shown.title!, /^Start Rec\n\d\d:\d\d$/);
	assert.equal(animator.isPlaying("record-1"), true);

	k.set({ ...k.settings(), press: "pause" });
	let heard = settled("record", "PAUSED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(sent.at(-1), "TogglePauseRecord");
	await heard;
	await action.show(k.key, k.settings());
	assert.ok(last(k.shown).endsWith("/neon/status/on/live/record-paused-0.png"), last(k.shown));
	assert.equal(k.shown.title, "Pause / Resume\nPaused");
	assert.equal(animator.isPlaying("record-1"), true, "paused pulses");
	heard = settled("record", "RESUMED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	await heard;
	await action.show(k.key, k.settings());
	assert.ok(last(k.shown).includes("record-recording-"), last(k.shown));

	k.set({ ...k.settings(), press: "resume" });
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	await sleep(50);
	assert.equal(sent.at(-1), "ResumeRecord");
	assert.equal(k.shown.alerts, 1, "resume while recording: OBS refuses, an alert");

	// OBS gone: the key shows what it was told last, still.
	assert.equal((k.settings().seen as { type?: string })?.type, "recording");
	server.clients.forEach((c) => c.terminate());
	await once(obs, "connection");
	await action.show(k.key, k.settings());
	assert.equal(k.shown.state, 1, "still recording, as far as it knows");
	assert.equal(animator.isPlaying("record-1"), false, "not moving while OBS cannot say");
	action.onWillDisappear({ action: k.key, payload: { settings: k.settings() } });
	while (!obs.isConnected) await once(obs, "connection");
	const stopped = settled("record", "STOPPED");
	await obs.outputRequest("StopRecord");
	await stopped;
});

test("a long press: a short press does the press, a long one the long press, once, at the hold time, and nothing on release", async () => {
	const action = await makeAction("stream");
	const k = fakeKey("stream-2", { press: "start", longPress: "stop", holdMs: "300", palette: "ocean" });
	await action.onWillAppear({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(k.shown.title, "Go Live\nHold: Stop", "the title says what a hold does");

	// Short: nothing on key down, the press on key up.
	const before = sent.length;
	let started = settled("stream", "STARTED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	await sleep(50);
	assert.equal(sent.length, before, "nothing yet: it might be a long press");
	action.onKeyUp({ action: k.key, payload: { settings: k.settings() } });
	await started;
	assert.deepEqual(sent.slice(before), ["StartStream"]);

	// Long: the long press at the hold time, the bar filling meanwhile, nothing on key up.
	await action.show(k.key, k.settings());
	const mark = k.shown.images.length;
	const stopped = settled("stream", "STOPPED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	await sleep(200);
	assert.equal(sent.length, before + 1, "not before the hold time");
	const bars = k.shown.images.slice(mark).filter((i) => i.image.startsWith("data:image/png"));
	assert.ok(bars.length >= 1, "a bar fills while held");
	await sleep(250);
	assert.deepEqual(sent.slice(before), ["StartStream", "StopStream"], "the long press, once, while still held");
	action.onKeyUp({ action: k.key, payload: { settings: k.settings() } });
	await sleep(50);
	assert.deepEqual(sent.slice(before), ["StartStream", "StopStream"], "and nothing on release");
	await stopped;

	// Long press Nothing: on key down at once, as before.
	k.set({ press: "toggle", longPress: "none", palette: "ocean" });
	started = settled("stream", "STARTED");
	await action.onKeyDown({ action: k.key, payload: { settings: k.settings() } });
	assert.equal(sent.at(-1), "ToggleStream", "no wait");
	action.onKeyUp({ action: k.key, payload: { settings: k.settings() } });
	await started;
	assert.equal(sent.filter((s) => s === "ToggleStream").length >= 1, true);
	const stop2 = settled("stream", "STOPPED");
	await obs.outputRequest("StopStream");
	await stop2;
	action.onWillDisappear({ action: k.key, payload: { settings: k.settings() } });
});

test("the hold helper on its own, and the bar it draws", async () => {
	const h = new HoldPress();
	const did: string[] = [];
	const plan = (long: boolean) => ({ holdMs: 60, long, short: () => did.push("short"), longPress: () => did.push("long"), progress: (f: number) => did.push(`p${f >= 1 ? 1 : 0}`) });
	h.down("a", plan(false));
	assert.deepEqual(did, ["short"], "no long press: at once");
	assert.equal(h.up("a"), "none");
	did.length = 0;
	h.down("a", plan(true));
	assert.equal(h.isHeld("a"), true);
	assert.equal(h.up("a"), "short");
	await sleep(5);
	assert.deepEqual(did, ["short"]);
	did.length = 0;
	h.down("a", plan(true));
	await sleep(120);
	assert.equal(h.up("a"), "long");
	assert.equal(did.filter((d) => d === "long").length, 1);
	assert.ok(!did.includes("short"));
	did.length = 0;
	h.down("a", plan(true));
	h.cancel("a");
	await sleep(100);
	assert.deepEqual(did, [], "a key gone while held does nothing");

	// The bar: the palette's accent, filled to the fraction, on the key's own image.
	const frame = "imgs/pack/forest/status/on/live/stream-live-0.png";
	const half = decodePng(Buffer.from(withHoldBar(frame, 0.5, ACCENTS.forest).split(",")[1], "base64"));
	const y = (HOLD_BAR.y0 + HOLD_BAR.y1) >> 1;
	const at = (x: number) => Array.from(half.data.subarray((y * half.width + x) * 4, (y * half.width + x) * 4 + 3));
	assert.deepEqual(at(HOLD_BAR.x0 + 5), ACCENTS.forest);
	assert.notDeepEqual(at(HOLD_BAR.x1 - 5), ACCENTS.forest);
	assert.equal(withHoldBar(frame, 0.5, ACCENTS.forest), withHoldBar(frame, 0.52, ACCENTS.forest), "made once a step");
});

test("titles: what a press does, what OBS is doing, and what a hold does", () => {
	assert.equal(outputTitle("stream", "toggle", "idle"), "Stream");
	assert.equal(outputTitle("stream", "start", "idle"), "Go Live");
	assert.equal(outputTitle("stream", "stop", "live", "12:34"), "End Stream\n12:34");
	assert.equal(outputTitle("stream", "toggle", "live"), "Stream\nLIVE");
	assert.equal(outputTitle("stream", "toggle", "reconnecting"), "Stream\nReconnect");
	assert.equal(outputTitle("stream", "toggle", "starting", "00:01"), "Stream\nStarting");
	assert.equal(outputTitle("record", "toggle", "recording", "1:02:03"), "Record\n1:02:03");
	assert.equal(outputTitle("record", "pauseonly", "paused"), "Pause Rec\nPaused");
	assert.equal(outputTitle("record", "start", "idle", undefined, "stop"), "Start Rec\nHold: Stop");
	assert.equal(outputTitle("record", "start", "recording", "04:05", "stop"), "Start Rec\n04:05\nHold: Stop", "three lines at most");
	assert.equal(outputTitle("stream", "toggle", "idle", undefined, "none"), "Stream", "no long press: no hold line");
	assert.equal(outputTitle("record", "pause", "paused", undefined, "stop"), "Pause\nPaused\nHold: Stop", "a long name gives way to its first word with three lines");
});
