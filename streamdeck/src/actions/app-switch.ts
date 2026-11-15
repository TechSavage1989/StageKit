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
import { file, stepPicture } from "../pictures.ts";
import { stepTitle } from "../titles.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * A key for the hand controls of a Show Source in Use filter (on a scene) or
 * a Capture App in Use filter (on a Window Capture): on to the next capture or
 * app switched to by hand, back to following the apps, or, for a Capture App
 * in Use, the capture hidden and shown. They are OBS hotkeys of that filter
 * ("stagekit.active.next.<filter>", ".back." and ".hide."), pressed by name.
 */
export class AppSwitch extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.appswitch";

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { filter } = targetOf(ev.payload.settings);
		if (!obs.isConnected || !filter) {
			await ev.action.showAlert();
			return;
		}
		try {
			const step = ev.payload.settings.step;
			await obs.pressHotkey(`stagekit.active.${step === "back" || step === "hide" || step === "toggle" || step === "clear" ? step : "next"}.${filter}`);
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action
			.getSettings()
			.then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_active_app" || kind === "stagekit_app_capture" || kind === "stagekit_auto_capture"));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		await key.setImage(file(stepPicture(settings.picture, settings.step), true, settings.palette));
		await key.setTitle(stepTitle(settings.step));
	}
}
