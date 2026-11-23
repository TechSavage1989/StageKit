// A View key keeps stable references to choices saved in OBS. The native
// controller owns their source membership, order and active state.

export type ViewChoice = { id: string; name: string; type: string; active: boolean; available: boolean };
export type ViewGroup = {
	id: string;
	name: string;
	kind: "modes" | "one" | "together" | "follow";
	enabled: boolean;
	available: boolean;
	reason: string;
	current: string;
	previous: string;
	allowNone: boolean;
	choices: ViewChoice[];
};
export type SavedView = ViewChoice & { reason: string };
export type SceneController = {
	controllerId: string;
	name: string;
	scene: string;
	sceneUuid: string;
	enabled: boolean;
	available: boolean;
	reason: string;
	groups: ViewGroup[];
	views: SavedView[];
};

export type ViewSettings = {
	controllerId?: string;
	viewTarget?: string;
	groupId?: string;
	choiceId?: string;
	viewId?: string;
	viewPress?: string;
};
export type ViewCommand = {
	controllerId: string;
	command: "select" | "next";
	groupId?: string;
	choiceId?: string;
	viewId?: string;
};

const string = (value: unknown): string => typeof value === "string" ? value : "";
const object = (value: unknown): Record<string, unknown> | undefined => value !== null && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : undefined;

/** Ambiguous IDs are unusable; never silently bind a key to the first duplicate. */
function unique<T extends { id: string }>(values: T[]): T[] {
	return values.filter((value, index) => value.id && values.findIndex((other) => other.id === value.id) === index
		&& values.findLastIndex((other) => other.id === value.id) === index);
}

function choices(value: unknown): SavedView[] {
	if (!Array.isArray(value)) return [];
	return unique(value.flatMap((entry) => {
		const item = object(entry);
		return item && string(item.id) ? [{
			id: string(item.id), name: string(item.name), type: string(item.type),
			active: item.active === true, available: item.available === true, reason: string(item.reason),
		}] : [];
	}));
}

/** Missing availability or a mismatched controller identity fails closed. */
export function readSceneController(value: unknown, expectedId?: string): SceneController | undefined {
	const item = object(value);
	if (!item || !string(item.controllerId) || (expectedId !== undefined && item.controllerId !== expectedId)) return undefined;
	const groups: ViewGroup[] = !Array.isArray(item.groups) ? [] : item.groups.flatMap((entry) => {
		const group = object(entry);
		if (!group || !string(group.id) || !["modes", "one", "together", "follow"].includes(string(group.kind))) return [];
		return [{
			id: string(group.id), name: string(group.name), kind: group.kind as ViewGroup["kind"],
			enabled: group.enabled === true, available: group.available === true, reason: string(group.reason),
			current: string(group.current), previous: string(group.previous), allowNone: group.allowNone === true,
			choices: choices(group.choices),
		}];
	});
	return {
		controllerId: string(item.controllerId), name: string(item.name), scene: string(item.scene), sceneUuid: string(item.sceneUuid),
		enabled: item.enabled === true, available: item.available === true, reason: string(item.reason),
		groups: unique(groups), views: choices(item.views),
	};
}

export function readSceneControllers(value: unknown): SceneController[] {
	const controllers = object(value)?.controllers;
	if (!Array.isArray(controllers)) return [];
	const parsed = controllers.flatMap((item) => {
		const controller = readSceneController(item);
		return controller ? [{ ...controller, id: controller.controllerId }] : [];
	});
	return unique(parsed).map(({ id: _id, ...controller }) => controller);
}

export function canStepView(group: ViewGroup | undefined): boolean {
	return !!group && (group.kind === "modes" || group.kind === "one") && group.choices.filter((choice) => choice.available).length > 1;
}

export type ViewState = {
	status: "ready" | "unconfigured" | "offline" | "missing" | "disabled" | "unavailable";
	name: string;
	type: string;
	active: boolean;
	reason: string;
	command?: ViewCommand;
};

