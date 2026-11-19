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
import { optionShown, shownId } from "../pictures.ts";
import { optionTitle } from "../titles.ts";
import { answerLists, type KeySettings, settle, showOnOff } from "./shared.ts";
import { keptPicture } from "../key-types.ts";

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
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => optionShown(keptPicture(s), s.option, s.sourceType);
		settings = await settle(key, "option", settings, (s) => shownId(pick(s)));
		const picture = pick(settings);
		const options = await obs.stageKit("GetOptions");
		await showOnOff(key, picture, options?.[settings.option ?? "snap"] === true, settings);
		await key.setTitle(optionTitle(settings.option, settings.press));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
