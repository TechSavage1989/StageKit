// Every key's settings page starts with *Works with*: what the key works in
// OBS (the kind of filter exactly as OBS's Filters window names it, or the
// feature) and, under it, what the key is set to. The owner (2026-11-18): "I
// want the stream deck keys to show the filter or feature they work with
// somewhere outside its [list]. somewhere in the configuration area at the
// top for all keys."

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import vm from "node:vm";

import features from "../src/features.json" with { type: "json" };
import { shownName } from "../src/kinds.ts";
import { chosenLine, firstLine, FOLLOWS, NOT_CHOSEN, WORKS_WITH, worksWithOf, worksWithRow } from "../src/works-with.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8")) as { Actions: Array<{ UUID: string; PropertyInspectorPath: string }> };
const keyOf = (uuid: string) => uuid.slice(uuid.lastIndexOf(".") + 1);
const pageOf = (path: string) => readFileSync(new URL(path, plugin), "utf8");
const SEP = "\u001f";
const DASH = "—";

/** As Python's html.escape(text, quote=True), which wrote the pages. */
const escape = (text: string) => text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#x27;");

/** The row of a page: its tooltip, the settings it follows and its two lines, as written. */
function rowOf(page: string): { tip: string; follow: string; kind: string; chosen: string; html: string } {
	const found = page.match(
		/<sdpi-item class="works" label="Works with" title="([^"]*)">\n\t\t\t<div id="worksWith" data-follow="([^"]*)">\n\t\t\t\t<div class="works-kind">([^<]*)<\/div>\n\t\t\t\t<div class="works-chosen">([^<]*)<\/div>\n\t\t\t<\/div>\n\t\t<\/sdpi-item>\n/,
	);
	assert.ok(found, "a Works with row");
	return { tip: found[1], follow: found[2], kind: found[3], chosen: found[4], html: found[0] };
}

test("every key in the manifest says what it works with, and the table has no key the manifest lacks", () => {
	const keys = manifest.Actions.map((a) => keyOf(a.UUID));
	for (const key of keys) assert.ok(WORKS_WITH[key], `${key}: no Works with entry`);
	assert.deepEqual(Object.keys(WORKS_WITH).sort(), [...keys].sort());
	for (const [key, works] of Object.entries(WORKS_WITH)) {
		assert.ok((works.kinds?.length ?? 0) > 0 || works.feature, `${key}: says neither a filter nor a feature`);
		assert.ok(works.tip.length > 20, `${key}: a tooltip`);
		// (The kinds are OBS's own names, Move Scene Item among them; the rest is our wording.)
		assert.ok(!/\bpicture\b|\bitem\b/i.test(`${works.feature ?? ""} ${works.note ?? ""} ${works.tip}`.replace(/Move Scene Item|Match Item/g, "")), `${key}: image and source, not picture and item`);
	}
	assert.equal(worksWithOf("com.techsavage1989.stagekit.showinscenes"), WORKS_WITH.showinscenes, "found by its UUID");
	assert.equal(worksWithOf("nothing"), undefined);
});

test("every settings page starts with the row, in one look, as information and not a control", () => {
	const scripts = new Set<string>();
	for (const action of manifest.Actions) {
		const page = pageOf(action.PropertyInspectorPath);
		const key = keyOf(action.UUID);
		const works = WORKS_WITH[key];
		const body = page.slice(page.indexOf("<body>\n") + "<body>\n".length);
		const row = rowOf(page);
		assert.ok(body.startsWith(`\t\t${row.html}`), `${action.PropertyInspectorPath}: the row is the first thing on the page`);
		assert.equal(page.split('label="Works with"').length, 2, "once");
		// The text per key is the table's.
		assert.equal(row.kind, escape(firstLine(works)), `${key}: its first line`);
		assert.equal(row.chosen, escape(works.note ?? ""), `${key}: its second line as written`);
		assert.equal(row.tip, escape(works.tip), `${key}: its tooltip`);
		assert.equal(row.follow, works.chosen ? FOLLOWS[works.chosen].join(" ") : "", `${key}: the lists it follows`);
		// Information: no setting, no input.
		assert.ok(!/setting=|<sdpi-(select|textfield|checkbox)|<input/.test(row.html), `${key}: no control in the row`);
		// What it follows is on the page, so a change is heard.
		if (works.chosen) assert.ok(FOLLOWS[works.chosen].some((s) => page.includes(`setting="${s}"`)), `${key}: none of ${FOLLOWS[works.chosen]} on the page`);
		// The beta189 line under the list said the kind again: the row says it now.
		assert.ok(!page.includes('class="which"'), `${key}: the line under the list is gone`);
		const script = page.match(/<script>\n\t\t\t\/\/ Works with:[\s\S]*?<\/script>/);
		assert.ok(script, `${key}: the row's script`);
		scripts.add(script[0]);
		// (One look on every page.)
		assert.ok(page.includes("#worksWith { line-height: 1.4;") && page.includes(".works-chosen { color: #8a8a8a; }"));
	}
	assert.equal(scripts.size, 1, "the same script on every page");
});

