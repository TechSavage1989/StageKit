// What StageKit's filters are, as far as a key is concerned.
//
// A switch stays on or off, and its key shows which. A move runs once when it
// is switched on and switches itself off again, so its key has one look.

/** Filter kinds that run once and switch themselves off. */
const MOVES = new Set(["stagekit_send_to", "stagekit_place_item", "stagekit_layout", "stagekit_picture_shape"]);

/**
 * What each kind is called, the name inside OBS's "Scene Filter: <name>
 * (StageKit)" or "Source Filter: <name> (StageKit)" (see `shownName`).
 */
const NAMES: Record<string, string> = {
	stagekit_active_app: "Automatic Source Switcher",
	stagekit_app_capture: "Automatic Window Switcher",
	stagekit_auto_capture: "Auto Capture",
	stagekit_auto_frame: "Auto Frame",
	stagekit_corners: "Corners & Edges",
	stagekit_fit_item: "Fit to Space",
	stagekit_layout: "Apply Layout",
	stagekit_match_item: "Match Item",
	stagekit_picture_shape: "Aspect Ratio Preset",
	stagekit_place_item: "Move Scene Item",
	stagekit_program_trigger: "Program Trigger",
	stagekit_ramp_blur: "Blur",
	stagekit_ramp_zoom: "Zoom",
	stagekit_send_to: "Move Source",
	stagekit_shadow_backdrop: "Shadow, Glow & Outline",
	stagekit_show_hide: "Show/Hide Animation",
	stagekit_show_in_scenes: "Show in Scenes",
	stagekit_standby: "Standby Image",
	stagekit_visibility_link: "Link Visibility",
	stagekit_scene_modes: "Scene Modes",
	stagekit_scene_transitions: "Scene Transitions",
	stagekit_scene_fit: "Scene Fit",
	stagekit_scene_standby: "Scene Standby",
	stagekit_scene_corners: "Scene Corners",
	stagekit_scene_shadow: "Scene Shadow",
	stagekit_scene_blur: "Scene Blur",
};

/**
 * What each kind was called before (AGENTS.md, *Names*). A filter keeps the
 * name it was given when it was added, so a filter added under an old name
 * still carries it and is still that kind's own name.
 */
const OLD_NAMES: Record<string, string[]> = {
	stagekit_active_app: ["Show Source in Use", "Follow Active App", "Active App"],
	stagekit_app_capture: ["Capture App in Use"],
	stagekit_fit_item: ["Fit Item"],
	stagekit_layout: ["Layout"],
	stagekit_picture_shape: ["Picture Shape key", "Picture Shape"],
	stagekit_place_item: ["Place Item"],
	stagekit_ramp_blur: ["Ramp Blur"],
	stagekit_ramp_zoom: ["Ramp Zoom"],
	stagekit_send_to: ["Send To"],
	stagekit_shadow_backdrop: ["Shadow & Backdrop"],
	stagekit_standby: ["Standby Picture"],
	stagekit_visibility_link: ["Visibility Link"],
};

/**
 * Filter kinds that only go on a scene: a key for one of them has no source
 * to picture, so its Source type is not offered or used, and no input kind
 * is looked for.
 */
const SCENE_KINDS = new Set([
	"stagekit_fit_item",
	"stagekit_active_app",
	"stagekit_visibility_link",
	"stagekit_show_hide",
	"stagekit_layout",
	"stagekit_place_item",
	"stagekit_match_item",
	"stagekit_scene_modes",
	"stagekit_scene_transitions",
	"stagekit_scene_fit",
	"stagekit_scene_standby",
	"stagekit_scene_corners",
	"stagekit_scene_shadow",
	"stagekit_scene_blur",
]);

export function isSceneKind(kind: string): boolean {
	return SCENE_KINDS.has(kind);
}

export type FilterInfo = {
	source: string;
	name: string;
	kind: string;
	enabled: boolean;
};

