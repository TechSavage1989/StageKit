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
import { keptPicture } from "../key-types.ts";
import { blurAllShown, shownId } from "../pictures.ts";
import { blurAllTitle } from "../titles.ts";
import { answerLists, type KeySettings, settle, showOnOff } from "./shared.ts";

/**
 * One key for every Ramp Blur at once: a press blurs everything that has a
 * Ramp Blur, the next press clears them all. It shows "on" while any is on.
 */
export class BlurAll extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.blurall";

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
		const { press } = ev.payload.settings;
		const answer = await obs.stageKit("BlurAll", { mode: press === "on" || press === "off" ? press : "toggle" });
		if (!answer) await ev.action.showAlert();
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false, this.manifestId));
	}

	/** Whether any Ramp Blur is on right now. */
	private async anyOn(): Promise<boolean> {
		const answer = await obs.stageKit("ListBlurFilters");
		const filters = (answer?.filters ?? []) as Array<{ active?: boolean }>;
		return filters.some((f) => f.active === true);
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => blurAllShown(keptPicture(s), s.look, s.sceneType);
		settings = await settle(key, "blurall", settings, (s) => shownId(pick(s)));
		const picture = pick(settings);
		await showOnOff(key, picture, await this.anyOn(), settings);
		await key.setTitle(blurAllTitle(settings.press));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
