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
import { shownId, toolShown } from "../pictures.ts";
import { toolTitle } from "../titles.ts";
import { hotkeyOf } from "../tools.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";
import { keptPicture } from "../key-types.ts";

/**
 * A key for one of StageKit's arrange tools: line up, space out, snap, match
 * sizes, copy a transform to other scenes. It works on the items selected in
 * OBS's preview, exactly like the tool's button in the StageKit dock.
 */
export class Tool extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.tool";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const hotkey = hotkeyOf(ev.payload.settings.tool);
		if (!obs.isConnected || !hotkey) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.pressHotkey(hotkey);
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => toolShown(keptPicture(s), s.tool, s.sourceType);
		settings = await settle(key, "tool", settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(toolTitle(settings.tool));
	}
}
