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
import { file, optionPicture } from "../pictures.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/** The dock's switches a key can hold, by the names StageKit's requests use. */
export const OPTIONS = ["snap", "guides", "limit", "slide", "everyScene", "keepShape"];

/**
 * A key for one of the StageKit dock's switches: snap on release, guides,
 * stop resizing at edges and the rest. It shows which way the switch is,
 * also when it is changed in the dock.
 */
export class DockOption extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.option";

	constructor() {
		super();
		const all = () => void this.showAll();
		obs.on("connection", all);
		obs.on("options", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { option = "snap", press = "toggle" } = ev.payload.settings;
		const answer = await obs.stageKit("SetOption", { option, mode: press });
		if (!answer) await ev.action.showAlert();
		// Stream Deck flips a two-state key by itself on every press; put it
		// back to what the switch really is.
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const picture = optionPicture(settings.picture, settings.option);
		await key.setImage(file(picture, false, settings.palette), { state: 0 });
		await key.setImage(file(picture, true, settings.palette), { state: 1 });
		const options = await obs.stageKit("GetOptions");
		await key.setState(options?.[settings.option ?? "snap"] === true ? 1 : 0);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
