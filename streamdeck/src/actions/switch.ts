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
import { chooseFor, plain } from "../pictures.ts";
import { isSourceType } from "../source-types.ts";
import { switchTitle } from "../titles.ts";
import { answerLists, type KeySettings, remembered, showOnOff } from "./shared.ts";

/**
 * A key that switches one StageKit filter on and off and shows which way it
 * is. There is one of these for each feature (Ramp Blur, Shadow, Auto
 * Capture and so on), each offering only that feature's filters; `wanted`
 * says which filter kinds this one is for.
 */
export class FilterSwitch extends SingletonAction<KeySettings> {
	override readonly manifestId: string;

	constructor(
		uuid: string,
		private readonly wanted: (kind: string) => boolean,
	) {
		super();
		this.manifestId = uuid;
		// OBS says when a filter is switched, by this key or any other way.
		obs.on("filter", (source: string, filter: string, enabled: boolean) => {
			for (const key of this.actions) {
				if (!key.isKey()) continue;
				void key.getSettings().then((settings) => {
					const mine = targetOf(settings);
					if (mine.source === source && mine.filter === filter) void key.setState(enabled ? 1 : 0);
				});
			}
		});
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
		const { press } = ev.payload.settings;
		const now = await obs.filterEnabled(source, filter);
		if (now === undefined) {
			// Not set up, OBS closed, or the filter is gone.
			await ev.action.showAlert();
			return;
		}
		try {
			await obs.setFilter(source, filter, afterPress(press, now));
		} catch {
			await ev.action.showAlert();
		}
		// Stream Deck flips a two-state key by itself on every press; put it
		// back to what the filter really is.
		await this.show(ev.action, ev.payload.settings);
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, this.wanted));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const { source, filter } = targetOf(settings);
		const now = await obs.filter(source, filter);
		// Its two looks: the picture that fits the filter (or the one chosen),
		// in its "off" and "on" form, in this key's palette.
		// (And the source it is on: the pack's own picture of that source,
		// or, for a key that shows and hides a source, what that source is.)
		// (While OBS cannot say, the key goes by what it last learned.)
		// (The kind of source it is on matters only for a key that shows and
		// hides it, left on Automatic; kept with the rest, for while OBS
		// cannot be asked.)
		const kindNow = now?.kind ?? (settings.seen as { kind?: string } | undefined)?.kind ?? "";
		const wantsInput = kindNow === "stagekit_show_in_scenes" && !settings.picture && !isSourceType(settings.sourceType);
		const inputKind = now && wantsInput ? await obs.inputKind(source) : undefined;
		const seen = await remembered(key, settings, `${source}\n${filter}`, now ? { ...now, inputKind } : undefined);
		const kind = seen?.kind ?? "";
		const picture =
			chooseFor(settings.picture, kind, seen?.settings, source, seen?.inputKind, settings.sourceType) ?? plain("switch");
		await showOnOff(key, picture, seen?.enabled === true, settings.palette);
		await key.setTitle(switchTitle(seen?.kind, settings.press));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
