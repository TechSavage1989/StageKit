import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type KeyUpEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
	type WillDisappearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { animator } from "../animate.ts";
import { withHoldBar } from "../compose.ts";
import { HoldPress } from "../hold.ts";
import { ACCENTS, animated, framesOf, holdMs, isOn, type Output, type OutputState, requestFor, shortTime, stateOfEvent, stateOfStatus, timeMs, timecodeOf } from "../live.ts";
import { obs } from "../obs.ts";
import { outputShown } from "../pictures.ts";
import { outputTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, remembered, showOnOff } from "./shared.ts";

/** What OBS last said of the stream or the recording, for every key of the kind. */
type Now = { state: OutputState; known: boolean; timeMs?: number; at: number };

/** A key as an action is given one: what this file uses of it. */
type Key = KeyAction<KeySettings>;

/**
 * The Stream key and the Record key: start, stop, toggle (and, for a
 * recording, pause and resume) OBS's stream or recording through
 * obs-websocket's own requests, nothing of StageKit's. The key is on (green
 * mark) while it runs; its picture moves in the states that change
 * (src/live.ts, tools/make-live-icons.py) while it is on the deck, unless
 * *Animate* is Off. A long press can do something else (the owner: "the new
 * keys also need a long press setting").
 */
export class OutputKey extends SingletonAction<KeySettings> {
	override readonly manifestId: string;
	private now: Now = { state: "idle", known: false, at: 0 };
	private readonly presses = new HoldPress();
	private clock: ReturnType<typeof setInterval> | undefined;
	/** The image each key shows now (the frame of its state), for a long press's bar. */
	private showing = new Map<string, { image: string; palette: string }>();

	readonly output: Output;

