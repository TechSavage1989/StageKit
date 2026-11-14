// What both kinds of key share: their settings, and filling the lists in the
// property inspector with what OBS has.

import streamDeck from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { filterItems, sourceItems, targetItems } from "../kinds.ts";
import { obs } from "../obs.ts";
import { pictureItems } from "../pictures.ts";

export type KeySettings = {
	/** The source and filter in one: what the keys for one feature choose from. */
	target?: string;
	source?: string;
	filter?: string;
	press?: "toggle" | "on" | "off";
	/** A picture of the pack by name; empty for the one that fits the filter. */
	picture?: string;
	/** The palette of this key's picture: mono unless another is chosen. */
	palette?: string;
	/** An arrange tool, for a Tool key. */
	tool?: string;
	/** A Hook key's change: an OBS transition's kind, or "none", and how long it lasts. */
	transition?: string;
	lasting?: string | number;
	/** A Picture shape key's shape ("4:3", "auto") and whether it lasts only for this game. */
	shape?: string;
	keep?: "game" | "always";
	/** A Window switch key: on to the next capture, or back to following the apps. */
	step?: "next" | "back";
	[key: string]: JsonValue | undefined;
};

/**
 * Answers the property inspector's lists (sdpi-components "datasource"):
 * `getSources` with the sources that have a filter of the wanted sort,
 * `getFilters` with the chosen source's filters of that sort, and
 * `getPictures` with the icon pack, and `getCaptures` with the Game and
 * Window Captures.
 */
export async function answerLists(
	payload: JsonValue,
	settings: KeySettings,
	wanted: (kind: string) => boolean,
): Promise<void> {
	const event = (payload as { event?: string } | null)?.event;
	if (event === "getPictures") {
		await streamDeck.ui.sendToPropertyInspector({ event, items: pictureItems() });
		return;
	}
	if (event === "getTransitions") {
		// "A fade" first, as everywhere in StageKit; then a cut; then the rest OBS has.
		const names: Record<string, string> = {
			fade_transition: "A fade",
			cut_transition: "",
			swipe_transition: "Swipe",
			slide_transition: "Slide",
			fade_to_color_transition: "Fade to colour",
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
			{ label: "A fade", value: "fade_transition" },
			{ label: "A cut (no transition)", value: "none" },
		];
		for (const kind of kinds) {
			if (kind === "fade_transition" || names[kind] === "") continue;
			items.push({ label: names[kind] ?? kind.replace(/_transition$/, "").replace(/_/g, " "), value: kind });
		}
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
