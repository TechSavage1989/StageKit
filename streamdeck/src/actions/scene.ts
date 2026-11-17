import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { obs } from "../obs.ts";
import { scenePicture } from "../pictures.ts";
import { sceneTitle } from "../titles.ts";
import { keptPicture } from "../key-types.ts";
import { answerLists, type KeySettings, settle, showOnOff } from "./shared.ts";

/**
 * A key that switches OBS to a scene: on the program (live) or, in Studio
 * Mode, the preview. It is lit while its scene is the one there, however the
 * scene was changed. A transition and how long it lasts can be chosen; that
 * transition becomes OBS's current one, as choosing it in OBS would.
 */
export class SceneSwitch extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.scene";

	constructor() {
		super();
		const all = () => void this.showAll();
		obs.on("scene", all);
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
		const { scene, where, transition, lasting } = ev.payload.settings;
		if (!obs.isConnected || !scene) {
			await ev.action.showAlert();
			return;
		}
		try {
			const ms = Number(lasting);
			await obs.switchScene(String(scene), where === "preview", transition ? String(transition) : "", Number.isFinite(ms) && ms > 0 ? ms : 0);
		} catch {
			await ev.action.showAlert();
		}
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const scene = settings.scene ? String(settings.scene) : "";
		const pick = (s: KeySettings) => scenePicture(keptPicture(s), scene, s.sceneType);
		settings = await settle(key, "scene", settings, (s) => pick(s).id);
		const now = await obs.currentScenes();
		const live = settings.where === "preview" ? now?.preview : now?.program;
		await showOnOff(key, pick(settings), scene !== "" && live === scene, settings.palette);
		await key.setTitle(sceneTitle(scene));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
