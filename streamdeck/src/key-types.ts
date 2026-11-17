// The type lists on every key's settings page, and the picture of each
// entry. The owner: "instead of having an image list on all the streamdeck
// buttons, have a similar source type, scene type, etc style list relative
// to the button function", and then: "I don't want any of the buttons, or
// the options specific to my stream scene collection, but I do want the
// specific icons I had". So every type is generic (src/key-types.json) and
// its picture is the owner's own artwork for that key's function where the
// pack has one: a Show in Scenes key's camera has the eye over a camera, a
// Shadow key's the camera with its shadow, a Blur key's the camera blurred
// in the filter's own style. Where the pack has none, the type's drawing.
//
// No Stream Deck code here: the tests and tools/make-plugin-files.py (which
// writes the settings pages from typeLists) use it as it is.

import data from "./key-types.json" with { type: "json" };
import features from "./features.json" with { type: "json" };

export type Family = "show" | "shadow" | "blur" | "mode";
export type Look = "blur" | "corners" | "position" | "capture";

export type Entry = { value: string; label: string; picture: string };
export type Group = { label: string; entries: Entry[] };
/** One list on a key's settings page. Automatic is its first entry (value "auto"). */
export type TypeList = { setting: "sourceType" | "sceneType" | "look"; label: string; tip: string; automatic: string; groups: Group[] };

type SourceType = { id: string; label: string; group: string; base: string; words: string[][] };
export const SOURCE_TYPES: SourceType[] = data.sourceTypes;
export const SCENE_TYPES: Array<{ id: string; label: string; group: string; picture: string }> = data.sceneTypes;
const ART = data.art as Record<Family, Record<string, string>>;
const LOOKS = data.looks as Record<Look, { label: string; tip: string; entries: string[][] }>;

/** The blur styles by the filter's number (`blur_type`, append-only). */
export const BLUR_STYLES = ["gaussian", "box", "kawase", "pixelate", "zoom", "motion", "hexagons"];

/**
 * A type setting's value when it names a type. Empty (never set, or
 * Automatic before beta156) and "auto" (Automatic) do not.
 */
export function explicit(value: unknown): string | undefined {
	return typeof value === "string" && value !== "" && value !== "auto" ? value : undefined;
}

const TYPE_SETTINGS = ["sourceType", "sceneType", "look"];

/** The key's type lists as they are set, as one string. */
function snapshot(settings: Record<string, unknown>): string {
	return TYPE_SETTINGS.map((name) => (typeof settings[name] === "string" ? settings[name] : "")).join("|");
}

/**
 * A key's settings from before beta156 with its Image (`picture`) put aside
 * as `oldImage`, with how its type lists were set at that moment
 * (`oldImageWhile`); undefined when there is nothing to put aside. The
 * Image list is gone from the settings pages; the image stays.
 */
export function adoptOld(settings: Record<string, unknown>): Record<string, unknown> | undefined {
	if (typeof settings.picture !== "string") return undefined;
	const out: Record<string, unknown> = { ...settings };
	delete out.picture;
	if (settings.picture !== "") {
		out.oldImage = settings.picture;
		out.oldImageWhile = snapshot(settings);
	}
	return out;
}

/**
 * The image a key was given in the Image list before beta156: still shown
 * while its type lists are as they were then, so no key changes look on
 * update. Choosing anything in a type list, Automatic included, takes its
 * place.
 */
export function keptPicture(settings: Record<string, unknown>): string | undefined {
	if (typeof settings.picture === "string" && settings.picture !== "") return settings.picture;
	const old = settings.oldImage;
	if (typeof old !== "string" || old === "") return undefined;
	return settings.oldImageWhile === snapshot(settings) ? old : undefined;
}

/**
 * For a key with a kept image: the type in its lists that shows that very
 * image, so its settings page says what it is. `resolve` is the key's own
 * choice of picture from its settings. Undefined when none does (the image
 * is then simply kept).
 */
