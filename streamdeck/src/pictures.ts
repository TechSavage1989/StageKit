// Which picture a key shows. The pictures are the StageKit icon pack, kept in
// imgs/pack/<colour>/<group>/<name>.png and listed in pack.json. Left to
// itself a key takes the picture that fits what its filter is and how it is
// set; its type lists (src/key-types.ts) say what it is for instead. An
// image a key was given in the Image list before beta156 is still shown
// (`chosen` below) until one of its type lists is changed.

import { isSceneKind } from "./kinds.ts";
import pack from "./pack.json" with { type: "json" };
import layouts from "./layouts.json" with { type: "json" };
import { BLUR_STYLES, explicit, type Family, layoutOf, lookPicture, packBlurStyle, sceneTypePicture, sourceTypePicture, typeBehind } from "./key-types.ts";
import { hookTypePictureId, typeOfInput, typeOfName, typePictureId } from "./source-types.ts";

export type Picture = { id: string; group: string; label: string; on: string; off: string };

export const COLOURS: string[] = pack.colours;
export const DEFAULT_COLOUR = "mono";
export const PICTURES: Picture[] = pack.pictures;

const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));

const BLUR = BLUR_STYLES;
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
	stagekit_match_item: "options/match-item",
	stagekit_scene_transitions: "options/scene-transitions",
	stagekit_scene_shadow: "options/scene-shadow",
	stagekit_scene_corners: "options/scene-corners",
	stagekit_scene_standby: "filters/standby-picture",
	stagekit_scene_modes: "options/mode",
	stagekit_scene_blur: "blur-types/gaussian",
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
export function typePicture(sourceType: string | undefined, family?: Family): Picture | undefined {
	return BY_ID.get((family ? sourceTypePicture(sourceType, family) : typePictureId(sourceType ?? "")) ?? "");
}

/** The picture of what a source is, by its kind in OBS: a camera, a game, a window, a screen ... */
export function inputPicture(inputKind: string | undefined, family?: Family): Picture | undefined {
	return typePicture(typeOfInput(inputKind), family) ?? BY_ID.get(SOUND[inputKind ?? ""] ?? "");
}

/** The owner's art a filter kind's keys use for a source type. */
function familyOf(kind: string): Family | undefined {
	if (kind === "stagekit_show_in_scenes") return "show";
	if (kind === "stagekit_shadow_backdrop") return "shadow";
	if (kind === "stagekit_ramp_blur") return "blur";
	return undefined;
}

/** What a key's type lists are set to (src/key-types.ts). */
export type KeyTypes = { sourceType?: unknown; sceneType?: unknown; look?: unknown };

