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
import { shownId, slideShown } from "../pictures.ts";
import { slideTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";
import { keptPicture } from "../key-types.ts";

/** The ways a slide can go, by the names StageKit's Slide request uses. */
export const DIRECTIONS = ["up-left", "up", "up-right", "left", "centre", "right", "down-left", "down", "down-right"];

/**
 * A key that slides the item selected in OBS to the next edge that way, as
 * the StageKit dock's arrows do with "Slide to the next edge": it stops at
 * the first edge it meets and passes it on the next press.
 */
export class Slide extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.slide";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const direction = DIRECTIONS.includes(ev.payload.settings.direction ?? "") ? ev.payload.settings.direction : "left";
		const answer = await obs.stageKit("Slide", { direction });
		if (answer) await ev.action.showOk();
		else await ev.action.showAlert();
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => slideShown(keptPicture(s), s.direction, s.sourceType);
		settings = await settle(key, "slide", settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(slideTitle(settings.direction));
	}
}