test("a key for a filter names its kind exactly as OBS does, and those names are the ones in OBS's locale", () => {
	const locale = readFileSync(new URL("../../data/locale/en-US.ini", import.meta.url), "utf8");
	const names = new Set([...locale.matchAll(/^[A-Za-z]+\.Name="(.*)"$/gm)].map((m) => m[1]));
	for (const [key, works] of Object.entries(WORKS_WITH))
		for (const kind of works.kinds ?? []) {
			assert.ok(names.has(shownName(kind)), `${key}: ${shownName(kind)} is not a filter name in en-US.ini`);
			assert.ok(firstLine(works).includes(shownName(kind)), `${key}: ${kind}`);
		}
	// One key per feature, for its own kind.
	for (const f of [...features.switches, ...features.moves, ...features.scenes]) assert.deepEqual(WORKS_WITH[f.key].kinds, [f.kind], f.key);
	assert.equal(firstLine(WORKS_WITH.showinscenes), "Source Filter: Show in Scenes (StageKit)");
	assert.equal(firstLine(WORKS_WITH.mode), "Scene Filter: Scene Modes (StageKit)");
	assert.equal(firstLine(WORKS_WITH.fitoverlap), "Scene Filter: Fit to Space (StageKit) or Scene Filter: Scene Fit (StageKit)");
	assert.equal(
		firstLine(WORKS_WITH.appswitch),
		"Source Filter: Automatic Window Switcher (StageKit), Scene Filter: Automatic Source Switcher (StageKit) or Source Filter: Auto Capture (StageKit)",
	);
	// The keys that are not a filter's say their feature.
	assert.equal(firstLine(WORKS_WITH.scene), "OBS scenes");
	assert.equal(firstLine(WORKS_WITH.stream), "OBS streaming");
	assert.equal(firstLine(WORKS_WITH.record), "OBS recording");
	assert.equal(firstLine(WORKS_WITH.tool), "StageKit dock: Arrange tools");
	assert.equal(firstLine(WORKS_WITH.hook), "A Game Capture or Window Capture");
});