	constructor(output: Output) {
		super();
		this.output = output;
		this.manifestId = `com.techsavage1989.stagekit.${output}`;
		obs.on("output", (which: string, outputState: unknown) => {
			if (which !== this.output) return;
			const state = stateOfEvent(this.output, outputState);
			if (!state) return;
			// The time: from nought at a start, held still while paused, on again at a resume (OBS's own timecode follows).
			const running = (s: string) => s === "live" || s === "recording";
			let ms = this.now.timeMs;
			if (ms !== undefined && running(this.now.state)) ms += Date.now() - this.now.at;
			if (running(state) && (this.now.state === "idle" || this.now.state === "starting")) ms = 0;
			if (state === "idle") ms = undefined;
			this.now = { state, known: true, timeMs: ms, at: Date.now() };
			void this.showAll();
			if (state === "recording" || state === "live") void this.refresh();
		});
		const all = () => void this.refresh();
		obs.on("connection", all);
		obs.on("changed", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		if (!ev.action.isKey()) return Promise.resolve();
		const key = ev.action;
		return (this.now.known ? Promise.resolve() : this.refresh(false)).then(() => this.show(key, ev.payload.settings));
	}

	override onWillDisappear(ev: WillDisappearEvent<KeySettings>): void {
		animator.stop(ev.action.id);
		this.presses.cancel(ev.action.id);
		this.showing.delete(ev.action.id);
		this.tickClock();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		if (!ev.action.isKey()) return;
		const key = ev.action;
		const settings = ev.payload.settings;
		// (Unset is Nothing: a key made before there was a long press acts on press down, as it did.)
		const long = typeof settings.longPress === "string" && settings.longPress !== "" && settings.longPress !== "none" && requestFor(this.output, settings.longPress) !== undefined;
		await this.presses.down(key.id, {
			holdMs: holdMs(settings.holdMs),
			long,
			short: () => this.send(key, settings, settings.press),
			longPress: () => this.send(key, settings, settings.longPress),
			progress: (fraction) => this.bar(key, fraction),
			done: () => this.unbar(key, settings),
		});
	}

	override onKeyUp(ev: KeyUpEvent<KeySettings>): void {
		this.presses.up(ev.action.id);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	/** Sends a press's request; an alert when OBS is not there or refuses (starting what runs, say). */
	private async send(key: Key, settings: KeySettings, press: unknown): Promise<void> {
		const request = requestFor(this.output, press);
		if (!request) return;
		if (!obs.isConnected) {
			await key.showAlert();
			return;
		}
		try {
			await obs.outputRequest(request);
		} catch {
			await key.showAlert();
			await this.show(key, settings);
		}
	}

	/** Asks OBS how the stream or recording is (when it can be asked), and shows every key again. */
	private async refresh(showAfter = true): Promise<void> {
		const status = await obs.outputStatus(this.output);
		if (status) {
			const state = stateOfStatus(this.output, status);
			// (The status knows nothing of starting and stopping: those come from the events, and are kept until the next.)
			const keep = (this.now.state === "starting" && state === "idle") || (this.now.state === "stopping" && state !== "idle");
			this.now = { state: keep ? this.now.state : state, known: true, timeMs: timeMs(status.outputTimecode), at: Date.now() };
		} else if (!obs.isConnected) {
			this.now = { ...this.now, known: false };
		}
		if (showAfter) await this.showAll();
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) if (key.isKey()) await this.show(key, await key.getSettings());
	}

	/** The time to show while live or recording, counted on from OBS's last timecode. */
	private time(): string | undefined {
		const { state, timeMs: ms, at } = this.now;
		if (ms === undefined || (state !== "live" && state !== "recording")) return undefined;
		return shortTime(timecodeOf(ms + (Date.now() - at)));
	}

	private async show(key: Key, settings: KeySettings): Promise<void> {
		// While OBS cannot be asked, what it said last (still: it is not known to be going on).
		const fresh = this.now.known ? { kind: `obs_${this.output}`, enabled: isOn(this.now.state), type: this.now.state } : undefined;
		const seen = await remembered(key, settings, this.output, fresh);
		const state = (this.now.known ? this.now.state : (seen?.type as OutputState | undefined)) ?? "idle";
		const on = isOn(state);
		const frames = framesOf(this.output, state);
		const shown = outputShown(this.output, state, frames[0], settings.sceneType);
		await showOnOff(key, shown, on, settings);
		const typed = shown.picture.id !== frames[0];
		const image = imageOf(shown, settings, on);
		this.showing.set(key.id, { image, palette: settings.palette ?? "mono" });
		const moving = this.now.known && settings.animate !== "off" && !typed && animated(this.output, state);
		if (moving)
			animator.play(
				key.id,
				frames.map((id) => imageOf({ picture: outputShown(this.output, state, id).picture }, settings, on)),
				on ? 1 : 0,
				(frame, s) => {
					const was = this.showing.get(key.id);
					if (was) was.image = frame;
					return key.setImage(frame, { state: s });
				},
			);
		else animator.stop(key.id);
		await key.setTitle(outputTitle(this.output, settings.press, state, settings.showTime === "off" ? undefined : this.time(), settings.longPress));
		this.tickClock();
	}

	/** A long press's bar on the key, filling: the key's picture holds still meanwhile. */
	private async bar(key: Key, fraction: number): Promise<void> {
		const showing = this.showing.get(key.id);
		if (!showing) return;
		animator.rest(key.id, true);
		const colour = ACCENTS[showing.palette] ?? ACCENTS.mono;
		await key.setImage(withHoldBar(showing.image, fraction, colour), { state: isOn(this.now.state) ? 1 : 0 });
	}

	private async unbar(key: Key, settings: KeySettings): Promise<void> {
		animator.rest(key.id, false);
		await this.show(key, settings);
	}

	/**
	 * Once a second, while a key of this kind shows a time: its title again,
	 * and every ten seconds OBS's own timecode (so the count does not drift).
	 */
	private tickClock(): void {
		const want = (this.now.state === "live" || this.now.state === "recording") && this.showing.size > 0;
		if (want && !this.clock) {
			let n = 0;
			this.clock = setInterval(() => {
				if (++n % 10 === 0) void this.refresh(false);
				void this.titles();
			}, 1000);
			this.clock.unref?.();
		} else if (!want && this.clock) {
			clearInterval(this.clock);
			this.clock = undefined;
		}
	}

	private async titles(): Promise<void> {
		for (const key of this.actions) {
			if (!key.isKey() || !this.showing.has(key.id)) continue;
			const settings = await key.getSettings();
			if (settings.showTime === "off") continue;
			await key.setTitle(outputTitle(this.output, settings.press, this.now.state, this.time(), settings.longPress));
		}
	}
}
