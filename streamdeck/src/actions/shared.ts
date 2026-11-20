// What both kinds of key share: their settings, and filling the lists in the
// property inspector with what OBS has.

import streamDeck from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { filterItems, sourceItems, switcherSteps, TARGET_SEP, targetItems, targetOf } from "../kinds.ts";
import { obs } from "../obs.ts";
import { adoptOld, typeForKept } from "../key-types.ts";
import { composed } from "../compose.ts";
import { modeKeyFilter } from "../modes.ts";
import { laidOut, layersOf, type LayoutSettings, type Picture, type Shown } from "../pictures.ts";
import { type Found, type WorksWith, worksWithOf, worksWithRow } from "../works-with.ts";

/** What a key's image depends on besides the picture: its palette and its Layout (with the type lists it goes by). */
export type Look = LayoutSettings & { palette?: string };

/**
 * A key's image as Stream Deck takes it: the picture's file, or, with a
 * badge (a type chosen with a function), the picture and its badges laid
 * together (src/compose.ts). `look` is the key's settings (its palette and
 * its Layout: the picture in the live or live + chat layout, src/pictures.ts
 * `laidOut`), or a palette alone. `state` is on or off for a key with two
 * states (the marked pictures), undefined for one.
 */
export function imageOf(shown: Picture | Shown, look: Look | string | undefined, state?: boolean): string {
	const plainShown: Shown = "picture" in shown ? shown : { picture: shown };
	const settings: Look = typeof look === "object" ? look : { palette: look };
	const s = laidOut(plainShown, settings);
	const palette = settings.palette;
	const layers = layersOf(s, palette, state);
	try {
		return composed(layers);
	} catch (error) {
		// (A file that cannot be read or laid together: the picture alone.)
		streamDeck.logger.warn(`Could not lay a badge over ${layers.base}: ${String(error)}`);
		return layers.base;
	}
}

/**
 * Shows whether a two-state key is on: its marked pictures for both states,
 * then the state. Stream Deck does not flip these keys by itself
 * ("DisableAutomaticStates" in the manifest), so what shows is always what
 * OBS says, never a flash of the other picture. A badge goes on both.
 */
export async function showOnOff(
	key: { setImage(image?: string, options?: { state?: 0 | 1 }): Promise<void>; setState(state: 0 | 1): Promise<void> },
	picture: Picture | Shown,
	on: boolean,
	look: Look | string | undefined,
): Promise<void> {
	await key.setImage(imageOf(picture, look, false), { state: 0 });
	await key.setImage(imageOf(picture, look, true), { state: 1 });
	await key.setState(on ? 1 : 0);
}

export { remembered, type Seen } from "../seen.ts";

/**
 * A key's settings from before beta156 brought up to date, once: the image
 * chosen in the old Image list is set as the type that shows it, where one
 * does (`resolve` is the key's own choice of picture), and kept aside
 * otherwise (src/key-types.ts). Returns the settings to go on with.
 */
export async function settle(
	key: { setSettings(settings: KeySettings): Promise<void> },
	name: string,
	settings: KeySettings,
	resolve: (settings: KeySettings) => string | undefined,
): Promise<KeySettings> {
	let next = settings;
	if ((name === "switch" || name === "move") && !settings.target && settings.source && settings.filter)
		next = { ...settings, target: settings.source + TARGET_SEP + settings.filter };
	if (typeof next.picture === "string")
		next = (typeForKept(name, next, (s) => resolve(s as KeySettings)) ?? adoptOld(next) ?? next) as KeySettings;
	if (next !== settings) await key.setSettings(next);
	return next;
}

