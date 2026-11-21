export const MARKS = [
	{ value: "next", minute: -1, label: "Next quarter hour", title: "Next ¼" },
	{ value: "15", minute: 15, label: "Next :15", title: ":15" },
	{ value: "30", minute: 30, label: "Next :30", title: ":30" },
	{ value: "45", minute: 45, label: "Next :45", title: ":45" },
	{ value: "hour", minute: 0, label: "Next full hour (:00)", title: ":00" },
	{ value: "saved", minute: -2, label: "Saved timer settings", title: "Timer" },
];
export const OPERATIONS = [
	["toggle", "Set and start / Stop"],
	["setStart", "Set and start"],
	["set", "Set only"],
	["start", "Start or resume"],
	["pause", "Pause"],
	["stop", "Stop and reset"],
];

export function timerPress(settings: { target?: string; timerMark?: string; timerPress?: string }) {
	const preset = MARKS.find((m) => m.value === (settings.timerMark ?? "next"));
	const operation = settings.timerPress ?? "toggle";
	if (!settings.target || !preset || !OPERATIONS.some(([key]) => key === operation)) return undefined;
	const set = ["toggle", "setStart", "set"].includes(operation);
	return { timer: settings.target, operation, mark: set ? preset.minute : -2 };
}
