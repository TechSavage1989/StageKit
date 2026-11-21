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
	stagekit_port_3d_effect_filter: "3D Effect",
	stagekit_port_advanced_masks_filter: "Advanced Mask",
	stagekit_port_obs_composite_blur: "Composite Blur",
	stagekit_port_obs_retro_effects_filter: "Retro Effects",
	stagekit_port_obs_noise_displace_filter: "Noise Displacement",
	stagekit_port_pixel_art_plugin: "Pixel Art",
	stagekit_port_freeze_filter: "Freeze",
	stagekit_port_dynamic_delay_filter: "Dynamic Delay",
	stagekit_port_recursion_effect_filter: "Recursion Effect",
	stagekit_port_time_warp_scan_filter: "Time Warp Scan",
	stagekit_port_dir_watch_media: "Directory Watch Media",
	stagekit_port_source_defaults_video: "Source Defaults",
	stagekit_port_source_defaults_audio: "Source Defaults",
	stagekit_port_move_action_filter: "Move Action",
	stagekit_port_move_audio_action_filter: "Move Action",
	stagekit_port_move_value_filter: "Move Value",
	stagekit_port_move_audio_value_filter: "Move Value",
	stagekit_port_move_source_filter: "Move Source",
	stagekit_port_move_source_swap_filter: "Move Source Swap",
	stagekit_port_move_directshow_filter: "Move Video Capture Device",
	stagekit_port_move_transition_override_filter: "Move Transition Override",
	stagekit_port_audio_move_filter: "Audio Move",
	stagekit_port_nv_move_filter: "NVIDIA Move",
	stagekit_port_shader_filter: "User-defined Shader",
	stagekit_port_ndi_filter: "Dedicated NDI Output",
	stagekit_port_ndi_audiofilter: "Dedicated NDI Audio Output",
	stagekit_effect_file: "Custom Effect File",
	stagekit_drop_shadow: "Dropshadow",
	stagekit_edge_detection: "Edge Detection",
	stagekit_gaussian_blur: "Gaussian Blur",
	stagekit_toon: "Toon",
	stagekit_fix_alpha: "Fix Alpha Blending",
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
	"stagekit_port_move_source_filter",
	"stagekit_port_move_source_swap_filter",
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

const manualSteps = [
	{ value: "toggle", label: "Toggle manual / automatic" },
	{ value: "next", label: "Next manual window or source" },
	{ value: "back", label: "Return to automatic" },
	{ value: "hide", label: "Hide or show the capture" },
	{ value: "clear", label: "Clear picked windows" },
] as const;

/** The hotkeys each filter registers on its parent source or scene. */
export function switcherSteps(kind: string | undefined): Item[] {
	if (kind === "stagekit_app_capture") return [...manualSteps];
	if (kind === "stagekit_active_app") return manualSteps.filter(({ value }) => value === "toggle" || value === "next" || value === "back");
	if (kind === "stagekit_auto_capture") return manualSteps.filter(({ value }) => value === "hide");
	return [];
}

export function switcherStep(kind: string | undefined, saved: string | undefined): string | undefined {
	const steps = switcherSteps(kind);
	const step = saved || (kind === "stagekit_auto_capture" ? "hide" : "next");
	return steps.some(({ value }) => value === step) ? step : undefined;
}

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
		.map((f) => ({ label: filterLabel(f), value: f.name }));
}

/** What separates a source from its filter in a key's one "target" setting. */
export const TARGET_SEP = "\u001f";

/**
 * Every filter of the wanted sort as one list to choose from: always
 * "<source> — <filter>", the filter as it is named in OBS's Filters window,
 * and what kind it is where its name does not say. The owner: "The stream
 * deck keys don't really say what filters they are associated with which
 * makes it difficult for me to find the associated filters". (Up to
 * beta175 a source with one such filter was listed by its name alone.) The
 * value is unchanged, so saved keys keep working.
 */
export function targetItems(filters: FilterInfo[], wanted: (kind: string) => boolean): Item[] {
	return filters
		.filter((f) => wanted(f.kind))
		.map((f) => ({ label: `${f.source} \u2014 ${filterLabel(f)}`, value: `${f.source}${TARGET_SEP}${f.name}` }))
		.sort((a, b) => a.label.localeCompare(b.label, undefined, { sensitivity: "base" }));
}

/**
 * A filter's name, and its kind in brackets where the name is not its
 * kind's own (`isOwnName`: the name OBS gives it now, or one from before).
 */
export function filterLabel(f: { name: string; kind: string }): string {
	return isOwnName(f.name, f.kind) ? f.name : `${f.name} (${kindName(f.kind)})`;
}

/** The source and filter a key acts on: its one "target", or the two older settings. */
export function targetOf(settings: { target?: string; source?: string; filter?: string }): { source: string; filter: string } {
	const at = settings.target ? settings.target.indexOf(TARGET_SEP) : -1;
	if (settings.target && at > 0) return { source: settings.target.slice(0, at), filter: settings.target.slice(at + 1) };
	return { source: settings.source ?? "", filter: settings.filter ?? "" };
}

/**
 * What a switch key does when pressed. Saved in the key's settings
 * (append-only). The three "cover" ones are the Standby Image key's: its
 * image shown over the source on purpose, or taken away ("StandbyCover"),
 * not the filter switched on or off.
 */
export type Press = "toggle" | "on" | "off" | "cover" | "coveron" | "coveroff";

export function afterPress(press: Press | undefined, enabled: boolean): boolean {
	if (press === "on") return true;
	if (press === "off") return false;
	return !enabled;
}

/** A Standby Image key's cover press as the request's "how", or undefined for a press that switches the filter. */
export function coverHow(press: string | undefined): "toggle" | "on" | "off" | undefined {
	return press === "cover" ? "toggle" : press === "coveron" ? "on" : press === "coveroff" ? "off" : undefined;
}

/**
 * How a Show in Scenes key's press hides its sources (`hideAs`,
 * append-only): as the filter's "Hide shows Standby Image" says (unset
 * reads the same), by hiding them, or by covering each with its Standby
 * Image. Undefined: the key switches the filter as any key does.
 */
export function hideAsOf(hideAs: unknown): "hide" | "cover" | undefined {
	return hideAs === "hide" || hideAs === "cover" ? hideAs : undefined;
}
