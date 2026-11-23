import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

import { canStepView, changedViewSettings, readSceneController, readSceneControllers, viewState, type ViewSettings } from "../src/views.ts";
import { COLOURS, layersOf, viewShown } from "../src/pictures.ts";
import { viewTitle } from "../src/titles.ts";
import { controllerSnapshot } from "./view-fixture.ts";

const select: ViewSettings = { controllerId: "controller-a", viewTarget: "group", groupId: "mode-group", choiceId: "game", viewPress: "select" };

test("View selections use stable controller, group and choice identities across renames", () => {
	const controller = controllerSnapshot();
	assert.deepEqual(viewState(select, controller, true).command, { controllerId: "controller-a", groupId: "mode-group", choiceId: "game", command: "select" });
	controller.name = "Renamed controller";
	controller.scene = "Renamed scene";
	controller.groups[0].name = "Renamed group";
	controller.groups[0].choices[0].name = "Renamed choice";
	const state = viewState(select, readSceneController(controller), true);
	assert.equal(state.name, "Renamed choice");
	assert.equal(state.active, true);
	assert.equal(state.command?.choiceId, "game");
	assert.equal(viewState(select, { ...controller, controllerId: "other-controller" }, true).command, undefined);
	assert.equal(viewState({ ...select, groupId: "camera-group" }, controller, true).status, "missing", "choice lookup never escapes its group");
	assert.equal(viewState({ ...select, choiceId: "Renamed choice" }, controller, true).status, "missing", "names never substitute for IDs");
});

test("disabled, missing, unavailable and offline selections cannot stay lit or send a command", () => {
	const initial = controllerSnapshot();
	for (const [name, alter] of [
		["controller disabled", (c) => { c.enabled = false; }],
		["controller unavailable", (c) => { c.available = false; }],
		["group disabled", (c) => { c.groups[0].enabled = false; }],
		["group unavailable", (c) => { c.groups[0].available = false; }],
		["choice unavailable", (c) => { c.groups[0].choices[0].available = false; }],
		["choice removed", (c) => { c.groups[0].choices = []; }],
	] as Array<[string, (c: ReturnType<typeof controllerSnapshot>) => void]>) {
		const controller = structuredClone(initial);
		alter(controller);
		const state = viewState(select, controller, true);
		assert.equal(state.active, false, name);
		assert.equal(state.command, undefined, name);
		assert.notEqual(state.status, "ready", name);
	}
	for (const state of [viewState(select, initial, false), viewState(select, undefined, true), viewState({}, initial, true)]) {
		assert.equal(state.active, false);
		assert.equal(state.command, undefined);
	}
});

test("saved views display native combined state and select only the saved view ID", () => {
	const controller = controllerSnapshot();
	const settings = { controllerId: controller.controllerId, viewTarget: "view", viewId: "stream-view" };
	assert.deepEqual(viewState(settings, controller, true).command, { controllerId: "controller-a", viewId: "stream-view", command: "select" });
	controller.views[0].active = false;
	assert.equal(viewState(settings, controller, true).active, false, "matching individual choices do not override the native combined-view state");
	controller.views[0].available = false;
	controller.views[0].reason = "A referenced group was deleted";
	assert.equal(viewState(settings, controller, true).reason, controller.views[0].reason);
	assert.equal(viewState(settings, controller, true).command, undefined);
	assert.equal(viewState({ ...settings, viewPress: "next" }, controller, true).command, undefined);
});

