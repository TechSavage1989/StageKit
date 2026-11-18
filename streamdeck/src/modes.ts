// What a Scene Mode key points at, the lists it chooses from, what a press
// sends and which mode its image shows. No Stream Deck code here: the tests
// use it as it is.
//
// The owner: "the key should connect to the selected scene mode filter, not
// individual modes in all filters, and it should allow configuration of the
// specific modes as desired, whether it's on, off, toggle, or next", and
// "allow for the mode key to have a layout/mode type selection for each
// associated mode".

import { TARGET_SEP } from "./kinds.ts";

export type ModeFilter = { scene: string; filter: string; modes: Array<{ id: string; name: string }> };

/** A press of a Scene Mode key: its mode on, no mode (Switch off), toggle its mode, or the next mode used. */
export type ModePress = "on" | "off" | "toggle" | "next";

/** A mode's own settings on the key: its picture (a Mode type and a Layout, Automatic when unset) and whether Next mode skips it. */
export type PerMode = { type?: string; layout?: string; skip?: boolean };

const str = (v: unknown) => (typeof v === "string" ? v : "");
const isAuto = (v: unknown) => typeof v !== "string" || v === "" || v === "auto";

/**
 * Up to beta174 a key's one setting, `target`: the scene, the filter and the
 * mode's id. Read here for a key saved then; `migrateModeKey` moves it to
 * `modeFilter` and `mode`.
 */
export function modeTarget(target: string | undefined): { scene: string; filter: string; mode: string } {
	const parts = (target ?? "").split(TARGET_SEP);
	if (parts.length < 3) return { scene: "", filter: "", mode: "" };
	return { scene: parts[0], filter: parts[1], mode: parts.slice(2).join(TARGET_SEP) };
}

/** The Scene Modes filter a key is tied to (`modeFilter`: the scene and the filter's name), or an old key's. */
export function modeKeyFilter(settings: Record<string, unknown>): { scene: string; filter: string } {
	const own = str(settings.modeFilter);
	if (own) {
		const parts = own.split(TARGET_SEP);
		return { scene: parts[0] ?? "", filter: parts.slice(1).join(TARGET_SEP) };
	}
	const old = modeTarget(str(settings.target));
	return { scene: old.scene, filter: old.filter };
}

/** The value of one filter in the key's Scene Modes list. */
export function modeFilterValue(scene: string, filter: string): string {
	return [scene, filter].join(TARGET_SEP);
}

/**
 * A key saved before the key was tied to a filter (one setting `target`, a
 * mode of one filter) brought up to date: the same filter, the same mode as
 * its press's mode, and its press as it was (Switch on or Toggle). Undefined
 * when there is nothing to move.
 */
export function migrateModeKey<T extends Record<string, unknown>>(settings: T): T | undefined {
	if (str(settings.modeFilter)) return undefined;
	const old = modeTarget(str(settings.target));
	if (!old.scene) return undefined;
	const filter = modeFilterValue(old.scene, old.filter);
	return { ...settings, modeFilter: filter, modeFilterWas: filter, mode: str(settings.mode) || old.mode };
}

/**
 * The key's Scene Modes list: every Scene Modes filter with modes, by its
 * scene and the filter's name ("Standby — Holding modes"), as every key's
 * list names its filter.
 */
export function modeFilterItems(filters: ModeFilter[]): Array<{ label: string; value: string }> {
	return [...filters]
		.sort((a, b) => a.scene.localeCompare(b.scene, undefined, { sensitivity: "base" }))
		.map((f) => ({ label: `${f.scene} — ${f.filter}`, value: modeFilterValue(f.scene, f.filter) }));
}

/** The modes of the key's filter, in OBS's order: the Mode, Long press mode and Settings for lists. */
export function modeChoiceItems(modes: Array<{ id: string; name: string }>): Array<{ label: string; value: string }> {
	return modes.map((m) => ({ label: m.name || m.id, value: m.id }));
}

/** A key's press ("on" when unset, as it always was). */
export function modePress(press: unknown): ModePress {
	return press === "off" || press === "toggle" || press === "next" ? press : "on";
}

/** A key's long press, or undefined for none ("none" or unset: the key acts the moment it is pressed). */
export function modeLongPress(press: unknown): ModePress | undefined {
	return press === "on" || press === "off" || press === "toggle" || press === "next" ? press : undefined;
}

/** The key's own settings of each mode, by mode id. */
export function perModeOf(settings: Record<string, unknown>): Record<string, PerMode> {
	const all = settings.perMode;
	return all && typeof all === "object" && !Array.isArray(all) ? (all as Record<string, PerMode>) : {};
}

/**
 * What a press sends to SetSceneMode, or undefined when it cannot be sent
 * (no mode chosen for Switch on or Toggle, no mode used for Next mode).
 * Next mode with every mode used is the filter's own `next` (it goes through
 * them in order and wraps, in OBS's tick); with some left out it is worked out
 * here, from the mode on now (`now`, which must be known), as `on` the next
 * mode used after it in the filter's order, wrapping.
 */
export function modeRequest(
	press: ModePress,
	mode: string,
	perMode: Record<string, PerMode>,
	now?: { current: string; modes: Array<{ id: string }> },
): { how: "on" | "toggle" | "next" | "none"; mode?: string } | undefined {
	if (press === "off") return { how: "none" };
	if (press === "on" || press === "toggle") return mode ? { how: press, mode } : undefined;
	const used = (id: string) => perMode[id]?.skip !== true;
	if (!now) return Object.keys(perMode).some((id) => !used(id)) ? undefined : { how: "next" };
	const modes = now.modes;
	if (modes.every((m) => used(m.id))) return modes.length ? { how: "next" } : undefined;
	// (From the mode after the one on, round to it; with none on, from the first.)
	const at = modes.findIndex((m) => m.id === now.current);
	for (let step = 1; step <= modes.length; step++) {
		const m = modes[(at + step) % modes.length];
		if (used(m.id)) return { how: "on", mode: m.id };
	}
	return undefined;
}