/**
 * A filter key's picture. In this order: the type chosen in the key's lists
 * (for a filter on a scene its Scene type, for one on a source its Source
 * type, drawn for the key's function: a Shadow key's type with its shadow, a
 * Blur key's blurred in the style); else the image kept from before the
 * type lists (`chosen`); else the style chosen (Blur style, Corner style,
 * Position); else the pack's own picture of the source it is on, by name;
 * else, for a key that shows and hides a source, the source type its name
 * speaks of ("Be Right Back", "Main Display") and then what OBS says the
 * source is (a camera, a window ...); else the one that fits the filter.
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
	types: KeyTypes | string = {},
): Picture | undefined {
	const t: KeyTypes = typeof types === "string" ? { sourceType: types } : types;
	const family = familyOf(kind);
	const look = explicit(t.look);
	const blur = kind === "stagekit_ramp_blur";
	const style = blur ? (look ?? BLUR[whole(settings, "blur_type", 0)] ?? BLUR[0]) : undefined;
	const scene = isSceneKind(kind);
	const typed = scene
		? sceneTypePicture(explicit(t.sceneType))
		: sourceTypePicture(explicit(t.sourceType), family, style);
	if (typed && BY_ID.has(typed)) return BY_ID.get(typed);
	if (chosen && BY_ID.has(chosen)) return BY_ID.get(chosen);
	if (look && kind === "stagekit_show_hide")
		return BY_ID.get(lookPicture("animation", look) ?? lookPicture("transition", look) ?? "") ?? automatic(kind, settings);
	if (look && (kind === "stagekit_corners" || kind === "stagekit_scene_corners"))
		return BY_ID.get(lookPicture("corners", look) ?? "") ?? automatic(kind, settings);
	if (look && (kind === "stagekit_send_to" || kind === "stagekit_place_item"))
		return BY_ID.get(lookPicture("position", look) ?? "") ?? automatic(kind, settings);
	if (look && (kind === "stagekit_shadow_backdrop" || kind === "stagekit_scene_shadow"))
		return BY_ID.get(lookPicture("shadow", look) ?? "") ?? automatic(kind, settings);
	const changes = ["stagekit_visibility_link", "stagekit_active_app", "stagekit_app_capture", "stagekit_scene_transitions"];
	if (look && changes.includes(kind)) return BY_ID.get(lookPicture("transition", look) ?? "") ?? automatic(kind, settings);
	const own = blur ? { ...settings, blur_type: BLUR.indexOf(packBlurStyle(style!)) } : settings;
	// (A Blur style the pack has no pictures of is its own drawing, unless the source has one in the nearest style.)
	const styled = blur && look ? BY_ID.get(lookPicture("blur", look) ?? "") : undefined;
	const shows = kind === "stagekit_show_in_scenes";
	return (
		sourcePictureFor(kind, own, source) ||
		(shows ? BY_ID.get(sourceTypePicture(typeOfName(source), "show") ?? "") : undefined) ||
		(shows ? inputPicture(inputKind, "show") : undefined) ||
		styled ||
		automatic(kind, own)
	);
}

/** The plain key for what the pack has no picture of. */
export function plain(sort: "switch" | "move" | "tool"): Picture {
	return BY_ID.get(`options/plain-${sort}`)!;
}

/** A Hook key's picture: the kind of capture it hooks. */
export function hookPicture(chosen: string | undefined, inputKind: string | undefined, sourceType?: string, look?: unknown): Picture {
	const fits = inputKind === "window_capture" ? "options/hook-window" : "options/hook-game";
	return (
		BY_ID.get(lookPicture("capture", explicit(look)) ?? "") ||
		(chosen && BY_ID.get(chosen)) ||
		BY_ID.get(hookTypePictureId(sourceType ?? "") ?? "") ||
		BY_ID.get(fits) ||
		plain("move")
	);
}

