// Source types: what a source is (a game, a window, a camera ...), chosen in a
// key's settings so its picture fits whatever the source is called. The
// owner: "instead of assigning a specific icon to my specific sources, ad an
// option in the key to choose a source type ... that way anyone with any
// source can pick it accordingly". No source or scene names are in here.
//
// The ids are kept in keys' settings (`sourceType`) and are append-only; the
// list is in src/key-types.json. The first twenty are the same as the Scene
// Modes filter's *Mode type* in OBS, so a Scene Mode key on Automatic takes
// the mode's type.

import { SOURCE_TYPES, sourceTypePicture, typeOfName } from "./key-types.ts";

export { SOURCE_TYPES, typeOfName };

/** The first twenty types (beta155): each has a drawing inside a capture's corners too. */
const HOOKED = new Set([
	"game", "window", "art_app", "code_editor", "browser", "camera", "capture_card", "display", "phone", "tablet",
	"chat", "music", "alerts", "title", "image", "video", "starting", "brb", "intermission", "ending",
]);

export function isSourceType(id: unknown): id is string {
	return typeof id === "string" && SOURCE_TYPES.some((t) => t.id === id);
}

/** A source type's own picture, the one for a key with no art of that type for its function. */
export function typePictureId(id: string): string | undefined {
	return sourceTypePicture(id);
}

/**
 * A Capture Active Window key's picture for a source type (chosen before
 * beta156, still shown): the type inside a capture's corners.
 */
export function hookTypePictureId(id: string): string | undefined {
	return HOOKED.has(id) ? `options/hook-type-${id.replace(/_/g, "-")}` : undefined;
}

/** What a source is, by its kind in OBS. Undefined for a kind that says nothing of the sort. */
const BY_INPUT: Record<string, string> = {
	game_capture: "game",
	window_capture: "window",
	xcomposite_input: "window",
	monitor_capture: "display",
	display_capture: "display",
	screen_capture: "display",
	xshm_input: "display",
	pipewire_screen_capture_source: "display",
	dshow_input: "camera",
	av_capture_input: "camera",
	av_capture_input_v2: "camera",
	"macos-avcapture": "camera",
	v4l2_input: "camera",
	browser_source: "browser",
	text_gdiplus: "title",
	text_gdiplus_v2: "title",
	text_gdiplus_v3: "title",
	text_ft2_source: "title",
	text_ft2_source_v2: "title",
	image_source: "image",
	slideshow: "image",
	slideshow_v2: "image",
	ffmpeg_source: "video",
	vlc_source: "video",
};

export function typeOfInput(inputKind: string | undefined): string | undefined {
	return BY_INPUT[inputKind ?? ""];
}

