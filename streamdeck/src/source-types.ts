// Source types: what a source is (a game, a window, a camera ...), chosen in a
// key's settings so its picture fits whatever the source is called. The
// owner: "instead of assigning a specific icon to my specific sources, ad an
// option in the key to choose a source type ... that way anyone with any
// source can pick it accordingly". No source or scene names are in here.
//
// The ids are kept in keys' settings (`sourceType`) and are append-only. They
// are the same as the Scene Modes filter's *Mode type* in OBS, so a Scene Mode
// key on Automatic takes the mode's type.

export type SourceType = { id: string; label: string; words: string[][] };

/** In the order the settings page lists them, after *Automatic*. */
export const SOURCE_TYPES: SourceType[] = [
	{ id: "game", label: "Game", words: [["game"], ["games"], ["gaming"], ["gba"], ["console"], ["emulator"]] },
	{ id: "window", label: "Window", words: [["window"], ["windows"]] },
	{ id: "art_app", label: "Art app", words: [["art"], ["paint"], ["painting"], ["drawing"], ["sketch"]] },
	{ id: "code_editor", label: "Code editor", words: [["code"], ["coding"], ["programming"], ["editor"], ["ide"]] },
	{ id: "browser", label: "Browser", words: [["browser"], ["web"]] },
	{ id: "camera", label: "Camera", words: [["camera"], ["cam"], ["webcam"], ["dslr"], ["facecam"]] },
	{ id: "capture_card", label: "Capture card", words: [["capture", "card"], ["card"]] },
	{ id: "display", label: "Display", words: [["display"], ["screen"], ["monitor"], ["desktop"]] },
	{ id: "phone", label: "Phone", words: [["phone"], ["mobile"]] },
	{ id: "tablet", label: "Tablet", words: [["tablet"]] },
	{ id: "chat", label: "Chat", words: [["chat"]] },
	{ id: "music", label: "Music", words: [["music"], ["song"], ["now", "playing"]] },
	{ id: "alerts", label: "Alerts", words: [["alert"], ["alerts"]] },
	{ id: "title", label: "Title / text", words: [["title"], ["titles"], ["caption"]] },
	{ id: "image", label: "Image", words: [["image"], ["picture"], ["photo"]] },
	{ id: "video", label: "Video", words: [["video"], ["clip"], ["replay"]] },
	{ id: "starting", label: "Starting", words: [["starting"], ["start"], ["soon"]] },
	{ id: "brb", label: "Be right back", words: [["brb"], ["be", "right", "back"]] },
	{ id: "intermission", label: "Intermission", words: [["intermission"], ["break"]] },
	{ id: "ending", label: "Ending", words: [["ending"], ["end"], ["outro"]] },
];

const BY_ID = new Map(SOURCE_TYPES.map((t) => [t.id, t]));

export function isSourceType(id: unknown): id is string {
	return typeof id === "string" && BY_ID.has(id);
}

/** The picture of a source type (tools/make-option-icons.py draws them). */
export function typePictureId(id: string): string | undefined {
	if (!BY_ID.has(id)) return undefined;
	const reuses: Record<string, string> = {
		starting: "options/mode-starting",
		brb: "options/mode-brb",
		intermission: "options/mode-intermission",
		ending: "options/mode-ending",
	};
	return reuses[id] ?? `options/type-${id.replace(/_/g, "-")}`;
}

/** A Capture Active Window key's picture for a source type: the type inside a capture's corners. */
export function hookTypePictureId(id: string): string | undefined {
	return BY_ID.has(id) ? `options/hook-type-${id.replace(/_/g, "-")}` : undefined;
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

/** The words of a name: lower case, letters and digits. */
function words(name: string): string[] {
	return name
		.toLowerCase()
		.split(/[^a-z0-9]+/)
		.filter((word) => word !== "");
}

/**
 * The source type a name speaks of ("Be Right Back", "GBA", "Main Display"):
 * the type with the longest of its phrases all in the name; the first in the
 * list of those as long. Undefined when none is.
 */
export function typeOfName(name: string | undefined): string | undefined {
	const have = words(name ?? "");
	if (have.length === 0) return undefined;
	let best: string | undefined;
	let bestLength = 0;
	for (const type of SOURCE_TYPES) {
		for (const phrase of type.words) {
			if (phrase.length > bestLength && phrase.every((word) => have.includes(word))) {
				best = type.id;
				bestLength = phrase.length;
			}
		}
	}
	return best;
}

/** The list a key's settings page offers: Automatic, then every type. */
export function sourceTypeItems(): Array<{ label: string; value: string }> {
	return [{ label: "Automatic", value: "" }, ...SOURCE_TYPES.map((t) => ({ label: t.label, value: t.id }))];
}