test("Next follows the native group order and selected state, while Together exposes explicit On and Off", () => {
	const controller = controllerSnapshot();
	const next = { ...select, viewPress: "next", choiceId: "irrelevant" };
	assert.deepEqual(viewState(next, controller, true).command, { controllerId: "controller-a", groupId: "mode-group", command: "next" });
	assert.equal(viewState(next, controller, true).name, "Game");
	controller.groups[0].current = "art";
	controller.groups[0].choices[1].active = true;
	controller.groups[0].choices[0].active = false;
	assert.equal(viewState(next, controller, true).name, "Art");
	controller.groups[0].current = "";
	assert.equal(viewState(next, controller, true).active, false);
	controller.groups[0].choices[1].available = false;
	assert.equal(canStepView(controller.groups[0]), false, "one available choice is not a meaningful cycle");
	assert.equal(viewState(next, controller, true).command, undefined);
	const off = viewState({ ...select, groupId: "overlay-group", choiceId: "off" }, controller, true);
	assert.deepEqual(off.command, { controllerId: "controller-a", groupId: "overlay-group", choiceId: "off", command: "select" });
	assert.equal(off.active, false);
	assert.equal(viewState({ ...next, groupId: "overlay-group" }, controller, true).command, undefined);
	assert.equal(viewState({ ...select, groupId: "follow-group" }, controller, true).command, undefined);
});

test("changing controller or group clears dependent IDs without copying or changing native choices", () => {
	const settings = { ...select, viewId: "stream-view", palette: "ocean", sourceType: "camera" };
	const changed = changedViewSettings(settings, { ...settings, controllerId: "controller-b" });
	assert.deepEqual(changed, { controllerId: "controller-b", viewTarget: "group", viewPress: "select", palette: "ocean", sourceType: "camera" });
	const group = changedViewSettings(settings, { ...settings, groupId: "camera-group" });
	assert.equal(group.choiceId, undefined);
	assert.equal(group.viewId, "stream-view");
	assert.equal(changedViewSettings(undefined, settings), settings, "initial appearance preserves existing saved bindings");
	assert.equal(changedViewSettings(settings, settings), settings);
	assert.equal(changedViewSettings({ ...settings, viewPress: "next" }, { ...settings, viewTarget: "view", viewPress: "next" }).viewPress, "select");
});

test("native snapshots reject mismatched identities and ambiguous IDs and fail closed on missing flags", () => {
	const controller = controllerSnapshot();
	assert.equal(readSceneController(controller, "other"), undefined);
	assert.equal(readSceneController(null), undefined);
	assert.deepEqual(readSceneControllers({ success: false }), []);
	assert.deepEqual(readSceneControllers({ controllers: [controller, controller] }), []);
	assert.equal(readSceneControllers({ controllers: [null, controller] }).length, 1);
	controller.groups.push(structuredClone(controller.groups[0]));
	assert.equal(readSceneController(controller)?.groups.some((group) => group.id === "mode-group"), false);
	const duplicate = controllerSnapshot();
	duplicate.groups[0].choices.push(structuredClone(duplicate.groups[0].choices[0]));
	assert.equal(readSceneController(duplicate)?.groups[0].choices.some((choice) => choice.id === "game"), false);
	const untrusted = { ...controllerSnapshot(), enabled: undefined, available: undefined };
	assert.equal(viewState(select, readSceneController(untrusted), true).active, false);
});

