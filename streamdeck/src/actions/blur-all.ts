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
import { automatic, choose, file, plain } from "../pictures.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * One key for every Ramp Blur at once: a press blurs everything that has a
 * Ramp Blur, the next press clears them all. It shows "on" while any is on.
 */
@action({ UUID: "com.techsavage1989.stagekit.blurall" })
export class BlurAll extends SingletonAction<KeySettings> {
	constructor() {
		super();
		const all = () => void this.showAll();
		obs.on("connection", all);
		obs.on("blur", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const answer = await obs.stageKit("BlurAll", { mode: "toggle" });
		if (!answer) await ev.action.showAlert();
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false));
	}

	/** Whether any Ramp Blur is on right now. */
	private async anyOn(): Promise<boolean> {
		const answer = await obs.stageKit("ListBlurFilters");
		const filters = (answer?.filters ?? []) as Array<{ active?: boolean }>;
		return filters.some((f) => f.active === true);
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const picture = choose(settings.picture, "stagekit_ramp_blur") ?? automatic("stagekit_ramp_blur") ?? plain("switch");
		await key.setImage(file(picture, false, settings.palette), { state: 0 });
		await key.setImage(file(picture, true, settings.palette), { state: 1 });
		await key.setState((await this.anyOn()) ? 1 : 0);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
