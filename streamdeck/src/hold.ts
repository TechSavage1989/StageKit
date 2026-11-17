// A key that does one thing when pressed and another when held. The owner,
// of the Stream and Record keys: "the new keys also need a long press
// setting". Any key could use it; the Stream and Record keys do.
//
// No Stream Deck code here: the tests use it as it is.

export type HoldPlan = {
	/** How long a press must last to be a long one, in ms. */
	holdMs: number;
	/** Whether the key has a long press at all. Without one the press acts on key down, with no wait. */
	long: boolean;
	/** What a press does. */
	short: () => unknown;
	/** What a long press does: once, the moment the hold time is reached. */
	longPress: () => unknown;
	/** How far a held key is to its long press, 0 to 1, about ten times a second while held (and 1 when it fires). */
	progress?: (fraction: number) => unknown;
	/** Called when a held key is let go or fires, so its picture can go back. */
	done?: () => unknown;
	stepMs?: number;
};

type Down = { timer?: ReturnType<typeof setTimeout>; ticker?: ReturnType<typeof setInterval>; fired: boolean; plan: HoldPlan };

/**
 * Presses, by key. `down` and `up` are the key's own key down and key up;
 * with a long press the choice is made on key up (a short press) or when the
 * hold time is reached (a long press, and the key up after it does nothing).
 */
export class HoldPress {
	private downs = new Map<string, Down>();

	/** A key pressed. Without a long press its press is done at once, and the promise is that press. */
	async down(id: string, plan: HoldPlan): Promise<void> {
		this.cancel(id);
		if (!plan.long) {
			// (As every other key: at once.)
			this.downs.set(id, { fired: true, plan });
			try {
				await plan.short();
			} catch {
				// (The key shows its own alert.)
			}
			return;
		}
		const started = Date.now();
		const down: Down = { fired: false, plan };
		this.downs.set(id, down);
		if (plan.progress) {
			down.ticker = setInterval(() => {
				if (!down.fired) void Promise.resolve(plan.progress!(Math.min(1, (Date.now() - started) / plan.holdMs))).catch(() => undefined);
			}, plan.stepMs ?? 100);
		}
		down.timer = setTimeout(() => {
			down.fired = true;
			this.clear(down);
			void (async () => {
				try {
					await plan.progress?.(1);
					await plan.longPress();
				} finally {
					await plan.done?.();
				}
			})().catch(() => undefined);
		}, plan.holdMs);
	}

	/** The key is let go: a short press if the long one has not fired. Says which it was ("none" when nothing was done here). */
	up(id: string): "short" | "long" | "none" {
		const down = this.downs.get(id);
		this.downs.delete(id);
		if (!down) return "none";
		if (down.fired) return down.plan.long ? "long" : "none";
		this.clear(down);
		void (async () => {
			try {
				await down.plan.done?.();
				await down.plan.short();
			} catch {
				// (The key shows its own alert.)
			}
		})();
		return "short";
	}

	/** The key went away while held: nothing is done. */
	cancel(id: string): void {
		const down = this.downs.get(id);
		if (!down) return;
		this.clear(down);
		this.downs.delete(id);
	}

	isHeld(id: string): boolean {
		const down = this.downs.get(id);
		return down !== undefined && !down.fired;
	}

	private clear(down: Down): void {
		if (down.timer) clearTimeout(down.timer);
		if (down.ticker) clearInterval(down.ticker);
		down.timer = undefined;
		down.ticker = undefined;
	}
}