export function typeForKept(key: string, settings: Record<string, unknown>, resolve: (settings: Record<string, unknown>) => string | undefined): Record<string, unknown> | undefined {
	const kept = keptPicture(settings);
	if (!kept) return undefined;
	const base = adoptOld(settings) ?? settings;
	for (const list of typeLists(key))
		for (const group of list.groups)
			for (const entry of group.entries) {
				const tried: Record<string, unknown> = { ...base, [list.setting]: entry.value };
				delete tried.oldImage;
				delete tried.oldImageWhile;
				if (resolve(tried) === kept) return tried;
			}
	return undefined;
}

/** A source type's picture for a key's function: the owner's art for it, else the type's own. */
export function sourceTypePicture(type: string | undefined, family?: Family, blurStyle = "gaussian"): string | undefined {
	const t = SOURCE_TYPES.find((s) => s.id === type);
	if (!t) return undefined;
	const art = family ? ART[family][t.id] : undefined;
	if (art) return family === "blur" ? `blur-${BLUR_STYLES.includes(blurStyle) ? blurStyle : "gaussian"}/${art}` : art;
	return t.base;
}

export function sceneTypePicture(type: string | undefined): string | undefined {
	return SCENE_TYPES.find((s) => s.id === type)?.picture;
}

export function lookPicture(look: Look, value: string | undefined): string | undefined {
	return LOOKS[look].entries.find((e) => e[0] === value)?.[2];
}

/** The types with art of their own in a family (the only ones a Shadow, Standby or Blur key offers). */
function withArt(family: Family): string[] {
	return SOURCE_TYPES.filter((t) => ART[family][t.id]).map((t) => t.id);
}

/** What a source key's list offers, for the keys that act on a source. */
const SOURCES: Record<string, { family?: Family; only?: string[]; label?: string; automatic: string; tip: string }> = {
	showinscenes: { family: "show", automatic: "Automatic (by the source)", tip: "What the source is, for the key's image. Automatic goes by the source's name and kind." },
	shadow: { family: "shadow", only: withArt("shadow"), automatic: "Automatic (the shadow's look)", tip: "What the shadow is on, for the key's image. Automatic shows the shadow, glow or outline the filter is set to." },
	// (Not the pack's standby-by-source pictures: without the words they
	// carried, all nine are the same mountain. Old keys that chose one keep it.)
	standby: {
		automatic: "Automatic (standby image)",
		tip: "What the standby image stands in for, for the key's image.",
		only: ["game", "retro", "emulator", "console", "handheld", "capture_card", "window", "browser", "display", "second_display", "tablet", "phone",
			"art_app", "app_3d", "sculpting", "cad", "camera", "dslr", "camcorder", "facecam", "phone_camera", "virtual_camera"],
	},
	blur: { family: "blur", only: withArt("blur"), automatic: "Automatic (by the source)", tip: "What is blurred, for the key's image, drawn in the blur style." },
	zoom: {
		automatic: "Automatic (zoom)",
		tip: "What is zoomed, for the key's image.",
		only: ["game", "retro", "emulator", "console", "handheld", "capture_card", "window", "browser", "display", "second_display", "tablet", "phone",
			"art_app", "app_3d", "sculpting", "cad", "code_editor", "office", "video_player", "reference",
			"camera", "dslr", "camcorder", "facecam", "full_camera", "side_camera", "phone_camera", "workspace_camera", "virtual_camera", "image", "video"],
	},
	autocapture: { automatic: "Automatic (auto capture)", tip: "What the capture takes, for the key's image.", only: ["game", "retro", "emulator", "app_3d", "sculpting", "cad", "art_app"] },
	autoframe: { automatic: "Automatic (auto frame)", tip: "What is framed, for the key's image.", only: ["game", "retro", "retro_2", "emulator", "console", "handheld", "capture_card", "window", "display"] },
	appcapture: {
		automatic: "Automatic (window switcher)",
		tip: "What the capture switches between, for the key's image.",
		only: ["window", "browser", "game", "retro", "emulator", "art_app", "app_3d", "sculpting", "cad", "code_editor", "office", "video_player", "reference", "chat", "voice_chat", "music"],
	},
	programtrigger: {
		automatic: "Automatic (program trigger)",
		tip: "Which program the trigger waits for, for the key's image.",
		only: ["game", "retro", "emulator", "window", "browser", "art_app", "app_3d", "sculpting", "cad", "code_editor", "office", "video_player", "reference", "voice_chat", "music"],
	},
	mode: {
		family: "mode",
		label: "Mode type",
		automatic: "Automatic (the mode's type)",
		tip: "What the mode shows, for the key's image. Automatic takes the Mode type set in OBS.",
		only: ["starting", "brb", "intermission", "technical", "ending", "countdown", "game", "retro", "console", "capture_card", "window", "display", "tablet",
			"camera", "column_chat", "column_retro"],
	},
};

