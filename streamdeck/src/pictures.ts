// Which picture a key shows. The pictures are the StageKit icon pack, kept in
// imgs/pack/<colour>/<group>/<name>.png and listed in pack.json. Left to
// itself a key takes the picture that fits what its filter is and how it is
// set; a key can also be given any picture of the pack.

import pack from "./pack.json" with { type: "json" };

export type Picture = { id: string; group: string; label: string; on: string; off: string };

export const COLOURS: string[] = pack.colours;
export const DEFAULT_COLOUR = "mono";
export const PICTURES: Picture[] = pack.pictures;

const BY_ID = new Map(PICTURES.map((p) => [p.id, p]));

const BLUR = ["gaussian", "box", "kawase", "pixelate", "zoom", "motion", "hexagons"];
const CORNERS = ["round", "cut", "notch", "scoop"];
const PLACES = [
	"top-left",
	"top-centre",
	"top-right",
	"middle-left",
	"centre",
	"middle-right",
	"bottom-left",
	"bottom-centre",
	"bottom-right",
];
const PLAIN: Record<string, string> = {
	stagekit_auto_capture: "filters/auto-capture",
	stagekit_auto_frame: "filters/auto-frame",
	stagekit_show_in_scenes: "filters/show-in-scenes",
	stagekit_standby: "filters/standby-picture",
};

type Settings = Record<string, unknown>;

/** A whole number setting; OBS leaves a setting out while it has its usual value. */
function whole(settings: Settings, name: string, usual: number): number {
	const value = settings[name];
	return typeof value === "number" && Number.isFinite(value) ? Math.trunc(value) : usual;
}

/**
 * The picture that fits a filter: by its kind and, where the pack has one
 * for each choice, by how the filter is set. Undefined where the pack has
 * nothing for that kind.
 */
export function automatic(kind: string, settings: Settings = {}): Picture | undefined {
	let id = PLAIN[kind];
	if (kind === "stagekit_ramp_blur") {
		id = `blur-types/${BLUR[whole(settings, "blur_type", 0)] ?? BLUR[0]}`;
	} else if (kind === "stagekit_corners") {
		id = `corners/${CORNERS[whole(settings, "style", 0)] ?? CORNERS[0]}`;
	} else if (kind === "stagekit_shadow_backdrop") {
		const shadow = settings.shadow !== false;
		const look =
			shadow && whole(settings, "shadow_type", 0) === 1
				? "glow"
				: !shadow && settings.outline === true
					? "outline"
					: !shadow && settings.backdrop === true
						? "panel"
						: "soft";
		id = `shadow-glow-outline/${look}`;
	} else if (kind === "stagekit_send_to" || kind === "stagekit_place_item") {
		// One of the nine places on the screen (top right is where a new one
		// starts); a saved spot has no picture of its own.
		const place = PLACES[whole(settings, "destination", 2)];
		id = place ? `camera/camera-${place}` : "";
	}
	return id ? BY_ID.get(id) : undefined;
}

/** The picture a key was given by name, or the one that fits its filter. */
export function choose(chosen: string | undefined, kind: string, settings: Settings = {}): Picture | undefined {
	return (chosen && BY_ID.get(chosen)) || automatic(kind, settings);
}

/** Where a picture's file is, inside the plugin's folder. */
export function file(picture: Picture, on: boolean, colour: string | undefined): string {
	const use = colour && COLOURS.includes(colour) ? colour : DEFAULT_COLOUR;
	return `imgs/pack/${use}/${on ? picture.on : picture.off}.png`;
}

export type ListItem = { label: string; value: string } | { label: string; children: Array<{ label: string; value: string }> };

/** The pack as a list to choose from: "Automatic" first, then group by group. */
export function pictureItems(): ListItem[] {
	const groups = new Map<string, Array<{ label: string; value: string }>>();
	for (const picture of PICTURES) {
		if (!groups.has(picture.group)) groups.set(picture.group, []);
		groups.get(picture.group)!.push({ label: picture.label, value: picture.id });
	}
	const out: ListItem[] = [{ label: "Automatic (fits the filter)", value: "" }];
	for (const [group, children] of groups) out.push({ label: group, children });
	return out;
}
