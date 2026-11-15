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
	stagekit_layout: "Layout",
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
	snap_now: "Snap",
	same_width: "Same\nWidth",
	same_height: "Same\nHeight",
	same_size: "Same\nSize",
	same_everywhere: "Copy to\nAll",
	same_chosen: "Copy to\nChosen",
};

const OPTIONS: Record<string, string> = {
	snap: "Snap",
	guides: "Guides",
	limit: "Resize\nLimit",
	slide: "Slide to\nEdge",
	everyScene: "Every\nScene",
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

/** A key that switches a filter. */
export function switchTitle(kind: string | undefined, press: string | undefined): string {
	return pressed(KINDS[kind ?? ""] ?? "Switch", press);
}

/** A key that sends something to a place, or applies a layout. */
export function moveTitle(kind: string | undefined, settings: Record<string, unknown> = {}): string {
	if (kind === "stagekit_send_to" || kind === "stagekit_place_item") {
		const place = settings.destination;
		const at = typeof place === "number" ? PLACES[Math.trunc(place)] : PLACES[2];
		return at ?? "Saved\nSpot";
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

export function hookTitle(inputKind: string | undefined): string {
	return inputKind === "window_capture" ? "Capture\nWindow" : "Capture\nGame";
}

export function blurAllTitle(press: string | undefined): string {
	return pressed("Blur All", press);
}

export function shapeTitle(shape: string | undefined): string {
	return shape && shape !== "auto" ? shape : "Auto\nRatio";
}
