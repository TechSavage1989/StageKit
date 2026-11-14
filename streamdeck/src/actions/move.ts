import {
	action,
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { isMove } from "../kinds.ts";
import { obs } from "../obs.ts";
import { choose, file } from "../pictures.ts";
import { answerLists, type KeySettings, look } from "./shared.ts";

/**
 * A key that runs one StageKit move: a Send To key on the camera, a Place
 * Item, a Layout or a Picture Shape key. Those run when they are switched on
 * and switch themselves off again.
 */
@action({ UUID: "com.techsavage1989.stagekit.move" })
export class Move extends SingletonAction<KeySettings> {
	constructor() {
		super();
		const all = () => void this.showAll();
		obs.on("connection", all);
		obs.on("changed", all);
		look.on("changed", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	/** The picture of where it sends the camera (or the one chosen). */
	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const now = await obs.filter(settings.source ?? "", settings.filter ?? "");
		const picture = choose(settings.picture, now?.kind ?? "", now?.settings);
		await key.setImage(picture ? file(picture, true, look.colour) : undefined);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source, filter } = ev.payload.settings;
		if (!obs.isConnected || !source || !filter) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilter(source, filter, true);
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, isMove));
	}
}