/** The arrange tools the pack has a picture of. */
const TOOLS: Record<string, string> = {
	same_everywhere: "options/copy-all",
	same_chosen: "options/copy-chosen",
	fit_space: "options/fit-space",
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
 * A Scene Mode key's picture: its Mode type; else the image kept from before
 * the type lists; else the mode's own type (its *Mode type* in OBS); else the
 * type its name speaks of ("Be Right Back"); else the plain mode key. A type
 * is drawn as a mode: a holding screen, or the owner's scene layout of that
 * kind ("mode" in src/key-types.json).
 */
export function modePicture(chosen: string | undefined, sourceType: unknown, modeType?: string, modeName?: string): Picture {
	const as = (type: string | undefined) => BY_ID.get(sourceTypePicture(type, "mode") ?? "");
	return (
		as(explicit(sourceType)) ||
		(chosen && BY_ID.get(chosen)) ||
		as(modeType) ||
		as(typeOfName(modeName)) ||
		BY_ID.get("options/mode") ||
		plain("switch")
	);
}

/**
 * A Scene Mode key's image: `on` is the mode it shows (src/modes.ts
 * `shownMode`: the mode that is on, or a Switch on key's own mode) (its id, its Mode type in OBS and its name), or
 * undefined for none. Its picture: the one chosen for that mode on the key
 * (`perMode`, by id); for the key's own mode (`own`), the key's Mode type
 * and then an image kept from before the type lists; then the mode's own type,
 * the type its name speaks of, the plain mode key, as `modePicture`. With no
 * mode on, the plain mode key. Returns the picture and the look to lay it out
 * with: the mode's own Layout where one is chosen, else the key's.
 */
export function followedModePicture(
	settings: { sourceType?: unknown; layout?: unknown; palette?: unknown; perMode?: unknown },
	own: string,
	kept: string | undefined,
	on: { id: string; type?: string; name?: string } | undefined,
): { picture: Picture; look: LayoutSettings & { palette?: string } } {
	const palette = typeof settings.palette === "string" ? settings.palette : undefined;
	if (!on || !on.id) return { picture: modePicture(undefined, undefined), look: { palette, layout: settings.layout } };
	const all = settings.perMode && typeof settings.perMode === "object" ? (settings.perMode as Record<string, { type?: unknown; layout?: unknown }>) : {};
	const mine = all[on.id] ?? {};
	const isOwn = on.id === own;
	const type = explicit(mine.type) ?? (isOwn ? explicit(settings.sourceType) : undefined);
	const picture = modePicture(isOwn ? kept : undefined, type, on.type, on.name);
	const layout = explicit(mine.layout) ?? settings.layout;
	return { picture, look: { palette, layout, sourceType: type } };
}

/** A Fit to Space Overlap key's picture: a window with a camera over its corner. */
export function overlapPicture(chosen: string | undefined, sceneType?: unknown): Picture {
	return BY_ID.get(sceneTypePicture(explicit(sceneType)) ?? "") || (chosen && BY_ID.get(chosen)) || BY_ID.get("options/overlap") || plain("switch");
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
 * A Switch Scene key's picture: its Scene type; else the image kept from
 * before the type lists; else the pack's picture of that scene's layout,
 * when it has one by that name; else a scene with its sources laid out.
 */
export function scenePicture(chosen: string | undefined, scene?: string, sceneType?: unknown): Picture {
	return (
		BY_ID.get(sceneTypePicture(explicit(sceneType)) ?? "") ||
		(chosen && BY_ID.get(chosen)) ||
		scenePictureFor(scene) ||
		BY_ID.get("options/scene") ||
		plain("switch")
	);
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
 * A Move Source, Move Scene Item or Apply Layout key's picture: its Position
 * (or, for a layout, its Scene type); else the image kept from before the
 * type lists; else where the filter sends the source.
 */
export function movePicture(chosen: string | undefined, kind: string, settings: Settings = {}, types: KeyTypes = {}): Picture | undefined {
	const typed = kind === "stagekit_layout" ? sceneTypePicture(explicit(types.sceneType)) : lookPicture("position", explicit(types.look));
	return BY_ID.get(typed ?? "") || choose(chosen, kind, settings);
}

/** A Blur All key's picture: its Blur style; else the image kept from before the type lists; else Gaussian. */
export function blurAllPicture(chosen: string | undefined, look?: unknown): Picture {
	return BY_ID.get(lookPicture("blur", explicit(look)) ?? "") || choose(chosen, "stagekit_ramp_blur") || plain("switch");
}

/**
 * A Scene Blur key's picture: its Blur style; else the image kept from before
 * the type lists; else the entry of the Blur style list the filter is set to
 * (its `style` as ListSceneBlurs says it); else Gaussian.
 */
export function sceneBlurPicture(chosen: string | undefined, look?: unknown, style?: string): Picture {
	return (
		BY_ID.get(lookPicture("blur", explicit(look)) ?? "") ||
		(chosen && BY_ID.get(chosen)) ||
		BY_ID.get(lookPicture("blur", style) ?? "") ||
		BY_ID.get("blur-types/gaussian") ||
		plain("switch")
	);
}

/**
 * A Scene Blur key, as Blur All: its Scene type with its Blur style in the
 * corner (on Automatic, the style the filter is set to, else Gaussian); with
 * no Scene type, the blur style's own picture.
 */
export function sceneBlurShown(chosen: string | undefined, look?: unknown, sceneType?: unknown, style?: string): Shown {
	const typed = BY_ID.get(sceneTypePicture(explicit(sceneType)) ?? "");
	if (typed) return { picture: typed, badge: blurBadge(explicit(look) ?? (style || "gaussian")) };
	return { picture: sceneBlurPicture(chosen, look, style) };
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

// ------------------------------------------------- a type with its function
// The owner, of the Manual Window / Source key: "no types selection for
// manual window/source? all keys must have the options". A key whose own
// list is what it does (a step, a tool, a slide, an aspect ratio, a dock
// option) or whose look is (a kind of capture, a blur style, a destination)
// also has a type list for what it acts on. With a type chosen, the key shows
// the type's picture with what it does drawn as a badge in the top right
// corner (tools/make-badges.py, laid over it by src/compose.ts). With none
// (Automatic) it shows exactly what it showed before.

/**
 * A key's image: a picture, and the badges laid over it (none: the picture as
 * it is). With `inset`, the picture is laid, made smaller, into that layout's
 * main panel (a picture with no layout of its own; see `laidOut`).
 */
export type Shown = { picture: Picture; badge?: string; extra?: string; inset?: "live" | "chat" };

/** One string for what a key shows, for comparing (`settle`'s resolvers). */
export function shownId(shown: Shown): string {
	return [shown.inset ? `${shown.inset}:${shown.picture.id}` : shown.picture.id, shown.badge, shown.extra].filter(Boolean).join("+");
}

// ------------------------------------------------------------------ layouts
// The owner: "every icon should have a standalone version, live/no chat
// version, and a component/chat version" and "perhaps have a selector to
// choose the layout and a second selector for the specific type". A key's
// Layout (src/key-types.ts) takes what the key would show and puts it in the
// pack's two scene layouts (tools/make-option-icons.py, src/layouts.json).

const BY_PICTURE = layouts.byPicture as Record<string, { live: string; chat: string }>;
const GLYPH_OF = layouts.glyphOf as Record<string, string>;
const STANDALONE = new Set<string>(layouts.standalone);
export const LAYOUT_FRAMES = layouts.frames as { live: string; chat: string };
export const LAYOUT_PANEL = layouts.panel as { live: number[]; chat: number[] };
export const LAYOUT_CROP: number[] = layouts.crop;

/** What a key's Layout depends on: the list itself and the type lists (for the picture a type's art stands for). */
export type LayoutSettings = { layout?: unknown; sourceType?: unknown; sceneType?: unknown };

/**
 * What a key shows in its Layout. Automatic (and a key from before there was
 * one): exactly what it showed. Standalone: the same, but a scene picture
 * that is itself a layout (the pack's game, capture card and window scenes)
 * is its drawing alone. Live (no chat) and Live + chat: the picture of that
 * type in that layout, the pack's own where it has one (game, window, capture
 * card), drawn otherwise; a key showing its Source type with the owner's art
 * for its function (an eye, a shadow, a blur) goes by the type. A picture
 * with no layout of its own (an app's logo, the owner's art for a source on
 * Automatic, an option's drawing) is laid, smaller, into the layout's panel
 * (`inset`; src/compose.ts). Badges and the on/off mark stay as they are.
 */
export function laidOut(shown: Shown, settings: LayoutSettings = {}): Shown {
	const layout = layoutOf(settings.layout);
	if (!layout) return shown;
	const behind = typeBehind(settings as Record<string, unknown>, shown.picture.id) ?? shown.picture.id;
	if (layout === "standalone") {
		const glyph = STANDALONE.has(behind) ? BY_ID.get(GLYPH_OF[behind]) : undefined;
		return glyph ? { ...shown, picture: glyph } : shown;
	}
	const own = BY_PICTURE[behind]?.[layout] ?? BY_PICTURE[GLYPH_OF[behind] ?? ""]?.[layout];
	const drawn = own ? BY_ID.get(own) : undefined;
	return drawn ? { ...shown, picture: drawn } : { ...shown, inset: layout };
}

function palette(of: string | undefined): string {
	return of && COLOURS.includes(of) ? of : DEFAULT_COLOUR;
}

/**
 * What src/compose.ts lays together for a key: the picture's file (marked on
 * or off for a key with two states) and its badges. A picture laid into a
 * layout (`inset`) is the empty layout with the picture in its panel, and
 * the on/off mark (`mark`) is put on the whole when it is laid together.
 */
export function layersOf(
	shown: Shown,
	colour: string | undefined,
	state?: boolean,
): { base: string; palette: string; right?: string; left?: string; inset?: { file: string; panel: number[]; crop: number[] }; mark?: boolean } {
	const right = shown.badge;
	const left = shown.extra;
	if (shown.inset) {
		const frame = BY_ID.get(LAYOUT_FRAMES[shown.inset])!;
		return {
			base: file(frame, true, colour),
			palette: palette(colour),
			right,
			left,
			inset: { file: file(shown.picture, true, colour), panel: LAYOUT_PANEL[shown.inset], crop: LAYOUT_CROP },
			mark: state,
		};
	}
	const base = state === undefined ? file(shown.picture, true, colour) : stateFile(shown.picture, state, colour);
	return { base, palette: palette(colour), right, left };
}

/** The picture of a source type chosen in a key's list, or undefined for Automatic. */
function typedPicture(sourceType: unknown): Picture | undefined {
	return typePicture(explicit(sourceType));
}

/** The badge of a picture of options/ drawn by make-option-icons.py (the same drawing). */
function badgeOf(picture: Picture): string {
	return picture.id.replace(/^options\//, "");
}

const STEP_BADGES: Record<string, string> = { toggle: "step-toggle", next: "step-next", back: "step-back", hide: "hide", clear: "clear" };

/** A Manual Window / Source key: its Source type with its step, else its picture as before. */
export function stepShown(chosen: string | undefined, step: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: STEP_BADGES[step ?? "next"] ?? "step-next" };
	return { picture: stepPicture(chosen, step, typeof sourceType === "string" ? sourceType : undefined) };
}

/** An Arrange tool key: its Source type with its tool, else the tool. */
export function toolShown(chosen: string | undefined, tool: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: badgeOf(toolPicture(undefined, tool)) };
	return { picture: toolPicture(chosen, tool) };
}

/** A Reset Stats key: its chosen type with the reset symbol, else the statistics and reset drawing. */
export function resetStatsShown(chosen: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: "reset-stats" };
	return { picture: BY_ID.get(chosen ?? "") ?? BY_ID.get("options/reset-stats")! };
}

/** A Dock Option key: its Source type with its option, else the option. */
export function optionShown(chosen: string | undefined, option: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: badgeOf(optionPicture(undefined, option)) };
	return { picture: optionPicture(chosen, option) };
}

/** A Slide to Edge key: its Source type with an arrow to the edge, else the slide. */
export function slideShown(chosen: string | undefined, direction: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: badgeOf(slidePicture(undefined, direction)) };
	return { picture: slidePicture(chosen, direction) };
}

