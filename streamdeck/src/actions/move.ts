import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { targetOf } from "../kinds.ts";
import { obs } from "../obs.ts";
import { keptPicture } from "../key-types.ts";
import { file, movePicture, plain } from "../pictures.ts";
import { moveTitle } from "../titles.ts";
import { answerLists, type KeySettings, remembered, settle } from "./shared.ts";

/**
 * A key that runs one StageKit move: a Send To on the camera, a Place Item or
 * a Layout. Those run when they are switched on and switch themselves off
 * again. One of these per feature; `wanted` says which filter kinds it is for.
 */
export class Move extends SingletonAction<KeySettings> {
	override readonly manifestId: string;
	/** The key's name in features.json ("sendto" ...), for its type lists. */
	private readonly name: string;

	constructor(
		uuid: string,
		private readonly wanted: (kind: string) => boolean,
	) {
		super();
		this.manifestId = uuid;
		this.name = uuid.slice(uuid.lastIndexOf(".") + 1);
		const all = () => void this.showAll();
		obs.on("connection", all);
		obs.on("changed", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	/** The picture of where it sends the camera (or the one chosen). */
	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { source, filter } = targetOf(settings);
		const now = await obs.filter(source, filter);
		// (While OBS cannot say, the key goes by what it last learned.)
		const seen = await remembered(key, settings, `${source}\n${filter}`, now);
		const pick = (s: KeySettings) => movePicture(keptPicture(s), seen?.kind ?? "", seen?.settings, s) ?? plain("move");
		settings = await settle(key, this.name, seen ? { ...settings, seen } : settings, (s) => pick(s).id);
		const picture = pick(settings);
		await key.setImage(file(picture, true, settings.palette));
		await key.setTitle(moveTitle(seen?.kind, seen?.settings));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source, filter } = targetOf(ev.payload.settings);
		if (!obs.isConnected || !source || !filter) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilter(source, filter, true);
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, this.wanted));
	}
}
