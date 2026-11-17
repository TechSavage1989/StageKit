// What a Scene Blur key points at, and the lists it chooses from. No Stream
// Deck code here: the tests use it as it is.
//
// A Scene Blur is a filter on a scene; scenes can have one of the same name
// each, and one key can switch them all ("all scenes with the blur filter of
// the same name", the owner) or the one on a scene.

/** One name of Scene Blur, as ListSceneBlurs gives it: the look of its blur, and each scene it is on. */
export type SceneBlurFilter = {
	filter: string;
	type: string;
	active: boolean;
	scenes: Array<{ scene: string; active: boolean }>;
};

/** The Scene setting for every scene that has a Scene Blur of the name chosen. */
export const ALL_SCENES = "";

/** ListSceneBlurs' answer, made safe to use. */
export function readSceneBlurs(answer: Record<string, unknown> | undefined): SceneBlurFilter[] {
	const filters = answer?.filters;
	if (!Array.isArray(filters)) return [];
	return filters
		.map((f: Record<string, unknown>) => ({
			filter: String(f?.filter ?? ""),
			type: String(f?.type ?? ""),
			active: f?.active === true,
			scenes: Array.isArray(f?.scenes)
				? (f.scenes as Array<Record<string, unknown>>).map((s) => ({ scene: String(s?.scene ?? ""), active: s?.active === true })).filter((s) => s.scene !== "")
				: [],
		}))
		.filter((f) => f.filter !== "");
}

/** The Blur filter list: each name once, with how many scenes have it. */
export function blurFilterItems(filters: SceneBlurFilter[]): Array<{ label: string; value: string }> {
	return [...filters]
		.sort((a, b) => a.filter.localeCompare(b.filter, undefined, { sensitivity: "base" }))
		.map((f) => ({ label: f.scenes.length > 1 ? `${f.filter} (${f.scenes.length} scenes)` : `${f.filter} (${f.scenes[0]?.scene ?? "no scene"})`, value: f.filter }));
}

/** The Scene list for a name: every scene with it first, then each scene that has one. */
export function blurSceneItems(filters: SceneBlurFilter[], filter: string | undefined): Array<{ label: string; value: string }> {
	const found = filters.find((f) => f.filter === filter);
	const items = [{ label: "All scenes with this filter", value: ALL_SCENES }];
	for (const s of found?.scenes ?? []) items.push({ label: s.scene, value: s.scene });
	return items;
}

/**
 * Whether the key's blur is on: the one on its scene, or (all scenes) any of
 * them, which is what a toggle goes by. Undefined when OBS has none such.
 */
export function sceneBlurOn(filters: SceneBlurFilter[], filter: string | undefined, scene: string | undefined): boolean | undefined {
	const found = filters.find((f) => f.filter === filter);
	if (!found) return undefined;
	if (!scene) return found.scenes.length > 0 ? found.scenes.some((s) => s.active) : undefined;
	const one = found.scenes.find((s) => s.scene === scene);
	return one ? one.active : undefined;
}

/** What a key keeps its filter and scene as, to tell when they change. */
export function sceneBlurTarget(filter: string | undefined, scene: string | undefined): string {
	return `${filter ?? ""}\u001f${scene ?? ""}`;
}
