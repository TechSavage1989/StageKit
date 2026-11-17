// Every key's type lists (src/key-types.ts): what replaced the Image list.

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };
import { adoptOld, keptPicture, keysWithLists, OWN_LISTS, SCENE_TYPES, SOURCE_TYPES, typeForKept, typeLists } from "../src/key-types.ts";
import {
	blurAllPicture,
	chooseFor,
	COLOURS,
	file,
	hookPicture,
	modePicture,
	movePicture,
	overlapPicture,
	PICTURES,
	scenePicture,
	stateFile,
} from "../src/pictures.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));
const KIND = new Map([...features.switches, ...features.moves].map((f) => [f.key, f.kind]));

/** The picture a key shows with one of its lists set, as the key itself chooses it. */
function shown(key: string, setting: string, value: string): string | undefined {
	const types = { [setting]: value };
	if (key === "scene") return scenePicture(undefined, "", value).id;
	if (key === "mode") return modePicture(undefined, value).id;
	if (key === "hook") return hookPicture(undefined, "game_capture", undefined, value).id;
	if (key === "blurall") return blurAllPicture(undefined, value).id;
	if (key === "fitoverlap") return overlapPicture(undefined, value).id;
	const kind = KIND.get(key)!;
	if (features.moves.some((f) => f.key === key)) return movePicture(undefined, kind, {}, types)?.id;
	return chooseFor(undefined, kind, {}, "", undefined, types)?.id;
}

test("every key but the two old hidden ones has a type list", () => {
	const all = [...features.switches, ...features.moves].map((f) => f.key).concat(["scene", "mode", "hook", "blurall", "fitoverlap"]);
	for (const key of all) assert.ok(typeLists(key).length > 0, key);
	for (const key of Object.keys(OWN_LISTS)) assert.equal(typeLists(key).length, 0, `${key}: its own list is its type`);
	assert.deepEqual([...keysWithLists()].sort(), [...all].sort());
});

test("every entry of every list is the picture the key shows for it, in every palette, on and off", () => {
	for (const key of keysWithLists())
		for (const list of typeLists(key))
			for (const group of list.groups)
				for (const entry of group.entries) {
					assert.equal(shown(key, list.setting, entry.value), entry.picture, `${key} ${list.setting}=${entry.value}`);
					const picture = BY_ID.get(entry.picture);
					assert.ok(picture, entry.picture);
					for (const colour of COLOURS)
						for (const on of [true, false]) {
							assert.ok(existsSync(new URL(file(picture, on, colour), plugin)), file(picture, on, colour));
							assert.ok(existsSync(new URL(stateFile(picture, on, colour), plugin)), stateFile(picture, on, colour));
						}
				}
});

test("no list offers a type twice, or two types with the same image", () => {
	for (const key of keysWithLists())
		for (const list of typeLists(key)) {
			const entries = list.groups.flatMap((g) => g.entries);
			assert.ok(entries.length >= 2, `${key} ${list.setting}`);
			assert.equal(new Set(entries.map((e) => e.value)).size, entries.length, `${key} ${list.setting}: a value twice`);
			assert.equal(new Set(entries.map((e) => e.label)).size, entries.length, `${key} ${list.setting}: a name twice`);
			assert.equal(new Set(entries.map((e) => e.picture)).size, entries.length, `${key} ${list.setting}: a picture twice`);
			const bytes = entries.map((e) => readFileSync(new URL(file(BY_ID.get(e.picture)!, true, "mono"), plugin)).toString("base64"));
			assert.equal(new Set(bytes).size, entries.length, `${key} ${list.setting}: two files alike`);
			assert.ok(!entries.some((e) => e.value === "auto" || e.value === ""), "Automatic is the list's own first entry");
		}
});

test("every type is generic: nothing named after one person's scenes, sources, apps or devices", () => {
	const owners = /\b(dolphin|hd60s?|blender|zbrush|fusion|elgato|gba|pureref|discord|square|printer|dslr component|live|component)\b/i;
	for (const key of keysWithLists())
		for (const list of typeLists(key)) {
			assert.ok(!owners.test(list.label) && !owners.test(list.automatic), list.label);
			for (const group of list.groups) {
				assert.ok(!owners.test(group.label), group.label);
				for (const entry of group.entries) assert.ok(!owners.test(entry.label), `${key}: ${entry.label}`);
			}
		}
	for (const t of SCENE_TYPES) assert.ok(!owners.test(t.label), t.label);
	for (const t of SOURCE_TYPES) assert.ok(!owners.test(t.label), t.label);
});

