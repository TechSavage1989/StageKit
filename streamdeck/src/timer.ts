// A mark counts from the press (+15 ... +60 min) unless the timer, or the key, chooses clock time
// (:15 ... :00). The list names both; the key's title says the one in use.
export const MARKS = [
	{ value: "next", minute: -1, label: "Quarter hour: +15 min (or next ¼)", title: "Next ¼", nowTitle: "+15m" },
	{ value: "15", minute: 15, label: "+15 min (or :15)", title: ":15", nowTitle: "+15m" },
	{ value: "30", minute: 30, label: "+30 min (or :30)", title: ":30", nowTitle: "+30m" },
	{ value: "45", minute: 45, label: "+45 min (or :45)", title: ":45", nowTitle: "+45m" },
	{ value: "hour", minute: 0, label: "+60 min (or :00)", title: ":00", nowTitle: "+60m" },
	{ value: "saved", minute: -2, label: "Saved timer settings", title: "Timer", nowTitle: "Timer" },
];
/** The key's Count from list: the timer's own setting as it comes, or one way for this key. */
export const MARK_FROM = [
	["timer", "As the timer says"],
	["now", "From now (+15 ... +60 min)"],
	["clock", "Clock time (:15 ... :00)"],
];
export const OPERATIONS = [
	["toggle", "Set and start / Stop"],
	["cycle", "Cycle 15 / 30 / 45 / 60"],
	["setStart", "Set and start"],
	["set", "Set only"],
	["start", "Start or resume"],
	["pause", "Pause"],
	["stop", "Stop and reset"],
];

type TimerSettings = { target?: string; timerMark?: string; timerPress?: string; timerFrom?: string };

/** "now" or "clock" when the key chooses for itself, else undefined (the timer decides). */
export function keyMarkFrom(settings: TimerSettings): "now" | "clock" | undefined {
	return settings.timerFrom === "now" || settings.timerFrom === "clock" ? settings.timerFrom : undefined;
}

export function timerPress(settings: TimerSettings, held = false) {
	const preset = MARKS.find((m) => m.value === (settings.timerMark ?? "next"));
	const operation = settings.timerPress ?? "toggle";
	if (!settings.target || !OPERATIONS.some(([key]) => key === operation)) return undefined;
	const from = keyMarkFrom(settings);
	const extra = from ? { markFrom: from } : {};
	if (operation === "cycle") return { timer: settings.target, operation: held ? "cycleToggle" : "cycle", mark: -2, ...extra };
	if (!preset) return undefined;
	const set = ["toggle", "setStart", "set"].includes(operation);
	return { timer: settings.target, operation, mark: set ? preset.minute : -2, ...(set ? extra : {}) };
}

/**
 * Whether the timer's marks count from the press. A StageKit that says nothing of markFrom
 * (before marks could count from now) sets clock marks whatever is asked; with no answer at all,
 * the key's own choice or the default, from now.
 */
export function marksFromNow(settings: TimerSettings, state?: Record<string, unknown>): boolean {
	const from = keyMarkFrom(settings);
	if (state) {
		const told = state.markFrom;
		if (told !== "now" && told !== "clock") return false;
		return (from ?? told) === "now";
	}
	return (from ?? "now") === "now";
}

/** A Set key's title: the mark as it counts, +15m or :15. */
export function timerMarkTitle(settings: TimerSettings, state?: Record<string, unknown>): string {
	const preset = MARKS.find((m) => m.value === (settings.timerMark ?? "next"));
	if (!preset) return "Timer";
	return marksFromNow(settings, state) ? preset.nowTitle : preset.title;
}