export function isStageKit(kind: string): boolean {
	return kind.startsWith("stagekit_");
}

export function isMove(kind: string): boolean {
	return MOVES.has(kind);
}

export function isSwitch(kind: string): boolean {
	return isStageKit(kind) && !isMove(kind);
}

export function kindName(kind: string): string {
	return NAMES[kind] ?? kind;
}

/** What OBS's list of filters shows for a kind: "Scene Filter: Fit to Space (StageKit)". */
export function shownName(kind: string): string {
	const name = NAMES[kind];
	if (!name) return kind;
	return `${SCENE_KINDS.has(kind) ? "Scene Filter" : "Source Filter"}: ${name} (StageKit)`;
}

/**
 * Whether a filter's name is its kind's own: the name OBS gives it now, one
 * that has the kind's name in it, or exactly a name it was given under an
 * earlier name ("Fit Item (StageKit)"). Old names are matched whole, never
 * as a part: some are plain words ("Layout") a user's own name may contain.
 */
export function isOwnName(name: string, kind: string): boolean {
	const now = kindName(kind);
	if (name === shownName(kind) || name === now || name.includes(now)) return true;
	return (OLD_NAMES[kind] ?? []).some((n) => name === n || name === `${n} (StageKit)`);
}

export type Item = { label: string; value: string };

/** The sources that have at least one filter of the wanted sort, by name. */
export function sourceItems(filters: FilterInfo[], wanted: (kind: string) => boolean): Item[] {
	const names = [...new Set(filters.filter((f) => wanted(f.kind)).map((f) => f.source))];
	names.sort((a, b) => a.localeCompare(b, undefined, { sensitivity: "base" }));
	return names.map((name) => ({ label: name, value: name }));
}

/**
 * One source's filters of the wanted sort, in OBS's order. A filter still
 * carrying its kind's own name is listed by that name alone; a renamed one
 * also says what it is.
 */
export function filterItems(filters: FilterInfo[], source: string, wanted: (kind: string) => boolean): Item[] {
	return filters
		.filter((f) => f.source === source && wanted(f.kind))
		.map((f) => {
			const kind = kindName(f.kind);
			const label = isOwnName(f.name, f.kind) ? f.name : `${f.name} (${kind})`;
			return { label, value: f.name };
		});
}

/** What separates a source from its filter in a key's one "target" setting. */
export const TARGET_SEP = "\u001f";

/**
 * Every filter of the wanted sort as one list to choose from. A source with
 * one such filter is listed by its own name; where a source has several, or
 * the filter was renamed, the filter's name is said too.
 */
export function targetItems(filters: FilterInfo[], wanted: (kind: string) => boolean): Item[] {
	const mine = filters.filter((f) => wanted(f.kind));
	const count = new Map<string, number>();
	for (const f of mine) count.set(f.source, (count.get(f.source) ?? 0) + 1);
	return mine
		.map((f) => {
			const plain = (count.get(f.source) ?? 0) === 1 && isOwnName(f.name, f.kind);
			return { label: plain ? f.source : `${f.source} \u2014 ${f.name}`, value: `${f.source}${TARGET_SEP}${f.name}` };
		})
		.sort((a, b) => a.label.localeCompare(b.label, undefined, { sensitivity: "base" }));
}

/** The source and filter a key acts on: its one "target", or the two older settings. */
export function targetOf(settings: { target?: string; source?: string; filter?: string }): { source: string; filter: string } {
	const at = settings.target ? settings.target.indexOf(TARGET_SEP) : -1;
	if (settings.target && at > 0) return { source: settings.target.slice(0, at), filter: settings.target.slice(at + 1) };
	return { source: settings.source ?? "", filter: settings.filter ?? "" };
}

/** What a switch key does when pressed. Saved in the key's settings. */
export type Press = "toggle" | "on" | "off";

export function afterPress(press: Press | undefined, enabled: boolean): boolean {
	if (press === "on") return true;
	if (press === "off") return false;
	return !enabled;
}