/**
 * Which mode a key's image shows, and whether it is lit. Switch on: its own
 * mode, lit while that mode is on (as it always was). The others show the
 * mode that is on, or none: Toggle lit while its own mode is on, Switch off
 * and Next mode while any mode is.
 */
export function shownMode(press: ModePress, own: string, current: string): { shows: string; lit: boolean } {
	if (press === "on") return { shows: own, lit: own !== "" && current === own };
	if (press === "toggle") return { shows: current, lit: own !== "" && current === own };
	return { shows: current, lit: current !== "" };
}

/**
 * The page has one *Mode settings* list (`modeFor`, a mode of the key's
 * filter; "Settings for" up to beta179) and, under it, *Use in Next*, *Layout*
 * and *Image* for that mode; what they say is kept per mode in `perMode`. The
 * owner (2026-11-18): "the scene modes key is very long in the configuration
 * when the different modes are stacked. allow the mode to configure to be
 * selected from another selector under the scene modes selector, and have it's
 * configurations configurable".
 *
 * Given the settings the page sent, returns them brought into line, or
 * undefined when nothing needs writing: with no mode chosen in Mode settings
 * (a new key, or after another Scene Modes was chosen) it is set to the key's
 * own mode (`mode`, *Switch to mode*), else to `firstMode`, the filter's first
 * mode when the caller knows it; choosing another mode loads its settings into
 * the lists; changing the lists stores them for the mode chosen; choosing
 * another Scene Modes filter forgets the old filter's modes (ids are only one
 * filter's).
 *
 * Keys saved up to beta179 may carry the key-wide *Layout* (`layout`) and
 * *Mode type* (`sourceType`) the page no longer shows. They are still read
 * exactly as before (src/pictures.ts `followedModePicture`): `layout` for every
 * mode without a Layout of its own, `sourceType` for the key's own mode
 * without an Image of its own. So the page shows the truth, the key's own mode
 * loads `sourceType` into its Image list, and a change to that mode's Image
 * then takes the key-wide value's place (it is set to Automatic): choosing
 * Automatic there afterwards means the mode's own type.
 * Which mode the key-wide value was loaded for is kept (`modeTypeKeyWide`):
 * changing *Switch to mode* loads the lists again and is never taken for an
 * edit of the Image (it neither writes the old mode an image nor erases the
 * key-wide one).
 */
export function editModeSettings<T extends Record<string, unknown>>(settings: T, firstMode?: string): T | undefined {
	let s: Record<string, unknown> = settings;
	let changed = false;
	const filter = str(s.modeFilter);
	if (filter !== str(s.modeFilterWas)) {
		const fresh = str(s.modeFilterWas) === "";
		s = fresh
			? { ...s, modeFilterWas: filter }
			: { ...s, modeFilterWas: filter, perMode: {}, mode: "", longMode: "", modeFor: "", modeForWas: "" };
		changed = true;
	}
	let forId = str(s.modeFor);
	if (!forId) {
		const fallback = str(s.mode) || (firstMode ?? "");
		if (fallback) {
			s = { ...s, modeFor: fallback };
			forId = fallback;
			changed = true;
		}
	}
	const map: Record<string, PerMode> = { ...perModeOf(s) };
	// (The key-wide Mode type of a key saved up to beta179 stands for the key's own mode while that mode has
	// no image of its own. `modeTypeKeyWide` names the mode it was loaded into the Image list for, so a later
	// change of *Switch to mode* is never taken for an edit of the Image: the lists are loaded again instead.)
	const keyWideNow = forId !== "" && forId === str(s.mode) && !isAuto(s.sourceType) ? forId : "";
	if (forId !== str(s.modeForWas) || keyWideNow !== str(s.modeTypeKeyWide)) {
		const mine = map[forId] ?? {};
		const keyWideType = keyWideNow ? (s.sourceType as string) : undefined;
		s = {
			...s,
			modeForWas: forId,
			modeTypeKeyWide: keyWideNow,
			modeUse: mine.skip ? "skip" : "use",
			modeType: mine.type ?? keyWideType ?? "auto",
			modeLayout: mine.layout ?? "auto",
		};
		return s as T;
	}
	const keyWide = str(s.modeTypeKeyWide) === forId && forId !== "" && !isAuto(s.sourceType) ? (s.sourceType as string) : undefined;
	if (forId) {
		const now: PerMode = {};
		if (!isAuto(s.modeType)) now.type = s.modeType as string;
		if (!isAuto(s.modeLayout)) now.layout = s.modeLayout as string;
		if (s.modeUse === "skip") now.skip = true;
		const before = map[forId] ?? {};
		const typeBefore = before.type ?? keyWide;
		if (typeBefore !== now.type || before.layout !== now.layout || (before.skip === true) !== (now.skip === true)) {
			if (now.type === undefined && now.layout === undefined && !now.skip) delete map[forId];
			else map[forId] = now;
			s = { ...s, perMode: map };
			// (Edited, the mode's Image is its own: the key-wide one no longer stands behind it.)
			if (keyWide !== undefined && typeBefore !== now.type) {
				s.sourceType = "auto";
				s.modeTypeKeyWide = "";
			}
			changed = true;
		}
	}
	return changed ? (s as T) : undefined;
}
