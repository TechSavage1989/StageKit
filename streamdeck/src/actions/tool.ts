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

import { obs } from "../obs.ts";
import { file, toolPicture } from "../pictures.ts";
import { hotkeyOf } from "../tools.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * A key for one of StageKit's arrange tools: line up, space out, snap, match
 * sizes, copy a transform to other scenes. It works on the items selected in
 * OBS's preview, exactly like the tool's button in the StageKit dock.
 */
@action({ UUID: "com.techsavage1989.stagekit.tool" })
export class Tool extends SingletonAction<KeySettings> {
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

	private show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		return key.setImage(file(toolPicture(settings.picture, settings.tool), true, settings.palette));
	}
}
