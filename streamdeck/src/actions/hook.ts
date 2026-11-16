import streamDeck, {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { obs } from "../obs.ts";
import { file, hookPicture } from "../pictures.ts";
import { hookTitle } from "../titles.ts";
import { answerLists, type KeySettings, remembered } from "./shared.ts";

/**
 * A key that hooks a Game Capture or a Window Capture onto whatever window
 * or game is in front at the moment it is pressed. OBS has no key for that.
 * An Auto Capture on that source is switched off by it, so the capture stays
 * where it was put; a Filter switch key for the Auto Capture hands it back.
 */
export class Hook extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.hook";

	constructor() {
		super();
		const all = () => void this.showAll();
		obs.on("connection", all);
		obs.on("changed", all);
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const { source } = ev.payload.settings;
		// How the capture changes over: a fade unless something else is chosen.
		const { transition, lasting } = ev.payload.settings;
		const ms = Math.max(0, Math.min(10000, Number(lasting ?? 300) || 0));
		const answer = source
			? await obs.stageKit("HookFront", {
					sourceName: source,
					transition: transition || "fade_transition",
					transitionMs: ms,
				})
			: undefined;
		if (answer) await ev.action.showOk();
		else await ev.action.showAlert();
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, () => false));
	}

	/** A game or a window, by the kind of capture the key is for. */
	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		let kind: string | undefined;
		try {
			kind = (await obs.captures()).find((c) => c.name === settings.source)?.kind;
		} catch (error) {
			// OBS went away meanwhile; what the key last learned will do.
			streamDeck.logger.warn(`Capture Active Window: could not ask OBS about '${settings.source ?? ""}': ${String(error)}`);
		}
		// (For the plugin's log, when a key shows the wrong kind: what OBS
		// said, or that it said nothing.)
		if (settings.source && !kind)
			streamDeck.logger.info(
				`Capture Active Window: OBS ${obs.isConnected ? "has no Game or Window Capture named" : "is not connected, so nothing is known of"} '${settings.source}'`,
			);
		// (While OBS cannot say which kind of capture it is, the key goes by
		// what it last learned: it used to turn into "Capture Game".)
		const seen = await remembered(key, settings, settings.source ?? "", kind ? { kind } : undefined);
		await key.setImage(file(hookPicture(settings.picture, seen?.kind), true, settings.palette));
		await key.setTitle(hookTitle(seen?.kind));
	}

	private async showAll(): Promise<void> {
		for (const key of this.actions) {
			if (key.isKey()) await this.show(key, await key.getSettings());
		}
	}
}
