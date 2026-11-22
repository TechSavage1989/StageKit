// What a key is called on the deck. The key art carries no words (the owner
// asked for them out of the pictures), so each key sets its own Title from
// what it is for and how it is set. A title the user types in Stream Deck
// takes the place of this one; Stream Deck sees to that.

const KINDS: Record<string, string> = {
	stagekit_active_app: "Source\nSwitcher",
	stagekit_app_capture: "Window\nSwitcher",
	stagekit_auto_capture: "Auto\nCapture",
	stagekit_auto_frame: "Auto\nFrame",
	stagekit_corners: "Corners",
	stagekit_fit_item: "Fit to\nSpace",
	stagekit_layout: "Apply\nLayout",
	stagekit_match_item: "Match\nItem",
	stagekit_scene_transitions: "Scene\nTransitions",
	stagekit_scene_modes: "Scene\nModes",
	stagekit_picture_shape: "Aspect Ratio\nPreset",
	stagekit_place_item: "Move\nItem",
	stagekit_program_trigger: "Program\nTrigger",
	stagekit_ramp_blur: "Blur",
	stagekit_ramp_zoom: "Zoom",
	stagekit_send_to: "Move",
	stagekit_shadow_backdrop: "Shadow",
	stagekit_show_hide: "Show/Hide",
	stagekit_show_in_scenes: "Show in\nScenes",
	stagekit_standby: "Standby",
	stagekit_visibility_link: "Link",
	stagekit_scene_fit: "Scene\nFit",
	stagekit_scene_shadow: "Scene\nShadow",
	stagekit_scene_corners: "Scene\nCorners",
	stagekit_scene_standby: "Scene\nStandby",
	stagekit_scene_blur: "Scene\nBlur",
};

const PLACES = [
	"Top\nLeft",
	"Top\nCentre",
	"Top\nRight",
	"Middle\nLeft",
	"Centre",
	"Middle\nRight",
	"Bottom\nLeft",
	"Bottom\nCentre",
	"Bottom\nRight",
];

const TOOLS: Record<string, string> = {
	left: "Align\nLeft",
	center: "Align\nCentre",
	right: "Align\nRight",
	top: "Align\nTop",
	middle: "Align\nMiddle",
	bottom: "Align\nBottom",
	space_across: "Space\nAcross",
	space_down: "Space\nDown",
	snap_now: "Snap Now",
	same_width: "Same\nWidth",
	same_height: "Same\nHeight",
	same_size: "Same\nSize",
	same_everywhere: "Copy to\nAll",
	same_chosen: "Copy to\nChosen",
	fit_space: "Fit Now",
};

const OPTIONS: Record<string, string> = {
	snap: "Snap on\nRelease",
	guides: "Guides",
	limit: "Stop at\nEdges",
	slide: "Slide to\nEdge",
	everyScene: "Move in\nEvery Scene",
	keepShape: "Keep\nShape",
};

const WAYS: Record<string, string> = {
	left: "Left",
	right: "Right",
	up: "Up",
	down: "Down",
	"up-left": "Up Left",
	"up-right": "Up Right",
	"down-left": "Down Left",
	"down-right": "Down Right",
	centre: "Centre",
};

/** "On" or "Off" under a name, for a key that only switches one way. */
function pressed(name: string, press: string | undefined): string {
	return press === "on" ? `${name}\nOn` : press === "off" ? `${name}\nOff` : name;
}

/** A key that switches a filter (or, a Standby Image key, covers its source). */
export function switchTitle(kind: string | undefined, press: string | undefined): string {
	if (press === "cover") return "Cover /\nUncover";
	if (press === "coveron") return "Cover";
	if (press === "coveroff") return "Uncover";
	return pressed(KINDS[kind ?? ""] ?? "Switch", press);
}

/** A key that sends something to a place, or applies a layout. */
export function moveTitle(kind: string | undefined, settings: Record<string, unknown> = {}, position?: string): string {
	if (kind === "stagekit_send_to" || kind === "stagekit_place_item") {
		if (position === "spot") return "Saved\nSpot";
		if (position === "screen") return "Screen\nSpot";
		const chosen = PLACES.find((name) => name.toLowerCase().replace("\n", "-") === position);
		if (chosen) return chosen;
		const place = settings.destination;
		const at = typeof place === "number" ? PLACES[Math.trunc(place)] : PLACES[2];
		// (10 is a spot on the screen, the dock's slides' way; 9 the item's own saved place.)
		return at ?? (place === 10 ? "Screen\nSpot" : "Saved\nSpot");
	}
	return KINDS[kind ?? ""] ?? "Move";
}

export function toolTitle(tool: string | undefined): string {
	return TOOLS[tool ?? "left"] ?? "Tool";
}

