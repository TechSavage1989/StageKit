import streamDeck, {
	type DidReceiveSettingsEvent, type KeyAction, type KeyDownEvent, type SendToPluginEvent,
	SingletonAction, type WillAppearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";
import { obs } from "../obs.ts";
import { pictureById } from "../pictures.ts";
import { MARKS, timerPress } from "../timer.ts";
import { answerLists, type KeySettings, showOnOff } from "./shared.ts";

type Settings = KeySettings & { timerPress?: string; timerMark?: string };

export class TimerKey extends SingletonAction<Settings> {
	override readonly manifestId = "com.techsavage1989.stagekit.timer";

	constructor() {
		super();
		obs.on("timer", (data: { timer?: string }) => void this.refresh(data?.timer));
		obs.on("changed", () => void this.refresh());
		obs.on("connection", () => void this.refresh());
	}

	override onWillAppear(ev: WillAppearEvent<Settings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override onDidReceiveSettings(ev: DidReceiveSettingsEvent<Settings>): Promise<void> {
		return ev.action.isKey() ? this.show(ev.action, ev.payload.settings) : Promise.resolve();
	}

	override async onKeyDown(ev: KeyDownEvent<Settings>): Promise<void> {
		const command = timerPress(ev.payload.settings);
		const state = command && await obs.stageKit("TimerControl", command);
		if (!state) {
			await ev.action.showAlert();
			return;
		}
		await ev.action.setState(state.running === true ? 1 : 0);
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, Settings>): Promise<void> {
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event !== "getTimers") {
			await answerLists(ev.payload, await ev.action.getSettings(), () => false, this.manifestId);
			return;
		}
		const reply = await obs.stageKit("ListTimers");
		const items = Array.isArray(reply?.timers)
			? reply.timers.filter((item): item is { label: string; value: string } => !!item && typeof item.label === "string" && typeof item.value === "string")
			: [];
		if (!items.length) items.push({ label: obs.isConnected ? "No StageKit timers available" : "OBS is disconnected", value: "" });
		await streamDeck.ui.sendToPropertyInspector({ event, items });
	}

	private async show(key: KeyAction<Settings>, settings: Settings): Promise<void> {
		const state = settings.target && await obs.stageKit("TimerControl", { timer: settings.target, operation: "state" });
		const picture = pictureById("options/type-countdown");
		const running = !!state && state.running === true;
		if (picture) await showOnOff(key, picture, running, settings);
		else await key.setState(running ? 1 : 0);
		const operation = settings.timerPress ?? "toggle";
		const preset = MARKS.find((m) => m.value === (settings.timerMark ?? "next"));
		const title = operation === "stop" ? "Stop" : operation === "pause" ? "Pause" : operation === "start" ? "Start" : preset?.title ?? "Timer";
		await key.setTitle(title);
	}

	private async refresh(target?: string): Promise<void> {
		for (const key of this.actions) {
			if (!key.isKey()) continue;
			const settings = await key.getSettings();
			if (!target || settings.target === target) await this.show(key, settings);
		}
	}
}