test("what a key is set to is said from its settings, and what OBS says of it", () => {
	const show = WORKS_WITH.showinscenes;
	// Before a choice: the kind alone.
	assert.deepEqual(worksWithRow(show, {}), { kind: "Source Filter: Show in Scenes (StageKit)", chosen: NOT_CHOSEN });
	// Chosen: the source and the filter, from the key's one setting (OBS closed: nothing more).
	const camera = { target: `Square Camera${SEP}Show in Scenes-Camera` };
	assert.equal(chosenLine(show, camera), `Square Camera ${DASH} Show in Scenes-Camera`);
	assert.deepEqual(worksWithRow(show, camera), { kind: "Source Filter: Show in Scenes (StageKit)", chosen: `Square Camera ${DASH} Show in Scenes-Camera` });
	// A key saved with the two older settings.
	assert.equal(chosenLine(WORKS_WITH.switch, { source: "Game", filter: "Ramp Blur" }), `Game ${DASH} Ramp Blur`);
	assert.equal(chosenLine(show, { target: `Half${SEP}` }), undefined, "half a choice is none");
	// A key that works several kinds says the chosen one's own once OBS has said it.
	const apps = { target: `Window Capture${SEP}Apps` };
	assert.equal(worksWithRow(WORKS_WITH.appswitch, apps).kind, firstLine(WORKS_WITH.appswitch), "OBS closed: every kind");
	assert.deepEqual(worksWithRow(WORKS_WITH.appswitch, apps, { kind: "stagekit_app_capture" }), {
		kind: "Source Filter: Automatic Window Switcher (StageKit)",
		chosen: `Window Capture ${DASH} Apps`,
	});
	assert.equal(worksWithRow(WORKS_WITH.switch, { target: `Game Scene${SEP}Fit` }, { kind: "stagekit_fit_item" }).kind, "Scene Filter: Fit to Space (StageKit)");
	assert.equal(worksWithRow(WORKS_WITH.switch, { target: `x${SEP}y` }, { kind: "color_filter" }).kind, "Any StageKit filter but a move", "only a StageKit kind is said");
	// Gone from OBS.
	assert.equal(worksWithRow(show, camera, { missing: true }).chosen, `Square Camera ${DASH} Show in Scenes-Camera (not found in OBS)`);
	// The Scene Mode key's filter, a key saved before beta175 too.
	assert.equal(worksWithRow(WORKS_WITH.mode, { modeFilter: `Standby${SEP}Holding modes` }).chosen, `Standby ${DASH} Holding modes`);
	assert.equal(worksWithRow(WORKS_WITH.mode, { target: `Standby${SEP}Holding modes${SEP}m2` }).chosen, `Standby ${DASH} Holding modes`);
	assert.equal(worksWithRow(WORKS_WITH.mode, {}).chosen, NOT_CHOSEN);
	// A Scene Blur: one scene, or every scene with a filter of that name.
	assert.equal(worksWithRow(WORKS_WITH.sceneblur, { filter: "Blur", scene: "Live" }).chosen, `Live ${DASH} Blur`);
	assert.equal(worksWithRow(WORKS_WITH.sceneblur, { filter: "Blur", scene: "" }).chosen, `All scenes with this filter ${DASH} Blur`);
	assert.equal(worksWithRow(WORKS_WITH.sceneblur, { scene: "Live" }).chosen, NOT_CHOSEN);
	// A capture, a scene.
	assert.equal(worksWithRow(WORKS_WITH.hook, { source: "Game" }, { what: "Game Capture" }).chosen, "Game (Game Capture)");
	assert.equal(worksWithRow(WORKS_WITH.scene, { scene: "Live" }).chosen, "Live");
	assert.equal(worksWithRow(WORKS_WITH.scene, {}).chosen, NOT_CHOSEN);
	// A key set to nothing in OBS keeps its note, or has no second line.
	assert.deepEqual(worksWithRow(WORKS_WITH.tool, { tool: "left" }), { kind: "StageKit dock: Arrange tools", chosen: "On the sources selected in OBS" });
	assert.deepEqual(worksWithRow(WORKS_WITH.stream, {}), { kind: "OBS streaming", chosen: "" });
	assert.equal(worksWithRow(WORKS_WITH.blurall, {}).kind, "Source Filter: Blur (StageKit)");
});

test("with OBS closed the plugin answers with the kind and the saved choice", async () => {
	const { worksWithNow } = await import("../src/actions/shared.ts");
	assert.deepEqual(await worksWithNow("com.techsavage1989.stagekit.showinscenes", { target: `Square Camera${SEP}Show in Scenes-Camera` }), {
		kind: "Source Filter: Show in Scenes (StageKit)",
		chosen: `Square Camera ${DASH} Show in Scenes-Camera`,
	});
	assert.deepEqual(await worksWithNow("com.techsavage1989.stagekit.blur", {}), { kind: "Source Filter: Blur (StageKit)", chosen: NOT_CHOSEN });
	assert.equal(await worksWithNow("com.techsavage1989.stagekit.nothing", {}), undefined);
});

