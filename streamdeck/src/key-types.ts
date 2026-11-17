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
export type Look = "blur" | "corners" | "shadow" | "transition" | "position" | "capture";

export type Entry = { value: string; label: string; picture: string };
export type Group = { label: string; entries: Entry[] };
/** One list on a key's settings page. Automatic is its first entry (value "auto"). */
export type TypeList = { setting: "sourceType" | "sceneType" | "look"; label: string; tip: string; automatic: string; groups: Group[] };

type SourceType = { id: string; label: string; group: string; base: string; words: string[][] };
export const SOURCE_TYPES: SourceType[] = data.sourceTypes;
export const SCENE_TYPES: Array<{ id: string; label: string; group: string; picture: string }> = data.sceneTypes;
const ART = data.art as Record<Family, Record<string, string>>;
const LOOKS = data.looks as Record<Look, { label: string; tip: string; groups: Array<{ label: string; entries: string[][] }> }>;

/** The blur styles by the filter's number (`blur_type`, append-only). */
export const BLUR_STYLES = ["gaussian", "box", "kawase", "pixelate", "zoom", "motion", "hexagons"];

/**
 * The pack's blur style nearest a Blur style it has no pictures of (the
 * mosaic's other cells, glass, the trail), for a source drawn in that style.
 */
export function packBlurStyle(style: string): string {
	if (BLUR_STYLES.includes(style)) return style;
	if (style === "trail") return "motion";
	if (style === "mosaic_circle") return "hexagons";
	return "pixelate";
}

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

/**
 * Every key offers every image (the owner: "I need all my original Scene type
 * icons to exist in the types lists ... I want to be able to set any
 * combination"): a Source type list offers the scene types too, as
 * "scene:<id>", and a Scene type list the source types, as "source:<id>".
 */
export const SCENE_PREFIX = "scene:";
export const SOURCE_PREFIX = "source:";

/** A source type's picture for a key's function: the owner's art for it, else the type's own. */
export function sourceTypePicture(type: string | undefined, family?: Family, blurStyle = "gaussian"): string | undefined {
	// (A scene type offered in a Source type list: its picture as it is.)
	if (type?.startsWith(SCENE_PREFIX)) return sceneTypePicture(type.slice(SCENE_PREFIX.length));
	const t = SOURCE_TYPES.find((s) => s.id === type);
	if (!t) return undefined;
	const art = family ? ART[family][t.id] : undefined;
	if (art) return family === "blur" ? `blur-${packBlurStyle(blurStyle)}/${art}` : art;
	return t.base;
}

export function sceneTypePicture(type: string | undefined): string | undefined {
	// (A source type offered in a Scene type list: its own picture.)
	if (type?.startsWith(SOURCE_PREFIX)) return sourceTypePicture(type.slice(SOURCE_PREFIX.length));
	return SCENE_TYPES.find((s) => s.id === type)?.picture;
}

export function lookPicture(look: Look, value: string | undefined): string | undefined {
	for (const group of LOOKS[look].groups) {
		const found = group.entries.find((e) => e[0] === value);
		if (found) return found[2];
	}
	return undefined;
}

/** The types with art of their own in a family (the only ones a Shadow, Standby or Blur key offers). */
function withArt(family: Family): string[] {
	return SOURCE_TYPES.filter((t) => ART[family][t.id]).map((t) => t.id);
}

/** The source types of some groups of src/key-types.json, in its order. */
function inGroups(...groups: string[]): string[] {
	return SOURCE_TYPES.filter((t) => groups.includes(t.group)).map((t) => t.id);
}

const CAPTURES = ["game", "retro", "emulator", "console", "handheld", "linked_handheld", "linked_handheld_2", "capture_card", "capture_box", "hdmi_source",
	"window", "browser", "display", "second_display", "tablet", "phone", "pc", "laptop", "retro_console", "pen_display", "vr_headset"];
const APPS = inGroups("Apps");
const CAMERAS = inGroups("Cameras");

/** What is moved or lined up: a source in a layout (the Move, Slide, Arrange tool and Dock Option keys). */
const MOVED = [...CAMERAS, ...inGroups("Overlays"),
	"game", "retro", "emulator", "console", "handheld", "linked_handheld", "linked_handheld_2", "capture_card", "window", "browser", "display", "tablet", "phone",
	...APPS];

