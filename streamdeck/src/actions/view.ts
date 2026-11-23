import streamDeck, {
	type DidReceiveSettingsEvent,
	type KeyAction,
	type KeyDownEvent,
	type SendToPluginEvent,
	SingletonAction,
	type WillAppearEvent,
	type WillDisappearEvent,
} from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";

import { obs } from "../obs.ts";
import { viewShown } from "../pictures.ts";
import { viewTitle } from "../titles.ts";
import { canStepView, changedViewSettings, viewBinding, viewState } from "../views.ts";
import { answerLists, imageOf, type KeySettings } from "./shared.ts";

type VisibleKey = { key: KeyAction<KeySettings>; settings: KeySettings; revision: number };

/** One native saved choice, or the native next choice. No source actions are copied onto keys. */
export class ViewKey extends SingletonAction<KeySettings> {
	override readonly manifestId = "com.techsavage1989.stagekit.view";
	private readonly visible = new Map<string, VisibleKey>();
	private readonly pending = new Set<string>();

	constructor() {
		super();
		obs.on("scenecontroller", (event: { controllerId?: string } | undefined) => void this.refresh(event?.controllerId));
		for (const event of ["connection", "changed", "filter"]) obs.on(event, () => void this.refresh());
	}

	override async onWillAppear(ev: WillAppearEvent<KeySettings>): Promise<void> {
		if (!ev.action.isKey()) return;
		const entry = { key: ev.action, settings: ev.payload.settings, revision: 0 };
		this.visible.set(ev.action.id, entry);
		await this.show(entry);
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<KeySettings>): Promise<void> {
		if (!ev.action.isKey()) return;
		const previous = this.visible.get(ev.action.id);
		const settings = changedViewSettings(previous?.settings, ev.payload.settings);
		const entry = previous ?? { key: ev.action, settings, revision: 0 };
		entry.settings = settings;
		entry.revision++;
		this.visible.set(ev.action.id, entry);
		if (settings !== ev.payload.settings) await ev.action.setSettings(settings);
		await this.show(entry);
	}

	override onWillDisappear(ev: WillDisappearEvent<KeySettings>): void {
		this.visible.delete(ev.action.id);
	}

	override async onKeyDown(ev: KeyDownEvent<KeySettings>): Promise<void> {
		if (this.pending.has(ev.action.id)) return;
		this.pending.add(ev.action.id);
		try {
			const settings = ev.payload.settings;
			const controller = settings.controllerId ? await obs.sceneController(settings.controllerId) : undefined;
			const state = viewState(settings, controller, obs.isConnected);
			const current = await ev.action.getSettings();
			if (!state.command || viewBinding(current) !== viewBinding(settings) || !await obs.sceneControllerCommand(state.command))
				await ev.action.showAlert();
		} catch {
			await ev.action.showAlert();
		} finally {
			this.pending.delete(ev.action.id);
			const entry = this.visible.get(ev.action.id);
			if (entry) await this.show(entry);
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<JsonValue, KeySettings>): Promise<void> {
		const settings = await ev.action.getSettings();
		const event = (ev.payload as { event?: string } | null)?.event;
		if (event === "getSceneControllers") {
			const items = (await obs.sceneControllers()).map((controller) => ({
				value: controller.controllerId,
				label: `${controller.scene} / ${controller.name}${!controller.enabled ? " (disabled)" : !controller.available ? " (unavailable)" : ""}`,
			}));
			await streamDeck.ui.sendToPropertyInspector({ event, items });
			return;
		}
		if (event && ["getViewGroups", "getViewChoices", "getSavedViews", "getViewCommands", "getViewStatus"].includes(event)) {
			const controller = settings.controllerId ? await obs.sceneController(settings.controllerId) : undefined;
			const group = controller?.groups.find((entry) => entry.id === settings.groupId);
			let items: Array<{ value: string; label: string }> = [];
			const named = (entry: { id: string; name: string; available: boolean; enabled?: boolean }) => ({
				value: entry.id, label: `${entry.name || "Unnamed"}${entry.enabled === false ? " (disabled)" : !entry.available ? " (unavailable)" : ""}`,
			});
			if (event === "getViewGroups") items = controller?.groups.filter((entry) => entry.kind !== "follow").map(named) ?? [];
			if (event === "getViewChoices") items = group?.choices.map(named) ?? [];
			if (event === "getSavedViews") items = controller?.views.map(named) ?? [];
			if (event === "getViewCommands") {
				items = [{ value: "select", label: "Select" }];
				if (settings.viewTarget !== "view" && canStepView(group)) items.push({ value: "next", label: "Next choice" });
			}
			if (event === "getViewStatus") {
				const state = viewState(settings, controller, obs.isConnected);
				await streamDeck.ui.sendToPropertyInspector({ event, text: state.reason || "Uses the saved choices in OBS. Configure groups and views in the StageKit Views dock." });
			} else await streamDeck.ui.sendToPropertyInspector({ event, items });
			return;
		}
		await answerLists(ev.payload, settings, (kind) => kind === "stagekit_scene_controller", this.manifestId);
	}

	private async refresh(controllerId?: string): Promise<void> {
		// Share only this event's reads. A newer native event or connection
		// starts another batch, even while an older response is still pending.
		const requests = new Map<string, ReturnType<typeof obs.sceneController>>();
		await Promise.all([...this.visible.values()].filter((entry) => !controllerId || entry.settings.controllerId === controllerId).map((entry) => {
			const id = entry.settings.controllerId;
			let request = id ? requests.get(id) : undefined;
			if (id && !request) {
				request = obs.sceneController(id);
				requests.set(id, request);
			}
			return this.show(entry, request);
		}));
	}

	private async show(entry: VisibleKey, request?: ReturnType<typeof obs.sceneController>): Promise<void> {
		const revision = ++entry.revision;
		const settings = entry.settings;
		const current = () => this.visible.get(entry.key.id) === entry && entry.revision === revision;
		try {
			const controller = await (request ?? (settings.controllerId ? obs.sceneController(settings.controllerId) : undefined));
			if (!current()) return;
			const state = viewState(settings, controller, obs.isConnected);
			const picture = viewShown(settings.sourceType, state.type);
			await entry.key.setImage(imageOf(picture, settings, false), { state: 0 });
			if (!current()) return;
			await entry.key.setImage(imageOf(picture, settings, true), { state: 1 });
			if (!current()) return;
			await entry.key.setState(state.active ? 1 : 0);
			if (!current()) return;
			await entry.key.setTitle(state.status === "ready" ? viewTitle(state.name) : state.name);
		} catch (error) {
			streamDeck.logger.warn(`Could not update a View key: ${String(error)}`);
		}
	}
}
