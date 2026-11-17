// Which picture a key shows. The pictures are the StageKit icon pack, kept in
// imgs/pack/<colour>/<group>/<name>.png and listed in pack.json. Left to
// itself a key takes the picture that fits what its filter is and how it is
// set; a key can also be given any picture of the pack.

import { isSceneKind } from "./kinds.ts";
import pack from "./pack.json" with { type: "json" };
import { hookTypePictureId, typeOfInput, typeOfName, typePictureId } from "./source-types.ts";

export type Picture = { id: string; group: string; label: string; on: string; off: string };

export const COLOURS: string[] = pack.colours;
export const DEFAULT_COLOUR = "mono";
export const PICTURES: Picture[] = pack.pictures;

const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));

const BLUR = ["gaussian", "box", "kawase", "pixelate", "zoom", "motion", "hexagons"];
const CORNERS = ["round", "cut", "notch", "scoop"];
const PLACES = [
	"top-left",
	"top-centre",
	"top-right",
	"middle-left",
	"centre",
	"middle-right",
	"bottom-left",
	"bottom-centre",
	"bottom-right",
];
const PLAIN: Record<string, string> = {
	stagekit_auto_capture: "filters/auto-capture",
	stagekit_auto_frame: "filters/auto-frame",
	stagekit_show_in_scenes: "filters/show-in-scenes",
	stagekit_standby: "filters/standby-picture",
	// Drawn for the kinds the pack has no picture of (tools/make-option-icons.py).
	stagekit_ramp_zoom: "options/zoom",
	stagekit_fit_item: "options/fit",
	stagekit_active_app: "options/source-switcher",
	stagekit_app_capture: "options/window-switcher",
	stagekit_program_trigger: "options/trigger",
	stagekit_visibility_link: "options/link",
	stagekit_show_hide: "options/show-hide",
	stagekit_layout: "options/layout",
	stagekit_scene_fit: "options/scene-fit",
	stagekit_scene_shadow: "options/scene-shadow",
	stagekit_scene_corners: "options/scene-corners",
	stagekit_scene_standby: "filters/standby-picture",
	stagekit_scene_modes: "options/mode",
};

type Settings = Record<string, unknown>;

/** A whole number setting; OBS leaves a setting out while it has its usual value. */
function whole(settings: Settings, name: string, usual: number): number {
	const value = settings[name];
	return typeof value === "number" && Number.isFinite(value) ? Math.trunc(value) : usual;
}

/**
 * The picture that fits a filter: by its kind and, where the pack has one
 * for each choice, by how the filter is set. Undefined where the pack has
 * nothing for that kind.
 */
export function automatic(kind: string, settings: Settings = {}): Picture | undefined {
	let id = PLAIN[kind];
	if (kind === "stagekit_ramp_blur") {
		id = `blur-types/${BLUR[whole(settings, "blur_type", 0)] ?? BLUR[0]}`;
	} else if (kind === "stagekit_corners") {
		id = `corners/${CORNERS[whole(settings, "style", 0)] ?? CORNERS[0]}`;
	} else if (kind === "stagekit_shadow_backdrop") {
		const shadow = settings.shadow !== false;
		const look =
			shadow && whole(settings, "shadow_type", 0) === 1
				? "glow"
				: !shadow && settings.outline === true
					? "outline"
					: !shadow && settings.backdrop === true
						? "panel"
						: "soft";
		id = `shadow-glow-outline/${look}`;
	} else if (kind === "stagekit_send_to" || kind === "stagekit_place_item") {
		// One of the nine places on the screen (top right is where a new one
		// starts); a saved spot has no picture of its own.
		const place = PLACES[whole(settings, "destination", 2)];
		id = place ? `options/camera-${place}` : "options/spot";
	}
	return id ? BY_ID.get(id) : undefined;
}

/** The pack's groups drawn source by source, for the filter kinds that have one. */
function sourceGroup(kind: string, settings: Settings): string | undefined {
	if (kind === "stagekit_show_in_scenes") return "show hide";
	if (kind === "stagekit_shadow_backdrop") return "shadow by source";
	if (kind === "stagekit_standby") return "standby by source";
	if (kind === "stagekit_ramp_blur") return `blur ${BLUR[whole(settings, "blur_type", 0)] ?? BLUR[0]}`;
	return undefined;
}

/**
 * The pack's picture of one source for a filter kind, by the source's name:
 * the pack has "show hide", shadow, standby and blur pictures drawn for
 * single sources ("square camera", "window capture" ...), and
 * tools/make-option-icons.py adds show keys for holding modes and columns
 * ("starting", "brb", "column chat" ...). It fits when all
 * the words of its label are in the source's name; of several, the one with
 * the most words. No source names are in this code.
 */