/** A Set Aspect Ratio key: its Source type with a frame of the ratio, else the ratio. */
export function shapeShown(chosen: string | undefined, shape: string | undefined, sourceType?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) return { picture: typed, badge: badgeOf(shapePicture(undefined, shape)) };
	return { picture: shapePicture(chosen, shape) };
}

/**
 * A Capture Active Window key: its Source type with the kind of capture (its
 * Capture type, else what OBS says the capture is); else as before.
 */
export function hookShown(chosen: string | undefined, inputKind: string | undefined, sourceType?: unknown, look?: unknown): Shown {
	const typed = typedPicture(sourceType);
	if (typed) {
		const capture = explicit(look) ?? (inputKind === "window_capture" ? "window" : "game");
		return { picture: typed, badge: capture === "window" ? "hook-window" : "hook-game" };
	}
	return { picture: hookPicture(chosen, inputKind, typeof sourceType === "string" ? sourceType : undefined, look) };
}

/** The badge of a Blur style (the filter's numbers' names and the Blur style list's). */
export function blurBadge(style: string): string {
	return BLUR_STYLES.includes(style) ? `blur-${style}` : `style-${style.replace(/_/g, "-")}`;
}

/** A Blur All key: its Scene type with its Blur style (Gaussian on Automatic), else as before. */
export function blurAllShown(chosen: string | undefined, look?: unknown, sceneType?: unknown): Shown {
	const typed = BY_ID.get(sceneTypePicture(explicit(sceneType)) ?? "");
	if (typed) return { picture: typed, badge: blurBadge(explicit(look) ?? "gaussian") };
	return { picture: blurAllPicture(chosen, look) };
}

