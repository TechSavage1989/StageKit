// StageKit's arrange tools, as its dock and OBS's hotkey list have them. Each
// has a hotkey in OBS named "stagekit.arrange.<tool>"; a Tool key presses it,
// so it acts on the items selected in OBS's preview, like the dock's buttons.

export const TOOLS: Array<{ value: string; label: string }> = [
	{ value: "left", label: "Line up: left edges" },
	{ value: "center", label: "Line up: centres, across" },
	{ value: "right", label: "Line up: right edges" },
	{ value: "top", label: "Line up: top edges" },
	{ value: "middle", label: "Line up: middles, down" },
	{ value: "bottom", label: "Line up: bottom edges" },
	{ value: "space_across", label: "Space out evenly, across" },
	{ value: "space_down", label: "Space out evenly, down" },
	{ value: "snap_now", label: "Snap to what is near" },
	{ value: "same_width", label: "Make the same width" },
	{ value: "same_height", label: "Make the same height" },
	{ value: "same_size", label: "Make the same size" },
	{ value: "same_everywhere", label: "Copy transform to all scenes" },
	{ value: "same_chosen", label: "Copy transform to chosen scenes" },
];

export function hotkeyOf(tool: string | undefined): string | undefined {
	return TOOLS.some((t) => t.value === tool) ? `stagekit.arrange.${tool}` : undefined;
}
