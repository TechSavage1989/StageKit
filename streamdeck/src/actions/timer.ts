import streamDeck, {
	type DidReceiveSettingsEvent, type KeyAction, type KeyDownEvent, type SendToPluginEvent,
	SingletonAction, type WillAppearEvent, type WillDisappearEvent, type KeyUpEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";
import { obs } from "../obs.ts";
import { HoldPress } from "../hold.ts";
import { timerPicture } from "../pictures.ts";
import { MARKS, timerPress } from "../timer.ts";
import { timerCycleTitle } from "../titles.ts";
import { answerLists, type KeySettings, showOnOff } from "./shared.ts";

type Settings = KeySettings & { timerPress?: string; timerMark?: string };

export class TimerKey extends SingletonAction<Settings> {
	override readonly manifestId = "com.techsavage1989.stagekit.timer";
	private readonly presses = new HoldPress();

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
		if (!ev.action.isKey()) return;
		const key = ev.action;
		const settings = ev.payload.settings;
		await this.presses.down(key.id, {
			holdMs: 800,
			long: settings.timerPress === "cycle",
			short: () => this.send(key, settings, false),
			longPress: () => this.send(key, settings, true),
		});
	}

	override onKeyUp(ev: KeyUpEvent<Settings>): void {
		this.presses.up(ev.action.id);
	}

	override onWillDisappear(ev: WillDisappearEvent<Settings>): void {
		this.presses.cancel(ev.action.id);
	}

	private async send(key: KeyAction<Settings>, settings: Settings, held: boolean): Promise<void> {
		const command = timerPress(settings, held);
		const state = command && await obs.stageKit("TimerControl", command);
		if (!state) {
			await key.showAlert();
			return;
		}
		await key.setState(state.running === true ? 1 : 0);
		if (settings.timerPress === "cycle") await key.setTitle(timerCycleTitle(state));
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
		const state = settings.target ? await obs.stageKit("TimerControl", { timer: settings.target, operation: "state" }) : undefined;
		const picture = timerPicture(settings.sourceType);
		const running = !!state && state.running === true;
		if (picture) await showOnOff(key, picture, running, settings);
		else await key.setState(running ? 1 : 0);
		const operation = settings.timerPress ?? "toggle";
		const preset = MARKS.find((m) => m.value === (settings.timerMark ?? "next"));
		const title = operation === "cycle" ? timerCycleTitle(state) : operation === "stop" ? "Stop" : operation === "pause" ? "Pause" : operation === "start" ? "Start" : preset?.title ?? "Timer";
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
