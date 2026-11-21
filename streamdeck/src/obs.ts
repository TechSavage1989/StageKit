// The link to OBS, through OBS's own WebSocket server (Tools > WebSocket
// Server Settings in OBS). One connection for every key. It is kept up: when
// OBS is closed or not started yet, it tries again every few seconds.

import { EventEmitter } from "node:events";

import streamDeck from "@elgato/streamdeck";
import OBSWebSocket from "obs-websocket-js/json";

import { decodeItem } from "./scene-item-target.ts";
import { type FilterInfo, isStageKit } from "./kinds.ts";
import { readSceneBlurs, type SceneBlurFilter } from "./scene-blurs.ts";

export type Connection = {
	host?: string;
	port?: string | number;
	password?: string;
};

export type FilterNow = { enabled: boolean; kind: string; settings: Record<string, unknown> };

/** A mode of a Scene Modes filter: its id (what keys keep), its name and its type. */
export type SceneMode = { id: string; name: string; type: string };
export type SceneModes = { scene: string; filter: string; enabled: boolean; current: string; modes: SceneMode[] };

/** The modes as the vendor requests give them: { id, name, type } each (a bare name is taken as both id and name). */
function modeList(modes: unknown): SceneMode[] {
	if (!Array.isArray(modes)) return [];
	return modes
		.map((m) =>
			typeof m === "string"
				? { id: m, name: m, type: "" }
				: { id: String((m as Record<string, unknown>)?.id ?? ""), name: String((m as Record<string, unknown>)?.name ?? ""), type: String((m as Record<string, unknown>)?.type ?? "") },
		)
		.filter((m) => m.id !== "");
}

const RETRY_MS = 3000;

