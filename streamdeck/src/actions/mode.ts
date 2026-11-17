import streamDeck, {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { modeItems, modeTarget } from "../modes.ts";
import { obs } from "../obs.ts";
import { modePicture } from "../pictures.ts";
import { modeTitle } from "../titles.ts";
import { answerLists, type KeySettings, remembered, showOnOff } from "./shared.ts";

/**
 * A key that turns on one mode of a Scene Modes filter: that mode's sources
 * show and the other modes' sources hide. It is lit while that mode is the
 * one on, however it was turned on. The key keeps the mode's id, so renaming
 * the mode in OBS does not break it; its title is the mode's name.
 */
export class SceneModeKey extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.mode";

	constructor() {
		super();
		const all = () => void this.showAll();
		// The filter says when its mode changes and when its modes are edited.
		obs.on("mode", all);
		obs.on("connection", all);
		obs.on("changed", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { scene, filter, mode } = modeTarget(ev.payload.settings.target);
		const how = ev.payload.settings.press === "toggle" ? "toggle" : "on";
		// (The filter acts in its next tick and then says so: the key is lit
		// by that, not here.)
		if (!scene || !mode || !(await obs.setSceneMode(scene, filter, mode, how))) await ev.action.showAlert();
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event !== "getModes") {
			await answerLists(ev.payload, await ev.action.getSettings(), () => false);
			return;
		}
		let items: Array<{ label: string; value: string }> = [];
		try {
			items = modeItems(await obs.sceneModes());
		} catch (error) {
			streamDeck.logger.warn(`Could not read the scene modes from OBS: ${String(error)}`);
		}
		if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
		else if (items.length === 0) items = [{ label: "No scene has a Scene Modes filter with modes yet", value: "" }];
		await streamDeck.ui.sendToPropertyInspector({ event, items });
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { scene, filter, mode } = modeTarget(settings.target);
		const now = scene && mode ? await obs.sceneMode(scene, filter) : undefined;
		// (A mode named by hand, rather than by its id, is found by name.)
		const found = now?.modes.find((m) => m.id === mode) ?? now?.modes.find((m) => m.name === mode);
		const fresh = now && found ? { kind: "stagekit_scene_modes", enabled: now.current === found.id, name: found.name, type: found.type } : undefined;
		// (While OBS cannot say, the key goes by what it last learned.)
		const seen = await remembered(key, settings, settings.target ?? "", fresh);
		const picture = modePicture(settings.picture, settings.sourceType, seen?.type, seen?.name);
		await showOnOff(key, picture, seen?.enabled === true, settings.palette);
		await key.setTitle(modeTitle(seen?.name));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
