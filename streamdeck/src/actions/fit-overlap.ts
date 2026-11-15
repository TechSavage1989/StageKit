import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { afterPress, targetOf } from "../kinds.ts";
import { obs } from "../obs.ts";
import { file, overlapPicture } from "../pictures.ts";
import { overlapTitle } from "../titles.ts";
import { answerLists, type KeySettings } from "./shared.ts";

/**
 * A key for one setting of a Fit to Space filter: "Overlap only when it
 * fills". On, the fitted item goes under the items never in the way (a
 * camera, say) only while it fills its space, and beside them otherwise.
 * The key shows which way the setting is, also when it is changed in OBS.
 */
export class FitOverlap extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.fitoverlap";

	constructor() {
		super();
		obs.on("settings", () => void this.showAll());
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
		const now = await obs.filter(source, filter);
		if (!now) {
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilterSettings(source, filter, {
				skip_fill_on: afterPress(ev.payload.settings.press, now.settings.skip_fill_on === true),
			});
		} catch {
			await ev.action.showAlert();
		}
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action
			.getSettings()
			.then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_fit_item"));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { source, filter } = targetOf(settings);
		const now = await obs.filter(source, filter);
		const picture = overlapPicture(settings.picture);
		await key.setImage(file(picture, false, settings.palette), { state: 0 });
		await key.setImage(file(picture, true, settings.palette), { state: 1 });
		await key.setState(now?.settings.skip_fill_on === true ? 1 : 0);
		await key.setTitle(overlapTitle(settings.press));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