/** The badge of a destination: by the Position list's value, or the filter's number. */
export function placeBadge(place: string | number | undefined): string {
	if (typeof place === "number") {
		const at = PLACES[Math.trunc(place)];
		return at ? `camera-${at}` : Math.trunc(place) === 10 ? "screen-spot" : "spot";
	}
	if (place === "spot") return "spot";
	if (place === "screen") return "screen-spot";
	return PLACES.includes(place ?? "") ? `camera-${place}` : "camera-top-right";
}

/**
 * A Move Source or Move Scene Item key's second badge, from how its filter is
 * set: a linked source brought along (flipped round the moved one or not),
 * else the move made in this scene or a chosen one only. Nothing for the
 * usual: every scene, nothing brought along.
 */
export function moveExtra(settings: Settings = {}): string | undefined {
	if (typeof settings.linked === "string" && settings.linked !== "") return settings.flip_linked === false ? "move-linked" : "move-flip";
	const where = whole(settings, "apply_in", 0);
	return where === 1 ? "scene-this" : where === 2 ? "scene-chosen" : undefined;
}

/**
 * A Move Source or Move Scene Item key: its Source type with its destination
 * (its Position, else where the filter sends the source) top right and the
 * move's extra option top left; else as before (movePicture). Apply Layout
 * has no Source type.
 */
