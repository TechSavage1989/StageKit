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
import { file, PICTURES, plain } from "../pictures.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * A key for an Active App filter's two hand controls: on to the next of the
 * "captures you switch to", or back to following the apps. They are OBS
 * hotkeys of that filter ("stagekit.active.next.<filter>" and ".back."),
 * pressed by name.
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
			await obs.pressHotkey(`stagekit.active.${ev.payload.settings.step === "back" ? "back" : "next"}.${filter}`);
			await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action
			.getSettings()
			.then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_active_app"));
	}

	private show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const fits = settings.step === "back" ? "filters/back-to-apps" : "filters/next-capture";
		const picture =
			PICTURES.find((p) => p.id === settings.picture) ?? PICTURES.find((p) => p.id === fits) ?? plain("move");
		return key.setImage(file(picture, true, settings.palette));
	}
}
