// What both kinds of key share: their settings, and filling the lists in the
// property inspector with what OBS has.

import streamDeck from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { filterItems, sourceItems } from "../kinds.ts";
import { obs } from "../obs.ts";
import { pictureItems } from "../pictures.ts";

export type KeySettings = {
	source?: string;
	filter?: string;
	press?: "toggle" | "on" | "off";
	/** A picture of the pack by name; empty for the one that fits the filter. */
	picture?: string;
	/** The palette of this key's picture: mono unless another is chosen. */
	palette?: string;
	/** An arrange tool, for a Tool key. */
	tool?: string;
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
	if (event !== "getSources" && event !== "getFilters") return;
	let items: Array<{ label: string; value: string }> = [];
	try {
		const filters = await obs.stageKitFilters();
		items = event === "getSources" ? sourceItems(filters, wanted) : filterItems(filters, settings.source ?? "", wanted);
	} catch (error) {
		streamDeck.logger.warn(`Could not read the filters from OBS: ${String(error)}`);
	}
	if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
	else if (items.length === 0)
		items = [{ label: event === "getSources" ? "No source has such a filter yet" : "Choose a source first", value: "" }];
	await streamDeck.ui.sendToPropertyInspector({ event, items });
}
