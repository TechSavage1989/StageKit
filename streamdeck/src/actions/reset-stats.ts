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
import { resetStatsShown, shownId } from "../pictures.ts";
import { resetStatsTitle } from "../titles.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";

/** A single press of OBS's own Reset Stats command, with no source or scene target. */
export class ResetStats extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.resetstats";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		if (!obs.isConnected) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.pressHotkey("OBSBasic.ResetStats");
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => resetStatsShown(keptPicture(s), s.sourceType);
		settings = await settle(key, "resetstats", settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(resetStatsTitle());
	}
}
