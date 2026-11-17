// Every key's type lists (src/key-types.ts): what replaced the Image list.

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";

import features from "../src/features.json" with { type: "json" };
import { adoptOld, keptPicture, keysWithLists, OWN_LISTS, SCENE_TYPES, sceneTypePicture, SOURCE_TYPES, sourceTypePicture, typeForKept, typeLists } from "../src/key-types.ts";
import {
	blurAllPicture,
	chooseFor,
	COLOURS,
	file,
	hookPicture,
	modePicture,
	movePicture,
	moveShown,
	optionShown,
	overlapPicture,
	PICTURES,
	scenePicture,
	shapeShown,
	slideShown,
	stateFile,
	stepShown,
	toolShown,
	hookShown,
	blurAllShown,
	sceneBlurShown,
} from "../src/pictures.ts";

const plugin = new URL("../com.techsavage1989.stagekit.sdPlugin/", import.meta.url);
const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));
const KIND = new Map([...features.switches, ...features.moves].map((f) => [f.key, f.kind]));

/** The picture a key shows with one of its lists set, as the key itself chooses it. */
function shown(key: string, setting: string, value: string): string | undefined {
	const types = { [setting]: value };
	if (key === "scene") return scenePicture(undefined, "", value).id;
	if (key === "mode") return modePicture(undefined, value).id;
	// (The keys that draw what they do as a badge: the type's picture is the one under it.)
	if (key === "hook") return hookShown(undefined, "game_capture", types.sourceType, types.look).picture.id;
	if (key === "blurall") return blurAllShown(undefined, types.look, types.sceneType).picture.id;
	if (key === "sceneblur") return sceneBlurShown(undefined, types.look, types.sceneType).picture.id;
	if (key === "appswitch") return stepShown(undefined, "next", value).picture.id;
	if (key === "tool") return toolShown(undefined, "left", value).picture.id;
	if (key === "slide") return slideShown(undefined, "left", value).picture.id;
	if (key === "shape") return shapeShown(undefined, "4:3", value).picture.id;
	if (key === "option") return optionShown(undefined, "snap", value).picture.id;
	if (key === "sendto" || key === "placeitem") return moveShown(undefined, KIND.get(key)!, {}, types)?.picture.id;
	if (key === "fitoverlap") return overlapPicture(undefined, value).id;
	const kind = KIND.get(key)!;
	if (features.moves.some((f) => f.key === key)) return movePicture(undefined, kind, {}, types)?.id;
	return chooseFor(undefined, kind, {}, "", undefined, types)?.id;
}

