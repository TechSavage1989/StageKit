// What StageKit's filters are, as far as a key is concerned.
//
// A switch stays on or off, and its key shows which. A move runs once when it
// is switched on and switches itself off again, so its key has one look.

/** Filter kinds that run once and switch themselves off. */
const MOVES = new Set(["stagekit_send_to", "stagekit_place_item", "stagekit_layout", "stagekit_picture_shape"]);

/** What each kind is called in OBS's own list of filters. */
const NAMES: Record<string, string> = {
	stagekit_active_app: "Follow Active App",
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
	stagekit_standby: "Standby Picture",
	stagekit_visibility_link: "Link Visibility",
};

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
			const label = f.name === kind || f.name.includes(kind) ? f.name : `${f.name} (${kind})`;
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
			const plain = (count.get(f.source) ?? 0) === 1 && (f.name === kindName(f.kind) || f.name.includes(kindName(f.kind)));
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