/** What a key's list offers, for the keys that act on a source or whose image shows one. */
const SOURCES: Record<string, { family?: Family; only?: string[]; label?: string; automatic: string; tip: string }> = {
	// Everything a source can be: not the activities, which are what a scene or a mode is for.
	showinscenes: {
		family: "show",
		only: SOURCE_TYPES.filter((t) => t.group !== "Activities").map((t) => t.id),
		automatic: "Automatic (by the source)",
		tip: "What the source is, for the key's image. Automatic goes by the source's name and kind.",
	},
	shadow: { family: "shadow", only: withArt("shadow"), automatic: "Automatic (by the source)", tip: "What the shadow is on, for the key's image." },
	// (Not the pack's standby-by-source pictures: without the words they
	// carried, all nine are the same mountain. Old keys that chose one keep it.)
	standby: { automatic: "Automatic (standby image)", tip: "What the standby image stands in for, for the key's image.", only: [...CAPTURES, ...APPS, ...CAMERAS] },
	blur: { family: "blur", only: withArt("blur"), automatic: "Automatic (by the source)", tip: "What is blurred, for the key's image, drawn in the blur style." },
	corners: { automatic: "Automatic (the corner style's image)", tip: "What the corners are on, for the key's image.", only: [...CAMERAS, ...CAPTURES, ...APPS, "image", "video"] },
	zoom: { automatic: "Automatic (zoom)", tip: "What is zoomed, for the key's image.", only: [...CAPTURES, ...APPS, ...CAMERAS, "image", "video", "green_screen"] },
	autocapture: {
		automatic: "Automatic (auto capture)",
		tip: "What the capture takes, for the key's image.",
		only: ["game", "retro", "emulator", "pc", "vr_headset", "app_3d", "sculpting", "cad", "art_app"],
	},
	autoframe: {
		automatic: "Automatic (auto frame)",
		tip: "What is framed, for the key's image.",
		only: ["game", "retro", "emulator", "console", "handheld", "linked_handheld", "linked_handheld_2", "capture_card", "capture_box", "hdmi_source",
			"retro_console", "window", "display"],
	},
	appcapture: {
		automatic: "Automatic (window switcher)",
		tip: "What the capture switches between, for the key's image.",
		only: ["window", "browser", "game", "retro", "emulator", "linked_handheld", ...APPS, "chat", "voice_chat", "music"],
	},
	programtrigger: {
		automatic: "Automatic (program trigger)",
		tip: "Which program the trigger waits for, for the key's image.",
		only: ["game", "retro", "emulator", "window", "browser", ...APPS, "voice_chat", "music", "recording", "live"],
	},
	// The keys whose own list is what they do (OWN_LISTS) and the ones with a
	// look only: the type is the image, what they do is its corner badge
	// (pictures.ts, `stepShown` and the rest). The owner: "no types
	// selection for manual window/source? all keys must have the options".
	appswitch: {
		automatic: "Automatic (the step's image)",
		tip: "What the capture switches between, for the key's image; the step is drawn in its corner. Automatic shows the step alone.",
		only: ["window", "browser", "game", "retro", "emulator", "console", "capture_card", "display", "linked_handheld", ...APPS, "chat", "voice_chat", "music", ...CAMERAS],
	},
	tool: {
		automatic: "Automatic (the tool's image)",
		tip: "What the tool lines up or resizes, for the key's image; the tool is drawn in its corner. Automatic shows the tool alone.",
		only: MOVED,
	},
	slide: {
		automatic: "Automatic (the slide's image)",
		tip: "What is slid, for the key's image; an arrow to the edge is drawn in its corner. Automatic shows the slide alone.",
		only: MOVED,
	},
	option: {
		automatic: "Automatic (the option's image)",
		tip: "What the option is for, for the key's image; the option is drawn in its corner. Automatic shows the option alone.",
		only: MOVED,
	},
	shape: {
		automatic: "Automatic (the ratio's image)",
		tip: "What is framed, for the key's image; the ratio is drawn in its corner. Automatic shows the ratio alone.",
		only: ["game", "retro", "emulator", "console", "retro_console", "handheld", "linked_handheld", "linked_handheld_2", "capture_card", "capture_box", "hdmi_source",
			"window", "display", "pc", "vr_headset"],
	},
	hook: {
		automatic: "Automatic (the capture's image)",
		tip: "What the capture takes, for the key's image; the kind of capture is drawn in its corner. Automatic shows the capture alone.",
		only: ["game", "retro", "emulator", "window", "browser", ...APPS, "chat", "voice_chat", "music"],
	},
	sendto: {
		automatic: "Automatic (the place's image)",
		tip: "What is moved, for the key's image; where it goes is drawn in its corner. Automatic shows the place alone.",
		only: MOVED,
	},
	placeitem: {
		automatic: "Automatic (the place's image)",
		tip: "What is moved, for the key's image; where it goes is drawn in its corner. Automatic shows the place alone.",
		only: MOVED,
	},
	mode: {
		family: "mode",
		label: "Mode type",
		automatic: "Automatic (the mode's type)",
		tip: "What the mode shows, for the key's image. Automatic takes the Mode type set in OBS.",
		only: [
			...inGroups("Status & holding screens"),
			"countdown",
			"game", "retro", "emulator", "console", "handheld", "linked_handheld", "capture_card", "window", "display", "tablet", "camera",
			...APPS,
			...inGroups("Layout markers", "Activities"),
		],
	},
};