test("every key a user can add has a type list; only the two old hidden ones have none", () => {
	// The owner: "no types selection for manual window/source? all keys must have the options".
	const manifest = JSON.parse(readFileSync(new URL("manifest.json", plugin), "utf8"));
	const offered = manifest.Actions.filter((a: { VisibleInActionsList?: boolean }) => a.VisibleInActionsList !== false).map((a: { UUID: string }) =>
		a.UUID.slice(a.UUID.lastIndexOf(".") + 1),
	);
	assert.ok(offered.length >= 31, `${offered.length}`);
	for (const key of offered) assert.ok(typeLists(key).length > 0, key);
	for (const key of Object.keys(OWN_LISTS)) assert.ok(typeLists(key).some((l) => l.setting === "sourceType"), `${key}: a Source type besides its own list`);
	for (const key of ["switch", "move"]) assert.equal(typeLists(key).length, 0, key);
	assert.deepEqual([...keysWithLists()].sort(), [...offered].sort());
	// The ones asked for by name, and what they offer.
	const values = (key: string, setting: string) => typeLists(key).find((l) => l.setting === setting)!.groups.flatMap((g) => g.entries.map((e) => e.value));
	for (const v of ["window", "browser", "game", "retro", "emulator", "art_app", "code_editor", "chat", "voice_chat", "music", "camera"])
		assert.ok(values("appswitch", "sourceType").includes(v), `appswitch ${v}`);
	for (const key of ["slide", "tool", "option", "sendto", "placeitem"])
		for (const v of ["camera", "facecam", "chat", "alerts", "game", "window", "music", "logo", "title"]) assert.ok(values(key, "sourceType").includes(v), `${key} ${v}`);
	for (const v of ["game", "retro", "console", "capture_card", "emulator", "handheld"]) assert.ok(values("shape", "sourceType").includes(v), `shape ${v}`);
	assert.deepEqual(typeLists("hook").map((l) => l.setting), ["look", "sourceType"], "Capture type and Source type");
	assert.deepEqual(typeLists("blurall").map((l) => l.setting), ["look", "sceneType"], "Blur style and Scene type");
	assert.deepEqual(typeLists("sendto").map((l) => l.setting), ["look", "sourceType"], "Position and Source type");
	// Every destination Move Source can have (place-item.cpp's Destination: nine places, a saved place, a spot on the screen).
	assert.deepEqual(values("sendto", "look"), ["top-left", "top-centre", "top-right", "middle-left", "centre", "middle-right", "bottom-left", "bottom-centre", "bottom-right", "spot", "screen"]);
	for (const list of typeLists("appswitch")) assert.ok(!list.groups.some((g) => g.label === "Activities"));
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
	const owners = /\b(dolphin|hd60s?|blender|zbrush|fusion|elgato|gba|game ?cube|game boy|pureref|discord|square camera|printer|component|chat live|stream ?deck)\b|^(live|standby)$/i;
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
	assert.equal(by.get("console_link"), "scenes/11-dolphin-gba", "the owner's console-and-handheld layout, by what it is");
	assert.equal(by.get("retro"), "options/scene-type-retro", "retro gaming has a drawing of its own");
	assert.notEqual(by.get("classic"), by.get("retro"));
	assert.equal(by.get("capture_card"), "scenes/05-game-hd60s");
	assert.equal(by.get("gameplay_chat"), "scenes/03-game-component");
	assert.equal(by.get("holding"), "options/scene-standby");
	for (const p of PICTURES.filter((p) => p.group === "scenes")) assert.ok([...by.values()].includes(p.id), `${p.id} is offered`);
	assert.deepEqual(list.groups.map((g) => g.label).slice(0, 7), ["Holding & status", "Games", "Apps & screens", "Creative", "Camera & talk", "IRL & lifestyle", "All in one"]);
	assert.ok(list.groups.slice(7).every((g) => g.label.startsWith("Sources: ")), "then the source types");
});

