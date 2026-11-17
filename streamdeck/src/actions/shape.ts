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
import { shapeShown, shownId } from "../pictures.ts";
import { shapeTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";
import { keptPicture } from "../key-types.ts";

/**
 * A key that sets a game's picture shape by hand, for when Auto Frame's own
 * reading is not wanted or is wrong: 4:3, 8:7 and so on, or "Automatic" to
 * hand it back. Set "for this game", the shape lasts until that game's
 * capture ends and then goes back to automatic by itself.
 */
export class PictureShape extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.shape";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source } = targetOf(ev.payload.settings);
		const { shape, keep } = ev.payload.settings;
		const answer = source
			? await obs.stageKit("SetPictureShape", {
					sourceName: source,
					shape: shape || "auto",
					untilGone: keep !== "always",
				})
			: undefined;
		if (answer) await ev.action.showOk();
		else await ev.action.showAlert();
	}

	/** It is offered the sources that have an Auto Frame. */
	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action
			.getSettings()
			.then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_auto_frame"));
	}

	/** The shape is the key's title unless the user wrote their own; the picture is Auto Frame's. */
	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => shapeShown(keptPicture(s), s.shape, s.sourceType);
		settings = await settle(key, "shape", settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(shapeTitle(settings.shape));
	}
}
