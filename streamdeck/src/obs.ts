// The link to OBS, through OBS's own WebSocket server (Tools > WebSocket
// Server Settings in OBS). One connection for every key. It is kept up: when
// OBS is closed or not started yet, it tries again every few seconds.

import { EventEmitter } from "node:events";

import streamDeck from "@elgato/streamdeck";
import OBSWebSocket from "obs-websocket-js/json";

import { type FilterInfo, isStageKit } from "./kinds.ts";

export type Connection = {
	host?: string;
	port?: string | number;
	password?: string;
};

export type FilterNow = { enabled: boolean; kind: string; settings: Record<string, unknown> };

const RETRY_MS = 3000;

class ObsLink extends EventEmitter {
	private obs = new OBSWebSocket();
	private settings: Connection = {};
	private wanted = false;
	private connected = false;
	private connecting = false;
	private timer: NodeJS.Timeout | undefined;
	/** Why the last try failed, for a property inspector to show. */
	lastError = "";

	constructor() {
		super();
		this.obs.on("ConnectionClosed", () => {
			const was = this.connected;
			this.connected = false;
			if (was) {
				streamDeck.logger.info("OBS: connection closed");
				this.emit("connection", false);
			}
			this.retryLater();
		});
		this.obs.on("SourceFilterEnableStateChanged", (ev) => {
			this.emit("filter", ev.sourceName, ev.filterName, ev.filterEnabled);
		});
		for (const name of [
			"SourceFilterCreated",
			"SourceFilterRemoved",
			"SourceFilterNameChanged",
			"InputNameChanged",
			"SceneNameChanged",
		] as const) {
			this.obs.on(name, () => this.emit("changed"));
		}
		this.obs.on("VendorEvent", (ev) => {
			if (ev.vendorName === "StageKit" && ev.eventType === "BlurChanged") this.emit("blur");
			if (ev.vendorName === "StageKit" && ev.eventType === "OptionsChanged") this.emit("options");
		});
	}

	get isConnected(): boolean {
		return this.connected;
	}

	/** Starts (or restarts, with other settings) the connection. */
	use(settings: Connection): void {
		const same =
			this.wanted &&
			(settings.host ?? "") === (this.settings.host ?? "") &&
			String(settings.port ?? "") === String(this.settings.port ?? "") &&
			(settings.password ?? "") === (this.settings.password ?? "");
		this.settings = settings;
		this.wanted = true;
		if (same && (this.connected || this.connecting)) return;
		void this.reconnect();
	}

	private async reconnect(): Promise<void> {
		if (this.connected) {
			try {
				await this.obs.disconnect();
			} catch {
				// Already gone.
			}
		}
		await this.connect();
	}

	private retryLater(): void {
		if (!this.wanted || this.timer) return;
		this.timer = setTimeout(() => {
			this.timer = undefined;
			void this.connect();
		}, RETRY_MS);
	}

	private async connect(): Promise<void> {
		if (this.connecting || this.connected || !this.wanted) return;
		this.connecting = true;
		const host = (this.settings.host || "127.0.0.1").trim();
		const port = Number(this.settings.port) || 4455;
		try {
			await this.obs.connect(`ws://${host}:${port}`, this.settings.password || undefined);
			this.connected = true;
			this.lastError = "";
			streamDeck.logger.info(`OBS: connected to ${host}:${port}`);
			this.emit("connection", true);
		} catch (error) {
			const message = error instanceof Error ? error.message : String(error);
			if (message !== this.lastError) streamDeck.logger.warn(`OBS: cannot connect to ${host}:${port}: ${message}`);
			this.lastError = message || "no answer";
			this.retryLater();
		} finally {
			this.connecting = false;
		}
	}