/** The row's script of a page, run as a page would, with a stand-in for sdpi-components' client. */
function runScript(page: string, client: unknown) {
	const script = page.match(/<script>\n(\t\t\t\/\/ Works with:[\s\S]*?)<\/script>/)![1];
	const row = rowOf(page);
	const kind = { textContent: row.kind };
	const chosen = { textContent: row.chosen };
	const lists = new Map<string, Array<{ fire: (type: string) => void }>>();
	const listen = (setting: string) => {
		const handlers: Record<string, Array<() => void>> = {};
		const list = {
			addEventListener: (type: string, fn: () => void) => (handlers[type] ??= []).push(fn),
			fire: (type: string) => (handlers[type] ?? []).forEach((fn) => fn()),
		};
		lists.set(setting, [...(lists.get(setting) ?? []), list]);
		return list;
	};
	const document = {
		getElementById: (id: string) =>
			id === "worksWith"
				? { dataset: { follow: row.follow }, querySelector: (q: string) => (q === ".works-kind" ? kind : q === ".works-chosen" ? chosen : null) }
				: null,
		querySelectorAll: (q: string) => {
			const setting = q.match(/^\[setting="(.*)"\]$/)?.[1];
			return setting && page.includes(`setting="${setting}"`) ? [listen(setting)] : [];
		},
	};
	const context: Record<string, unknown> = { document, setTimeout, Promise, SDPIComponents: client === undefined ? undefined : { streamDeckClient: client } };
	context.window = context;
	vm.runInNewContext(script, context);
	return { kind, chosen, lists };
}

test("the page asks the plugin when it opens and when its list changes, and shows the answer", async () => {
	const page = pageOf("ui/showinscenes.html");
	const sent: Array<{ event: string; payload: unknown }> = [];
	const heard: Array<(ev: unknown) => void> = [];
	const client = {
		send: async (event: string, payload: unknown) => {
			sent.push({ event, payload });
		},
		sendToPropertyInspector: { subscribe: (fn: (ev: unknown) => void) => heard.push(fn) },
	};
	const shown = runScript(page, client);
	await new Promise((r) => setTimeout(r, 10));
	// (Made in the page's own realm: compared as JSON.)
	assert.deepEqual(JSON.parse(JSON.stringify(sent)), [{ event: "sendToPlugin", payload: { event: "getWorksWith" } }], "asked once the page opens");
	assert.equal(shown.kind.textContent, "Source Filter: Show in Scenes (StageKit)", "until then, as written");
	assert.equal(shown.chosen.textContent, "");
	// Another list's answer is not this row's.
	heard.forEach((fn) => fn({ event: "sendToPropertyInspector", payload: { event: "getTargets", items: [] } }));
	assert.equal(shown.chosen.textContent, "");
	const answer = { event: "getWorksWith", kind: "Source Filter: Show in Scenes (StageKit)", chosen: `Square Camera ${DASH} Show in Scenes-Camera` };
	heard.forEach((fn) => fn({ event: "sendToPropertyInspector", payload: answer }));
	assert.equal(shown.chosen.textContent, `Square Camera ${DASH} Show in Scenes-Camera`);
	// The list changes: asked again once it has been saved.
	shown.lists.get("target")![0].fire("valuechange");
	assert.equal(sent.length, 1, "not at once");
	await new Promise((r) => setTimeout(r, 260));
	assert.equal(sent.length, 2, "asked again");
	// An answer with no kind keeps the one there.
	heard.forEach((fn) => fn({ payload: { event: "getWorksWith", kind: "", chosen: NOT_CHOSEN } }));
	assert.equal(shown.kind.textContent, "Source Filter: Show in Scenes (StageKit)");
	assert.equal(shown.chosen.textContent, NOT_CHOSEN);
});

test("a page with no client to ask keeps its row as written", () => {
	for (const path of ["ui/showinscenes.html", "ui/stream.html", "ui/mode.html"]) {
		const page = pageOf(path);
		const shown = runScript(page, undefined);
		assert.equal(shown.kind.textContent, rowOf(page).kind);
		const refused = runScript(page, {
			send: () => Promise.reject(new Error("no connection")),
			sendToPropertyInspector: { subscribe: () => {} },
		});
		assert.equal(refused.chosen.textContent, rowOf(page).chosen, "a send refused changes nothing, and throws nothing");
	}
});