export function moveShown(chosen: string | undefined, kind: string, settings: Settings = {}, types: KeyTypes = {}): Shown | undefined {
	const typed = kind === "stagekit_layout" ? undefined : typedPicture(types.sourceType);
	if (typed) {
		const look = explicit(types.look);
		const place = look ?? (typeof settings.destination === "number" ? settings.destination : 2);
		return { picture: typed, badge: placeBadge(place), extra: moveExtra(settings) };
	}
	const picture = movePicture(chosen, kind, settings, types);
	return picture ? { picture } : undefined;
}

// ------------------------------------------------------ Stream and Record
// The two keys' pictures are frames of tools/make-live-icons.py (src/live.ts).

/** A picture of the pack by id (a Stream or Record key's frame). */
export function pictureById(id: string): Picture | undefined {
	return BY_ID.get(id);
}

/** A Stream or Record key's badge for OBS's state, over a Scene type's picture. */
export function outputBadge(output: "stream" | "record", state: string): string {
	if (output === "record" && state === "paused") return "record-paused";
	return `${output}-${state !== "idle" && state !== "starting" ? "on" : "off"}`;
}

/**
 * A Stream or Record key's still image: with a Scene type, the type's
 * picture with OBS's state as a badge (it does not move); else the state's
 * own picture (its first frame).
 */
export function outputShown(output: "stream" | "record", state: string, firstFrame: string, sceneType?: unknown): Shown {
	const typed = BY_ID.get(sceneTypePicture(explicit(sceneType)) ?? "");
	if (typed) return { picture: typed, badge: outputBadge(output, state) };
	return { picture: BY_ID.get(firstFrame) ?? plain("switch") };
}

export function timerPicture(sourceType?: string): Picture {
	return pictureById(sourceTypePicture(sourceType) ?? "options/type-countdown") ?? plain("switch");
}

export function ordinaryControlPicture(key: string, source: string | undefined, sourceType?: string, sceneType?: string): Picture {
	if (key === "obsstudio" || key === "movesourceport" || key === "moveswapport") return scenePicture(undefined, source, sceneType);
	return chooseFor(undefined, "", {}, source, undefined, { sourceType }) ?? plain("switch");
}
