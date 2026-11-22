// What every key works with in OBS: the first row of its settings page,
// *Works with*. tools/make-plugin-files.py writes the row from this table
// (the one place it is said), and the plugin fills in what the key is set to
// when the page asks ("getWorksWith", src/actions/shared.ts). The owner
// (2026-11-18): "I want the stream deck keys to show the filter or feature
// they work with somewhere outside its [list]. somewhere in the configuration
// area at the top for all keys." No Stream Deck code here: the generator and
// the tests use it as it is.

import features from "./features.json" with { type: "json" };
import { isSceneKind, isStageKit, shownName, targetOf } from "./kinds.ts";
import { modeKeyFilter } from "./modes.ts";

/**
 * What a key is set to in OBS, by the settings that say it: a source (or a
 * scene) and its filter (`target`, or the older `source` and `filter`), a
 * capture (`source`), a scene (`scene`), a Scene Modes filter (`modeFilter`)
 * or a Scene Blur's name and its scene (`filter`, `scene`).
 */
export type Chosen = "target" | "capture" | "scene" | "modeFilter" | "sceneBlur";

export type WorksWith = {
	/** The kinds of StageKit filter the key works, in the order they are said. */
	kinds?: string[];
	/** For a key that works no one kind of filter: what it works, plainly. */
	feature?: string;
	/** A second line that never changes, for a key that is set to nothing in OBS. */
	note?: string;
	/** What the key is set to, said on the second line from its settings. */
	chosen?: Chosen;
	/** The row's tooltip. */
	tip: string;
};

/** The settings on the page each kind of choice is made with: the row is asked for again when one of them changes. */
export const FOLLOWS: Record<Chosen, string[]> = {
	target: ["target", "source", "filter"],
	capture: ["source"],
	scene: ["scene"],
	modeFilter: ["modeFilter"],
	sceneBlur: ["filter", "scene"],
};

/** The second line of a key that can be set to something in OBS and is not yet. */
export const NOT_CHOSEN = "(choose one below)";

/** Where in OBS a filter of these kinds is put: on a source, a scene, or either. */
export function whereOf(kinds: string[]): "source" | "scene" | "source or scene" {
	const scenes = kinds.filter(isSceneKind).length;
	return scenes === 0 ? "source" : scenes === kinds.length ? "scene" : "source or scene";
}

/** "A", "A or B", "A, B or C". */
function orList(names: string[]): string {
	return names.length < 2 ? (names[0] ?? "") : `${names.slice(0, -1).join(", ")} or ${names[names.length - 1]}`;
}

function filterTip(kinds: string[], more = ""): string {
	const where = whereOf(kinds);
	const kind =
		kinds.length > 1
			? "The kinds of StageKit filter this key works, as OBS's Filters window names them (once one is chosen, its own kind)"
			: "The kind of StageKit filter this key works, as OBS's Filters window names it";
	return `${kind}, and under it the one chosen below. In OBS: right-click the ${where} and choose Filters.${more ? ` ${more}` : ""}`;
}

