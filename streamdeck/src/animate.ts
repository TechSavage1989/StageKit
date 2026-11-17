// Keys whose picture moves: a short loop of frames, one every FRAME_MS (the
// Stream and Record keys, src/live.ts). One timer for every key; a key is
// only played while it is on the deck and in a state that moves.
//
// No Stream Deck code here: the tests use it as it is.

import { FRAME_MS } from "./live.ts";

/** Sets one image of a key: the frame, for the key's state (0 or 1). */
export type FrameSetter = (image: string, state: 0 | 1) => unknown;

type Clip = { frames: string[]; state: 0 | 1; set: FrameSetter; at: number; resting: boolean };

/**
 * Plays frames on keys. Several keys share one timer, and at most
 * `perTick` keys are given a new frame each tick, in turn: with more keys
 * than that each simply moves a little less often, so Stream Deck is never
 * sent more than `perTick` images every `frameMs`. Images are file paths or
 * data URLs already made (cached by whoever made them).
 */
export class Animator {
	private clips = new Map<string, Clip>();
	private timer: ReturnType<typeof setInterval> | undefined;
	private turn = 0;
	/** Images handed to keys, for the tests. */
	sent = 0;

	private readonly frameMs: number;
	private readonly perTick: number;

	constructor(frameMs = FRAME_MS, perTick = 12) {
		this.frameMs = frameMs;
		this.perTick = perTick;
	}

	/**
	 * Plays `frames` on a key (its first frame is assumed to be showing
	 * already). The same frames again carry on where they were; one frame or
	 * none stops the key.
	 */
	play(id: string, frames: string[], state: 0 | 1, set: FrameSetter): void {
		if (frames.length <= 1) {
			this.stop(id);
			return;
		}
		const was = this.clips.get(id);
		if (was && was.state === state && was.frames.length === frames.length && was.frames.every((f, i) => f === frames[i])) {
			was.set = set;
			return;
		}
		this.clips.set(id, { frames, state, set, at: 0, resting: was?.resting ?? false });
		if (!this.timer) {
			this.timer = setInterval(() => this.tick(), this.frameMs);
			this.timer.unref?.();
		}
	}

	/** Stops a key (it keeps the frame it shows). */
	stop(id: string): void {
		this.clips.delete(id);
		if (this.clips.size === 0 && this.timer) {
			clearInterval(this.timer);
			this.timer = undefined;
		}
	}

	/** Holds a key still for a while (a long press's bar is drawn on it), or lets it go on. */
	rest(id: string, resting: boolean): void {
		const clip = this.clips.get(id);
		if (clip) clip.resting = resting;
	}

	isPlaying(id: string): boolean {
		return this.clips.has(id);
	}

	/** How many keys are playing. */
	get size(): number {
		return this.clips.size;
	}

	/** Whether the shared timer runs (only while some key plays). */
	get running(): boolean {
		return this.timer !== undefined;
	}

	/** One step: the next frame on up to `perTick` keys, taken in turn. */
	tick(): void {
		const all = [...this.clips.values()].filter((c) => !c.resting);
		if (all.length === 0) return;
		const count = Math.min(all.length, this.perTick);
		for (let i = 0; i < count; i++) {
			const clip = all[(this.turn + i) % all.length];
			clip.at = (clip.at + 1) % clip.frames.length;
			this.sent++;
			try {
				void Promise.resolve(clip.set(clip.frames[clip.at], clip.state)).catch(() => undefined);
			} catch {
				// (A key that has gone: it is stopped when Stream Deck says so.)
			}
		}
		this.turn = all.length > count ? (this.turn + count) % all.length : 0;
	}
}

/** The one animator every moving key shares. */
export const animator = new Animator();