/** The keys whose list is the Scene type: Switch Scene, Blur All, Scene Blur, and keys for a filter on a scene. */
function sceneKeys(): string[] {
	const keys = ["scene", "fitoverlap", "layout", "blurall", "sceneblur"];
	for (const f of features.switches) if ((f as { scene?: boolean }).scene) keys.push(f.key);
	return keys;
}

const LOOK_KEYS: Record<string, Look> = {
	blur: "blur",
	blurall: "blur",
	sceneblur: "blur",
	corners: "corners",
	shadow: "shadow",
	sendto: "position",
	placeitem: "position",
	hook: "capture",
	// Keys for filters whose job is a change: its transition.
	showhide: "transition",
	visibilitylink: "transition",
	activeapp: "transition",
	appcapture: "transition",
};

/**
 * The keys whose own list is the thing the key does: a tool, an option, a
 * way to slide, an aspect ratio, a step. Since beta159 each also has a Source
 * type list; with a type chosen, what the key does is drawn as a badge in the
 * corner of the type's picture.
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

/** The scene types in their groups, their values after `prefix`. */
function sceneGroups(prefix: string): Group[] {
	const groups: Group[] = [];
	for (const label of data.sceneGroups) {
		const entries = SCENE_TYPES.filter((t) => t.group === label).map((t) => ({ value: prefix + t.id, label: t.label, picture: t.picture }));
		if (entries.length) groups.push({ label, entries });
	}
	return groups;
}

/**
 * One list's groups with each picture offered once (the first, the key's own
 * kind, wins) and each name once: a later entry of the same name is told
 * apart by `mark` ("Console (scene)").
 */
function distinct(groups: Group[], mark: string): Group[] {
	const pictures = new Set<string>();
	const labels = new Set<string>();
	const out: Group[] = [];
	for (const g of groups) {
		const entries: Entry[] = [];
		for (const e of g.entries) {
			if (pictures.has(e.picture)) continue;
			pictures.add(e.picture);
			const label = labels.has(e.label) ? e.label + mark : e.label;
			labels.add(label);
			entries.push({ ...e, label });
		}
		if (entries.length) out.push({ label: g.label, entries });
	}
	return out;
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
			groups: l.groups.map((g) => ({ label: g.label, entries: g.entries.map(([value, label, picture]) => ({ value, label, picture })) })),
		});
	}
	const source = SOURCES[key];
	if (source) {
		const ids = source.only ?? SOURCE_TYPES.map((t) => t.id);
		const rest = SOURCE_TYPES.map((t) => t.id).filter((id) => !ids.includes(id));
		const groups = [
			...grouped(ids, source.family),
			...grouped(rest, source.family).map((g) => ({ label: `More: ${g.label}`, entries: g.entries })),
			...sceneGroups(SCENE_PREFIX).map((g) => ({ label: `Scenes: ${g.label}`, entries: g.entries })),
		];
		lists.push({ setting: "sourceType", label: source.label ?? "Source type", tip: source.tip, automatic: source.automatic, groups: distinct(groups, " (scene)") });
	}
	if (sceneKeys().includes(key)) {
		const groups = [
			...sceneGroups(""),
			...grouped(SOURCE_TYPES.map((t) => t.id), undefined).map((g) => ({
				label: `Sources: ${g.label}`,
				entries: g.entries.map((e) => ({ ...e, value: SOURCE_PREFIX + e.value })),
			})),
		];
		lists.push({
			setting: "sceneType",
			label: "Scene type",
			tip:
				key === "scene"
					? "What the scene is, for the key's image. Automatic goes by the scene's name."
					: key === "blurall" || key === "sceneblur"
						? "What is blurred, for the key's image; the blur style is drawn in its corner. Automatic shows the blur style alone."
						: "What the scene is, for the key's image. Automatic shows the filter's own image.",
			automatic: key === "scene" ? "Automatic (by the scene's name)" : key === "blurall" || key === "sceneblur" ? "Automatic (the blur's image)" : "Automatic (the filter's image)",
			groups: distinct(groups, " (source)"),
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