/** The keys that are not one feature's (features.json), by their name in the manifest. */
const OTHERS: Record<string, WorksWith> = {
	timer: { feature: "Countdown Timer (StageKit) or the private StageKit Timer dock", tip: features.timer.tip },
	resetstats: {
		feature: "OBS Stats",
		note: "Same as Reset in the OBS Stats window.",
		tip: "Resets OBS Stats with one press, using OBS's own Reset command.",
	},
	obsfilter: { feature: "A filter on an OBS source or scene", chosen: "target", tip: "Switches any OBS source or scene filter on and off." },
	obssource: { feature: "A source in an OBS scene or group", tip: "Shows or hides a source in its scene or group." },
	obsmute: { feature: "An input in the OBS audio mixer", tip: "Mutes or unmutes an OBS audio input." },
	obsstudio: { feature: "OBS Studio Mode", tip: "Switches OBS Studio Mode on or off. One monitor means off; two monitors mean on." },
	studiotransition: {
		feature: "OBS Studio Mode",
		note: "Same as the Transition button in Studio Mode.",
		tip: "Sends the current Preview to Program using OBS's current transition and duration. Studio Mode must be on.",
	},
	obscamera: { feature: "The OBS virtual camera", tip: "Starts or stops the OBS virtual camera." },
	movesourceport: { kinds: ["stagekit_port_move_source_filter"], chosen: "target", tip: filterTip(["stagekit_port_move_source_filter"]) },
	moveswapport: { kinds: ["stagekit_port_move_source_swap_filter"], chosen: "target", tip: filterTip(["stagekit_port_move_source_swap_filter"]) },
	shape: {
		kinds: ["stagekit_auto_frame"],
		chosen: "target",
		tip: filterTip(["stagekit_auto_frame"], "The aspect ratio is set on that capture's Auto Frame."),
	},
	hook: {
		feature: "A Game Capture or Window Capture",
		chosen: "capture",
		tip:
			"The key aims the Game Capture or Window Capture chosen below at the active window or game, and switches an Auto Capture on it off. " +
			"With an Automatic Window Switcher on it, the window becomes that filter's manual window instead: shown at once or when switched to, as its Captured window says.",
	},
	appswitch: {
		kinds: ["stagekit_app_capture", "stagekit_active_app", "stagekit_auto_capture"],
		chosen: "target",
		tip: filterTip(["stagekit_app_capture", "stagekit_active_app", "stagekit_auto_capture"], "The key presses that filter's own hotkeys."),
	},
	fitoverlap: {
		kinds: ["stagekit_fit_item", "stagekit_scene_fit"],
		chosen: "target",
		tip: filterTip(["stagekit_fit_item", "stagekit_scene_fit"], "The key switches its Overlap only when it fills."),
	},
	scene: {
		feature: "OBS scenes",
		chosen: "scene",
		tip: "The key switches OBS's program scene to the one chosen below, or Studio Mode's preview when Switch is set to Preview. No StageKit filter is needed.",
	},
	mode: {
		kinds: ["stagekit_scene_modes"],
		chosen: "modeFilter",
		tip: filterTip(["stagekit_scene_modes"]),
	},
	stream: {
		feature: "OBS streaming",
		tip: "The key starts and stops OBS's stream, as OBS's own Start Streaming button does. No StageKit filter is needed.",
	},
	record: {
		feature: "OBS recording",
		tip: "The key starts, stops, pauses and resumes OBS's recording, as OBS's own recording buttons do. No StageKit filter is needed.",
	},
	tool: {
		feature: "StageKit dock: Arrange tools",
		note: "On the sources selected in OBS",
		tip: "The key presses the tool chosen below, as its button on the StageKit dock's Arrange or Copy tab does, on the sources selected in OBS's preview.",
	},
	blurall: {
		kinds: ["stagekit_ramp_blur"],
		note: "Every one with Include in 'Blur All' hotkeys ticked",
		tip: "The key switches every Blur filter with Include in 'Blur All' hotkeys ticked on, or all of them off again. In OBS: right-click a source and choose Filters.",
	},
	option: {
		feature: "StageKit dock: Options",
		tip: "The key sets the option chosen below, as its tick box on the StageKit dock's Arrange or Place tab does.",
	},
	slide: {
		feature: "StageKit dock: Slide to the next edge",
		note: "On the source selected in OBS",
		tip: "The key slides the source selected in OBS's preview to the next edge that way, as the StageKit dock's arrows do.",
	},
	switch: {
		feature: "Any StageKit filter but a move",
		chosen: "target",
		tip: "An older key: it switches any StageKit filter but a move (Move Source, Move Scene Item, Apply Layout, Aspect Ratio Preset) on and off. The key named after the feature does the same.",
	},
	move: {
		feature: "A StageKit move: Move Source, Move Scene Item, Apply Layout or Aspect Ratio Preset",
		chosen: "target",
		tip: "An older key: it runs any StageKit move. The key named after the feature does the same.",
	},
};

