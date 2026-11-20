import streamDeck, {
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

import { withHoldBar } from "../compose.ts";
import { HoldPress } from "../hold.ts";
import { keptPicture } from "../key-types.ts";
import { TARGET_SEP } from "../kinds.ts";
import { ACCENTS, holdMs } from "../live.ts";
import {
	editModeSettings,
	migrateModeKey,
	modeChoiceItems,
	modeFilterItems,
	modeKeyFilter,
	modeLongPress,
	type ModePress,
	modePress,
	modeRequest,
	perModeOf,
	shownMode,
} from "../modes.ts";
import { obs } from "../obs.ts";
import { followedModePicture } from "../pictures.ts";
import { followedModeTitle, modeTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, remembered, settle, showOnOff } from "./shared.ts";

/**
 * A key for one Scene Modes filter (`modeFilter`). *On press* (and an optional
 * *Long press*, src/hold.ts as the Stream and Record keys): *Switch on* or
 * *Toggle* a mode of the filter (`mode`, `longMode`), *Switch off* (no mode),
 * or *Next mode*, through the modes the key uses in the filter's order. Each
 * mode has its own settings on the key (`perMode`: Use in Next, Layout,
 * Image), edited through the page's *Mode settings* list (src/modes.ts
 * `editModeSettings`); the key-wide Layout and Mode type of a key saved up to
 * beta179 are still read behind them. The image and title are the mode that is on; a Switch
 * on key shows its own mode, lit or dimmed, as before. Modes are kept by id,
 * so renaming one in OBS breaks no key.
 */
export class SceneModeKey extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.mode";

	private readonly presses = new HoldPress();
	/** What each key shows now, for the long press's bar. */
	private readonly showing = new Map<string, { image: string; palette: string; lit: boolean }>();

	constructor() {
		super();
		const all = () => void this.showAll();
		// The filter says when its mode changes and when its modes are edited.
		obs.on("mode", all);
		obs.on("connection", all);
		obs.on("changed", all);
	}

	override async onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		if (ev.action.isKey()) await this.show(ev.action, await this.upToDate(ev.action, ev.payload.settings));
	}

	override onWillDisappear(ev: WillDisappearEvent<KeySettings>): void {
		this.presses.cancel(ev.action.id);
		this.showing.delete(ev.action.id);
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		if (ev.action.isKey()) await this.show(ev.action, await this.upToDate(ev.action, ev.payload.settings));
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		if (!ev.action.isKey()) return;
		const key = ev.action;
		const settings = await this.upToDate(key, ev.payload.settings);
		const long = modeLongPress(settings.longPress);
		await this.presses.down(key.id, {
			holdMs: holdMs(settings.holdMs),
			long: long !== undefined,
			short: () => this.send(key, settings, modePress(settings.press), ownMode(settings, "mode")),
			longPress: () => this.send(key, settings, long ?? "on", ownMode(settings, "longMode")),
			progress: (fraction) => this.bar(key, fraction),
			done: () => this.show(key, settings),
		});
	}

	override onKeyUp(ev: KeyUpEvent<KeySettings>): void {
		this.presses.up(ev.action.id);
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event === "getModeFilters") {
			let items: Array<{ label: string; value: string }> = [];
			try {
				items = modeFilterItems(await obs.sceneModes());
			} catch (error) {
				streamDeck.logger.warn(`Could not read the scene modes from OBS: ${String(error)}`);
			}
			if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
			else if (items.length === 0) items = [{ label: "No scene has a Scene Modes filter yet", value: "" }];
			await streamDeck.ui.sendToPropertyInspector({ event, items });
			return;
		}
		if (event === "getFilterModes") {
			// The modes of the key's filter: the Mode settings, Switch to mode and Long press mode lists.
			const { scene, filter } = modeKeyFilter(await ev.action.getSettings());
			const now = scene ? await obs.sceneMode(scene, filter) : undefined;
			let items = now ? modeChoiceItems(now.modes) : [];
			if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
			else if (!scene) items = [{ label: "Choose the Scene Modes first", value: "" }];
			else if (items.length === 0) items = [{ label: "That Scene Modes has no modes", value: "" }];
			await streamDeck.ui.sendToPropertyInspector({ event, items });
			return;
		}
		await answerLists(ev.payload, await ev.action.getSettings(), () => false, this.manifestId);
	}

	/**
	 * A key from before beta175 moved to its filter, and the page's per-mode
	 * lists kept per mode; written back when changed. With no mode in *Mode
	 * settings* and none to switch to, the filter's first mode is chosen there.
	 */
	private async upToDate(key: KeyAction<KeySettings>, settings: KeySettings): Promise<KeySettings> {
		const moved = migrateModeKey(settings) ?? settings;
		let edited = editModeSettings(moved) ?? moved;
		const { scene, filter } = modeKeyFilter(edited);
		if (scene && !edited.modeFor) {
			const first = (await obs.sceneMode(scene, filter))?.modes[0]?.id;
			if (first) edited = editModeSettings(edited, first) ?? edited;
		}
		if (edited !== settings) await key.setSettings(edited);
		return edited;
	}

	private async send(key: KeyAction<KeySettings>, settings: KeySettings, press: ModePress, mode: string): Promise<void> {
		const { scene, filter } = modeKeyFilter(settings);
		const perMode = perModeOf(settings);
		// (Next mode with some modes left out goes by the mode on now.)
		const skips = Object.values(perMode).some((m) => m.skip === true);
		const now = press === "next" && skips && scene ? await obs.sceneMode(scene, filter) : undefined;
		const request = scene ? modeRequest(press, mode, perMode, now) : undefined;
		// (The filter acts in its next tick and then says so: the key shows that, not here.)
		if (!request || !(await obs.setSceneMode(scene, filter, request.mode ?? "", request.how))) await key.showAlert();
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { scene, filter } = modeKeyFilter(settings);
		const press = modePress(settings.press);
		const asked = ownMode(settings, "mode");
		const now = scene ? await obs.sceneMode(scene, filter) : undefined;
		// (A mode named by hand, rather than by its id, is found by name.)
		const own = now?.modes.find((m) => m.id === asked) ?? now?.modes.find((m) => m.name === asked);
		const ownId = own?.id ?? asked;
		const { shows, lit } = shownMode(press, ownId, now?.current ?? "");
		const mode = now?.modes.find((m) => m.id === shows);
		const kind = "stagekit_scene_modes";
		const fresh = !now
			? undefined
			: press === "on"
				? own && { kind, enabled: lit, name: own.name, type: own.type }
				: { kind, enabled: lit, id: mode?.id ?? "", name: mode?.name ?? "", type: mode?.type ?? "" };
		// (While OBS cannot say, the key goes by what it last learned. A Switch
		// on key keeps what it kept before beta175; the others keep the mode
		// that was on, apart, since that means another mode.)
		const target = [scene, filter, ownId].join(TARGET_SEP);
		const seen = await remembered(key, settings, press === "on" ? target : `${target}|follows`, fresh || undefined);
		const on =
			press === "on"
				? { id: ownId, name: seen?.name, type: seen?.type }
				: seen?.id
					? { id: seen.id, name: seen.name, type: seen.type }
					: undefined;
		const pick = (s: KeySettings) => followedModePicture(s, ownId, keptPicture(s), on).picture;
		settings = await settle(key, "mode", seen ? { ...settings, seen } : settings, (s) => pick(s).id);
		const { picture, look } = followedModePicture(settings, ownId, keptPicture(settings), on);
		const isLit = seen?.enabled === true;
		await showOnOff(key, picture, isLit, look);
		this.showing.set(key.id, { image: imageOf(picture, look, isLit), palette: look.palette ?? "mono", lit: isLit });
		await key.setTitle(press === "on" ? modeTitle(seen?.name) : followedModeTitle(on));
	}

	/** A long press's bar on the key, filling. */
	private async bar(key: KeyAction<KeySettings>, fraction: number): Promise<void> {
		const showing = this.showing.get(key.id);
		if (!showing) return;
		await key.setImage(withHoldBar(showing.image, fraction, ACCENTS[showing.palette] ?? ACCENTS.mono), { state: showing.lit ? 1 : 0 });
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey() && !this.presses.isHeld(key.id)) await this.show(key, await key.getSettings());
		}
	}
}

/** The mode a press of the key acts on: `mode` or `longMode` (`mode` when unset; an old key's from its `target`). */
function ownMode(settings: KeySettings, which: "mode" | "longMode"): string {
	const own = settings[which];
	if (typeof own === "string" && own !== "") return own;
	if (which === "longMode") return ownMode(settings, "mode");
	return (migrateModeKey(settings)?.mode as string | undefined) ?? "";
}