test("every key offers every scene image and every source type, in any combination", () => {
	// The owner: "I need all my original Scene type icons to exist in the
	// types lists ... I want to be able to set any combination".
	const scenePictures = new Set(SCENE_TYPES.map((t) => t.picture));
	for (const key of keysWithLists()) {
		const lists = typeLists(key).filter((l) => l.setting !== "look");
		assert.ok(lists.length, `${key}: a Source or Scene type list`);
		for (const list of lists) {
			const entries = list.groups.flatMap((g) => g.entries);
			const pictures = new Set(entries.map((e) => e.picture));
			for (const p of scenePictures) assert.ok(pictures.has(p), `${key} ${list.setting}: ${p}`);
			const values = new Set(entries.map((e) => e.value.replace(/^(scene|source):/, "")));
			for (const t of SOURCE_TYPES) {
				const own = list.setting === "sourceType" ? t.id : `source:${t.id}`;
				assert.ok(values.has(t.id) || pictures.has(sourceTypePicture(t.id)!), `${key} ${list.setting}: ${own}`);
			}
		}
	}
	// A scene type chosen on a source key, and a source type on a scene key,
	// show their own pictures.
	assert.equal(sourceTypePicture("scene:brb", "show"), "scenes/14-be-right-back");
	assert.equal(sceneTypePicture("source:camera"), sourceTypePicture("camera"));
	assert.equal(chooseFor(undefined, "stagekit_show_in_scenes", {}, "Starting", undefined, { sourceType: "scene:starting" })?.id, "scenes/01-stream-start");
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
	assert.deepEqual(typeForKept("showinscenes", old, resolve), { target: "Cam\nShow", sourceType: "linked_handheld" });
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

test("devices, status, styles and activities are in the lists they belong to", () => {
	const groupsOf = (key: string, setting: string) => typeLists(key).find((l) => l.setting === setting)!.groups;
	const values = (key: string, setting: string) => groupsOf(key, setting).flatMap((g) => g.entries.map((e) => e.value));
	// Show in Scenes: what a source can be, devices and status included; no activities.
	const show = groupsOf("showinscenes", "sourceType").map((g) => g.label);
	for (const g of ["Captures & games", "Apps", "Cameras", "Devices & audio", "Overlays", "Status & holding screens", "Layout markers"]) assert.ok(show.includes(g), g);
	assert.ok(!show.includes("Activities"));
	for (const v of ["pc", "controller", "stream_deck", "pen_display", "vr_headset", "action_cam", "ptz_camera", "headset", "muted", "privacy", "live"])
		assert.ok(values("showinscenes", "sourceType").includes(v), v);
	// Scene and Mode types: status and activities.
	for (const v of ["paused", "offline", "privacy", "raid", "speedrun", "cooking", "irl", "fitness", "watch_party", "tutorial", "console_link"])
		assert.ok(values("scene", "sceneType").includes(v), v);
	for (const v of ["live", "muted", "speedrun", "cooking", "podcast", "column_handheld", "linked_handheld"]) assert.ok(values("mode", "sourceType").includes(v), v);
	// Styles.
	assert.deepEqual(groupsOf("blur", "look").map((g) => g.label), ["Blurs", "Mosaics"]);
	assert.ok(values("blur", "look").length >= 14);
	assert.equal(values("corners", "look").length, 7);
	assert.equal(values("shadow", "look").length, 7);
	for (const key of ["showhide", "visibilitylink", "activeapp", "appcapture"])
		assert.deepEqual(values(key, "look"), ["fade", "cut", "slide", "swipe", "wipe", "zoom", "morph"], key);
	// Lists stay to their job: no device in a Blur source list without the pack's blurred image of it, no activity on a capture key.
	for (const key of ["zoom", "standby", "autoframe", "autocapture", "appcapture", "programtrigger"])
		assert.ok(!groupsOf(key, "sourceType").some((g) => g.label === "Activities"), key);
});

test("a Blur style the pack has no pictures of: its drawing, or the source in the nearest of the pack's styles", () => {
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "", undefined, { look: "mosaic_circle" })?.id, "options/style-mosaic-circle");
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "", undefined, { look: "mosaic_circle", sourceType: "game" })?.id, "blur-hexagons/game-capture");
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", {}, "", undefined, { look: "trail", sourceType: "window" })?.id, "blur-motion/window-capture");
	assert.equal(chooseFor(undefined, "stagekit_ramp_blur", { blur_type: 4 }, "", undefined, {})?.id, "blur-types/zoom", "Automatic: the filter's own");
	assert.equal(chooseFor(undefined, "stagekit_shadow_backdrop", {}, "", undefined, { look: "neon" })?.id, "options/style-shadow-neon");
	assert.equal(chooseFor(undefined, "stagekit_show_hide", {}, "", undefined, { look: "morph" })?.id, "options/style-morph");
	assert.equal(chooseFor(undefined, "stagekit_show_hide", {}, "", undefined, { look: "morph", sceneType: "cooking" })?.id, "options/scene-type-cooking", "the scene first");
});

test("the Scene Blur key's Blur style list is the Blur key's, all fourteen", () => {
	const look = (key: string) => typeLists(key).find((l) => l.setting === "look");
	const values = (key: string) => look(key)!.groups.flatMap((g) => g.entries.map((e) => e.value));
	assert.equal(values("sceneblur").length, 14);
	assert.deepEqual(look("sceneblur"), look("blur"));
});
