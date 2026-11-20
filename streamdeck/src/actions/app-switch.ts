import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { switcherStep, targetOf } from "../kinds.ts";
import { obs } from "../obs.ts";
import { shownId, stepShown } from "../pictures.ts";
import { stepTitle } from "../titles.ts";
import { keptPicture } from "../key-types.ts";
import { answerLists, imageOf, type KeySettings, settle, showOnOff } from "./shared.ts";

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
		const { source, filter } = targetOf(ev.payload.settings);
		const target = await obs.filter(source, filter);
		const step = switcherStep(target?.kind, ev.payload.settings.step);
		if (!step) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.pressHotkey(`stagekit.active.${step}.${filter}`, source);
			// (A key showing which way it is says so by its picture.)
			if (step !== "toggle" && step !== "hide") await ev.action.showOk();
		} catch {
			await ev.action.showAlert();
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action
			.getSettings()
			.then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_active_app" || kind === "stagekit_app_capture" || kind === "stagekit_auto_capture", this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { source, filter } = targetOf(settings);
		const target = await obs.filter(source, filter);
		const step = switcherStep(target?.kind, settings.step) ?? settings.step;
		const pick = (s: KeySettings) => stepShown(keptPicture(s), step, s.sourceType);
		settings = await settle(key, "appswitch", settings, (s) => shownId(pick(s)));
		const picture = pick(settings);
		await key.setTitle(stepTitle(step));
		if (step === "toggle" || step === "hide") {
			// Which way it is: on a manual window (Toggle), or the capture
			// shown (Hide).
			const now = await obs.switcherState(source, filter);
			const on = step === "toggle" ? now?.manual === true : now !== undefined && !now.hidden;
			await showOnOff(key, picture, on, settings);
			return;
		}
		// One look for a key that only does something.
		await key.setImage(imageOf(picture, settings), { state: 0 });
		await key.setImage(imageOf(picture, settings), { state: 1 });
		await key.setState(0);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
