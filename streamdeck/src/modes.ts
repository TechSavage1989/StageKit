// What a Scene Mode key points at, and the list it chooses from. No Stream
// Deck code here: the tests use it as it is.

import { TARGET_SEP } from "./kinds.ts";

export type ModeFilter = { scene: string; filter: string; modes: Array<{ id: string; name: string }> };

/** A Scene Mode key's one setting: the scene, the Scene Modes filter on it and the mode's id. */
export function modeTarget(target: string | undefined): { scene: string; filter: string; mode: string } {
	const parts = (target ?? "").split(TARGET_SEP);
	if (parts.length < 3) return { scene: "", filter: "", mode: "" };
	return { scene: parts[0], filter: parts[1], mode: parts.slice(2).join(TARGET_SEP) };
}

/**
 * Every mode of every Scene Modes filter as one list: "Standby: Intermission",
 * "Chat Live: GBA"; the filter's name too where a scene has two such filters.
 * In OBS's order of each filter's modes.
 */
export function modeItems(filters: ModeFilter[]): Array<{ label: string; value: string }> {
	const count = new Map<string, number>();
	for (const f of filters) count.set(f.scene, (count.get(f.scene) ?? 0) + 1);
	const out: Array<{ label: string; value: string }> = [];
	const sorted = [...filters].sort((a, b) => a.scene.localeCompare(b.scene, undefined, { sensitivity: "base" }));
	for (const f of sorted) {
		const where = (count.get(f.scene) ?? 0) > 1 ? `${f.scene} — ${f.filter}` : f.scene;
		for (const mode of f.modes)
			out.push({ label: `${where}: ${mode.name || mode.id}`, value: [f.scene, f.filter, mode.id].join(TARGET_SEP) });
	}
	return out;
}