export type KeySettings = {
	/** The source and filter in one: what the keys for one feature choose from. */
	target?: string;
	source?: string;
	filter?: string;
	press?: "toggle" | "on" | "off" | "cover" | "coveron" | "coveroff";
	/** A Show in Scenes key's way of hiding: "filter" (as the filter says; unset reads the same), "hide", "cover". */
	hideAs?: string;
	/**
	 * A picture of the pack by name, from the Image list before beta156: put
	 * aside as `oldImage` (with `oldImageWhile`) and shown until a type list
	 * is changed (src/key-types.ts).
	 */
	picture?: string;
	/**
	 * The key's type lists (src/key-types.ts): what the source it acts on is
	 * ("game", "camera" ...), what the scene is ("gameplay" ...), and the
	 * look of what it does (a blur style, a corner style, a position, a
	 * capture type). Empty or "auto" for Automatic.
	 */
	sourceType?: string;
	sceneType?: string;
	look?: string;
	/** The palette of this key's picture: mono unless another is chosen. */
	palette?: string;
	/** How the image is laid out (src/key-types.ts `LAYOUTS`): "standalone", "live", "chat"; empty or "auto" for Automatic. */
	layout?: string;
	/** An arrange tool, for a Tool key. */
	tool?: string;
	/** A Hook key's change: an OBS transition's kind, or "none", and how long it lasts. */
	transition?: string;
	lasting?: string | number;
	/** A Picture shape key's shape ("4:3", "auto") and whether it lasts only for this game. */
	shape?: string;
	keep?: "game" | "always";
	/** A Window switch key: on to the next capture, or back to following the apps. */
	step?: "next" | "back" | "hide" | "toggle" | "clear";
	/** A Switch Scene key's scene, and whether it goes to the program or the preview. */
	scene?: string;
	where?: "program" | "preview";
	/** A Dock switch key's switch ("snap", "guides", ...) and a Slide key's way ("up", "down-left", ...). */
	option?: string;
	direction?: string;
	[key: string]: JsonValue | undefined;
};

/**
 * What OBS says of what a key is set to (src/works-with.ts `Found`): the
 * chosen filter's kind, what the chosen capture is, or that OBS has no such
 * thing. Undefined while OBS cannot be asked, or nothing is chosen.
 */
async function foundInObs(works: WorksWith, settings: KeySettings): Promise<Found | undefined> {
	if (!obs.isConnected) return undefined;
	switch (works.chosen) {
		case "target": {
			const { source, filter } = targetOf(settings);
			if (!source || !filter) return undefined;
			const now = await obs.filter(source, filter);
			return now ? { kind: now.kind } : { missing: true };
		}
		case "modeFilter": {
			const { scene, filter } = modeKeyFilter(settings);
			if (!scene || !filter) return undefined;
			return (await obs.sceneMode(scene, filter)) ? { kind: "stagekit_scene_modes" } : { missing: true };
		}
		case "sceneBlur": {
			const filter = typeof settings.filter === "string" ? settings.filter : "";
			if (!filter) return undefined;
			const found = (await obs.sceneBlurs()).find((f) => f.filter === filter);
			const here = found && (!settings.scene || found.scenes.some((s) => s.scene === settings.scene));
			return here ? { kind: "stagekit_scene_blur" } : { missing: true };
		}
		case "capture": {
			if (!settings.source) return undefined;
			const capture = (await obs.captures()).find((c) => c.name === settings.source);
			return capture ? { what: capture.kind === "window_capture" ? "Window Capture" : "Game Capture" } : { missing: true };
		}
		case "scene":
			if (!settings.scene) return undefined;
			return (await obs.scenes()).includes(String(settings.scene)) ? {} : { missing: true };
		default:
			return undefined;
	}
}

/**
 * The first row of a key's settings page, *Works with* (src/works-with.ts):
 * what the key works in OBS and what it is set to, from its settings, and
 * from OBS when it can be asked (with OBS closed, the kind and the saved
 * choice alone). `uuid` is the key's action. Undefined for a key not in the
 * table.
 */
export async function worksWithNow(uuid: string, settings: KeySettings): Promise<{ kind: string; chosen: string } | undefined> {
	const works = worksWithOf(uuid);
	if (!works) return undefined;
	let found: Found | undefined;
	try {
		found = await foundInObs(works, settings);
	} catch (error) {
		streamDeck.logger.warn(`Could not ask OBS what a key works with: ${String(error)}`);
	}
	return worksWithRow(works, settings, found);
}

