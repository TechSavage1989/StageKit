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

function answer(requestType: string, data: Record<string, unknown> = {}): { ok: boolean; data?: unknown } {
	const list = filters[String(data.sourceName)];
	switch (requestType) {
		case "GetInputList":
			return { ok: true, data: { inputs: [{ inputName: "camera" }] } };
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
		case "TriggerHotkeyByName":
			pressed.push(String(data.hotkeyName));
			return { ok: true };
		case "CallVendorRequest": {
			if (data.vendorName !== "StageKit") return { ok: false };
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
	assert.equal(await obs.stageKit("NoSuchRequest"), undefined, "and one it refuses comes back as nothing");
});
