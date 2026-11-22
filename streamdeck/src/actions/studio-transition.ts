import {
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
import { shownId, studioTransitionShown } from "../pictures.ts";
import { studioTransitionTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";

/** Presses OBS's Transition button with its current Preview, transition and duration. */
export class StudioTransition extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.studiotransition";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		try {
			await obs.triggerStudioModeTransition();
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => studioTransitionShown(keptPicture(s), s.sourceType);
		settings = await settle(key, "studiotransition", settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(studioTransitionTitle());
	}
}