/** Native selection and availability are authoritative, including saved combined views. */
export function viewState(settings: ViewSettings, controller: SceneController | undefined, connected: boolean): ViewState {
	const stopped = (status: ViewState["status"], name: string, reason: string): ViewState => ({ status, name, reason, type: "", active: false });
	if (!settings.controllerId) return stopped("unconfigured", "Choose\nView", "Choose a Scene Controller in OBS.");
	if (!connected) return stopped("offline", "OBS\nOffline", "OBS is not connected.");
	if (!controller || controller.controllerId !== settings.controllerId) return stopped("missing", "Missing\nController", "The selected controller is unavailable. Check the OBS collection and StageKit version.");
	if (!controller.enabled) return stopped("disabled", "Disabled", "The Scene Controller is disabled in OBS.");
	if (!controller.available) return stopped("unavailable", "Unavailable", controller.reason || "The Scene Controller is unavailable.");
	const press = settings.viewPress || "select";
	const target = settings.viewTarget || "group";
	if (target === "view") {
		if (press !== "select") return stopped("unconfigured", "Choose\nAction", "Saved views support Select.");
		if (!settings.viewId) return stopped("unconfigured", "Choose\nView", "Choose a saved view.");
		const view = controller.views.find((entry) => entry.id === settings.viewId);
		if (!view) return stopped("missing", "Missing\nView", "The selected saved view no longer exists.");
		if (!view.available) return stopped("unavailable", "Unavailable", view.reason || "The saved view is unavailable.");
		return { status: "ready", name: view.name || "View", type: view.type, active: view.active, reason: "", command: { controllerId: controller.controllerId, viewId: view.id, command: "select" } };
	}
	if (target !== "group" || !settings.groupId) return stopped("unconfigured", "Choose\nGroup", "Choose a controller group.");
	const group = controller.groups.find((entry) => entry.id === settings.groupId);
	if (!group) return stopped("missing", "Missing\nGroup", "The selected group no longer exists.");
	if (!group.enabled) return stopped("disabled", "Disabled", "The selected group is disabled in OBS.");
	if (!group.available) return stopped("unavailable", "Unavailable", group.reason || "The selected group is unavailable.");
	if (group.kind === "follow") return stopped("unavailable", "Follow\nRule", "Follow groups are controlled by their leads in OBS.");
	if (press === "next") {
		if (!canStepView(group)) return stopped("unavailable", "No Next\nChoice", "Next requires a Modes or One group with at least two available choices.");
		const current = group.choices.find((entry) => entry.id === group.current && entry.active && entry.available);
		return { status: "ready", name: current?.name || "Next\nChoice", type: current?.type || "", active: !!current, reason: "", command: { controllerId: controller.controllerId, groupId: group.id, command: "next" } };
	}
	if (press !== "select") return stopped("unconfigured", "Choose\nAction", "Choose Select or Next choice.");
	if (!settings.choiceId) return stopped("unconfigured", "Choose\nChoice", "Choose an option from this group.");
	const choice = group.choices.find((entry) => entry.id === settings.choiceId);
	if (!choice) return stopped("missing", "Missing\nChoice", "The selected choice no longer exists in this group.");
	if (!choice.available) return stopped("unavailable", "Unavailable", "The selected choice is unavailable.");
	return { status: "ready", name: choice.name || "Choice", type: choice.type, active: choice.active, reason: "", command: { controllerId: controller.controllerId, groupId: group.id, choiceId: choice.id, command: "select" } };
}

/** A new owner must be chosen explicitly before any of its children can be used. */
export function changedViewSettings<T extends ViewSettings>(previous: T | undefined, next: T): T {
	if (!previous) return next;
	if (previous.controllerId !== next.controllerId) {
		const changed = { ...next, viewPress: "select" };
		delete changed.groupId;
		delete changed.choiceId;
		delete changed.viewId;
		return changed;
	}
	if (previous.groupId !== next.groupId) {
		const changed = { ...next, viewPress: "select" };
		delete changed.choiceId;
		return changed;
	}
	return previous.viewTarget !== next.viewTarget ? { ...next, viewPress: "select" } : next;
}

export function viewBinding(settings: ViewSettings): string {
	return JSON.stringify([settings.controllerId, settings.viewTarget, settings.groupId, settings.choiceId, settings.viewId, settings.viewPress]);
}
