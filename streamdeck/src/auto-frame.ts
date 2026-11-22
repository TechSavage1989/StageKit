import features from "./features.json" with { type: "json" };

export type AutoFrameOperation = "measure" | "remember" | "measure_and_remember";
export type AutoFrameCommandFeature = {
	key: string;
	kind: string;
	action: AutoFrameOperation;
	name: string;
	title: string;
	pendingTitle: string;
	image: string;
	tip: string;
};

export const AUTO_FRAME_COMMANDS = features.autoFrameCommands as AutoFrameCommandFeature[];
type Reply = Record<string, unknown> | undefined;
type Send = (requestType: string, requestData: Record<string, unknown>) => Promise<Reply>;
type Clock = { now(): number; wait(ms: number): Promise<void> };
const clock: Clock = { now: Date.now, wait: (ms) => new Promise((resolve) => setTimeout(resolve, ms)) };

/** Native Auto Frame owns measurement and saving; poll only the operation this press started. */
export async function runAutoFrameCommand(
	send: Send,
	sourceName: string,
	filterName: string,
	action: AutoFrameOperation,
	rememberBy: unknown = "window",
	time: Clock = clock,
): Promise<Reply> {
	if (!sourceName || !filterName) return undefined;
	const scope = action === "measure" ? undefined : rememberBy;
	if (action !== "measure" && scope !== "application" && scope !== "window") return undefined;
	const target = { sourceName, filterName };
	const deadline = time.now() + 5000;
	const request = async (name: string, data: Record<string, unknown>): Promise<Reply> => {
		const remaining = deadline - time.now();
		if (remaining <= 0) return undefined;
		let timer: ReturnType<typeof setTimeout> | undefined;
		try {
			const reply = await Promise.race([
				send(name, data),
				new Promise<undefined>((resolve) => { timer = setTimeout(() => resolve(undefined), remaining); }),
			]);
			return time.now() < deadline ? reply : undefined;
		} finally {
			clearTimeout(timer);
		}
	};
	if (scope) {
		// Old plugins ignore unknown command fields. Ask before saving, so
		// choosing Application cannot silently create a window-name rule.
		const state = await request("GetAutoFrameState", target);
		if (state?.supportsRememberBy !== true) return undefined;
	}
	let reply = await request("AutoFrameCommand", { ...target, action, ...(scope ? { rememberBy: scope } : {}) });
	const requestId = reply?.requestId;
	if (typeof requestId !== "number" || !Number.isSafeInteger(requestId) || requestId <= 0) return undefined;
	const rememberTarget = reply?.rememberTarget;
	if (scope && (typeof rememberTarget !== "string" || !rememberTarget.trim())) return undefined;
	for (;;) {
		if (reply?.success !== true || reply.requestId !== requestId) return undefined;
		if (scope && (reply.rememberBy !== scope || reply.rememberTarget !== rememberTarget)) return undefined;
		if (reply.complete === true) {
			const completed = action === "measure" ? reply.status === "measured" : reply.status === "saved" && reply.saved === true;
			return completed ? reply : undefined;
		}
		if (reply.complete !== false || reply.status !== "pending" || time.now() >= deadline) return undefined;
		await time.wait(Math.min(100, deadline - time.now()));
		if (time.now() >= deadline) return undefined;
		reply = await request("GetAutoFrameState", { ...target, requestId });
	}
}