export function sourcePictureFor(kind: string, settings: Settings, source: string | undefined): Picture | undefined {
	const group = sourceGroup(kind, settings);
	const words = nameWords(source ?? "");
	if (!group || words.length === 0) return undefined;
	let best: Picture | undefined;
	let bestWords = 0;
	for (const picture of PICTURES) {
		if (picture.group !== group) continue;
		const own = nameWords(picture.label);
		if (own.length > bestWords && own.every((word) => words.includes(word))) {
			best = picture;
			bestWords = own.length;
		}
	}
	return best;
}

/** Kinds of sound source: they have pictures of their own, not a source type. */
const SOUND: Record<string, string> = {
	wasapi_input_capture: "options/microphone",
	wasapi_output_capture: "options/speaker",
	coreaudio_input_capture: "options/microphone",
	coreaudio_output_capture: "options/speaker",
	pulse_input_capture: "options/microphone",
	pulse_output_capture: "options/speaker",
};

/** The picture of a source type ("game", "camera" ...), or undefined for no such type. */
export function typePicture(sourceType: string | undefined): Picture | undefined {
	return BY_ID.get(typePictureId(sourceType ?? "") ?? "");
}

/** The picture of what a source is, by its kind in OBS: a camera, a game, a window, a screen ... */
export function inputPicture(inputKind: string | undefined): Picture | undefined {
	return typePicture(typeOfInput(inputKind)) ?? BY_ID.get(SOUND[inputKind ?? ""] ?? "");
}

/**
 * A filter key's picture: the one chosen; else the key's source type (not
 * for a filter on a scene, which is no source); else the pack's own picture
 * of the source it is on, by name; else, for a key that shows and hides a
 * source, the source type its name speaks of ("Be Right Back", "Main
 * Display") and then what OBS says the source is (a camera, a window ...);
 * else the one that fits the filter.
 *
 * (The source's name comes before its kind in OBS: a text source called
 * "BRB" is a be-right-back key, not a title, and a capture card is a camera
 * to OBS.)
 */
export function chooseFor(
	chosen: string | undefined,
	kind: string,
	settings: Settings = {},
	source?: string,
	inputKind?: string,
	sourceType?: string,
): Picture | undefined {
	const shows = kind === "stagekit_show_in_scenes";
	return (
		(chosen && BY_ID.get(chosen)) ||
		(isSceneKind(kind) ? undefined : typePicture(sourceType)) ||
		sourcePictureFor(kind, settings, source) ||
		(shows ? typePicture(typeOfName(source)) : undefined) ||
		(shows ? inputPicture(inputKind) : undefined) ||
		automatic(kind, settings)
	);
}

/** The plain key for what the pack has no picture of. */
export function plain(sort: "switch" | "move" | "tool"): Picture {
	return BY_ID.get(`options/plain-${sort}`)!;
}

/** A Hook key's picture: the kind of capture it hooks. */
export function hookPicture(chosen: string | undefined, inputKind: string | undefined, sourceType?: string): Picture {
	const fits = inputKind === "window_capture" ? "options/hook-window" : "options/hook-game";
	return (chosen && BY_ID.get(chosen)) || BY_ID.get(hookTypePictureId(sourceType ?? "") ?? "") || BY_ID.get(fits) || plain("move");
}

/** The arrange tools the pack has a picture of. */
const TOOLS: Record<string, string> = {
	same_everywhere: "options/copy-all",
	same_chosen: "options/copy-chosen",
	snap_now: "options/snap-now",
	left: "options/align-left",
	center: "options/align-center",
	right: "options/align-right",
	top: "options/align-top",
	middle: "options/align-middle",
	bottom: "options/align-bottom",
	space_across: "options/space-across",
	space_down: "options/space-down",
	same_width: "options/same-width",
	same_height: "options/same-height",
	same_size: "options/same-size",
};

export function toolPicture(chosen: string | undefined, tool: string | undefined): Picture {
	return (chosen && BY_ID.get(chosen)) || BY_ID.get(TOOLS[tool ?? ""] ?? "") || plain("tool");
}

/** The dock's switches the pack has a picture of. */
const OPTION_PICTURES: Record<string, string> = {
	snap: "options/snap",
	guides: "options/guides",
	limit: "options/limit",
	slide: "options/slide-to-edge",
	everyScene: "options/every-scene",
	keepShape: "options/keep-shape",
};

export function optionPicture(chosen: string | undefined, option: string | undefined): Picture {
	return (chosen && BY_ID.get(chosen)) || BY_ID.get(OPTION_PICTURES[option ?? "snap"] ?? "") || plain("switch");
}

export function slidePicture(chosen: string | undefined, direction: string | undefined): Picture {
	const fits = direction === "centre" ? "options/slide-centre" : `options/slide-${direction ?? "left"}`;
	return (chosen && BY_ID.get(chosen)) || BY_ID.get(fits) || plain("move");
}

