// The Stream and Record keys: what OBS's stream or recording is doing, what a
// press asks of it, and the pictures of each state (tools/make-live-icons.py).
// The owner: "Id like animated stream Start buttons and stream Record buttons.
// both should be able to be set to toggle, start, pause or stop, and have
// animations that are relevant, and match the existing color themes".
//
// No Stream Deck code here: the tests use it as it is.

import live from "./live.json" with { type: "json" };

export type Output = "stream" | "record";
export type StreamState = "idle" | "starting" | "live" | "reconnecting" | "stopping";
export type RecordState = "idle" | "starting" | "recording" | "paused" | "stopping";
export type OutputState = StreamState | RecordState;

/** The frames of each state (1 for a still one), as tools/make-live-icons.py drew them. */
export const FRAMES = live.frames as Record<Output, Record<string, number>>;
/** Each palette's accent, as [r, g, b]: what a long press's bar fills in. */
export const ACCENTS = live.accents as Record<string, number[]>;
/** One frame every this many ms: 8 a second. */
export const FRAME_MS = 125;

/** A state's pictures, by id in src/pack.json: one for a still state. */
export function framesOf(output: Output, state: string): string[] {
	const n = FRAMES[output][state] ?? 1;
	const known = FRAMES[output][state] !== undefined ? state : "idle";
	return Array.from({ length: n }, (_, f) => `${live.group}/${output}-${known}-${f}`);
}

/** Whether a state is drawn as a loop of frames. */
export function animated(output: Output, state: string): boolean {
	return (FRAMES[output][state] ?? 1) > 1;
}

/**
 * Whether the key is on (the green mark, the key's second state) in a state:
 * on while OBS's output is running (live, recording, paused, reconnecting,
 * stopping), off while it is stopped or still starting.
 */
export function isOn(state: string): boolean {
	return state !== "idle" && state !== "starting";
}

/**
 * The state of obs-websocket's StreamStateChanged and RecordStateChanged
 * (`outputState`, "OBS_WEBSOCKET_OUTPUT_STARTED" and so on) for a key.
 * Undefined for one it does not know.
 */
export function stateOfEvent(output: Output, outputState: unknown): OutputState | undefined {
	const s = String(outputState ?? "").replace(/^OBS_WEBSOCKET_OUTPUT_/, "");
	switch (s) {
		case "STARTING":
			return "starting";
		case "STARTED":
		case "RECONNECTED":
		case "RESUMED":
			return output === "stream" ? "live" : "recording";
		case "STOPPING":
			return "stopping";
		case "STOPPED":
			return "idle";
		case "RECONNECTING":
			return output === "stream" ? "reconnecting" : undefined;
		case "PAUSED":
			return output === "record" ? "paused" : undefined;
		default:
			return undefined;
	}
}

/** The state GetStreamStatus or GetRecordStatus says (they know nothing of starting or stopping). */
export function stateOfStatus(output: Output, status: Record<string, unknown>): OutputState {
	if (status.outputActive !== true) return "idle";
	if (output === "stream") return status.outputReconnecting === true ? "reconnecting" : "live";
	return status.outputPaused === true ? "paused" : "recording";
}

/**
 * What a press can do, by key. A stream cannot be paused: obs-websocket has
 * no request for it, because OBS itself cannot pause a stream (only a
 * recording). So Pause is offered on the Record key only.
 */
export const PRESSES: Record<Output, Array<[value: string, label: string]>> = {
	stream: [
		["toggle", "Toggle (start or stop)"],
		["start", "Start streaming"],
		["stop", "Stop streaming"],
	],
	record: [
		["toggle", "Toggle (start or stop)"],
		["start", "Start recording"],
		["stop", "Stop recording"],
		["pause", "Pause / resume"],
		["pauseonly", "Pause"],
		["resume", "Resume"],
	],
};

/** A long press: Nothing (as it comes: the key acts on press down, as every other key), or what a press can do. */
export function longPresses(output: Output): Array<[string, string]> {
	return [["none", "Nothing"], ...PRESSES[output]];
}

const REQUESTS: Record<Output, Record<string, string>> = {
	stream: { toggle: "ToggleStream", start: "StartStream", stop: "StopStream" },
	record: {
		toggle: "ToggleRecord",
		start: "StartRecord",
		stop: "StopRecord",
		pause: "TogglePauseRecord",
		pauseonly: "PauseRecord",
		resume: "ResumeRecord",
	},
};

/**
 * The obs-websocket request a press sends, or undefined for nothing (a long
 * press left on Nothing, or a press a key does not have: Pause on a Stream
 * key). An unset press is Toggle.
 */
export function requestFor(output: Output, press: unknown): string | undefined {
	const p = typeof press === "string" && press !== "" ? press : "toggle";
	return REQUESTS[output][p];
}

/** The hold time of a long press, in ms: 800 as it comes, kept between 300 and 5000. */
export function holdMs(value: unknown): number {
	const n = Number(value);
	if (value === undefined || value === "" || !Number.isFinite(n)) return 800;
	return Math.min(5000, Math.max(300, Math.round(n)));
}

/** obs-websocket's timecode ("01:02:03.456") as a key shows it: "1:02:03", or "02:03" under an hour. */
export function shortTime(timecode: unknown): string | undefined {
	const m = /^(\d+):(\d\d):(\d\d)/.exec(String(timecode ?? ""));
	if (!m) return undefined;
	const h = Number(m[1]);
	return h > 0 ? `${h}:${m[2]}:${m[3]}` : `${m[2]}:${m[3]}`;
}

/** The ms in a timecode, or undefined. */
export function timeMs(timecode: unknown): number | undefined {
	const m = /^(\d+):(\d\d):(\d\d)(?:\.(\d+))?/.exec(String(timecode ?? ""));
	if (!m) return undefined;
	return ((Number(m[1]) * 60 + Number(m[2])) * 60 + Number(m[3])) * 1000 + Number((m[4] ?? "0").padEnd(3, "0").slice(0, 3));
}

/** A time in ms as obs-websocket writes a timecode. */
export function timecodeOf(ms: number): string {
	const s = Math.floor(ms / 1000);
	const p = (n: number) => String(n).padStart(2, "0");
	return `${p(Math.floor(s / 3600))}:${p(Math.floor(s / 60) % 60)}:${p(s % 60)}.${String(Math.floor(ms % 1000)).padStart(3, "0")}`;
}