test("View presentation has shared type, layout, palette and custom-title defaults with a distinct action icon", () => {
	const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	const action = manifest.Actions.find((entry: { UUID: string }) => entry.UUID.endsWith(".view"));
	assert.equal(manifest.Version, "0.43.0.0");
	assert.equal(action.Name, "View");
	assert.equal(action.DisableAutomaticStates, true);
	assert.equal(action.States.length, 2);
	assert.equal(action.Icon, "imgs/actions/view/icon");
	const page = readFileSync(new URL(action.PropertyInspectorPath, plugin), "utf8");
	const settings = [...new Set([...page.matchAll(/<sdpi-[^>]+\bsetting="([^"]+)"/g)].map((match) => match[1]))].sort();
	assert.deepEqual(settings, ["choiceId", "controllerId", "groupId", "host", "layout", "palette", "password", "port", "sourceType", "viewId", "viewPress", "viewTarget"]);
	assert.match(page, /<summary>Image<\/summary>/);
	assert.match(page, /getSceneControllers/);
	assert.match(page, /getViewGroups/);
	assert.match(page, /getViewChoices/);
	assert.match(page, /getSavedViews/);
	assert.match(page, /getViewCommands/);
	assert.equal(viewShown().picture.id, "options/view");
	assert.equal(viewShown("camera").badge, "view");
	assert.equal(viewShown(undefined, "game").badge, "view");
	assert.equal(viewShown("camera", "game").picture.id, viewShown("camera").picture.id);
	for (const palette of COLOURS)
		for (const active of [false, true]) {
			assert.ok(existsSync(new URL(layersOf(viewShown(), palette, active).base, plugin)));
			assert.ok(existsSync(new URL(`imgs/pack/${palette}/badges/view.png`, plugin)));
		}
	assert.equal(viewTitle("Game"), "Game");
	assert.equal(viewTitle("A long saved view"), "A long\nsaved view");
	assert.equal(viewTitle(undefined), "View");
});

test("View inspector follows parent choices without a refresh loop and shows only the relevant selectors", async () => {
	const page = readFileSync(new URL("../com.techsavage1989.stagekit.sdPlugin/ui/view.html", import.meta.url), "utf8");
	const script = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((match) => match[1]).find((text) => text.includes("const dependencies"));
	assert.ok(script);
	const settings = ["controllerId", "viewTarget", "groupId", "choiceId", "viewId", "viewPress"];
	const controls = Object.fromEntries(settings.map((setting) => {
		const handlers: Record<string, Array<() => void>> = {};
		const row = { hidden: false, style: { display: "" } };
		return [setting, {
			value: setting === "viewTarget" ? "group" : setting === "viewPress" ? "select" : "",
			refreshes: 0, row,
			getAttribute: (name: string) => name === "setting" ? setting : "",
			closest: () => row,
			addEventListener: (name: string, handler: () => void) => { (handlers[name] ??= []).push(handler); },
			emit: () => { handlers.valuechange?.forEach((handler) => handler()); },
			refresh() { this.refreshes++; this.emit(); },
		}];
	}));
	let next = 0;
	const timers = new Map<number, () => void>();
	const status = { textContent: "" };
	let receive!: (event: { payload: { event: string; text: string } }) => void;
	runInNewContext(script, {
		document: { querySelectorAll: () => Object.values(controls), getElementById: () => status },
		window: { SDPIComponents: { streamDeckClient: {
			send: async () => {},
			sendToPropertyInspector: { subscribe: (listener: typeof receive) => { receive = listener; } },
			didReceiveSettings: { subscribe: () => {} },
		} } },
		customElements: { whenDefined: () => Promise.resolve() },
		requestAnimationFrame: (callback: () => void) => callback(),
		setTimeout: (callback: () => void) => { timers.set(++next, callback); return next; },
		clearTimeout: (id: number) => { timers.delete(id); },
	});
	await Promise.resolve();
	const flush = () => {
		let count = 0;
		while (timers.size) {
			assert.ok(++count < 10, "refreshing a child must not refresh itself or its parent");
			const [id, callback] = timers.entries().next().value!;
			timers.delete(id);
			callback();
		}
	};
	controls.controllerId.value = "controller-a";
	controls.controllerId.emit();
	flush();
	assert.equal(controls.controllerId.refreshes, 0);
	for (const setting of ["groupId", "choiceId", "viewId", "viewPress"]) assert.ok(controls[setting].refreshes > 0);
	assert.equal(controls.viewId.row.hidden, true);
	assert.equal(controls.choiceId.row.hidden, false);
	controls.viewPress.value = "next";
	controls.viewPress.emit();
	flush();
	assert.equal(controls.choiceId.row.hidden, true);
	assert.equal(controls.groupId.row.hidden, false);
	controls.viewTarget.value = "view";
	controls.viewTarget.emit();
	flush();
	assert.equal(controls.groupId.row.hidden, true);
	assert.equal(controls.viewId.row.hidden, false);
	receive({ payload: { event: "getViewStatus", text: "A missing <source>" } });
	assert.equal(status.textContent, "A missing <source>", "OBS text is plain text, never page markup");
});
