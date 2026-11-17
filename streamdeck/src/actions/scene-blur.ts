import streamDeck, {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { keptPicture } from "../key-types.ts";
import { obs } from "../obs.ts";
import { sceneBlurShown, shownId } from "../pictures.ts";
import { blurFilterItems, blurSceneItems, sceneBlurOn, sceneBlurTarget, type SceneBlurFilter } from "../scene-blurs.ts";
import { sceneBlurTitle } from "../titles.ts";
import { answerLists, type KeySettings, remembered, settle, showOnOff } from "./shared.ts";

/**
 * A key for a Scene Blur: switches the one on a scene, or every Scene Blur of
 * that name in all the scenes that have one. The owner: "the stream deck key
 * must be able to activate the blur for a specific scene, or all scenes with
 * the blur filter of the same name". Lit while it is on (with all scenes,
 * while any is), however it was switched.
 */
export class SceneBlurKey extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.sceneblur";

	constructor() {
		super();
		const all = () => void this.showAll();
		// The filter says when it is switched; the lists change with the collection.
		obs.on("sceneblur", all);
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
		const { filter, scene, press } = ev.payload.settings;
		const how = press === "on" || press === "off" ? press : "toggle";
		// (Undefined: OBS refused, or cannot be asked. False is "now off".)
		const now = filter ? await obs.sceneBlur(filter, scene ?? "", how) : undefined;
		if (now === undefined) await ev.action.showAlert();
		// (The filter says so as well, from its next tick.)
		if (ev.action.isKey()) await this.show(ev.action, ev.payload.settings);
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event !== "getSceneBlurs" && event !== "getSceneBlurScenes") {
			await answerLists(ev.payload, await ev.action.getSettings(), () => false);
			return;
		}
		let filters: SceneBlurFilter[] = [];
		try {
			filters = await obs.sceneBlurs();
		} catch (error) {
			streamDeck.logger.warn(`Could not read the Scene Blurs from OBS: ${String(error)}`);
		}
		let items =
			event === "getSceneBlurs" ? blurFilterItems(filters) : blurSceneItems(filters, (await ev.action.getSettings()).filter);
		if (!obs.isConnected) items = [{ label: "OBS is not connected (see below)", value: "" }];
		else if (event === "getSceneBlurs" && items.length === 0) items = [{ label: "No scene has a Scene Blur yet", value: "" }];
		await streamDeck.ui.sendToPropertyInspector({ event, items });
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { filter, scene } = settings;
		const filters = filter ? await obs.sceneBlurs() : [];
		const on = sceneBlurOn(filters, filter, scene);
		const type = filters.find((f) => f.filter === filter)?.type;
		const fresh = on === undefined ? undefined : { kind: "stagekit_scene_blur", enabled: on, type };
		// (While OBS cannot say, the key goes by what it last learned.)
		const seen = await remembered(key, settings, sceneBlurTarget(filter, scene), fresh);
		const pick = (s: KeySettings) => sceneBlurShown(keptPicture(s), s.look, s.sceneType, seen?.type);
		settings = await settle(key, "sceneblur", seen ? { ...settings, seen } : settings, (s) => shownId(pick(s)));
		await showOnOff(key, pick(settings), seen?.enabled === true, settings.palette);
		await key.setTitle(sceneBlurTitle(settings.press));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