class ObsLink extends EventEmitter {
	private obs = new OBSWebSocket();
	private settings: Connection = {};
	private wanted = false;
	private connected = false;
	private connecting = false;
	private timer: NodeJS.Timeout | undefined;
	private sceneSwitches: Promise<void> = Promise.resolve();
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
		// Sources coming and going, and another scene collection: while OBS
		// loads there are a great many of these, so they are told on as one.
		for (const name of ["InputCreated", "InputRemoved", "CurrentSceneCollectionChanged", "SceneListChanged"] as const) {
			this.obs.on(name, () => this.changedSoon(500));
		}
		// The scene on the program or the preview changed, or Studio Mode.
		for (const name of ["CurrentProgramSceneChanged", "CurrentPreviewSceneChanged", "StudioModeStateChanged"] as const) {
			this.obs.on(name, () => this.emit("scene"));
		}
		// The stream or the recording started, stopped, paused ... (the Stream
		// and Record keys; obs-websocket's own events, nothing of StageKit's).
		this.obs.on("SceneItemEnableStateChanged", () => this.emit("ordinary", "obssource"));
		this.obs.on("InputMuteStateChanged", () => this.emit("ordinary", "obsmute"));
		this.obs.on("StudioModeStateChanged", () => this.emit("ordinary", "obsstudio"));
		this.obs.on("VirtualcamStateChanged", () => this.emit("ordinary", "obscamera"));
		this.obs.on("StreamStateChanged", (ev) => this.emit("output", "stream", ev.outputState, ev.outputActive));
		this.obs.on("RecordStateChanged", (ev) => this.emit("output", "record", ev.outputState, ev.outputActive));
		// A filter's settings changed (in OBS's window, or by a key).
		this.obs.on("SourceFilterSettingsChanged", (ev) => {
			this.emit("settings", ev.sourceName, ev.filterName);
		});
		this.obs.on("VendorEvent", (ev) => {
			if (ev.vendorName === "StageKit" && ev.eventType === "BlurChanged") this.emit("blur");
			if (ev.vendorName === "StageKit" && ev.eventType === "OptionsChanged") this.emit("options");
			if (ev.vendorName === "StageKit" && ev.eventType === "SwitcherChanged") this.emit("switcher");
			// A Scene Modes filter changed its mode, or its list of modes.
			if (ev.vendorName === "StageKit" && ev.eventType === "SceneModeChanged") this.emit("mode", ev.eventData);
			// A Scene Blur was switched on or off.
			if (ev.vendorName === "StageKit" && ev.eventType === "SceneBlurChanged") this.emit("sceneblur", ev.eventData);
			// A Standby Image was shown over its source on purpose, or taken away.
			if (ev.vendorName === "StageKit" && ev.eventType === "StandbyCoverChanged") this.emit("standbycover", ev.eventData);
		});
	}

	get isConnected(): boolean {
		return this.connected;
	}

	private soon = new Map<number, ReturnType<typeof setTimeout>>();

	/** Says "changed" once, a little later, however often it is asked meanwhile. */
	private changedSoon(ms: number): void {
		if (this.soon.has(ms)) return;
		this.soon.set(
			ms,
			setTimeout(() => {
				this.soon.delete(ms);
				if (this.connected) this.emit("changed");
			}, ms),
		);
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
			// OBS answers before it has loaded its scenes: asked then, a key
			// finds no source and no filter. The keys look again shortly
			// after, and again once OBS has surely finished.
			for (const ms of [2000, 6000, 15000]) this.changedSoon(ms);
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
	async stageKitFilters(includeOther = false): Promise<FilterInfo[]> {
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
				if (!includeOther && !isStageKit(kind)) continue;
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

	/**
	 * What a source is in OBS ("dshow_input", "window_capture" ...). A Cropped
	 * View is what it shows: a camera's crop is a camera. Undefined for a
	 * scene, a group, or when OBS cannot be asked.
	 */
	async inputKind(sourceName: string): Promise<string | undefined> {
		if (!this.connected || !sourceName) return undefined;
		try {
			const answer = await this.obs.call("GetInputList");
			const kinds = new Map(
				answer.inputs.map((input) => [String(input.inputName), String(input.unversionedInputKind ?? input.inputKind ?? "")]),
			);
			let name = sourceName;
			for (let step = 0; step < 4 && kinds.get(name) === "stagekit_cropped_view"; step++) {
				const view = await this.obs.call("GetInputSettings", { inputName: name });
				const shows = (view.inputSettings as Record<string, unknown>).source;
				if (typeof shows !== "string" || shows === "") break;
				name = shows;
			}
			return kinds.get(name);
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
	async pressHotkey(hotkeyName: string, contextName?: string): Promise<void> {
		await this.obs.call("TriggerHotkeyByName", contextName ? { hotkeyName, contextName } : { hotkeyName });
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

	/** The scenes, in OBS's order (top first). */
	async scenes(): Promise<string[]> {
		if (!this.connected) return [];
		const answer = await this.obs.call("GetSceneList");
		return (answer.scenes as Array<{ sceneName?: unknown }>)
			.map((s) => String(s.sceneName ?? ""))
			.filter((name) => name !== "")
			.reverse();
	}

	/** OBS's transitions by name, for a Switch Scene key. */
	async sceneTransitions(): Promise<string[]> {
		if (!this.connected) return [];
		const answer = await this.obs.call("GetSceneTransitionList");
		return (answer.transitions as Array<{ transitionName?: unknown }>).map((t) => String(t.transitionName ?? ""));
	}

	/** The scene on the program, and on the preview while Studio Mode is on. */
	async currentScenes(): Promise<{ program: string; preview: string } | undefined> {
		if (!this.connected) return undefined;
		try {
			const program = await this.obs.call("GetCurrentProgramScene");
			let preview = "";
			try {
				preview = String((await this.obs.call("GetCurrentPreviewScene")).currentPreviewSceneName ?? "");
			} catch {
				// Studio Mode is off: there is no preview.
			}
			return { program: String(program.currentProgramSceneName ?? ""), preview };
		} catch {
			return undefined;
		}
	}

	/**
	 * Switches to a scene: the program, or the preview (Studio Mode; the program
	 * when it is off). A transition named here, and a duration, are set first.
	 */
	switchScene(sceneName: string, toPreview: boolean, transitionName: string, durationMs: number, restore = false): Promise<void> {
		const apply = () => this.applySceneSwitch(sceneName, toPreview, transitionName, durationMs, restore);
		const next = this.sceneSwitches.then(apply);
		this.sceneSwitches = next.catch(() => {});
		return next;
	}

	private async applySceneSwitch(sceneName: string, toPreview: boolean, transitionName: string, durationMs: number, restore: boolean): Promise<void> {
		const preview = toPreview && (await this.obs.call("GetStudioModeEnabled")).studioModeEnabled;
		const request = preview ? "SetCurrentPreviewScene" : "SetCurrentProgramScene";
		if ((restore && preview) || (!transitionName && durationMs <= 0)) {
			await this.obs.call(request, { sceneName });
			return;
		}
		const before = restore ? await this.obs.call("GetCurrentSceneTransition") : undefined;
		let changedName = false;
		let changedTime = false;
		let expectedTime: number | undefined;
		let cancelWait: (() => void) | undefined;
		try {
			if (transitionName && transitionName !== before?.transitionName) {
				await this.obs.call("SetCurrentSceneTransition", { transitionName });
				changedName = true;
			}
			if (durationMs > 0) {
				await this.obs.call("SetCurrentSceneTransitionDuration", { transitionDuration: durationMs });
				changedTime = true;
				expectedTime = durationMs;
			}
			let finished: Promise<void> | undefined;
			if (before && (changedName || changedTime)) {
				const active = await this.obs.call("GetCurrentSceneTransition");
				expectedTime = active.transitionDuration ?? undefined;
				if (active.transitionKind !== "cut_transition" && (await this.obs.call("GetCurrentProgramScene")).currentProgramSceneName !== sceneName) {
					// Subscribe before the switch so even a very short transition can signal completion.
					finished = new Promise<void>((resolve) => {
						const ended = (event: { transitionName: string }) => {
							if (event.transitionName === active.transitionName) resolve();
						};
						this.obs.on("SceneTransitionEnded", ended);
						const timer = setTimeout(resolve, Math.max(active.transitionDuration ?? 0, 300) + 500);
						cancelWait = () => { clearTimeout(timer); this.obs.off("SceneTransitionEnded", ended); };
					});
				}
			}
			await this.obs.call(request, { sceneName });
			if (finished) await finished;
		} finally {
			cancelWait?.();
			if (before && (changedName || changedTime)) {
				const current = await this.obs.call("GetCurrentSceneTransition");
				// A selection made by the operator during the switch takes precedence.
				if (current.transitionName === (transitionName || before.transitionName) &&
					(expectedTime === undefined || current.transitionDuration === expectedTime)) {
					if (changedName) await this.obs.call("SetCurrentSceneTransition", { transitionName: before.transitionName });
					if (before.transitionDuration != null && !before.transitionFixed && (changedTime || changedName))
						await this.obs.call("SetCurrentSceneTransitionDuration", { transitionDuration: before.transitionDuration });
				}
			}
		}
	}

	/**
	 * A window or source switcher (or Auto Capture): is it on a manual window,
	 * and is its capture hidden. Undefined when it cannot be asked.
	 */
	async switcherState(sourceName: string, filterName: string): Promise<{ manual: boolean; hidden: boolean } | undefined> {
		if (!sourceName || !filterName) return undefined;
		const answer = await this.stageKit("GetSwitcherState", { sourceName, filterName });
		if (!answer) return undefined;
		return { manual: answer.manual === true, hidden: answer.hidden === true };
	}

	/**
	 * A Standby Image filter's image over its source on purpose: shown ("on"),
	 * taken away ("off") or the other way ("toggle"), with that filter's own
	 * transition. Says whether it covers afterwards, or undefined when OBS
	 * refused (no such filter, or it is switched off) or cannot be asked.
	 */
	async standbyCover(sourceName: string, filterName: string, how: "on" | "off" | "toggle"): Promise<boolean | undefined> {
		if (!sourceName) return undefined;
		const answer = await this.stageKit("StandbyCover", { sourceName, filterName, how });
		return answer ? answer.covered === true : undefined;
	}

	/** Whether a Standby Image filter's image is over its source on purpose; undefined when it cannot be asked. */
	async standbyCovered(sourceName: string, filterName: string): Promise<boolean | undefined> {
		if (!sourceName) return undefined;
		const answer = await this.stageKit("GetStandbyCover", { sourceName, filterName });
		return answer ? answer.covered === true : undefined;
	}

	/**
	 * A Show in Scenes key's press with its own way of hiding ("hide", or
	 * "cover" with each source's Standby Image): the filter is switched as
	 * any key switches it, and that press hides that way. Says whether the
	 * filter is on afterwards, or undefined when OBS refused or cannot be asked.
	 */
	async showInScenesPress(sourceName: string, filterName: string, how: "on" | "off" | "toggle", hideAs: "hide" | "cover"): Promise<boolean | undefined> {
		if (!sourceName || !filterName) return undefined;
		const answer = await this.stageKit("ShowInScenesPress", { sourceName, filterName, how, hideAs });
		return answer ? answer.enabled === true : undefined;
	}

	/**
	 * Every Scene Modes filter (on the scene named, or all): its scene, its
	 * name, whether it is on, the mode on now (an id) and its modes. Empty
	 * when OBS, or a StageKit with Scene Modes, cannot be asked.
	 */
	async sceneModes(scene = ""): Promise<SceneModes[]> {
		const answer = await this.stageKit("ListSceneModes", scene ? { scene } : {});
		const filters = answer?.filters;
		if (!Array.isArray(filters)) return [];
		return filters.map((f: Record<string, unknown>) => ({
			scene: String(f.scene ?? ""),
			filter: String(f.filter ?? ""),
			enabled: f.enabled !== false,
			current: String(f.current ?? ""),
			modes: modeList(f.modes),
		}));
	}

	/** One Scene Modes filter as it is now, or undefined when it cannot be asked. */
	async sceneMode(scene: string, filter: string): Promise<SceneModes | undefined> {
		if (!scene) return undefined;
		const answer = await this.stageKit("GetSceneMode", filter ? { scene, filter } : { scene });
		if (!answer) return undefined;
		return {
			scene,
			filter: String(answer.filter ?? filter),
			enabled: answer.enabled !== false,
			current: String(answer.current ?? ""),
			modes: modeList(answer.modes),
		};
	}

	/** Turns a mode on ("on"), toggles it ("toggle"), steps to the next ("next") or turns every mode off ("none"); says whether OBS took it. */
	async setSceneMode(scene: string, filter: string, mode: string, how: "on" | "toggle" | "next" | "none"): Promise<boolean> {
		// (Next and none need no mode.)
		return (await this.stageKit("SetSceneMode", how === "next" || how === "none" ? { scene, filter, how } : { scene, filter, mode, how })) !== undefined;
	}

	/**
	 * Every Scene Blur by name: its blur's look, whether any is on, and each
	 * scene it is on. Empty when OBS, or a StageKit with Scene Blur, cannot
	 * be asked.
	 */
	async sceneBlurs(): Promise<SceneBlurFilter[]> {
		return readSceneBlurs(await this.stageKit("ListSceneBlurs"));
	}

	/**
	 * Switches the Scene Blurs of a name: on one scene, or on every scene that
	 * has one ("" for the scene). Says whether they are on afterwards, or
	 * undefined when OBS refused (no such blur) or cannot be asked.
	 */
	async sceneBlur(filter: string, scene: string, how: "on" | "off" | "toggle"): Promise<boolean | undefined> {
		const answer = await this.stageKit("SceneBlur", { filter, scene, how });
		return answer ? answer.active === true : undefined;
	}

	/**
	 * The stream's or the recording's status (GetStreamStatus,
	 * GetRecordStatus): whether it runs, is paused or reconnecting, and its
	 * timecode. Undefined when OBS cannot be asked.
	 */
	async outputStatus(output: "stream" | "record"): Promise<Record<string, unknown> | undefined> {
		if (!this.connected) return undefined;
		try {
			return (await this.obs.call(output === "stream" ? "GetStreamStatus" : "GetRecordStatus")) as Record<string, unknown>;
		} catch {
			return undefined;
		}
	}

	/** Sends one of obs-websocket's stream or record requests (StartStream, TogglePauseRecord ...) by name. */
	async outputRequest(requestType: string): Promise<void> {
		await this.obs.call(requestType as "StartStream");
	}

	/** Changes some of a filter's settings, leaving the rest as they are. */
	/** Scene item IDs belong to one scene or group in one collection. The full identity is kept on the key. */
	async sceneItemTargets(): Promise<Array<{ label: string; value: string }>> {
		if (!this.connected) return [];
		const [scenes, groups, current] = await Promise.all([
			this.obs.call("GetSceneList"), this.obs.call("GetGroupList"), this.obs.call("GetSceneCollectionList"),
		]);
		const parents = [
			...scenes.scenes.map((s) => ({ name: String(s.sceneName), group: false })),
			...groups.groups.map((name) => ({ name: String(name), group: true })),
		];
		if (!parents.length) return [];
		const answers = await this.obs.callBatch(parents.map((p) => ({
			requestType: p.group ? "GetGroupSceneItemList" as const : "GetSceneItemList" as const, requestData: { sceneName: p.name },
		})));
		const items: Array<{ label: string; value: string }> = [];
		for (const [i, response] of answers.entries()) {
			if (!response.requestStatus.result) continue;
			const parent = parents[i];
			const list = (response.responseData as { sceneItems?: { sceneItemId: number; sourceName: string }[] } | undefined)?.sceneItems ?? [];
			for (const item of list) {
				items.push({ label: parent.name + " / " + item.sourceName + " (#" + item.sceneItemId + ")",
					value: JSON.stringify([current.currentSceneCollectionName, parent.name, item.sceneItemId, item.sourceName, parent.group]) });
			}
		}
		return items;
	}

	async mixerInputs(): Promise<string[]> {
		if (!this.connected) return [];
		const list = (await this.obs.call("GetInputList")).inputs.map((i) => String(i.inputName));
		if (!list.length) return [];
		const answers = await this.obs.callBatch(list.map((inputName) => ({ requestType: "GetInputMute" as const, requestData: { inputName } })));
		return list.filter((_name, i) => answers[i].requestStatus.result);
	}

	async controlState(key: string, target: string, apply?: boolean): Promise<boolean> {
		if (!this.connected) throw new Error("OBS is disconnected");
		if (key === "obsstudio") {
			if (apply !== undefined) await this.obs.call("SetStudioModeEnabled", { studioModeEnabled: apply });
			return (await this.obs.call("GetStudioModeEnabled")).studioModeEnabled;
		}
		if (key === "obscamera") {
			const active = (await this.obs.call("GetVirtualCamStatus")).outputActive;
			if (apply !== undefined && apply !== active) await this.obs.call(apply ? "StartVirtualCam" : "StopVirtualCam");
			return apply ?? active;
		}
		if (key === "obsmute" && target) {
			if (apply !== undefined) await this.obs.call("SetInputMute", { inputName: target, inputMuted: apply });
			return (await this.obs.call("GetInputMute", { inputName: target })).inputMuted;
		}
		if (key !== "obssource") throw new Error("Choose an OBS target");
		const item = decodeItem(target);
		if (!item) throw new Error("Choose a scene item");
		const current = (await this.obs.call("GetSceneCollectionList")).currentSceneCollectionName;
		if (current !== item.collection) throw new Error("A different scene collection is open");
		const request = { sceneName: item.parent, sceneItemId: item.id };
		const list = await this.obs.call(item.group ? "GetGroupSceneItemList" : "GetSceneItemList", { sceneName: item.parent });
		const row = list.sceneItems.find((entry) => entry.sceneItemId === item.id && entry.sourceName === item.name);
		if (!row) throw new Error("The saved scene item has changed");
		if (apply !== undefined) await this.obs.call("SetSceneItemEnabled", { ...request, sceneItemEnabled: apply });
		return apply ?? (row.sceneItemEnabled === true);
	}

	async setFilterSettings(sourceName: string, filterName: string, filterSettings: Record<string, boolean | number | string>): Promise<void> {
		await this.obs.call("SetSourceFilterSettings", { sourceName, filterName, filterSettings, overlay: true });
	}

	async setFilter(sourceName: string, filterName: string, filterEnabled: boolean): Promise<void> {
		await this.obs.call("SetSourceFilterEnabled", { sourceName, filterName, filterEnabled });
	}
}

export const obs = new ObsLink();
