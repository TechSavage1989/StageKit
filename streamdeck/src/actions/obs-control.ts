import streamDeck, {
	type DidReceiveSettingsEvent, type KeyAction, type KeyDownEvent, type KeyUpEvent, type SendToPluginEvent,
	SingletonAction, type WillAppearEvent, type WillDisappearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";
import { afterPress, targetOf } from "../kinds.ts";
import { HoldPress } from "../hold.ts";
import { holdMs } from "../live.ts";
import { obs } from "../obs.ts";
import { ordinaryControlPicture, studioModeShown } from "../pictures.ts";
import { answerLists, imageOf, remembered, showOnOff, type KeySettings } from "./shared.ts";

export class ObsControl extends SingletonAction<KeySettings> {
	override readonly manifestId: string;
	readonly key: string;
	readonly moveKind: string;
	private readonly presses = new HoldPress();
	constructor(key: string, moveKind = "") {
		super();
		this.key = key;
		this.moveKind = moveKind;
		this.manifestId = "com.techsavage1989.stagekit." + key;
		if (moveKind) return;
		obs.on("ordinary", (changed: string) => { if (changed === key) void this.refresh(); });
		obs.on("changed", () => void this.refresh());
		obs.on("connection", () => void this.refresh());
	}

	override onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onWillDisappear(ev: WillDisappearEvent<KeySettings>): void {
		this.presses.cancel(ev.action.id);
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		const settings = ev.payload.settings;
		if (this.key !== "obsstudio") {
			await this.press(ev.action, settings);
			return;
		}
		if (!ev.action.isKey()) return;
		const key = ev.action;
		// A hold must choose the transition before any short-press mode change.
		const long = settings.longPress === undefined || settings.longPress === "" || settings.longPress === "transition";
		await this.presses.down(key.id, {
			holdMs: holdMs(settings.holdMs),
			long,
			short: () => this.press(key, settings),
			longPress: () => this.transition(key),
		});
	}

	override onKeyUp(ev: KeyUpEvent<KeySettings>): void {
		if (this.key === "obsstudio") this.presses.up(ev.action.id);
	}

	private async transition(key: KeyAction<KeySettings>): Promise<void> {
		try {
			await obs.triggerStudioModeTransition();
			await key.showOk();
		} catch {
			await key.showAlert();
		}
	}

	private async press(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		try {
			if (this.moveKind) {
				const { source, filter } = targetOf(settings);
				if ((await obs.filter(source, filter))?.kind !== this.moveKind) throw new Error("Choose a Move filter");
				await obs.pressHotkey(filter, source);
			} else {
				const target = settings.target ?? "";
				const before = await obs.controlState(this.key, target);
				const result = await obs.controlState(this.key, target, afterPress(settings.press, before));
				if (this.key === "obsstudio")
					await remembered(key, settings, this.key, { kind: this.key, enabled: result });
				await key.setState(result ? 1 : 0);
			}
		} catch {
			await key.showAlert();
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event !== "getOrdinaryTargets") {
			await answerLists(ev.payload, await ev.action.getSettings(), (kind) => kind === this.moveKind, this.manifestId);
			return;
		}
		let items: Array<{ label: string; value: string }> = [];
		try {
			if (this.key === "obssource") items = await obs.sceneItemTargets();
			if (this.key === "obsmute") items = (await obs.mixerInputs()).map((input) => ({ label: input, value: input }));
		} catch {
			streamDeck.logger.warn("Could not list targets for " + this.key);
		}
		if (!items.length) items = [{ label: obs.isConnected ? "No matching sources" : "OBS is disconnected", value: "" }];
		await streamDeck.ui.sendToPropertyInspector({ event, items });
	}

	private async show(key: KeyAction<KeySettings>, settings: KeySettings): Promise<void> {
		if (this.moveKind) {
			const { source } = targetOf(settings);
			const image = ordinaryControlPicture(this.key, source, settings.sourceType, settings.sceneType);
			await key.setImage(imageOf(image, settings, true));
			return;
		}
		if (this.key === "obsstudio") {
			let active: boolean | undefined;
			try { active = await obs.controlState(this.key, ""); } catch { /* Keep OBS's last reported state. */ }
			const seen = await remembered(key, settings, this.key, active === undefined ? undefined : { kind: this.key, enabled: active });
			await showOnOff(key, studioModeShown(true, settings.sceneType), seen?.enabled === true, settings, studioModeShown(false, settings.sceneType));
			return;
		}
		try {
			const active = await obs.controlState(this.key, settings.target ?? "");
			const picture = ordinaryControlPicture(this.key, undefined, settings.sourceType, settings.sceneType);
			await showOnOff(key, picture, active, settings);
		} catch {
			await key.setState(0);
		}
	}

	private async refresh(): Promise<void> {
		for (const key of this.actions)
			if (key.isKey()) await this.show(key, await key.getSettings());
	}
}