test("the Scene type list is the owner's scene pictures under generic names, and as full as can be", () => {
	const list = typeLists("scene")[0];
	assert.equal(list.setting, "sceneType");
	assert.equal(list.label, "Scene type");
	const all = list.groups.flatMap((g) => g.entries);
	assert.ok(all.length >= 30, `${all.length}`);
	const by = new Map(all.map((e) => [e.value, e.picture]));
	assert.equal(by.get("retro"), "scenes/11-dolphin-gba", "the owner's handheld-link scene is a retro one");
	assert.equal(by.get("capture_card"), "scenes/05-game-hd60s");
	assert.equal(by.get("gameplay_chat"), "scenes/03-game-component");
	assert.equal(by.get("holding"), "options/scene-standby");
	for (const p of PICTURES.filter((p) => p.group === "scenes")) assert.ok([...by.values()].includes(p.id), `${p.id} is offered`);
	assert.deepEqual(list.groups.map((g) => g.label), ["Holding screens", "Games", "Apps & screens", "Camera & talk", "All in one"]);
});

test("every settings page has its type lists and no Image list", () => {
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	for (const action of manifest.Actions) {
		const page = readFileSync(new URL(action.PropertyInspectorPath, plugin), "utf8");
		assert.ok(!page.includes('setting="picture"') && !page.includes("getPictures"), `${action.PropertyInspectorPath}: an Image list`);
	}
	const pageOf = (key: string) => (key === "blurall" ? "blur-all" : key);
	for (const key of keysWithLists()) {
		const page = readFileSync(new URL(`ui/${pageOf(key)}.html`, plugin), "utf8");
		for (const list of typeLists(key)) {
			assert.ok(page.includes(`label="${list.label}"`), `${key}: ${list.label}`);
			assert.ok(page.includes(`setting="${list.setting}" placeholder=`), `${key}: ${list.setting}`);
			assert.ok(page.includes('<option value="auto">'), `${key}: Automatic`);
			for (const group of list.groups) if (group.label) assert.ok(page.includes(`<optgroup label="${group.label.replace(/&/g, "&amp;")}">`), group.label);
		}
	}
	for (const [key, setting] of Object.entries(OWN_LISTS)) assert.ok(readFileSync(new URL(`ui/${key}.html`, plugin), "utf8").includes(`setting="${setting}"`), key);
});

test("an image chosen in the old Image list is still shown, and set as its type where one shows it", () => {
	// Kept aside, shown until a type list is changed.
	const old = { target: "Cam\nShow", picture: "show-hide/gba-link-1", sourceType: "" };
	assert.equal(keptPicture(old), "show-hide/gba-link-1");
	const adopted = adoptOld(old)!;
	assert.ok(!("picture" in adopted));
	assert.equal(keptPicture(adopted), "show-hide/gba-link-1");
	assert.equal(keptPicture({ ...adopted, sourceType: "auto" }), undefined, "Automatic chosen: the image goes");
	assert.equal(keptPicture({ ...adopted, sourceType: "camera" }), undefined);
	// A key that had both an image and a type keeps the image as well.
	assert.equal(keptPicture(adoptOld({ picture: "options/browser", sourceType: "game" })!), "options/browser");
	assert.equal(chooseFor(keptPicture({ picture: "options/browser", sourceType: "game" }), "stagekit_show_in_scenes", {}, "", undefined, {})?.id, "options/browser");

	// Where a type of the key's lists shows that very image, it is set.
	const resolve = (s: Record<string, unknown>) => chooseFor(keptPicture(s), "stagekit_show_in_scenes", {}, "Cam", undefined, s)?.id;
	assert.deepEqual(typeForKept("showinscenes", old, resolve), { target: "Cam\nShow", sourceType: "retro" });
	const blur = (s: Record<string, unknown>) => chooseFor(keptPicture(s), "stagekit_ramp_blur", { blur_type: 1 }, "Cam", undefined, s)?.id;
	assert.equal(typeForKept("blur", { picture: "blur-box/hd60s" }, blur)?.sourceType, "capture_card");
	assert.equal(typeForKept("blur", { picture: "blur-types/box" }, blur)?.look, "box");
	const scene = (s: Record<string, unknown>) => scenePicture(keptPicture(s), "Anything", s.sceneType).id;
	assert.equal(typeForKept("scene", { scene: "Anything", picture: "scenes/07-window-capture" }, scene)?.sceneType, "app");
	// An image no type shows (another key's, say) is simply kept.
	assert.equal(typeForKept("scene", { scene: "Anything", picture: "show-hide/chat" }, scene), undefined);
	assert.equal(scenePicture(keptPicture(adoptOld({ picture: "show-hide/chat" })!), "Anything").id, "show-hide/chat");

	// Every image of the pack a key may have been given still shows.
	for (const p of PICTURES) {
		const kept = keptPicture(adoptOld({ picture: p.id })!);
		assert.equal(chooseFor(kept, "stagekit_ramp_blur", {}, "", undefined, {})?.id, p.id);
		assert.equal(scenePicture(kept, "x").id, p.id);
		assert.equal(modePicture(kept, "").id, p.id);
		assert.equal(hookPicture(kept, "window_capture").id, p.id);
	}
});

test("a key's title follows its Capture type", async () => {
	const { hookTitle } = await import("../src/titles.ts");
	assert.equal(hookTitle("game_capture", "window"), "Capture\nWindow");
	assert.equal(hookTitle("window_capture", "auto"), "Capture\nWindow");
	assert.equal(hookTitle(undefined, "game"), "Capture\nGame");
});