/** Every key, by its name in the manifest (the last part of its UUID): one entry each. */
export const WORKS_WITH: Record<string, WorksWith> = (() => {
	const all: Record<string, WorksWith> = {};
	for (const f of [...features.switches, ...features.moves]) all[f.key] = { kinds: [f.kind], chosen: "target", tip: filterTip([f.kind]) };
	for (const f of features.scenes) all[f.key] = { kinds: [f.kind], chosen: "sceneBlur", tip: filterTip([f.kind], "Its Scene list chooses one scene, or every scene with a filter of that name.") };
	return { ...all, ...OTHERS };
})();

/** A key's entry by its UUID or its name ("com.techsavage1989.stagekit.blur" or "blur"). */
export function worksWithOf(key: string): WorksWith | undefined {
	return WORKS_WITH[key.slice(key.lastIndexOf(".") + 1)];
}

/** The row's first line: the kinds of filter exactly as OBS's Filters window names them, or the feature. */
export function firstLine(works: WorksWith): string {
	return works.kinds ? orList(works.kinds.map(shownName)) : (works.feature ?? "");
}

const str = (v: unknown) => (typeof v === "string" ? v : "");

/** What the key is set to, from its settings alone ("Square Camera — Show in Scenes-Camera"); undefined when nothing is. */
export function chosenLine(works: WorksWith, settings: Record<string, unknown>): string | undefined {
	switch (works.chosen) {
		case "target": {
			const { source, filter } = targetOf(settings as { target?: string; source?: string; filter?: string });
			return source && filter ? `${source} — ${filter}` : undefined;
		}
		case "capture":
			return str(settings.source) || undefined;
		case "scene":
			return str(settings.scene) || undefined;
		case "modeFilter": {
			const { scene, filter } = modeKeyFilter(settings);
			return scene && filter ? `${scene} — ${filter}` : undefined;
		}
		case "sceneBlur": {
			const filter = str(settings.filter);
			return filter ? `${str(settings.scene) || "All scenes with this filter"} — ${filter}` : undefined;
		}
		default:
			return undefined;
	}
}

/**
 * What OBS says of what the key is set to: the chosen filter's kind, what
 * the chosen capture is ("Game Capture"), or that OBS has no such thing.
 * Undefined while OBS cannot be asked.
 */
export type Found = { kind?: string; what?: string; missing?: boolean };

/**
 * The row as the page shows it: its first line (the chosen filter's own kind
 * once OBS has said it, so a key that works several kinds says which) and
 * its second (what the key is set to, or that nothing is yet, or its note).
 */
export function worksWithRow(works: WorksWith, settings: Record<string, unknown>, found?: Found): { kind: string; chosen: string } {
	let kind = firstLine(works);
	if (!works.chosen) return { kind, chosen: works.note ?? "" };
	const chosen = chosenLine(works, settings);
	if (!chosen) return { kind, chosen: NOT_CHOSEN };
	if (found?.kind && isStageKit(found.kind)) kind = shownName(found.kind);
	const what = found?.what ? ` (${found.what})` : "";
	return { kind, chosen: `${chosen}${what}${found?.missing ? " (not found in OBS)" : ""}` };
}

/**
 * Every key's row as tools/make-plugin-files.py writes it: the first line,
 * the note, the settings it follows, the tooltip, and (for the list that
 * picks a filter) the kinds as OBS names them and where they are put.
 */
export function worksWithPages(): Record<string, { first: string; note: string; follow: string[]; tip: string; kinds: string[]; where: string }> {
	const out: Record<string, { first: string; note: string; follow: string[]; tip: string; kinds: string[]; where: string }> = {};
	for (const [key, works] of Object.entries(WORKS_WITH))
		out[key] = {
			first: firstLine(works),
			note: works.note ?? "",
			follow: works.chosen ? FOLLOWS[works.chosen] : [],
			tip: works.tip,
			kinds: (works.kinds ?? []).map(shownName),
			where: works.kinds ? whereOf(works.kinds) : "",
		};
	return out;
}
