import {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { type AutoFrameCommandFeature, runAutoFrameCommand } from "../auto-frame.ts";
import { keptPicture } from "../key-types.ts";
import { targetOf } from "../kinds.ts";
import { obs } from "../obs.ts";
import { autoFrameCommandShown, shownId } from "../pictures.ts";
import { answerLists, imageOf, type KeySettings, settle } from "./shared.ts";

export class AutoFrameCommand extends SingletonAction<KeySettings> {
	override readonly manifestId: string;
	private readonly pending = new Set<string>();
	private readonly feature: AutoFrameCommandFeature;

	constructor(feature: AutoFrameCommandFeature) {
		super();
		this.feature = feature;
		this.manifestId = "com.techsavage1989.stagekit." + feature.key;
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		if (this.pending.has(ev.action.id)) return;
		this.pending.add(ev.action.id);
		const { source, filter } = targetOf(ev.payload.settings);
		try {
			await ev.action.setTitle(this.feature.pendingTitle);
			const result = await runAutoFrameCommand((name, data) => obs.stageKit(name, data), source, filter, this.feature.action);
			if (result) await ev.action.showOk();
			else await ev.action.showAlert();
		} catch {
			await ev.action.showAlert();
		} finally {
			this.pending.delete(ev.action.id);
			await ev.action.setTitle(this.feature.title);
		}
	}

	override onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		return ev.action.getSettings().then((settings) => answerLists(ev.payload, settings, (kind) => kind === "stagekit_auto_frame", this.manifestId));
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		const pick = (s: KeySettings) => autoFrameCommandShown(keptPicture(s), this.feature.image, s.sourceType);
		settings = await settle(key, this.feature.key, settings, (s) => shownId(pick(s)));
		await key.setImage(imageOf(pick(settings), settings));
		await key.setTitle(this.pending.has(key.id) ? this.feature.pendingTitle : this.feature.title);
	}
}