/**
 * Answers the property inspector's lists (sdpi-components "datasource"):
 * `getSources` with the sources that have a filter of the wanted sort,
 * `getFilters` with the chosen source's filters of that sort, and
 * `getCaptures` with the Game and Window Captures. And the page's first
 * row, `getWorksWith` (`worksWithNow`), for the key's action `uuid`.
 */
export async function answerLists(
	payload: JsonValue,
	settings: KeySettings,
	wanted: (kind: string) => boolean,
	uuid: string,
): Promise<void> {
	const event = (payload as { event?: string } | null)?.event;
	if (event === "getWorksWith") {
		const row = await worksWithNow(uuid, settings);
		if (row) await streamDeck.ui.sendToPropertyInspector({ event, ...row });
		return;
	}
	if (event === "getTransitions") {
		// The same transition names used on the Auto Capture filter.
		const names: Record<string, string> = {
			fade_transition: "Fade",
			cut_transition: "",
			swipe_transition: "Swipe",
			slide_transition: "Slide",
			fade_to_color_transition: "Fade to Color",
			wipe_transition: "Luma wipe",
			obs_stinger_transition: "",
		};
		let kinds: string[] = [];
		try {
			kinds = await obs.transitionKinds();
		} catch {
			// Not connected: the two that are always there.
		}
		const items = [
			{ label: "Fade", value: "fade_transition" },
			{ label: "Cut", value: "none" },
		];
		for (const kind of kinds) {
			if (kind === "fade_transition" || names[kind] === "") continue;
			items.push({ label: names[kind] ?? kind.replace(/_transition$/, "").replace(/_/g, " "), value: kind });
		}
		await streamDeck.ui.sendToPropertyInspector({ event, items });
		return;
	}
	if (event === "getScenes" || event === "getSceneTransitions") {
		let items: Array<{ label: string; value: string }> = [];
		try {
			items =
				event === "getScenes"
					? (await obs.scenes()).map((name) => ({ label: name, value: name }))
					: [{ label: "Leave OBS's as it is", value: "" }, ...(await obs.sceneTransitions()).map((name) => ({ label: name, value: name }))];
		} catch (error) {
			streamDeck.logger.warn(`Could not read the scenes from OBS: ${String(error)}`);
		}
		if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
		await streamDeck.ui.sendToPropertyInspector({ event, items });
		return;
	}
	if (event === "getCaptures") {
		let captures: Array<{ label: string; value: string }> = [];
		try {
			captures = (await obs.captures()).map((c) => ({
				label: `${c.name} (${c.kind === "window_capture" ? "Window Capture" : "Game Capture"})`,
				value: c.name,
			}));
		} catch (error) {
			streamDeck.logger.warn(`Could not read the captures from OBS: ${String(error)}`);
		}
		if (!obs.isConnected) captures = [{ label: "OBS is not connected (see below)", value: "" }];
		else if (captures.length === 0) captures = [{ label: "No Game Capture or Window Capture yet", value: "" }];
		await streamDeck.ui.sendToPropertyInspector({ event, items: captures });
		return;
	}
	if (event === "getSwitcherSteps") {
		const { source, filter } = targetOf(settings);
		const current = await obs.filter(source, filter);
		const items = switcherSteps(current?.kind);
		if (!items.length) items.push({ value: "", label: obs.isConnected ? "Choose a source or scene and filter first" : "Connect to OBS to see its controls" });
		await streamDeck.ui.sendToPropertyInspector({ event, items });
		return;
	}
	if (event !== "getSources" && event !== "getFilters" && event !== "getTargets") return;
	let items: Array<{ label: string; value: string }> = [];
	try {
		const filters = await obs.stageKitFilters();
		items =
			event === "getTargets"
				? targetItems(filters, wanted)
				: event === "getSources"
					? sourceItems(filters, wanted)
					: filterItems(filters, settings.source ?? "", wanted);
	} catch (error) {
		streamDeck.logger.warn(`Could not read the filters from OBS: ${String(error)}`);
	}
	if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
	else if (items.length === 0)
		items = [
			{
				label: event === "getFilters" ? "Choose a source first" : "Nothing in OBS has this filter yet",
				value: "",
			},
		];
	await streamDeck.ui.sendToPropertyInspector({ event, items });
}
