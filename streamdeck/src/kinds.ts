// What StageKit's filters are, as far as a key is concerned.
//
// A switch stays on or off, and its key shows which. A move runs once when it
// is switched on and switches itself off again, so its key has one look.

/** Filter kinds that run once and switch themselves off. */
const MOVES = new Set(["stagekit_send_to", "stagekit_place_item", "stagekit_layout", "stagekit_picture_shape"]);

/** What each kind is called in OBS's own list of filters. */
const NAMES: Record<string, string> = {
	stagekit_active_app: "Active App",
	stagekit_auto_capture: "Auto Capture",
	stagekit_auto_frame: "Auto Frame",
	stagekit_corners: "Corners & Edges",
	stagekit_fit_item: "Fit Item",
	stagekit_layout: "Layout",
	stagekit_match_item: "Match Item",
	stagekit_picture_shape: "Picture Shape key",
	stagekit_place_item: "Place Item",
	stagekit_program_trigger: "Program Trigger",
	stagekit_ramp_blur: "Ramp Blur",
	stagekit_ramp_zoom: "Ramp Zoom",
	stagekit_send_to: "Send To",
	stagekit_shadow_backdrop: "Shadow, Glow & Outline",
	stagekit_show_hide: "Show/Hide Animation",
	stagekit_show_in_scenes: "Show in Scenes",
	stagekit_standby: "Standby Picture",
	stagekit_visibility_link: "Visibility Link",
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

/** What a switch key does when pressed. Saved in the key's settings. */
export type Press = "toggle" | "on" | "off";

export function afterPress(press: Press | undefined, enabled: boolean): boolean {
	if (press === "on") return true;
	if (press === "off") return false;
	return !enabled;
}