/** A Manual Window / Source key's picture: one for each thing it can do. */
export function stepPicture(chosen: string | undefined, step: string | undefined, sourceType?: string): Picture {
	const fits =
		step === "clear"
			? "options/clear"
			: step === "toggle"
			? "options/toggle"
			: step === "hide"
				? "options/hide"
				: step === "back"
					? "filters/back-to-apps"
					: "filters/next-capture";
	return (chosen && BY_ID.get(chosen)) || typePicture(sourceType) || BY_ID.get(fits) || plain("move");
}

/**
 * A Scene Mode key's picture: the one chosen; else the key's source type;
 * else the mode's own type (its *Mode type* in OBS); else the type its name
 * speaks of ("Be Right Back", "GBA"); else the plain mode key.
 */
export function modePicture(chosen: string | undefined, sourceType: string | undefined, modeType?: string, modeName?: string): Picture {
	return (
		(chosen && BY_ID.get(chosen)) ||
		typePicture(sourceType) ||
		typePicture(modeType) ||
		typePicture(typeOfName(modeName)) ||
		BY_ID.get("options/mode") ||
		plain("switch")
	);
}

/** A Fit to Space Overlap key's picture: a window with a camera over its corner. */
export function overlapPicture(chosen: string | undefined): Picture {
	return (chosen && BY_ID.get(chosen)) || BY_ID.get("options/overlap") || plain("switch");
}

/** The words of a scene's or a picture's name: lower case, without "scene" and any leading number. */
function nameWords(name: string): string[] {
	return name
		.toLowerCase()
		.split(/[^a-z0-9]+/)
		.filter((word) => word !== "" && word !== "scene" && !/^\d+$/.test(word));
}

/**
 * The pack's own scene picture for a scene, by name: the pack has one drawn
 * from each scene's layout ("game component", "window capture" ...), and
 * tools/make-option-icons.py adds "live", "chat live" and "standby". It fits
 * when all its words are in the scene's name; of several, the one with the
 * most words, and then the one that follows the name furthest from its start
 * ("Game-Component-Capture" is "game component" before "game capture").
 */
export function scenePictureFor(scene: string | undefined): Picture | undefined {
	const words = nameWords(scene ?? "");
	if (words.length === 0) return undefined;
	let best: Picture | undefined;
	let bestScore = 0;
	for (const picture of PICTURES) {
		if (picture.group !== "scenes") continue;
		const own = nameWords(picture.label);
		if (own.length === 0 || !own.every((word) => words.includes(word))) continue;
		let run = 0;
		while (run < own.length && own[run] === words[run]) run++;
		const score = own.length * 10 + run;
		if (score > bestScore) {
			best = picture;
			bestScore = score;
		}
	}
	return best;
}

/**
 * A Switch Scene key's picture: the one chosen; else the pack's picture of
 * that scene's layout, when it has one by that name; else a scene with its
 * sources laid out.
 */
export function scenePicture(chosen: string | undefined, scene?: string): Picture {
	return (chosen && BY_ID.get(chosen)) || scenePictureFor(scene) || BY_ID.get("options/scene") || plain("switch");
}

/** A Set Aspect Ratio key's picture: a frame of that shape. */
export function shapePicture(chosen: string | undefined, shape: string | undefined): Picture {
	const fits = `options/ratio-${shape && shape !== "auto" ? shape.replace(":", "x") : "auto"}`;
	return (chosen && BY_ID.get(chosen)) || BY_ID.get(fits) || plain("switch");
}

/** The picture a key was given by name, or the one that fits its filter. */
export function choose(chosen: string | undefined, kind: string, settings: Settings = {}): Picture | undefined {
	return (chosen && BY_ID.get(chosen)) || automatic(kind, settings);
}

/**
 * A key with two states: the picture marked so that which way it is cannot be
 * missed (a green dot on, a red slash off; tools/make-state-icons.py).
 */
export function stateFile(picture: Picture, on: boolean, palette: string | undefined): string {
	const use = palette && COLOURS.includes(palette) ? palette : DEFAULT_COLOUR;
	return `imgs/pack/${use}/status/${on ? "on" : "off"}/${on ? picture.on : picture.off}.png`;
}

/** Where a picture's file is, inside the plugin's folder, in a key's palette. */
export function file(picture: Picture, on: boolean, palette: string | undefined): string {
	const use = palette && COLOURS.includes(palette) ? palette : DEFAULT_COLOUR;
	return `imgs/pack/${use}/${on ? picture.on : picture.off}.png`;
}

export type ListItem = { label: string; value: string } | { label: string; children: Array<{ label: string; value: string }> };

/** The pack as a list to choose from: "Automatic" first, then group by group. */
export function pictureItems(): ListItem[] {
	const groups = new Map<string, Array<{ label: string; value: string }>>();
	for (const picture of PICTURES) {
		if (!groups.has(picture.group)) groups.set(picture.group, []);
		groups.get(picture.group)!.push({ label: picture.label, value: picture.id });
	}
	const out: ListItem[] = [{ label: "Automatic (fits the filter)", value: "" }];
	for (const [group, children] of groups) out.push({ label: group, children });
	return out;
}