	/** Every StageKit filter in OBS: on inputs, scenes and groups. */
	async stageKitFilters(): Promise<FilterInfo[]> {
		if (!this.connected) return [];
		const [inputs, scenes, groups] = await Promise.all([
			this.obs.call("GetInputList"),
			this.obs.call("GetSceneList"),
			this.obs.call("GetGroupList"),
		]);
		const names = new Set<string>();
		for (const input of inputs.inputs) names.add(String(input.inputName));
		for (const scene of scenes.scenes) names.add(String(scene.sceneName));
		for (const group of groups.groups) names.add(String(group));
		const sources = [...names];
		if (sources.length === 0) return [];
		const answers = await this.obs.callBatch(
			sources.map((sourceName) => ({ requestType: "GetSourceFilterList" as const, requestData: { sourceName } })),
		);
		const out: FilterInfo[] = [];
		answers.forEach((answer, index) => {
			const list = (answer.responseData as { filters?: Array<Record<string, unknown>> } | undefined)?.filters;
			if (!answer.requestStatus.result || !list) return;
			for (const filter of list) {
				const kind = String(filter.filterKind ?? "");
				if (!isStageKit(kind)) continue;
				out.push({
					source: sources[index],
					name: String(filter.filterName ?? ""),
					kind,
					enabled: filter.filterEnabled === true,
				});
			}
		});
		return out;
	}

	/**
	 * One filter as it is now: on or off, what kind it is, and its settings
	 * (OBS leaves out the ones still at their usual value). Undefined when
	 * it, or OBS, cannot be reached.
	 */
	async filter(sourceName: string, filterName: string): Promise<FilterNow | undefined> {
		if (!this.connected || !sourceName || !filterName) return undefined;
		try {
			const answer = await this.obs.call("GetSourceFilter", { sourceName, filterName });
			return {
				enabled: answer.filterEnabled,
				kind: answer.filterKind,
				settings: (answer.filterSettings ?? {}) as Record<string, unknown>,
			};
		} catch {
			return undefined;
		}
	}

	/** The Game Captures and Window Captures in OBS: the sources a Hook key can aim. */
	async captures(): Promise<Array<{ name: string; kind: string }>> {
		if (!this.connected) return [];
		const answer = await this.obs.call("GetInputList");
		return answer.inputs
			.map((input) => ({
				name: String(input.inputName),
				kind: String(input.unversionedInputKind ?? input.inputKind ?? ""),
			}))
			.filter((input) => input.kind === "game_capture" || input.kind === "window_capture")
			.sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));
	}

	/** The kinds of transition OBS has ("fade_transition", "swipe_transition" and so on). */
	async transitionKinds(): Promise<string[]> {
		if (!this.connected) return [];
		const answer = await this.obs.call("GetTransitionKindList");
		return answer.transitionKinds.map((kind) => String(kind));
	}

	/** Whether a filter is on; undefined when it (or OBS) cannot be reached. */
	async filterEnabled(sourceName: string, filterName: string): Promise<boolean | undefined> {
		return (await this.filter(sourceName, filterName))?.enabled;
	}

	/** Presses one of OBS's hotkeys by its name (StageKit's arrange tools have one each). */
	async pressHotkey(hotkeyName: string): Promise<void> {
		await this.obs.call("TriggerHotkeyByName", { hotkeyName });
	}

	/**
	 * Asks the StageKit plugin in OBS for something (its own requests, see
	 * "Remote control" in the README). Undefined when it cannot be reached
	 * or says no.
	 */
	async stageKit(requestType: string, requestData: Record<string, unknown> = {}): Promise<Record<string, unknown> | undefined> {
		if (!this.connected) return undefined;
		try {
			const answer = await this.obs.call("CallVendorRequest", {
				vendorName: "StageKit",
				requestType,
				requestData: requestData as never,
			});
			const data = (answer.responseData ?? {}) as Record<string, unknown>;
			return data.success === false ? undefined : data;
		} catch {
			return undefined;
		}
	}

	async setFilter(sourceName: string, filterName: string, filterEnabled: boolean): Promise<void> {
		await this.obs.call("SetSourceFilterEnabled", { sourceName, filterName, filterEnabled });
	}
}

export const obs = new ObsLink();