/** The keys whose list is the Scene type: Switch Scene, and keys for a filter on a scene. */
function sceneKeys(): string[] {
	const keys = ["scene", "fitoverlap", "layout"];
	for (const f of features.switches) if ((f as { scene?: boolean }).scene) keys.push(f.key);
	return keys;
}

const LOOK_KEYS: Record<string, Look> = { blur: "blur", blurall: "blur", corners: "corners", sendto: "position", placeitem: "position", hook: "capture" };

/**
 * The keys whose type is the thing the key does: a tool, an option, a way to
 * slide, an aspect ratio, a step. Their settings page's list of that is
 * their type list; it has a picture for every entry already.
 */
export const OWN_LISTS: Record<string, string> = { tool: "tool", option: "option", slide: "direction", shape: "shape", appswitch: "step" };

function grouped(ids: string[], family: Family | undefined): Group[] {
	const groups: Group[] = [];
	for (const label of data.sourceGroups) {
		const entries = SOURCE_TYPES.filter((t) => t.group === label && ids.includes(t.id)).map((t) => ({
			value: t.id,
			label: t.label,
			picture: sourceTypePicture(t.id, family)!,
		}));
		if (entries.length) groups.push({ label, entries });
	}
	return groups;
}

/** A key's type lists, by its key name (features.json's `key`, or the page name of the others). */
export function typeLists(key: string): TypeList[] {
	const lists: TypeList[] = [];
	const look = LOOK_KEYS[key];
	if (look) {
		const l = LOOKS[look];
		lists.push({
			setting: "look",
			label: l.label,
			tip: l.tip,
			automatic: "Automatic",
			groups: [{ label: "", entries: l.entries.map(([value, label, picture]) => ({ value, label, picture })) }],
		});
	}
	const source = SOURCES[key];
	if (source) {
		const ids = source.only ?? SOURCE_TYPES.map((t) => t.id);
		lists.push({ setting: "sourceType", label: source.label ?? "Source type", tip: source.tip, automatic: source.automatic, groups: grouped(ids, source.family) });
	}
	if (sceneKeys().includes(key)) {
		const groups: Group[] = [];
		for (const label of data.sceneGroups) {
			const entries = SCENE_TYPES.filter((t) => t.group === label).map((t) => ({ value: t.id, label: t.label, picture: t.picture }));
			if (entries.length) groups.push({ label, entries });
		}
		lists.push({
			setting: "sceneType",
			label: "Scene type",
			tip: key === "scene" ? "What the scene is, for the key's image. Automatic goes by the scene's name." : "What the scene is, for the key's image. Automatic shows the filter's own image.",
			automatic: key === "scene" ? "Automatic (by the scene's name)" : "Automatic (the filter's image)",
			groups,
		});
	}
	return lists;
}

/** Every key that has type lists of its own, for the settings pages and the tests. */
export function keysWithLists(): string[] {
	const keys = new Set<string>([...Object.keys(LOOK_KEYS), ...Object.keys(SOURCES), ...sceneKeys()]);
	return [...keys].filter((k) => typeLists(k).length > 0);
}

/** The source type a name speaks of ("Be Right Back", "Main Display"): its longest phrase; the first in the list of those as long. */
export function typeOfName(name: string | undefined): string | undefined {
	const have = (name ?? "")
		.toLowerCase()
		.split(/[^a-z0-9]+/)
		.filter((word) => word !== "");
	if (have.length === 0) return undefined;
	let best: string | undefined;
	let bestLength = 0;
	for (const type of SOURCE_TYPES)
		for (const phrase of type.words)
			if (phrase.length > bestLength && phrase.every((word) => have.includes(word))) {
				best = type.id;
				bestLength = phrase.length;
			}
	return best;
}