export function optionTitle(option: string | undefined, press: string | undefined): string {
	return pressed(OPTIONS[option ?? "snap"] ?? "Option", press);
}

export function slideTitle(direction: string | undefined): string {
	return `Slide\n${WAYS[direction ?? "left"] ?? "Left"}`;
}

export function stepTitle(step: string | undefined): string {
	return step === "clear"
		? "Clear\nPicked"
		: step === "toggle"
		? "Manual /\nAuto"
		: step === "back"
			? "Automatic"
			: step === "hide"
				? "Hide /\nShow"
				: "Next\nManual";
}

export function overlapTitle(press: string | undefined): string {
	return pressed("Overlap\nIf Fills", press);
}

/** A name on a key: as it is, or over two lines of about ten letters if it is long. */
function fitted(text: string | undefined, none: string): string {
	const name = (text ?? "").trim();
	if (!name) return none;
	if (name.length <= 10) return name;
	const words = name.split(/[\s_-]+/).filter(Boolean);
	let first = "";
	while (words.length && (first + " " + words[0]).trim().length <= 10) first = (first + " " + words.shift()).trim();
	const rest = words.join(" ");
	if (!first) return name.slice(0, 10) + "\n" + name.slice(10, 20);
	return rest ? `${first}\n${rest.length > 10 ? rest.slice(0, 9) + "…" : rest}` : first;
}

/** A Switch Scene key: the scene's name, over two lines if it is long. */
export function sceneTitle(scene: string | undefined): string {
	return fitted(scene, "Scene");
}

/** A Scene Mode key: the mode's name, over two lines if it is long. */
export function modeTitle(mode: string | undefined): string {
	return fitted(mode, "Mode");
}

/** A Scene Mode key on Toggle or Next mode: the name of the mode that is on, or "No mode". */
export function followedModeTitle(on: { id: string; name?: string } | undefined): string {
	if (!on || !on.id) return "No mode";
	return fitted(on.name || on.id, "Mode");
}

/** A Capture Active Window key: its Capture type when one is chosen, else the kind of capture. */
export function hookTitle(inputKind: string | undefined, captureType?: string): string {
	if (captureType === "window" || captureType === "game") return captureType === "window" ? "Capture\nWindow" : "Capture\nGame";
	return inputKind === "window_capture" ? "Capture\nWindow" : "Capture\nGame";
}

export function blurAllTitle(press: string | undefined): string {
	return pressed("Blur All", press);
}

export function resetStatsTitle(): string {
	return "Reset\nStats";
}

/** A Scene Blur key: "Scene Blur", and "On" or "Off" under it for a key that only switches one way. */
export function sceneBlurTitle(press: string | undefined): string {
	return pressed(KINDS.stagekit_scene_blur, press);
}

export function shapeTitle(shape: string | undefined): string {
	return shape && shape !== "auto" ? shape : "Auto\nRatio";
}

const OUTPUT_NAMES: Record<string, Record<string, string>> = {
	stream: { toggle: "Stream", start: "Go Live", stop: "End\nStream" },
	record: { toggle: "Record", start: "Start\nRec", stop: "Stop\nRec", pause: "Pause /\nResume", pauseonly: "Pause\nRec", resume: "Resume\nRec" },
};

const OUTPUT_STATES: Record<string, string> = {
	starting: "Starting",
	stopping: "Stopping",
	reconnecting: "Reconnect",
	live: "LIVE",
	recording: "REC",
	paused: "Paused",
};

/** What a long press does, in a word or two, under "Hold:". */
const HOLD_WORDS: Record<string, string> = {
	toggle: "Toggle",
	start: "Start",
	stop: "Stop",
	pause: "Pause",
	pauseonly: "Pause",
	resume: "Resume",
};

/**
 * A Stream or Record key: what a press does, then what OBS is doing (the
 * time while live or recording, when `time` is given), then what a long
 * press does ("Hold: Stop"). A key has room for three short lines under its
 * picture: a two-line name gives way to its first line when all three are
 * there.
 */
export function outputTitle(output: "stream" | "record", press: unknown, state: string, time?: string, hold?: unknown): string {
	const names = OUTPUT_NAMES[output];
	let name = names[typeof press === "string" && names[press] ? press : "toggle"];
	const now = (state === "live" || state === "recording") && time ? time : OUTPUT_STATES[state];
	const held = typeof hold === "string" && HOLD_WORDS[hold] ? `Hold: ${HOLD_WORDS[hold]}` : undefined;
	const lines = [now, held].filter(Boolean) as string[];
	if (name.includes("\n") && lines.length > 0) name = name.replace("\n", " ");
	if (lines.length === 2 && name.length > 9) name = name.split(" ")[0];
	return [name, ...lines].join("\n");
}
