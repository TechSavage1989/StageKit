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

	/** Whether a filter is on; undefined when it (or OBS) cannot be reached. */
	async filterEnabled(sourceName: string, filterName: string): Promise<boolean | undefined> {
		if (!this.connected || !sourceName || !filterName) return undefined;
		try {
			const answer = await this.obs.call("GetSourceFilter", { sourceName, filterName });
			return answer.filterEnabled;
		} catch {
			return undefined;
		}
	}

	async setFilter(sourceName: string, filterName: string, filterEnabled: boolean): Promise<void> {
		await this.obs.call("SetSourceFilterEnabled", { sourceName, filterName, filterEnabled });
	}
}

export const obs = new ObsLink();
