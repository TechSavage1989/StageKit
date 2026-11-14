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

import { afterPress, isSwitch } from "../kinds.ts";
import { obs } from "../obs.ts";
import { choose, file } from "../pictures.ts";
import { answerLists, type KeySettings, look } from "./shared.ts";

/**
 * A key that switches one StageKit filter on and off and shows which way it
 * is: Ramp Blur, Shadow, Glow & Outline, Corners & Edges, Show in Scenes,
 * Standby Picture and the rest.
 */
@action({ UUID: "com.techsavage1989.stagekit.switch" })
export class FilterSwitch extends SingletonAction<KeySettings> {
	constructor() {
		super();
		// OBS says when a filter is switched, by this key or any other way.
		obs.on("filter", (source: string, filter: string, enabled: boolean) => {
			for (const key of this.actions) {
				if (!key.isKey()) continue;
				void key.getSettings().then((settings) => {
					if (settings.source === source && settings.filter === filter) void key.setState(enabled ? 1 : 0);
				});
			}
		});
		obs.on("connection", () => void this.showAll());
		obs.on("changed", () => void this.showAll());
		look.on("changed", () => void this.showAll());
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source, filter, press } = ev.payload.settings;
		const now = await obs.filterEnabled(source ?? "", filter ?? "");
		if (now === undefined) {
			// Not set up, OBS closed, or the filter is gone.
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilter(source!, filter!, afterPress(press, now));
		} catch {
			await ev.action.showAlert();
		}
		// Stream Deck flips a two-state key by itself on every press; put it
		// back to what the filter really is.
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, isSwitch));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const now = await obs.filter(settings.source ?? "", settings.filter ?? "");
		// Its two looks: the picture that fits the filter (or the one chosen),
		// in its "off" and "on" form. Without one, the plugin's plain switch.
		const picture = choose(settings.picture, now?.kind ?? "", now?.settings);
		await key.setImage(picture ? file(picture, false, look.colour) : undefined, { state: 0 });
		await key.setImage(picture ? file(picture, true, look.colour) : undefined, { state: 1 });
		await key.setState(now?.enabled ? 1 : 0);
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
