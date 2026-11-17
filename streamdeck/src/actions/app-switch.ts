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
import { answerLists, type KeySettings, showOnOff } from "./shared.ts";

/**
 * A key for the hand controls of a Show Source in Use filter (on a scene) or
 * a Capture App in Use filter (on a Window Capture): on to the next capture or
 * app switched to by hand, back to following the apps, or, for a Capture App
 * in Use, the capture hidden and shown. They are OBS hotkeys of that filter
 * ("stagekit.active.next.<filter>", ".back." and ".hide."), pressed by name.
 */
export class AppSwitch extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.appswitch";

	constructor() {
		super();
		// The switcher says when it goes manual or automatic, hidden or shown.
		obs.on("switcher", () => void this.showAll());
		obs.on("connection", () => void this.showAll());
		obs.on("changed", () => void this.showAll());
	}

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
			// (A key showing which way it is says so by its picture.)
			if (step !== "toggle" && step !== "hide") await ev.action.showOk();
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
		const picture = stepPicture(settings.picture, settings.step, settings.sourceType);
		await key.setTitle(stepTitle(settings.step));
		if (settings.step === "toggle" || settings.step === "hide") {
			// Which way it is: on a manual window (Toggle), or the capture
			// shown (Hide).
			const { source, filter } = targetOf(settings);
			const now = await obs.switcherState(source, filter);
			const on = settings.step === "toggle" ? now?.manual === true : now !== undefined && !now.hidden;
			await showOnOff(key, picture, on, settings.palette);
			return;
		}
		// One look for a key that only does something.
		await key.setImage(file(picture, true, settings.palette), { state: 0 });
		await key.setImage(file(picture, true, settings.palette), { state: 1 });
		await key.setState(0);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
